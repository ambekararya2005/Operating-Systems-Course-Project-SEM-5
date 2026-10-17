/*
 * OS concept: Memory allocation behind one interface — variable partitioning (first/best/worst
 *             fit), the buddy system and slab allocation — Unit 4
 *
 * The warehouse floor is a memory of `size` units (1024 by default). A shipment is `count`
 * relief kits of one kind; a kit of kind k takes mem_kit_units[k] units (FOOD 4, MED 2,
 * HYGIENE 1), so a shipment needs count * mem_kit_units[kind] units. Each allocator places
 * shipments its own way:
 *   first, best, worst  one contiguous partition, cut from a hole of the free list
 *   buddy               one power-of-two block (split on allocate, buddies merged on free)
 *   slab                `count` objects from the kind's cache; caches are made of slabs,
 *                       each slab a MEM_SLAB_UNITS-unit frame of the floor
 * Callers (memsim, the tests) only use mem_ops_t, so the strategies are interchangeable.
 * src/mem is not on the include path: include it as "../mem/mem.h".
 */
#ifndef RELIEF_MEM_H
#define RELIEF_MEM_H

#define MEM_KINDS       3                   /* FOOD, MED, HYGIENE */
#define MEM_SLAB_UNITS  16                  /* one slab = one 16-unit frame */
#define MEM_MAX_IDS     65536               /* shipment ids 0..MEM_MAX_IDS-1 */

extern const int mem_kit_units[MEM_KINDS];  /* { 4, 2, 1 } */
extern const char *const mem_kind_names[MEM_KINDS];

typedef struct mem_alloc mem_alloc_t;

typedef struct {
    int    size;                /* units on the floor */
    int    requested;           /* units the live shipments asked for */
    int    occupied;            /* units taken: partitions, buddy blocks, slab frames */
    int    free_total;          /* units not taken */
    int    largest_free;        /* largest contiguous free region (largest free block for buddy) */
    int    holes;               /* free regions (free blocks for buddy, free frames for slab) */
    double ext_frag;            /* 1 - largest_free / free_total: free space split into pieces */
    double int_frag;            /* (occupied - requested) / occupied: wasted inside what is taken */
    /* slab only */
    int    slabs[MEM_KINDS][3]; /* per cache: full, partial, empty slabs */
    int    objs_per_slab[MEM_KINDS];
    int    objs_used[MEM_KINDS], objs_total[MEM_KINDS];
} mem_stats_t;

typedef struct {
    const char *name;
    mem_alloc_t *(*create)(int size);
    /* Place shipment `id` (count kits of `kind`, `units` = count * kit size). 0, or -1 if it
     * does not fit (nothing is allocated then). */
    int  (*alloc)(mem_alloc_t *m, int id, int kind, int count, int units);
    /* Free shipment `id` (ignored if it is not allocated). */
    void (*release)(mem_alloc_t *m, int id);
    void (*stats)(const mem_alloc_t *m, mem_stats_t *st);
    /* One character per unit: a shipment letter (A-Z, a-z by id), '~' wasted inside a block,
     * '.' free; slab: F/M/H for a used kit, '_' a free object slot, '.' an unused frame. */
    void (*map)(const mem_alloc_t *m, char *units);
    /* Start address of shipment `id` (its first object for slab), or -1 (tests). */
    int  (*addr_of)(const mem_alloc_t *m, int id);
    void (*destroy)(mem_alloc_t *m);
} mem_ops_t;

/* The allocator called `name` (first, best, worst, buddy, slab), or NULL. */
const mem_ops_t *mem_find(const char *name);
const char *mem_names(void);
char mem_id_letter(int id);

/* Fill free_total, largest_free, holes and ext_frag from a per-unit map ('.' = free). */
void mem_free_runs(const char *units, int size, mem_stats_t *st);

/* Slab internals the unit tests look at. */
int  mem_slab_cache_of(const mem_alloc_t *m, int id);

#endif /* RELIEF_MEM_H */
