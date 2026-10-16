/*
 * OS concept: CPU scheduling and scheduling criteria — deterministic simulator — Unit 2
 *
 * schedsim: run one scheduling policy over a scenario with N rescue teams (= CPUs) and
 * report waiting, turnaround and response times, throughput, utilisation, context
 * switches and starvation. Simulated time only, no sleeping: the same arguments always
 * give the same output.
 *
 *   schedsim --policy fcfs --scenario scenarios/flood_pune.csv --teams 3
 *            [--quantum Q] [--seed S] [--starve MIN] [--csv out.csv] [--gantt]
 *
 * Exit status: 0 ok, 1 runtime error, 2 usage error.
 */
#include "sched.h"
#include "sim.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DEFAULT_SCENARIO  "scenarios/flood_pune.csv"
#define DEFAULT_TEAMS     3
#define DEFAULT_QUANTUM   10
#define DEFAULT_STARVE    120
#define GANTT_MINUTES     60

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "usage: %s --policy NAME [--scenario FILE] [--teams N] [--quantum Q] [--seed S]\n"
            "          [--starve MIN] [--csv FILE] [--gantt]\n"
            "  --policy    scheduling policy: %s\n"
            "  --scenario  scenario CSV (default %s)\n"
            "  --teams     rescue teams = CPUs (default %d)\n"
            "  --quantum   time slice in minutes for preemptive policies (default %d)\n"
            "  --seed      order requests arriving in the same minute by a seeded hash\n"
            "              (default: keep scenario order)\n"
            "  --starve    a request waiting longer than this many minutes counts as starved\n"
            "              (default %d)\n"
            "  --csv       write per-request results (id,camp,severity,arrival,burst,start,\n"
            "              finish,wait,turnaround,response) to FILE\n"
            "  --gantt     print a Gantt chart of the first %d minutes per team\n",
            prog, sched_policy_names(), DEFAULT_SCENARIO, DEFAULT_TEAMS, DEFAULT_QUANTUM,
            DEFAULT_STARVE, GANTT_MINUTES);
}

static int parse_long(const char *s, long min, long max, long *out)
{
    char *end;
    long v;

    errno = 0;
    v = strtol(s, &end, 10);
    if (errno || end == s || *end || v < min || v > max)
        return -1;
    *out = v;
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "policy",   required_argument, NULL, 'p' },
        { "scenario", required_argument, NULL, 'f' },
        { "teams",    required_argument, NULL, 'n' },
        { "quantum",  required_argument, NULL, 'q' },
        { "seed",     required_argument, NULL, 's' },
        { "starve",   required_argument, NULL, 'w' },
        { "csv",      required_argument, NULL, 'c' },
        { "gantt",    no_argument,       NULL, 'g' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    const char *policy_name = NULL, *scenario = DEFAULT_SCENARIO, *csv = NULL;
    sim_config_t cfg = { DEFAULT_TEAMS, DEFAULT_QUANTUM, DEFAULT_STARVE, 0, 0, 0 };
    sched_params_t params;
    sched_policy_t *policy;
    sched_job_t *jobs = NULL;
    sim_metrics_t m;
    sim_trace_t trace = { 0, 0, NULL };
    size_t n = 0;
    int gantt = 0, opt, rc = 1;
    long v;

    while ((opt = getopt_long(argc, argv, "p:f:n:q:s:w:c:gh", opts, NULL)) != -1) {
        switch (opt) {
        case 'p': policy_name = optarg; break;
        case 'f': scenario = optarg; break;
        case 'c': csv = optarg; break;
        case 'g': gantt = 1; break;
        case 'n':
            if (parse_long(optarg, 1, 64, &v) == -1) {
                fprintf(stderr, "invalid --teams (1-64): %s\n", optarg);
                return 2;
            }
            cfg.teams = (int)v;
            break;
        case 'q':
            if (parse_long(optarg, 1, 100000, &v) == -1) {
                fprintf(stderr, "invalid --quantum (>= 1): %s\n", optarg);
                return 2;
            }
            cfg.quantum = (int)v;
            break;
        case 's': {
            char *end;
            errno = 0;
            cfg.seed = strtoull(optarg, &end, 10);
            if (errno || end == optarg || *end) {
                fprintf(stderr, "invalid --seed: %s\n", optarg);
                return 2;
            }
            cfg.seed_set = 1;
            break;
        }
        case 'w':
            if (parse_long(optarg, 0, 1000000, &v) == -1) {
                fprintf(stderr, "invalid --starve (minutes >= 0): %s\n", optarg);
                return 2;
            }
            cfg.starve_threshold = v;
            break;
        case 'h': usage(stdout, argv[0]); return 0;
        default:  usage(stderr, argv[0]); return 2;
        }
    }
    if (optind < argc || !policy_name) {
        if (!policy_name)
            fprintf(stderr, "--policy is required\n");
        usage(stderr, argv[0]);
        return 2;
    }

    params.teams = cfg.teams;
    params.quantum = cfg.quantum;
    params.seed = cfg.seed;
    policy = sched_policy_create(policy_name, &params);
    if (!policy) {
        fprintf(stderr, "unknown policy '%s' (available: %s)\n", policy_name, sched_policy_names());
        return 2;
    }
    if (sim_load_scenario(scenario, &jobs, &n) == -1)
        goto out;
    if (gantt) {
        cfg.gantt_minutes = GANTT_MINUTES;
        if (sim_trace_init(&trace, cfg.teams, GANTT_MINUTES) == -1) {
            fprintf(stderr, "out of memory\n");
            goto out;
        }
    }
    if (sim_run(policy, jobs, n, &cfg, &m, gantt ? &trace : NULL) == -1)
        goto out;

    printf("scenario %s\n", scenario);
    sim_print_report(stdout, policy->name, &cfg, &m);
    if (!policy->preemptive)
        printf("  (%s is non-preemptive: --quantum has no effect)\n", policy->name);
    if (gantt) {
        printf("\n");
        sim_print_gantt(stdout, &trace);
    }
    if (csv) {
        if (sim_write_jobs_csv(csv, jobs, n) == -1)
            goto out;
        printf("\nper-request results written to %s\n", csv);
    }
    rc = 0;
out:
    sim_trace_free(&trace);
    free(jobs);
    sched_policy_free(policy);
    return rc;
}
