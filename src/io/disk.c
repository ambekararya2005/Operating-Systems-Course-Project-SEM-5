/*
 * OS concept: Disk scheduling — FCFS, SSTF, SCAN, C-SCAN and LIFO — as truck routing on one
 *             highway — Unit 5
 */
#include "disk.h"

#include <stdlib.h>
#include <string.h>

static int fcfs_plan(const int *stops, int n, int start, int dir, int max_km, disk_point_t *path)
{
    (void)dir; (void)max_km;
    path[0] = (disk_point_t){ start, -1 };
    for (int i = 0; i < n; i++)
        path[i + 1] = (disk_point_t){ stops[i], i };
    return n + 1;
}

static int lifo_plan(const int *stops, int n, int start, int dir, int max_km, disk_point_t *path)
{
    (void)dir; (void)max_km;
    path[0] = (disk_point_t){ start, -1 };
    for (int i = 0; i < n; i++)
        path[i + 1] = (disk_point_t){ stops[n - 1 - i], n - 1 - i };
    return n + 1;
}

static int sstf_plan(const int *stops, int n, int start, int dir, int max_km, disk_point_t *path)
{
    (void)dir; (void)max_km;
    unsigned char *done = calloc((size_t)n + 1, 1);
    int head = start, k = 1;
    if (!done)
        return -1;
    path[0] = (disk_point_t){ start, -1 };
    for (int served = 0; served < n; served++) {
        int best = -1;
        for (int i = 0; i < n; i++) {
            if (done[i])
                continue;
            int d = abs(stops[i] - head), bd = best < 0 ? 0 : abs(stops[best] - head);
            if (best < 0 || d < bd || (d == bd && stops[i] < stops[best]))
                best = i;
        }
        done[best] = 1;
        head = stops[best];
        path[k++] = (disk_point_t){ head, best };
    }
    free(done);
    return k;
}

/* Stop indexes sorted by km (ties: the order given). */
static int *sorted_by_km(const int *stops, int n)
{
    int *idx = malloc((size_t)(n ? n : 1) * sizeof *idx);
    if (!idx)
        return NULL;
    for (int i = 0; i < n; i++)
        idx[i] = i;
    for (int a = 1; a < n; a++)             /* insertion sort: stable, n is small */
        for (int b = a; b > 0 && stops[idx[b - 1]] > stops[idx[b]]; b--) {
            int t = idx[b];
            idx[b] = idx[b - 1];
            idx[b - 1] = t;
        }
    return idx;
}

/* SCAN (circular = 0) and C-SCAN (circular = 1). */
static int sweep_plan(const int *stops, int n, int start, int dir, int max_km, disk_point_t *path, int circular)
{
    int *idx = sorted_by_km(stops, n), k = 1, first = 0;
    if (!idx)
        return -1;
    path[0] = (disk_point_t){ start, -1 };
    while (first < n && stops[idx[first]] < start)
        first++;                            /* idx[first..] are at or above start */
    if (dir == DISK_UP) {
        for (int i = first; i < n; i++)
            path[k++] = (disk_point_t){ stops[idx[i]], idx[i] };
        if (first > 0) {                    /* stops remain below: to the end, then back */
            path[k++] = (disk_point_t){ max_km, -1 };
            if (circular) {
                path[k++] = (disk_point_t){ 0, -1 };
                for (int i = 0; i < first; i++)
                    path[k++] = (disk_point_t){ stops[idx[i]], idx[i] };
            } else {
                for (int i = first - 1; i >= 0; i--)
                    path[k++] = (disk_point_t){ stops[idx[i]], idx[i] };
            }
        }
    } else {
        int below = first;                  /* stops strictly below start: idx[0..below-1] */
        int at = first;
        while (at < n && stops[idx[at]] == start)
            at++;                           /* stops exactly at start are served first */
        for (int i = first; i < at; i++)
            path[k++] = (disk_point_t){ stops[idx[i]], idx[i] };
        for (int i = below - 1; i >= 0; i--)
            path[k++] = (disk_point_t){ stops[idx[i]], idx[i] };
        if (at < n) {                       /* stops remain above */
            path[k++] = (disk_point_t){ 0, -1 };
            if (circular) {
                path[k++] = (disk_point_t){ max_km, -1 };
                for (int i = n - 1; i >= at; i--)
                    path[k++] = (disk_point_t){ stops[idx[i]], idx[i] };
            } else {
                for (int i = at; i < n; i++)
                    path[k++] = (disk_point_t){ stops[idx[i]], idx[i] };
            }
        }
    }
    free(idx);
    return k;
}

static int scan_plan(const int *s, int n, int start, int dir, int max_km, disk_point_t *p)
{
    return sweep_plan(s, n, start, dir, max_km, p, 0);
}

static int cscan_plan(const int *s, int n, int start, int dir, int max_km, disk_point_t *p)
{
    return sweep_plan(s, n, start, dir, max_km, p, 1);
}

static const disk_policy_t g_policies[] = {
    { "fcfs", fcfs_plan }, { "sstf", sstf_plan }, { "scan", scan_plan }, { "cscan", cscan_plan }, { "lifo", lifo_plan },
};

const disk_policy_t *disk_policy_find(const char *name)
{
    for (size_t i = 0; i < sizeof g_policies / sizeof g_policies[0]; i++)
        if (strcmp(name, g_policies[i].name) == 0)
            return &g_policies[i];
    return NULL;
}

const char *disk_policy_names(void)
{
    return "fcfs, sstf, scan, cscan, lifo";
}

int disk_route(const disk_policy_t *p, const int *stops, int n, int start, int dir, int max_km,
               disk_point_t *path, disk_stats_t *st)
{
    if (!p || n < 0 || n > DISK_MAX_STOPS || max_km < 1 || start < 0 || start > max_km ||
        (dir != DISK_UP && dir != DISK_DOWN))
        return -1;
    for (int i = 0; i < n; i++)
        if (stops[i] < 0 || stops[i] > max_km)
            return -1;
    int k = p->plan(stops, n, start, dir, max_km, path);
    if (k < 0)
        return -1;
    memset(st, 0, sizeof *st);
    for (int i = 1; i < k; i++) {
        long d = labs((long)path[i].km - path[i - 1].km);
        /* the C-SCAN return: an end-to-end move between two non-stop points */
        if (path[i].stop < 0 && path[i - 1].stop < 0 && i > 1 && d == max_km)
            st->jump += d;
        st->total += d;
        if (path[i].stop >= 0) {
            st->sum_wait += st->total;
            if (st->total > st->worst_wait)
                st->worst_wait = st->total;
            long detour = st->total - labs((long)path[i].km - start);
            if (detour > st->worst_detour)
                st->worst_detour = detour;
        }
    }
    return k;
}
