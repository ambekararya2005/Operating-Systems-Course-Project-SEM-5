/*
 * OS concept: The readers-writers problem — readers-preference with semaphores vs a
 *             writer-preference pthread rwlock — Unit 3
 */
#define _GNU_SOURCE     /* pthread_rwlockattr_setkind_np, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP */

#include "rwsync.h"

#include <errno.h>
#include <string.h>

static void sem_P(sem_t *s)     /* wait(): retry if a signal handler interrupts it */
{
    while (sem_wait(s) == -1 && errno == EINTR)
        ;
}

static void sem_V(sem_t *s)     /* signal() */
{
    sem_post(s);
}

/* ---- "semaphore": the first readers-writers problem (readers preference) ----------------
 *
 * Textbook pseudocode (Silberschatz):
 *     semaphore rw_mutex = 1;  semaphore mutex = 1;  int read_count = 0;
 *
 *     writer:                         reader:
 *       wait(rw_mutex);                 wait(mutex);
 *         ... writing ...                 read_count++;
 *       signal(rw_mutex);                 if (read_count == 1) wait(rw_mutex);   // first reader locks writers out
 *                                       signal(mutex);
 *                                         ... reading ...
 *                                       wait(mutex);
 *                                         read_count--;
 *                                         if (read_count == 0) signal(rw_mutex); // last reader lets writers in
 *                                       signal(mutex);
 *
 * rw_mutex must be a semaphore, not a mutex: the first reader takes it and a *different*
 * thread, the last reader, releases it. Readers preference: while any reader is inside, new
 * readers walk straight in, so if readers keep overlapping read_count never drops to 0 and a
 * waiting writer starves (build/rwlab shows it).
 */
static int sem_init_rw(rwsync_t *rw)
{
    rw->u.sem.read_count = 0;
    if (sem_init(&rw->u.sem.mutex, 0, 1) == -1)
        return -1;
    if (sem_init(&rw->u.sem.rw_mutex, 0, 1) == -1) {
        sem_destroy(&rw->u.sem.mutex);
        return -1;
    }
    return 0;
}

static void sem_read_lock(rwsync_t *rw)
{
    sem_P(&rw->u.sem.mutex);
    if (++rw->u.sem.read_count == 1)
        sem_P(&rw->u.sem.rw_mutex);
    sem_V(&rw->u.sem.mutex);
}

static void sem_read_unlock(rwsync_t *rw)
{
    sem_P(&rw->u.sem.mutex);
    if (--rw->u.sem.read_count == 0)
        sem_V(&rw->u.sem.rw_mutex);
    sem_V(&rw->u.sem.mutex);
}

static void sem_write_lock(rwsync_t *rw)   { sem_P(&rw->u.sem.rw_mutex); }
static void sem_write_unlock(rwsync_t *rw) { sem_V(&rw->u.sem.rw_mutex); }

static void sem_destroy_rw(rwsync_t *rw)
{
    sem_destroy(&rw->u.sem.rw_mutex);
    sem_destroy(&rw->u.sem.mutex);
}

/* ---- "rwlock": pthread rwlock, writer preference ------------------------------------------
 * glibc's default kind prefers readers (like the semaphore version). The _NP kind below makes
 * a waiting writer block new readers. NONRECURSIVE: a thread holding a read lock must not
 * take it again, or it could deadlock behind a waiting writer. */
static int rwlock_init(rwsync_t *rw)
{
    pthread_rwlockattr_t attr;
    int err = pthread_rwlockattr_init(&attr);
    if (!err)
        err = pthread_rwlockattr_setkind_np(&attr, PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP);
    if (!err)
        err = pthread_rwlock_init(&rw->u.rwlock, &attr);
    pthread_rwlockattr_destroy(&attr);
    if (err) {
        errno = err;
        return -1;
    }
    return 0;
}

static void rwlock_read_lock(rwsync_t *rw)    { pthread_rwlock_rdlock(&rw->u.rwlock); }
static void rwlock_write_lock(rwsync_t *rw)   { pthread_rwlock_wrlock(&rw->u.rwlock); }
static void rwlock_unlock(rwsync_t *rw)       { pthread_rwlock_unlock(&rw->u.rwlock); }
static void rwlock_destroy(rwsync_t *rw)      { pthread_rwlock_destroy(&rw->u.rwlock); }

static const rwsync_ops_t g_kinds[] = {
    { "semaphore", sem_init_rw, sem_read_lock, sem_read_unlock, sem_write_lock, sem_write_unlock,
      sem_destroy_rw },
    { "rwlock", rwlock_init, rwlock_read_lock, rwlock_unlock, rwlock_write_lock, rwlock_unlock,
      rwlock_destroy },
};

int rwsync_init(rwsync_t *rw, const char *kind)
{
    for (size_t i = 0; i < sizeof g_kinds / sizeof g_kinds[0]; i++) {
        if (strcmp(kind, g_kinds[i].name) == 0) {
            rw->ops = &g_kinds[i];
            return g_kinds[i].init(rw);
        }
    }
    errno = EINVAL;
    return -1;
}

const char *rwsync_kinds(void)
{
    return "semaphore, rwlock";
}
