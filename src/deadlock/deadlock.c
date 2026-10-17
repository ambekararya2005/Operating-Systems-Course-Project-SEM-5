/*
 * OS concept: Deadlock handling strategies — avoidance, prevention, detection, or none — Unit 3
 */
#include "deadlock.h"

#include <string.h>

static const deadlock_policy_t g_policies[] = {
    { "avoid",   "Banker's algorithm (deadlock avoidance)",              banker_request },
    { "none",    "grant when available (no deadlock handling)",          NULL },
    { "prevent", "resource ordering (deadlock prevention)",              NULL },
    { "detect",  "wait-for graph detection and recovery",                NULL },
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
