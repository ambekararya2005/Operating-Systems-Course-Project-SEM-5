/*
 * OS concept: Threads (POSIX pthreads) and live CPU scheduling — Unit 2;
 *             mutex + condition variable protecting a shared ready queue — Unit 3
 *
 * reliefd's live dispatcher. RESCUE requests that arrive over the message queue are
 * handed to the active scheduling policy (src/sched); N rescue-team threads take work
 * from it and "work" for burst x time-scale using nanosleep, one simulated minute at a
 * time. A clock thread calls the policy's on_tick once per simulated minute (aging and
 * preemption decisions); a team whose job was marked for preemption puts it back with
 * its remaining time at its next minute boundary.
 *
 * Synchronisation: one mutex guards the policy (ready queue), the running/preempt
 * arrays, every job field and the counters; teams sleep outside it. Idle teams wait on a
 * condition variable signalled when work is enqueued or preempted. Lock order is always
 * dispatcher mutex -> shared-memory status lock, never the reverse.
 */
#ifndef RELIEFD_DISPATCH_H
#define RELIEFD_DISPATCH_H

#include "relief.h"

#include <stdint.h>

typedef struct dispatcher dispatcher_t;

typedef struct {
    const char    *policy;          /* sched policy name: fcfs sjf srtf rr prio mlfq */
    int            teams;           /* rescue team threads, 1..RELIEF_SHM_TEAMS */
    int            quantum;         /* RR quantum, simulated minutes */
    int            preemptive;      /* prio: 1 on, 0 off, -1 default */
    int64_t        age;             /* MLFQ aging threshold, simulated minutes */
    unsigned long  time_scale_ms;   /* real ms per simulated minute (0 is treated as 1) */
    relief_shm_t  *shm;             /* status table to publish into, may be NULL */
} dispatch_config_t;

/* Create the policy and start the team and clock threads. NULL on error (logged). */
dispatcher_t *dispatch_start(const dispatch_config_t *cfg);

/* Hand one RESCUE request to the scheduler (called from reliefd's receive loop). */
int dispatch_submit(dispatcher_t *d, const relief_request_t *req);

/* Stop and join all threads, free unfinished jobs (logged as abandoned), free d. */
void dispatch_stop(dispatcher_t *d);

#endif /* RELIEFD_DISPATCH_H */
