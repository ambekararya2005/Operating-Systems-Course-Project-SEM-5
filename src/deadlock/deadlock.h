/*
 * OS concept: Deadlock handling strategies — avoidance, prevention, detection, or none — Unit 3
 *
 * reliefd --deadlock MODE picks one policy through this function-pointer interface, so the
 * resource manager (src/reliefd/resmgr.c) never depends on a particular strategy:
 *   avoid    the Banker's algorithm: grant a request only if the state stays safe (step 3)
 *   none     grant whenever the resources are available (deadlock possible)    (step 4)
 *   prevent  resource ordering: break the circular-wait condition               (step 4)
 *   detect   grant when available, find cycles in the wait-for graph, recover   (step 4)
 * A policy whose `request` is NULL is not implemented yet.
 */
#ifndef RELIEF_DEADLOCK_H
#define RELIEF_DEADLOCK_H

#include "banker.h"

typedef struct {
    const char *name;
    const char *what;
    /* Decide one request of process p (see banker_request for the result codes). On
     * BANKER_GRANT the allocation is made and seq/len hold the safe sequence (if any). */
    banker_result_t (*request)(banker_t *b, int p, const int req[], int seq[], int *len);
} deadlock_policy_t;

/* The policy called `name`, or NULL if there is none. */
const deadlock_policy_t *deadlock_policy_find(const char *name);

/* "avoid, none, prevent, detect" for usage messages. */
const char *deadlock_policy_names(void);

#endif /* RELIEF_DEADLOCK_H */
