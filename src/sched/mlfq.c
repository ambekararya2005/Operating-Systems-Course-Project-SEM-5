/*
 * OS concept: Multilevel feedback queue (MLFQ) with aging — Unit 2
 *
 * Algorithm: three ready queues, Q0 highest.
 *   Q0: round robin, quantum 8 min    Q1: round robin, quantum 16 min    Q2: FCFS
 *   - A new request enters by severity: CRITICAL -> Q0, URGENT -> Q1, ROUTINE -> Q2.
 *   - An idle team takes the head of the highest non-empty queue.
 *   - Demotion: a request that uses its full quantum in Q0 or Q1 drops one level.
 *   - Preemption: a request ready in a higher queue takes a team from a request running
 *     in a lower one (the preempted request stays at its level, at the back).
 *   - Aging: a request that has waited in Q1 or Q2 longer than --age minutes (default
 *     60) since it entered that queue is promoted one level. Every promotion and demotion
 *     is reported through params.on_event (schedsim logs each promotion).
 *
 * Trade-off: approximates SJF without knowing bursts (short jobs finish within their
 * quantum, long ones sink) and lets severity set the starting point, while aging bounds
 * how long anything can wait at the bottom. The cost is more parameters to tune (levels,
 * quanta, age) and more context switches than plain priority.
 *
 * Disaster relief: CRITICAL rescues start at the top and get teams quickly; long jobs
 * sink so they cannot monopolise every team; and a ROUTINE food request that has waited
 * an hour climbs up instead of waiting forever behind a stream of emergencies.
 */
#include "sched.h"

#include <stdlib.h>

#define MLFQ_LEVELS 3

static const int64_t g_quantum[MLFQ_LEVELS] = { 8, 16, 0 };    /* 0 = FCFS, no quantum */

typedef struct {
    sched_fifo_t q[MLFQ_LEVELS];
    int64_t      age;
    int64_t     *keys;
    size_t       max_keys;
} mlfq_state_t;

static int64_t level_key(const sched_job_t *j)
{
    return j->queue_level;
}

static int mlfq_init(sched_policy_t *p, const sched_params_t *params)
{
    mlfq_state_t *s = calloc(1, sizeof *s);

    if (!s)
        return -1;
    s->age = params->age > 0 ? params->age : SCHED_DEFAULT_AGE;
    s->max_keys = 2 * (size_t)(params->teams > 0 ? params->teams : 1);
    s->keys = calloc(s->max_keys, sizeof *s->keys);
    if (!s->keys) {
        free(s);
        return -1;
    }
    p->state = s;
    return 0;
}

static void mlfq_enqueue(sched_policy_t *p, sched_job_t *job, int64_t now)
{
    mlfq_state_t *s = p->state;
    (void)now;

    if (job->start < 0)     /* new request: the starting level comes from its severity */
        job->queue_level = job->severity < MLFQ_LEVELS ? job->severity : MLFQ_LEVELS - 1;
    sched_fifo_push(&s->q[job->queue_level], job);
}

static sched_job_t *mlfq_pick_next(sched_policy_t *p, int64_t now)
{
    mlfq_state_t *s = p->state;
    (void)now;

    for (int l = 0; l < MLFQ_LEVELS; l++)
        if (s->q[l].len)
            return sched_fifo_pop(&s->q[l]);
    return NULL;
}

/* Promote every job in queue `level` that has waited longer than the age limit. */
static void age_level(sched_policy_t *p, mlfq_state_t *s, int level, int64_t now)
{
    sched_fifo_t keep = { NULL, NULL, 0 };
    sched_job_t *j;

    while ((j = sched_fifo_pop(&s->q[level])) != NULL) {
        if (now - j->ready_since > s->age) {
            j->queue_level = level - 1;
            j->ready_since = now;           /* the next promotion needs another full wait */
            sched_fifo_push(&s->q[level - 1], j);
            sched_emit(p, now, j, SCHED_EV_PROMOTE, level, level - 1);
        } else {
            sched_fifo_push(&keep, j);
        }
    }
    s->q[level] = keep;
}

static void mlfq_on_tick(sched_policy_t *p, int64_t now, sched_job_t *const *running, int teams,
                         int *preempt)
{
    mlfq_state_t *s = p->state;
    size_t n = 0;

    /* 1. Demotion: a full quantum used in Q0/Q1 sends the job one level down. */
    for (int t = 0; t < teams; t++) {
        sched_job_t *j = running[t];
        int l;
        if (!j)
            continue;
        l = j->queue_level;
        if (l < MLFQ_LEVELS - 1 && j->slice >= g_quantum[l]) {
            j->queue_level = l + 1;
            preempt[t] = 1;
            sched_emit(p, now, j, SCHED_EV_DEMOTE, l, l + 1);
        }
    }

    /* 2. Aging. Q1 first, so a job just promoted from Q2 is not promoted twice. */
    for (int l = 1; l < MLFQ_LEVELS; l++)
        age_level(p, s, l, now);

    /* 3. Preemption: ready work in a higher queue displaces work in a lower one. */
    for (int l = 0; l < MLFQ_LEVELS && n < s->max_keys; l++)
        n += sched_fifo_keys(&s->q[l], level_key, s->keys + n, s->max_keys - n);
    sched_preempt_worst(s->keys, n, running, teams, level_key, preempt);
}

static void mlfq_destroy(sched_policy_t *p)
{
    mlfq_state_t *s = p->state;
    if (s)
        free(s->keys);
    free(s);
    p->state = NULL;
}

sched_policy_t *sched_mlfq_new(void)
{
    sched_policy_t *p = calloc(1, sizeof *p);

    if (!p)
        return NULL;
    p->name = "mlfq";
    p->preemptive = 1;
    p->init = mlfq_init;
    p->enqueue = mlfq_enqueue;
    p->pick_next = mlfq_pick_next;
    p->on_tick = mlfq_on_tick;
    p->destroy = mlfq_destroy;
    return p;
}
