/*
 * OS concept: Daemon process, signal handling and POSIX message-queue IPC (receiver side) — Unit 2
 *
 * ReliefOS coordinator:
 *   - creates the request queue and writes $RELIEF_HOME/reliefd.pid
 *   - fork/execs one camp per line of the camps config and supervises them (camps.c)
 *   - receives and logs HELLO / REQUEST / BYE messages from camps
 *   - stops on SIGINT, SIGTERM or a SHUTDOWN message: SIGTERM all camps, wait up to
 *     2 s, SIGKILL stragglers, reap everything, then remove the queue and pid file
 * The triage scheduler, deadlock control, allocators etc. arrive in later phases.
 */
#include "camps.h"
#include "log.h"
#include "relief.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <mqueue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RECV_TIMEOUT_MS      500
#define SHUTDOWN_GRACE_MS    2000
#define SHUTDOWN_POLL_MS     50
#define DEFAULT_CAMPS_CONF   "scenarios/camps.conf"
#define DEFAULT_SCENARIO     "scenarios/flood_pune.csv"
#define DEFAULT_TIME_SCALE   100     /* ms of real time per simulated minute */

static volatile sig_atomic_t g_stop = 0;     /* signal number that requested shutdown */
static volatile sig_atomic_t g_child = 0;    /* SIGCHLD seen: reap in the main loop */

static void on_stop_signal(int sig)
{
    g_stop = sig;
}

static void on_sigchld(int sig)
{
    (void)sig;
    g_child = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;    /* no SA_RESTART: let mq_timedreceive return EINTR */
    sa.sa_handler = on_stop_signal;
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1)
        goto fail;

    sa.sa_handler = on_sigchld;
    sa.sa_flags = SA_NOCLDSTOP;     /* only termination, not stop/continue */
    if (sigaction(SIGCHLD, &sa, NULL) == -1)
        goto fail;
    return 0;
fail:
    LOG_ERROR("sigaction: %s", strerror(errno));
    return -1;
}

/* Returns 1 if the pid file names a live process, 0 otherwise. */
static int pidfile_owner_alive(const char *path, long *pid_out)
{
    FILE *f = fopen(path, "r");
    long pid = 0;
    int alive = 0;

    if (!f)
        return 0;
    if (fscanf(f, "%ld", &pid) == 1 && pid > 0)
        alive = (kill((pid_t)pid, 0) == 0 || errno == EPERM);
    fclose(f);
    *pid_out = pid;
    return alive;
}

static int write_pidfile(const char *path)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        LOG_ERROR("cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    fprintf(f, "%d\n", (int)getpid());
    if (fclose(f) != 0) {
        LOG_ERROR("cannot write %s: %s", path, strerror(errno));
        return -1;
    }
    return 0;
}

static void deadline_after_ms(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

static long monotonic_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000L + ts.tv_nsec / 1000000L;
}

/* Default camp binary: "camp" next to our own executable (works for build/ and build/tsan/). */
static int default_camp_bin(char *out, size_t len)
{
    char self[CAMPS_PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
    char *slash;

    if (n <= 0)
        return -1;
    self[n] = '\0';
    slash = strrchr(self, '/');
    if (!slash)
        return -1;
    *slash = '\0';
    return (size_t)snprintf(out, len, "%s/camp", self) < len ? 0 : -1;
}

typedef struct {
    unsigned long counts[MSG_SHUTDOWN + 1];
} stats_t;

/* Handle one message. Returns 1 if it was SHUTDOWN. */
static int handle_message(const relief_msg_t *m, camp_table_t *camps, stats_t *st)
{
    const relief_request_t *r = &m->u.req;

    if (m->kind >= MSG_HELLO && m->kind <= MSG_SHUTDOWN)
        st->counts[m->kind]++;

    switch (m->kind) {
    case MSG_HELLO:
        EV_INFO("hello", "camp=%u camp_pid=%d", m->camp_id, m->camp_pid);
        camps_on_hello(camps, m->camp_id, m->camp_pid);
        return 0;
    case MSG_BYE:
        EV_INFO("bye", "camp=%u camp_pid=%d", m->camp_id, m->camp_pid);
        return 0;
    case MSG_REQUEST:
        EV_INFO("request", "id=%u camp=%u camp_pid=%d severity=%s resource=%s qty=%u service=%u arrival=%u",
                r->id, r->camp_id, r->camp_pid, relief_severity_name(r->severity),
                relief_resource_name(r->resource), r->quantity, r->service_time, r->arrival);
        return 0;
    case MSG_SHUTDOWN:
        EV_INFO("shutdown_request", "from_pid=%d", m->camp_pid);
        return 1;
    default:
        EV_WARN("unknown_message", "kind=%u camp_pid=%d", m->kind, m->camp_pid);
        return 0;
    }
}

enum { RECV_NONE = 0, RECV_MSG = 1, RECV_SHUTDOWN = 2, RECV_ERROR = -1 };

/* Wait up to timeout_ms (0 = poll) for one message and handle it. */
static int receive_one(mqd_t mq, long timeout_ms, camp_table_t *camps, stats_t *st)
{
    relief_msg_t msg;
    struct timespec deadline;
    ssize_t n;

    deadline_after_ms(&deadline, timeout_ms);
    n = mq_timedreceive(mq, (char *)&msg, sizeof msg, NULL, &deadline);
    if (n == -1) {
        if (errno == ETIMEDOUT || errno == EINTR)
            return RECV_NONE;
        LOG_ERROR("mq_timedreceive: %s", strerror(errno));
        return RECV_ERROR;
    }
    if ((size_t)n != sizeof msg) {
        EV_WARN("short_message", "bytes=%zd", n);
        return RECV_MSG;
    }
    return handle_message(&msg, camps, st) ? RECV_SHUTDOWN : RECV_MSG;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s [--camps FILE] [--scenario FILE] [--seed N] [--time-scale MS] [--camp-bin PATH]\n"
            "  --camps     camps config, one \"<id> <name>\" per line (default %s)\n"
            "  --scenario  scenario CSV passed to every camp (default %s)\n"
            "  --seed      seed passed to every camp (default 1)\n"
            "  --time-scale  real ms per simulated minute for camps (default %d, 0 = no pacing)\n"
            "  --camp-bin  camp executable (default: camp next to reliefd)\n",
            prog, DEFAULT_CAMPS_CONF, DEFAULT_SCENARIO, DEFAULT_TIME_SCALE);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "camps",    required_argument, NULL, 'c' },
        { "scenario", required_argument, NULL, 's' },
        { "seed",     required_argument, NULL, 'S' },
        { "time-scale", required_argument, NULL, 't' },
        { "camp-bin", required_argument, NULL, 'b' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    static camp_table_t camps;      /* static: ~10 KB, keep it off the stack */
    const char *camps_conf = DEFAULT_CAMPS_CONF;
    const char *scenario = DEFAULT_SCENARIO;
    const char *camp_bin = NULL;
    const char *home = getenv("RELIEF_HOME");
    char pidpath[CAMPS_PATH_MAX], logdir[CAMPS_PATH_MAX], logpath[CAMPS_PATH_MAX];
    stats_t st = { { 0 } };
    struct mq_attr attr;
    long other_pid = 0;
    mqd_t mq;
    int rc = EXIT_SUCCESS, opt, shutdown_msg = 0;

    log_init("reliefd");
    camps.seed = 1;
    camps.time_scale_ms = DEFAULT_TIME_SCALE;

    while ((opt = getopt_long(argc, argv, "c:s:S:t:b:h", opts, NULL)) != -1) {
        char *end;
        switch (opt) {
        case 'c': camps_conf = optarg; break;
        case 's': scenario = optarg; break;
        case 'b': camp_bin = optarg; break;
        case 'S':
            errno = 0;
            camps.seed = strtoul(optarg, &end, 10);
            if (errno || end == optarg || *end) {
                fprintf(stderr, "invalid --seed: %s\n", optarg);
                return 2;
            }
            break;
        case 't':
            errno = 0;
            camps.time_scale_ms = strtoul(optarg, &end, 10);
            if (errno || end == optarg || *end || camps.time_scale_ms > 60000) {
                fprintf(stderr, "invalid --time-scale (0-60000): %s\n", optarg);
                return 2;
            }
            break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (optind < argc) {
        usage(argv[0]);
        return 2;
    }

    if (!home || !*home)
        home = RELIEF_HOME_DEFAULT;
    if ((size_t)snprintf(pidpath, sizeof pidpath, "%s/%s", home, RELIEF_PID_FILE) >= sizeof pidpath ||
        (size_t)snprintf(logdir, sizeof logdir, "%s/%s", home, RELIEF_LOG_DIR) >= sizeof logdir ||
        (size_t)snprintf(logpath, sizeof logpath, "%s/%s", logdir, RELIEF_LOG_FILE) >= sizeof logpath) {
        LOG_ERROR("RELIEF_HOME path too long");
        return EXIT_FAILURE;
    }
    if ((mkdir(home, 0775) == -1 && errno != EEXIST) ||
        (mkdir(logdir, 0775) == -1 && errno != EEXIST)) {
        LOG_ERROR("cannot create %s: %s", logdir, strerror(errno));
        return EXIT_FAILURE;
    }
    if (log_open_file(logpath) == -1) {
        LOG_ERROR("cannot open %s: %s", logpath, strerror(errno));
        return EXIT_FAILURE;
    }

    /* Resolve camp binary and scenario up front so every camp gets absolute paths. */
    if (camp_bin)
        snprintf(camps.camp_bin, sizeof camps.camp_bin, "%s", camp_bin);
    else if (default_camp_bin(camps.camp_bin, sizeof camps.camp_bin) == -1) {
        LOG_ERROR("cannot locate camp binary; use --camp-bin");
        goto fail_log;
    }
    if (access(camps.camp_bin, X_OK) == -1) {
        LOG_ERROR("camp binary %s: %s", camps.camp_bin, strerror(errno));
        goto fail_log;
    }
    if (!realpath(scenario, camps.scenario)) {
        LOG_ERROR("scenario %s: %s", scenario, strerror(errno));
        goto fail_log;
    }
    if (camps_load(&camps, camps_conf) == -1)
        goto fail_log;

    if (pidfile_owner_alive(pidpath, &other_pid)) {
        LOG_ERROR("reliefd already running (pid %ld)", other_pid);
        goto fail_log;
    }

    /* No live owner: any existing queue is left over from a crash. */
    if (mq_unlink(RELIEF_MQ_NAME) == 0)
        EV_WARN("stale_queue_removed", "queue=%s", RELIEF_MQ_NAME);

    memset(&attr, 0, sizeof attr);
    attr.mq_maxmsg = RELIEF_MQ_MAXMSG;
    attr.mq_msgsize = sizeof(relief_msg_t);
    mq = mq_open(RELIEF_MQ_NAME, O_CREAT | O_EXCL | O_RDONLY, 0660, &attr);
    if (mq == (mqd_t)-1) {
        LOG_ERROR("mq_open %s: %s", RELIEF_MQ_NAME, strerror(errno));
        goto fail_log;
    }

    if (install_signal_handlers() == -1 || write_pidfile(pidpath) == -1) {
        mq_close(mq);
        mq_unlink(RELIEF_MQ_NAME);
        goto fail_log;
    }

    EV_INFO("daemon_start", "queue=%s home=%s camps=%zu scenario=%s seed=%lu time_scale_ms=%lu",
            RELIEF_MQ_NAME, home, camps.n, camps.scenario, camps.seed, camps.time_scale_ms);
    camps_spawn_all(&camps);

    /* Main loop. SIGCHLD/SIGTERM interrupt mq_timedreceive (EINTR), so reaping and
     * shutdown happen promptly; the 500 ms timeout bounds the check-then-block race. */
    while (!g_stop) {
        int r;

        if (g_child) {
            g_child = 0;            /* clear before reaping so no SIGCHLD is lost */
            camps_reap(&camps, 0);
        }
        r = receive_one(mq, RECV_TIMEOUT_MS, &camps, &st);
        if (r == RECV_ERROR) {
            rc = EXIT_FAILURE;
            break;
        }
        if (r == RECV_SHUTDOWN) {
            shutdown_msg = 1;
            break;
        }
    }

    if (g_stop)
        EV_INFO("shutdown", "reason=signal sig=%d", (int)g_stop);
    else
        EV_INFO("shutdown", "reason=%s", shutdown_msg ? "message" : "error");

    /* Terminate camps: SIGTERM, grace period (keep draining the queue so camps
     * blocked on mq_send can deliver BYE), then SIGKILL and a blocking reap. */
    camps_terminate_all(&camps);
    for (long end = monotonic_ms() + SHUTDOWN_GRACE_MS;
         camps_live(&camps) > 0 && monotonic_ms() < end;) {
        g_child = 0;
        camps_reap(&camps, 0);
        if (camps_live(&camps) == 0)
            break;
        if (receive_one(mq, SHUTDOWN_POLL_MS, &camps, &st) == RECV_ERROR)
            break;
    }
    camps_kill_stragglers(&camps);
    camps_reap(&camps, 1);      /* blocks until ECHILD: no zombies left */

    /* Camps may have queued BYE just before exiting: drain what is left. */
    while (receive_one(mq, 0, &camps, &st) == RECV_MSG)
        ;

    for (size_t i = 0; i < camps.n; i++)
        EV_INFO("camp_final", "camp=%u name=%s state=%s restarts=%u", camps.camps[i].id,
                camps.camps[i].name, camp_state_name(camps.camps[i].state),
                camps.camps[i].restarts);
    EV_INFO("daemon_stop", "hello=%lu request=%lu bye=%lu",
            st.counts[MSG_HELLO], st.counts[MSG_REQUEST], st.counts[MSG_BYE]);

    mq_close(mq);
    if (mq_unlink(RELIEF_MQ_NAME) == -1)
        LOG_WARN("mq_unlink: %s", strerror(errno));
    if (unlink(pidpath) == -1)
        LOG_WARN("unlink %s: %s", pidpath, strerror(errno));
    EV_INFO("stopped", "status=%d", rc);
    log_close_file();
    return rc;

fail_log:
    log_close_file();
    return EXIT_FAILURE;
}
