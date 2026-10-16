/*
 * OS concept: Process as an IPC client (POSIX message queues) and signals as IPC — Unit 2
 *
 * A relief camp. Two modes:
 *   --scenario FILE  (how reliefd starts camps) replay the rows of the scenario CSV whose
 *                    camp_id matches --id, each at its arrival time on a simulated clock
 *                    (--time-scale ms of real time per simulated minute, default 100;
 *                    0 = no pacing), then stay resident until SIGTERM/SIGINT, send BYE, exit 0.
 *   --count N        (manual/testing) send N random requests (deterministic for --seed
 *                    and --id) immediately, then BYE, and exit.
 * Both send HELLO first. --shutdown instead sends a single SHUTDOWN message (admin/test hook).
 * Sends never hang on a full queue: mq_timedsend retries with a "queue full, retrying" warning.
 *
 * Signals (sent by `reliefctl alert|dump` or by reliefd):
 *   SIGUSR1  evacuation alert: the next ALERT_ESCALATIONS requests are sent as CRITICAL
 *   SIGUSR2  status dump: log counters (sent, by severity, escalations, uptime)
 *   SIGTERM  graceful exit: finish the send in progress, send BYE, exit 0 (SIGINT likewise)
 */
#include "log.h"
#include "relief.h"
#include "rng.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <mqueue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SEND_RETRY_MS        500     /* mq_timedsend deadline before warning and retrying */
#define BYE_MAX_TRIES        3       /* BYE/SHUTDOWN give up after this (reliefd may be gone) */
#define STOP_SEND_TRIES      3       /* extra attempts to finish a send after SIGTERM */
#define DEFAULT_TIME_SCALE   100     /* ms of real time per simulated minute */
#define ALERT_ESCALATIONS    5       /* requests escalated to CRITICAL per SIGUSR1 */

/*
 * Signal handlers only set these flags; the main loop does the real work.
 *
 * Why not printf/malloc (or LOG_*) inside a handler? A signal can arrive at any
 * instruction, including while the main flow is itself inside printf or malloc.
 * At that moment stdio holds its internal lock / half-filled buffer and malloc's
 * free lists may be mid-update. Re-entering them from the handler would deadlock on
 * the lock (our logger also takes a pthread mutex) or corrupt the heap/buffers.
 * POSIX only guarantees "async-signal-safe" functions (write, _exit, kill, ...) inside
 * handlers; printf and malloc are not among them. Writing a volatile sig_atomic_t is
 * the one thing a handler can always do safely.
 */
static volatile sig_atomic_t g_stop = 0;    /* SIGTERM/SIGINT: signal number */
static volatile sig_atomic_t g_alert = 0;   /* SIGUSR1 pending */
static volatile sig_atomic_t g_dump = 0;    /* SIGUSR2 pending */

static void on_stop(int sig)  { g_stop = sig; }
static void on_alert(int sig) { (void)sig; g_alert = 1; }
static void on_dump(int sig)  { (void)sig; g_dump = 1; }

/* Camp state; touched only by the main flow, never by handlers. */
typedef struct {
    mqd_t           mq;
    relief_msg_t    msg;
    uint32_t        id;
    struct timespec started;
    unsigned long   sent;
    unsigned long   by_sev[SEV_COUNT];
    unsigned long   escalated;          /* requests sent CRITICAL because of an alert */
    unsigned long   alerts;
    unsigned long   queue_full_retries;
    unsigned        escalate_left;      /* remaining escalations from the latest alert */
    long            rows_total;         /* scenario rows for this camp; -1 in random mode */
} camp_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --id N --scenario FILE [--time-scale MS] [--seed S]   (resident until SIGTERM)\n"
            "       %s --id N [--count N] [--seed S]   (send N random requests and exit)\n"
            "       %s --shutdown\n"
            "  --time-scale MS  real milliseconds per simulated minute (default %d, 0 = no pacing)\n"
            "signals: SIGUSR1 = evacuation alert, SIGUSR2 = status dump, SIGTERM = graceful exit\n",
            prog, prog, prog, DEFAULT_TIME_SCALE);
}

static int parse_ulong(const char *s, unsigned long max, unsigned long *out)
{
    char *end;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || end == s || *end || v > max)
        return -1;
    *out = v;
    return 0;
}

/*
 * SA_RESTART on every camp handler: a signal arriving while mq_timedsend waits for
 * queue space must not abort that send, so the kernel restarts it transparently
 * ("finish the current send"). Sleeps still wake immediately, because
 * clock_nanosleep() and sigsuspend() are never restarted (they return EINTR), so
 * alerts, dumps and SIGTERM are handled promptly while the camp is idle.
 */
static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sa.sa_handler = on_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_alert;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_dump;
    sigaction(SIGUSR2, &sa, NULL);
}

static long uptime_ms(const camp_ctx_t *c)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - c->started.tv_sec) * 1000L +
           (now.tv_nsec - c->started.tv_nsec) / 1000000L;
}

/* Act on pending SIGUSR1/SIGUSR2. Called from the main flow only. */
static void service_signals(camp_ctx_t *c)
{
    if (g_alert) {
        g_alert = 0;
        c->alerts++;
        c->escalate_left = ALERT_ESCALATIONS;
        log_event(LOG_LVL_WARN, "alert", "camp=%u signal=USR1 escalate_next=%d alerts=%lu",
                  c->id, ALERT_ESCALATIONS, c->alerts);
    }
    if (g_dump) {
        g_dump = 0;
        EV_INFO("status", "camp=%u pid=%d sent=%lu critical=%lu urgent=%lu routine=%lu "
                "escalated=%lu alerts=%lu escalate_pending=%u queue_full_retries=%lu "
                "rows_left=%ld uptime_ms=%ld",
                c->id, (int)getpid(), c->sent, c->by_sev[SEV_CRITICAL], c->by_sev[SEV_URGENT],
                c->by_sev[SEV_ROUTINE], c->escalated, c->alerts, c->escalate_left,
                c->queue_full_retries,
                c->rows_total < 0 ? -1L : c->rows_total - (long)c->sent, uptime_ms(c));
    }
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

/*
 * Send c->msg without ever blocking indefinitely. When the queue is full,
 * mq_timedsend times out after SEND_RETRY_MS; we log and retry.
 *   max_tries == 0: retry until stopped; once SIGTERM arrives, the send in progress
 *                   still gets STOP_SEND_TRIES more attempts (graceful: finish it)
 *   max_tries  > 0: give up after that many attempts (BYE, SHUTDOWN: reliefd may be gone)
 */
static int send_msg(camp_ctx_t *c, int max_tries)
{
    const char *kind = relief_msg_kind_name(c->msg.kind);
    int limit = max_tries;

    for (int attempt = 1;; attempt++) {
        struct timespec deadline;

        service_signals(c);
        if (g_stop && limit == 0)
            limit = attempt + STOP_SEND_TRIES - 1;

        deadline_after_ms(&deadline, SEND_RETRY_MS);
        if (mq_timedsend(c->mq, (const char *)&c->msg, sizeof c->msg, 0, &deadline) == 0)
            return 0;
        if (errno != ETIMEDOUT && errno != EINTR) {
            LOG_ERROR("mq_timedsend %s: %s", kind, strerror(errno));
            return -1;
        }
        if (limit > 0 && attempt >= limit) {
            LOG_ERROR("queue full, giving up on %s after %d attempts", kind, attempt);
            return -1;
        }
        if (errno == ETIMEDOUT) {
            c->queue_full_retries++;
            LOG_WARN("queue full, retrying %s (attempt %d)", kind, attempt);
        }
    }
}

static int send_simple(camp_ctx_t *c, relief_msg_kind_t kind, int max_tries)
{
    memset(&c->msg.u, 0, sizeof c->msg.u);
    c->msg.kind = kind;
    return send_msg(c, max_tries);
}

/* Send one request, applying any pending alert escalation. */
static int send_request(camp_ctx_t *c, const relief_request_t *req)
{
    relief_request_t *q = &c->msg.u.req;

    service_signals(c);     /* an alert that just arrived applies to this request */
    c->msg.kind = MSG_REQUEST;
    *q = *req;
    q->camp_id = c->id;
    q->camp_pid = c->msg.camp_pid;
    q->orig_severity = req->severity;
    q->flags = 0;
    if (c->escalate_left > 0) {
        c->escalate_left--;
        c->escalated++;
        q->severity = SEV_CRITICAL;
        q->flags |= REQ_FLAG_ALERT;
    }
    if (send_msg(c, 0) == -1)
        return -1;

    c->sent++;
    c->by_sev[q->severity]++;
    LOG_INFO("sent REQUEST id=%u severity=%s resource=%s qty=%u service=%u arrival=%u%s",
             q->id, relief_severity_name(q->severity), relief_resource_name(q->resource),
             q->quantity, q->service_time, q->arrival,
             (q->flags & REQ_FLAG_ALERT) ? " alert=1" : "");
    return 0;
}

/* Send the random workload immediately. Returns number sent, or -1 on error. */
static long send_random(camp_ctx_t *c, unsigned long count, unsigned long seed)
{
    uint32_t arrival = 0;
    rng_t rng;

    /* Mix the camp id into the seed so camps differ but each stays reproducible. */
    rng_seed(&rng, (uint64_t)seed ^ ((uint64_t)c->id * 0x9E3779B97F4A7C15ULL));
    for (unsigned long i = 0; i < count && !g_stop; i++) {
        relief_request_t r;

        memset(&r, 0, sizeof r);
        arrival += rng_range(&rng, 0, 10);
        r.id = c->id * RELIEF_REQ_ID_BASE + (uint32_t)i + 1;
        r.severity = (uint8_t)rng_range(&rng, 0, SEV_COUNT - 1);
        r.resource = (uint8_t)rng_range(&rng, 0, RES_COUNT - 1);
        r.quantity = (uint16_t)rng_range(&rng, 1, 50);
        r.service_time = rng_range(&rng, 1, 30);
        r.arrival = arrival;
        if (send_request(c, &r) == -1)
            return g_stop ? (long)c->sent : -1;
    }
    return (long)c->sent;
}

typedef struct {
    relief_request_t req;
    unsigned lineno;
} scenario_row_t;

static int row_cmp(const void *a, const void *b)
{
    const scenario_row_t *x = a, *y = b;
    if (x->req.arrival != y->req.arrival)
        return x->req.arrival < y->req.arrival ? -1 : 1;
    return x->lineno < y->lineno ? -1 : (x->lineno > y->lineno);
}

/* Load the scenario rows whose camp_id == id, sorted by arrival. Returns count or -1. */
static long load_scenario(const char *path, uint32_t id, scenario_row_t **rows_out)
{
    FILE *f = fopen(path, "r");
    scenario_row_t *rows = NULL;
    size_t n = 0, cap = 0;
    char line[256];
    unsigned lineno = 0;

    if (!f) {
        LOG_ERROR("cannot open scenario %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        unsigned arrival, camp, qty, service;
        char sev[16], res[16];
        int s, r;

        lineno++;
        if (lineno == 1 || line[0] == '#' || line[0] == '\n')
            continue;   /* header / comments / blank lines */
        if (sscanf(line, "%u,%u,%15[^,],%15[^,],%u,%u", &arrival, &camp, sev, res, &qty,
                   &service) != 6 ||
            (s = relief_severity_parse(sev)) < 0 || (r = relief_resource_parse(res)) < 0 ||
            qty == 0 || qty > UINT16_MAX) {
            LOG_WARN("%s:%u: skipping malformed row", path, lineno);
            continue;
        }
        if (camp != id)
            continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            scenario_row_t *tmp = realloc(rows, ncap * sizeof *rows);
            if (!tmp) {
                LOG_ERROR("out of memory loading scenario");
                free(rows);
                fclose(f);
                return -1;
            }
            rows = tmp;
            cap = ncap;
        }
        scenario_row_t *row = &rows[n++];
        memset(row, 0, sizeof *row);
        row->lineno = lineno;
        row->req.id = id * RELIEF_REQ_ID_BASE + lineno;   /* stable across restarts */
        row->req.severity = (uint8_t)s;
        row->req.resource = (uint8_t)r;
        row->req.quantity = (uint16_t)qty;
        row->req.service_time = service;
        row->req.arrival = arrival;
    }
    fclose(f);
    if (n > 1)
        qsort(rows, n, sizeof *rows, row_cmp);
    *rows_out = rows;
    return (long)n;
}

/* Sleep until origin + offset_ms on CLOCK_MONOTONIC, servicing alerts/dumps that wake
 * us early. Returns -1 if a stop signal arrived. */
static int sleep_until(camp_ctx_t *c, const struct timespec *origin, uint64_t offset_ms)
{
    struct timespec t = *origin;
    int err;

    t.tv_sec += (time_t)(offset_ms / 1000);
    t.tv_nsec += (long)(offset_ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    /* Absolute deadline: no drift across rows, and an interrupted sleep resumes exactly. */
    while ((err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL)) == EINTR) {
        if (g_stop)
            return -1;
        service_signals(c);
    }
    return err == 0 ? 0 : -1;
}

/*
 * Replay this camp's scenario rows on a simulated clock: row with arrival A minutes is
 * sent at camp start + A * time_scale_ms. Returns number sent, or -1 on error.
 */
static long send_scenario(camp_ctx_t *c, const char *path, unsigned long time_scale_ms)
{
    scenario_row_t *rows = NULL;
    long n, rc;

    n = load_scenario(path, c->id, &rows);
    if (n < 0)
        return -1;
    c->rows_total = n;
    LOG_INFO("camp %u: %ld scenario rows, time scale %lu ms per simulated minute",
             c->id, n, time_scale_ms);

    rc = 0;
    for (long i = 0; i < n && !g_stop; i++) {
        if (time_scale_ms > 0 &&
            sleep_until(c, &c->started, (uint64_t)rows[i].req.arrival * time_scale_ms) == -1)
            break;
        if (send_request(c, &rows[i].req) == -1) {
            if (!g_stop)
                rc = -1;
            break;
        }
    }
    free(rows);
    return rc < 0 ? -1 : (long)c->sent;
}

/* Idle until SIGTERM/SIGINT, still answering SIGUSR1/SIGUSR2. All four signals are
 * blocked between checking the flags and sigsuspend(), so none can slip through the gap. */
static void wait_for_stop(camp_ctx_t *c)
{
    sigset_t block, old;

    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    sigaddset(&block, SIGUSR1);
    sigaddset(&block, SIGUSR2);
    sigprocmask(SIG_BLOCK, &block, &old);
    for (;;) {
        service_signals(c);
        if (g_stop)
            break;
        sigsuspend(&old);   /* atomically unblock and wait for a signal */
    }
    sigprocmask(SIG_SETMASK, &old, NULL);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "id",         required_argument, NULL, 'i' },
        { "count",      required_argument, NULL, 'c' },
        { "seed",       required_argument, NULL, 's' },
        { "scenario",   required_argument, NULL, 'f' },
        { "time-scale", required_argument, NULL, 't' },
        { "shutdown",   no_argument,       NULL, 'S' },
        { "help",       no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    static camp_ctx_t camp;
    unsigned long id = 0, count = 5, seed = 1, time_scale = DEFAULT_TIME_SCALE;
    int have_id = 0, have_count = 0, shutdown_only = 0, opt;
    const char *scenario = NULL;
    long sent;
    int rc = EXIT_SUCCESS;

    log_init("camp");

    while ((opt = getopt_long(argc, argv, "i:c:s:f:t:Sh", opts, NULL)) != -1) {
        switch (opt) {
        case 'i':
            if (parse_ulong(optarg, UINT32_MAX / RELIEF_REQ_ID_BASE - 1, &id) == -1 || id == 0) {
                fprintf(stderr, "invalid --id: %s\n", optarg);
                return 2;
            }
            have_id = 1;
            break;
        case 'c':
            if (parse_ulong(optarg, RELIEF_REQ_ID_BASE - 1, &count) == -1) {
                fprintf(stderr, "invalid --count: %s\n", optarg);
                return 2;
            }
            have_count = 1;
            break;
        case 's':
            if (parse_ulong(optarg, ULONG_MAX, &seed) == -1) {
                fprintf(stderr, "invalid --seed: %s\n", optarg);
                return 2;
            }
            break;
        case 'f':
            scenario = optarg;
            break;
        case 't':
            if (parse_ulong(optarg, 60000UL, &time_scale) == -1) {
                fprintf(stderr, "invalid --time-scale (0-60000 ms per simulated minute): %s\n", optarg);
                return 2;
            }
            break;
        case 'S':
            shutdown_only = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }
    if (optind < argc || (!have_id && !shutdown_only) || (scenario && have_count)) {
        usage(argv[0]);
        return 2;
    }

    install_signal_handlers();
    clock_gettime(CLOCK_MONOTONIC, &camp.started);  /* also simulated minute 0 */
    camp.id = (uint32_t)id;
    camp.rows_total = -1;

    camp.mq = mq_open(RELIEF_MQ_NAME, O_WRONLY);
    if (camp.mq == (mqd_t)-1) {
        LOG_ERROR("mq_open %s: %s (is reliefd running?)", RELIEF_MQ_NAME, strerror(errno));
        return EXIT_FAILURE;
    }
    camp.msg.camp_id = camp.id;
    camp.msg.camp_pid = (int32_t)getpid();

    if (shutdown_only) {
        rc = send_simple(&camp, MSG_SHUTDOWN, BYE_MAX_TRIES) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        if (rc == EXIT_SUCCESS)
            LOG_INFO("sent SHUTDOWN");
        mq_close(camp.mq);
        return rc;
    }

    if (send_simple(&camp, MSG_HELLO, 0) == -1) {
        mq_close(camp.mq);
        return g_stop ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (scenario)
        sent = send_scenario(&camp, scenario, time_scale);
    else
        sent = send_random(&camp, count, seed);
    if (sent < 0 && !g_stop)
        rc = EXIT_FAILURE;

    if (scenario && rc == EXIT_SUCCESS) {
        LOG_INFO("camp %u resident: sent %lu scenario requests, waiting for SIGTERM",
                 camp.id, camp.sent);
        wait_for_stop(&camp);
    }

    if (send_simple(&camp, MSG_BYE, BYE_MAX_TRIES) == -1 && !g_stop)
        rc = EXIT_FAILURE;
    LOG_INFO("done: camp=%u sent=%lu escalated=%lu%s", camp.id, camp.sent, camp.escalated,
             g_stop ? " (stopped by signal)" : "");
    mq_close(camp.mq);
    return rc;
}
