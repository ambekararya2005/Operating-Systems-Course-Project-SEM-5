/*
 * OS concept: The producer-consumer (bounded-buffer) problem with three semaphores
 *             (empty, full, mutex) — Unit 3
 */
#include "dock.h"

#include "log.h"
#include "shm.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void P(sem_t *s)         /* wait() */
{
    while (sem_wait(s) == -1 && errno == EINTR)
        ;
}

static void V(sem_t *s)         /* signal() */
{
    sem_post(s);
}

/* ---- The bounded buffer -------------------------------------------------------------------
 *
 * Textbook pseudocode (Silberschatz, the bounded-buffer problem):
 *     int n;  semaphore mutex = 1;  semaphore empty = n;  semaphore full = 0;
 *
 *     producer:                              consumer:
 *       while (true) {                         while (true) {
 *           ... produce next_produced ...          wait(full);
 *           wait(empty);                           wait(mutex);
 *           wait(mutex);                               next_consumed = buffer[out];
 *               buffer[in] = next_produced;            out = (out + 1) % n;
 *               in = (in + 1) % n;                 signal(mutex);
 *           signal(mutex);                         signal(empty);
 *           signal(full);                          ... consume next_consumed ...
 *       }                                      }
 *
 * empty counts free slots and full counts packages, so a producer sleeps while the dock is
 * full and a truck sleeps while it is empty; mutex makes the buffer update atomic. The order
 * wait(empty) then wait(mutex) matters: the other way round, a producer could sleep on a full
 * dock while holding mutex, and no truck could ever get in to empty it (deadlock).
 */
int dockbuf_init(dockbuf_t *b, int capacity)
{
    memset(b, 0, sizeof *b);
    b->slot = calloc((size_t)capacity, sizeof *b->slot);
    if (!b->slot)
        return -1;
    b->capacity = capacity;
    if (sem_init(&b->empty, 0, (unsigned)capacity) == -1 || sem_init(&b->full, 0, 0) == -1 ||
        sem_init(&b->mutex, 0, 1) == -1) {
        free(b->slot);
        return -1;
    }
    b->t0_ns = b->last_ns = mono_ns();
    return 0;
}

void dockbuf_destroy(dockbuf_t *b)
{
    sem_destroy(&b->mutex);
    sem_destroy(&b->full);
    sem_destroy(&b->empty);
    free(b->slot);
    b->slot = NULL;
}

/* Caller holds mutex: account for the time spent at the old occupancy. */
static void occupancy_changes(dockbuf_t *b, int64_t now)
{
    b->area_ns += (int64_t)b->count * (now - b->last_ns);
    b->last_ns = now;
}

void dockbuf_put(dockbuf_t *b, const dock_pkg_t *p, dock_note_fn note, void *ctx)
{
    int64_t waited = 0;
    int blocked = 0;

    if (sem_trywait(&b->empty) == -1) {         /* dock full: we are about to block */
        int64_t t = mono_ns();
        blocked = 1;
        P(&b->empty);                           /* wait(empty) */
        waited = mono_ns() - t;
    }
    P(&b->mutex);                               /* wait(mutex) */
    occupancy_changes(b, mono_ns());
    b->slot[b->in] = *p;                        /* buffer[in] = next_produced */
    b->in = (b->in + 1) % b->capacity;          /* in = (in + 1) % n */
    b->count++;
    b->puts++;
    if (blocked) {
        b->blocked_puts++;
        b->blocked_ns += waited;
    }
    if (b->count > b->max_count)
        b->max_count = b->count;
    if (note)
        note(ctx, &b->slot[(b->in + b->capacity - 1) % b->capacity], b->count, waited);
    V(&b->mutex);                               /* signal(mutex) */
    V(&b->full);                                /* signal(full) */
}

void dockbuf_take(dockbuf_t *b, dock_pkg_t *p, dock_note_fn note, void *ctx)
{
    int64_t waited = 0;

    if (sem_trywait(&b->full) == -1) {          /* dock empty: the truck waits */
        int64_t t = mono_ns();
        P(&b->full);                            /* wait(full) */
        waited = mono_ns() - t;
    }
    P(&b->mutex);                               /* wait(mutex) */
    occupancy_changes(b, mono_ns());
    *p = b->slot[b->out];                       /* next_consumed = buffer[out] */
    b->out = (b->out + 1) % b->capacity;        /* out = (out + 1) % n */
    b->count--;
    b->takes++;
    b->idle_ns += waited;
    if (note)
        note(ctx, p, b->count, waited);
    V(&b->mutex);                               /* signal(mutex) */
    V(&b->empty);                               /* signal(empty) */
}

void dockbuf_stats(dockbuf_t *b, dockbuf_stats_t *out)
{
    P(&b->mutex);
    int64_t now = mono_ns();
    occupancy_changes(b, now);
    out->puts = b->puts;
    out->takes = b->takes;
    out->blocked_puts = b->blocked_puts;
    out->count = b->count;
    out->max_count = b->max_count;
    out->capacity = b->capacity;
    out->blocked_ns = b->blocked_ns;
    out->idle_ns = b->idle_ns;
    out->avg_count = now > b->t0_ns ? (double)b->area_ns / (double)(now - b->t0_ns) : 0.0;
    V(&b->mutex);
}

/* ---- The dock inside reliefd --------------------------------------------------------------- */

/* One-way truck trip from the warehouse to each camp of scenarios/camps.conf, in simulated
 * minutes; other camp ids get a value derived from the id. */
static const int g_distance[] = { 0, 12, 10, 4, 7, 5, 14, 11, 16 };

int dock_distance_min(uint32_t camp)
{
    if (camp >= 1 && camp < sizeof g_distance / sizeof g_distance[0])
        return g_distance[camp];
    return 5 + (int)(camp * 7 % 12);
}

int dock_handles(unsigned resource)
{
    return inventory_kit_index(resource) >= 0;
}

typedef struct order {
    relief_request_t req;
    struct order    *next;
} order_t;

typedef struct {
    struct dock *d;
    int          index;         /* 1-based */
} worker_arg_t;

struct dock {
    dockbuf_t      buf;
    inventory_t   *inv;
    int            producers, trucks;
    int64_t        scale_ns;            /* real ns per simulated minute */

    pthread_mutex_t olock;              /* guards the order queue, stop and the order counters */
    pthread_cond_t  ocond;
    order_t        *head, *tail;
    size_t          pending;
    uint64_t        orders, abandoned_orders;
    int             stop;

    atomic_int      halt;               /* set at stop: cut packing and trips short */
    atomic_uint_fast64_t delivered, undelivered, dropped_packs;

    pthread_t      *threads;            /* producers first, then trucks */
    worker_arg_t   *args;
    int             nstarted_prod, nstarted_trucks;
};

static double to_min(const dock_t *d, int64_t ns)
{
    return (double)ns / (double)d->scale_ns;
}

/* Sleep `minutes` simulated minutes in slices of at most 50 ms. Returns 1 if halted early. */
static int sim_sleep(dock_t *d, int64_t minutes)
{
    int64_t left = minutes * d->scale_ns;
    while (left > 0) {
        if (atomic_load(&d->halt))
            return 1;
        int64_t step = left < 50000000 ? left : 50000000;
        struct timespec ts = { (time_t)(step / 1000000000), (long)(step % 1000000000) };
        while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
            ;
        left -= step;
    }
    return atomic_load(&d->halt) != 0;
}

static void note_produce(void *ctx, const dock_pkg_t *p, int occupancy, int64_t waited_ns)
{
    dock_t *d = ctx;
    EV_INFO("produce", "producer=%u id=%u camp=%u kit=%s qty=%u occupancy=%d capacity=%d blocked_min=%.2f",
            p->producer, p->id, p->camp, relief_resource_name(p->resource), p->qty, occupancy,
            d->buf.capacity, to_min(d, waited_ns));
}

typedef struct {
    dock_t *d;
    int     truck;
} take_ctx_t;

static void note_consume(void *ctx, const dock_pkg_t *p, int occupancy, int64_t waited_ns)
{
    take_ctx_t *c = ctx;
    if (p->id == 0)
        return;                 /* poison pill: not a package */
    EV_INFO("consume", "truck=%d id=%u camp=%u kit=%s qty=%u occupancy=%d capacity=%d idle_min=%.2f",
            c->truck, p->id, p->camp, relief_resource_name(p->resource), p->qty, occupancy,
            c->d->buf.capacity, to_min(c->d, waited_ns));
}

static void *producer_thread(void *p)
{
    worker_arg_t *a = p;
    dock_t *d = a->d;

    for (;;) {
        pthread_mutex_lock(&d->olock);
        while (!d->head && !d->stop)
            pthread_cond_wait(&d->ocond, &d->olock);
        if (d->stop) {
            pthread_mutex_unlock(&d->olock);
            break;
        }
        order_t *o = d->head;
        d->head = o->next;
        if (!d->head)
            d->tail = NULL;
        d->pending--;
        pthread_mutex_unlock(&d->olock);

        const relief_request_t *r = &o->req;
        int w = inventory_warehouse_of(r->camp_id), k = inventory_kit_index(r->resource);
        int64_t restocked;
        int64_t left = inventory_take(d->inv, w, k, r->quantity, &restocked);   /* writer */
        if (restocked)
            EV_INFO("restock", "warehouse=%d kit=%s added=%lld stock=%lld", w + 1,
                    relief_resource_name(r->resource), (long long)restocked, (long long)left);

        if (sim_sleep(d, 1 + r->quantity / 50)) {          /* packing */
            atomic_fetch_add(&d->dropped_packs, 1);
            EV_INFO("pack_abandoned", "producer=%d id=%u camp=%u", a->index, r->id, r->camp_id);
            free(o);
            break;
        }
        dock_pkg_t pkg = { r->id, r->camp_id, r->quantity, (uint8_t)r->resource, r->severity,
                           (uint16_t)a->index, mono_ns() };
        free(o);
        dockbuf_put(&d->buf, &pkg, note_produce, d);
    }
    return NULL;
}

static void *truck_thread(void *p)
{
    worker_arg_t *a = p;
    dock_t *d = a->d;
    take_ctx_t ctx = { d, a->index };

    for (;;) {
        dock_pkg_t pkg;
        dockbuf_take(&d->buf, &pkg, note_consume, &ctx);
        if (pkg.id == 0)
            break;                              /* poison pill */
        int dist = dock_distance_min(pkg.camp);
        if (sim_sleep(d, dist)) {
            atomic_fetch_add(&d->undelivered, 1);
            EV_INFO("deliver_abandoned", "truck=%d id=%u camp=%u kit=%s", a->index, pkg.id, pkg.camp,
                    relief_resource_name(pkg.resource));
            continue;
        }
        int w = inventory_warehouse_of(pkg.camp), k = inventory_kit_index(pkg.resource);
        int64_t stock = inventory_stock(d->inv, w, k);                          /* reader */
        atomic_fetch_add(&d->delivered, 1);
        EV_INFO("deliver", "truck=%d id=%u camp=%u kit=%s qty=%u distance=%d age_min=%.1f "
                "warehouse=%d stock_left=%lld",
                a->index, pkg.id, pkg.camp, relief_resource_name(pkg.resource), pkg.qty, dist,
                to_min(d, mono_ns() - pkg.packed_ns), w + 1, (long long)stock);
    }
    return NULL;
}

dock_t *dock_start(const dock_config_t *cfg)
{
    dock_t *d;
    int err;

    if (cfg->capacity < 1 || cfg->capacity > DOCK_MAX_CAPACITY || cfg->producers < 1 ||
        cfg->producers > DOCK_MAX_PRODUCERS || cfg->trucks < 1 || cfg->trucks > DOCK_MAX_TRUCKS ||
        !cfg->inv) {
        LOG_ERROR("dock: bad configuration");
        return NULL;
    }
    d = calloc(1, sizeof *d);
    if (!d)
        return NULL;
    if (dockbuf_init(&d->buf, cfg->capacity) == -1) {
        LOG_ERROR("dock: %s", strerror(errno));
        free(d);
        return NULL;
    }
    d->inv = cfg->inv;
    d->producers = cfg->producers;
    d->trucks = cfg->trucks;
    d->scale_ns = (int64_t)(cfg->time_scale_ms ? cfg->time_scale_ms : 1) * 1000000;
    pthread_mutex_init(&d->olock, NULL);
    pthread_cond_init(&d->ocond, NULL);
    atomic_init(&d->halt, 0);
    atomic_init(&d->delivered, 0);
    atomic_init(&d->undelivered, 0);
    atomic_init(&d->dropped_packs, 0);
    d->threads = calloc((size_t)(d->producers + d->trucks), sizeof *d->threads);
    d->args = calloc((size_t)(d->producers + d->trucks), sizeof *d->args);
    if (!d->threads || !d->args) {
        dock_stop(d);
        return NULL;
    }
    EV_INFO("dock_start", "capacity=%d producers=%d trucks=%d rw=%s", d->buf.capacity, d->producers,
            d->trucks, inventory_rw_kind(d->inv));
    /* Like the rescue teams, the dock threads block every signal (the mask is inherited), so
     * SIGCHLD/SIGTERM always reach the main thread and interrupt its mq_timedreceive. */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    err = 0;
    for (int i = 0; i < d->producers && !err; i++) {
        d->args[i] = (worker_arg_t){ d, i + 1 };
        if ((err = pthread_create(&d->threads[i], NULL, producer_thread, &d->args[i])) == 0)
            d->nstarted_prod++;
    }
    for (int i = 0; i < d->trucks && !err; i++) {
        int s = d->producers + i;
        d->args[s] = (worker_arg_t){ d, i + 1 };
        if ((err = pthread_create(&d->threads[s], NULL, truck_thread, &d->args[s])) == 0)
            d->nstarted_trucks++;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (err) {
        LOG_ERROR("dock: pthread_create: %s", strerror(err));
        dock_stop(d);
        return NULL;
    }
    return d;
}

int dock_submit(dock_t *d, const relief_request_t *req)
{
    order_t *o = malloc(sizeof *o);
    if (!o)
        return -1;
    o->req = *req;
    o->next = NULL;
    pthread_mutex_lock(&d->olock);
    if (d->tail)
        d->tail->next = o;
    else
        d->head = o;
    d->tail = o;
    d->pending++;
    d->orders++;
    pthread_cond_signal(&d->ocond);
    pthread_mutex_unlock(&d->olock);
    return 0;
}

void dock_publish(dock_t *d, relief_shm_t *shm)
{
    dockbuf_stats_t s;
    uint64_t orders;
    int rc;

    if (!shm)
        return;
    dockbuf_stats(&d->buf, &s);
    pthread_mutex_lock(&d->olock);
    orders = d->orders;
    pthread_mutex_unlock(&d->olock);
    rc = relief_shm_begin_write(shm);       /* dock locks are released: no nesting */
    if (rc == -1)
        return;
    if (rc == 1)
        EV_WARN("status_lock_recovered", "by=dock reason=EOWNERDEAD");
    relief_status_t *st = &shm->data;
    st->dock_capacity = (uint32_t)s.capacity;
    st->dock_occupancy = (uint32_t)s.count;
    st->dock_max_occupancy = (uint32_t)s.max_count;
    st->dock_producers = (uint32_t)d->producers;
    st->dock_trucks = (uint32_t)d->trucks;
    st->dock_orders = orders;
    st->dock_produced = s.puts;
    st->dock_consumed = s.takes;
    st->dock_delivered = atomic_load(&d->delivered);
    st->dock_blocked_puts = s.blocked_puts;
    st->dock_blocked_min = to_min(d, s.blocked_ns);
    relief_shm_end_write(shm);
}

void dock_stop(dock_t *d)
{
    dockbuf_stats_t s;
    uint64_t orders, abandoned = 0;

    if (!d)
        return;
    pthread_mutex_lock(&d->olock);
    d->stop = 1;
    while (d->head) {               /* orders no producer has started: abandoned */
        order_t *o = d->head;
        d->head = o->next;
        free(o);
        abandoned++;
    }
    d->tail = NULL;
    d->pending = 0;
    d->abandoned_orders = abandoned;
    orders = d->orders;
    pthread_cond_broadcast(&d->ocond);
    pthread_mutex_unlock(&d->olock);
    atomic_store(&d->halt, 1);

    /* Producers finish the package in hand (a producer blocked on a full dock gets in as the
     * trucks keep taking), then exit. */
    for (int i = 0; i < d->nstarted_prod; i++)
        pthread_join(d->threads[i], NULL);

    /* One poison pill per truck, through the same bounded buffer. */
    for (int i = 0; i < d->nstarted_trucks; i++) {
        dock_pkg_t pill = { 0 };
        dockbuf_put(&d->buf, &pill, NULL, NULL);
    }
    for (int i = 0; i < d->nstarted_trucks; i++)
        pthread_join(d->threads[d->producers + i], NULL);
    dockbuf_stats(&d->buf, &s);
    s.puts -= (uint64_t)d->nstarted_trucks;     /* the pills are not packages */
    s.takes -= (uint64_t)d->nstarted_trucks;

    if (d->threads)
        EV_INFO("dock_stop", "orders=%llu produced=%llu consumed=%llu delivered=%llu undelivered=%llu "
                "abandoned_orders=%llu dropped_packs=%llu avg_occupancy=%.2f max_occupancy=%d "
                "capacity=%d blocked_puts=%llu blocked_min=%.1f",
                (unsigned long long)orders, (unsigned long long)s.puts, (unsigned long long)s.takes,
                (unsigned long long)atomic_load(&d->delivered),
                (unsigned long long)atomic_load(&d->undelivered), (unsigned long long)abandoned,
                (unsigned long long)atomic_load(&d->dropped_packs), s.avg_count, s.max_count,
                s.capacity, (unsigned long long)s.blocked_puts, to_min(d, s.blocked_ns));
    pthread_cond_destroy(&d->ocond);
    pthread_mutex_destroy(&d->olock);
    dockbuf_destroy(&d->buf);
    free(d->threads);
    free(d->args);
    free(d);
}
