/*
 * OS concept: The readers-writers problem — readers-preference with semaphores vs a
 *             writer-preference pthread rwlock — Unit 3
 *
 * One interface, two implementations, chosen by name at init time:
 *
 *   "semaphore"  the textbook first readers-writers solution (readers preference):
 *                read_count + mutex + rw_mutex, all POSIX semaphores. Readers never wait
 *                for a *waiting* writer, so a steady stream of readers starves writers.
 *   "rwlock"     pthread_rwlock_t with PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP
 *                (glibc): once a writer waits, new readers queue behind it. Writers do not
 *                starve; readers wait at most for the writers queued ahead of them.
 *
 * Used by reliefd's inventory (src/reliefd/inventory.c) and by build/rwlab, the benchmark
 * that shows the writer starvation. Not recursive: a thread must not take the lock twice.
 */
#ifndef RELIEF_RWSYNC_H
#define RELIEF_RWSYNC_H

#include <pthread.h>
#include <semaphore.h>

typedef struct rwsync rwsync_t;

typedef struct {
    const char *name;
    int  (*init)(rwsync_t *rw);
    void (*read_lock)(rwsync_t *rw);
    void (*read_unlock)(rwsync_t *rw);
    void (*write_lock)(rwsync_t *rw);
    void (*write_unlock)(rwsync_t *rw);
    void (*destroy)(rwsync_t *rw);
} rwsync_ops_t;

struct rwsync {
    const rwsync_ops_t *ops;
    union {
        struct {                    /* "semaphore" */
            sem_t mutex;            /* guards read_count */
            sem_t rw_mutex;         /* held by the writer, or by the readers as a group */
            int   read_count;       /* readers inside (guarded by mutex) */
        } sem;
        pthread_rwlock_t rwlock;    /* "rwlock" */
    } u;
};

/* Initialise rw with the implementation called `kind` ("semaphore" or "rwlock").
 * Returns 0, or -1 with errno set (EINVAL for an unknown kind). */
int rwsync_init(rwsync_t *rw, const char *kind);

/* Comma-separated list of the implementation names, for usage messages. */
const char *rwsync_kinds(void);

static inline void rwsync_read_lock(rwsync_t *rw)    { rw->ops->read_lock(rw); }
static inline void rwsync_read_unlock(rwsync_t *rw)  { rw->ops->read_unlock(rw); }
static inline void rwsync_write_lock(rwsync_t *rw)   { rw->ops->write_lock(rw); }
static inline void rwsync_write_unlock(rwsync_t *rw) { rw->ops->write_unlock(rw); }
static inline void rwsync_destroy(rwsync_t *rw)      { rw->ops->destroy(rw); }

#endif /* RELIEF_RWSYNC_H */
