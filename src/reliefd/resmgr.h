/*
 * OS concept: Deadlock handling in a live system — avoidance (Banker's), prevention, detection
 *             and recovery; waiting requests on a condition variable — Unit 3
 *
 * reliefd's resource manager for the Banker's resources (TRUCK, BOAT, MED_KIT, FOOD_KIT).
 *   - HELLO: the camp declares its maximum claim; the camp becomes an active process.
 *   - REQUEST (one resource, or a whole task's vector with REQ_FLAG_BUNDLE): queued; the
 *     allocator thread asks the --deadlock policy (src/deadlock/deadlock.c). Granted ->
 *     event=grant (with the safe sequence under avoid) and MSG_GRANT on the camp's reply queue
 *     (kits go on to the loading dock). Out of resource order (prevent) -> event=reject_order and
 *     MSG_REJECT with REQ_FLAG_ORDER. Unsafe or not available -> the request stays queued
 *     (event=deny_unsafe / wait_unavailable, each logged at most once per request) and the
 *     allocator sleeps on a condition variable until something is released or a new request
 *     arrives, then retries every waiting request in arrival order.
 *   - RELEASE: the camp hands a grant back by request id (event=release).
 *   - BYE or process exit: everything the camp holds is released (process termination).
 *   - --deadlock detect: a detector thread wakes every --detect-interval seconds, builds the
 *     wait-for graph from the Allocation and Request tables, looks for a cycle with DFS and
 *     confirms it with the detection algorithm (event=deadlock_detected cycle=camp1->camp2->camp1).
 *     Recovery: the camp in the cycle whose waiting request has the lowest severity loses a
 *     vehicle another cycle member waits for; that grant is rolled back and its request queued
 *     again (event=recovery, MSG_REVOKE to the camp).
 *   - Write-ahead journal (src/io/journal.c, Unit 5): every CLAIM, GRANT, RELEASE (also a
 *     revoke) and RELEASE_ALL is appended to $RELIEF_HOME/journal.log and fsync'ed before it is
 *     applied and acknowledged (the camp is told; the log line carries seq=). For a grant the
 *     Banker's check allocates tentatively; it is rolled back if the record cannot be made
 *     durable. On startup the snapshot and journal are replayed (event=recovered). Off with
 *     reliefd --no-journal.
 * All state is guarded by one mutex; the main thread only enqueues and releases.
 */
#ifndef RELIEFD_RESMGR_H
#define RELIEFD_RESMGR_H

#include "relief.h"

#include <stdint.h>

typedef struct resmgr resmgr_t;

typedef struct {
    const char    *mode;            /* a deadlock_policy_find() name: avoid none prevent prevent-all detect */
    int            total[RELIEF_BANKER_RES];
    unsigned long  time_scale_ms;   /* real ms per simulated minute (0 is treated as 1) */
    long           detect_interval_ms;  /* detect: detector period in real ms */
    /* Called on the allocator thread after a request is granted and the camp told (kits go
     * to the loading dock from here). May be NULL. */
    void         (*on_grant)(void *ctx, const relief_request_t *req);
    void          *grant_ctx;
    const char    *journal_dir;     /* journal.log + snapshot.bin go here; NULL = no journal */
} resmgr_config_t;

/* NULL on error (logged): unknown mode, no memory, a journal that cannot be recovered. */
resmgr_t *resmgr_start(const resmgr_config_t *cfg);

/* HELLO: (re)declare the camp's maximum claim, open its reply queue if it has one. */
void resmgr_hello(resmgr_t *rm, uint32_t camp, const relief_hello_t *hello);

/* A REQUEST for a Banker's resource: msg->u.req (units > 0), or a bundle (REQ_FLAG_BUNDLE). */
void resmgr_submit(resmgr_t *rm, const relief_msg_t *msg);

/* A RELEASE: the camp hands back the grant of request req->id. */
void resmgr_release(resmgr_t *rm, const relief_request_t *req);

/* The camp said BYE or its process ended: release all it holds, drop its waiting requests.
 * unlink_queue: also remove its reply queue (the process is gone). */
void resmgr_camp_gone(resmgr_t *rm, uint32_t camp, const char *why, int unlink_queue);

/* Copy Allocation, Max, Request, Available and the counters into the status table. */
void resmgr_publish(resmgr_t *rm, relief_shm_t *shm);

/* Stop the threads; waiting requests are abandoned (logged). Frees rm. */
void resmgr_stop(resmgr_t *rm);

#endif /* RELIEFD_RESMGR_H */
