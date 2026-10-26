/*
 * OS concept: Virtual memory — demand paging and page replacement (FIFO, LRU, Optimal, and an
 *             expiry-first policy for perishables) — Unit 4
 */
#include "paging.h"

#include <limits.h>
#include <string.h>

/* Ties go to the lowest frame index, so every run is deterministic. */
static int fifo_victim(const page_frame_t *fr, int nf, const page_ref_t *refs, int n, int t)
{
    (void)refs; (void)n; (void)t;
    int v = 0;
    for (int i = 1; i < nf; i++)
        if (fr[i].loaded < fr[v].loaded)
            v = i;
    return v;
}

static int lru_victim(const page_frame_t *fr, int nf, const page_ref_t *refs, int n, int t)
{
    (void)refs; (void)n; (void)t;
    int v = 0;
    for (int i = 1; i < nf; i++)
        if (fr[i].last_used < fr[v].last_used)
            v = i;
    return v;
}

/* Belady's OPT: evict the item used furthest in the future, or never again. */
static int optimal_victim(const page_frame_t *fr, int nf, const page_ref_t *refs, int n, int t)
{
    int v = 0, far = -1;
    for (int i = 0; i < nf; i++) {
        int next = INT_MAX;
        for (int k = t + 1; k < n; k++)
            if (refs[k].item == fr[i].item) {
                next = k;
                break;
            }
        if (next > far) {
            far = next;
            v = i;
            if (next == INT_MAX)
                break;
        }
    }
    return v;
}

/* First-expired-first-out: evict the stock closest to its expiry (never-expiring stock last;
 * among equals, the oldest). */
static int expiry_victim(const page_frame_t *fr, int nf, const page_ref_t *refs, int n, int t)
{
    (void)refs; (void)n; (void)t;
    int v = 0;
    for (int i = 1; i < nf; i++) {
        long ei = fr[i].expiry < 0 ? LONG_MAX : fr[i].expiry, ev = fr[v].expiry < 0 ? LONG_MAX : fr[v].expiry;
        if (ei < ev || (ei == ev && fr[i].loaded < fr[v].loaded))
            v = i;
    }
    return v;
}

static const page_policy_t g_policies[] = {
    { "fifo", fifo_victim }, { "lru", lru_victim }, { "optimal", optimal_victim }, { "expiry", expiry_victim },
};

const page_policy_t *page_policy_find(const char *name)
{
    for (size_t i = 0; i < sizeof g_policies / sizeof g_policies[0]; i++)
        if (strcmp(name, g_policies[i].name) == 0)
            return &g_policies[i];
    return NULL;
}

const char *page_policy_names(void)
{
    return "fifo, lru, optimal, expiry";
}

static int expired_at(const page_frame_t *f, int t)
{
    return f->item >= 0 && f->expiry >= 0 && f->expiry < t;
}

int page_run(const page_policy_t *p, const page_ref_t *refs, int n, int frames, page_stats_t *st,
             page_step_fn step, void *ctx)
{
    page_frame_t fr[PAGE_MAX_FRAMES];

    if (!p || !refs || n < 0 || frames < 1 || frames > PAGE_MAX_FRAMES || !st)
        return -1;
    memset(st, 0, sizeof *st);
    for (int i = 0; i < frames; i++)
        fr[i] = (page_frame_t){ -1, 0, 0, -1 };
    for (int t = 0; t < n; t++) {
        const page_ref_t *r = &refs[t];
        int slot = -1, evicted = -1, fault = 1;
        for (int i = 0; i < frames; i++)
            if (fr[i].item == r->item)
                slot = i;
        if (slot >= 0 && !expired_at(&fr[slot], t)) {
            fault = 0;                          /* hit: on the shelf and still good */
            st->hits++;
        } else if (slot >= 0) {
            st->expired++;                      /* on the shelf but expired: wasted, restock in place */
        } else {
            for (int i = 0; i < frames && slot < 0; i++)
                if (fr[i].item < 0)
                    slot = i;                   /* a free slot */
            if (slot < 0) {
                slot = p->victim(fr, frames, refs, n, t);
                evicted = fr[slot].item;
                if (expired_at(&fr[slot], t))
                    st->expired++;              /* thrown out after it had expired */
            }
        }
        if (fault) {
            st->faults++;
            fr[slot] = (page_frame_t){ r->item, t, t, r->expiry };
        } else {
            fr[slot].last_used = t;
        }
        if (step)
            step(ctx, t, r, fr, frames, fault, evicted);
    }
    for (int i = 0; i < frames; i++)            /* still on the shelf, past its date, at the end */
        if (expired_at(&fr[i], n))
            st->expired++;
    return 0;
}
