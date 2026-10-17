/*
 * OS concept: The producer-consumer (bounded-buffer) problem with three semaphores
 *             (empty, full, mutex) — Unit 3
 *
 * The loading dock between the warehouses and the trucks. Kit requests (FOOD_KIT, MED_KIT,
 * HYGIENE_KIT) from the camps become orders; P warehouse producer threads take an order,
 * deduct the kits from the inventory (a write, see inventory.h), pack them (1 + qty/50
 * simulated minutes) and put the package on the dock, a circular buffer of --dock slots.
 * C truck consumer threads take packages off the dock and deliver them: the trip takes
 * dock_distance_min(camp) simulated minutes.
 *
 *   orders (unbounded FIFO,       dock (bounded buffer,           trucks
 *   mutex + condvar)  -> producers -> empty/full/mutex sems)  ->  (deliver, read inventory)
 *
 * Every put/take is logged *inside* the dock's critical section (event=produce / consume with
 * occupancy=), so the log's occupancy sequence is exactly the buffer's history.
 */
#ifndef RELIEFD_DOCK_H
#define RELIEFD_DOCK_H

#include "inventory.h"
#include "relief.h"

#include <semaphore.h>
#include <stdint.h>

/* One package of kits for one request. id 0 is a poison pill: the truck that takes it stops. */
typedef struct {
    uint32_t id;
    uint32_t camp;
    uint32_t qty;
    uint8_t  resource;          /* relief_resource_t: a kit */
    uint8_t  severity;
    uint16_t producer;          /* 1-based producer thread that packed it */
    int64_t  packed_ns;         /* CLOCK_MONOTONIC when it was put on the dock */
} dock_pkg_t;

/* ---- The bounded buffer (usable on its own: tests/test_sync.c) --------------------------- */
typedef struct {
    dock_pkg_t *slot;
    int      capacity;
    int      in, out, count;    /* guarded by mutex */
    sem_t    empty;             /* free slots, starts at capacity */
    sem_t    full;              /* filled slots, starts at 0 */
    sem_t    mutex;             /* binary semaphore around the buffer and the statistics */
    /* statistics, guarded by mutex */
    uint64_t puts, takes, blocked_puts;
    int      max_count;
    int64_t  blocked_ns;        /* total time producers waited on `empty` (dock full) */
    int64_t  idle_ns;           /* total time consumers waited on `full` (dock empty) */
    int64_t  area_ns;           /* integral of count over time, for the time-weighted average */
    int64_t  t0_ns, last_ns;
} dockbuf_t;

/* Called inside the critical section after a put or take, with the occupancy after it and
 * how long this thread waited on empty (put) or full (take) before getting in. */
typedef void (*dock_note_fn)(void *ctx, const dock_pkg_t *p, int occupancy, int64_t waited_ns);

int  dockbuf_init(dockbuf_t *b, int capacity);
void dockbuf_destroy(dockbuf_t *b);
void dockbuf_put(dockbuf_t *b, const dock_pkg_t *p, dock_note_fn note, void *ctx);
void dockbuf_take(dockbuf_t *b, dock_pkg_t *p, dock_note_fn note, void *ctx);

typedef struct {
    uint64_t puts, takes, blocked_puts;
    int      count, max_count, capacity;
    int64_t  blocked_ns, idle_ns;
    double   avg_count;         /* time-weighted since init */
} dockbuf_stats_t;

void dockbuf_stats(dockbuf_t *b, dockbuf_stats_t *out);

/* ---- The dock inside reliefd ------------------------------------------------------------ */
#define DOCK_MAX_CAPACITY   64
#define DOCK_MAX_PRODUCERS  8
#define DOCK_MAX_TRUCKS     16

typedef struct dock dock_t;

typedef struct {
    int            capacity;        /* slots, 1..DOCK_MAX_CAPACITY */
    int            producers;       /* warehouse threads, 1..DOCK_MAX_PRODUCERS */
    int            trucks;          /* truck threads, 1..DOCK_MAX_TRUCKS */
    unsigned long  time_scale_ms;   /* real ms per simulated minute (0 is treated as 1) */
    inventory_t   *inv;
} dock_config_t;

/* Delivery trip of a truck to camp `camp`, in simulated minutes (fixed per camp for now). */
int dock_distance_min(uint32_t camp);

/* 1 if this resource is a kit that goes through the dock. */
int dock_handles(unsigned resource);

/* Start the producer and truck threads. NULL on error (logged). */
dock_t *dock_start(const dock_config_t *cfg);

/* Queue one kit request as an order for the producers. 0, or -1 on allocation failure. */
int dock_submit(dock_t *d, const relief_request_t *req);

/* Copy the dock's counters into the shared-memory status table. */
void dock_publish(dock_t *d, relief_shm_t *shm);

/* Stop: pending orders are abandoned, deliveries in progress are cut short (logged), the
 * producers are joined, one poison pill per truck goes on the dock, the trucks are joined.
 * Logs event=dock_stop with the totals, then frees d. */
void dock_stop(dock_t *d);

#endif /* RELIEFD_DOCK_H */
