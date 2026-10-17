/*
 * OS concept: Deadlock avoidance — the Banker's algorithm — Unit 3
 *
 * Unit tests for src/deadlock/banker.c on the standard textbook example (Silberschatz,
 * "Operating System Concepts", Banker's algorithm example): 5 processes P0..P4 and three
 * resource types with A = 10, B = 5, C = 7 instances.
 *
 *            Allocation   Max       Need      Available
 *            A  B  C      A  B  C   A  B  C   A  B  C
 *       P0   0  1  0      7  5  3   7  4  3   3  3  2
 *       P1   2  0  0      3  2  2   1  2  2
 *       P2   3  0  2      9  0  2   6  0  0
 *       P3   2  1  1      2  2  2   0  1  1
 *       P4   0  0  2      4  3  3   4  3  1
 *
 * Expected answers (worked by hand below each test):
 *   1. the state is safe, with the safe sequence <P1, P3, P4, P0, P2>
 *   2. P1 requests (1,0,2): granted; the new state is safe, again <P1, P3, P4, P0, P2>
 *   3. then P4 requests (3,3,0): denied, the resources are not available (Available = 2 3 0)
 *   4. then P0 requests (0,2,0): denied as unsafe, although 0 2 0 <= Available
 *   5. on the original state, P4 (3,3,0) is available but unsafe: denied
 *   6. a request above Need is an error (over claim); release and remove return resources
 *
 * Deadlock detection, prevention and the wait-for graph (Phase 3 step 4):
 *   7. detection algorithm, textbook example (A = 7, B = 2, C = 6, Available 0 0 0):
 *            Allocation   Request
 *       P0   0 1 0        0 0 0
 *       P1   2 0 0        2 0 2
 *       P2   3 0 3        0 0 0
 *       P3   2 1 1        1 0 0
 *       P4   0 0 2        0 0 2
 *      -> no deadlock (<P0, P2, P3, P1, P4> can finish). If P2 then asks for one more C
 *      (Request P2 = 0 0 1), P1, P2, P3 and P4 are deadlocked; only P0 can finish.
 *   8. wait-for graph + DFS: P0 holds R0 and waits for R1, P1 holds R1 and waits for R0, P2
 *      waits for R0 -> edges P0->P1, P1->P0, P2->P0; the cycle is P0 -> P1 -> P0. Without
 *      P1's request the graph is acyclic.
 *   9. resource ordering (prevent): holding a BOAT, a TRUCK request is rejected (out of order);
 *      holding a TRUCK, a BOAT request is granted. Grant-if-free (none/detect) blocks when
 *      the resources are not available.
 * Exit status 0 if every check passed.
 */
#include "../src/deadlock/banker.h"
#include "../src/deadlock/deadlock.h"
#include "../src/deadlock/waitfor.h"

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

static const int TOTAL[3] = { 10, 5, 7 };
static const int MAX[5][3] = { { 7, 5, 3 }, { 3, 2, 2 }, { 9, 0, 2 }, { 2, 2, 2 }, { 4, 3, 3 } };
static const int ALLOC[5][3] = { { 0, 1, 0 }, { 2, 0, 0 }, { 3, 0, 2 }, { 2, 1, 1 }, { 0, 0, 2 } };

static void textbook(banker_t *b)
{
    banker_init(b, 5, 3, TOTAL);
    for (int p = 0; p < 5; p++) {
        CHECK(banker_set_max(b, p, MAX[p]) == 0, "set_max P%d", p);
        CHECK(banker_set_alloc(b, p, ALLOC[p]) == 0, "set_alloc P%d", p);
    }
}

static const char *fmt_seq(const int *seq, int len, char *buf, size_t size)
{
    size_t o = 0;
    o += (size_t)snprintf(buf + o, size - o, "<");
    for (int i = 0; i < len && o < size; i++)
        o += (size_t)snprintf(buf + o, size - o, "%sP%d", i ? ", " : "", seq[i]);
    if (o < size)
        snprintf(buf + o, size - o, ">");
    return buf;
}

static int vec_eq(const int *a, int x, int y, int z)
{
    return a[0] == x && a[1] == y && a[2] == z;
}

int main(void)
{
    banker_t b;
    int seq[BANKER_MAX_P], len;
    char buf[128];
    const int expected[5] = { 1, 3, 4, 0, 2 };

    /* 1. Available = (10,5,7) - column sums (7,2,5) = (3,3,2). Safety, passes over P0..P4:
     *    Work 3 3 2: P0 needs 7 4 3 no; P1 1 2 2 yes -> Work 5 3 2; P2 6 0 0 no;
     *    P3 0 1 1 yes -> 7 4 3; P4 4 3 1 yes -> 7 4 5. Next pass: P0 7 4 3 yes -> 7 5 5;
     *    P2 6 0 0 yes -> 10 5 7. Safe: <P1, P3, P4, P0, P2>. */
    textbook(&b);
    CHECK(vec_eq(b.available, 3, 3, 2), "Available should be 3 3 2, got %d %d %d", b.available[0],
          b.available[1], b.available[2]);
    CHECK(banker_need(&b, 0, 0) == 7 && banker_need(&b, 0, 1) == 4 && banker_need(&b, 0, 2) == 3 &&
          banker_need(&b, 4, 0) == 4 && banker_need(&b, 4, 1) == 3 && banker_need(&b, 4, 2) == 1,
          "Need P0 should be 7 4 3 and P4 4 3 1");
    CHECK(banker_is_safe(&b, seq, &len) == 1 && len == 5 && memcmp(seq, expected, sizeof expected) == 0,
          "initial state: expected safe <P1, P3, P4, P0, P2>, got %s", fmt_seq(seq, len, buf, sizeof buf));
    printf("ok   textbook state is safe: %s\n", fmt_seq(seq, len, buf, sizeof buf));

    /* 2. P1 requests (1,0,2): 1 0 2 <= Need 1 2 2 and <= Available 3 3 2. Pretend:
     *    Available 2 3 0, Allocation P1 3 0 2, Need P1 0 2 0. Safety: Work 2 3 0: P0 no;
     *    P1 0 2 0 yes -> 5 3 2; P2 no; P3 yes -> 7 4 3; P4 yes -> 7 4 5; P0 yes -> 7 5 5;
     *    P2 yes -> 10 5 7. Safe <P1, P3, P4, P0, P2>: granted. */
    const int p1_req[3] = { 1, 0, 2 };
    banker_result_t r = banker_request(&b, 1, p1_req, seq, &len);
    CHECK(r == BANKER_GRANT, "P1 (1,0,2) should be granted, got %s", banker_result_name(r));
    CHECK(len == 5 && memcmp(seq, expected, sizeof expected) == 0,
          "after P1's grant: expected <P1, P3, P4, P0, P2>, got %s", fmt_seq(seq, len, buf, sizeof buf));
    CHECK(vec_eq(b.available, 2, 3, 0) && vec_eq(b.alloc[1], 3, 0, 2), "after P1's grant: Available 2 3 0, "
          "Allocation P1 3 0 2");
    printf("ok   P1 requests (1,0,2): granted, safe sequence %s, Available 2 3 0\n",
           fmt_seq(seq, len, buf, sizeof buf));

    /* 3. P4 requests (3,3,0): 3 3 0 <= Need 4 3 1, but A: 3 > Available 2. Denied: not available. */
    const int p4_req[3] = { 3, 3, 0 };
    r = banker_request(&b, 4, p4_req, seq, &len);
    CHECK(r == BANKER_UNAVAILABLE, "P4 (3,3,0) should be denied (unavailable), got %s", banker_result_name(r));
    CHECK(vec_eq(b.available, 2, 3, 0) && vec_eq(b.alloc[4], 0, 0, 2), "a denied request must change nothing");
    printf("ok   P4 requests (3,3,0): denied, resources not available (Available 2 3 0)\n");

    /* 4. P0 requests (0,2,0): <= Need 7 4 3 and <= Available 2 3 0. Pretend: Available 2 1 0,
     *    Need P0 7 2 3. Safety: Work 2 1 0: P0 7 2 3 no, P1 0 2 0 no (B 2 > 1), P2 6 0 0 no,
     *    P3 0 1 1 no (C 1 > 0), P4 4 3 1 no. Nobody can finish: unsafe, rolled back. */
    const int p0_req[3] = { 0, 2, 0 };
    r = banker_request(&b, 0, p0_req, seq, &len);
    CHECK(r == BANKER_UNSAFE, "P0 (0,2,0) should be denied as unsafe, got %s", banker_result_name(r));
    CHECK(vec_eq(b.available, 2, 3, 0) && vec_eq(b.alloc[0], 0, 1, 0), "the unsafe request must be rolled back");
    printf("ok   P0 requests (0,2,0): denied as unsafe and rolled back\n");

    /* 5. Original state, P4 requests (3,3,0): available (3 3 2) but leaves Work 0 0 2 with
     *    Need P4 1 0 1: P3 0 1 1 no (B), P1 1 2 2 no, ... unsafe. */
    textbook(&b);
    r = banker_request(&b, 4, p4_req, seq, &len);
    CHECK(r == BANKER_UNSAFE, "original state: P4 (3,3,0) should be unsafe, got %s", banker_result_name(r));
    printf("ok   original state, P4 requests (3,3,0): available but unsafe, denied\n");

    /* 6. Errors, release, remove. P1 Need is 1 2 2: asking for 2 0 0 exceeds the claim. */
    const int over[3] = { 2, 0, 0 };
    CHECK(banker_request(&b, 1, over, seq, &len) == BANKER_OVER_CLAIM, "P1 (2,0,0) exceeds Need 1 2 2");
    const int rel[3] = { 1, 0, 0 }, too_much[3] = { 3, 0, 0 };
    CHECK(banker_release(&b, 1, too_much) == -1, "P1 cannot release 3 A (holds 2)");
    CHECK(banker_release(&b, 1, rel) == 0 && vec_eq(b.available, 4, 3, 2) && vec_eq(b.alloc[1], 1, 0, 0),
          "P1 releases 1 A: Available 4 3 2");
    banker_remove(&b, 2);                   /* P2 terminates: its 3 0 2 return */
    CHECK(vec_eq(b.available, 7, 3, 4) && !b.active[2], "P2 removed: Available 7 3 4, P2 inactive");
    CHECK(banker_is_safe(&b, seq, &len) == 1 && len == 4, "4 active processes, still safe");
    CHECK(banker_request(&b, 2, rel, seq, &len) == BANKER_INACTIVE, "an inactive process cannot request");
    const int bad_max[3] = { 11, 0, 0 };
    CHECK(banker_set_max(&b, 2, bad_max) == -1, "a claim above the total is refused");
    CHECK(banker_set_max(&b, 1, MAX[1]) == -1, "a claim cannot be re-declared while holding resources");
    printf("ok   over-claim is an error; release and termination return resources\n");

    /* 7. Detection algorithm (several instances per resource type). */
    {
        static const int total[3] = { 7, 2, 6 };
        static const int alloc[5][3] = { { 0, 1, 0 }, { 2, 0, 0 }, { 3, 0, 3 }, { 2, 1, 1 }, { 0, 0, 2 } };
        static int request[BANKER_MAX_P][BANKER_MAX_R] = {
            { 0, 0, 0 }, { 2, 0, 2 }, { 0, 0, 0 }, { 1, 0, 0 }, { 0, 0, 2 } };
        unsigned char dead[BANKER_MAX_P];
        banker_init(&b, 5, 3, total);
        for (int p = 0; p < 5; p++) {
            CHECK(banker_set_max(&b, p, total) == 0 && banker_set_alloc(&b, p, alloc[p]) == 0, "setup P%d", p);
        }
        CHECK(vec_eq(b.available, 0, 0, 0), "detection example: Available 0 0 0");
        CHECK(banker_detect(&b, (const int (*)[BANKER_MAX_R])request, dead) == 0,
              "detection example: no deadlock expected");
        request[2][2] = 1;      /* P2 asks for one more C */
        int nd = banker_detect(&b, (const int (*)[BANKER_MAX_R])request, dead);
        CHECK(nd == 4 && !dead[0] && dead[1] && dead[2] && dead[3] && dead[4],
              "after P2's extra request: P1..P4 deadlocked, P0 not (got %d)", nd);
        printf("ok   detection algorithm: textbook example has no deadlock; after P2 requests 0 0 1, "
               "P1, P2, P3, P4 are deadlocked\n");
    }

    /* 8. Wait-for graph and DFS. */
    {
        static const int tot2[2] = { 1, 1 }, max2[2] = { 1, 1 };
        static const int a0[2] = { 1, 0 }, a1[2] = { 0, 1 };
        static int request[BANKER_MAX_P][BANKER_MAX_R] = { { 0, 1 }, { 1, 0 }, { 1, 0 } };
        static waitfor_t g;
        int cyc[BANKER_MAX_P], clen;
        banker_init(&b, 3, 2, tot2);
        for (int p = 0; p < 3; p++)
            banker_set_max(&b, p, max2);
        banker_set_alloc(&b, 0, a0);
        banker_set_alloc(&b, 1, a1);
        waitfor_build(&g, &b, (const int (*)[BANKER_MAX_R])request);
        CHECK(g.label[0][1] == 2u && g.label[1][0] == 1u && g.label[2][0] == 1u && waitfor_edges(&g) == 3,
              "edges P0->P1 [R1], P1->P0 [R0], P2->P0 [R0] expected");
        CHECK(waitfor_find_cycle(&g, cyc, &clen) == 1 && clen == 2 &&
              ((cyc[0] == 0 && cyc[1] == 1) || (cyc[0] == 1 && cyc[1] == 0)), "cycle P0 -> P1 -> P0 expected");
        request[1][0] = 0;      /* P1 no longer waits: acyclic */
        waitfor_build(&g, &b, (const int (*)[BANKER_MAX_R])request);
        CHECK(waitfor_find_cycle(&g, cyc, &clen) == 0 && waitfor_edges(&g) == 2, "acyclic without P1's request");
        printf("ok   wait-for graph: edges built from Allocation + Request; DFS finds P0 -> P1 -> P0, "
               "and no cycle once P1 stops waiting\n");
    }

    /* 9. Prevention by resource ordering; grant-if-free. Columns TRUCK, BOAT, MED, FOOD. */
    {
        static const int tot4[4] = { 2, 2, 4, 4 };
        const int boat[4] = { 0, 1, 0, 0 }, truck[4] = { 1, 0, 0, 0 }, two_trucks[4] = { 2, 0, 0, 0 };
        banker_init(&b, 2, 4, tot4);
        banker_set_max(&b, 0, tot4);
        banker_set_max(&b, 1, tot4);
        CHECK(deadlock_ordered(&b, 0, boat, NULL, seq, &len) == BANKER_GRANT, "P0: a BOAT first is in order");
        CHECK(deadlock_ordered(&b, 0, truck, NULL, seq, &len) == BANKER_ORDER,
              "P0 holds a BOAT: a TRUCK request is out of order");
        CHECK(deadlock_ordered(&b, 1, truck, NULL, seq, &len) == BANKER_GRANT &&
              deadlock_ordered(&b, 1, boat, NULL, seq, &len) == BANKER_GRANT, "P1: TRUCK then BOAT is in order");
        const int nothing[4] = { 0 };
        CHECK(deadlock_ordered(&b, 0, truck, nothing, seq, &len) == BANKER_GRANT,
              "per job: a job holding nothing may take a TRUCK although its camp holds a BOAT");
        CHECK(deadlock_grant_if_free(&b, 1, truck, NULL, seq, &len) == BANKER_UNAVAILABLE,
              "grant-if-free: no truck left, P1 must wait");
        CHECK(deadlock_grant_if_free(&b, 1, two_trucks, NULL, seq, &len) == BANKER_OVER_CLAIM,
              "grant-if-free: beyond the claim is an error");
        CHECK(deadlock_policy_find("detect")->detector && deadlock_policy_find("prevent-all")->all_or_nothing &&
              !deadlock_policy_find("bogus"), "policy table");
        printf("ok   resource ordering: BOAT then TRUCK rejected, TRUCK then BOAT granted; grant-if-free blocks\n");
    }

    if (g_failed) {
        fprintf(stderr, "test_banker: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    printf("ok   test_banker: all %d checks passed\n", g_checks);
    return 0;
}
