/*
 * OS concept: CPU scheduling and scheduling criteria — deterministic simulator — Unit 2
 *
 * schedsim: run one scheduling policy over a scenario with N rescue teams (= CPUs) and
 * report waiting, turnaround and response times, throughput, utilisation, context
 * switches and starvation. Simulated time only, no sleeping: the same arguments always
 * give the same output.
 *
 *   schedsim --policy fcfs|sjf|srtf|rr|prio|mlfq --scenario scenarios/flood_pune.csv --teams 3
 *            [--quantum Q] [--preemptive on|off] [--age MIN] [--seed S] [--starve MIN]
 *            [--csv out.csv] [--gantt] [--summary-csv runs.csv] [--label NAME]
 *
 * --summary-csv appends one row of metrics per run (header written once), which is what
 * the experiment scripts (experiments/run_e1.sh) collect.
 *
 * MLFQ promotions (aging) are logged one per line on stderr; the report shows the number
 * of promotions and demotions.
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

typedef struct {
    unsigned long promotions, demotions;
    FILE         *trace;        /* --trace: every scheduling event as CSV, or NULL */
} event_counts_t;

static void trace_row(FILE *f, int64_t t, const char *ev, const sched_job_t *j, int team)
{
    static const char *const sev[3] = { "CRITICAL", "URGENT", "ROUTINE" };
    fprintf(f, "%lld,%s,%u,%s,%d,%d,%lld,%lld\n", (long long)t, ev, j->id,
            sev[j->severity < 3 ? j->severity : 2], j->queue_level, team + 1,
            (long long)j->remaining, (long long)j->burst);
}

/* Simulator trace hook (arrive / dispatch / preempt / finish). */
static void on_trace(void *ctx, int64_t t, const char *ev, const sched_job_t *job, int team)
{
    event_counts_t *c = ctx;
    if (c->trace)
        trace_row(c->trace, t, ev, job, team);
}

/* Policy event hook: log every promotion, count both kinds. */
static void on_event(void *ctx, int64_t now, const sched_job_t *job, sched_event_t ev,
                     int from, int to)
{
    event_counts_t *c = ctx;

    if (c->trace)
        trace_row(c->trace, now, ev == SCHED_EV_PROMOTE ? "promote" : "demote", job, job->team);
    if (ev == SCHED_EV_PROMOTE) {
        c->promotions++;
        fprintf(stderr, "t=%lld promote request %u Q%d -> Q%d (waited > age in Q%d)\n",
                (long long)now, job->id, from, to, from);
    } else {
        c->demotions++;
    }
}
#define GANTT_MINUTES     60

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "usage: %s --policy NAME [--scenario FILE] [--teams N] [--quantum Q]\n"
            "          [--preemptive on|off] [--age MIN] [--seed S] [--starve MIN] [--csv FILE] [--gantt]\n"
            "  --policy    scheduling policy: %s\n"
            "  --scenario  scenario CSV (default %s)\n"
            "  --teams     rescue teams = CPUs (default %d)\n"
            "  --quantum   RR time slice in minutes (default %d)\n"
            "  --preemptive on|off  prio: preempt lower-severity work (default off)\n"
            "  --age       mlfq: promote a request waiting longer than this many minutes\n"
            "              in its queue (default %d); each promotion is logged on stderr\n"
            "  --crit-floor Q  mlfq-floor: deepest queue (0-2) for CRITICAL jobs (default 1)\n"
            "  --seed      order requests arriving in the same minute by a seeded hash\n"
            "              (default: keep scenario order)\n"
            "  --starve    a request waiting longer than this many minutes counts as starved\n"
            "              (default %d)\n"
            "  --csv       write per-request results (id,camp,severity,arrival,burst,start,\n"
            "              finish,wait,turnaround,response) to FILE\n"
            "  --gantt     print a Gantt chart of the first %d minutes per team\n"
            "  --summary-csv FILE  append one row of run metrics to FILE (header if new)\n"
            "  --label NAME        name of this variant in the summary row (default: policy)\n"
            "  --trace FILE        write every scheduling event as CSV (t,event,id,severity,level,\n"
            "                      team,remaining,burst; level = queue level after the event)\n",
            prog, sched_policy_names(), DEFAULT_SCENARIO, DEFAULT_TEAMS, DEFAULT_QUANTUM,
            SCHED_DEFAULT_AGE, DEFAULT_STARVE, GANTT_MINUTES);
}

/* Append one CSV row of run metrics to path, with a header if the file is new or empty. */
static int write_summary(const char *path, const char *label, const sched_policy_t *p,
                         const char *scenario, const sim_config_t *cfg, long age,
                         const sim_metrics_t *m, const event_counts_t *ev)
{
    static const char *const sev[SIM_SEVERITIES] = { "critical", "urgent", "routine" };
    FILE *f = fopen(path, "a");
    long size;

    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    if (size == 0) {
        fprintf(f, "label,policy,preemptive,teams,quantum,age,seed,scenario,requests,completed,"
                   "avg_wait,avg_turnaround,avg_response,max_wait,starve_threshold,starved,"
                   "throughput_per_hour,utilisation,context_switches,promotions,demotions,makespan");
        for (int s = 0; s < SIM_SEVERITIES; s++)
            fprintf(f, ",%s_n,%s_avg_wait,%s_avg_response,%s_max_wait", sev[s], sev[s], sev[s], sev[s]);
        fprintf(f, "\n");
    }
    fprintf(f, "%s,%s,%d,%d,%d,%ld,%llu,%s,%zu,%zu,%.3f,%.3f,%.3f,%lld,%lld,%zu,%.3f,%.4f,%lu,%lu,%lu,%lld",
            label, p->name, p->preemptive, cfg->teams, cfg->quantum, age,
            cfg->seed_set ? (unsigned long long)cfg->seed : 0ULL, scenario, m->jobs, m->completed,
            m->all.avg_wait, m->all.avg_turnaround, m->all.avg_response, (long long)m->all.max_wait,
            (long long)cfg->starve_threshold, m->starved, m->throughput_per_hour, m->utilisation,
            m->context_switches, ev->promotions, ev->demotions, (long long)m->makespan);
    for (int s = 0; s < SIM_SEVERITIES; s++)
        fprintf(f, ",%zu,%.3f,%.3f,%lld", m->sev[s].n, m->sev[s].avg_wait, m->sev[s].avg_response,
                (long long)m->sev[s].max_wait);
    fprintf(f, "\n");
    if (fclose(f) != 0) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
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
        { "preemptive", required_argument, NULL, 'P' },
        { "age",      required_argument, NULL, 'a' },
        { "crit-floor", required_argument, NULL, 'F' },
        { "seed",     required_argument, NULL, 's' },
        { "starve",   required_argument, NULL, 'w' },
        { "csv",      required_argument, NULL, 'c' },
        { "gantt",    no_argument,       NULL, 'g' },
        { "summary-csv", required_argument, NULL, 'u' },
        { "label",    required_argument, NULL, 'l' },
        { "trace",    required_argument, NULL, 'T' },
        { "help",     no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    const char *policy_name = NULL, *scenario = DEFAULT_SCENARIO, *csv = NULL;
    const char *summary = NULL, *label = NULL, *trace = NULL;
    sim_config_t cfg = { DEFAULT_TEAMS, DEFAULT_QUANTUM, DEFAULT_STARVE, 0, 0, 0, NULL, NULL };
    sched_params_t params;
    event_counts_t events = { 0, 0, NULL };
    int preemptive = -1;
    long age = SCHED_DEFAULT_AGE, crit_floor = -1;
    sched_policy_t *policy;
    sched_job_t *jobs = NULL;
    sim_metrics_t m;
    sim_trace_t gantt_trace = { 0, 0, NULL };
    size_t n = 0;
    int gantt = 0, opt, rc = 1;
    long v;

    while ((opt = getopt_long(argc, argv, "p:f:n:q:P:a:F:s:w:c:gu:l:T:h", opts, NULL)) != -1) {
        switch (opt) {
        case 'p': policy_name = optarg; break;
        case 'f': scenario = optarg; break;
        case 'c': csv = optarg; break;
        case 'g': gantt = 1; break;
        case 'u': summary = optarg; break;
        case 'l': label = optarg; break;
        case 'T': trace = optarg; break;
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
        case 'P':
            if (strcmp(optarg, "on") == 0)
                preemptive = 1;
            else if (strcmp(optarg, "off") == 0)
                preemptive = 0;
            else {
                fprintf(stderr, "invalid --preemptive (on or off): %s\n", optarg);
                return 2;
            }
            break;
        case 'a':
            if (parse_long(optarg, 1, 1000000, &age) == -1) {
                fprintf(stderr, "invalid --age (minutes >= 1): %s\n", optarg);
                return 2;
            }
            break;
        case 'F':
            if (parse_long(optarg, 0, 2, &crit_floor) == -1) {
                fprintf(stderr, "invalid --crit-floor (0, 1 or 2): %s\n", optarg);
                return 2;
            }
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

    memset(&params, 0, sizeof params);
    params.teams = cfg.teams;
    params.quantum = cfg.quantum;
    params.seed = cfg.seed;
    params.preemptive = preemptive;
    params.age = age;
    params.crit_floor = crit_floor >= 0 ? (int)crit_floor + 1 : 0;
    params.on_event = on_event;
    params.event_ctx = &events;
    policy = sched_policy_create(policy_name, &params);
    if (!policy) {
        fprintf(stderr, "unknown policy '%s' (available: %s)\n", policy_name, sched_policy_names());
        return 2;
    }
    if (sim_load_scenario(scenario, &jobs, &n) == -1)
        goto out;
    if (trace) {
        events.trace = fopen(trace, "w");
        if (!events.trace) {
            fprintf(stderr, "cannot write %s: %s\n", trace, strerror(errno));
            goto out;
        }
        fprintf(events.trace, "t,event,id,severity,level,team,remaining,burst\n");
        cfg.trace = on_trace;
        cfg.trace_ctx = &events;
    }
    if (gantt) {
        cfg.gantt_minutes = GANTT_MINUTES;
        if (sim_trace_init(&gantt_trace, cfg.teams, GANTT_MINUTES) == -1) {
            fprintf(stderr, "out of memory\n");
            goto out;
        }
    }
    if (sim_run(policy, jobs, n, &cfg, &m, gantt ? &gantt_trace : NULL) == -1)
        goto out;

    printf("scenario %s\n", scenario);
    sim_print_report(stdout, policy->name, &cfg, &m);
    if (events.promotions || events.demotions)
        printf("  promotions (aging)  %lu\n  demotions           %lu\n", events.promotions,
               events.demotions);
    if (!policy->uses_quantum)
        printf("  (--quantum is only used by rr)\n");
    if (gantt) {
        printf("\n");
        sim_print_gantt(stdout, &gantt_trace);
    }
    if (summary && write_summary(summary, label ? label : policy_name, policy, scenario, &cfg,
                                 age, &m, &events) == -1)
        goto out;
    if (csv) {
        if (sim_write_jobs_csv(csv, jobs, n) == -1)
            goto out;
        printf("\nper-request results written to %s\n", csv);
    }
    rc = 0;
out:
    if (events.trace && fclose(events.trace) != 0) {
        fprintf(stderr, "cannot write %s\n", trace);
        rc = 1;
    }
    sim_trace_free(&gantt_trace);
    free(jobs);
    sched_policy_free(policy);
    return rc;
}
