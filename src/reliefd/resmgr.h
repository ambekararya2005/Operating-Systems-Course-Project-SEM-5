/*
 * OS concept: Deadlock avoidance in a live system — the Banker's algorithm deciding every
 *             vehicle/kit request; waiting requests on a condition variable — Unit 3
 *
 * reliefd's resource manager for the Banker's resources (TRUCK, BOAT, MED_KIT, FOOD_KIT).
 *   - HELLO: the camp declares its maximum claim; the camp becomes an active process.
 *   - REQUEST: queued; the allocator thread asks the deadlock policy (--deadlock avoid: the
 *     Banker's resource-request algorithm). Granted -> event=grant with the safe sequence,
 *     MSG_GRANT on the camp's reply queue (and kits go on to the loading dock). Unsafe or not
 *     available -> the request stays queued (event=deny_unsafe / wait_unavailable, each logged
 *     at most once per request) and the allocator sleeps on a condition variable until
 *     something is released or a new request arrives, then retries every waiting request in
 *     arrival order.
 *   - RELEASE: the camp hands units back after its hold time (event=release).
 *   - BYE or process exit: everything the camp holds is released (process termination).
 * All state is guarded by one mutex; the main thread only enqueues and releases.
 */
#ifndef RELIEFD_RESMGR_H
#define RELIEFD_RESMGR_H

#include "relief.h"

#include <stdint.h>

typedef struct resmgr resmgr_t;

typedef struct {
    const char    *mode;            /* --deadlock: a deadlock_policy_find() name */
    int            total[RELIEF_BANKER_RES];
    unsigned long  time_scale_ms;   /* real ms per simulated minute (0 is treated as 1) */
    /* Called on the allocator thread after a request is granted and the camp told (kits go
     * to the loading dock from here). May be NULL. */
    void         (*on_grant)(void *ctx, const relief_request_t *req);
    void          *grant_ctx;
} resmgr_config_t;

/* NULL on error (logged): unknown mode, a mode that is not implemented yet, no memory. */
resmgr_t *resmgr_start(const resmgr_config_t *cfg);

/* HELLO: (re)declare the camp's maximum claim, open its reply queue if it has one. */
void resmgr_hello(resmgr_t *rm, uint32_t camp, const relief_hello_t *hello);

/* A REQUEST with req->units > 0 for a Banker's resource. */
void resmgr_submit(resmgr_t *rm, const relief_request_t *req);

/* A RELEASE of req->units of req->resource by camp req->camp_id. */
void resmgr_release(resmgr_t *rm, const relief_request_t *req);

/* The camp said BYE or its process ended: release all it holds, drop its waiting requests.
 * unlink_queue: also remove its reply queue (the process is gone). */
void resmgr_camp_gone(resmgr_t *rm, uint32_t camp, const char *why, int unlink_queue);

/* Copy the Banker's state (Allocation, Max, Available, counters) into the status table. */
void resmgr_publish(resmgr_t *rm, relief_shm_t *shm);

/* Stop the allocator thread; waiting requests are abandoned (logged). Frees rm. */
void resmgr_stop(resmgr_t *rm);

#endif /* RELIEFD_RESMGR_H */
