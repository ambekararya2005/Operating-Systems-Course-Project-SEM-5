/*
 * OS concept: SRTF (shortest remaining time first) — preemptive SJF — Unit 2
 *
 * Algorithm: the ready request with the least remaining service time runs. Whenever a
 * request becomes ready with strictly less remaining time than a running one, the running
 * request is preempted (put back in the ready queue) and the shorter one takes its team.
 * Ties go to the earlier arrival, and equal remaining times never preempt.
 *
 * Trade-off: optimal average waiting time overall (provably minimal when bursts are known),
 * at the cost of context switches (a team abandons work in progress) and an even stronger
 * starvation risk than SJF: a long job keeps losing to every newly arriving short one.
 *
 * Disaster relief: a rescue team would be pulled off a half-done boat evacuation to drop
 * off a 3-minute hygiene kit. Average numbers look great; the long, often life-critical
 * rescues are the ones that wait. Real preemption also has a cost (travel, hand-over) that
 * this model ignores.
 */
#include "sched.h"

#include <stdlib.h>

typedef struct {
    sched_fifo_t ready;     /* sorted by (remaining, arrival order) */
    int64_t     *keys;      /* scratch: remaining time of the best ready jobs */
    size_t       max_keys;
} srtf_state_t;

static int64_t remaining_key(const sched_job_t *j)
{
    return j->remaining;
}

static int less_remaining(const sched_job_t *a, const sched_job_t *b)
{
    return a->remaining != b->remaining ? a->remaining < b->remaining : a->seq < b->seq;
}

static int srtf_init(sched_policy_t *p, const sched_params_t *params)
{
    srtf_state_t *s = calloc(1, sizeof *s);

    if (!s)
        return -1;
    s->max_keys = 2 * (size_t)(params->teams > 0 ? params->teams : 1);
    s->keys = calloc(s->max_keys, sizeof *s->keys);
    if (!s->keys) {
        free(s);
        return -1;
    }
    p->state = s;
    return 0;
}

static void srtf_enqueue(sched_policy_t *p, sched_job_t *job, int64_t now)
{
    srtf_state_t *s = p->state;
    (void)now;
    sched_fifo_insert(&s->ready, job, less_remaining);
}

static sched_job_t *srtf_pick_next(sched_policy_t *p, int64_t now)
{
    srtf_state_t *s = p->state;
    (void)now;
    return sched_fifo_pop(&s->ready);
}

/* Remaining times of waiting jobs do not change, so the queue stays sorted. */
static void srtf_on_tick(sched_policy_t *p, int64_t now, sched_job_t *const *running, int teams,
                         int *preempt)
{
    srtf_state_t *s = p->state;
    size_t n = sched_fifo_keys(&s->ready, remaining_key, s->keys, s->max_keys);
    (void)now;
    sched_preempt_worst(s->keys, n, running, teams, remaining_key, preempt);
}

static void srtf_destroy(sched_policy_t *p)
{
    srtf_state_t *s = p->state;
    if (s)
        free(s->keys);
    free(s);
    p->state = NULL;
}

sched_policy_t *sched_srtf_new(void)
{
    sched_policy_t *p = calloc(1, sizeof *p);

    if (!p)
        return NULL;
    p->name = "srtf";
    p->preemptive = 1;
    p->init = srtf_init;
    p->enqueue = srtf_enqueue;
    p->pick_next = srtf_pick_next;
    p->on_tick = srtf_on_tick;
    p->destroy = srtf_destroy;
    return p;
}
