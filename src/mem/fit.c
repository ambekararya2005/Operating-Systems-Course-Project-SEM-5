/*
 * OS concept: Variable partitioning with a free list — first fit, best fit, worst fit;
 *             external fragmentation — Unit 4
 *
 * The free list holds the holes in address order. To place n units:
 *   first fit  the first hole that is big enough
 *   best fit   the smallest hole that is big enough (leaves the smallest leftover)
 *   worst fit  the largest hole (leaves the largest leftover)
 * The partition is cut from the start of the hole; the rest stays a hole. Freeing a partition
 * adds a hole and coalesces it with its neighbours. A request fails when no single hole is big
 * enough, even if the holes together would be: that is external fragmentation.
 */
#include "mem.h"

#include <stdlib.h>
#include <string.h>

enum { FIRST, BEST, WORST };

typedef struct { int addr, len; } hole_t;

struct mem_alloc {
    int     strategy, size;
    hole_t *holes;              /* sorted by address, never adjacent (always coalesced) */
    int     nholes;
    int    *addr, *len;         /* per shipment id: -1 / 0 when not allocated */
    int    *req;
};

static mem_alloc_t *create(int size, int strategy)
{
    mem_alloc_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->strategy = strategy;
    m->size = size;
    m->holes = calloc((size_t)size + 1, sizeof *m->holes);     /* at most size holes */
    m->addr = malloc(MEM_MAX_IDS * sizeof *m->addr);
    m->len = calloc(MEM_MAX_IDS, sizeof *m->len);
    m->req = calloc(MEM_MAX_IDS, sizeof *m->req);
    if (!m->holes || !m->addr || !m->len || !m->req) {
        free(m->holes); free(m->addr); free(m->len); free(m->req); free(m);
        return NULL;
    }
    for (int i = 0; i < MEM_MAX_IDS; i++)
        m->addr[i] = -1;
    m->holes[0] = (hole_t){ 0, size };
    m->nholes = 1;
    return m;
}

static mem_alloc_t *create_first(int size) { return create(size, FIRST); }
static mem_alloc_t *create_best(int size)  { return create(size, BEST); }
static mem_alloc_t *create_worst(int size) { return create(size, WORST); }

static int fit_alloc(mem_alloc_t *m, int id, int kind, int count, int units)
{
    (void)kind; (void)count;
    if (id < 0 || id >= MEM_MAX_IDS || m->addr[id] >= 0 || units < 1)
        return -1;
    int pick = -1;
    for (int i = 0; i < m->nholes; i++) {
        if (m->holes[i].len < units)
            continue;
        if (pick < 0 || (m->strategy == BEST && m->holes[i].len < m->holes[pick].len) ||
            (m->strategy == WORST && m->holes[i].len > m->holes[pick].len))
            pick = i;
        if (m->strategy == FIRST)
            break;
    }
    if (pick < 0)
        return -1;
    hole_t *h = &m->holes[pick];
    m->addr[id] = h->addr;
    m->len[id] = units;
    m->req[id] = units;
    h->addr += units;
    h->len -= units;
    if (h->len == 0) {
        memmove(&m->holes[pick], &m->holes[pick + 1], (size_t)(m->nholes - pick - 1) * sizeof *h);
        m->nholes--;
    }
    return 0;
}

static void fit_release(mem_alloc_t *m, int id)
{
    if (id < 0 || id >= MEM_MAX_IDS || m->addr[id] < 0)
        return;
    int a = m->addr[id], n = m->len[id], i = 0;
    m->addr[id] = -1;
    m->len[id] = m->req[id] = 0;
    while (i < m->nholes && m->holes[i].addr < a)
        i++;
    /* insert at i, then coalesce with the hole before and after */
    memmove(&m->holes[i + 1], &m->holes[i], (size_t)(m->nholes - i) * sizeof *m->holes);
    m->holes[i] = (hole_t){ a, n };
    m->nholes++;
    if (i + 1 < m->nholes && m->holes[i].addr + m->holes[i].len == m->holes[i + 1].addr) {
        m->holes[i].len += m->holes[i + 1].len;
        memmove(&m->holes[i + 1], &m->holes[i + 2], (size_t)(m->nholes - i - 2) * sizeof *m->holes);
        m->nholes--;
    }
    if (i > 0 && m->holes[i - 1].addr + m->holes[i - 1].len == m->holes[i].addr) {
        m->holes[i - 1].len += m->holes[i].len;
        memmove(&m->holes[i], &m->holes[i + 1], (size_t)(m->nholes - i - 1) * sizeof *m->holes);
        m->nholes--;
    }
}

static void fit_map(const mem_alloc_t *m, char *units)
{
    memset(units, '.', (size_t)m->size);
    for (int id = 0; id < MEM_MAX_IDS; id++)
        if (m->addr[id] >= 0)
            memset(units + m->addr[id], mem_id_letter(id), (size_t)m->len[id]);
}

static void fit_stats(const mem_alloc_t *m, mem_stats_t *st)
{
    char *units = malloc((size_t)m->size);
    memset(st, 0, sizeof *st);
    st->size = m->size;
    for (int id = 0; id < MEM_MAX_IDS; id++) {
        st->requested += m->req[id];
        st->occupied += m->len[id];
    }
    if (units) {
        fit_map(m, units);
        mem_free_runs(units, m->size, st);
        free(units);
    }
    st->int_frag = 0.0;         /* a partition is exactly what was asked for */
}

static int fit_addr_of(const mem_alloc_t *m, int id)
{
    return id >= 0 && id < MEM_MAX_IDS ? m->addr[id] : -1;
}

static void fit_destroy(mem_alloc_t *m)
{
    if (!m)
        return;
    free(m->holes);
    free(m->addr);
    free(m->len);
    free(m->req);
    free(m);
}

const mem_ops_t mem_first_fit = { "first", create_first, fit_alloc, fit_release, fit_stats, fit_map, fit_addr_of, fit_destroy };
const mem_ops_t mem_best_fit  = { "best",  create_best,  fit_alloc, fit_release, fit_stats, fit_map, fit_addr_of, fit_destroy };
const mem_ops_t mem_worst_fit = { "worst", create_worst, fit_alloc, fit_release, fit_stats, fit_map, fit_addr_of, fit_destroy };
