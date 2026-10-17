/*
 * OS concept: SJF (shortest job first) CPU scheduling — non-preemptive — Unit 2
 *
 * Algorithm: when a team becomes free it takes the ready request with the smallest
 * burst (service time); ties go to the earlier arrival. A running request is never
 * interrupted.
 *
 * Trade-off: SJF gives the minimum average waiting time of any non-preemptive policy
 * (running short jobs first shortens everyone's wait), but it needs the burst in advance
 * and a long job can be postponed indefinitely while shorter ones keep arriving.
 *
 * Disaster relief: quick deliveries (hygiene kits, 3-8 min) are cleared fast, but a long
 * rescue (40-60 min boat run) can starve behind a steady stream of short tasks, and
 * severity is ignored entirely: a short ROUTINE request beats a long CRITICAL one.
 */
#include "sched.h"

#include <stdlib.h>

typedef struct {
    sched_fifo_t ready;     /* sorted by (burst, arrival order) */
} sjf_state_t;

static int shorter(const sched_job_t *a, const sched_job_t *b)
{
    return a->burst != b->burst ? a->burst < b->burst : a->seq < b->seq;
}

static int sjf_init(sched_policy_t *p, const sched_params_t *params)
{
    (void)params;
    p->state = calloc(1, sizeof(sjf_state_t));
    return p->state ? 0 : -1;
}

static void sjf_enqueue(sched_policy_t *p, sched_job_t *job, int64_t now)
{
    sjf_state_t *s = p->state;
    (void)now;
    sched_fifo_insert(&s->ready, job, shorter);
}

static sched_job_t *sjf_pick_next(sched_policy_t *p, int64_t now)
{
    sjf_state_t *s = p->state;
    (void)now;
    return sched_fifo_pop(&s->ready);
}

static void sjf_destroy(sched_policy_t *p)
{
    free(p->state);
    p->state = NULL;
}

sched_policy_t *sched_sjf_new(void)
{
    sched_policy_t *p = calloc(1, sizeof *p);

    if (!p)
        return NULL;
    p->name = "sjf";
    p->preemptive = 0;
    p->init = sjf_init;
    p->enqueue = sjf_enqueue;
    p->pick_next = sjf_pick_next;
    p->destroy = sjf_destroy;
    return p;
}
