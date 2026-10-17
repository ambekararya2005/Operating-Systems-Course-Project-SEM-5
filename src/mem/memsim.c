/*
 * OS concept: Memory allocation strategies and fragmentation — a warehouse-floor simulator
 *             (experiment E5) — Unit 4
 *
 * memsim replays a trace of relief shipments arriving (allocate) and leaving (free) on a
 * warehouse floor of --size units (default 1024) with one allocator:
 *   --alloc first|best|worst   variable partitioning: external fragmentation, failures
 *   --alloc buddy              buddy system: internal fragmentation (blocks rounded up)
 *   --alloc slab               slab caches of FOOD (4), MED (2), HYGIENE (1) unit kits
 * The trace is generated from --seed (--ops operations) or read with --trace F, and is the
 * same for every allocator: a shipment that does not fit fails, and its later "free" does
 * nothing. After every operation it samples the external and internal fragmentation.
 *
 *   memsim --alloc A [--seed S] [--ops N] [--size N] [--trace F] [--write-trace F]
 *          [--show N] [--width W] [--csv F] [--label L]
 *
 * --show N prints an ASCII map of the floor after every N operations (and at the end): one
 * character per size/W units, a shipment letter (A-Z, a-z), '~' wasted inside a buddy block,
 * F/M/H a slab's used kit and '_' its free slot, '.' free floor.
 * Trace CSV: "alloc,<id>,<FOOD|MED|HYGIENE>,<count>" and "free,<id>" lines ('#' comments).
 * Generated traces: a shipment is 1-12 FOOD, 1-16 MED or 1-16 HYGIENE kits (kinds 50/30/20%);
 * arrivals are more likely while the shipments on the floor (if all had fitted) take less
 * than 90% of it, so the floor runs close to full.
 * Exit status: 0 ok, 1 runtime error, 2 usage error.
 */
#include "../mem/mem.h"         /* src/mem is not on the include path */
#include "rng.h"

#include <errno.h>
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int is_alloc, id, kind, count;
} op_t;

static int kind_parse(const char *s)
{
    for (int k = 0; k < MEM_KINDS; k++)
        if (strcmp(s, mem_kind_names[k]) == 0)
            return k;
    return -1;
}

/* A seeded trace, independent of any allocator. */
static op_t *gen_trace(unsigned long seed, int nops, int size)
{
    static const int max_count[MEM_KINDS] = { 12, 16, 16 };
    op_t *ops = calloc((size_t)nops, sizeof *ops);
    int *live = calloc((size_t)nops, sizeof *live), *live_units = calloc((size_t)nops, sizeof *live_units);
    int nlive = 0, next_id = 0, units_live = 0;
    rng_t r;
    if (!ops || !live || !live_units) {
        free(ops); free(live); free(live_units);
        return NULL;
    }
    rng_seed(&r, seed * 0x9E3779B97F4A7C15ULL + 5);
    for (int i = 0; i < nops; i++) {
        unsigned p_alloc = units_live < size * 9 / 10 ? 65 : 30;
        if (nlive == 0 || rng_range(&r, 1, 100) <= p_alloc) {
            unsigned w = rng_range(&r, 1, 100);
            int k = w <= 50 ? 0 : w <= 80 ? 1 : 2;
            int count = (int)rng_range(&r, 1, (uint32_t)max_count[k]);
            ops[i] = (op_t){ 1, next_id, k, count };
            live[nlive] = next_id++;
            live_units[nlive++] = count * mem_kit_units[k];
            units_live += count * mem_kit_units[k];
        } else {
            int j = (int)rng_range(&r, 0, (uint32_t)nlive - 1);
            ops[i] = (op_t){ 0, live[j], 0, 0 };
            units_live -= live_units[j];
            live[j] = live[nlive - 1];
            live_units[j] = live_units[nlive - 1];
            nlive--;
        }
    }
    free(live);
    free(live_units);
    return ops;
}

static op_t *read_trace(const char *path, int *nops)
{
    FILE *f = fopen(path, "r");
    char line[128], what[16], kind[16];
    int cap = 0, n = 0, lineno = 0;
    op_t *ops = NULL;
    if (!f) {
        fprintf(stderr, "memsim: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    while (fgets(line, sizeof line, f)) {
        int id, count;
        lineno++;
        if (line[0] == '#' || line[0] == '\n')
            continue;
        if (n == cap) {
            cap = cap ? cap * 2 : 256;
            op_t *t = realloc(ops, (size_t)cap * sizeof *ops);
            if (!t) {
                free(ops);
                fclose(f);
                return NULL;
            }
            ops = t;
        }
        if (sscanf(line, "%15[^,],%d,%15[^,\n],%d", what, &id, kind, &count) == 4 && strcmp(what, "alloc") == 0 &&
            kind_parse(kind) >= 0 && count >= 1 && id >= 0 && id < MEM_MAX_IDS) {
            ops[n++] = (op_t){ 1, id, kind_parse(kind), count };
        } else if (sscanf(line, "%15[^,],%d", what, &id) == 2 && strcmp(what, "free") == 0 && id >= 0 && id < MEM_MAX_IDS) {
            ops[n++] = (op_t){ 0, id, 0, 0 };
        } else {
            fprintf(stderr, "memsim: %s:%d: expected alloc,<id>,<FOOD|MED|HYGIENE>,<count> or free,<id>\n", path, lineno);
            free(ops);
            fclose(f);
            return NULL;
        }
    }
    fclose(f);
    *nops = n;
    return ops;
}

/* The floor map compressed to `width` cells: each cell shows its most frequent character. */
static void print_map(const char *units, int size, int width)
{
    putchar('[');
    for (int c = 0; c < width; c++) {
        int from = (int)((long)c * size / width), to = (int)((long)(c + 1) * size / width);
        int counts[128] = { 0 }, best = '.';
        for (int a = from; a < to; a++)
            counts[(unsigned char)units[a] & 127]++;
        for (int a = from; a < to; a++)
            if (counts[(unsigned char)units[a] & 127] > counts[best])
                best = (unsigned char)units[a] & 127;
        putchar(best);
    }
    puts("]");
}

static void usage(FILE *f)
{
    fprintf(f, "usage: memsim --alloc first|best|worst|buddy|slab [--seed S] [--ops N] [--size N]\n"
               "              [--trace F] [--write-trace F] [--show N] [--width W] [--csv F] [--label L]\n");
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "alloc", required_argument, NULL, 'a' }, { "seed", required_argument, NULL, 's' },
        { "ops", required_argument, NULL, 'n' }, { "size", required_argument, NULL, 'z' },
        { "trace", required_argument, NULL, 't' }, { "write-trace", required_argument, NULL, 'T' },
        { "show", required_argument, NULL, 'S' }, { "width", required_argument, NULL, 'w' },
        { "csv", required_argument, NULL, 'c' }, { "label", required_argument, NULL, 'l' },
        { "help", no_argument, NULL, 'h' }, { NULL, 0, NULL, 0 }
    };
    const char *name = NULL, *trace = NULL, *wtrace = NULL, *csv = NULL, *label = NULL;
    long seed = 1, nops = 2000, size = 1024, show = 0, width = 128;
    int c;
    char *end;

    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        long *num = NULL, lo = 0, hi = 0;
        switch (c) {
        case 'a': name = optarg; break;
        case 't': trace = optarg; break;
        case 'T': wtrace = optarg; break;
        case 'c': csv = optarg; break;
        case 'l': label = optarg; break;
        case 's': num = &seed; lo = 0; hi = 1L << 30; break;
        case 'n': num = &nops; lo = 1; hi = 1000000; break;
        case 'z': num = &size; lo = 16; hi = 1 << 20; break;
        case 'S': num = &show; lo = 1; hi = 1000000; break;
        case 'w': num = &width; lo = 8; hi = 1 << 20; break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
        if (num) {
            errno = 0;
            long v = strtol(optarg, &end, 10);
            if (errno || *end || end == optarg || v < lo || v > hi) {
                fprintf(stderr, "memsim: bad value '%s' (%ld-%ld)\n", optarg, lo, hi);
                return 2;
            }
            *num = v;
        }
    }
    const mem_ops_t *ops = name ? mem_find(name) : NULL;
    if (!ops || optind < argc) {
        if (name && !ops)
            fprintf(stderr, "memsim: unknown --alloc '%s' (%s)\n", name, mem_names());
        usage(stderr);
        return 2;
    }
    if (width > size)
        width = size;

    int n = (int)nops;
    op_t *trace_ops = trace ? read_trace(trace, &n) : gen_trace((unsigned long)seed, n, (int)size);
    if (!trace_ops)
        return 1;
    if (wtrace) {
        FILE *f = fopen(wtrace, "w");
        if (!f) {
            fprintf(stderr, "memsim: %s: %s\n", wtrace, strerror(errno));
            free(trace_ops);
            return 1;
        }
        fprintf(f, "# memsim trace: seed %ld, %d operations\n", seed, n);
        for (int i = 0; i < n; i++) {
            if (trace_ops[i].is_alloc)
                fprintf(f, "alloc,%d,%s,%d\n", trace_ops[i].id, mem_kind_names[trace_ops[i].kind], trace_ops[i].count);
            else
                fprintf(f, "free,%d\n", trace_ops[i].id);
        }
        fclose(f);
    }
    mem_alloc_t *m = ops->create((int)size);
    char *units = malloc((size_t)size);
    if (!m || !units) {
        fprintf(stderr, "memsim: cannot create a %s allocator of %ld units%s\n", ops->name, size,
                strcmp(ops->name, "buddy") == 0 ? " (buddy needs a power of two)" : "");
        free(trace_ops);
        free(units);
        if (m)
            ops->destroy(m);
        return m ? 1 : 2;
    }

    long allocs = 0, failures = 0, fail_space = 0, frees = 0;
    double ext_sum = 0, int_sum = 0, occ_sum = 0, req_sum = 0;
    mem_stats_t st;
    for (int i = 0; i < n; i++) {
        const op_t *o = &trace_ops[i];
        if (o->is_alloc) {
            int need = o->count * mem_kit_units[o->kind];
            allocs++;
            ops->stats(m, &st);
            if (ops->alloc(m, o->id, o->kind, o->count, need) != 0) {
                failures++;
                if (st.free_total >= need)
                    fail_space++;       /* enough free space in total, but not usable */
            }
        } else {
            frees++;
            ops->release(m, o->id);
        }
        ops->stats(m, &st);
        ext_sum += st.ext_frag;
        int_sum += st.int_frag;
        occ_sum += (double)st.occupied / (double)size;
        req_sum += (double)st.requested / (double)size;
        if (show && ((i + 1) % show == 0 || i == n - 1)) {
            ops->map(m, units);
            printf("after %d ops (last: %s %d): requested %d, occupied %d, free %d in %d piece%s (largest %d); "
                   "ext %.0f%%, int %.0f%%\n", i + 1, o->is_alloc ? "alloc" : "free", o->id,
                   st.requested, st.occupied, st.free_total, st.holes,
                   st.holes == 1 ? "" : "s", st.largest_free, 100 * st.ext_frag, 100 * st.int_frag);
            print_map(units, (int)size, (int)width);
        }
    }
    ops->stats(m, &st);
    printf("memsim --alloc %s: %d units, %d operations (%ld allocations, %ld frees), %s\n", ops->name, (int)size, n,
           allocs, frees, trace ? trace : "generated trace");
    printf("  allocation failures %ld (%.1f%%), %ld of them with enough free space in total\n", failures,
           allocs ? 100.0 * failures / allocs : 0.0, fail_space);
    printf("  average external fragmentation %.1f%%, internal fragmentation %.1f%%, floor occupied %.1f%% "
           "(requested %.1f%%)\n", 100 * ext_sum / n, 100 * int_sum / n, 100 * occ_sum / n, 100 * req_sum / n);
    if (strcmp(ops->name, "slab") == 0) {
        for (int k = 0; k < MEM_KINDS; k++)
            printf("  cache %-8s %2d objects/slab; slabs full %d, partial %d, empty %d; objects %d/%d used "
                   "(utilisation %.0f%%)\n", mem_kind_names[k], st.objs_per_slab[k], st.slabs[k][0], st.slabs[k][1],
                   st.slabs[k][2], st.objs_used[k], st.objs_total[k],
                   st.objs_total[k] ? 100.0 * st.objs_used[k] / st.objs_total[k] : 0.0);
    }
    int rc = 0;
    if (csv) {
        FILE *f = fopen(csv, "a");
        if (!f) {
            fprintf(stderr, "memsim: %s: %s\n", csv, strerror(errno));
            rc = 1;
        } else {
            if (ftell(f) == 0)
                fprintf(f, "label,alloc,seed,size,ops,allocs,failures,failures_with_space,ext_frag_avg,int_frag_avg,"
                           "occupied_avg,requested_avg\n");
            fprintf(f, "%s,%s,%ld,%ld,%d,%ld,%ld,%ld,%.4f,%.4f,%.4f,%.4f\n", label ? label : ops->name, ops->name,
                    seed, size, n, allocs, failures, fail_space, ext_sum / n, int_sum / n, occ_sum / n, req_sum / n);
            if (fclose(f) != 0)
                rc = 1;
        }
    }
    ops->destroy(m);
    free(units);
    free(trace_ops);
    return rc;
}
