/*
 * OS concept: Demand paging and page replacement — FIFO, LRU, Optimal, expiry-first — Unit 4
 *
 * Unit tests for src/mem/paging.c. Expected answers (Silberschatz):
 *   1. Reference string 7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1 with 3 frames:
 *        FIFO 15 faults, LRU 12 faults, Optimal 9 faults
 *   2. Belady's anomaly: 1,2,3,4,1,2,5,1,2,3,4,5 under FIFO gives 9 faults with 3 frames and
 *      10 with 4 (more frames, more faults); LRU and Optimal (stack algorithms) never do that:
 *      LRU 10 -> 8, Optimal 7 -> 6
 *   3. Expiry-first: with frames full of A (expires at 3), B (expires at 50) and a new item C,
 *      expiry evicts A (closest to expiry) while FIFO and LRU evict by age / use. Stock that
 *      expires on the shelf is counted: needing an expired item is a fault and one expired unit.
 * Exit status 0 if every check passed.
 */
#include "../src/mem/paging.h"

#include <stdio.h>
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

static int faults(const char *policy, const int *items, int n, int frames)
{
    page_ref_t refs[64];
    page_stats_t st;
    for (int i = 0; i < n; i++)
        refs[i] = (page_ref_t){ items[i], -1 };
    if (page_run(page_policy_find(policy), refs, n, frames, &st, NULL, NULL) != 0)
        return -1;
    CHECK(st.faults + st.hits == n && st.expired == 0, "%s: faults + hits = references, nothing expires", policy);
    return st.faults;
}

static int last_evicted;

static void record_eviction(void *ctx, int t, const page_ref_t *r, const page_frame_t *fr, int nframes, int fault,
                            int evicted)
{
    (void)ctx; (void)t; (void)r; (void)fr; (void)nframes; (void)fault;
    if (evicted >= 0)
        last_evicted = evicted;
}

int main(void)
{
    /* 1. The classic reference string. */
    static const int classic[] = { 7, 0, 1, 2, 0, 3, 0, 4, 2, 3, 0, 3, 2, 1, 2, 0, 1, 7, 0, 1 };
    int f = faults("fifo", classic, 20, 3), l = faults("lru", classic, 20, 3), o = faults("optimal", classic, 20, 3);
    CHECK(f == 15, "FIFO, 3 frames: expected 15 faults, got %d", f);
    CHECK(l == 12, "LRU, 3 frames: expected 12 faults, got %d", l);
    CHECK(o == 9, "Optimal, 3 frames: expected 9 faults, got %d", o);
    printf("ok   7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1 with 3 frames: FIFO %d, LRU %d, Optimal %d faults\n", f, l, o);

    /* 2. Belady's anomaly. */
    static const int belady[] = { 1, 2, 3, 4, 1, 2, 5, 1, 2, 3, 4, 5 };
    int f3 = faults("fifo", belady, 12, 3), f4 = faults("fifo", belady, 12, 4);
    CHECK(f3 == 9 && f4 == 10, "FIFO: expected 9 faults with 3 frames and 10 with 4, got %d and %d", f3, f4);
    int l3 = faults("lru", belady, 12, 3), l4 = faults("lru", belady, 12, 4);
    int o3 = faults("optimal", belady, 12, 3), o4 = faults("optimal", belady, 12, 4);
    CHECK(l3 == 10 && l4 == 8 && o3 == 7 && o4 == 6, "stack algorithms: LRU %d -> %d, Optimal %d -> %d", l3, l4, o3, o4);
    printf("ok   Belady's anomaly 1,2,3,4,1,2,5,1,2,3,4,5: FIFO %d faults with 3 frames, %d with 4 "
           "(LRU %d -> %d, Optimal %d -> %d)\n", f3, f4, l3, l4, o3, o4);

    /* 3. Expiry-first. Items: A=1 expires at 3, B=2 expires at 50; at t=2, C=3 arrives. */
    page_ref_t refs[] = { { 1, 3 }, { 2, 50 }, { 3, 60 }, { 2, 50 } };
    page_stats_t st;
    last_evicted = -1;
    page_run(page_policy_find("expiry"), refs, 3, 2, &st, record_eviction, NULL);
    CHECK(last_evicted == 1, "expiry evicts A (expires first), got %d", last_evicted);
    page_ref_t refs2[] = { { 2, 50 }, { 1, 3 }, { 3, 60 } };
    last_evicted = -1;
    page_run(page_policy_find("expiry"), refs2, 3, 2, &st, record_eviction, NULL);
    CHECK(last_evicted == 1, "expiry evicts A although B came first, got %d", last_evicted);
    last_evicted = -1;
    page_run(page_policy_find("fifo"), refs2, 3, 2, &st, record_eviction, NULL);
    CHECK(last_evicted == 2, "FIFO evicts B (oldest), got %d", last_evicted);
    /* A expires at 3 and is needed at 5: expired on the shelf, a fault, restocked. */
    page_ref_t refs3[] = { { 1, 3 }, { 2, -1 }, { 2, -1 }, { 2, -1 }, { 2, -1 }, { 1, 20 }, { 1, 20 } };
    page_run(page_policy_find("lru"), refs3, 7, 2, &st, NULL, NULL);
    CHECK(st.faults == 3 && st.expired == 1 && st.hits == 4,
          "needing expired stock: 3 faults, 1 expired unit, 4 hits (got %d, %d, %d)", st.faults, st.expired, st.hits);
    page_ref_t refs4[] = { { 1, 0 }, { 2, -1 } };
    page_run(page_policy_find("fifo"), refs4, 2, 2, &st, NULL, NULL);
    CHECK(st.expired == 1, "stock past its date at the end counts as expired (got %d)", st.expired);
    printf("ok   expiry-first evicts the stock closest to its expiry; expired stock is counted (needed, evicted, or left)\n");

    CHECK(page_policy_find("bogus") == NULL && page_run(page_policy_find("lru"), refs, 1, 0, &st, NULL, NULL) == -1,
          "bad policy / 0 frames");
    if (g_failed) {
        fprintf(stderr, "test_page: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    printf("ok   test_page: all %d checks passed\n", g_checks);
    return 0;
}
