/*
 * OS concept: CPU scheduling — jobs, ready queues and pluggable scheduling policies — Unit 2
 *
 * The triage scheduler, independent of any IPC. Rescue teams play the role of CPUs and
 * rescue requests the role of jobs (burst = service time). A policy is a table of
 * function pointers, so the simulator (and later reliefd) can swap FCFS, SJF, SRTF, RR,
 * Priority and MLFQ without changing callers. One file per policy: fcfs.c sjf.c srtf.c
 * rr.c prio.c mlfq.c, registered by name in sched.c.
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
    int64_t  ready_since;   /* when it last entered a ready queue (the simulator sets it on
                             * every enqueue; MLFQ resets it on promotion) */
    int64_t  slice;         /* minutes run since it was last dispatched (for quanta) */
    uint64_t seq;           /* arrival order: FCFS tie-break for every policy */
    int      queue_level;   /* MLFQ level (0 = top); unused by other policies */
    int      team;          /* team running it, -1 if not running */
    struct sched_job *next; /* intrusive link for policy ready queues */
} sched_job_t;

/* Scheduling events reported by policies (the scheduler itself has no I/O). */
typedef enum {
    SCHED_EV_PROMOTE,       /* aging moved a waiting job up a level */
    SCHED_EV_DEMOTE         /* a job used its full quantum and moved down a level */
} sched_event_t;

typedef void (*sched_event_fn)(void *ctx, int64_t now, const sched_job_t *job, sched_event_t ev,
                               int from_level, int to_level);

/* Parameters every policy receives at init (each uses what it needs). */
typedef struct {
    int      teams;         /* number of rescue teams (= CPUs) */
    int      quantum;       /* time slice in minutes for RR */
    uint64_t seed;
    int      preemptive;    /* prio: 1 on, 0 off, -1 policy default (off) */
    int64_t  age;           /* mlfq: promote a job that waited longer than this (minutes) */
    sched_event_fn on_event;    /* optional: promotions / demotions */
    void          *event_ctx;
} sched_params_t;

/* Defaults for callers that do not set the newer fields. */
#define SCHED_DEFAULT_QUANTUM 10
#define SCHED_DEFAULT_AGE     60

typedef struct sched_policy sched_policy_t;

struct sched_policy {
    const char *name;
    int         preemptive;     /* 1 if on_tick can take a running job off its team */
    int         uses_quantum;   /* 1 if the --quantum parameter matters */
    void       *state;          /* policy-private data */
    sched_params_t params;      /* copy of the creation parameters */

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

/* Insert job in front of the first queued job e for which before(job, e) is true, so the
 * queue stays sorted best-first. O(n): fine for hundreds of requests. */
void sched_fifo_insert(sched_fifo_t *q, sched_job_t *job,
                       int (*before)(const sched_job_t *a, const sched_job_t *b));

/*
 * Preemption shared by SRTF, preemptive Priority and MLFQ. key() ranks jobs (lower is
 * better); ready_keys[0..n) are the keys of the best ready jobs, best first (n need not
 * exceed 2 * teams). The best ready jobs first fill idle teams; each remaining ready job
 * that is STRICTLY better than the worst running job (not yet marked) preempts it. Equal
 * keys never preempt, which keeps ties first-come, first-served.
 */
void sched_preempt_worst(const int64_t *ready_keys, size_t n, sched_job_t *const *running,
                         int teams, int64_t (*key)(const sched_job_t *job), int *preempt);

/* Fill keys[] with key() of the first max jobs of a best-first queue; returns the count. */
size_t sched_fifo_keys(const sched_fifo_t *q, int64_t (*key)(const sched_job_t *job),
                       int64_t *keys, size_t max);

/* Report an event through params.on_event, if set. */
void sched_emit(sched_policy_t *p, int64_t now, const sched_job_t *job, sched_event_t ev,
                int from_level, int to_level);

/* Policy constructors (one per policy file). */
sched_policy_t *sched_fcfs_new(void);
sched_policy_t *sched_sjf_new(void);
sched_policy_t *sched_srtf_new(void);
sched_policy_t *sched_rr_new(void);
sched_policy_t *sched_prio_new(void);
sched_policy_t *sched_mlfq_new(void);

#endif /* RELIEF_SCHED_H */
