/*
 * OS concept: Process as an IPC client (POSIX message-queue sender) and signal-driven termination — Unit 2
 *
 * A relief camp. Two modes:
 *   --scenario FILE  (how reliefd starts camps) replay this camp's rows from the scenario
 *                    CSV, then stay resident until SIGTERM/SIGINT, send BYE and exit 0.
 *   --count N        (manual/testing) send N random requests (deterministic for --seed
 *                    and --id), then BYE, and exit.
 * Both send HELLO first. --shutdown instead sends a single SHUTDOWN message (admin/test hook).
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

#define BYE_TIMEOUT_MS 1000

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    g_stop = sig;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --id N --scenario FILE [--seed S]   (resident until SIGTERM)\n"
            "       %s --id N [--count N] [--seed S]       (send N random requests and exit)\n"
            "       %s --shutdown\n", prog, prog, prog);
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

static int send_msg(mqd_t mq, const relief_msg_t *m)
{
    if (mq_send(mq, (const char *)m, sizeof *m, 0) == -1) {
        if (errno == EINTR && g_stop)
            return -1;      /* asked to stop while blocked on a full queue */
        LOG_ERROR("mq_send %s: %s", relief_msg_kind_name(m->kind), strerror(errno));
        return -1;
    }
    return 0;
}

/* BYE must not block forever if reliefd is gone or the queue stays full. */
static int send_bye(mqd_t mq, relief_msg_t *m)
{
    struct timespec deadline;

    memset(&m->u, 0, sizeof m->u);
    m->kind = MSG_BYE;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += BYE_TIMEOUT_MS / 1000;
    if (mq_timedsend(mq, (const char *)m, sizeof *m, 0, &deadline) == -1) {
        LOG_ERROR("mq_timedsend BYE: %s", strerror(errno));
        return -1;
    }
    return 0;
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
        if (send_msg(mq, msg) == -1)
            return -1;
        log_sent(r);
    }
    return (long)count;
}

/* Send this camp's rows of the scenario CSV. Returns number sent, or -1 on error. */
static long send_scenario(mqd_t mq, relief_msg_t *msg, uint32_t id, const char *path)
{
    FILE *f = fopen(path, "r");
    char line[256];
    unsigned lineno = 0;
    long sent = 0;

    if (!f) {
        LOG_ERROR("cannot open scenario %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof line, f) && !g_stop) {
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

        relief_request_t *q = &msg->u.req;
        msg->kind = MSG_REQUEST;
        q->id = id * RELIEF_REQ_ID_BASE + lineno;   /* stable across restarts */
        q->camp_id = id;
        q->camp_pid = msg->camp_pid;
        q->severity = (uint8_t)s;
        q->resource = (uint8_t)r;
        q->quantity = (uint16_t)qty;
        q->service_time = service;
        q->arrival = arrival;
        if (send_msg(mq, msg) == -1) {
            fclose(f);
            return -1;
        }
        log_sent(q);
        sent++;
    }
    fclose(f);
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
        { "shutdown", no_argument,       NULL, 'S' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    unsigned long id = 0, count = 5, seed = 1;
    int have_id = 0, have_count = 0, shutdown_only = 0, opt;
    const char *scenario = NULL;
    struct sigaction sa;
    relief_msg_t msg;
    long sent;
    mqd_t mq;
    int rc = EXIT_SUCCESS;

    log_init("camp");

    while ((opt = getopt_long(argc, argv, "i:c:s:f:Sh", opts, NULL)) != -1) {
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
        rc = send_msg(mq, &msg) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        if (rc == EXIT_SUCCESS)
            LOG_INFO("sent SHUTDOWN");
        mq_close(mq);
        return rc;
    }

    msg.kind = MSG_HELLO;
    if (send_msg(mq, &msg) == -1) {
        mq_close(mq);
        return g_stop ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (scenario)
        sent = send_scenario(mq, &msg, (uint32_t)id, scenario);
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
