/*
 * OS concept: Disk scheduling — FCFS, SSTF, SCAN, C-SCAN, LIFO — Unit 5
 *
 * Unit tests for src/io/disk.c on the textbook queue (Silberschatz): requests for cylinders
 * 98, 183, 37, 122, 14, 124, 65, 67 (in that order), head at 53, disk 0-199.
 *
 *   FCFS   53 -> 98 -> 183 -> 37 -> 122 -> 14 -> 124 -> 65 -> 67
 *          45 + 85 + 146 + 85 + 108 + 110 + 59 + 2                     = 640
 *   SSTF   53 -> 65 -> 67 -> 37 -> 14 -> 98 -> 122 -> 124 -> 183
 *          12 + 2 + 30 + 23 + 84 + 24 + 2 + 59                         = 236
 *   Our SCAN / C-SCAN convention: the head moves UP and goes to the end of the disk (199):
 *   SCAN   53 -> 65 -> 67 -> 98 -> 122 -> 124 -> 183 -> 199 -> 37 -> 14
 *          (199 - 53) + (199 - 14) = 146 + 185                          = 331
 *   C-SCAN 53 -> 65 -> 67 -> 98 -> 122 -> 124 -> 183 -> 199, jump to 0, -> 14 -> 37
 *          (199 - 53) + 199 (the return jump, counted) + (37 - 0) = 146 + 199 + 37 = 382
 *          (183 without the return jump)
 *   SCAN moving DOWN (the textbook figure): 53 -> 37 -> 14 -> 0 -> 65 ... 183
 *          53 + 183                                                     = 236
 *   LIFO   53 -> 67 -> 65 -> 124 -> 14 -> 122 -> 37 -> 183 -> 98
 *          14 + 2 + 59 + 110 + 108 + 85 + 146 + 85                      = 609
 * Worst detour (wait - direct distance from 53; every stop is known up front, so the worst
 * wait is always the whole route and the detour is what tells the policies apart):
 *   FCFS   stop 65: reached after 638 km, 12 km away                    -> 626
 *   SSTF   stop 183: 236 - 130 (98, 122 and 124 tie at 106)             -> 106
 *   SCAN   stop 37: 146 + (199 - 37) = 308, 16 km away                  -> 292
 *   C-SCAN stop 37: 382 - 16                                            -> 366
 * Exit status 0 if every check passed.
 */
#include "../src/io/disk.h"

#include <stdio.h>

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

static const int Q[8] = { 98, 183, 37, 122, 14, 124, 65, 67 };

static long total(const char *policy, int dir, disk_stats_t *st, disk_point_t *path, int *k)
{
    *k = disk_route(disk_policy_find(policy), Q, 8, 53, dir, 199, path, st);
    return *k < 0 ? -1 : st->total;
}

/* The km of the stops in visiting order, as one comparable number list. */
static int order_is(const disk_point_t *path, int k, const int *want, int nwant)
{
    int j = 0;
    for (int i = 1; i < k; i++) {
        if (path[i].stop < 0)
            continue;
        if (j >= nwant || path[i].km != want[j])
            return 0;
        j++;
    }
    return j == nwant;
}

int main(void)
{
    disk_point_t path[16];
    disk_stats_t st;
    int k;

    CHECK(total("fcfs", DISK_UP, &st, path, &k) == 640, "FCFS: expected 640, got %ld", st.total);
    static const int fcfs_o[] = { 98, 183, 37, 122, 14, 124, 65, 67 };
    CHECK(order_is(path, k, fcfs_o, 8), "FCFS order");

    CHECK(total("sstf", DISK_UP, &st, path, &k) == 236, "SSTF: expected 236, got %ld", st.total);
    static const int sstf_o[] = { 65, 67, 37, 14, 98, 122, 124, 183 };
    CHECK(order_is(path, k, sstf_o, 8), "SSTF order");

    CHECK(total("scan", DISK_UP, &st, path, &k) == 331, "SCAN up: expected 331, got %ld", st.total);
    static const int scan_o[] = { 65, 67, 98, 122, 124, 183, 37, 14 };
    CHECK(order_is(path, k, scan_o, 8), "SCAN up order");
    CHECK(path[7].km == 199 && path[7].stop < 0, "SCAN goes to the end (199) before reversing");

    CHECK(total("cscan", DISK_UP, &st, path, &k) == 382 && st.jump == 199,
          "C-SCAN up: expected 382 with a 199 return jump, got %ld (jump %ld)", st.total, st.jump);
    static const int cscan_o[] = { 65, 67, 98, 122, 124, 183, 14, 37 };
    CHECK(order_is(path, k, cscan_o, 8), "C-SCAN order");

    CHECK(total("scan", DISK_DOWN, &st, path, &k) == 236, "SCAN down (textbook figure): expected 236, got %ld", st.total);
    CHECK(total("lifo", DISK_UP, &st, path, &k) == 609, "LIFO: expected 609, got %ld", st.total);
    static const int lifo_o[] = { 67, 65, 124, 14, 122, 37, 183, 98 };
    CHECK(order_is(path, k, lifo_o, 8), "LIFO order");
    printf("ok   textbook queue 98,183,37,122,14,124,65,67 from 53: FCFS 640, SSTF 236, SCAN(up) 331, "
           "C-SCAN(up) 382 incl. 199 return, SCAN(down) 236, LIFO 609\n");

    /* Waits: FCFS's first stop (98) is reached after 45 km, its last (67) after 640. */
    total("fcfs", DISK_UP, &st, path, &k);
    CHECK(st.worst_wait == 640, "FCFS worst wait 640, got %ld", st.worst_wait);
    total("sstf", DISK_UP, &st, path, &k);
    CHECK(st.worst_wait == 236, "SSTF worst wait = total (the last stop), got %ld", st.worst_wait);
    static const struct { const char *p; long d; } det[] = { { "fcfs", 626 }, { "sstf", 106 }, { "scan", 292 }, { "cscan", 366 } };
    for (int i = 0; i < 4; i++) {
        total(det[i].p, DISK_UP, &st, path, &k);
        CHECK(st.worst_detour == det[i].d, "%s worst detour: expected %ld, got %ld", det[i].p, det[i].d, st.worst_detour);
    }

    /* Edges: no stops; a stop at the start; out of range; C-SCAN down. */
    CHECK(disk_route(disk_policy_find("scan"), Q, 0, 53, DISK_UP, 199, path, &st) == 1 && st.total == 0, "no stops");
    const int here[2] = { 53, 60 };
    CHECK(disk_route(disk_policy_find("cscan"), here, 2, 53, DISK_DOWN, 199, path, &st) > 0 && path[1].km == 53,
          "a stop at the start is served first");
    const int bad[1] = { 250 };
    CHECK(disk_route(disk_policy_find("fcfs"), bad, 1, 53, DISK_UP, 199, path, &st) == -1, "km out of range");
    CHECK(total("cscan", DISK_DOWN, &st, path, &k) == 53 + 199 + (199 - 65) && st.jump == 199,
          "C-SCAN down: 53 to 0, jump to 199, down to 65 = 386, got %ld", st.total);
    CHECK(disk_policy_find("bogus") == NULL, "unknown policy");
    printf("ok   waits and detours (FCFS 626, SSTF 106, SCAN 292, C-SCAN 366), empty queue, stop at the start, range check, C-SCAN downwards\n");

    if (g_failed) {
        fprintf(stderr, "test_disk: %d of %d checks FAILED\n", g_failed, g_checks);
        return 1;
    }
    printf("ok   test_disk: all %d checks passed\n", g_checks);
    return 0;
}
