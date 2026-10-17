/*
 * OS concept: Process creation and termination — fork/execv, SIGCHLD, waitpid, zombie reaping — Unit 2
 */
#include "camps.h"

#include "log.h"
#include "relief.h"
#include "shm.h"

#include <ctype.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

/* WCOREDUMP is not POSIX, so strict C11 + _POSIX_C_SOURCE hides it; Linux uses bit 0x80. */
#ifndef WCOREDUMP
#define WCOREDUMP(status) ((status) & 0x80)
#endif

const char *camp_state_name(camp_state_t s)
{
    return relief_camp_state_name(s);
}

static const char *sig_name(int sig)
{
    switch (sig) {
    case SIGHUP:  return "HUP";
    case SIGINT:  return "INT";
    case SIGQUIT: return "QUIT";
    case SIGILL:  return "ILL";
    case SIGABRT: return "ABRT";
    case SIGFPE:  return "FPE";
    case SIGKILL: return "KILL";
    case SIGSEGV: return "SEGV";
    case SIGPIPE: return "PIPE";
    case SIGTERM: return "TERM";
    case SIGBUS:  return "BUS";
    default:      return "OTHER";
    }
}

static long uptime_ms(const camp_t *c)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - c->started_mono.tv_sec) * 1000L +
           (now.tv_nsec - c->started_mono.tv_nsec) / 1000000L;
}

int camps_load(camp_table_t *t, const char *conf_path)
{
    FILE *f = fopen(conf_path, "r");
    char line[256];
    unsigned lineno = 0;

    if (!f) {
        LOG_ERROR("cannot open camps config %s: %s", conf_path, strerror(errno));
        return -1;
    }
    t->n = 0;
    while (fgets(line, sizeof line, f)) {
        char name[CAMP_NAME_MAX], extra[2];
        unsigned long id;
        char *p = line;
        int fields;

        lineno++;
        while (isspace((unsigned char)*p))
            p++;
        if (*p == '\0' || *p == '#')
            continue;

        fields = sscanf(p, "%lu %31s %1s", &id, name, extra);
        if (fields != 2 || id == 0 || id >= UINT32_MAX / RELIEF_REQ_ID_BASE) {
            LOG_ERROR("%s:%u: expected \"<id> <name>\" (id >= 1, name without spaces)",
                      conf_path, lineno);
            fclose(f);
            return -1;
        }
        for (size_t i = 0; i < t->n; i++) {
            if (t->camps[i].id == id) {
                LOG_ERROR("%s:%u: duplicate camp id %lu", conf_path, lineno, id);
                fclose(f);
                return -1;
            }
        }
        if (t->n == CAMPS_MAX) {
            LOG_ERROR("%s: more than %d camps", conf_path, CAMPS_MAX);
            fclose(f);
            return -1;
        }

        camp_t *c = &t->camps[t->n++];
        memset(c, 0, sizeof *c);
        c->id = (uint32_t)id;
        snprintf(c->name, sizeof c->name, "%s", name);
        c->state = CAMP_EXITED;
        c->wait_status = -1;
    }
    fclose(f);
    EV_INFO("camps_loaded", "count=%zu conf=%s", t->n, conf_path);
    return 0;
}

/* fork + execv one camp. On failure the camp is marked CRASHED. */
static void camp_spawn(camp_table_t *t, camp_t *c)
{
    char id_arg[16], seed_arg[24], scale_arg[24];
    pid_t parent = getpid();
    pid_t pid;

    snprintf(id_arg, sizeof id_arg, "%u", c->id);
    snprintf(seed_arg, sizeof seed_arg, "%lu", t->seed);
    snprintf(scale_arg, sizeof scale_arg, "%lu", t->time_scale_ms);
    char *argv[16];         /* 14 used at most; keep spare room */
    int argc = 0;

    argv[argc++] = t->camp_bin;
    argv[argc++] = "--id";
    argv[argc++] = id_arg;
    argv[argc++] = "--seed";
    argv[argc++] = seed_arg;
    argv[argc++] = "--scenario";
    argv[argc++] = t->scenario;
    argv[argc++] = "--time-scale";
    argv[argc++] = scale_arg;
    if (t->exit_when_done)
        argv[argc++] = "--exit-when-done";
    if (t->resources[0]) {
        argv[argc++] = "--resources";
        argv[argc++] = t->resources;
    }
    if (t->all_or_nothing)
        argv[argc++] = "--all-or-nothing";
    argv[argc] = NULL;

    pid = fork();
    if (pid == -1) {
        EV_ERROR("camp_spawn_failed", "camp=%u name=%s errno=%d", c->id, c->name, errno);
        c->state = CAMP_CRASHED;
        c->pid = 0;
        t->dirty = 1;
        return;
    }
    if (pid == 0) {
        /* Child: only async-signal-safe calls until execv. */
        /* Die with reliefd so camps are never orphaned if it is SIGKILLed.
         * PDEATHSIG tracks the forking *thread*: keep spawning on reliefd's main thread. */
        prctl(PR_SET_PDEATHSIG, SIGTERM);
        if (getppid() != parent)
            _exit(126);
        execv(t->camp_bin, argv);
        static const char msg[] = "camp: execv failed\n";
        ssize_t w = write(STDERR_FILENO, msg, sizeof msg - 1);
        (void)w;
        _exit(127);
    }

    c->pid = pid;
    c->state = CAMP_STARTING;
    t->dirty = 1;
    c->started = time(NULL);
    clock_gettime(CLOCK_MONOTONIC, &c->started_mono);
    EV_INFO("camp_spawn", "camp=%u name=%s pid=%d restarts=%u", c->id, c->name, (int)pid,
            c->restarts);
}

void camps_spawn_all(camp_table_t *t)
{
    for (size_t i = 0; i < t->n; i++)
        camp_spawn(t, &t->camps[i]);
}

static camp_t *find_by_pid(camp_table_t *t, pid_t pid)
{
    for (size_t i = 0; i < t->n; i++)
        if (t->camps[i].pid == pid &&
            (t->camps[i].state == CAMP_STARTING || t->camps[i].state == CAMP_RUNNING))
            return &t->camps[i];
    return NULL;
}

void camps_on_hello(camp_table_t *t, uint32_t id, pid_t pid)
{
    camp_t *c = find_by_pid(t, pid);
    if (c && c->id == id && c->state == CAMP_STARTING) {
        c->state = CAMP_RUNNING;
        t->dirty = 1;
        EV_INFO("camp_running", "camp=%u name=%s pid=%d", c->id, c->name, (int)pid);
    }
}

/* Update the table for one reaped child. */
static void on_child_exit(camp_table_t *t, pid_t pid, int status)
{
    camp_t *c = find_by_pid(t, pid);

    if (!c) {
        EV_WARN("child_reaped_unknown", "pid=%d", (int)pid);
        return;
    }
    c->wait_status = status;
    t->dirty = 1;
    if (t->on_exit && (WIFEXITED(status) || WIFSIGNALED(status)))
        t->on_exit(t->exit_ctx, c->id);     /* process termination returns its resources */

    if (WIFEXITED(status)) {
        int code = WEXITSTATUS(status);
        c->state = CAMP_EXITED;
        log_event(code == 0 ? LOG_LVL_INFO : LOG_LVL_WARN, "camp_exit",
                  "camp=%u name=%s pid=%d status=%d uptime_ms=%ld",
                  c->id, c->name, (int)pid, code, uptime_ms(c));
        return;
    }
    if (!WIFSIGNALED(status))
        return;

    int sig = WTERMSIG(status);
    EV_WARN("camp_signaled", "camp=%u name=%s pid=%d sig=%d signame=%s core=%d uptime_ms=%ld",
            c->id, c->name, (int)pid, sig, sig_name(sig), WCOREDUMP(status) ? 1 : 0,
            uptime_ms(c));

    if (t->shutting_down) {
        c->state = CAMP_EXITED;
    } else if (c->restarts < CAMP_MAX_RESTARTS) {
        c->restarts++;
        EV_INFO("camp_restart", "camp=%u name=%s restart=%u max=%d",
                c->id, c->name, c->restarts, CAMP_MAX_RESTARTS);
        camp_spawn(t, c);
    } else {
        c->state = CAMP_CRASHED;
        EV_ERROR("camp_crashed", "camp=%u name=%s pid=%d restarts=%u",
                 c->id, c->name, (int)pid, c->restarts);
    }
}

void camps_reap(camp_table_t *t, int block)
{
    for (;;) {
        int status;
        pid_t pid = waitpid(-1, &status, block ? 0 : WNOHANG);

        if (pid > 0) {
            on_child_exit(t, pid, status);
            continue;
        }
        if (pid == -1 && errno == EINTR)
            continue;
        if (pid == -1 && errno != ECHILD)
            LOG_ERROR("waitpid: %s", strerror(errno));
        return;     /* 0: nothing more to reap now; ECHILD: no children left */
    }
}

int camps_save(camp_table_t *t)
{
    char tmp[CAMPS_PATH_MAX + 8];
    FILE *f;

    t->dirty = 0;
    if (!t->pids_path[0])
        return 0;
    if ((size_t)snprintf(tmp, sizeof tmp, "%s.tmp", t->pids_path) >= sizeof tmp)
        return -1;
    f = fopen(tmp, "we");
    if (!f) {
        LOG_ERROR("cannot write %s: %s", tmp, strerror(errno));
        return -1;
    }
    fprintf(f, "# ReliefOS camp table, rewritten by reliefd pid %d on every change\n"
               "# id pid state name\n", (int)getpid());
    for (size_t i = 0; i < t->n; i++) {
        const camp_t *c = &t->camps[i];
        fprintf(f, "%u %d %s %s\n", c->id, (int)c->pid, camp_state_name(c->state), c->name);
    }
    if (fclose(f) != 0 || rename(tmp, t->pids_path) == -1) {   /* rename(2) is atomic */
        LOG_ERROR("cannot publish %s: %s", t->pids_path, strerror(errno));
        unlink(tmp);
        return -1;
    }
    return 0;
}

void camps_unpublish(camp_table_t *t)
{
    if (t->pids_path[0] && unlink(t->pids_path) == -1 && errno != ENOENT)
        LOG_WARN("unlink %s: %s", t->pids_path, strerror(errno));
}

void camps_publish_shm(const camp_table_t *t, relief_shm_t *shm)
{
    int rc;

    if (!shm)
        return;
    rc = relief_shm_begin_write(shm);
    if (rc == -1) {
        LOG_ERROR("status table lock: %s", strerror(errno));
        return;
    }
    if (rc == 1)
        EV_WARN("status_lock_recovered", "by=reliefd reason=EOWNERDEAD");
    shm->data.ncamps = (uint32_t)t->n;
    for (size_t i = 0; i < t->n; i++) {
        const camp_t *c = &t->camps[i];
        relief_shm_camp_t *row = &shm->data.camps[i];

        if (row->pid != (int32_t)c->pid) {      /* new process in this slot */
            row->requests_sent = 0;
            row->heartbeat_ms = 0;
        }
        row->id = c->id;
        memcpy(row->name, c->name, sizeof row->name);
        row->pid = (int32_t)c->pid;
        row->state = (uint32_t)c->state;
        row->restarts = c->restarts;
    }
    relief_shm_end_write(shm);
}

size_t camps_live(const camp_table_t *t)
{
    size_t live = 0;
    for (size_t i = 0; i < t->n; i++)
        if (t->camps[i].state == CAMP_STARTING || t->camps[i].state == CAMP_RUNNING)
            live++;
    return live;
}

void camps_terminate_all(camp_table_t *t)
{
    t->shutting_down = 1;
    EV_INFO("camps_terminate", "live=%zu signal=TERM", camps_live(t));
    for (size_t i = 0; i < t->n; i++) {
        camp_t *c = &t->camps[i];
        if ((c->state == CAMP_STARTING || c->state == CAMP_RUNNING) && c->pid > 0)
            kill(c->pid, SIGTERM);
    }
}

void camps_kill_stragglers(camp_table_t *t)
{
    for (size_t i = 0; i < t->n; i++) {
        camp_t *c = &t->camps[i];
        if ((c->state == CAMP_STARTING || c->state == CAMP_RUNNING) && c->pid > 0) {
            EV_WARN("camp_killed", "camp=%u name=%s pid=%d signal=KILL reason=grace_expired",
                    c->id, c->name, (int)c->pid);
            kill(c->pid, SIGKILL);
        }
    }
}
