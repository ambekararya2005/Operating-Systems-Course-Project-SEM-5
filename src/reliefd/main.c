/*
 * OS concept: Daemon process, signal handling and POSIX message-queue IPC (receiver side) — Unit 2
 *
 * ReliefOS coordinator:
 *   - creates the request queue and writes $RELIEF_HOME/reliefd.pid
 *   - fork/execs one camp per line of the camps config and supervises them (camps.c)
 *   - receives and logs HELLO / REQUEST / BYE messages from camps
 *   - stops on SIGINT, SIGTERM or a SHUTDOWN message: SIGTERM all camps, wait up to
 *     2 s, SIGKILL stragglers, reap everything, then remove the queue and pid file
 *   - hands RESCUE requests to the live scheduler (dispatch.c) and kit requests to the
 *     loading dock (dock.c: warehouse producers -> bounded buffer -> trucks), which updates
 *     the inventory (inventory.c, readers-writers)
 *   - decides vehicle and kit requests with the deadlock policy (resmgr.c, --deadlock avoid:
 *     the Banker's algorithm); camps release vehicles/crates with MSG_RELEASE after use
 * Allocators etc. arrive in later phases.
 */
#include "camps.h"
#include "dispatch.h"
#include "dock.h"
#include "inventory.h"
#include "resconf.h"
#include "resmgr.h"
#include "../deadlock/deadlock.h"   /* src/deadlock is not on the include path */
#include "log.h"
#include "relief.h"
#include "shm.h"

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
#define DEFAULT_POLICY       "fcfs"
#define DEFAULT_TEAMS        3
#define DEFAULT_QUANTUM      10      /* simulated minutes, RR */
#define DEFAULT_AGE          60      /* simulated minutes, MLFQ aging */
#define DEFAULT_DOCK         8       /* loading dock slots */
#define DEFAULT_PRODUCERS    2       /* warehouse producer threads */
#define DEFAULT_TRUCKS       4       /* truck consumer threads */
#define DEFAULT_RW           "rwlock"
#define DEFAULT_DEADLOCK     "avoid"

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
    unsigned long counts[MSG_KIND_COUNT];
    relief_shm_t *shm;      /* live status table (total_requests) */
    dispatcher_t *disp;     /* live scheduler: RESCUE requests go here */
    dock_t       *dock;     /* loading dock: kit requests go here */
    resmgr_t     *rm;       /* Banker's resources: vehicle and MED/FOOD kit requests go here first */
} stats_t;

/* Allocator thread, after a grant: kits continue to the loading dock. */
static void on_grant(void *ctx, const relief_request_t *req)
{
    stats_t *st = ctx;
    if (dock_handles(req->resource) && st->dock && dock_submit(st->dock, req) == -1)
        EV_WARN("dock_submit_failed", "id=%u reason=ENOMEM", req->id);
}

/* Main thread, a camp process ended: everything it held is released. */
static void on_camp_exit(void *ctx, uint32_t camp)
{
    stats_t *st = ctx;
    if (st->rm)
        resmgr_camp_gone(st->rm, camp, "exit", 1);
}

/* Lock the status table. Every reliefd lock site goes through here, so a robust-mutex
 * recovery (a writer died holding the lock) is always logged, whichever path finds it. */
static int status_lock(relief_shm_t *shm)
{
    int rc = relief_shm_begin_write(shm);
    if (rc == 1)
        EV_WARN("status_lock_recovered", "by=reliefd reason=EOWNERDEAD");
    return rc;
}

/* Handle one message. Returns 1 if it was SHUTDOWN. */
static int handle_message(const relief_msg_t *m, camp_table_t *camps, stats_t *st)
{
    const relief_request_t *r = &m->u.req;

    if (m->kind >= MSG_HELLO && m->kind < MSG_KIND_COUNT)
        st->counts[m->kind]++;

    switch (m->kind) {
    case MSG_HELLO:
        EV_INFO("hello", "camp=%u camp_pid=%d", m->camp_id, m->camp_pid);
        camps_on_hello(camps, m->camp_id, m->camp_pid);
        if (st->rm)
            resmgr_hello(st->rm, m->camp_id, &m->u.hello);
        return 0;
    case MSG_BYE:
        EV_INFO("bye", "camp=%u camp_pid=%d", m->camp_id, m->camp_pid);
        if (st->rm)
            resmgr_camp_gone(st->rm, m->camp_id, "bye", 0);
        return 0;
    case MSG_RELEASE:
        if (st->rm)
            resmgr_release(st->rm, r);
        return 0;
    case MSG_REQUEST: {
        /* RESCUE requests are CPU-scheduled jobs (teams = CPUs). TRUCK, BOAT, MED_KIT and
         * FOOD_KIT go to the Banker's resource manager first; kits continue to the loading dock
         * once granted (HYGIENE_KIT goes straight there). */
        int sched = r->resource == RES_RESCUE && st->disp != NULL;
        int bundle = (r->flags & REQ_FLAG_BUNDLE) != 0;
        int banker = (bundle || (relief_banker_index(r->resource) >= 0 && r->units > 0)) && st->rm != NULL;
        int dock = dock_handles(r->resource) && st->dock != NULL && !bundle;
        EV_INFO("request", "id=%u camp=%u camp_pid=%d severity=%s resource=%s qty=%u service=%u "
                "arrival=%u alert=%d orig_severity=%s sched=%d dock=%d banker=%d units=%u bundle=%d",
                r->id, r->camp_id, r->camp_pid, relief_severity_name(r->severity),
                relief_resource_name(r->resource), r->quantity, r->service_time, r->arrival,
                (r->flags & REQ_FLAG_ALERT) ? 1 : 0, relief_severity_name(r->orig_severity), sched,
                dock, banker, r->units, bundle);
        if (sched)
            dispatch_submit(st->disp, r);
        if (banker)
            resmgr_submit(st->rm, m);
        else if (dock && dock_submit(st->dock, r) == -1)
            EV_WARN("dock_submit_failed", "id=%u reason=ENOMEM", r->id);
        if (st->shm && status_lock(st->shm) != -1) {
            st->shm->data.total_requests++;
            relief_shm_end_write(st->shm);
        }
        return 0;
    }
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
            "usage: %s [--camps FILE] [--scenario FILE] [--seed N] [--time-scale MS] [--exit-when-done]\n"
            "          [--policy NAME] [--teams N] [--quantum Q] [--age MIN] [--preemptive on|off]\n"
            "          [--dock N] [--producers P] [--trucks C] [--rw semaphore|rwlock]\n"
            "          [--deadlock avoid|none|prevent|detect] [--resources FILE] [--camp-bin PATH]\n"
            "  --camps     camps config, one \"<id> <name>\" per line (default %s)\n"
            "  --scenario  scenario CSV passed to every camp (default %s)\n"
            "  --seed      seed passed to every camp (default 1)\n"
            "  --time-scale  real ms per simulated minute for camps (default %d, 0 = no pacing)\n"
            "  --exit-when-done  camps send BYE and exit 0 after replaying the scenario\n"
            "  --policy    live scheduling policy for RESCUE requests (default %s)\n"
            "  --teams     rescue team threads, 1-%d (default %d)\n"
            "  --quantum   RR quantum in simulated minutes (default %d)\n"
            "  --age       MLFQ aging threshold in simulated minutes (default %d)\n"
            "  --preemptive on|off  prio preemption (default off)\n"
            "  --dock      loading-dock capacity in packages, 1-%d (default %d)\n"
            "  --producers warehouse producer threads, 1-%d (default %d)\n"
            "  --trucks    truck consumer threads, 1-%d (default %d)\n"
            "  --rw        inventory readers-writers lock: semaphore (readers preference) or\n"
            "              rwlock (writer preference) (default %s)\n"
            "  --deadlock  deadlock handling for vehicle/kit requests (default %s): avoid (Banker's\n"
            "              algorithm), none (grant when free: deadlocks can happen), prevent (resource\n"
            "              ordering TRUCK < BOAT < MED_KIT < FOOD_KIT), detect (wait-for graph + recovery)\n"
            "  --all-or-nothing  with --deadlock prevent: a task asks for all its resources at once\n"
            "              (breaks hold-and-wait instead of circular wait)\n"
            "  --detect-interval S  with --deadlock detect: seconds between detector runs (default 1)\n"
            "  --resources Banker's resource totals (default %s)\n"
            "  --camp-bin  camp executable (default: camp next to reliefd)\n",
            prog, DEFAULT_CAMPS_CONF, DEFAULT_SCENARIO, DEFAULT_TIME_SCALE, DEFAULT_POLICY,
            RELIEF_SHM_TEAMS, DEFAULT_TEAMS, DEFAULT_QUANTUM, DEFAULT_AGE, DOCK_MAX_CAPACITY,
            DEFAULT_DOCK, DOCK_MAX_PRODUCERS, DEFAULT_PRODUCERS, DOCK_MAX_TRUCKS, DEFAULT_TRUCKS,
            DEFAULT_RW, DEFAULT_DEADLOCK, RELIEF_RESOURCES_CONF);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "camps",    required_argument, NULL, 'c' },
        { "scenario", required_argument, NULL, 's' },
        { "seed",     required_argument, NULL, 'S' },
        { "time-scale", required_argument, NULL, 't' },
        { "exit-when-done", no_argument,   NULL, 'x' },
        { "policy",   required_argument, NULL, 'p' },
        { "teams",    required_argument, NULL, 'n' },
        { "quantum",  required_argument, NULL, 'q' },
        { "age",      required_argument, NULL, 'a' },
        { "preemptive", required_argument, NULL, 'e' },
        { "camp-bin", required_argument, NULL, 'b' },
        { "dock",     required_argument, NULL, 'D' },
        { "producers", required_argument, NULL, 'P' },
        { "trucks",   required_argument, NULL, 'C' },
        { "rw",       required_argument, NULL, 'w' },
        { "deadlock", required_argument, NULL, 'L' },
        { "all-or-nothing", no_argument, NULL, 'A' },
        { "detect-interval", required_argument, NULL, 'I' },
        { "resources", required_argument, NULL, 'R' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    static camp_table_t camps;      /* static: ~10 KB, keep it off the stack */
    const char *camps_conf = DEFAULT_CAMPS_CONF;
    const char *scenario = DEFAULT_SCENARIO;
    const char *camp_bin = NULL;
    const char *home = getenv("RELIEF_HOME");
    char pidpath[CAMPS_PATH_MAX], logdir[CAMPS_PATH_MAX], logpath[CAMPS_PATH_MAX];
    stats_t st = { { 0 }, NULL, NULL, NULL, NULL };
    resmgr_config_t rcfg = { DEFAULT_DEADLOCK, RELIEF_BANKER_DEFAULT_TOTALS, 0, 1000, on_grant, &st };
    int all_or_nothing = 0;
    const char *resources = RELIEF_RESOURCES_CONF;
    char rerr[CAMPS_PATH_MAX + 128] = "";
    dock_config_t kcfg = { DEFAULT_DOCK, DEFAULT_PRODUCERS, DEFAULT_TRUCKS, 0, NULL };
    const char *rw_kind = DEFAULT_RW;
    inventory_t *inv = NULL;
    dispatch_config_t dcfg = { DEFAULT_POLICY, DEFAULT_TEAMS, DEFAULT_QUANTUM, -1, DEFAULT_AGE, 0, NULL };
    struct mq_attr attr;
    long other_pid = 0;
    mqd_t mq;
    int rc = EXIT_SUCCESS, opt, shutdown_msg = 0;

    log_init("reliefd");
    camps.seed = 1;
    camps.time_scale_ms = DEFAULT_TIME_SCALE;

    while ((opt = getopt_long(argc, argv, "c:s:S:t:xp:n:q:a:e:b:D:P:C:w:L:R:AI:h", opts, NULL)) != -1) {
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
        case 'x': camps.exit_when_done = 1; break;
        case 'p': dcfg.policy = optarg; break;
        case 'n': case 'q': case 'a': {
            unsigned long v, max = opt == 'n' ? RELIEF_SHM_TEAMS : 100000;
            errno = 0;
            v = strtoul(optarg, &end, 10);
            if (errno || end == optarg || *end || v < 1 || v > max) {
                fprintf(stderr, "invalid --%s (1-%lu): %s\n",
                        opt == 'n' ? "teams" : opt == 'q' ? "quantum" : "age", max, optarg);
                return 2;
            }
            if (opt == 'n')
                dcfg.teams = (int)v;
            else if (opt == 'q')
                dcfg.quantum = (int)v;
            else
                dcfg.age = (int64_t)v;
            break;
        }
        case 'e':
            if (strcmp(optarg, "on") == 0 || strcmp(optarg, "off") == 0) {
                dcfg.preemptive = strcmp(optarg, "on") == 0;
            } else {
                fprintf(stderr, "invalid --preemptive (on or off): %s\n", optarg);
                return 2;
            }
            break;
        case 'D': case 'P': case 'C': {
            unsigned long v, max = opt == 'D' ? DOCK_MAX_CAPACITY
                                 : opt == 'P' ? DOCK_MAX_PRODUCERS : DOCK_MAX_TRUCKS;
            errno = 0;
            v = strtoul(optarg, &end, 10);
            if (errno || end == optarg || *end || v < 1 || v > max) {
                fprintf(stderr, "invalid --%s (1-%lu): %s\n",
                        opt == 'D' ? "dock" : opt == 'P' ? "producers" : "trucks", max, optarg);
                return 2;
            }
            if (opt == 'D')
                kcfg.capacity = (int)v;
            else if (opt == 'P')
                kcfg.producers = (int)v;
            else
                kcfg.trucks = (int)v;
            break;
        }
        case 'w':
            if (strcmp(optarg, "semaphore") && strcmp(optarg, "rwlock")) {
                fprintf(stderr, "invalid --rw (semaphore or rwlock): %s\n", optarg);
                return 2;
            }
            rw_kind = optarg;
            break;
        case 'L': {
            const deadlock_policy_t *pol = deadlock_policy_find(optarg);
            if (!pol) {
                fprintf(stderr, "invalid --deadlock (%s): %s\n", deadlock_policy_names(), optarg);
                return 2;
            }
            rcfg.mode = pol->name;
            break;
        }
        case 'A': all_or_nothing = 1; break;
        case 'I': {
            double secs;
            errno = 0;
            secs = strtod(optarg, &end);
            if (errno || end == optarg || *end || secs < 0.01 || secs > 600) {
                fprintf(stderr, "invalid --detect-interval (0.01-600 seconds): %s\n", optarg);
                return 2;
            }
            rcfg.detect_interval_ms = (long)(secs * 1000 + 0.5);
            break;
        }
        case 'R': resources = optarg; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (optind < argc) {
        usage(argv[0]);
        return 2;
    }
    if (all_or_nothing) {
        if (strcmp(rcfg.mode, "prevent") != 0 && strcmp(rcfg.mode, "prevent-all") != 0) {
            fprintf(stderr, "--all-or-nothing needs --deadlock prevent\n");
            return 2;
        }
        rcfg.mode = "prevent-all";
    }
    camps.all_or_nothing = deadlock_policy_find(rcfg.mode)->all_or_nothing;

    if (!home || !*home)
        home = RELIEF_HOME_DEFAULT;
    if ((size_t)snprintf(pidpath, sizeof pidpath, "%s/%s", home, RELIEF_PID_FILE) >= sizeof pidpath ||
        (size_t)snprintf(camps.pids_path, sizeof camps.pids_path, "%s/%s", home,
                         RELIEF_CAMPS_PIDS) >= sizeof camps.pids_path ||
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
    if (relief_resources_load(resources, rcfg.total, rerr, sizeof rerr) == -1 ||
        !realpath(resources, camps.resources)) {
        LOG_ERROR("resources: %s", rerr[0] ? rerr : strerror(errno));
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

    /* Live status table in POSIX shared memory; camps attach to it at startup. */
    if (relief_shm_unlink_stale())
        EV_WARN("stale_shm_removed", "name=%s", RELIEF_SHM_NAME);
    st.shm = relief_shm_create();
    if (!st.shm) {
        LOG_ERROR("shm %s: %s", RELIEF_SHM_NAME, strerror(errno));
        mq_close(mq);
        mq_unlink(RELIEF_MQ_NAME);
        goto fail_log;
    }
    if (status_lock(st.shm) != -1) {
        st.shm->data.daemon_pid = (int32_t)getpid();
        st.shm->data.start_time_ms = relief_now_ms();
        relief_shm_end_write(st.shm);
    }

    if (install_signal_handlers() == -1 || write_pidfile(pidpath) == -1) {
        relief_shm_destroy(st.shm);
        mq_close(mq);
        mq_unlink(RELIEF_MQ_NAME);
        goto fail_log;
    }

    EV_INFO("daemon_start", "queue=%s shm=%s home=%s camps=%zu scenario=%s seed=%lu time_scale_ms=%lu",
            RELIEF_MQ_NAME, RELIEF_SHM_NAME, home, camps.n, camps.scenario, camps.seed,
            camps.time_scale_ms);

    /* Rescue team threads. Started before the camps exist; fork() still happens only on
     * this (main) thread, and the child calls nothing but async-signal-safe functions
     * before execv, which is what makes forking a multithreaded process safe. */
    dcfg.time_scale_ms = camps.time_scale_ms;
    dcfg.shm = st.shm;
    st.disp = dispatch_start(&dcfg);
    inv = st.disp ? inventory_new(rw_kind) : NULL;
    if (inv) {
        kcfg.time_scale_ms = camps.time_scale_ms;
        kcfg.inv = inv;
        st.dock = dock_start(&kcfg);
    }
    if (st.dock) {
        rcfg.time_scale_ms = camps.time_scale_ms;
        st.rm = resmgr_start(&rcfg);
    }
    if (!st.rm) {
        dock_stop(st.dock);
        dispatch_stop(st.disp);
        inventory_free(inv);
        unlink(pidpath);
        relief_shm_destroy(st.shm);
        mq_close(mq);
        mq_unlink(RELIEF_MQ_NAME);
        goto fail_log;
    }
    dock_publish(st.dock, st.shm);
    inventory_publish(inv, st.shm);
    resmgr_publish(st.rm, st.shm);
    camps.on_exit = on_camp_exit;
    camps.exit_ctx = &st;
    camps_spawn_all(&camps);
    camps_publish_shm(&camps, st.shm);
    camps_save(&camps);

    /* Main loop. SIGCHLD/SIGTERM interrupt mq_timedreceive (EINTR), so reaping and
     * shutdown happen promptly; the 500 ms timeout bounds the check-then-block race. */
    while (!g_stop) {
        int r;

        /* Dock counters and inventory (a reader) first: keep the window between the g_child
         * check below and mq_timedreceive as short as possible. */
        dock_publish(st.dock, st.shm);
        inventory_publish(inv, st.shm);
        resmgr_publish(st.rm, st.shm);
        if (g_child) {
            g_child = 0;            /* clear before reaping so no SIGCHLD is lost */
            /* Whatever a camp sent before it exited is already queued: handle it first, so
             * its last RELEASEs and BYE come before the exit releases all it still holds. */
            int r0;
            while ((r0 = receive_one(mq, 0, &camps, &st)) == RECV_MSG)
                ;
            if (r0 == RECV_SHUTDOWN) {
                shutdown_msg = 1;
                break;
            }
            camps_reap(&camps, 0);
        }
        if (camps.dirty) {          /* keep the status table and camps.pids current */
            camps_publish_shm(&camps, st.shm);
            camps_save(&camps);
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
    camps_unpublish(&camps);    /* camps are going away: no more alert/dump targets */
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

    /* Stop the rescue teams (unfinished requests are logged as abandoned) and join them,
     * then the dock (pending orders abandoned, trucks stopped with poison pills). */
    dispatch_stop(st.disp);
    st.disp = NULL;
    resmgr_stop(st.rm);         /* waiting Banker's requests are abandoned */
    st.rm = NULL;
    dock_stop(st.dock);
    st.dock = NULL;
    inventory_free(inv);

    for (size_t i = 0; i < camps.n; i++)
        EV_INFO("camp_final", "camp=%u name=%s state=%s restarts=%u", camps.camps[i].id,
                camps.camps[i].name, camp_state_name(camps.camps[i].state),
                camps.camps[i].restarts);
    EV_INFO("daemon_stop", "hello=%lu request=%lu bye=%lu",
            st.counts[MSG_HELLO], st.counts[MSG_REQUEST], st.counts[MSG_BYE]);

    camps_publish_shm(&camps, st.shm);     /* final states, for anyone still reading */
    if (relief_shm_destroy(st.shm) == -1)
        LOG_WARN("shm_unlink %s: %s", RELIEF_SHM_NAME, strerror(errno));
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
