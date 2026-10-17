/*
 * OS concept: Deadlock handling strategies — avoidance, prevention, detection, or none — Unit 3
 */
#include "deadlock.h"

#include <string.h>

banker_result_t deadlock_grant_if_free(banker_t *b, int p, const int req[], const int held[], int seq[],
                                       int *len)
{
    (void)seq;
    (void)held;
    if (len)
        *len = 0;
    if (p < 0 || p >= b->n || !b->active[p])
        return BANKER_INACTIVE;
    for (int j = 0; j < b->m; j++)
        if (req[j] < 0 || req[j] > banker_need(b, p, j))
            return BANKER_OVER_CLAIM;
    for (int j = 0; j < b->m; j++)
        if (req[j] > b->available[j])
            return BANKER_UNAVAILABLE;      /* block: the caller keeps the request waiting */
    for (int j = 0; j < b->m; j++) {
        b->available[j] -= req[j];
        b->alloc[p][j] += req[j];
    }
    return BANKER_GRANT;
}

/*
 * Resource ordering (prevention of circular wait): number the resource types
 * F(TRUCK) = 0 < F(BOAT) = 1 < F(MED_KIT) = 2 < F(FOOD_KIT) = 3 and allow a process holding
 * R_k to request R_j only if F(j) >= F(k). A cycle P1 -> P2 -> ... -> P1 would need
 * F(R_1) < F(R_2) < ... < F(R_1), which is impossible.
 */
banker_result_t deadlock_ordered(banker_t *b, int p, const int req[], const int held[], int seq[],
                                 int *len)
{
    int lowest = -1, highest_held = -1;

    if (p < 0 || p >= b->n || !b->active[p])
        return BANKER_INACTIVE;
    for (int j = 0; j < b->m; j++) {
        if (req[j] > 0 && lowest < 0)
            lowest = j;
        if ((held ? held[j] : b->alloc[p][j]) > 0)
            highest_held = j;
    }
    if (lowest >= 0 && highest_held > lowest) {
        if (len)
            *len = 0;
        return BANKER_ORDER;
    }
    return deadlock_grant_if_free(b, p, req, held, seq, len);
}

static banker_result_t avoid_request(banker_t *b, int p, const int req[], const int held[], int seq[], int *len)
{
    (void)held;
    return banker_request(b, p, req, seq, len);
}

static const deadlock_policy_t g_policies[] = {
    { "avoid",       "Banker's algorithm (deadlock avoidance)", "none: stays in safe states",
      avoid_request, 0, 0 },
    { "none",        "grant when available (no deadlock handling)", "none",
      deadlock_grant_if_free, 0, 0 },
    { "prevent",     "resource ordering TRUCK < BOAT < MED_KIT < FOOD_KIT", "circular wait",
      deadlock_ordered, 0, 0 },
    { "prevent-all", "all-or-nothing acquisition of a task", "hold and wait",
      deadlock_grant_if_free, 0, 1 },
    { "detect",      "wait-for graph detection + recovery by preemption", "no preemption (recovery)",
      deadlock_grant_if_free, 1, 0 },
};

const deadlock_policy_t *deadlock_policy_find(const char *name)
{
    for (size_t i = 0; i < sizeof g_policies / sizeof g_policies[0]; i++)
        if (strcmp(name, g_policies[i].name) == 0)
            return &g_policies[i];
    return NULL;
}

const char *deadlock_policy_names(void)
{
    return "avoid, none, prevent, detect";
}
