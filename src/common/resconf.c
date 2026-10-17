/*
 * OS concept: Deadlock avoidance — the system's resource totals for the Banker's algorithm — Unit 3
 */
#include "resconf.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int relief_resources_load(const char *path, int total[RELIEF_BANKER_RES], char *err, size_t errlen)
{
    static const int defaults[RELIEF_BANKER_RES] = RELIEF_BANKER_DEFAULT_TOTALS;
    char line[256];
    unsigned lineno = 0;
    FILE *f;

    memcpy(total, defaults, sizeof defaults);
    f = fopen(path, "r");
    if (!f) {
        snprintf(err, errlen, "%s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        char name[32], extra[2];
        long v;
        int b = -1, n;

        lineno++;
        char *hash = strchr(line, '#');
        if (hash)
            *hash = '\0';
        n = sscanf(line, "%31s %ld %1s", name, &v, extra);
        if (n <= 0)
            continue;                       /* blank or comment */
        for (int j = 0; j < RELIEF_BANKER_RES; j++)
            if (strcmp(name, relief_banker_name((unsigned)j)) == 0)
                b = j;
        if (n != 2 || b < 0 || v < 1 || v > RESCONF_MAX_TOTAL) {
            snprintf(err, errlen, "%s:%u: expected \"<TRUCK|BOAT|MED_KIT|FOOD_KIT> <1-%d>\"", path,
                     lineno, RESCONF_MAX_TOTAL);
            fclose(f);
            return -1;
        }
        total[b] = (int)v;
    }
    fclose(f);
    return 0;
}
