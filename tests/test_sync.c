/*
 * OS concept: Producer-consumer (bounded buffer with empty/full/mutex semaphores) and
 *             readers-writers (semaphore readers-preference, writer-preference rwlock) — Unit 3
 *
 * Unit tests for the Phase 3 synchronisation code:
 *   1. dock bounded buffer: P producers and C consumers move N packages through a dock of
 *      capacity 8 (and 1). Occupancy never exceeds the capacity (checked inside the critical
 *      section against a shadow counter), and every produced package is consumed exactly once.
 *   2. a full dock blocks the producer until a consumer takes a package (and the blocked time
 *      is counted)
 *   3. rwsync, both kinds: a writer is alone in the critical section, readers never see a
 *      half-written table, readers do overlap each other
 *   4. inventory: restock when short, warehouse and kit mapping
 *   test_sync [--quick]    --quick: 20x fewer packages (for valgrind, which serialises threads)
 * Exit status 0 if every check passed.
 */
#include "../src/reliefd/dock.h"
#include "../src/reliefd/inventory.h"
#include "rwsync.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int g_checks, g_failed;

#define CHECK(cond, ...) do {                                         \
        g_checks++;                                                   \
        if (!(cond)) {                                                \
            g_failed++;                                               \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fprintf(stderr, "\n");                                    \
        }                                                             \
    } while (0)

static void sleep_ms(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/* ---- 1. bounded buffer: capacity and exactly-once ------------------------------------------ */
#define MAX_ITEMS 200000

typedef struct {
    dockbuf_t  *buf;
    int         capacity;
    int         shadow;             /* occupancy replayed from the notes: only touched inside the CS */
    atomic_int  over_capacity, mismatch;
    atomic_int *seen;               /* seen[id]: times consumed */
} bb_t;

typedef struct {
    bb_t *bb;
    int   first, count;             /* producer: ids first .. first+count-1 */
} bb_arg_t;

static void note_put(void *ctx, const dock_pkg_t *p, int occ, int64_t waited)
{
    bb_t *bb = ctx;
    (void)p; (void)waited;
    if (++bb->shadow != occ)
        atomic_fetch_add(&bb->mismatch, 1);
    if (occ > bb->capacity || occ < 1)
        atomic_fetch_add(&bb->over_capacity, 1);
}

static void note_take(void *ctx, const dock_pkg_t *p, int occ, int64_t waited)
{
    bb_t *bb = ctx;
    (void)p; (void)waited;
    if (--bb->shadow != occ)
        atomic_fetch_add(&bb->mismatch, 1);
    if (occ < 0 || occ >= bb->capacity)
        atomic_fetch_add(&bb->over_capacity, 1);
}

static void *bb_producer(void *p)
{
    bb_arg_t *a = p;
    for (int i = 0; i < a->count; i++) {
        dock_pkg_t pkg = { 0 };
        pkg.id = (uint32_t)(a->first + i);
        pkg.camp = pkg.id % 8 + 1;
        dockbuf_put(a->bb->buf, &pkg, note_put, a->bb);
    }
    return NULL;
}

static void *bb_consumer(void *p)
{
    bb_t *bb = p;
    for (;;) {
        dock_pkg_t pkg;
        dockbuf_take(bb->buf, &pkg, note_take, bb);
        if (pkg.id == 0)
            return NULL;                            /* poison pill */
        if (pkg.id < MAX_ITEMS)
            atomic_fetch_add(&bb->seen[pkg.id], 1);
    }
}

static void test_bounded_buffer(int capacity, int producers, int consumers, int per_producer)
{
    dockbuf_t buf;
    bb_t bb;
    pthread_t pt[16], ct[16];
    bb_arg_t args[16];
    int total = producers * per_producer;

    memset(&bb, 0, sizeof bb);
    bb.buf = &buf;
    bb.capacity = capacity;
    bb.seen = calloc(MAX_ITEMS, sizeof *bb.seen);
    CHECK(bb.seen && dockbuf_init(&buf, capacity) == 0, "dockbuf_init(%d)", capacity);
    if (!bb.seen)
        return;
    for (int i = 0; i < consumers; i++)
        pthread_create(&ct[i], NULL, bb_consumer, &bb);
    for (int i = 0; i < producers; i++) {
        args[i] = (bb_arg_t){ &bb, 1 + i * per_producer, per_producer };
        pthread_create(&pt[i], NULL, bb_producer, &args[i]);
    }
    for (int i = 0; i < producers; i++)
        pthread_join(pt[i], NULL);
    for (int i = 0; i < consumers; i++) {
        dock_pkg_t pill = { 0 };
        dockbuf_put(&buf, &pill, note_put, &bb);
    }
    for (int i = 0; i < consumers; i++)
        pthread_join(ct[i], NULL);

    dockbuf_stats_t s;
    dockbuf_stats(&buf, &s);
    int once = 0, missing = 0, dup = 0;
    for (int id = 1; id <= total; id++) {
        int n = atomic_load(&bb.seen[id]);
        once += n == 1;
        missing += n == 0;
        dup += n > 1;
    }
    CHECK(once == total && missing == 0 && dup == 0,
          "cap %d: %d of %d consumed exactly once (%d missing, %d more than once)", capacity, once,
          total, missing, dup);
    CHECK(atomic_load(&bb.over_capacity) == 0 && s.max_count <= capacity,
          "cap %d: occupancy out of range %d times, max %d", capacity, atomic_load(&bb.over_capacity),
          s.max_count);
    CHECK(atomic_load(&bb.mismatch) == 0, "cap %d: logged occupancy disagrees with the put/take history "
          "%d times", capacity, atomic_load(&bb.mismatch));
    CHECK(s.puts == (uint64_t)(total + consumers) && s.takes == s.puts && s.count == 0,
          "cap %d: puts %llu takes %llu count %d", capacity, (unsigned long long)s.puts,
          (unsigned long long)s.takes, s.count);
    printf("ok   dock cap %d: %d producers x %d, %d consumers: each package consumed once, "
           "max occupancy %d, %llu puts blocked on a full dock\n", capacity, producers, per_producer,
           consumers, s.max_count, (unsigned long long)s.blocked_puts);
    dockbuf_destroy(&buf);
    free(bb.seen);
}

/* ---- 2. a full dock blocks the producer ------------------------------------------------------ */
static atomic_int g_third_put_done;

static void *put_three(void *p)
{
    dockbuf_t *b = p;
    for (uint32_t id = 1; id <= 3; id++) {
        dock_pkg_t pkg = { 0 };
        pkg.id = id;
        dockbuf_put(b, &pkg, NULL, NULL);
    }
    atomic_store(&g_third_put_done, 1);
    return NULL;
}

static void test_full_dock_blocks(void)
{
    dockbuf_t b;
    pthread_t t;
    dock_pkg_t pkg;
    dockbuf_stats_t s;

    CHECK(dockbuf_init(&b, 2) == 0, "dockbuf_init(2)");
    pthread_create(&t, NULL, put_three, &b);
    sleep_ms(100);
    dockbuf_stats(&b, &s);
    CHECK(s.puts == 2 && s.count == 2 && !atomic_load(&g_third_put_done),
          "with capacity 2 and no consumer, the third put must block (puts %llu)",
          (unsigned long long)s.puts);
    dockbuf_take(&b, &pkg, NULL, NULL);
    CHECK(pkg.id == 1, "FIFO: first package out should be 1, got %u", pkg.id);
    pthread_join(t, NULL);
    dockbuf_stats(&b, &s);
    CHECK(s.blocked_puts == 1 && s.blocked_ns >= 50000000,
          "blocked puts %llu (want 1), blocked %.1f ms (want >= 50)", (unsigned long long)s.blocked_puts,
          s.blocked_ns / 1e6);
    dockbuf_take(&b, &pkg, NULL, NULL);
    CHECK(pkg.id == 2, "FIFO: second package out should be 2, got %u", pkg.id);
    dockbuf_take(&b, &pkg, NULL, NULL);
    CHECK(pkg.id == 3, "FIFO: third package out should be 3, got %u", pkg.id);
    dockbuf_destroy(&b);
    printf("ok   full dock: the producer blocks until a truck takes a package (blocked %.0f ms counted)\n",
           s.blocked_ns / 1e6);
}

/* ---- 3. readers-writers ---------------------------------------------------------------------- */
#define RW_ITERS 3000

static rwsync_t g_rw;
static long g_table[8];
static atomic_int g_readers_in, g_writers_in, g_max_readers, g_violations, g_torn;

static void *rw_reader(void *p)
{
    (void)p;
    for (int i = 0; i < RW_ITERS; i++) {
        rwsync_read_lock(&g_rw);
        int in = atomic_fetch_add(&g_readers_in, 1) + 1, m = atomic_load(&g_max_readers);
        while (in > m && !atomic_compare_exchange_weak(&g_max_readers, &m, in))
            ;
        if (atomic_load(&g_writers_in) != 0)
            atomic_fetch_add(&g_violations, 1);
        for (int k = 1; k < 8; k++)
            if (g_table[k] != g_table[0])
                atomic_fetch_add(&g_torn, 1);
        for (volatile int spin = 0; spin < 200; spin++)
            ;
        atomic_fetch_sub(&g_readers_in, 1);
        rwsync_read_unlock(&g_rw);
    }
    return NULL;
}

static void *rw_writer(void *p)
{
    (void)p;
    for (int i = 0; i < RW_ITERS / 10; i++) {
        rwsync_write_lock(&g_rw);
        if (atomic_fetch_add(&g_writers_in, 1) != 0 || atomic_load(&g_readers_in) != 0)
            atomic_fetch_add(&g_violations, 1);
        long v = g_table[0] + 1;
        for (int k = 0; k < 8; k++)
            g_table[k] = v;
        atomic_fetch_sub(&g_writers_in, 1);
        rwsync_write_unlock(&g_rw);
    }
    return NULL;
}

static void test_rwsync(const char *kind)
{
    pthread_t t[8];

    memset(g_table, 0, sizeof g_table);
    atomic_store(&g_max_readers, 0);
    atomic_store(&g_violations, 0);
    atomic_store(&g_torn, 0);
    CHECK(rwsync_init(&g_rw, kind) == 0, "rwsync_init(%s)", kind);
    CHECK(strcmp(g_rw.ops->name, kind) == 0, "ops name %s", g_rw.ops->name);
    for (int i = 0; i < 8; i++)
        pthread_create(&t[i], NULL, i < 6 ? rw_reader : rw_writer, NULL);
    for (int i = 0; i < 8; i++)
        pthread_join(t[i], NULL);
    CHECK(atomic_load(&g_violations) == 0, "%s: %d times a writer was not alone", kind,
          atomic_load(&g_violations));
    CHECK(atomic_load(&g_torn) == 0, "%s: %d torn reads", kind, atomic_load(&g_torn));
    CHECK(g_table[0] == 2 * (RW_ITERS / 10), "%s: %ld writes recorded, want %d", kind, g_table[0],
          2 * (RW_ITERS / 10));
    rwsync_destroy(&g_rw);
    printf("ok   rwsync %s: writers exclusive, no torn read, up to %d readers inside together\n", kind,
           atomic_load(&g_max_readers));
}

/* ---- 4. inventory ----------------------------------------------------------------------------- */
static void test_inventory(void)
{
    rwsync_t bad;
    int64_t restocked, snap[RELIEF_WAREHOUSES][RELIEF_KIT_KINDS];

    errno = 0;
    CHECK(rwsync_init(&bad, "bogus") == -1 && errno == EINVAL, "unknown rwsync kind must fail with EINVAL");
    CHECK(inventory_warehouse_of(1) == 0 && inventory_warehouse_of(2) == 1 && inventory_warehouse_of(3) == 0,
          "warehouse mapping");
    CHECK(inventory_kit_index(RES_FOOD_KIT) == 0 && inventory_kit_index(RES_MED_KIT) == 1 &&
          inventory_kit_index(RES_HYGIENE_KIT) == 2 && inventory_kit_index(RES_RESCUE) == -1 &&
          inventory_kit_index(RES_TRUCK) == -1, "kit index mapping");
    CHECK(dock_handles(RES_MED_KIT) && !dock_handles(RES_BOAT), "dock_handles");
    for (int k = 0; k < 2; k++) {
        const char *kind = k ? "rwlock" : "semaphore";
        inventory_t *inv = inventory_new(kind);
        CHECK(inv != NULL, "inventory_new(%s)", kind);
        if (!inv)
            continue;
        CHECK(inventory_take(inv, 0, 0, 100, &restocked) == INV_INITIAL_STOCK - 100 && restocked == 0,
              "%s: take 100", kind);
        CHECK(inventory_take(inv, 0, 0, INV_INITIAL_STOCK, &restocked) ==
                  INV_INITIAL_STOCK - 100 + INV_RESTOCK_AMOUNT - INV_INITIAL_STOCK &&
              restocked == INV_RESTOCK_AMOUNT, "%s: a take larger than the stock restocks first", kind);
        inventory_snapshot(inv, snap);
        CHECK(snap[0][0] == inventory_stock(inv, 0, 0) && snap[1][2] == INV_INITIAL_STOCK,
              "%s: snapshot", kind);
        CHECK(strcmp(inventory_rw_kind(inv), kind) == 0, "%s: rw kind", kind);
        inventory_free(inv);
    }
    printf("ok   inventory: restock when short, warehouse/kit mapping, both lock kinds\n");
}

int main(int argc, char **argv)
{
    int div = argc > 1 && strcmp(argv[1], "--quick") == 0 ? 20 : 1;

    test_bounded_buffer(8, 4, 3, 25000 / div);
    test_bounded_buffer(1, 3, 2, 5000 / div);
    test_bounded_buffer(8, 1, 1, 20000 / div);
    test_full_dock_blocks();
    test_rwsync("semaphore");
    test_rwsync("rwlock");
    test_inventory();
    if (g_failed) {
        fprintf(stderr, "test_sync: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    printf("ok   test_sync: all %d checks passed\n", g_checks);
    return 0;
}
