/*
 * OS concept: CPU scheduling — unit tests for the policy interface, FCFS, SJF, SRTF, RR,
 * Priority and MLFQ with aging, and the scheduling-criteria metrics — Unit 2
 *
 * Every textbook answer below is worked out by hand in the comment above its test, then
 * asserted exactly. Each test prints a Gantt chart (run build/test_sched to see them all;
 * they are meant for the report).
 *
 * Built as build/test_sched and run by tests/sched.sh (so `make test`, `make tsan` and
 * `make valgrind` all run it). Exit status: 0 if every check passed, 1 otherwise.
 */
/* Relative paths on purpose: src/sched is not on the include path, because its sched.h
 * would shadow the system <sched.h>. */
#include "../src/sched/sched.h"
#include "../src/sched/sim.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failures;

#define CHECK(cond)                                                                 \
    do {                                                                            \
        g_checks++;                                                                 \
        if (!(cond)) {                                                              \
            g_failures++;                                                           \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);       \
        }                                                                           \
    } while (0)

#define CHECK_NEAR(a, b)                                                            \
    do {                                                                            \
        double a_ = (a), b_ = (b);                                                  \
        g_checks++;                                                                 \
        if (fabs(a_ - b_) > 1e-9) {                                                 \
            g_failures++;                                                           \
            fprintf(stderr, "  FAIL %s:%d: %s = %g, expected %g\n", __FILE__,       \
                    __LINE__, #a, a_, b_);                                          \
        }                                                                           \
    } while (0)

enum { CRIT = 0, URG = 1, ROUT = 2 };

static sched_job_t job(uint32_t id, int64_t arrival, int64_t burst, uint8_t severity)
{
    sched_job_t j;
    memset(&j, 0, sizeof j);
    j.id = id;
    j.camp = 1;
    j.severity = severity;
    j.arrival = arrival;
    j.burst = burst;
    sched_job_reset(&j);
    return j;
}

/* Chart labels: ids 1-49 "P<id>"; 50-99 the severity letter (C/U/R);
 * 100-999 "C<id-100>" (the critical stream); 1000+ "R" (the routine job). */
static const char *label(const sched_job_t *j)
{
    static char buf[16];
    if (j->id < 50)
        snprintf(buf, sizeof buf, "P%u", j->id);
    else if (j->id < 100)
        snprintf(buf, sizeof buf, "%c", "CUR"[j->severity < 3 ? j->severity : 2]);
    else if (j->id < 1000)
        snprintf(buf, sizeof buf, "C%u", j->id - 100);
    else
        snprintf(buf, sizeof buf, "R");
    return buf;
}

typedef struct {
    unsigned long promotions, demotions;
} events_t;

/* Log every promotion (as schedsim does) and count both kinds of event. */
static void on_event(void *ctx, int64_t now, const sched_job_t *j, sched_event_t ev, int from,
                     int to)
{
    events_t *e = ctx;
    if (ev == SCHED_EV_PROMOTE) {
        e->promotions++;
        printf("    t=%lld promote %s Q%d -> Q%d (waited > age)\n", (long long)now, label(j),
               from, to);
    } else {
        e->demotions++;
    }
}

static sim_config_t config(int teams)
{
    sim_config_t c = { teams, SCHED_DEFAULT_QUANTUM, 120, 0, 0, 0, NULL, NULL };
    return c;
}

/* Run policy `name` over jobs. preemptive: 1/0 for prio, -1 = default.
 * trace and ev may be NULL. Returns sim_run's result. */
static int run(const char *name, int preemptive, sched_job_t *jobs, size_t n,
               const sim_config_t *cfg, sim_metrics_t *m, sim_trace_t *trace, events_t *ev)
{
    sched_params_t params;
    sched_policy_t *p;
    int rc;

    memset(&params, 0, sizeof params);
    params.teams = cfg->teams;
    params.quantum = cfg->quantum;
    params.seed = cfg->seed;
    params.preemptive = preemptive;
    params.age = SCHED_DEFAULT_AGE;
    if (ev) {
        params.on_event = on_event;
        params.event_ctx = ev;
    }
    p = sched_policy_create(name, &params);
    if (!p)
        return -1;
    rc = sim_run(p, jobs, n, cfg, m, trace);
    sched_policy_free(p);
    return rc;
}

/* Run with a trace of `minutes` and print the labelled Gantt chart of every team. */
static int run_chart(const char *title, const char *name, int preemptive, sched_job_t *jobs,
                     size_t n, const sim_config_t *cfg, sim_metrics_t *m, events_t *ev,
                     int minutes, int cols_per_min)
{
    sim_trace_t tr;
    int rc;

    if (sim_trace_init(&tr, cfg->teams, minutes) == -1)
        return -1;
    rc = run(name, preemptive, jobs, n, cfg, m, &tr, ev);
    if (rc == 0) {
        printf("  Gantt: %s\n", title);
        for (int t = 0; t < cfg->teams; t++) {
            if (cfg->teams > 1)
                printf("  team %d\n", t + 1);
            sim_print_gantt_labeled(stdout, &tr, t, 0, minutes, label, cols_per_min);
        }
        printf("  average wait %.2f, average turnaround %.2f, average response %.2f, "
               "context switches %lu\n\n",
               m->all.avg_wait, m->all.avg_turnaround, m->all.avg_response, m->context_switches);
    }
    sim_trace_free(&tr);
    return rc;
}

/* ---- FCFS ----------------------------------------------------------------------------
 * Silberschatz et al., Operating System Concepts, FCFS example:
 * P1 = 24, P2 = 3, P3 = 3, all arriving at 0, one CPU, order P1, P2, P3.
 *   | P1 [0,24) | P2 [24,27) | P3 [27,30) |
 *   waits 0, 24, 27 -> average 17; turnaround 24, 27, 30 -> average 27. */
static void test_textbook_fcfs(void)
{
    sched_job_t jobs[] = { job(1, 0, 24, ROUT), job(2, 0, 3, ROUT), job(3, 0, 3, ROUT) };
    sim_config_t cfg = config(1);
    sim_metrics_t m;

    printf("FCFS textbook: P1=24, P2=3, P3=3 at t=0, 1 CPU (expect average wait 17)\n");
    CHECK(run_chart("FCFS, order P1 P2 P3", "fcfs", -1, jobs, 3, &cfg, &m, NULL, 30, 2) == 0);
    CHECK(jobs[0].wait == 0 && jobs[1].wait == 24 && jobs[2].wait == 27);
    CHECK_NEAR(m.all.avg_wait, 17.0);
    CHECK_NEAR(m.all.avg_turnaround, 27.0);
    CHECK_NEAR(m.all.avg_response, 17.0);  /* non-preemptive: response = wait */
    CHECK(m.all.max_wait == 27 && m.max_wait_id == 3);
    CHECK(m.makespan == 30);
    CHECK_NEAR(m.utilisation, 1.0);
    CHECK_NEAR(m.throughput_per_hour, 6.0);  /* 3 jobs in 30 minutes */
    CHECK(m.context_switches == 2);          /* P1 -> P2, P2 -> P3 */
    CHECK(m.completed == 3 && m.starved == 0);
}

/* Same jobs in the order P2, P3, P1 (the textbook's other case):
 *   | P2 [0,3) | P3 [3,6) | P1 [6,30) |   waits 0, 3, 6 -> average 3.
 * 17 vs 3 is the convoy effect: short jobs stuck behind a long one. */
static void test_textbook_fcfs_reordered(void)
{
    sched_job_t jobs[] = { job(2, 0, 3, ROUT), job(3, 0, 3, ROUT), job(1, 0, 24, ROUT) };
    sim_config_t cfg = config(1);
    sim_metrics_t m;

    printf("FCFS textbook reordered: P2, P3, P1 (expect average wait 3: no convoy)\n");
    CHECK(run_chart("FCFS, order P2 P3 P1", "fcfs", -1, jobs, 3, &cfg, &m, NULL, 30, 2) == 0);
    CHECK_NEAR(m.all.avg_wait, 3.0);
    CHECK_NEAR(m.all.avg_turnaround, 13.0);
}

/* Two teams: team 1 runs P1 [0,24); team 2 runs P2 [0,3) then P3 [3,6).
 * waits 0, 0, 3 -> average 1. */
static void test_two_teams(void)
{
    sched_job_t jobs[] = { job(1, 0, 24, ROUT), job(2, 0, 3, ROUT), job(3, 0, 3, ROUT) };
    sim_config_t cfg = config(2);
    sim_metrics_t m;
    sim_trace_t tr;

    printf("FCFS on 2 teams (expect average wait 1)\n");
    CHECK(run_chart("FCFS, 2 teams", "fcfs", -1, jobs, 3, &cfg, &m, NULL, 24, 2) == 0);
    CHECK(jobs[0].start == 0 && jobs[1].start == 0 && jobs[2].start == 3);
    CHECK_NEAR(m.all.avg_wait, 1.0);
    CHECK(m.context_switches == 1);          /* team 2: P2 -> P3 */
    CHECK(m.makespan == 24 && m.busy == 30);
    CHECK_NEAR(m.utilisation, 30.0 / 48.0);

    CHECK(sim_trace_init(&tr, 2, 30) == 0);
    CHECK(run("fcfs", -1, jobs, 3, &cfg, &m, &tr, NULL) == 0);
    CHECK(tr.cell[0] == &jobs[0] && tr.cell[23] == &jobs[0]);       /* team 1 */
    CHECK(tr.cell[30 + 2] == &jobs[1] && tr.cell[30 + 3] == &jobs[2]); /* team 2 */
    CHECK(tr.cell[30 + 6] == NULL);
    sim_trace_free(&tr);
}

/* Arrival order, not id order; an idle gap is skipped and is not a context switch.
 *   | P9 [0,5) | P1 [5,9) | idle [9,100) | P5 [100,105) | */
static void test_arrivals_and_idle(void)
{
    sched_job_t jobs[] = { job(9, 0, 5, URG), job(1, 2, 4, CRIT), job(5, 100, 5, ROUT) };
    sim_config_t cfg = config(1);
    sim_metrics_t m;

    printf("FCFS by arrival time, idle gap\n");
    CHECK(run_chart("FCFS with an idle gap", "fcfs", -1, jobs, 3, &cfg, &m, NULL, 105, 1) == 0);
    CHECK(jobs[0].start == 0 && jobs[0].finish == 5);
    CHECK(jobs[1].start == 5 && jobs[1].wait == 3);     /* arrived at 2, waited for P9 */
    CHECK(jobs[2].start == 100 && jobs[2].wait == 0);   /* team idle 9..100 */
    CHECK(m.makespan == 105 && m.busy == 14);
    CHECK(m.context_switches == 1);                     /* P9 -> P1 only; not across the gap */
    CHECK_NEAR(m.sev[CRIT].avg_wait, 3.0);
    CHECK(m.sev[CRIT].n == 1 && m.sev[URG].n == 1 && m.sev[ROUT].n == 1);
}

/* Starvation threshold and per-severity grouping on the textbook jobs. */
static void test_starvation_threshold(void)
{
    sched_job_t jobs[] = { job(1, 0, 24, ROUT), job(2, 0, 3, CRIT), job(3, 0, 3, CRIT) };
    sim_config_t cfg = config(1);
    sim_metrics_t m;

    printf("starvation threshold\n");
    cfg.starve_threshold = 20;
    CHECK(run("fcfs", -1, jobs, 3, &cfg, &m, NULL, NULL) == 0);
    CHECK(m.starved == 2);                              /* waits 24 and 27 exceed 20 */
    CHECK_NEAR(m.sev[CRIT].avg_wait, 25.5);             /* (24 + 27) / 2 */
    CHECK(m.sev[CRIT].max_wait == 27 && m.sev[ROUT].max_wait == 0);
    cfg.starve_threshold = 27;
    CHECK(run("fcfs", -1, jobs, 3, &cfg, &m, NULL, NULL) == 0);
    CHECK(m.starved == 0);                              /* strictly greater than */
}

/* Same-minute ties: input order by default; a seed gives another, but reproducible, order. */
static void test_tie_order(void)
{
    sched_job_t a[6], b[6];
    sim_config_t cfg = config(1);
    sim_metrics_t m;
    int differs = 0;

    printf("same-minute arrival order and --seed\n");
    for (uint32_t i = 0; i < 6; i++)
        a[i] = job(i + 1, 0, 1, ROUT);
    CHECK(run("fcfs", -1, a, 6, &cfg, &m, NULL, NULL) == 0);
    for (int i = 0; i < 6; i++)
        CHECK(a[i].start == i);                         /* input order */

    cfg.seed_set = 1;
    cfg.seed = 42;
    memcpy(b, a, sizeof a);
    CHECK(run("fcfs", -1, a, 6, &cfg, &m, NULL, NULL) == 0);
    CHECK(run("fcfs", -1, b, 6, &cfg, &m, NULL, NULL) == 0);
    for (int i = 0; i < 6; i++) {
        CHECK(a[i].start == b[i].start);                /* deterministic */
        differs |= a[i].start != i;
    }
    CHECK(differs);                                     /* seed 42 permutes the 6 ties */
}

/* ---- SJF and SRTF ---------------------------------------------------------------------
 * Silberschatz's standard example, 1 CPU:
 *   P1 arrives 0, burst 8;  P2 arrives 1, burst 4;  P3 arrives 2, burst 9;  P4 arrives 3, burst 5
 *
 * SJF (non-preemptive): at t=0 only P1 is ready, so it runs to completion.
 *   t=8: ready P2(4) P3(9) P4(5) -> P2;  t=12: P4(5) before P3(9);  t=17: P3.
 *   | P1 [0,8) | P2 [8,12) | P4 [12,17) | P3 [17,26) |
 *   waits: P1 0, P2 8-1 = 7, P3 17-2 = 15, P4 12-3 = 9 -> 31 / 4 = 7.75
 *
 * SRTF (preemptive): t=1 P2 arrives with 4 < P1's remaining 7 -> preempt P1.
 *   t=2 P3(9), t=3 P4(5) arrive; neither beats P2's remaining 2-3.
 *   t=5 P2 done; ready P1(7) P3(9) P4(5) -> P4;  t=10 P1(7);  t=17 P3.
 *   | P1 [0,1) | P2 [1,5) | P4 [5,10) | P1 [10,17) | P3 [17,26) |
 *   waits: P1 (17-0-8) = 9, P2 0, P3 (26-2-9) = 15, P4 (10-3-5) = 2 -> 26 / 4 = 6.5
 *   responses: P1 0, P2 0, P3 15, P4 2 -> 4.25; context switches: 4 */
static void sjf_jobs(sched_job_t *jobs)
{
    jobs[0] = job(1, 0, 8, ROUT);
    jobs[1] = job(2, 1, 4, ROUT);
    jobs[2] = job(3, 2, 9, ROUT);
    jobs[3] = job(4, 3, 5, ROUT);
}

static void test_textbook_sjf(void)
{
    sched_job_t jobs[4];
    sim_config_t cfg = config(1);
    sim_metrics_t m;

    printf("SJF textbook: arrivals 0,1,2,3, bursts 8,4,9,5 (expect average wait 7.75)\n");
    sjf_jobs(jobs);
    CHECK(run_chart("SJF (non-preemptive)", "sjf", -1, jobs, 4, &cfg, &m, NULL, 26, 2) == 0);
    CHECK(jobs[0].start == 0 && jobs[1].start == 8 && jobs[3].start == 12 && jobs[2].start == 17);
    CHECK(jobs[0].wait == 0 && jobs[1].wait == 7 && jobs[2].wait == 15 && jobs[3].wait == 9);
    CHECK_NEAR(m.all.avg_wait, 7.75);
    CHECK_NEAR(m.all.avg_response, 7.75);   /* non-preemptive: response = wait */
    CHECK(m.context_switches == 3);
}

static void test_textbook_srtf(void)
{
    sched_job_t jobs[4];
    sim_config_t cfg = config(1);
    sim_metrics_t m;

    printf("SRTF textbook: same jobs, preemptive (expect average wait 6.5)\n");
    sjf_jobs(jobs);
    CHECK(run_chart("SRTF (preemptive SJF)", "srtf", -1, jobs, 4, &cfg, &m, NULL, 26, 2) == 0);
    CHECK(jobs[0].wait == 9 && jobs[1].wait == 0 && jobs[2].wait == 15 && jobs[3].wait == 2);
    CHECK(jobs[0].finish == 17 && jobs[1].finish == 5 && jobs[2].finish == 26 && jobs[3].finish == 10);
    CHECK(jobs[1].start == 1 && jobs[3].start == 5 && jobs[2].start == 17);
    CHECK_NEAR(m.all.avg_wait, 6.5);
    CHECK_NEAR(m.all.avg_response, 4.25);
    CHECK(m.context_switches == 4);         /* P1->P2, P2->P4, P4->P1, P1->P3 */
}

/* ---- Round robin ----------------------------------------------------------------------
 * Silberschatz RR example: P1 = 24, P2 = 3, P3 = 3 at t=0, quantum 4, 1 CPU.
 *   t=4 P1's quantum expires with P2, P3 waiting -> back of the queue.
 *   P2 and P3 finish inside their first quantum. From t=10 P1 runs alone (a fresh quantum
 *   each time, nobody to rotate with).
 *   | P1 [0,4) | P2 [4,7) | P3 [7,10) | P1 [10,30) |
 *   waits: P1 30-24 = 6, P2 4, P3 7 -> 17 / 3 = 5.67 (the textbook prints 5.66)
 *   responses: 0, 4, 7 -> 11 / 3; context switches: 3
 * With quantum >= 24, RR never preempts here and degenerates to FCFS (average 17). */
static void test_textbook_rr(void)
{
    sched_job_t jobs[] = { job(1, 0, 24, ROUT), job(2, 0, 3, ROUT), job(3, 0, 3, ROUT) };
    sim_config_t cfg = config(1);
    sim_metrics_t m;

    printf("RR textbook: P1=24, P2=3, P3=3, quantum 4 (expect average wait 5.66)\n");
    cfg.quantum = 4;
    CHECK(run_chart("RR, quantum 4", "rr", -1, jobs, 3, &cfg, &m, NULL, 30, 2) == 0);
    CHECK(jobs[0].wait == 6 && jobs[1].wait == 4 && jobs[2].wait == 7);
    CHECK(jobs[0].finish == 30 && jobs[1].finish == 7 && jobs[2].finish == 10);
    CHECK_NEAR(m.all.avg_wait, 17.0 / 3.0);
    CHECK(fabs(m.all.avg_wait - 5.66) < 0.01);
    CHECK_NEAR(m.all.avg_response, 11.0 / 3.0);
    CHECK(m.context_switches == 3);

    cfg.quantum = 30;
    CHECK(run("rr", -1, jobs, 3, &cfg, &m, NULL, NULL) == 0);
    CHECK_NEAR(m.all.avg_wait, 17.0);       /* huge quantum = FCFS */
}

/* ---- Priority -------------------------------------------------------------------------
 * R (ROUTINE) arrives 0, burst 10;  C (CRITICAL) arrives 3, burst 4;  U (URGENT) arrives 5, burst 2.
 *
 * Non-preemptive: R keeps the team; at t=10 C beats U.
 *   | R [0,10) | C [10,14) | U [14,16) |   waits R 0, C 7, U 9 -> 16 / 3
 * Preemptive: C preempts R at t=3; U (urgent) does not preempt C; at t=7 U beats R.
 *   | R [0,3) | C [3,7) | U [7,9) | R [9,16) |   waits R 16-0-10 = 6, C 0, U 2 -> 8 / 3
 * Ties are FCFS: C1 (arrives 0, burst 5) and C2 (arrives 1, burst 2) are both CRITICAL, so
 * C2 does not preempt C1:  | C [0,5) | C [5,7) |  C2 waits 4. */
static void test_priority(void)
{
    sched_job_t jobs[3];
    sim_config_t cfg = config(1);
    sim_metrics_t m;
    sched_job_t ties[] = { job(54, 0, 5, CRIT), job(55, 1, 2, CRIT) };

    jobs[0] = job(51, 0, 10, ROUT);
    jobs[1] = job(52, 3, 4, CRIT);
    jobs[2] = job(53, 5, 2, URG);

    printf("Priority by severity, non-preemptive (expect average wait 5.33)\n");
    CHECK(run_chart("prio, non-preemptive", "prio", 0, jobs, 3, &cfg, &m, NULL, 16, 2) == 0);
    CHECK(jobs[0].wait == 0 && jobs[1].wait == 7 && jobs[2].wait == 9);
    CHECK_NEAR(m.all.avg_wait, 16.0 / 3.0);

    printf("Priority by severity, preemptive (expect average wait 2.67)\n");
    CHECK(run_chart("prio, preemptive", "prio", 1, jobs, 3, &cfg, &m, NULL, 16, 2) == 0);
    CHECK(jobs[0].wait == 6 && jobs[1].wait == 0 && jobs[2].wait == 2);
    CHECK(jobs[1].start == 3 && jobs[2].start == 7 && jobs[0].finish == 16);
    CHECK_NEAR(m.all.avg_wait, 8.0 / 3.0);
    CHECK(m.context_switches == 3);         /* R->C, C->U, U->R */

    printf("Priority ties are FCFS (equal severity never preempts)\n");
    CHECK(run("prio", 1, ties, 2, &cfg, &m, NULL, NULL) == 0);
    CHECK(ties[0].finish == 5 && ties[1].start == 5 && ties[1].wait == 4);
}

/* ---- MLFQ -----------------------------------------------------------------------------
 * One CRITICAL job, burst 30, alone: Q0 [0,8) uses its full quantum -> Q1 [8,24) uses its
 * full quantum -> Q2 [24,30). Two demotions; no context switch (same job every time). */
static void test_mlfq_demotion(void)
{
    sched_job_t jobs[] = { job(1, 0, 30, CRIT) };
    sim_config_t cfg = config(1);
    sim_metrics_t m;
    events_t ev = { 0, 0 };

    printf("MLFQ demotion: CRITICAL burst 30 drops Q0 -> Q1 -> Q2\n");
    CHECK(run_chart("mlfq, one long job", "mlfq", -1, jobs, 1, &cfg, &m, &ev, 30, 2) == 0);
    CHECK(jobs[0].finish == 30 && jobs[0].wait == 0);
    CHECK(jobs[0].queue_level == 2);
    CHECK(ev.demotions == 2 && ev.promotions == 0);
    CHECK(m.context_switches == 0);
}

/* mlfq-floor: a CRITICAL job is never demoted below Q1 (default floor).
 *   CRITICAL burst 30 alone: Q0 [0,8) -> demoted to Q1 -> [8,24) uses Q1's 16-minute quantum, but
 *   it is at its floor, so it stays in Q1 (fresh quantum, nobody else waiting) -> [24,30) done.
 *   1 demotion (not 2 as in plain mlfq), final level Q1.
 * The floor applies to CRITICAL only: an URGENT burst 30 still drops Q1 -> Q2 after 16 minutes.
 * With --crit-floor 0 (params.crit_floor = 1) a CRITICAL job is never demoted at all. */
static void test_mlfq_floor(void)
{
    sched_job_t crit[] = { job(1, 0, 30, CRIT) };
    sched_job_t urg[] = { job(2, 0, 30, URG) };
    sim_config_t cfg = config(1);
    sim_metrics_t m;
    events_t ev = { 0, 0 };
    sched_params_t params;
    sched_policy_t *p;

    printf("MLFQ floor: CRITICAL never demoted below Q1\n");
    CHECK(run_chart("mlfq-floor, one long CRITICAL job", "mlfq-floor", -1, crit, 1, &cfg, &m, &ev, 30, 2) == 0);
    CHECK(crit[0].finish == 30 && crit[0].wait == 0);
    CHECK(crit[0].queue_level == 1);
    CHECK(ev.demotions == 1);

    ev.demotions = ev.promotions = 0;
    CHECK(run("mlfq-floor", -1, urg, 1, &cfg, &m, NULL, &ev) == 0);
    CHECK(urg[0].queue_level == 2 && ev.demotions == 1);    /* URGENT: Q1 -> Q2 as usual */

    memset(&params, 0, sizeof params);
    params.teams = 1;
    params.crit_floor = 1;                                  /* floor at Q0 */
    params.on_event = on_event;
    ev.demotions = ev.promotions = 0;
    params.event_ctx = &ev;
    p = sched_policy_create("mlfq-floor", &params);
    CHECK(p != NULL);
    if (p) {
        crit[0] = job(1, 0, 30, CRIT);
        CHECK(sim_run(p, crit, 1, &cfg, &m, NULL) == 0);
        CHECK(crit[0].queue_level == 0 && ev.demotions == 0 && crit[0].finish == 30);
        sched_policy_free(p);
    }
}

/* ---- Starvation and aging -------------------------------------------------------------
 * One team. A steady stream of CRITICAL jobs C0..C49: Ck arrives at 10k with burst 10, so
 * criticals alone keep the team 100% busy until t=500. A ROUTINE job R (burst 5) arrives
 * at t=1.
 *
 * Priority (preemptive or not): at every completion a critical is ready, so R waits.
 *   | C0 [0,10) | C1 [10,20) | ... | C49 [490,500) | R [500,505) |
 *   R waits 505 - 1 - 5 = 499 minutes: starved (threshold 120).
 *
 * MLFQ, age 60. Each Ck enters Q0, uses its 8-minute quantum and is demoted to Q1, where
 * its last 2 minutes run: [10k, 10k+8) in Q0, [10k+8, 10k+10) in Q1. R starts in Q2.
 *   t=62  R has waited 61 > 60 in Q2 -> promoted to Q1 (logged)
 *   t=68  C6 demoted, Q1 = [R, C6]: R runs [68,70) (3 left)
 *   t=70  C7 arrives in Q0 and preempts R; Q1 = [C6, R]
 *   [70,78) C7 -> Q1 = [C6, R, C7];  [78,80) C6 finishes
 *   [80,88) C8 -> Q1 = [R, C7, C8];  [88,90) R (1 left)
 *   t=90  C9 preempts R; [90,98) C9; [98,100) C7 finishes; [100,108) C10; [108,110) C8
 *   [110,118) C11 -> Q1 = [R, C9, C10, C11];  [118,119) R finishes
 *   R waits 119 - 1 - 5 = 113 minutes: served, not starved. One promotion (R), 50 demotions.
 */
static void starvation_jobs(sched_job_t *jobs)
{
    for (uint32_t k = 0; k < 50; k++)
        jobs[k] = job(100 + k, 10 * (int64_t)k, 10, CRIT);
    jobs[50] = job(1000, 1, 5, ROUT);
}

static void test_starvation_and_aging(void)
{
    sched_job_t jobs[51];
    sim_config_t cfg = config(1);
    sim_metrics_t m;
    events_t ev = { 0, 0 };

    printf("Starvation: plain priority never serves R while criticals keep coming\n");
    starvation_jobs(jobs);
    CHECK(run_chart("prio (non-preemptive), first 130 min: R never runs", "prio", 0, jobs, 51,
                    &cfg, &m, NULL, 130, 1) == 0);
    CHECK(jobs[50].start == 500 && jobs[50].finish == 505 && jobs[50].wait == 499);
    CHECK(m.starved == 1 && m.sev[ROUT].max_wait == 499);
    printf("  R starts at %lld after waiting %lld min (starved)\n\n", (long long)jobs[50].start,
           (long long)jobs[50].wait);

    starvation_jobs(jobs);
    CHECK(run("prio", 1, jobs, 51, &cfg, &m, NULL, NULL) == 0);
    CHECK(jobs[50].wait == 499 && m.starved == 1);  /* preemption does not help R either */

    printf("Aging: MLFQ promotes R after 60 min and serves it\n");
    starvation_jobs(jobs);
    CHECK(run_chart("mlfq (age 60), first 130 min", "mlfq", -1, jobs, 51, &cfg, &m, &ev, 130, 1) == 0);
    CHECK(jobs[50].finish == 119 && jobs[50].wait == 113);
    CHECK(ev.promotions == 1 && ev.demotions == 50);
    CHECK(m.starved == 0);
    printf("  R finishes at %lld after waiting %lld min (vs 499 under prio)\n\n",
           (long long)jobs[50].finish, (long long)jobs[50].wait);
}

static void test_registry(void)
{
    static const char *const names[] = { "fcfs", "sjf", "srtf", "rr", "prio", "mlfq", "mlfq-floor" };
    static const int preemptive[] = { 0, 0, 1, 1, 0, 1, 1 };
    sched_params_t params;

    printf("policy registry\n");
    memset(&params, 0, sizeof params);
    params.teams = 1;
    params.quantum = SCHED_DEFAULT_QUANTUM;
    params.preemptive = -1;
    for (size_t i = 0; i < sizeof names / sizeof names[0]; i++) {
        sched_policy_t *p = sched_policy_create(names[i], &params);
        CHECK(p != NULL);
        if (!p)
            continue;
        CHECK(strncmp(p->name, names[i], strlen(names[i])) == 0);
        CHECK(p->preemptive == preemptive[i]);
        CHECK(p->init && p->enqueue && p->pick_next && p->destroy);
        CHECK(p->pick_next(p, 0) == NULL);              /* empty ready queue */
        CHECK(strstr(sched_policy_names(), names[i]) != NULL);
        sched_policy_free(p);
    }
    CHECK(sched_policy_create("no-such-policy", &params) == NULL);
}

int main(void)
{
    test_textbook_fcfs();
    test_textbook_fcfs_reordered();
    test_two_teams();
    test_arrivals_and_idle();
    test_starvation_threshold();
    test_tie_order();
    test_textbook_sjf();
    test_textbook_srtf();
    test_textbook_rr();
    test_priority();
    test_mlfq_demotion();
    test_mlfq_floor();
    test_starvation_and_aging();
    test_registry();

    if (g_failures) {
        printf("test_sched: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_sched: all %d checks passed\n", g_checks);
    return 0;
}
