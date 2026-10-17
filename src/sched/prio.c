/*
 * OS concept: Priority CPU scheduling — preemptive or non-preemptive — Unit 2
 *
 * Algorithm: priority = triage severity, CRITICAL > URGENT > ROUTINE. An idle team takes
 * the highest-priority ready request; ties are broken FCFS (earlier arrival first).
 *   --preemptive off (default): a running request always finishes.
 *   --preemptive on: when a request becomes ready with STRICTLY higher priority than a
 *   running one, the lowest-priority running request is put back in the queue (it keeps
 *   its place by arrival among its own severity) and the team switches immediately.
 *
 * Trade-off: the most important work is served first, and preemption makes the response
 * for CRITICAL requests almost immediate. The cost is starvation: under a steady stream
 * of higher-priority work, a low-priority request may never run. The classic remedy is
 * aging (see mlfq.c); this policy deliberately has none, to show the problem.
 *
 * Disaster relief: exactly what triage wants for life-threatening requests, but in a
 * long flood with constant CRITICAL calls, a camp's ROUTINE food and hygiene requests can
 * wait for days. "Routine" stops being routine when it never arrives.
 */
#include "sched.h"

#include <stdlib.h>

typedef struct {
    sched_fifo_t ready;     /* sorted by (severity, arrival order) */
    int64_t     *keys;
    size_t       max_keys;
} prio_state_t;

static int64_t severity_key(const sched_job_t *j)
{
    return j->severity;     /* 0 = CRITICAL is the best */
}

static int higher_priority(const sched_job_t *a, const sched_job_t *b)
{
    return a->severity != b->severity ? a->severity < b->severity : a->seq < b->seq;
}

static int prio_init(sched_policy_t *p, const sched_params_t *params)
{
    prio_state_t *s = calloc(1, sizeof *s);

    if (!s)
        return -1;
    s->max_keys = 2 * (size_t)(params->teams > 0 ? params->teams : 1);
    s->keys = calloc(s->max_keys, sizeof *s->keys);
    if (!s->keys) {
        free(s);
        return -1;
    }
    p->preemptive = params->preemptive == 1;
    p->name = p->preemptive ? "prio (preemptive)" : "prio (non-preemptive)";
    p->state = s;
    return 0;
}

static void prio_enqueue(sched_policy_t *p, sched_job_t *job, int64_t now)
{
    prio_state_t *s = p->state;
    (void)now;
    sched_fifo_insert(&s->ready, job, higher_priority);
}

static sched_job_t *prio_pick_next(sched_policy_t *p, int64_t now)
{
    prio_state_t *s = p->state;
    (void)now;
    return sched_fifo_pop(&s->ready);
}

static void prio_on_tick(sched_policy_t *p, int64_t now, sched_job_t *const *running, int teams,
                         int *preempt)
{
    prio_state_t *s = p->state;
    size_t n;
    (void)now;

    if (!p->preemptive)
        return;
    n = sched_fifo_keys(&s->ready, severity_key, s->keys, s->max_keys);
    sched_preempt_worst(s->keys, n, running, teams, severity_key, preempt);
}

static void prio_destroy(sched_policy_t *p)
{
    prio_state_t *s = p->state;
    if (s)
        free(s->keys);
    free(s);
    p->state = NULL;
}

sched_policy_t *sched_prio_new(void)
{
    sched_policy_t *p = calloc(1, sizeof *p);

    if (!p)
        return NULL;
    p->name = "prio";
    p->preemptive = 0;      /* set from params->preemptive in init */
    p->init = prio_init;
    p->enqueue = prio_enqueue;
    p->pick_next = prio_pick_next;
    p->on_tick = prio_on_tick;
    p->destroy = prio_destroy;
    return p;
}
