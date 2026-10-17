/*
 * OS concept: Slab allocation — caches of fixed-size objects made of slabs, with full,
 *             partial and empty slab lists — Unit 4
 *
 * One cache per kind of pre-packed relief kit: FOOD (4 units), MED (2), HYGIENE (1). A slab is
 * one MEM_SLAB_UNITS-unit frame of the floor cut into equal object slots, so a FOOD slab holds
 * 4 kits, a MED slab 8 and a HYGIENE slab 16. To allocate a kit, the cache takes a slot from a
 * partial slab, else from an empty slab, else asks for a new frame; when the floor has no free
 * frame, an empty slab of another cache is reclaimed. Each slab sits on the full, partial or
 * empty list of its cache, by how many slots are in use. A shipment of `count` kits takes
 * `count` objects (all or none). Object sizes divide the slab exactly, so the only waste is
 * free slots in slabs that are not full: the slab's unused share (1 - utilisation).
 */
#include "mem.h"

#include <stdlib.h>
#include <string.h>

enum { L_FULL, L_PARTIAL, L_EMPTY };

typedef struct slab {
    int           frame, used, list;
    unsigned char slot_used[MEM_SLAB_UNITS];
    struct slab  *next, *prev;
} slab_t;

typedef struct {
    int     obj_units, per_slab;
    slab_t *lists[3];
    int     counts[3];
} cache_t;

struct mem_alloc {
    int      size, frames;
    cache_t  cache[MEM_KINDS];
    slab_t **slab_of;           /* per frame: its slab, or NULL if the frame is free */
    int     *kind_of;           /* per frame: the cache owning it */
    int    **objs;              /* per shipment id: frame * MEM_SLAB_UNITS + slot, per kit */
    int     *nobjs, *ship_kind;
};

static void list_remove(cache_t *c, slab_t *s)
{
    if (s->prev)
        s->prev->next = s->next;
    else
        c->lists[s->list] = s->next;
    if (s->next)
        s->next->prev = s->prev;
    c->counts[s->list]--;
}

static void list_push(cache_t *c, slab_t *s, int list)
{
    s->list = list;
    s->prev = NULL;
    s->next = c->lists[list];
    if (s->next)
        s->next->prev = s;
    c->lists[list] = s;
    c->counts[list]++;
}

/* Put the slab on the list that matches its fill. */
static void relist(cache_t *c, slab_t *s)
{
    int want = s->used == 0 ? L_EMPTY : s->used == c->per_slab ? L_FULL : L_PARTIAL;
    if (want != s->list) {
        list_remove(c, s);
        list_push(c, s, want);
    }
}

static mem_alloc_t *slab_create(int size)
{
    mem_alloc_t *m = calloc(1, sizeof *m);
    if (!m)
        return NULL;
    m->size = size;
    m->frames = size / MEM_SLAB_UNITS;
    m->slab_of = calloc((size_t)m->frames, sizeof *m->slab_of);
    m->kind_of = calloc((size_t)m->frames, sizeof *m->kind_of);
    m->objs = calloc(MEM_MAX_IDS, sizeof *m->objs);
    m->nobjs = calloc(MEM_MAX_IDS, sizeof *m->nobjs);
    m->ship_kind = calloc(MEM_MAX_IDS, sizeof *m->ship_kind);
    if (!m->slab_of || !m->kind_of || !m->objs || !m->nobjs || !m->ship_kind) {
        free(m->slab_of); free(m->kind_of); free(m->objs); free(m->nobjs); free(m->ship_kind); free(m);
        return NULL;
    }
    for (int k = 0; k < MEM_KINDS; k++) {
        m->cache[k].obj_units = mem_kit_units[k];
        m->cache[k].per_slab = MEM_SLAB_UNITS / mem_kit_units[k];
    }
    return m;
}

/* A frame for a new slab: a free one, else reclaim an empty slab of any cache. -1 if none. */
static int get_frame(mem_alloc_t *m)
{
    for (int f = 0; f < m->frames; f++)
        if (!m->slab_of[f])
            return f;
    for (int k = 0; k < MEM_KINDS; k++) {
        slab_t *s = m->cache[k].lists[L_EMPTY];
        if (s) {
            int f = s->frame;
            list_remove(&m->cache[k], s);
            free(s);
            m->slab_of[f] = NULL;
            return f;
        }
    }
    return -1;
}

/* One object from cache k: returns frame * MEM_SLAB_UNITS + slot, or -1. */
static int obj_alloc(mem_alloc_t *m, int k)
{
    cache_t *c = &m->cache[k];
    slab_t *s = c->lists[L_PARTIAL] ? c->lists[L_PARTIAL] : c->lists[L_EMPTY];
    if (!s) {
        int f = get_frame(m);
        if (f < 0)
            return -1;
        s = calloc(1, sizeof *s);
        if (!s)
            return -1;
        s->frame = f;
        m->slab_of[f] = s;
        m->kind_of[f] = k;
        list_push(c, s, L_EMPTY);
    }
    int slot = 0;
    while (s->slot_used[slot])
        slot++;
    s->slot_used[slot] = 1;
    s->used++;
    relist(c, s);
    return s->frame * MEM_SLAB_UNITS + slot;
}

static void obj_free(mem_alloc_t *m, int obj)
{
    int f = obj / MEM_SLAB_UNITS, slot = obj % MEM_SLAB_UNITS;
    slab_t *s = m->slab_of[f];
    if (!s || !s->slot_used[slot])
        return;
    s->slot_used[slot] = 0;
    s->used--;
    relist(&m->cache[m->kind_of[f]], s);     /* an emptied slab stays cached on the empty list */
}

static int slab_alloc(mem_alloc_t *m, int id, int kind, int count, int units)
{
    (void)units;
    if (id < 0 || id >= MEM_MAX_IDS || m->objs[id] || kind < 0 || kind >= MEM_KINDS || count < 1)
        return -1;
    int *objs = malloc((size_t)count * sizeof *objs);
    if (!objs)
        return -1;
    for (int i = 0; i < count; i++) {
        objs[i] = obj_alloc(m, kind);
        if (objs[i] < 0) {      /* all or nothing: give back what this shipment got */
            while (i--)
                obj_free(m, objs[i]);
            free(objs);
            return -1;
        }
    }
    m->objs[id] = objs;
    m->nobjs[id] = count;
    m->ship_kind[id] = kind;
    return 0;
}

static void slab_release(mem_alloc_t *m, int id)
{
    if (id < 0 || id >= MEM_MAX_IDS || !m->objs[id])
        return;
    for (int i = 0; i < m->nobjs[id]; i++)
        obj_free(m, m->objs[id][i]);
    free(m->objs[id]);
    m->objs[id] = NULL;
    m->nobjs[id] = 0;
}

static void slab_map(const mem_alloc_t *m, char *units)
{
    static const char letter[MEM_KINDS] = { 'F', 'M', 'H' };
    memset(units, '.', (size_t)m->size);
    for (int f = 0; f < m->frames; f++) {
        const slab_t *s = m->slab_of[f];
        if (!s)
            continue;
        int k = m->kind_of[f], w = m->cache[k].obj_units;
        for (int slot = 0; slot < m->cache[k].per_slab; slot++)
            memset(units + f * MEM_SLAB_UNITS + slot * w, s->slot_used[slot] ? letter[k] : '_', (size_t)w);
    }
}

static void slab_stats(const mem_alloc_t *m, mem_stats_t *st)
{
    memset(st, 0, sizeof *st);
    st->size = m->size;
    for (int k = 0; k < MEM_KINDS; k++) {
        const cache_t *c = &m->cache[k];
        st->objs_per_slab[k] = c->per_slab;
        for (int l = 0; l < 3; l++) {
            st->slabs[k][l] = c->counts[l];
            for (const slab_t *s = c->lists[l]; s; s = s->next)
                st->objs_used[k] += s->used;
        }
        st->objs_total[k] = (c->counts[0] + c->counts[1] + c->counts[2]) * c->per_slab;
        st->requested += st->objs_used[k] * c->obj_units;
        st->occupied += (c->counts[0] + c->counts[1] + c->counts[2]) * MEM_SLAB_UNITS;
    }
    st->free_total = m->size - st->occupied;
    st->holes = st->free_total / MEM_SLAB_UNITS;
    st->largest_free = st->free_total ? MEM_SLAB_UNITS : 0;
    st->ext_frag = 0.0;         /* no request ever needs more than one frame */
    st->int_frag = st->occupied ? (double)(st->occupied - st->requested) / st->occupied : 0.0;
}

static int slab_addr_of(const mem_alloc_t *m, int id)
{
    return id >= 0 && id < MEM_MAX_IDS && m->objs[id] ? m->objs[id][0] : -1;
}

int mem_slab_cache_of(const mem_alloc_t *m, int id)
{
    if (id < 0 || id >= MEM_MAX_IDS || !m->objs[id])
        return -1;
    return m->kind_of[m->objs[id][0] / MEM_SLAB_UNITS];
}

static void slab_destroy(mem_alloc_t *m)
{
    if (!m)
        return;
    for (int id = 0; id < MEM_MAX_IDS; id++)
        free(m->objs[id]);
    for (int f = 0; f < m->frames; f++)
        free(m->slab_of[f]);
    free(m->slab_of);
    free(m->kind_of);
    free(m->objs);
    free(m->nobjs);
    free(m->ship_kind);
    free(m);
}

const mem_ops_t mem_slab = { "slab", slab_create, slab_alloc, slab_release, slab_stats, slab_map, slab_addr_of,
                             slab_destroy };
