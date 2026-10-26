/*
 * OS concept: Virtual memory — demand paging and page replacement (FIFO, LRU, Optimal, and an
 *             expiry-first policy for perishables) — Unit 4
 *
 * A camp has F shelf slots (page frames). The items it needs, in order, are the reference
 * string (page references); an item not on the shelf is a stock-out (page fault), which brings
 * it in, evicting a victim chosen by the policy when the shelf is full:
 *   fifo     the item on the shelf longest
 *   lru      the item used least recently
 *   optimal  the item whose next use is furthest in the future (or never): Belady's OPT
 *   expiry   the item closest to its expiry date (first-expired-first-out)
 * Perishables: a reference may carry an expiry time (the stock brought in then expires at that
 * time; -1 = never). Stock still on the shelf after its expiry is wasted: an expired unit. It
 * is noticed when the item is needed again (then it is a fault too: restocked), when it is
 * evicted, or at the end. Time is the reference index.
 * src/mem is not on the include path: include it as "../mem/paging.h".
 */
#ifndef RELIEF_PAGING_H
#define RELIEF_PAGING_H

#define PAGE_MAX_FRAMES 64

typedef struct {
    int item;
    int expiry;                 /* time the stock brought in at this reference expires; -1 = never */
} page_ref_t;

typedef struct {
    int item;                   /* -1 = empty slot */
    int loaded, last_used, expiry;
} page_frame_t;

typedef struct {
    const char *name;
    /* Index of the frame to evict at time t (all frames are full). */
    int (*victim)(const page_frame_t *fr, int nframes, const page_ref_t *refs, int n, int t);
} page_policy_t;

typedef struct {
    int faults, hits, expired;
} page_stats_t;

/* Called after every reference: the shelf after it, whether it faulted, and what was evicted
 * (-1 if nothing). May be NULL. */
typedef void (*page_step_fn)(void *ctx, int t, const page_ref_t *r, const page_frame_t *fr, int nframes,
                             int fault, int evicted);

const page_policy_t *page_policy_find(const char *name);
const char *page_policy_names(void);

/* Run the reference string through `frames` slots. Returns 0, or -1 for bad arguments. */
int page_run(const page_policy_t *p, const page_ref_t *refs, int n, int frames, page_stats_t *st,
             page_step_fn step, void *ctx);

#endif /* RELIEF_PAGING_H */
