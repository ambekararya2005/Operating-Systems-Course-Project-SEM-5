/*
 * OS concept: IPC via POSIX shared memory (shm_open + ftruncate + mmap) — Unit 2;
 *             process-shared, robust pthread mutex plus a seqlock for read-only readers — Unit 3
 */
#include "shm.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define SNAPSHOT_SPINS     100      /* quick retries (sched_yield) before sleeping */
#define SNAPSHOT_SLEEPS    2000     /* then retry every 1 ms: about 2 s in total */

int64_t relief_now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

relief_shm_t *relief_shm_create(void)
{
    pthread_mutexattr_t attr;
    relief_shm_t *s;
    int fd, err;

    /* 0644: owner (reliefd and its camps) read-write, anyone may run reliefstat. */
    fd = shm_open(RELIEF_SHM_NAME, O_CREAT | O_EXCL | O_RDWR, 0644);
    if (fd == -1)
        return NULL;
    if (ftruncate(fd, sizeof *s) == -1)
        goto fail_fd;
    s = mmap(NULL, sizeof *s, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (s == MAP_FAILED)
        goto fail_fd;
    close(fd);      /* the mapping keeps the segment alive */

    memset(s, 0, sizeof *s);    /* ftruncate already zero-fills; be explicit */
    atomic_init(&s->seq, 0);

    /* A mutex in shared memory must be marked PTHREAD_PROCESS_SHARED, or its behaviour
     * across processes is undefined. ROBUST: if a camp is killed (kill -9) while holding
     * the lock, the next locker gets EOWNERDEAD and can repair it instead of hanging. */
    if ((err = pthread_mutexattr_init(&attr)) != 0 ||
        (err = pthread_mutexattr_setpshared(&attr, PTHREAD_PROCESS_SHARED)) != 0 ||
        (err = pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST)) != 0 ||
        (err = pthread_mutex_init(&s->lock, &attr)) != 0) {
        pthread_mutexattr_destroy(&attr);
        munmap(s, sizeof *s);
        shm_unlink(RELIEF_SHM_NAME);
        errno = err;
        return NULL;
    }
    pthread_mutexattr_destroy(&attr);

    s->version = RELIEF_SHM_VERSION;
    atomic_thread_fence(memory_order_release);
    s->magic = RELIEF_SHM_MAGIC;    /* last: readers treat the segment as ready from here */
    return s;

fail_fd:
    err = errno;
    close(fd);
    shm_unlink(RELIEF_SHM_NAME);
    errno = err;
    return NULL;
}

relief_shm_t *relief_shm_open(int writable)
{
    struct stat st;
    relief_shm_t *s;
    int fd, err;

    fd = shm_open(RELIEF_SHM_NAME, writable ? O_RDWR : O_RDONLY, 0);
    if (fd == -1)
        return NULL;
    if (fstat(fd, &st) == -1)
        goto fail_fd;
    if ((size_t)st.st_size < sizeof *s) {
        close(fd);
        errno = EPROTO;     /* not (yet) a fully sized status segment */
        return NULL;
    }
    s = mmap(NULL, sizeof *s, writable ? PROT_READ | PROT_WRITE : PROT_READ, MAP_SHARED, fd, 0);
    if (s == MAP_FAILED)
        goto fail_fd;
    close(fd);

    if (s->magic != RELIEF_SHM_MAGIC || s->version != RELIEF_SHM_VERSION) {
        munmap(s, sizeof *s);
        errno = EPROTO;     /* uninitialised or from an incompatible build */
        return NULL;
    }
    atomic_thread_fence(memory_order_acquire);
    return s;

fail_fd:
    err = errno;
    close(fd);
    errno = err;
    return NULL;
}

void relief_shm_close(relief_shm_t *s)
{
    if (s)
        munmap(s, sizeof *s);
}

int relief_shm_destroy(relief_shm_t *s)
{
    int rc = 0;

    if (s) {
        pthread_mutex_destroy(&s->lock);
        munmap(s, sizeof *s);
    }
    if (shm_unlink(RELIEF_SHM_NAME) == -1 && errno != ENOENT)
        rc = -1;
    return rc;
}

int relief_shm_unlink_stale(void)
{
    return shm_unlink(RELIEF_SHM_NAME) == 0;
}

int relief_shm_begin_write(relief_shm_t *s)
{
    int err = pthread_mutex_lock(&s->lock), recovered = 0;
    uint32_t seq;

    if (err == EOWNERDEAD) {
        recovered = 1;
        /* The previous holder died inside its critical section. Its write may be half
         * done, but each field is independently meaningful, so repair the seqlock
         * (seq may have been left odd) and mark the mutex usable again. */
        seq = atomic_load_explicit(&s->seq, memory_order_relaxed);
        if (seq & 1u)
            atomic_store_explicit(&s->seq, seq + 1, memory_order_relaxed);
        pthread_mutex_consistent(&s->lock);
    } else if (err != 0) {
        errno = err;
        return -1;
    }

    seq = atomic_load_explicit(&s->seq, memory_order_relaxed);
    atomic_store_explicit(&s->seq, seq + 1, memory_order_relaxed);     /* odd: writing */
    atomic_thread_fence(memory_order_release);
    return recovered;
}

void relief_shm_end_write(relief_shm_t *s)
{
    uint32_t seq = atomic_load_explicit(&s->seq, memory_order_relaxed);
    atomic_store_explicit(&s->seq, seq + 1, memory_order_release);    /* even: stable */
    pthread_mutex_unlock(&s->lock);
}

int relief_shm_snapshot(const relief_shm_t *s, relief_status_t *out)
{
    /* The C11 atomic_load_explicit takes a non-const pointer; reading through it does
     * not write, which is what makes this safe on a PROT_READ mapping. */
    _Atomic uint32_t *seqp = (_Atomic uint32_t *)&s->seq;

    const struct timespec one_ms = { 0, 1000000L };

    for (int tries = 0; tries < SNAPSHOT_SPINS + SNAPSHOT_SLEEPS; tries++) {
        uint32_t before = atomic_load_explicit(seqp, memory_order_acquire);
        if (before & 1u) {          /* a writer is inside (or died there): wait */
            if (tries < SNAPSHOT_SPINS)
                sched_yield();
            else
                nanosleep(&one_ms, NULL);
            continue;
        }
        memcpy(out, (const void *)&s->data, sizeof *out);
        atomic_thread_fence(memory_order_acquire);
        if (atomic_load_explicit(seqp, memory_order_relaxed) == before)
            return 0;               /* nothing changed while copying: consistent */
    }
    errno = EBUSY;
    return -1;
}
