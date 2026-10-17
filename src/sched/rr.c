/*
 * OS concept: Round Robin CPU scheduling — preemptive time slicing — Unit 2
 *
 * Algorithm: one FIFO ready queue. A dispatched request may run for one quantum
 * (--quantum, default 10 minutes); if it is not finished by then and someone else is
 * waiting, it goes to the back of the queue and the team takes the next request. If no one
 * is waiting it simply continues with a fresh quantum. Requests that arrive at the moment a
 * quantum expires are queued ahead of the preempted request.
 *
 * Trade-off: fair and starvation-free, with a bounded response time (a request waits at
 * most (n-1) quanta per round). Average waiting/turnaround are usually worse than SJF, and
 * the quantum matters: too large degenerates to FCFS, too small spends the teams on
 * switching (each switch is overhead in reality).
 *
 * Disaster relief: every camp gets a team's attention regularly, nobody is forgotten,
 * but a CRITICAL rescue gets the same share as a ROUTINE delivery, and splitting a rescue
 * into 10-minute visits is wasteful when real hand-overs cost travel time.
 */
#include "sched.h"

#include <stdlib.h>

typedef struct {
    sched_fifo_t ready;
    int64_t      quantum;
} rr_state_t;

static int rr_init(sched_policy_t *p, const sched_params_t *params)
{
    rr_state_t *s = calloc(1, sizeof *s);

    if (!s)
        return -1;
    s->quantum = params->quantum > 0 ? params->quantum : SCHED_DEFAULT_QUANTUM;
    p->state = s;
    return 0;
}

static void rr_enqueue(sched_policy_t *p, sched_job_t *job, int64_t now)
{
    rr_state_t *s = p->state;
    (void)now;
    sched_fifo_push(&s->ready, job);
}

static sched_job_t *rr_pick_next(sched_policy_t *p, int64_t now)
{
    rr_state_t *s = p->state;
    (void)now;
    return sched_fifo_pop(&s->ready);
}

static void rr_on_tick(sched_policy_t *p, int64_t now, sched_job_t *const *running, int teams,
                       int *preempt)
{
    rr_state_t *s = p->state;
    (void)now;

    for (int t = 0; t < teams; t++) {
        sched_job_t *j = running[t];
        if (!j || j->slice < s->quantum)
            continue;
        if (s->ready.len > 0)
            preempt[t] = 1;     /* quantum used up and others are waiting: rotate */
        else
            j->slice = 0;       /* nobody waiting: keep going with a fresh quantum */
    }
}

static void rr_destroy(sched_policy_t *p)
{
    free(p->state);
    p->state = NULL;
}

sched_policy_t *sched_rr_new(void)
{
    sched_policy_t *p = calloc(1, sizeof *p);

    if (!p)
        return NULL;
    p->name = "rr";
    p->preemptive = 1;
    p->uses_quantum = 1;
    p->init = rr_init;
    p->enqueue = rr_enqueue;
    p->pick_next = rr_pick_next;
    p->on_tick = rr_on_tick;
    p->destroy = rr_destroy;
    return p;
}
