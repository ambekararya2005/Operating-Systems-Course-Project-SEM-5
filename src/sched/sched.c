/*
 * OS concept: CPU scheduling — policy registry and ready-queue helpers — Unit 2
 */
#include "sched.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *name;
    sched_policy_t *(*create)(void);
} policy_entry_t;

/* Add new policies here; everything else finds them by name. */
static const policy_entry_t g_policies[] = {
    { "fcfs", sched_fcfs_new },
    { "sjf",  sched_sjf_new },
    { "srtf", sched_srtf_new },
    { "rr",   sched_rr_new },
    { "prio", sched_prio_new },
    { "mlfq", sched_mlfq_new },
};

#define N_POLICIES (sizeof g_policies / sizeof g_policies[0])

const char *sched_policy_names(void)
{
    static char names[256];    /* built once from the table; single-threaded callers */

    if (!names[0]) {
        size_t off = 0;
        for (size_t i = 0; i < N_POLICIES; i++)
            off += (size_t)snprintf(names + off, sizeof names - off, "%s%s", i ? " " : "",
                                    g_policies[i].name);
    }
    return names;
}

sched_policy_t *sched_policy_create(const char *name, const sched_params_t *params)
{
    for (size_t i = 0; i < N_POLICIES; i++) {
        if (strcmp(name, g_policies[i].name) != 0)
            continue;
        sched_policy_t *p = g_policies[i].create();
        if (!p)
            return NULL;
        p->params = *params;
        if (p->init && p->init(p, params) == -1) {
            free(p);
            return NULL;
        }
        return p;
    }
    return NULL;
}

void sched_policy_free(sched_policy_t *p)
{
    if (!p)
        return;
    if (p->destroy)
        p->destroy(p);
    free(p);
}

void sched_job_reset(sched_job_t *job)
{
    job->remaining = job->burst;
    job->start = -1;
    job->finish = -1;
    job->wait = 0;
    job->last_run = job->arrival;
    job->ready_since = job->arrival;
    job->slice = 0;
    job->queue_level = 0;
    job->team = -1;
    job->next = NULL;
}

void sched_fifo_push(sched_fifo_t *q, sched_job_t *job)
{
    job->next = NULL;
    if (q->tail)
        q->tail->next = job;
    else
        q->head = job;
    q->tail = job;
    q->len++;
}

sched_job_t *sched_fifo_pop(sched_fifo_t *q)
{
    sched_job_t *job = q->head;

    if (!job)
        return NULL;
    q->head = job->next;
    if (!q->head)
        q->tail = NULL;
    job->next = NULL;
    q->len--;
    return job;
}

void sched_fifo_insert(sched_fifo_t *q, sched_job_t *job,
                       int (*before)(const sched_job_t *a, const sched_job_t *b))
{
    sched_job_t **link = &q->head;

    while (*link && !before(job, *link))
        link = &(*link)->next;
    job->next = *link;
    *link = job;
    if (!job->next)
        q->tail = job;
    q->len++;
}

void sched_preempt_worst(const int64_t *ready_keys, size_t n, sched_job_t *const *running,
                         int teams, int64_t (*key)(const sched_job_t *job), int *preempt)
{
    size_t i = 0;
    int idle = 0;

    for (int t = 0; t < teams; t++)
        if (!running[t] || preempt[t])  /* already marked (e.g. MLFQ demotion): about to free */
            idle++;
    i = (size_t)idle < n ? (size_t)idle : n;    /* the best ready jobs take the idle teams */

    for (; i < n; i++) {
        int worst = -1;
        for (int t = 0; t < teams; t++) {
            if (!running[t] || preempt[t])
                continue;
            if (worst < 0 || key(running[t]) > key(running[worst]))
                worst = t;
        }
        if (worst < 0 || ready_keys[i] >= key(running[worst]))
            break;              /* no running job is strictly worse: done */
        preempt[worst] = 1;
    }
}

size_t sched_fifo_keys(const sched_fifo_t *q, int64_t (*key)(const sched_job_t *job),
                       int64_t *keys, size_t max)
{
    size_t n = 0;
    for (const sched_job_t *j = q->head; j && n < max; j = j->next)
        keys[n++] = key(j);
    return n;
}

void sched_emit(sched_policy_t *p, int64_t now, const sched_job_t *job, sched_event_t ev,
                int from_level, int to_level)
{
    if (p->params.on_event)
        p->params.on_event(p->params.event_ctx, now, job, ev, from_level, to_level);
}
