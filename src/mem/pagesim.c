/*
 * OS concept: Virtual memory — demand paging and page replacement on a camp's shelves
 *             (experiment E6) — Unit 4
 *
 * pagesim: a camp's F shelf slots are page frames, the items it needs in order are the reference
 * string, and a stock-out is a page fault.
 *
 *   pagesim --policy fifo|lru|optimal|expiry [--frames F]
 *           (--refs 7,0,1,... | --trace FILE | --seed S [--length N --items I])
 *           [--verbose] [--write-trace FILE] [--csv FILE] [--label L]
 *
 * --verbose prints the textbook table: one column per reference, one row per frame, and a
 * fault row (Y/N), in blocks of 20 references.
 * Trace file: one reference per line, "item" or "item,expiry" ('#' comments); expiry = the time
 * (reference index) the stock brought in then expires, none = never.
 * Generated trace (--seed): --length references (default 300) over --items items (default 12),
 * each item with a shelf life of 5-60 time units; 80% of the references come from a working set
 * of 4 items that moves every 40 references. Deterministic for a seed.
 * Exit status: 0 ok, 1 runtime error, 2 usage error.
 */
#include "../mem/paging.h"      /* src/mem is not on the include path */
#include "rng.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_REFS 1000000
#define BLOCK    20

static page_ref_t *parse_refs(const char *list, int *n)
{
    page_ref_t *r = calloc(strlen(list) + 1, sizeof *r);
    const char *p = list;
    char *end;
    *n = 0;
    while (r && *p) {
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v > 1000000 || (*end && *end != ',')) {
            free(r);
            return NULL;
        }
        r[(*n)++] = (page_ref_t){ (int)v, -1 };
        p = *end ? end + 1 : end;
    }
    return r;
}

static page_ref_t *read_trace(const char *path, int *n)
{
    FILE *f = fopen(path, "r");
    char line[128];
    int cap = 0, lineno = 0;
    page_ref_t *r = NULL;
    if (!f) {
        fprintf(stderr, "pagesim: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    *n = 0;
    while (fgets(line, sizeof line, f)) {
        int item, expiry = -1, k;
        lineno++;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        k = sscanf(line, "%d,%d", &item, &expiry);
        if (k < 1 || item < 0 || (k == 2 && expiry < 0) || *n >= MAX_REFS) {
            fprintf(stderr, "pagesim: %s:%d: expected \"item\" or \"item,expiry\"\n", path, lineno);
            free(r);
            fclose(f);
            return NULL;
        }
        if (*n == cap) {
            cap = cap ? cap * 2 : 256;
            page_ref_t *t = realloc(r, (size_t)cap * sizeof *r);
            if (!t) {
                free(r);
                fclose(f);
                return NULL;
            }
            r = t;
        }
        r[(*n)++] = (page_ref_t){ item, k == 2 ? expiry : -1 };
    }
    fclose(f);
    return r;
}

static page_ref_t *gen_trace(unsigned long seed, int n, int items)
{
    page_ref_t *r = calloc((size_t)n, sizeof *r);
    int life[256], ws[4];
    rng_t g;
    if (!r)
        return NULL;
    rng_seed(&g, seed * 0x9E3779B97F4A7C15ULL + 6);
    for (int i = 0; i < items; i++)
        life[i] = (int)rng_range(&g, 5, 60);
    for (int t = 0; t < n; t++) {
        if (t % 40 == 0)                            /* the working set moves */
            for (int k = 0; k < 4; k++)
                ws[k] = (int)rng_range(&g, 0, (uint32_t)items - 1);
        int item = rng_range(&g, 1, 100) <= 80 ? ws[rng_range(&g, 0, 3)] : (int)rng_range(&g, 0, (uint32_t)items - 1);
        r[t] = (page_ref_t){ item, t + life[item] };
    }
    return r;
}

/* --verbose: remember every column of the textbook table. */
typedef struct {
    int  frames, n, width;
    int *cell;                  /* n * frames item ids, -1 empty */
    char *fault;
    const page_ref_t *refs;
} table_t;

static void record(void *ctx, int t, const page_ref_t *r, const page_frame_t *fr, int nframes, int fault, int evicted)
{
    table_t *tb = ctx;
    (void)r; (void)evicted;
    for (int i = 0; i < nframes; i++)
        tb->cell[t * nframes + i] = fr[i].item;
    tb->fault[t] = fault ? 'Y' : 'N';
}

static void print_table(const table_t *tb)
{
    for (int from = 0; from < tb->n; from += BLOCK) {
        int to = from + BLOCK < tb->n ? from + BLOCK : tb->n;
        printf("%-10s", "reference");
        for (int t = from; t < to; t++)
            printf("%*d", tb->width, tb->refs[t].item);
        putchar('\n');
        for (int i = 0; i < tb->frames; i++) {
            printf("frame %-4d", i + 1);
            for (int t = from; t < to; t++) {
                int v = tb->cell[t * tb->frames + i];
                if (v < 0)
                    printf("%*s", tb->width, "");
                else
                    printf("%*d", tb->width, v);
            }
            putchar('\n');
        }
        printf("%-10s", "fault");
        for (int t = from; t < to; t++)
            printf("%*c", tb->width, tb->fault[t]);
        printf("\n\n");
    }
}

static void usage(FILE *f)
{
    fprintf(f, "usage: pagesim --policy fifo|lru|optimal|expiry [--frames F]\n"
               "               (--refs 7,0,1,... | --trace FILE | --seed S [--length N --items I])\n"
               "               [--verbose] [--write-trace FILE] [--csv FILE] [--label L]\n");
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "policy", required_argument, NULL, 'p' }, { "frames", required_argument, NULL, 'f' },
        { "refs", required_argument, NULL, 'r' }, { "trace", required_argument, NULL, 't' },
        { "seed", required_argument, NULL, 's' }, { "length", required_argument, NULL, 'n' },
        { "items", required_argument, NULL, 'i' }, { "verbose", no_argument, NULL, 'v' },
        { "write-trace", required_argument, NULL, 'w' }, { "csv", required_argument, NULL, 'c' },
        { "label", required_argument, NULL, 'l' }, { "help", no_argument, NULL, 'h' }, { NULL, 0, NULL, 0 }
    };
    const char *policy = NULL, *list = NULL, *trace = NULL, *wtrace = NULL, *csv = NULL, *label = NULL;
    long frames = 3, seed = 1, length = 300, items = 12;
    int verbose = 0, have_seed = 0, c;
    char *end;

    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        long *num = NULL, lo = 0, hi = 0;
        switch (c) {
        case 'p': policy = optarg; break;
        case 'r': list = optarg; break;
        case 't': trace = optarg; break;
        case 'w': wtrace = optarg; break;
        case 'c': csv = optarg; break;
        case 'l': label = optarg; break;
        case 'v': verbose = 1; break;
        case 'f': num = &frames; lo = 1; hi = PAGE_MAX_FRAMES; break;
        case 's': num = &seed; lo = 0; hi = 1L << 30; have_seed = 1; break;
        case 'n': num = &length; lo = 1; hi = MAX_REFS; break;
        case 'i': num = &items; lo = 1; hi = 256; break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
        if (num) {
            errno = 0;
            long v = strtol(optarg, &end, 10);
            if (errno || *end || end == optarg || v < lo || v > hi) {
                fprintf(stderr, "pagesim: bad value '%s' (%ld-%ld)\n", optarg, lo, hi);
                return 2;
            }
            *num = v;
        }
    }
    const page_policy_t *p = policy ? page_policy_find(policy) : NULL;
    if (!p || optind < argc || (!!list + !!trace + have_seed) > 1) {
        if (policy && !p)
            fprintf(stderr, "pagesim: unknown --policy '%s' (%s)\n", policy, page_policy_names());
        usage(stderr);
        return 2;
    }

    int n = 0;
    page_ref_t *refs = list ? parse_refs(list, &n) : trace ? read_trace(trace, &n)
                     : gen_trace((unsigned long)seed, (int)length, (int)items);
    if (list && !refs) {
        fprintf(stderr, "pagesim: --refs needs comma-separated item numbers, e.g. 7,0,1,2\n");
        return 2;
    }
    if (!refs)
        return 1;
    if (!list && !trace)
        n = (int)length;
    if (wtrace) {
        FILE *f = fopen(wtrace, "w");
        if (!f) {
            fprintf(stderr, "pagesim: %s: %s\n", wtrace, strerror(errno));
            free(refs);
            return 1;
        }
        fprintf(f, "# pagesim trace: item,expiry per reference\n");
        for (int t = 0; t < n; t++)
            refs[t].expiry < 0 ? fprintf(f, "%d\n", refs[t].item) : fprintf(f, "%d,%d\n", refs[t].item, refs[t].expiry);
        fclose(f);
    }

    table_t tb = { (int)frames, n, 3, NULL, NULL, refs };
    if (verbose) {
        int maxi = 0;
        for (int t = 0; t < n; t++)
            maxi = refs[t].item > maxi ? refs[t].item : maxi;
        while (maxi >= 100) {
            tb.width++;
            maxi /= 10;
        }
        tb.cell = malloc((size_t)n * (size_t)frames * sizeof *tb.cell);
        tb.fault = malloc((size_t)n + 1);
        if (!tb.cell || !tb.fault) {
            fprintf(stderr, "pagesim: out of memory\n");
            free(tb.cell); free(tb.fault); free(refs);
            return 1;
        }
    }
    page_stats_t st;
    page_run(p, refs, n, (int)frames, &st, verbose ? record : NULL, &tb);
    if (verbose)
        print_table(&tb);
    printf("pagesim --policy %s, %ld frames, %d references: %d faults (stock-outs), %d hits, hit ratio %.1f%%, "
           "expired on the shelf %d\n", p->name, frames, n, st.faults, st.hits, n ? 100.0 * st.hits / n : 0.0, st.expired);

    int rc = 0;
    if (csv) {
        FILE *f = fopen(csv, "a");
        if (!f) {
            fprintf(stderr, "pagesim: %s: %s\n", csv, strerror(errno));
            rc = 1;
        } else {
            if (ftell(f) == 0)
                fprintf(f, "label,policy,frames,references,faults,hits,hit_ratio,expired\n");
            fprintf(f, "%s,%s,%ld,%d,%d,%d,%.4f,%d\n", label ? label : p->name, p->name, frames, n, st.faults, st.hits,
                    n ? (double)st.hits / n : 0.0, st.expired);
            if (fclose(f) != 0)
                rc = 1;
        }
    }
    free(tb.cell);
    free(tb.fault);
    free(refs);
    return rc;
}
