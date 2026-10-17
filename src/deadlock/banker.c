/*
 * OS concept: Deadlock avoidance — the Banker's algorithm (safety algorithm and
 *             resource-request algorithm) — Unit 3
 */
#include "banker.h"

#include <string.h>

const char *banker_result_name(banker_result_t r)
{
    switch (r) {
    case BANKER_GRANT:       return "grant";
    case BANKER_OVER_CLAIM:  return "over_claim";
    case BANKER_UNAVAILABLE: return "unavailable";
    case BANKER_UNSAFE:      return "unsafe";
    case BANKER_INACTIVE:    return "inactive";
    }
    return "?";
}

void banker_init(banker_t *b, int n, int m, const int total[])
{
    memset(b, 0, sizeof *b);
    b->n = n > BANKER_MAX_P ? BANKER_MAX_P : n;
    b->m = m > BANKER_MAX_R ? BANKER_MAX_R : m;
    for (int j = 0; j < b->m; j++)
        b->total[j] = b->available[j] = total[j];
}

int banker_set_max(banker_t *b, int p, const int max[])
{
    if (p < 0 || p >= b->n)
        return -1;
    for (int j = 0; j < b->m; j++)
        if (b->alloc[p][j] != 0 || max[j] < 0 || max[j] > b->total[j])
            return -1;
    for (int j = 0; j < b->m; j++)
        b->max[p][j] = max[j];
    b->active[p] = 1;
    return 0;
}

int banker_set_alloc(banker_t *b, int p, const int alloc[])
{
    if (p < 0 || p >= b->n || !b->active[p])
        return -1;
    for (int j = 0; j < b->m; j++)
        if (alloc[j] < 0 || alloc[j] > b->max[p][j] || alloc[j] - b->alloc[p][j] > b->available[j])
            return -1;
    for (int j = 0; j < b->m; j++) {
        b->available[j] -= alloc[j] - b->alloc[p][j];
        b->alloc[p][j] = alloc[j];
    }
    return 0;
}

/*
 * Safety algorithm (Silberschatz):
 *   1. Work = Available; Finish[i] = false for every process.
 *   2. Find an i with Finish[i] == false and Need[i] <= Work. If none, go to 4.
 *   3. Work = Work + Allocation[i]; Finish[i] = true; go to 2.   (i finishes and returns all)
 *   4. The state is safe iff Finish[i] == true for every i.
 * Step 2 is done as repeated passes over i = 0..n-1: a pass does not restart at 0 after a
 * find, it carries on with i+1 and wraps around for the next pass. This is the usual
 * hand-worked order, and it gives the textbook's <P1, P3, P4, P0, P2> for its example.
 */
int banker_is_safe(const banker_t *b, int seq[], int *len)
{
    int work[BANKER_MAX_R];
    unsigned char finish[BANKER_MAX_P];
    int found = 0, active = 0, progress;

    for (int j = 0; j < b->m; j++)
        work[j] = b->available[j];
    for (int i = 0; i < b->n; i++) {
        finish[i] = !b->active[i];          /* inactive processes take no part */
        active += b->active[i];
    }
    do {
        progress = 0;
        for (int i = 0; i < b->n; i++) {
            if (finish[i])
                continue;
            int fits = 1;
            for (int j = 0; j < b->m && fits; j++)
                fits = banker_need(b, i, j) <= work[j];
            if (!fits)
                continue;
            for (int j = 0; j < b->m; j++)
                work[j] += b->alloc[i][j];
            finish[i] = 1;
            if (seq)
                seq[found] = i;
            found++;
            progress = 1;
        }
    } while (progress);
    if (len)
        *len = found;
    return found == active;
}

/*
 * Resource-request algorithm for process i with Request_i:
 *   1. If Request_i <= Need_i go to 2, else error: the process exceeded its maximum claim.
 *   2. If Request_i <= Available go to 3, else P_i must wait (the resources are not available).
 *   3. Pretend to allocate:  Available -= Request_i;  Allocation_i += Request_i;  Need_i -= Request_i.
 *      If the resulting state is safe, the allocation is made. Otherwise P_i must wait and the
 *      old state is restored.
 */
banker_result_t banker_request(banker_t *b, int p, const int req[], int seq[], int *len)
{
    if (len)
        *len = 0;
    if (p < 0 || p >= b->n || !b->active[p])
        return BANKER_INACTIVE;
    for (int j = 0; j < b->m; j++)
        if (req[j] < 0 || req[j] > banker_need(b, p, j))
            return BANKER_OVER_CLAIM;
    for (int j = 0; j < b->m; j++)
        if (req[j] > b->available[j])
            return BANKER_UNAVAILABLE;
    for (int j = 0; j < b->m; j++) {        /* pretend */
        b->available[j] -= req[j];
        b->alloc[p][j] += req[j];
    }
    if (banker_is_safe(b, seq, len))
        return BANKER_GRANT;
    for (int j = 0; j < b->m; j++) {        /* roll back */
        b->available[j] += req[j];
        b->alloc[p][j] -= req[j];
    }
    return BANKER_UNSAFE;
}

int banker_release(banker_t *b, int p, const int rel[])
{
    if (p < 0 || p >= b->n)
        return -1;
    for (int j = 0; j < b->m; j++)
        if (rel[j] < 0 || rel[j] > b->alloc[p][j])
            return -1;
    for (int j = 0; j < b->m; j++) {
        b->alloc[p][j] -= rel[j];
        b->available[j] += rel[j];
    }
    return 0;
}

void banker_remove(banker_t *b, int p)
{
    if (p < 0 || p >= b->n)
        return;
    for (int j = 0; j < b->m; j++) {
        b->available[j] += b->alloc[p][j];
        b->alloc[p][j] = 0;
        b->max[p][j] = 0;
    }
    b->active[p] = 0;
}
