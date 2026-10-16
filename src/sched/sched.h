/*
 * OS concept: CPU scheduling — jobs, ready queues and pluggable scheduling policies — Unit 2
 *
 * The triage scheduler, independent of any IPC. Rescue teams play the role of CPUs and
 * rescue requests the role of jobs (burst = service time). A policy is a table of
 * function pointers, so the simulator (and later reliefd) can swap FCFS, SJF, RR,
 * Priority and MLFQ without changing callers.
 *
 * Include it as "sched.h" from src/sched/ only: src/sched is deliberately not on the
 * global include path, because this file would shadow the system <sched.h>.
 *
 * Time is in simulated minutes. Life of a job in the simulator:
 *   arrival -> enqueue() -> [pick_next() -> runs on a team -> on_tick() may preempt it,
 *   -> enqueue() again] -> remaining == 0 -> on_finish()
 */
#ifndef RELIEF_SCHED_H
#define RELIEF_SCHED_H

#include <stddef.h>
#include <stdint.h>

typedef struct sched_job {
    uint32_t id;            /* request id (camp_id * 1000000 + scenario line) */
    uint32_t camp;
    uint8_t  severity;      /* relief_severity_t: 0 = CRITICAL, 1 = URGENT, 2 = ROUTINE */
    int64_t  arrival;       /* minute the request arrives */
    int64_t  burst;         /* total service time needed (= service_time) */
    int64_t  remaining;     /* service time still needed */
    int64_t  start;         /* first minute on a team, -1 if never run */
    int64_t  finish;        /* minute it completed, -1 until done */
    int64_t  wait;          /* time spent ready but not running (set at finish) */
    int64_t  last_run;      /* end of the last minute it ran (arrival until it first runs) */
    int      queue_level;   /* MLFQ level (0 = top); unused by other policies */
    int      team;          /* team running it, -1 if not running */
    struct sched_job *next; /* intrusive link for policy ready queues */
} sched_job_t;

/* Parameters every policy receives at init (each uses what it needs). */
typedef struct {
    int      teams;         /* number of rescue teams (= CPUs) */
    int      quantum;       /* time slice in minutes for RR / MLFQ */
    uint64_t seed;
} sched_params_t;

typedef struct sched_policy sched_policy_t;

struct sched_policy {
    const char *name;
    int         preemptive;     /* 1 if on_tick can take a running job off its team */
    void       *state;          /* policy-private data */

    /* Set up state. Returns 0 or -1. */
    int  (*init)(sched_policy_t *p, const sched_params_t *params);
    /* A job became ready: it arrived, or it was preempted. */
    void (*enqueue)(sched_policy_t *p, sched_job_t *job, int64_t now);
    /* Remove and return the next job to run on an idle team, or NULL if none is ready. */
    sched_job_t *(*pick_next)(sched_policy_t *p, int64_t now);
    /* Called at every minute boundary before idle teams are filled. Ages ready jobs and
     * may set preempt[t] = 1 for any team t whose running[t] should go back to the ready
     * queue (running[t] is NULL for an idle team). May be NULL. */
    void (*on_tick)(sched_policy_t *p, int64_t now, sched_job_t *const *running, int teams,
                    int *preempt);
    /* A job completed. May be NULL. */
    void (*on_finish)(sched_policy_t *p, sched_job_t *job, int64_t now);
    /* Free state. */
    void (*destroy)(sched_policy_t *p);
};

/* Create a policy by name ("fcfs", ...) and initialise it. NULL if unknown or init failed. */
sched_policy_t *sched_policy_create(const char *name, const sched_params_t *params);
void sched_policy_free(sched_policy_t *p);

/* Space-separated names of all registered policies (for usage messages). */
const char *sched_policy_names(void);

/* Reset a job's run-time fields so it can be simulated again. */
void sched_job_reset(sched_job_t *job);

/* ---- Simple FIFO of jobs via the intrusive `next` link, shared by policies ---- */
typedef struct {
    sched_job_t *head, *tail;
    size_t       len;
} sched_fifo_t;

void sched_fifo_push(sched_fifo_t *q, sched_job_t *job);
sched_job_t *sched_fifo_pop(sched_fifo_t *q);

/* Policy constructors (one per policy file). */
sched_policy_t *sched_fcfs_new(void);

#endif /* RELIEF_SCHED_H */
