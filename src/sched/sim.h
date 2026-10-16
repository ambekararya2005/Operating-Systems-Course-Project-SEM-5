/*
 * OS concept: Scheduling criteria — deterministic simulation and metrics — Unit 2
 *
 * Discrete-time simulation of a scheduling policy on `teams` rescue teams (= CPUs) with a
 * 1-minute clock; idle gaps jump straight to the next arrival. No real time passes, and
 * the result depends only on the jobs, the policy and the config.
 *
 * Each minute t:
 *   1. admit jobs with arrival <= t (enqueue)
 *   2. on_tick: aging / preemption (preempted jobs are enqueued again)
 *   3. every idle team takes pick_next() (team 1 first)
 *   4. each busy team runs its job for one minute; a job reaching remaining == 0
 *      finishes at t + 1 (on_finish), freeing the team for minute t + 1
 *
 * Metrics (per job, then averaged overall and per severity):
 *   turnaround = finish - arrival
 *   waiting    = turnaround - burst   (all time spent ready but not running)
 *   response   = first start - arrival
 *   context switch: a team hands over directly from one job to a different one at the
 *                   same minute boundary (after a finish or a preemption); picking up
 *                   work after sitting idle is not a switch
 *   starved:   waiting > starve threshold
 */
#ifndef RELIEF_SCHED_SIM_H
#define RELIEF_SCHED_SIM_H

#include "sched.h"

#include <stdio.h>

#define SIM_SEVERITIES 3

typedef struct {
    int      teams;             /* >= 1 */
    int      quantum;           /* passed to the policy */
    int64_t  starve_threshold;  /* waiting above this counts as starved */
    int      seed_set;          /* 0: same-minute arrivals keep input order */
    uint64_t seed;              /* 1: same-minute arrivals ordered by a seeded hash */
    int      gantt_minutes;     /* record the first N minutes for a Gantt chart (0 = none) */
} sim_config_t;

typedef struct {
    size_t  n;
    double  avg_wait, avg_turnaround, avg_response;
    int64_t max_wait;
} sim_group_t;

typedef struct {
    size_t        jobs, completed;
    sim_group_t   all;
    sim_group_t   sev[SIM_SEVERITIES];  /* CRITICAL, URGENT, ROUTINE */
    uint32_t      max_wait_id;          /* job with the worst-case wait */
    int64_t       makespan;             /* minute the last job finished (clock starts at 0) */
    int64_t       busy;                 /* team-minutes spent running jobs */
    double        utilisation;          /* busy / (teams * makespan), 0..1 */
    double        throughput_per_hour;  /* completed / makespan * 60 */
    unsigned long context_switches;
    size_t        starved;
} sim_metrics_t;

/* Gantt trace: cell[team * minutes + t] = job running in minute t (NULL = idle). */
typedef struct {
    int                 teams, minutes;
    const sched_job_t **cell;
} sim_trace_t;

/* Run the policy over jobs[0..n). Jobs are reset first; afterwards their start, finish
 * and wait fields hold the results. trace may be NULL. Returns 0, or -1 if the policy
 * lost track of a job (it stopped returning ready jobs) or memory ran out. */
int sim_run(sched_policy_t *p, sched_job_t *jobs, size_t n, const sim_config_t *cfg,
            sim_metrics_t *m, sim_trace_t *trace);

/* Read a scenario CSV (arrival_min,camp_id,severity,resource,quantity,service_time).
 * Job ids are camp_id * 1000000 + line, as in the camps. Returns 0 or -1 (message on stderr). */
int sim_load_scenario(const char *path, sched_job_t **jobs, size_t *n);

int  sim_trace_init(sim_trace_t *tr, int teams, int minutes);
void sim_trace_free(sim_trace_t *tr);

void sim_print_report(FILE *out, const char *policy, const sim_config_t *cfg, const sim_metrics_t *m);
void sim_print_gantt(FILE *out, const sim_trace_t *tr);
/* One row per job: id,camp,severity,arrival,burst,start,finish,wait,turnaround,response */
int  sim_write_jobs_csv(const char *path, const sched_job_t *jobs, size_t n);

#endif /* RELIEF_SCHED_SIM_H */
