/*
 * OS concept: Scheduling criteria — deterministic simulation and metrics — Unit 2
 */
#include "sim.h"

#include "relief.h"

#include <ctype.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    sched_job_t *job;
    uint64_t     tie;   /* order among jobs arriving in the same minute */
} arrival_t;

static uint64_t splitmix64(uint64_t z)
{
    z += 0x9E3779B97F4A7C15ULL;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    return z ^ (z >> 31);
}

static int arrival_cmp(const void *a, const void *b)
{
    const arrival_t *x = a, *y = b;
    if (x->job->arrival != y->job->arrival)
        return x->job->arrival < y->job->arrival ? -1 : 1;
    if (x->tie != y->tie)
        return x->tie < y->tie ? -1 : 1;
    return x->job->id < y->job->id ? -1 : (x->job->id > y->job->id);
}

static void summarise(sim_metrics_t *m, const sched_job_t *jobs, size_t n, const sim_config_t *cfg)
{
    double sum_w[SIM_SEVERITIES + 1] = { 0 }, sum_t[SIM_SEVERITIES + 1] = { 0 },
           sum_r[SIM_SEVERITIES + 1] = { 0 };

    m->all.max_wait = -1;
    for (int s = 0; s < SIM_SEVERITIES; s++)
        m->sev[s].max_wait = -1;

    for (size_t i = 0; i < n; i++) {
        const sched_job_t *j = &jobs[i];
        int64_t turnaround = j->finish - j->arrival, response = j->start - j->arrival;
        int s = j->severity < SIM_SEVERITIES ? j->severity : SIM_SEVERITIES - 1;
        sim_group_t *g[2] = { &m->all, &m->sev[s] };

        if (j->finish < 0)
            continue;
        m->completed++;
        if (j->finish > m->makespan)
            m->makespan = j->finish;
        if (j->wait > cfg->starve_threshold)
            m->starved++;
        if (j->wait > m->all.max_wait)
            m->max_wait_id = j->id;
        for (int k = 0; k < 2; k++) {
            g[k]->n++;
            if (j->wait > g[k]->max_wait)
                g[k]->max_wait = j->wait;
        }
        sum_w[SIM_SEVERITIES] += (double)j->wait;       sum_w[s] += (double)j->wait;
        sum_t[SIM_SEVERITIES] += (double)turnaround;    sum_t[s] += (double)turnaround;
        sum_r[SIM_SEVERITIES] += (double)response;      sum_r[s] += (double)response;
    }
    for (int k = 0; k <= SIM_SEVERITIES; k++) {
        sim_group_t *g = k == SIM_SEVERITIES ? &m->all : &m->sev[k];
        if (g->n == 0) {
            g->max_wait = 0;
            continue;
        }
        g->avg_wait = sum_w[k] / (double)g->n;
        g->avg_turnaround = sum_t[k] / (double)g->n;
        g->avg_response = sum_r[k] / (double)g->n;
    }
    if (m->makespan > 0) {
        m->utilisation = (double)m->busy / ((double)cfg->teams * (double)m->makespan);
        m->throughput_per_hour = (double)m->completed * 60.0 / (double)m->makespan;
    }
}

int sim_run(sched_policy_t *p, sched_job_t *jobs, size_t n, const sim_config_t *cfg,
            sim_metrics_t *m, sim_trace_t *trace)
{
    arrival_t *order = NULL;
    sched_job_t **running = NULL, **last_job = NULL;
    int64_t *last_end = NULL;   /* minute each team last gave up a job (finish or preempt) */
    int *preempt = NULL, rc = -1, teams = cfg->teams;
    size_t next = 0, done = 0, ready = 0;
    int64_t t = 0;

    memset(m, 0, sizeof *m);
    m->jobs = n;
    if (teams < 1)
        return -1;
    order = calloc(n ? n : 1, sizeof *order);
    running = calloc((size_t)teams, sizeof *running);
    last_job = calloc((size_t)teams, sizeof *last_job);
    preempt = calloc((size_t)teams, sizeof *preempt);
    last_end = calloc((size_t)teams, sizeof *last_end);
    if (!order || !running || !last_job || !preempt || !last_end)
        goto out;

    for (size_t i = 0; i < n; i++) {
        sched_job_reset(&jobs[i]);
        order[i].job = &jobs[i];
        order[i].tie = cfg->seed_set ? splitmix64(cfg->seed ^ jobs[i].id) : (uint64_t)i;
    }
    qsort(order, n, sizeof *order, arrival_cmp);
    for (size_t i = 0; i < n; i++)
        order[i].job->seq = i;      /* arrival order: the FCFS tie-break for every policy */

    while (done < n) {
        int busy_any = 0;

        /* 1. arrivals */
        while (next < n && order[next].job->arrival <= t) {
            sched_job_t *j = order[next++].job;
            j->ready_since = t;
            p->enqueue(p, j, t);
            ready++;
        }

        /* 2. aging / preemption */
        if (p->on_tick) {
            memset(preempt, 0, (size_t)teams * sizeof *preempt);
            p->on_tick(p, t, running, teams, preempt);
            for (int i = 0; i < teams; i++) {
                if (preempt[i] && running[i]) {
                    sched_job_t *j = running[i];
                    running[i] = NULL;
                    last_end[i] = t;
                    j->team = -1;
                    j->ready_since = t;
                    p->enqueue(p, j, t);
                    ready++;
                }
            }
        }

        /* 3. dispatch onto idle teams */
        for (int i = 0; i < teams && ready > 0; i++) {
            sched_job_t *j;
            if (running[i])
                continue;
            j = p->pick_next(p, t);
            if (!j)
                break;
            ready--;
            /* A context switch: the team hands over directly from one job to another
             * (not when it picks up work after sitting idle). */
            if (last_job[i] && last_job[i] != j && last_end[i] == t)
                m->context_switches++;
            last_job[i] = j;
            if (j->start < 0)
                j->start = t;
            j->slice = 0;           /* a fresh quantum on every dispatch */
            j->team = i;
            running[i] = j;
        }

        /* 4. run one minute */
        for (int i = 0; i < teams; i++) {
            sched_job_t *j = running[i];
            if (!j)
                continue;
            busy_any = 1;
            if (trace && t < trace->minutes)
                trace->cell[(size_t)i * (size_t)trace->minutes + (size_t)t] = j;
            j->remaining--;
            j->slice++;
            j->last_run = t + 1;
            m->busy++;
            if (j->remaining <= 0) {
                j->finish = t + 1;
                j->wait = j->finish - j->arrival - j->burst;
                j->team = -1;
                running[i] = NULL;
                last_end[i] = t + 1;
                done++;
                if (p->on_finish)
                    p->on_finish(p, j, t + 1);
            }
        }

        if (!busy_any) {
            if (ready > 0) {
                fprintf(stderr, "sim: policy %s returned no job with %zu ready at t=%lld\n",
                        p->name, ready, (long long)t);
                goto out;
            }
            if (next < n) {                 /* idle: jump to the next arrival */
                t = order[next].job->arrival > t ? order[next].job->arrival : t + 1;
                continue;
            }
            if (done < n) {
                fprintf(stderr, "sim: %zu jobs never completed\n", n - done);
                goto out;
            }
        }
        t++;
    }
    summarise(m, jobs, n, cfg);
    rc = 0;
out:
    free(order);
    free(running);
    free(last_job);
    free(preempt);
    free(last_end);
    return rc;
}

int sim_load_scenario(const char *path, sched_job_t **jobs_out, size_t *n_out)
{
    FILE *f = fopen(path, "r");
    sched_job_t *jobs = NULL;
    size_t n = 0, cap = 0;
    char line[256];
    unsigned lineno = 0, bad = 0;

    if (!f) {
        fprintf(stderr, "cannot open scenario %s: %s\n", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        unsigned arrival, camp, qty, service;
        char sev[16], res[16];
        int s;

        lineno++;
        if (lineno == 1 || line[0] == '#' || isspace((unsigned char)line[0]))
            continue;
        if (sscanf(line, "%u,%u,%15[^,],%15[^,],%u,%u", &arrival, &camp, sev, res, &qty,
                   &service) != 6 ||
            (s = relief_severity_parse(sev)) < 0 || relief_resource_parse(res) < 0 ||
            service == 0) {
            fprintf(stderr, "%s:%u: skipping malformed row\n", path, lineno);
            bad++;
            continue;
        }
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 256;
            sched_job_t *tmp = realloc(jobs, ncap * sizeof *jobs);
            if (!tmp) {
                free(jobs);
                fclose(f);
                fprintf(stderr, "out of memory loading %s\n", path);
                return -1;
            }
            jobs = tmp;
            cap = ncap;
        }
        sched_job_t *j = &jobs[n++];
        memset(j, 0, sizeof *j);
        j->id = camp * RELIEF_REQ_ID_BASE + lineno;
        j->camp = camp;
        j->severity = (uint8_t)s;
        j->arrival = arrival;
        j->burst = service;
        sched_job_reset(j);
    }
    fclose(f);
    if (n == 0) {
        fprintf(stderr, "%s: no jobs%s\n", path, bad ? " (all rows malformed)" : "");
        free(jobs);
        return -1;
    }
    *jobs_out = jobs;
    *n_out = n;
    return 0;
}

int sim_trace_init(sim_trace_t *tr, int teams, int minutes)
{
    tr->teams = teams;
    tr->minutes = minutes;
    tr->cell = calloc((size_t)teams * (size_t)(minutes > 0 ? minutes : 1), sizeof *tr->cell);
    return tr->cell ? 0 : -1;
}

void sim_trace_free(sim_trace_t *tr)
{
    free(tr->cell);
    tr->cell = NULL;
}

static void print_group(FILE *out, const char *label, const sim_group_t *g)
{
    if (g->n == 0) {
        fprintf(out, "  %-9s %6s\n", label, "-");
        return;
    }
    fprintf(out, "  %-9s %6zu %10.1f %12.1f %10.1f %10lld\n", label, g->n, g->avg_wait,
            g->avg_turnaround, g->avg_response, (long long)g->max_wait);
}

void sim_print_report(FILE *out, const char *policy, const sim_config_t *cfg, const sim_metrics_t *m)
{
    static const char *const sev[SIM_SEVERITIES] = { "CRITICAL", "URGENT", "ROUTINE" };

    fprintf(out, "policy %s, %d team%s, quantum %d, %zu requests (times in simulated minutes)\n",
            policy, cfg->teams, cfg->teams == 1 ? "" : "s", cfg->quantum, m->jobs);
    fprintf(out, "  %-9s %6s %10s %12s %10s %10s\n", "", "jobs", "avg wait", "avg turnaround",
            "avg resp", "max wait");
    print_group(out, "all", &m->all);
    for (int s = 0; s < SIM_SEVERITIES; s++)
        print_group(out, sev[s], &m->sev[s]);
    fprintf(out, "  worst-case wait     %lld min (request %u)\n", (long long)m->all.max_wait,
            m->max_wait_id);
    fprintf(out, "  makespan            %lld min\n", (long long)m->makespan);
    fprintf(out, "  throughput          %.2f requests/hour\n", m->throughput_per_hour);
    fprintf(out, "  team utilisation    %.1f%%\n", 100.0 * m->utilisation);
    fprintf(out, "  context switches    %lu\n", m->context_switches);
    fprintf(out, "  starved (wait > %lld) %zu\n", (long long)cfg->starve_threshold, m->starved);
}

void sim_print_gantt(FILE *out, const sim_trace_t *tr)
{
    static const char sev_letter[SIM_SEVERITIES] = { 'C', 'U', 'R' };

    fprintf(out, "Gantt chart, minutes 0-%d: one column per minute, letter = severity of the job\n"
                 "on that team (C/U/R; case alternates at each job change), '.' = idle\n",
            tr->minutes - 1);
    fprintf(out, "        ");
    for (int t = 0; t < tr->minutes; t += 10)
        fprintf(out, "%-10d", t);
    fprintf(out, "\n");
    for (int i = 0; i < tr->teams; i++) {
        const sched_job_t *prev = NULL;
        int upper = 0;

        fprintf(out, "team %-2d ", i + 1);
        for (int t = 0; t < tr->minutes; t++) {
            const sched_job_t *j = tr->cell[(size_t)i * (size_t)tr->minutes + (size_t)t];
            if (!j) {
                fputc('.', out);
                prev = NULL;
                continue;
            }
            if (j != prev)
                upper = !upper;
            prev = j;
            char c = sev_letter[j->severity < SIM_SEVERITIES ? j->severity : 2];
            fputc(upper ? c : (char)tolower((unsigned char)c), out);
        }
        fprintf(out, "\n");
    }
    for (int i = 0; i < tr->teams; i++) {
        int t = 0;
        fprintf(out, "team %d:", i + 1);
        while (t < tr->minutes) {
            const sched_job_t *j = tr->cell[(size_t)i * (size_t)tr->minutes + (size_t)t];
            int s = t;
            while (t < tr->minutes && tr->cell[(size_t)i * (size_t)tr->minutes + (size_t)t] == j)
                t++;
            if (j)
                fprintf(out, " [%d-%d) #%u %c", s, t, j->id,
                        sev_letter[j->severity < SIM_SEVERITIES ? j->severity : 2]);
        }
        fprintf(out, "\n");
    }
}

#define GANTT_WRAP 100

static void flush_rows(FILE *out, char *bar, size_t *bl, char *axis, size_t *al, int64_t end)
{
    bar[(*bl)++] = '|';
    bar[*bl] = '\0';
    snprintf(axis + *al, 24, "%lld", (long long)end);
    fprintf(out, "  %s\n  %s\n", bar, axis);
    *bl = 0;
    *al = 0;
}

void sim_print_gantt_labeled(FILE *out, const sim_trace_t *tr, int team, int from, int to,
                             sim_label_fn label, int cols_per_min)
{
    char bar[GANTT_WRAP + 128], axis[GANTT_WRAP + 128];
    size_t bl = 0, al = 0;
    int t;

    if (to > tr->minutes)
        to = tr->minutes;
    if (cols_per_min < 1)
        cols_per_min = 1;
    t = from;
    while (t < to) {
        const sched_job_t *j = tr->cell[(size_t)team * (size_t)tr->minutes + (size_t)t];
        int s = t;
        char num[24], text[64];
        size_t w, minw, lab;

        while (t < to && tr->cell[(size_t)team * (size_t)tr->minutes + (size_t)t] == j)
            t++;
        snprintf(text, sizeof text, "%s", j ? label(j) : "");
        snprintf(num, sizeof num, "%d", s);
        lab = strlen(text);
        minw = (lab > strlen(num) ? lab : strlen(num)) + 2;
        w = (size_t)(t - s) * (size_t)cols_per_min;
        if (w < minw)
            w = minw;
        if (bl + w + 1 > GANTT_WRAP && bl > 0)
            flush_rows(out, bar, &bl, axis, &al, s);

        /* bar: "|" then the label centred in w-1 columns ('.' fill when idle) */
        bar[bl++] = '|';
        for (size_t k = 0; k < w - 1; k++) {
            size_t pad = (w - 1 - lab) / 2;
            bar[bl++] = j ? (k >= pad && k < pad + lab ? text[k - pad] : ' ') : '.';
        }
        /* axis: the start time under the "|", padded to the segment width */
        al += (size_t)snprintf(axis + al, sizeof axis - al, "%-*s", (int)w, num);
    }
    if (bl > 0)
        flush_rows(out, bar, &bl, axis, &al, t);
}

int sim_write_jobs_csv(const char *path, const sched_job_t *jobs, size_t n)
{
    static const char *const sev[SIM_SEVERITIES] = { "CRITICAL", "URGENT", "ROUTINE" };
    FILE *f = fopen(path, "w");

    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "id,camp,severity,arrival,burst,start,finish,wait,turnaround,response\n");
    for (size_t i = 0; i < n; i++) {
        const sched_job_t *j = &jobs[i];
        fprintf(f, "%u,%u,%s,%lld,%lld,%lld,%lld,%lld,%lld,%lld\n", j->id, j->camp,
                sev[j->severity < SIM_SEVERITIES ? j->severity : 2], (long long)j->arrival,
                (long long)j->burst, (long long)j->start, (long long)j->finish,
                (long long)j->wait, (long long)(j->finish - j->arrival),
                (long long)(j->start - j->arrival));
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}
