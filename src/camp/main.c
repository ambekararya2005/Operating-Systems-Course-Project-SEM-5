/*
 * OS concept: Process as an IPC client (POSIX message-queue sender) — Unit 2
 *
 * Phase 0 skeleton of a relief camp: sends HELLO, then --count random requests
 * (deterministic for a given --seed and --id), then BYE.
 * --shutdown instead sends a single SHUTDOWN message (admin/test hook).
 */
#include "log.h"
#include "relief.h"
#include "rng.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <mqueue.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --id N [--count N] [--seed S]\n"
            "       %s --shutdown\n", prog, prog);
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
        LOG_ERROR("mq_send %s: %s", relief_msg_kind_name(m->kind), strerror(errno));
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "id",       required_argument, NULL, 'i' },
        { "count",    required_argument, NULL, 'c' },
        { "seed",     required_argument, NULL, 's' },
        { "shutdown", no_argument,       NULL, 'S' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    unsigned long id = 0, count = 5, seed = 1;
    int have_id = 0, shutdown_only = 0, opt;
    relief_msg_t msg;
    uint32_t arrival = 0;
    rng_t rng;
    mqd_t mq;
    int rc = EXIT_SUCCESS;

    log_init("camp");

    while ((opt = getopt_long(argc, argv, "i:c:s:Sh", opts, NULL)) != -1) {
        switch (opt) {
        case 'i':
            if (parse_ulong(optarg, UINT32_MAX, &id) == -1) {
                fprintf(stderr, "invalid --id: %s\n", optarg);
                return 2;
            }
            have_id = 1;
            break;
        case 'c':
            if (parse_ulong(optarg, 1000000UL, &count) == -1) {
                fprintf(stderr, "invalid --count: %s\n", optarg);
                return 2;
            }
            break;
        case 's':
            if (parse_ulong(optarg, ULONG_MAX, &seed) == -1) {
                fprintf(stderr, "invalid --seed: %s\n", optarg);
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
    if (optind < argc || (!have_id && !shutdown_only)) {
        usage(argv[0]);
        return 2;
    }

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

    /* Mix the camp id into the seed so camps differ but each stays reproducible. */
    rng_seed(&rng, (uint64_t)seed ^ ((uint64_t)id * 0x9E3779B97F4A7C15ULL));

    msg.kind = MSG_HELLO;
    if (send_msg(mq, &msg) == -1)
        rc = EXIT_FAILURE;

    for (unsigned long i = 0; i < count && rc == EXIT_SUCCESS; i++) {
        relief_request_t *r = &msg.u.req;

        arrival += rng_range(&rng, 0, 10);
        msg.kind = MSG_REQUEST;
        r->id = (uint32_t)(id * 1000000UL + i + 1);
        r->camp_id = (uint32_t)id;
        r->camp_pid = msg.camp_pid;
        r->severity = (uint8_t)rng_range(&rng, 0, SEV_COUNT - 1);
        r->resource = (uint8_t)rng_range(&rng, 0, RES_COUNT - 1);
        r->quantity = (uint16_t)rng_range(&rng, 1, 50);
        r->service_time = rng_range(&rng, 1, 30);
        r->arrival = arrival;

        if (send_msg(mq, &msg) == -1) {
            rc = EXIT_FAILURE;
            break;
        }
        LOG_INFO("sent REQUEST id=%u severity=%s resource=%s qty=%u service=%u arrival=%u",
                 r->id, relief_severity_name(r->severity), relief_resource_name(r->resource),
                 r->quantity, r->service_time, r->arrival);
    }

    memset(&msg.u, 0, sizeof msg.u);
    msg.kind = MSG_BYE;
    if (send_msg(mq, &msg) == -1)
        rc = EXIT_FAILURE;

    LOG_INFO("done: camp=%lu sent %lu requests", id, count);
    mq_close(mq);
    return rc;
}
