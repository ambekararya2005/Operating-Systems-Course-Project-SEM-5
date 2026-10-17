/*
 * OS concept: The readers-writers problem — writer starvation under the readers-preference
 *             semaphore solution vs a writer-preference pthread rwlock — Unit 3
 *
 * rwlab: R reader threads (status, report, Banker's checks) read the inventory table in a
 * tight loop, each holding the read lock for --read-us microseconds and re-entering at once;
 * W writer threads (restocking, deliveries) update it every --writer-gap-ms. Every lock
 * acquisition is timed. Under the readers-preference semaphore solution, while any reader is
 * inside, new readers walk in, so with overlapping readers read_count never drops to 0 and
 * the writer waits until the readers stop (its maximum wait is about the whole run). Under
 * the writer-preference rwlock, a waiting writer blocks new readers, so it waits at most for
 * the readers already inside.
 *
 *   rwlab --rw semaphore|rwlock [--readers 8] [--writers 1] [--seconds 2] [--read-us 100]
 *         [--write-us 20] [--writer-gap-ms 2] [--csv FILE] [--run K]
 *
 * Every write sets all entries of the table to a new version, and every read checks that
 * they are all equal: torn_reads > 0 would mean a reader overlapped a writer (a bug).
 * Prints one key=value line; --csv appends a row (header if the file is new).
 * Exit status: 0 ran (torn_reads must be 0), 1 runtime error or a torn read, 2 usage error.
 */
#include "relief.h"
#include "rwsync.h"

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define MAX_THREADS 64

static rwsync_t g_rw;
static int64_t g_stock[RELIEF_WAREHOUSES][RELIEF_KIT_KINDS];    /* the shared inventory */
static atomic_int g_running = 1;
static atomic_int g_inside;                 /* readers inside right now */
static atomic_int g_max_inside;
static int64_t g_end_ns;                    /* readers stop at this time (set before start) */

typedef struct {
    int      index;
    int64_t  hold_ns, gap_ns;
    /* results, read by main after join */
    uint64_t ops, ops_during_run, torn;
    int64_t  wait_sum_ns, wait_max_ns;
} worker_t;

static int64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

static void record_wait(worker_t *w, int64_t waited)
{
    w->wait_sum_ns += waited;
    if (waited > w->wait_max_ns)
        w->wait_max_ns = waited;
}

static void *reader(void *p)
{
    worker_t *w = p;
    while (atomic_load(&g_running)) {
        int64_t t = now_ns();
        rwsync_read_lock(&g_rw);
        int64_t got = now_ns();
        record_wait(w, got - t);

        int in = atomic_fetch_add(&g_inside, 1) + 1, m = atomic_load(&g_max_inside);
        while (in > m && !atomic_compare_exchange_weak(&g_max_inside, &m, in))
            ;
        /* Read the whole table repeatedly for hold_ns; every entry must carry the same version. */
        do {
            int64_t v = g_stock[0][0];
            for (int i = 0; i < RELIEF_WAREHOUSES; i++)
                for (int k = 0; k < RELIEF_KIT_KINDS; k++)
                    if (g_stock[i][k] != v)
                        w->torn++;
        } while (now_ns() - got < w->hold_ns);
        atomic_fetch_sub(&g_inside, 1);

        rwsync_read_unlock(&g_rw);
        w->ops++;
        if (now_ns() >= g_end_ns)
            atomic_store(&g_running, 0);
    }
    return NULL;
}

static void *writer(void *p)
{
    worker_t *w = p;
    struct timespec gap = { (time_t)(w->gap_ns / 1000000000), (long)(w->gap_ns % 1000000000) };
    for (;;) {
        int64_t t = now_ns();
        rwsync_write_lock(&g_rw);
        int64_t got = now_ns();
        record_wait(w, got - t);
        int64_t v = g_stock[0][0] + 1;
        do {
            for (int i = 0; i < RELIEF_WAREHOUSES; i++)
                for (int k = 0; k < RELIEF_KIT_KINDS; k++)
                    g_stock[i][k] = v;
        } while (now_ns() - got < w->hold_ns);
        rwsync_write_unlock(&g_rw);
        w->ops++;
        if (got < g_end_ns)
            w->ops_during_run++;
        if (!atomic_load(&g_running) || now_ns() >= g_end_ns)
            break;
        nanosleep(&gap, NULL);
    }
    return NULL;
}

static void usage(FILE *f)
{
    fprintf(f, "usage: rwlab --rw semaphore|rwlock [--readers N] [--writers N] [--seconds S]\n"
               "             [--read-us US] [--write-us US] [--writer-gap-ms MS] [--csv FILE] [--run K]\n");
}

static int parse_num(const char *name, const char *s, double lo, double hi, double *out)
{
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno || end == s || *end || v < lo || v > hi) {
        fprintf(stderr, "rwlab: bad --%s '%s' (%g-%g)\n", name, s, lo, hi);
        return -1;
    }
    *out = v;
    return 0;
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "rw", required_argument, NULL, 'w' }, { "readers", required_argument, NULL, 'r' },
        { "writers", required_argument, NULL, 'W' }, { "seconds", required_argument, NULL, 's' },
        { "read-us", required_argument, NULL, 'R' }, { "write-us", required_argument, NULL, 'X' },
        { "writer-gap-ms", required_argument, NULL, 'g' }, { "csv", required_argument, NULL, 'c' },
        { "run", required_argument, NULL, 'k' }, { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    const char *kind = NULL, *csv = NULL;
    double readers = 8, writers = 1, seconds = 2, read_us = 100, write_us = 20, gap_ms = 2, run = 0;
    int c;

    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        int bad = 0;
        switch (c) {
        case 'w': kind = optarg; break;
        case 'r': bad = parse_num("readers", optarg, 1, MAX_THREADS - 1, &readers); break;
        case 'W': bad = parse_num("writers", optarg, 1, 8, &writers); break;
        case 's': bad = parse_num("seconds", optarg, 0.01, 600, &seconds); break;
        case 'R': bad = parse_num("read-us", optarg, 0, 1e6, &read_us); break;
        case 'X': bad = parse_num("write-us", optarg, 0, 1e6, &write_us); break;
        case 'g': bad = parse_num("writer-gap-ms", optarg, 0, 1e4, &gap_ms); break;
        case 'c': csv = optarg; break;
        case 'k': bad = parse_num("run", optarg, 0, 1e9, &run); break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
        if (bad)
            return 2;
    }
    if (!kind || optind < argc) {
        usage(stderr);
        return 2;
    }
    if (rwsync_init(&g_rw, kind) == -1) {
        if (errno == EINVAL) {
            fprintf(stderr, "rwlab: unknown --rw '%s' (%s)\n", kind, rwsync_kinds());
            return 2;
        }
        perror("rwlab: rwsync_init");
        return 1;
    }

    int nr = (int)readers, nw = (int)writers, n = nr + nw;
    pthread_t tid[MAX_THREADS + 8];
    worker_t wk[MAX_THREADS + 8];
    memset(wk, 0, sizeof wk);
    int64_t t0 = now_ns();
    g_end_ns = t0 + (int64_t)(seconds * 1e9);
    for (int i = 0; i < n; i++) {
        int is_reader = i < nr;
        wk[i].index = i;
        wk[i].hold_ns = (int64_t)((is_reader ? read_us : write_us) * 1000);
        wk[i].gap_ns = (int64_t)(gap_ms * 1e6);
        int err = pthread_create(&tid[i], NULL, is_reader ? reader : writer, &wk[i]);
        if (err) {
            fprintf(stderr, "rwlab: pthread_create: %s\n", strerror(err));
            atomic_store(&g_running, 0);
            for (int j = 0; j < i; j++)
                pthread_join(tid[j], NULL);
            return 1;
        }
    }
    for (int i = 0; i < n; i++)
        pthread_join(tid[i], NULL);
    double elapsed_s = (double)(now_ns() - t0) / 1e9;
    rwsync_destroy(&g_rw);

    uint64_t reads = 0, torn = 0, writes = 0, writes_run = 0;
    int64_t rmax = 0, wmax = 0, wsum = 0;
    for (int i = 0; i < n; i++) {
        if (i < nr) {
            reads += wk[i].ops;
            torn += wk[i].torn;
            if (wk[i].wait_max_ns > rmax)
                rmax = wk[i].wait_max_ns;
        } else {
            writes += wk[i].ops;
            writes_run += wk[i].ops_during_run;
            wsum += wk[i].wait_sum_ns;
            if (wk[i].wait_max_ns > wmax)
                wmax = wk[i].wait_max_ns;
        }
    }
    double wavg_ms = writes ? (double)wsum / (double)writes / 1e6 : 0.0;
    printf("rw=%s readers=%d writers=%d seconds=%.2f elapsed_s=%.2f reads=%llu writes=%llu "
           "writes_during_run=%llu writer_wait_avg_ms=%.3f writer_wait_max_ms=%.3f "
           "reader_wait_max_ms=%.3f max_concurrent_readers=%d torn_reads=%llu\n",
           kind, nr, nw, seconds, elapsed_s, (unsigned long long)reads, (unsigned long long)writes,
           (unsigned long long)writes_run, wavg_ms, (double)wmax / 1e6, (double)rmax / 1e6,
           atomic_load(&g_max_inside), (unsigned long long)torn);
    if (csv) {
        FILE *f = fopen(csv, "a");
        if (!f) {
            fprintf(stderr, "rwlab: %s: %s\n", csv, strerror(errno));
            return 1;
        }
        if (ftell(f) == 0)
            fprintf(f, "rw,run,readers,writers,seconds,elapsed_s,reads,writes,writes_during_run,"
                       "writer_wait_avg_ms,writer_wait_max_ms,reader_wait_max_ms,max_concurrent_readers,"
                       "torn_reads\n");
        fprintf(f, "%s,%d,%d,%d,%.2f,%.3f,%llu,%llu,%llu,%.3f,%.3f,%.3f,%d,%llu\n", kind, (int)run, nr, nw,
                seconds, elapsed_s, (unsigned long long)reads, (unsigned long long)writes,
                (unsigned long long)writes_run, wavg_ms, (double)wmax / 1e6, (double)rmax / 1e6,
                atomic_load(&g_max_inside), (unsigned long long)torn);
        if (fclose(f) != 0) {
            perror("rwlab: csv");
            return 1;
        }
    }
    return torn ? 1 : 0;
}
