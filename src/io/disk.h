/*
 * OS concept: Disk scheduling — FCFS, SSTF, SCAN, C-SCAN and LIFO — as truck routing on one
 *             highway — Unit 5
 *
 * The highway (km 0 to max, 0-199 by default) is the disk, the truck is the head, and the
 * delivery stops are the pending I/O requests (cylinders). Every policy plans the whole route:
 *   fcfs   in the order the stops were given
 *   sstf   always the nearest unserved stop (shortest seek time first; ties: the lower km)
 *   scan   the elevator: keep going in --dir, serving stops on the way, to the end of the
 *          highway (km 0 or max), then reverse and serve the rest. The truck goes to the end
 *          only when stops remain behind it.
 *   cscan  circular SCAN: go to the end in --dir, jump back to the other end (that empty
 *          return counts as distance) and continue in the same direction
 *   lifo   the newest stop first (the reverse of the order given)
 * The route is a list of points: the start, each stop, and any end-of-highway turn or jump.
 * A stop's wait is the distance travelled before the truck reaches it; its detour is that wait
 * minus the direct distance from the start (0 = reached as if driven to first). With every stop
 * known up front the last stop always waits the whole route, so the worst detour is the fairness
 * measure that tells the policies apart.
 * src/io is not on the include path: include it as "../io/disk.h".
 */
#ifndef RELIEF_DISK_H
#define RELIEF_DISK_H

#define DISK_MAX_STOPS 4096

enum { DISK_UP = 1, DISK_DOWN = -1 };

typedef struct {
    int  km;
    int  stop;                  /* index of the stop served here, or -1 (start, turn, jump) */
} disk_point_t;

typedef struct {
    const char *name;
    /* Plan the route: write points (path[0] = start) and return how many, or -1. */
    int (*plan)(const int *stops, int n, int start, int dir, int max_km, disk_point_t *path);
} disk_policy_t;

typedef struct {
    long total;                 /* km travelled */
    long jump;                  /* of which the C-SCAN return jump(s) */
    long worst_wait, sum_wait;  /* km travelled before a stop is reached: worst, sum */
    long worst_detour;          /* max over stops of wait - |stop - start| */
} disk_stats_t;

const disk_policy_t *disk_policy_find(const char *name);
const char *disk_policy_names(void);

/* Plan the route and measure it. Returns the number of path points, or -1. path must hold
 * at least n + 4 points. */
int disk_route(const disk_policy_t *p, const int *stops, int n, int start, int dir, int max_km,
               disk_point_t *path, disk_stats_t *st);

#endif /* RELIEF_DISK_H */
