/*
 * OS concept: IPC via POSIX shared memory (shm_open + ftruncate + mmap) — Unit 2;
 *             process-shared, robust pthread mutex plus a seqlock for read-only readers — Unit 3
 *
 * Access functions for the live status table (layout: relief_shm_t in relief.h).
 *
 * Synchronisation:
 *   - Writers (reliefd, camps) bracket every update with relief_shm_begin_write() /
 *     relief_shm_end_write(). That takes the process-shared mutex and makes `seq` odd
 *     for the duration of the write.
 *   - Readers that map the segment read-only (reliefstat) cannot lock the mutex, because
 *     locking writes to the mutex itself. relief_shm_snapshot() copies the data and
 *     retries if `seq` was odd or changed meanwhile (a seqlock).
 *   - Phase 3 will compare this mutex with POSIX semaphores (sem_open / sem_init with
 *     pshared=1) guarding the same table.
 */
#ifndef RELIEF_SHM_H
#define RELIEF_SHM_H

#include "relief.h"

#include <stdint.h>

/* reliefd: create and initialise the segment (O_CREAT | O_EXCL). NULL + errno on failure. */
relief_shm_t *relief_shm_create(void);

/* Attach to an existing segment: writable for camps, read-only for reliefstat.
 * Checks the magic and version. NULL + errno on failure (ENOENT: no reliefd). */
relief_shm_t *relief_shm_open(int writable);

/* Detach (munmap) without removing the segment. */
void relief_shm_close(relief_shm_t *s);

/* reliefd: destroy the mutex, unmap and shm_unlink. Returns 0 or -1. */
int relief_shm_destroy(relief_shm_t *s);

/* Remove a segment left behind by a crashed reliefd. Returns 1 if one was removed. */
int relief_shm_unlink_stale(void);

/* Take the lock and mark a write in progress. Returns 0; 1 if a previous holder died
 * inside its critical section and the robust mutex was recovered (EOWNERDEAD); or -1 if
 * the lock is unusable. Either way of success must be followed by relief_shm_end_write(). */
int relief_shm_begin_write(relief_shm_t *s);
void relief_shm_end_write(relief_shm_t *s);

/* Lock-free consistent copy of s->data (seqlock read). Retries with back-off for up to
 * about 2 s, long enough for a robust-mutex recovery after a writer died mid-write.
 * Returns 0, or -1 (EBUSY) if the table stayed busy. */
int relief_shm_snapshot(const relief_shm_t *s, relief_status_t *out);

/* CLOCK_REALTIME in milliseconds (the time base for start_time_ms and heartbeat_ms). */
int64_t relief_now_ms(void);

#endif /* RELIEF_SHM_H */
