/*
 * OS concept: FCFS (first-come, first-served) CPU scheduling — non-preemptive — Unit 2
 *
 * Ready jobs wait in one FIFO queue in arrival order; an idle team always takes the
 * job at the head and keeps it until it finishes. Simple and starvation-free, but a long
 * job at the head makes every short job behind it wait (the convoy effect), and a
 * CRITICAL request gets no precedence over a ROUTINE one that arrived earlier.
 */
#include "sched.h"

#include <stdlib.h>

typedef struct {
    sched_fifo_t ready;
} fcfs_state_t;

static int fcfs_init(sched_policy_t *p, const sched_params_t *params)
{
    (void)params;
    p->state = calloc(1, sizeof(fcfs_state_t));
    return p->state ? 0 : -1;
}

static void fcfs_enqueue(sched_policy_t *p, sched_job_t *job, int64_t now)
{
    fcfs_state_t *s = p->state;
    (void)now;
    sched_fifo_push(&s->ready, job);
}

static sched_job_t *fcfs_pick_next(sched_policy_t *p, int64_t now)
{
    fcfs_state_t *s = p->state;
    (void)now;
    return sched_fifo_pop(&s->ready);
}

static void fcfs_destroy(sched_policy_t *p)
{
    free(p->state);     /* jobs belong to the caller; the queue only links them */
    p->state = NULL;
}

sched_policy_t *sched_fcfs_new(void)
{
    sched_policy_t *p = calloc(1, sizeof *p);

    if (!p)
        return NULL;
    p->name = "fcfs";
    p->preemptive = 0;
    p->init = fcfs_init;
    p->enqueue = fcfs_enqueue;
    p->pick_next = fcfs_pick_next;
    p->on_tick = NULL;          /* non-preemptive, no aging */
    p->on_finish = NULL;
    p->destroy = fcfs_destroy;
    return p;
}
