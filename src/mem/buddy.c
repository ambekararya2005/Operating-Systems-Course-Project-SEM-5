/*
 * OS concept: The buddy system — power-of-two blocks, split on allocate, merge buddies on free;
 *             internal fragmentation — Unit 4
 *
 * The floor (a power of two, e.g. 1024) starts as one free block of order K (2^K units). A
 * request of n units gets a block of order k = ceil(log2 n): take the smallest free block of
 * order >= k and split it in halves until it is 2^k (each split leaves the other half, its
 * buddy, on the free list of the lower order). The buddy of the block at address a with size
 * s is at a XOR s. On free, while the buddy is free and of the same size, the two merge into
 * the block of the next order. Blocks are rounded up to a power of two, so the space between
 * the request and the block size is wasted: internal fragmentation.
 */
#include "mem.h"

#include <stdlib.h>
#include <string.h>

#define MAX_ORDER 20

struct mem_alloc {
    int            size, top;   /* size = 2^top */
    unsigned char *free_at;     /* free_at[a] = order + 1 if a free block of that order starts at a, else 0 */
    int           *addr, *order, *req;
};

static int order_for(int n)
{
    int k = 0;
    while ((1 << k) < n)
        k++;
    return k;
}

static mem_alloc_t *buddy_create(int size)
{
    int top = order_for(size);
    if ((1 << top) != size || top > MAX_ORDER)
        return NULL;            /* the floor must be a power of two */
    mem_alloc_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->size = size;
    m->top = top;
    m->free_at = calloc((size_t)size, 1);
    m->addr = malloc(MEM_MAX_IDS * sizeof *m->addr);
    m->order = calloc(MEM_MAX_IDS, sizeof *m->order);
    m->req = calloc(MEM_MAX_IDS, sizeof *m->req);
    if (!m->free_at || !m->addr || !m->order || !m->req) {
        free(m->free_at); free(m->addr); free(m->order); free(m->req); free(m);
        return NULL;
    }
    for (int i = 0; i < MEM_MAX_IDS; i++)
        m->addr[i] = -1;
    m->free_at[0] = (unsigned char)(top + 1);
    return m;
}

/* The lowest-addressed free block of exactly order k, or -1. */
static int find_free(const mem_alloc_t *m, int k)
{
    for (int a = 0; a < m->size; a += 1 << k)
        if (m->free_at[a] == k + 1)
            return a;
    return -1;
}

static int buddy_alloc(mem_alloc_t *m, int id, int kind, int count, int units)
{
    (void)kind; (void)count;
    if (id < 0 || id >= MEM_MAX_IDS || m->addr[id] >= 0 || units < 1 || units > m->size)
        return -1;
    int k = order_for(units), j = k, a = -1;
    while (j <= m->top && (a = find_free(m, j)) < 0)
        j++;
    if (a < 0)
        return -1;
    m->free_at[a] = 0;
    while (j > k) {             /* split: keep the lower half, free the upper half (the buddy) */
        j--;
        m->free_at[a + (1 << j)] = (unsigned char)(j + 1);
    }
    m->addr[id] = a;
    m->order[id] = k;
    m->req[id] = units;
    return 0;
}

static void buddy_release(mem_alloc_t *m, int id)
{
    if (id < 0 || id >= MEM_MAX_IDS || m->addr[id] < 0)
        return;
    int a = m->addr[id], k = m->order[id];
    m->addr[id] = -1;
    m->req[id] = 0;
    while (k < m->top) {        /* merge with the buddy while it is free and whole */
        int b = a ^ (1 << k);
        if (m->free_at[b] != k + 1)
            break;
        m->free_at[b] = 0;
        a = a < b ? a : b;
        k++;
    }
    m->free_at[a] = (unsigned char)(k + 1);
}

static void buddy_map(const mem_alloc_t *m, char *units)
{
    memset(units, '.', (size_t)m->size);
    for (int id = 0; id < MEM_MAX_IDS; id++) {
        if (m->addr[id] < 0)
            continue;
        memset(units + m->addr[id], mem_id_letter(id), (size_t)m->req[id]);
        memset(units + m->addr[id] + m->req[id], '~', (size_t)((1 << m->order[id]) - m->req[id]));
    }
}

static void buddy_stats(const mem_alloc_t *m, mem_stats_t *st)
{
    memset(st, 0, sizeof *st);
    st->size = m->size;
    for (int id = 0; id < MEM_MAX_IDS; id++) {
        if (m->addr[id] < 0)
            continue;
        st->requested += m->req[id];
        st->occupied += 1 << m->order[id];
    }
    /* Free space comes in free blocks; the largest one bounds what can be allocated. */
    for (int a = 0; a < m->size; a++) {
        if (!m->free_at[a])
            continue;
        int s = 1 << (m->free_at[a] - 1);
        st->free_total += s;
        st->holes++;
        if (s > st->largest_free)
            st->largest_free = s;
    }
    st->ext_frag = st->free_total ? 1.0 - (double)st->largest_free / st->free_total : 0.0;
    st->int_frag = st->occupied ? (double)(st->occupied - st->requested) / st->occupied : 0.0;
}

static int buddy_addr_of(const mem_alloc_t *m, int id)
{
    return id >= 0 && id < MEM_MAX_IDS ? m->addr[id] : -1;
}

static void buddy_destroy(mem_alloc_t *m)
{
    if (!m)
        return;
    free(m->free_at);
    free(m->addr);
    free(m->order);
    free(m->req);
    free(m);
}

const mem_ops_t mem_buddy = { "buddy", buddy_create, buddy_alloc, buddy_release, buddy_stats, buddy_map,
                              buddy_addr_of, buddy_destroy };
