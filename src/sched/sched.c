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
