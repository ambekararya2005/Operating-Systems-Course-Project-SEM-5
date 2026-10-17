/*
 * OS concept: Memory allocation — first/best/worst fit, the buddy system, slab caches — Unit 4
 *
 * Unit tests for src/mem, through the same mem_ops_t interface memsim uses.
 *   1. First, best and worst fit on the textbook example (Silberschatz): free holes of 100,
 *      500, 200, 300 and 600 units (in that address order), requests of 212, 417, 112 and 426.
 *      Expected (worked by hand below):
 *        first fit  212 -> 500-hole, 417 -> 600-hole, 112 -> rest of the 500-hole (288),
 *                   426 fails (largest hole left: 300)
 *        best fit   212 -> 300-hole, 417 -> 500-hole, 112 -> 200-hole, 426 -> 600-hole
 *        worst fit  212 -> 600-hole, 417 -> 500-hole, 112 -> rest of the 600-hole (388),
 *                   426 fails (largest hole left: 300)
 *   2. Buddy system on 1024 units: a request of 70 gets a 128-unit block at 0 (1024 -> 512 ->
 *      256 -> 128: free buddies of 128, 256 and 512 remain); 58 units are wasted inside it;
 *      freeing it merges everything back into one 1024 block. Two neighbours that are buddies
 *      merge only when both are free.
 *   3. Slab caches: FOOD/MED/HYGIENE objects come from their own cache (4/8/16 objects per
 *      slab), full/partial/empty lists follow the use; freed objects return to their cache
 *      (an emptied slab stays cached and is reused); a shipment is all or nothing; when the
 *      floor has no free frame an empty slab of another cache is reclaimed.
 * Exit status 0 if every check passed.
 */
#include "../src/mem/mem.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* Units-only allocation (fit and buddy ignore kind and count). */
static int place(const mem_ops_t *ops, mem_alloc_t *m, int id, int units)
{
    return ops->alloc(m, id, 2, units, units);
}

/* ---- 1. first / best / worst fit ------------------------------------------------------------ */
/* Lay out holes of 100, 500, 200, 300, 600 separated by 10-unit walls, all in one floor of 1740:
 * hole addresses 0, 110, 620, 830, 1140. */
static mem_alloc_t *textbook_floor(const mem_ops_t *ops)
{
    static const int parts[] = { 100, 10, 500, 10, 200, 10, 300, 10, 600 };
    mem_alloc_t *m = ops->create(1740);
    for (int i = 0; i < 9; i++)
        CHECK(place(ops, m, 100 + i, parts[i]) == 0, "%s: setup part %d", ops->name, i);
    for (int i = 0; i < 9; i += 2)
        ops->release(m, 100 + i);        /* free the 5 holes, keep the 4 walls */
    return m;
}

static void test_fits(void)
{
    static const int req[4] = { 212, 417, 112, 426 };
    /* expected start address per request, -1 = must wait (fails) */
    static const struct { const char *name; int addr[4]; } want[] = {
        /* first: 212 -> 500-hole @110 (288 left @322); 417 -> 600-hole @1140; 112 -> @322; 426: no hole */
        { "first", { 110, 1140, 322, -1 } },
        /* best: 212 -> 300-hole @830; 417 -> 500-hole @110; 112 -> 200-hole @620; 426 -> 600-hole @1140 */
        { "best",  { 830, 110, 620, 1140 } },
        /* worst: 212 -> 600-hole @1140 (388 left @1352); 417 -> 500-hole @110; 112 -> @1352; 426: no hole */
        { "worst", { 1140, 110, 1352, -1 } },
    };
    for (int s = 0; s < 3; s++) {
        const mem_ops_t *ops = mem_find(want[s].name);
        CHECK(ops != NULL, "allocator %s", want[s].name);
        mem_alloc_t *m = textbook_floor(ops);
        mem_stats_t st;
        ops->stats(m, &st);
        CHECK(st.holes == 5 && st.free_total == 1700 && st.largest_free == 600,
              "%s: 5 holes, 1700 free, largest 600 (got %d, %d, %d)", ops->name, st.holes, st.free_total, st.largest_free);
        for (int r = 0; r < 4; r++) {
            int rc = place(ops, m, r, req[r]), a = ops->addr_of(m, r);
            if (want[s].addr[r] < 0)
                CHECK(rc == -1 && a == -1, "%s: request %d must fail, got address %d", ops->name, req[r], a);
            else
                CHECK(rc == 0 && a == want[s].addr[r], "%s: request %d should go to %d, got %d", ops->name, req[r],
                      want[s].addr[r], a);
        }
        ops->stats(m, &st);
        if (want[s].addr[3] < 0)
            CHECK(st.free_total >= 426 && st.largest_free < 426,
                  "%s: 426 fails although %d units are free (largest %d): external fragmentation", ops->name,
                  st.free_total, st.largest_free);
        ops->destroy(m);
        printf("ok   %-5s fit: 212 -> %d, 417 -> %d, 112 -> %d, 426 -> %s\n", want[s].name, want[s].addr[0],
               want[s].addr[1], want[s].addr[2], want[s].addr[3] < 0 ? "fails (must wait)" : "1140");
    }
}

/* ---- 2. buddy system ---------------------------------------------------------------------- */
static void test_buddy(void)
{
    const mem_ops_t *ops = mem_find("buddy");
    mem_alloc_t *m = ops->create(1024);
    mem_stats_t st;

    CHECK(ops->create(1000) == NULL, "buddy needs a power-of-two floor");
    CHECK(place(ops, m, 1, 70) == 0 && ops->addr_of(m, 1) == 0, "70 units -> a block at 0");
    ops->stats(m, &st);
    CHECK(st.occupied == 128 && st.requested == 70, "70 units take a 128-unit block (occupied %d)", st.occupied);
    CHECK(st.holes == 3 && st.largest_free == 512 && st.free_total == 896,
          "split 1024 -> 512 -> 256 -> 128 leaves free buddies 128, 256, 512 (got %d holes, largest %d, free %d)",
          st.holes, st.largest_free, st.free_total);
    CHECK(st.int_frag > 0.45 && st.int_frag < 0.46, "internal fragmentation 58/128 = 45.3%% (got %.3f)", st.int_frag);
    ops->release(m, 1);
    ops->stats(m, &st);
    CHECK(st.holes == 1 && st.largest_free == 1024 && st.occupied == 0, "freeing merges back into one 1024 block");

    /* Buddies merge only when both are free. */
    CHECK(place(ops, m, 2, 100) == 0 && place(ops, m, 3, 128) == 0, "two 128-blocks");
    CHECK(ops->addr_of(m, 2) == 0 && ops->addr_of(m, 3) == 128, "they are buddies (0 and 0 ^ 128)");
    ops->release(m, 2);
    ops->stats(m, &st);
    CHECK(st.largest_free == 512 && st.holes == 3, "one buddy free: no merge yet (largest %d)", st.largest_free);
    ops->release(m, 3);
    ops->stats(m, &st);
    CHECK(st.largest_free == 1024 && st.holes == 1, "both free: merged up to 1024");
    CHECK(place(ops, m, 4, 1025) == -1, "more than the floor fails");
    ops->destroy(m);
    printf("ok   buddy: 70 -> 128-block at 0 (58 wasted), free -> merged back to 1024; buddies merge only when both free\n");
}

/* ---- 3. slab caches --------------------------------------------------------------------------- */
enum { FOOD, MED, HYG };

static void test_slab(void)
{
    const mem_ops_t *ops = mem_find("slab");
    mem_alloc_t *m = ops->create(1024);
    mem_stats_t st;

    CHECK(ops->alloc(m, 1, FOOD, 5, 20) == 0, "5 FOOD kits");
    CHECK(ops->alloc(m, 2, MED, 3, 6) == 0, "3 MED kits");
    CHECK(ops->alloc(m, 3, HYG, 16, 16) == 0, "16 HYGIENE kits");
    ops->stats(m, &st);
    CHECK(st.objs_per_slab[FOOD] == 4 && st.objs_per_slab[MED] == 8 && st.objs_per_slab[HYG] == 16,
          "objects per slab 4/8/16");
    CHECK(st.slabs[FOOD][0] == 1 && st.slabs[FOOD][1] == 1, "FOOD: 5 kits = 1 full + 1 partial slab");
    CHECK(st.slabs[MED][1] == 1 && st.slabs[HYG][0] == 1, "MED: 1 partial; HYGIENE: 1 full");
    CHECK(mem_slab_cache_of(m, 1) == FOOD && mem_slab_cache_of(m, 2) == MED && mem_slab_cache_of(m, 3) == HYG,
          "each shipment's objects come from its own cache");
    CHECK(st.occupied == 4 * 16 && st.requested == 20 + 6 + 16, "4 slabs, 42 units of kits");

    ops->release(m, 1);         /* the FOOD kits go back to the FOOD cache */
    ops->stats(m, &st);
    CHECK(st.objs_used[FOOD] == 0 && st.slabs[FOOD][2] == 2 && st.slabs[MED][1] == 1 && st.slabs[HYG][0] == 1,
          "freed FOOD objects return to the FOOD cache: 2 empty FOOD slabs, MED and HYGIENE untouched");
    CHECK(st.occupied == 64, "emptied slabs stay cached (still 4 frames)");
    CHECK(ops->alloc(m, 4, FOOD, 4, 16) == 0, "4 FOOD kits again");
    ops->stats(m, &st);
    CHECK(st.occupied == 64 && st.slabs[FOOD][0] == 1 && st.slabs[FOOD][2] == 1, "reused an empty FOOD slab, no new frame");
    ops->destroy(m);

    /* A 2-frame floor: all-or-nothing shipments, reclaiming an empty slab of another cache. */
    m = ops->create(32);
    CHECK(ops->alloc(m, 1, FOOD, 12, 48) == -1, "12 FOOD kits need 3 slabs: fails");
    ops->stats(m, &st);
    CHECK(st.objs_used[FOOD] == 0 && st.slabs[FOOD][0] == 0 && st.slabs[FOOD][1] == 0,
          "a failed shipment leaves no object allocated (its slabs stay cached as empty, reclaimable)");
    CHECK(ops->alloc(m, 2, FOOD, 4, 16) == 0 && ops->alloc(m, 3, HYG, 16, 16) == 0, "both frames in use");
    CHECK(ops->alloc(m, 4, MED, 1, 2) == -1, "no frame left for MED");
    ops->release(m, 2);
    CHECK(ops->alloc(m, 5, MED, 8, 16) == 0, "MED reclaims the empty FOOD slab's frame");
    ops->stats(m, &st);
    CHECK(st.slabs[FOOD][2] == 0 && st.slabs[MED][0] == 1, "FOOD's empty slab was reclaimed for MED");
    ops->destroy(m);
    printf("ok   slab: objects from their own cache (4/8/16 per slab), full/partial/empty lists, freed objects return "
           "to their cache, all-or-nothing, empty slabs reclaimed\n");
}

int main(void)
{
    test_fits();
    test_buddy();
    test_slab();
    CHECK(mem_find("bogus") == NULL, "unknown allocator");
    if (g_failed) {
        fprintf(stderr, "test_mem: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    printf("ok   test_mem: all %d checks passed\n", g_checks);
    return 0;
}
