/*
 * OS concept: Robust process-shared mutex — recovering from a lock holder that died — Unit 3
 *
 * Test helper: attach to the live status table, enter a write (lock the mutex, make the
 * seqlock odd) and exit without unlocking, exactly like a camp killed mid-update.
 * tests/status.sh then checks that the next writer recovers the lock (EOWNERDEAD) and
 * that reliefstat can read the table again.
 */
#include "shm.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(void)
{
    relief_shm_t *s = relief_shm_open(1);

    if (!s) {
        fprintf(stderr, "shm_die_locked: %s: %s\n", RELIEF_SHM_NAME, strerror(errno));
        return 1;
    }
    if (relief_shm_begin_write(s) == -1) {
        fprintf(stderr, "shm_die_locked: lock: %s\n", strerror(errno));
        return 1;
    }
    printf("holding the status lock (seq odd); exiting without unlocking\n");
    fflush(stdout);
    _exit(0);       /* no unlock, no atexit handlers: the mutex owner is now dead */
}
