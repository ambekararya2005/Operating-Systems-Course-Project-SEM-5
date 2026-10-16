/*
 * OS concept: Daemon process, signal handling and POSIX message-queue IPC (receiver side) — Unit 2
 *
 * Phase 0 skeleton of the ReliefOS coordinator:
 *   - creates the request queue and writes $RELIEF_HOME/reliefd.pid
 *   - receives and logs HELLO / REQUEST / BYE messages from camps
 *   - stops on SIGINT, SIGTERM or a SHUTDOWN message, then cleans up
 * The triage scheduler, deadlock control, allocators etc. arrive in later phases.
 */
#include "log.h"
#include "relief.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define RECV_TIMEOUT_MS 500

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int install_signal_handlers(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;    /* no SA_RESTART: let mq_timedreceive return EINTR */
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1) {
        LOG_ERROR("sigaction: %s", strerror(errno));
        return -1;
    }
    return 0;
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

static void log_message(const relief_msg_t *m)
{
    const relief_request_t *r = &m->u.req;

    switch (m->kind) {
    case MSG_HELLO:
    case MSG_BYE:
        LOG_INFO("%s camp=%u pid=%d", relief_msg_kind_name(m->kind), m->camp_id, m->camp_pid);
        break;
    case MSG_REQUEST:
        LOG_INFO("REQUEST id=%u camp=%u pid=%d severity=%s resource=%s qty=%u service=%u arrival=%u",
                 r->id, r->camp_id, r->camp_pid, relief_severity_name(r->severity),
                 relief_resource_name(r->resource), r->quantity, r->service_time, r->arrival);
        break;
    case MSG_SHUTDOWN:
        LOG_INFO("SHUTDOWN requested by pid=%d", m->camp_pid);
        break;
    default:
        LOG_WARN("unknown message kind %u from pid=%d", m->kind, m->camp_pid);
        break;
    }
}

int main(void)
{
    const char *home = getenv("RELIEF_HOME");
    char pidpath[4096];
    unsigned long counts[MSG_SHUTDOWN + 1] = { 0 };
    struct mq_attr attr;
    long other_pid = 0;
    mqd_t mq;
    int rc = EXIT_SUCCESS;

    log_init("reliefd");
    if (!home || !*home)
        home = RELIEF_HOME_DEFAULT;

    if (mkdir(home, 0775) == -1 && errno != EEXIST) {
        LOG_ERROR("cannot create RELIEF_HOME %s: %s", home, strerror(errno));
        return EXIT_FAILURE;
    }
    if ((size_t)snprintf(pidpath, sizeof pidpath, "%s/%s", home, RELIEF_PID_FILE) >= sizeof pidpath) {
        LOG_ERROR("RELIEF_HOME path too long");
        return EXIT_FAILURE;
    }
    if (pidfile_owner_alive(pidpath, &other_pid)) {
        LOG_ERROR("reliefd already running (pid %ld)", other_pid);
        return EXIT_FAILURE;
    }

    /* No live owner: any existing queue is left over from a crash. */
    if (mq_unlink(RELIEF_MQ_NAME) == 0)
        LOG_WARN("removed stale queue %s", RELIEF_MQ_NAME);

    memset(&attr, 0, sizeof attr);
    attr.mq_maxmsg = RELIEF_MQ_MAXMSG;
    attr.mq_msgsize = sizeof(relief_msg_t);
    mq = mq_open(RELIEF_MQ_NAME, O_CREAT | O_EXCL | O_RDONLY, 0660, &attr);
    if (mq == (mqd_t)-1) {
        LOG_ERROR("mq_open %s: %s", RELIEF_MQ_NAME, strerror(errno));
        return EXIT_FAILURE;
    }

    if (install_signal_handlers() == -1 || write_pidfile(pidpath) == -1) {
        mq_close(mq);
        mq_unlink(RELIEF_MQ_NAME);
        return EXIT_FAILURE;
    }

    LOG_INFO("started: queue=%s home=%s", RELIEF_MQ_NAME, home);

    while (!g_stop) {
        relief_msg_t msg;
        struct timespec deadline;
        ssize_t n;

        deadline_after_ms(&deadline, RECV_TIMEOUT_MS);
        n = mq_timedreceive(mq, (char *)&msg, sizeof msg, NULL, &deadline);
        if (n == -1) {
            if (errno == ETIMEDOUT || errno == EINTR)
                continue;
            LOG_ERROR("mq_timedreceive: %s", strerror(errno));
            rc = EXIT_FAILURE;
            break;
        }
        if ((size_t)n != sizeof msg) {
            LOG_WARN("dropping short message (%zd bytes)", n);
            continue;
        }

        log_message(&msg);
        if (msg.kind >= MSG_HELLO && msg.kind <= MSG_SHUTDOWN)
            counts[msg.kind]++;
        if (msg.kind == MSG_SHUTDOWN)
            break;
    }

    if (g_stop)
        LOG_INFO("caught termination signal");
    LOG_INFO("stopping: hello=%lu request=%lu bye=%lu",
             counts[MSG_HELLO], counts[MSG_REQUEST], counts[MSG_BYE]);

    mq_close(mq);
    if (mq_unlink(RELIEF_MQ_NAME) == -1)
        LOG_WARN("mq_unlink: %s", strerror(errno));
    if (unlink(pidpath) == -1)
        LOG_WARN("unlink %s: %s", pidpath, strerror(errno));
    LOG_INFO("stopped");
    return rc;
}
