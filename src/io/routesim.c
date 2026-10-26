/*
 * OS concept: Disk scheduling as delivery routing — a truck on one highway is a disk head
 *             (experiment E8) — Unit 5
 *
 * routesim: plan a truck's route along one highway (km 0 to --max, 199 by default) through a
 * set of delivery stops, as a disk head through its pending requests.
 *
 *   routesim --policy fcfs|sstf|scan|cscan|lifo [--start KM] [--dir up|down] [--max KM]
 *            (--stops 98,183,37,... | --seed S [--count N]) [--quiet] [--width W]
 *            [--csv FILE] [--label L]
 *
 * Prints the visiting order, the total km (and the C-SCAN return jump), the worst and the
 * average wait (km driven before a stop is reached), the worst detour (wait minus the direct
 * distance from the start) and an ASCII track diagram: one row per
 * leg, '=' the stretch driven, '>' or '<' where it arrives, '|' an end-of-highway turn and '~'
 * the C-SCAN jump back. --quiet leaves the diagram out. --seed: N random stops (default 20) and
 * a random start, deterministic for the seed (unless --start is given).
 * Exit status: 0 ok, 1 runtime error, 2 usage error.
 */
#include "../io/disk.h"         /* src/io is not on the include path */
#include "rng.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int col(int km, int max_km, int width)
{
    return (int)((long)km * (width - 1) / max_km);
}

static void print_diagram(const disk_point_t *path, int k, int max_km, int width)
{
    char *row = malloc((size_t)width + 1);
    if (!row)
        return;
    /* Scale: a tick every 50 km. */
    printf("%-11s", "km");
    for (int c = 0; c < width; c++)
        row[c] = ' ';
    row[width] = '\0';
    for (int km = 0; km <= max_km; km += 50) {
        char lab[8];
        int n = snprintf(lab, sizeof lab, "%d", km), at = col(km, max_km, width);
        if (max_km - km < 25)
            break;                      /* too close to the end label */
        for (int i = 0; i < n && at + i < width; i++)
            row[at + i] = lab[i];
    }
    char endlab[12];
    int en = snprintf(endlab, sizeof endlab, "%d", max_km);
    memcpy(row + width - en, endlab, (size_t)en);   /* the end of the highway, right-aligned */
    puts(row);
    printf("%-11s", "");
    for (int c = 0; c < width; c++)
        row[c] = '-';
    for (int km = 0; km <= max_km; km += 50)
        row[col(km, max_km, width)] = '+';
    row[width - 1] = '+';
    puts(row);
    long total = 0;
    for (int i = 0; i < k; i++) {
        memset(row, ' ', (size_t)width);
        int to = col(path[i].km, max_km, width);
        char label[16];
        if (i == 0) {
            row[to] = '*';
            snprintf(label, sizeof label, "start %d", path[i].km);
            printf("%-11s%s\n", label, row);
            continue;
        }
        int from = col(path[i - 1].km, max_km, width), lo = from < to ? from : to, hi = from < to ? to : from;
        long d = labs((long)path[i].km - path[i - 1].km);
        int jump = path[i].stop < 0 && path[i - 1].stop < 0;
        total += d;
        for (int c = lo; c <= hi; c++)
            row[c] = jump ? '~' : '=';
        row[to] = jump ? '~' : path[i].stop < 0 ? '|' : (to >= from ? '>' : '<');
        if (path[i].stop >= 0)
            snprintf(label, sizeof label, "%d", path[i].km);
        else
            snprintf(label, sizeof label, jump ? "jump %d" : "end %d", path[i].km);
        printf("%-11s%s  +%ld = %ld km\n", label, row, d, total);
    }
    free(row);
}

static int *parse_stops(const char *s, int *n)
{
    int *v = malloc((strlen(s) / 2 + 2) * sizeof *v);
    char *end;
    *n = 0;
    while (v && *s) {
        long x = strtol(s, &end, 10);
        if (end == s || x < 0 || (*end && *end != ',')) {
            free(v);
            return NULL;
        }
        v[(*n)++] = (int)x;
        s = *end ? end + 1 : end;
    }
    return v;
}

static void usage(FILE *f)
{
    fprintf(f, "usage: routesim --policy fcfs|sstf|scan|cscan|lifo [--start KM] [--dir up|down] [--max KM]\n"
               "                (--stops 98,183,37,... | --seed S [--count N]) [--quiet] [--width W]\n"
               "                [--csv FILE] [--label L]\n");
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "policy", required_argument, NULL, 'p' }, { "start", required_argument, NULL, 's' },
        { "dir", required_argument, NULL, 'd' }, { "max", required_argument, NULL, 'm' },
        { "stops", required_argument, NULL, 'S' }, { "seed", required_argument, NULL, 'r' },
        { "count", required_argument, NULL, 'n' }, { "quiet", no_argument, NULL, 'q' },
        { "width", required_argument, NULL, 'w' }, { "csv", required_argument, NULL, 'c' },
        { "label", required_argument, NULL, 'l' }, { "help", no_argument, NULL, 'h' }, { NULL, 0, NULL, 0 }
    };
    const char *policy = NULL, *list = NULL, *csv = NULL, *label = NULL;
    long start = -1, max_km = 199, seed = -1, count = 20, width = 100;
    int dir = DISK_UP, quiet = 0, c;
    char *end;

    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        long *num = NULL, lo = 0, hi = 0;
        switch (c) {
        case 'p': policy = optarg; break;
        case 'S': list = optarg; break;
        case 'c': csv = optarg; break;
        case 'l': label = optarg; break;
        case 'q': quiet = 1; break;
        case 'd':
            if (strcmp(optarg, "up") && strcmp(optarg, "down")) {
                fprintf(stderr, "routesim: --dir up or down\n");
                return 2;
            }
            dir = strcmp(optarg, "up") == 0 ? DISK_UP : DISK_DOWN;
            break;
        case 's': num = &start; lo = 0; hi = 1000000; break;
        case 'm': num = &max_km; lo = 1; hi = 1000000; break;
        case 'r': num = &seed; lo = 0; hi = 1L << 30; break;
        case 'n': num = &count; lo = 1; hi = DISK_MAX_STOPS; break;
        case 'w': num = &width; lo = 20; hi = 400; break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
        if (num) {
            errno = 0;
            long v = strtol(optarg, &end, 10);
            if (errno || *end || end == optarg || v < lo || v > hi) {
                fprintf(stderr, "routesim: bad value '%s' (%ld-%ld)\n", optarg, lo, hi);
                return 2;
            }
            *num = v;
        }
    }
    const disk_policy_t *p = policy ? disk_policy_find(policy) : NULL;
    if (!p || optind < argc || (!list) == (seed < 0)) {
        if (policy && !p)
            fprintf(stderr, "routesim: unknown --policy '%s' (%s)\n", policy, disk_policy_names());
        else if (!list && seed < 0)
            fprintf(stderr, "routesim: give --stops or --seed\n");
        usage(stderr);
        return 2;
    }

    int n = 0, *stops;
    if (list) {
        stops = parse_stops(list, &n);
        if (!stops || n == 0 || n > DISK_MAX_STOPS) {
            fprintf(stderr, "routesim: --stops needs comma-separated km values, e.g. 98,183,37\n");
            free(stops);
            return 2;
        }
        if (start < 0)
            start = 53;                 /* the textbook head position */
    } else {
        rng_t r;
        rng_seed(&r, (uint64_t)seed * 0x9E3779B97F4A7C15ULL + 8);
        n = (int)count;
        stops = malloc((size_t)n * sizeof *stops);
        if (!stops)
            return 1;
        long s0 = (long)rng_range(&r, 0, (uint32_t)max_km);
        for (int i = 0; i < n; i++)
            stops[i] = (int)rng_range(&r, 0, (uint32_t)max_km);
        if (start < 0)
            start = s0;
    }
    disk_point_t *path = malloc(((size_t)n + 4) * sizeof *path);
    disk_stats_t st;
    int k = path ? disk_route(p, stops, n, (int)start, dir, (int)max_km, path, &st) : -1;
    if (k < 0) {
        fprintf(stderr, "routesim: every km (start and stops) must be within 0-%ld\n", max_km);
        free(stops);
        free(path);
        return 2;
    }

    printf("routesim --policy %s: highway 0-%ld km, start %ld, moving %s, %d stops\n", p->name, max_km, start,
           dir == DISK_UP ? "up" : "down", n);
    printf("order: %ld", start);
    for (int i = 1; i < k; i++)
        printf(" -> %s%d", path[i].stop < 0 ? (path[i - 1].stop < 0 && i > 1 ? "(jump) " : "(end) ") : "", path[i].km);
    printf("\ntotal %ld km", st.total);
    if (st.jump)
        printf(" (of which %ld km the empty return jump; %ld without it)", st.jump, st.total - st.jump);
    printf("; wait before a stop: worst %ld km, average %.1f km; worst detour %ld km\n", st.worst_wait,
           n ? (double)st.sum_wait / n : 0.0, st.worst_detour);
    if (!quiet) {
        putchar('\n');
        print_diagram(path, k, (int)max_km, (int)width);
    }

    int rc = 0;
    if (csv) {
        FILE *f = fopen(csv, "a");
        if (!f) {
            fprintf(stderr, "routesim: %s: %s\n", csv, strerror(errno));
            rc = 1;
        } else {
            if (ftell(f) == 0)
                fprintf(f, "label,policy,seed,stops,start,dir,total_km,jump_km,worst_wait,avg_wait,worst_detour\n");
            fprintf(f, "%s,%s,%ld,%d,%ld,%s,%ld,%ld,%ld,%.2f,%ld\n", label ? label : p->name, p->name, seed, n, start,
                    dir == DISK_UP ? "up" : "down", st.total, st.jump, st.worst_wait, n ? (double)st.sum_wait / n : 0.0,
                    st.worst_detour);
            if (fclose(f) != 0)
                rc = 1;
        }
    }
    free(stops);
    free(path);
    return rc;
}
