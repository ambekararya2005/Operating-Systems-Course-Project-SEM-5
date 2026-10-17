/*
 * OS concept: Deadlock handling strategies — avoidance, prevention, detection, or none — Unit 3
 *
 * reliefd --deadlock MODE picks one policy through this function-pointer interface, so the
 * resource manager (src/reliefd/resmgr.c) never depends on a particular strategy. Which of
 * the four Coffman conditions (mutual exclusion, hold and wait, no preemption, circular wait)
 * each mode deals with:
 *   none         nothing: grant whatever is free, block otherwise. Deadlock can happen.
 *   avoid        none of them is broken; the Banker's algorithm never enters an unsafe
 *                state, so the four conditions never combine into a deadlock.
 *   prevent      circular wait: resources must be acquired in the order TRUCK < BOAT <
 *                MED_KIT < FOOD_KIT; an out-of-order request is rejected (the camp releases
 *                and retries in order), so no cycle can form.
 *   prevent-all  hold and wait (--deadlock prevent --all-or-nothing): a task asks for all
 *                its resources in one request and gets all or nothing, so it never waits
 *                while holding.
 *   detect       no preemption, after the fact: grant what is free, let a detector thread
 *                find cycles in the wait-for graph, and recover by preempting a vehicle.
 * Mutual exclusion is never broken: a truck or a boat cannot be shared.
 */
#ifndef RELIEF_DEADLOCK_H
#define RELIEF_DEADLOCK_H

#include "banker.h"

typedef struct {
    const char *name;
    const char *what;
    const char *breaks;             /* the Coffman condition it deals with, for the log */
    /* Decide one request vector of process p (see banker_request for the result codes). held is
     * what the requesting job (a task of the camp) already holds, NULL = all p holds. On
     * BANKER_GRANT the allocation is made and seq/len hold a safe sequence (avoid only). */
    banker_result_t (*request)(banker_t *b, int p, const int req[], const int held[], int seq[], int *len);
    int detector;                   /* run the wait-for-graph detector thread */
    int all_or_nothing;             /* camps send each task as one bundle request */
} deadlock_policy_t;

/* The policy called `name` (avoid, none, prevent, prevent-all, detect), or NULL. */
const deadlock_policy_t *deadlock_policy_find(const char *name);

/* "avoid, none, prevent, detect" for usage messages. */
const char *deadlock_policy_names(void);

/* --deadlock none / detect: grant if request <= Need and request <= Available, else wait. */
banker_result_t deadlock_grant_if_free(banker_t *b, int p, const int req[], const int held[], int seq[],
                                       int *len);

/* --deadlock prevent: as above, but first reject a request for resource j while the job holds
 * any resource ordered after j (BANKER_ORDER). The order is enforced per job: a camp's tasks
 * are the units that hold and wait (a lone row never waits while holding). */
banker_result_t deadlock_ordered(banker_t *b, int p, const int req[], const int held[], int seq[],
                                 int *len);

#endif /* RELIEF_DEADLOCK_H */
