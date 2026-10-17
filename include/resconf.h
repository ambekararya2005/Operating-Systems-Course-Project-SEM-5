/*
 * OS concept: Deadlock avoidance — the system's resource totals for the Banker's algorithm — Unit 3
 *
 * scenarios/resources.conf: one "<RESOURCE> <total>" per line for TRUCK, BOAT, MED_KIT and
 * FOOD_KIT ('#' comments and blank lines allowed). Resources not listed keep the defaults
 * RELIEF_BANKER_DEFAULT_TOTALS (3, 2, 40, 60). Read by reliefd (the totals) and by every camp
 * (to keep its declared maximum claim within the totals).
 */
#ifndef RELIEF_RESCONF_H
#define RELIEF_RESCONF_H

#include "relief.h"

#include <stddef.h>

#define RESCONF_MAX_TOTAL 10000

/* Fill total[] (defaults first, then the file). Returns 0, or -1 with a message in err. */
int relief_resources_load(const char *path, int total[RELIEF_BANKER_RES], char *err, size_t errlen);

#endif /* RELIEF_RESCONF_H */
