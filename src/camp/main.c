/*
 * OS concept: Process as an IPC client (POSIX message-queue sender) and signal-driven termination — Unit 2
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
#define DEFAULT_TIME_SCALE   100     /* ms of real time per simulated minute */

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    g_stop = sig;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --id N --scenario FILE [--time-scale MS] [--seed S]   (resident until SIGTERM)\n"
            "       %s --id N [--count N] [--seed S]   (send N random requests and exit)\n"
            "       %s --shutdown\n"
            "  --time-scale MS  real milliseconds per simulated minute (default %d, 0 = no pacing)\n",
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
 * Send one message without ever blocking indefinitely. When the queue is full,
 * mq_timedsend times out after SEND_RETRY_MS; we log and retry.
 *   max_tries == 0: retry until a stop signal arrives (HELLO, REQUEST)
 *   max_tries  > 0: give up after that many attempts (BYE, SHUTDOWN: sent after the
 *                   stop signal, when reliefd may already be gone)
 */
static int send_msg(mqd_t mq, const relief_msg_t *m, int max_tries)
{
    const char *kind = relief_msg_kind_name(m->kind);

    for (int attempt = 1;; attempt++) {
        struct timespec deadline;

        deadline_after_ms(&deadline, SEND_RETRY_MS);
        if (mq_timedsend(mq, (const char *)m, sizeof *m, 0, &deadline) == 0)
            return 0;
        if (errno != ETIMEDOUT && errno != EINTR) {
            LOG_ERROR("mq_timedsend %s: %s", kind, strerror(errno));
            return -1;
        }
        if (max_tries == 0 && g_stop)
            return -1;      /* asked to stop while waiting for queue space */
        if (max_tries > 0 && attempt >= max_tries) {
            LOG_ERROR("queue full, giving up on %s after %d attempts", kind, attempt);
            return -1;
        }
        if (errno == ETIMEDOUT)
            LOG_WARN("queue full, retrying %s (attempt %d)", kind, attempt);
    }
}

static int send_bye(mqd_t mq, relief_msg_t *m)
{
    memset(&m->u, 0, sizeof m->u);
    m->kind = MSG_BYE;
    return send_msg(mq, m, BYE_MAX_TRIES);
}

static void log_sent(const relief_request_t *r)
{
    LOG_INFO("sent REQUEST id=%u severity=%s resource=%s qty=%u service=%u arrival=%u",
             r->id, relief_severity_name(r->severity), relief_resource_name(r->resource),
             r->quantity, r->service_time, r->arrival);
}

/* Send the random workload. Returns number sent, or -1 on error. */
static long send_random(mqd_t mq, relief_msg_t *msg, uint32_t id, unsigned long count,
                        unsigned long seed)
{
    uint32_t arrival = 0;
    rng_t rng;

    /* Mix the camp id into the seed so camps differ but each stays reproducible. */
    rng_seed(&rng, (uint64_t)seed ^ ((uint64_t)id * 0x9E3779B97F4A7C15ULL));
    for (unsigned long i = 0; i < count && !g_stop; i++) {
        relief_request_t *r = &msg->u.req;

        arrival += rng_range(&rng, 0, 10);
        msg->kind = MSG_REQUEST;
        r->id = id * RELIEF_REQ_ID_BASE + (uint32_t)i + 1;
        r->camp_id = id;
        r->camp_pid = msg->camp_pid;
        r->severity = (uint8_t)rng_range(&rng, 0, SEV_COUNT - 1);
        r->resource = (uint8_t)rng_range(&rng, 0, RES_COUNT - 1);
        r->quantity = (uint16_t)rng_range(&rng, 1, 50);
        r->service_time = rng_range(&rng, 1, 30);
        r->arrival = arrival;
        if (send_msg(mq, msg, 0) == -1)
            return -1;
        log_sent(r);
    }
    return (long)count;
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
        row->req.camp_id = id;
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

/* Sleep until origin + offset_ms on CLOCK_MONOTONIC. Returns -1 if a stop signal arrived. */
static int sleep_until(const struct timespec *origin, uint64_t offset_ms)
{
    struct timespec t = *origin;
    int err;

    t.tv_sec += (time_t)(offset_ms / 1000);
    t.tv_nsec += (long)(offset_ms % 1000) * 1000000L;
    if (t.tv_nsec >= 1000000000L) {
        t.tv_sec++;
        t.tv_nsec -= 1000000000L;
    }
    /* Absolute deadline: no drift accumulates across rows, and EINTR resumes correctly. */
    while ((err = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL)) == EINTR)
        if (g_stop)
            return -1;
    return err == 0 ? 0 : -1;
}

/*
 * Replay this camp's scenario rows on a simulated clock: row with arrival A minutes is
 * sent at camp start + A * time_scale_ms. Returns number sent, or -1 on error.
 */
static long send_scenario(mqd_t mq, relief_msg_t *msg, uint32_t id, const char *path,
                          unsigned long time_scale_ms)
{
    scenario_row_t *rows = NULL;
    struct timespec origin;
    long n, sent = 0;

    clock_gettime(CLOCK_MONOTONIC, &origin);    /* simulated minute 0 */
    n = load_scenario(path, id, &rows);
    if (n < 0)
        return -1;
    LOG_INFO("camp %u: %ld scenario rows, time scale %lu ms per simulated minute",
             id, n, time_scale_ms);

    for (long i = 0; i < n && !g_stop; i++) {
        relief_request_t *q = &msg->u.req;

        if (time_scale_ms > 0 &&
            sleep_until(&origin, (uint64_t)rows[i].req.arrival * time_scale_ms) == -1)
            break;
        *q = rows[i].req;
        q->camp_pid = msg->camp_pid;
        msg->kind = MSG_REQUEST;
        if (send_msg(mq, msg, 0) == -1) {
            if (!g_stop)
                sent = -1;
            break;
        }
        log_sent(q);
        sent++;
    }
    free(rows);
    return sent;
}

/* Sleep until SIGTERM/SIGINT without a check-then-pause race. */
static void wait_for_stop(void)
{
    sigset_t block, old;

    sigemptyset(&block);
    sigaddset(&block, SIGTERM);
    sigaddset(&block, SIGINT);
    sigprocmask(SIG_BLOCK, &block, &old);
    while (!g_stop)
        sigsuspend(&old);   /* atomically unblock and wait */
    sigprocmask(SIG_SETMASK, &old, NULL);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "id",       required_argument, NULL, 'i' },
        { "count",    required_argument, NULL, 'c' },
        { "seed",     required_argument, NULL, 's' },
        { "scenario", required_argument, NULL, 'f' },
        { "time-scale", required_argument, NULL, 't' },
        { "shutdown", no_argument,       NULL, 'S' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    unsigned long id = 0, count = 5, seed = 1, time_scale = DEFAULT_TIME_SCALE;
    int have_id = 0, have_count = 0, shutdown_only = 0, opt;
    const char *scenario = NULL;
    struct sigaction sa;
    relief_msg_t msg;
    long sent;
    mqd_t mq;
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

    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;    /* no SA_RESTART: a blocked mq_send returns EINTR */
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);

    mq = mq_open(RELIEF_MQ_NAME, O_WRONLY);
    if (mq == (mqd_t)-1) {
        LOG_ERROR("mq_open %s: %s (is reliefd running?)", RELIEF_MQ_NAME, strerror(errno));
        return EXIT_FAILURE;
    }

    memset(&msg, 0, sizeof msg);
    msg.camp_id = (uint32_t)id;
    msg.camp_pid = (int32_t)getpid();

    if (shutdown_only) {
        msg.kind = MSG_SHUTDOWN;
        rc = send_msg(mq, &msg, BYE_MAX_TRIES) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        if (rc == EXIT_SUCCESS)
            LOG_INFO("sent SHUTDOWN");
        mq_close(mq);
        return rc;
    }

    msg.kind = MSG_HELLO;
    if (send_msg(mq, &msg, 0) == -1) {
        mq_close(mq);
        return g_stop ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (scenario)
        sent = send_scenario(mq, &msg, (uint32_t)id, scenario, time_scale);
    else
        sent = send_random(mq, &msg, (uint32_t)id, count, seed);
    if (sent < 0 && !g_stop)
        rc = EXIT_FAILURE;

    if (scenario && rc == EXIT_SUCCESS) {
        LOG_INFO("camp %lu resident: sent %ld scenario requests, waiting for SIGTERM", id, sent);
        wait_for_stop();
    }

    if (send_bye(mq, &msg) == -1 && !g_stop)
        rc = EXIT_FAILURE;
    LOG_INFO("done: camp=%lu sent=%ld%s", id, sent < 0 ? 0 : sent,
             g_stop ? " (stopped by signal)" : "");
    mq_close(mq);
    return rc;
}
