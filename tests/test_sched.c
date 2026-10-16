/*
 * OS concept: CPU scheduling — unit tests for the policy interface, FCFS and the
 * scheduling-criteria metrics — Unit 2
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

static sim_config_t config(int teams)
{
    sim_config_t c = { teams, 10, 120, 0, 0, 0 };
    return c;
}

/* Run FCFS over jobs; returns sim_run's result. */
static int run_fcfs(sched_job_t *jobs, size_t n, const sim_config_t *cfg, sim_metrics_t *m,
                    sim_trace_t *trace)
{
    sched_params_t params = { cfg->teams, cfg->quantum, cfg->seed };
    sched_policy_t *p = sched_policy_create("fcfs", &params);
    int rc;

    if (!p)
        return -1;
    rc = sim_run(p, jobs, n, cfg, m, trace);
    sched_policy_free(p);
    return rc;
}

/* Silberschatz et al., Operating System Concepts, FCFS example:
 * P1 = 24, P2 = 3, P3 = 3, all arriving at 0, one CPU, order P1, P2, P3.
 * Waiting times 0, 24, 27 -> average 17; turnaround 24, 27, 30 -> average 27. */
static void test_textbook_fcfs(void)
{
    sched_job_t jobs[] = { job(1, 0, 24, 2), job(2, 0, 3, 2), job(3, 0, 3, 2) };
    sim_config_t cfg = config(1);
    sim_metrics_t m;

    printf("textbook FCFS: P1=24, P2=3, P3=3 at t=0, 1 CPU\n");
    CHECK(run_fcfs(jobs, 3, &cfg, &m, NULL) == 0);
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

/* Same jobs in the order P2, P3, P1: the textbook's other case, average wait 3.
 * The difference between 17 and 3 is the convoy effect. */
static void test_textbook_fcfs_reordered(void)
{
    sched_job_t jobs[] = { job(2, 0, 3, 2), job(3, 0, 3, 2), job(1, 0, 24, 2) };
    sim_config_t cfg = config(1);
    sim_metrics_t m;

    printf("textbook FCFS reordered: P2, P3, P1 -> average wait 3 (convoy effect avoided)\n");
    CHECK(run_fcfs(jobs, 3, &cfg, &m, NULL) == 0);
    CHECK_NEAR(m.all.avg_wait, 3.0);
    CHECK_NEAR(m.all.avg_turnaround, 13.0);
}

/* Two teams: P1 on team 1 [0,24), P2 then P3 on team 2 [0,3) [3,6). */
static void test_two_teams(void)
{
    sched_job_t jobs[] = { job(1, 0, 24, 2), job(2, 0, 3, 2), job(3, 0, 3, 2) };
    sim_config_t cfg = config(2);
    sim_metrics_t m;
    sim_trace_t tr;

    printf("FCFS on 2 teams\n");
    CHECK(sim_trace_init(&tr, 2, 30) == 0);
    CHECK(run_fcfs(jobs, 3, &cfg, &m, &tr) == 0);
    CHECK(jobs[0].start == 0 && jobs[1].start == 0 && jobs[2].start == 3);
    CHECK_NEAR(m.all.avg_wait, 1.0);
    CHECK(m.context_switches == 1);          /* team 2: P2 -> P3 */
    CHECK(m.makespan == 24 && m.busy == 30);
    CHECK_NEAR(m.utilisation, 30.0 / 48.0);
    CHECK(tr.cell[0] == &jobs[0] && tr.cell[23] == &jobs[0]);       /* team 1 */
    CHECK(tr.cell[30 + 2] == &jobs[1] && tr.cell[30 + 3] == &jobs[2]); /* team 2 */
    CHECK(tr.cell[30 + 6] == NULL);
    sim_trace_free(&tr);
}

/* Arrival order, not id order; an idle gap is skipped and is not a context switch. */
static void test_arrivals_and_idle(void)
{
    sched_job_t jobs[] = { job(9, 0, 5, 1), job(1, 2, 4, 0), job(5, 100, 5, 2) };
    sim_config_t cfg = config(1);
    sim_metrics_t m;

    printf("FCFS by arrival time, idle gap\n");
    CHECK(run_fcfs(jobs, 3, &cfg, &m, NULL) == 0);
    CHECK(jobs[0].start == 0 && jobs[0].finish == 5);
    CHECK(jobs[1].start == 5 && jobs[1].wait == 3);     /* arrived at 2, waited for job 9 */
    CHECK(jobs[2].start == 100 && jobs[2].wait == 0);   /* team idle 9..100 */
    CHECK(m.makespan == 105 && m.busy == 14);
    CHECK(m.context_switches == 1);                     /* 9 -> 1 only; not across the gap */
    CHECK_NEAR(m.sev[0].avg_wait, 3.0);                 /* the CRITICAL job */
    CHECK(m.sev[0].n == 1 && m.sev[1].n == 1 && m.sev[2].n == 1);
}

/* Starvation threshold and per-severity grouping on the textbook jobs. */
static void test_starvation(void)
{
    sched_job_t jobs[] = { job(1, 0, 24, 2), job(2, 0, 3, 0), job(3, 0, 3, 0) };
    sim_config_t cfg = config(1);
    sim_metrics_t m;

    printf("starvation threshold\n");
    cfg.starve_threshold = 20;
    CHECK(run_fcfs(jobs, 3, &cfg, &m, NULL) == 0);
    CHECK(m.starved == 2);                              /* waits 24 and 27 exceed 20 */
    CHECK_NEAR(m.sev[0].avg_wait, 25.5);                /* CRITICAL: (24 + 27) / 2 */
    CHECK(m.sev[0].max_wait == 27 && m.sev[2].max_wait == 0);
    cfg.starve_threshold = 27;
    CHECK(run_fcfs(jobs, 3, &cfg, &m, NULL) == 0);
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
        a[i] = job(i + 1, 0, 1, 2);
    CHECK(run_fcfs(a, 6, &cfg, &m, NULL) == 0);
    for (int i = 0; i < 6; i++)
        CHECK(a[i].start == i);                         /* input order */

    cfg.seed_set = 1;
    cfg.seed = 42;
    memcpy(b, a, sizeof a);
    CHECK(run_fcfs(a, 6, &cfg, &m, NULL) == 0);
    CHECK(run_fcfs(b, 6, &cfg, &m, NULL) == 0);
    for (int i = 0; i < 6; i++) {
        CHECK(a[i].start == b[i].start);                /* deterministic */
        differs |= a[i].start != i;
    }
    CHECK(differs);                                     /* seed 42 permutes the 6 ties */
}

static void test_registry(void)
{
    sched_params_t params = { 1, 10, 0 };
    sched_policy_t *p = sched_policy_create("fcfs", &params);

    printf("policy registry\n");
    CHECK(p != NULL);
    if (p) {
        CHECK(strcmp(p->name, "fcfs") == 0 && p->preemptive == 0);
        CHECK(p->init && p->enqueue && p->pick_next && p->destroy);
        CHECK(p->pick_next(p, 0) == NULL);              /* empty ready queue */
    }
    sched_policy_free(p);
    CHECK(sched_policy_create("no-such-policy", &params) == NULL);
    CHECK(strstr(sched_policy_names(), "fcfs") != NULL);
}

int main(void)
{
    test_textbook_fcfs();
    test_textbook_fcfs_reordered();
    test_two_teams();
    test_arrivals_and_idle();
    test_starvation();
    test_tie_order();
    test_registry();

    if (g_failures) {
        printf("test_sched: %d of %d checks FAILED\n", g_failures, g_checks);
        return 1;
    }
    printf("test_sched: all %d checks passed\n", g_checks);
    return 0;
}
