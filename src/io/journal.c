/*
 * OS concept: File-system implementation — a write-ahead journal with fsync, checkpointing by
 *             atomic rename, crash recovery by replay — Unit 5
 */
#include "../io/journal.h"      /* src/io is not on the include path */
#include "log.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CRC_LEN offsetof(jr_record_t, crc)

struct journal {
    char       logpath[PATH_MAX], snappath[PATH_MAX], tmppath[PATH_MAX], dir[PATH_MAX];
    int        fd;                  /* journal.log, O_APPEND */
    off_t      bytes;               /* its valid length */
    jr_state_t st;                  /* the committed state */
    uint64_t   seq;                 /* last committed record */
    unsigned   since_ckpt;
    uint64_t   appended, checkpoints;
    int64_t    sync_ns, sync_max_ns;    /* write + fsync time of the appends */
};

uint32_t jr_crc32(const void *buf, unsigned long len)
{
    const unsigned char *p = buf;
    uint32_t c = 0xFFFFFFFFu;
    while (len--) {
        c ^= *p++;
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & -(c & 1u));
    }
    return c ^ 0xFFFFFFFFu;
}

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* ---- The committed state ---------------------------------------------------------------- */

static int find_claim(const jr_state_t *s, uint32_t camp)
{
    for (int i = 0; i < s->nclaim; i++)
        if (s->claim[i].camp == camp)
            return i;
    return -1;
}

static int find_grant(const jr_state_t *s, uint32_t camp, uint32_t id)
{
    for (int i = 0; i < s->ngrant; i++)
        if (s->grant[i].camp == camp && s->grant[i].id == id)
            return i;
    return -1;
}

/* dry: only check that r fits the state. */
static int apply(jr_state_t *s, const jr_record_t *r, int dry)
{
    int c = find_claim(s, r->camp), g = find_grant(s, r->camp, r->id);

    switch (r->type) {
    case JR_CLAIM:
        if (r->camp == 0 || (c < 0 && s->nclaim == JR_MAX_CAMPS))
            return -1;
        for (int i = 0; i < s->ngrant; i++)
            if (s->grant[i].camp == r->camp)
                return -1;          /* re-claim only after RELEASE_ALL */
        if (dry)
            return 0;
        if (c < 0)
            c = s->nclaim++;
        s->claim[c].camp = r->camp;
        for (int j = 0; j < JR_NRES; j++)
            s->claim[c].max[j] = r->units[j];
        return 0;
    case JR_GRANT: {
        if (c < 0 || g >= 0)
            return -1;
        int held[JR_NRES] = { 0 };
        for (int i = 0; i < s->ngrant; i++)
            for (int j = 0; j < JR_NRES && s->grant[i].camp == r->camp; j++)
                held[j] += s->grant[i].units[j];
        for (int j = 0; j < JR_NRES; j++)
            if (r->units[j] < 0 || held[j] + r->units[j] > s->claim[c].max[j])
                return -1;          /* more than the claim */
        if (dry)
            return 0;
        if (s->ngrant == s->cap) {
            int cap = s->cap ? 2 * s->cap : 64;
            jr_grant_t *v = realloc(s->grant, (size_t)cap * sizeof *v);
            if (!v)
                return -1;
            s->grant = v;
            s->cap = cap;
        }
        jr_grant_t *n = &s->grant[s->ngrant++];
        n->camp = r->camp;
        n->id = r->id;
        memcpy(n->units, r->units, sizeof n->units);
        return 0;
    }
    case JR_RELEASE:
        if (g < 0)
            return -1;
        if (!dry)
            memmove(&s->grant[g], &s->grant[g + 1], (size_t)(--s->ngrant - g) * sizeof *s->grant);
        return 0;
    case JR_RELEASE_ALL:
        if (dry)
            return 0;
        for (int i = 0; i < s->ngrant;)
            if (s->grant[i].camp == r->camp)
                memmove(&s->grant[i], &s->grant[i + 1], (size_t)(--s->ngrant - i) * sizeof *s->grant);
            else
                i++;
        if (c >= 0)
            memmove(&s->claim[c], &s->claim[c + 1], (size_t)(--s->nclaim - c) * sizeof *s->claim);
        return 0;
    default:
        return -1;
    }
}

int jr_apply(jr_state_t *s, const jr_record_t *r)
{
    return apply(s, r, 1) == 0 ? apply(s, r, 0) : -1;
}

void jr_state_free(jr_state_t *s)
{
    free(s->grant);
    memset(s, 0, sizeof *s);
}

/* ---- Records on disk -------------------------------------------------------------------- */

static void seal(jr_record_t *r)
{
    r->crc = jr_crc32(r, CRC_LEN);
}

static void make(jr_record_t *r, uint64_t seq, jr_type_t type, uint32_t camp, uint32_t id, const int *units)
{
    memset(r, 0, sizeof *r);
    r->seq = seq;
    r->type = type;
    r->camp = camp;
    r->id = id;
    for (int j = 0; j < JR_NRES && units; j++)
        r->units[j] = units[j];
    seal(r);
}

/* 1 = a good record, 0 = end of file, -1 = torn (short) or bad CRC. */
static int read_record(int fd, jr_record_t *r)
{
    size_t got = 0;
    while (got < sizeof *r) {
        ssize_t n = read(fd, (char *)r + got, sizeof *r - got);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            break;
        got += (size_t)n;
    }
    if (got == 0)
        return 0;
    return got == sizeof *r && r->crc == jr_crc32(r, CRC_LEN) ? 1 : -1;
}

static int write_all(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* Write the state to snapshot.tmp, fsync, rename over snapshot.bin, fsync the directory, then
 * truncate the journal: every record up to j->seq is now in the snapshot. */
static int checkpoint(journal_t *j)
{
    size_t n = 1 + (size_t)j->st.nclaim + (size_t)j->st.ngrant, k = 0;
    jr_record_t *buf = malloc(n * sizeof *buf);
    int fd = -1, rc = -1, saved;

    if (!buf)
        return -1;
    make(&buf[k++], j->seq, JR_SNAPSHOT, 0, (uint32_t)(n - 1), NULL);
    for (int i = 0; i < j->st.nclaim; i++)
        make(&buf[k++], j->seq, JR_CLAIM, j->st.claim[i].camp, 0, j->st.claim[i].max);
    for (int i = 0; i < j->st.ngrant; i++)
        make(&buf[k++], j->seq, JR_GRANT, j->st.grant[i].camp, j->st.grant[i].id, j->st.grant[i].units);
    fd = open(j->tmppath, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (fd >= 0 && write_all(fd, buf, n * sizeof *buf) == 0 && fsync(fd) == 0 && close(fd) == 0) {
        fd = -1;
        int dfd = rename(j->tmppath, j->snappath) == 0 ? open(j->dir, O_RDONLY | O_DIRECTORY) : -1;
        if (dfd >= 0) {
            if (fsync(dfd) == 0 && ftruncate(j->fd, 0) == 0 && fsync(j->fd) == 0)
                rc = 0;
            close(dfd);
        }
    }
    saved = errno;
    if (fd >= 0)
        close(fd);
    free(buf);
    if (rc == -1) {
        EV_ERROR("checkpoint_failed", "seq=%llu errno=%d", (unsigned long long)j->seq, saved);
        errno = saved;
        return -1;
    }
    j->bytes = 0;
    j->since_ckpt = 0;
    j->checkpoints++;
    EV_INFO("checkpoint", "seq=%llu camps=%d grants=%d bytes=%zu file=%s", (unsigned long long)j->seq,
            j->st.nclaim, j->st.ngrant, n * sizeof *buf, JR_SNAPSHOT_FILE);
    return 0;
}

/* Load snapshot.bin (if any) into j->st. 0, or -1 if it exists but is unreadable or corrupt. */
static int load_snapshot(journal_t *j, uint64_t *snap_seq)
{
    jr_record_t r;
    int fd = open(j->snappath, O_RDONLY), ok = 0;

    *snap_seq = 0;
    if (fd < 0)
        return errno == ENOENT ? 0 : -1;
    if (read_record(fd, &r) == 1 && r.type == JR_SNAPSHOT) {
        uint32_t n = r.id, i;
        *snap_seq = r.seq;
        for (i = 0; i < n; i++)
            if (read_record(fd, &r) != 1 || (r.type != JR_CLAIM && r.type != JR_GRANT) ||
                jr_apply(&j->st, &r) == -1)
                break;
        ok = i == n && read_record(fd, &r) == 0;
    }
    close(fd);
    return ok ? 0 : -1;
}

journal_t *journal_open(const char *dir, const jr_state_t **state, jr_recovery_t *rec)
{
    journal_t *j = calloc(1, sizeof *j);
    jr_record_t r;
    struct stat sb;

    if (!j)
        return NULL;
    memset(rec, 0, sizeof *rec);
    j->fd = -1;
    if ((size_t)snprintf(j->dir, sizeof j->dir, "%s", dir) >= sizeof j->dir ||
        (size_t)snprintf(j->logpath, sizeof j->logpath, "%s/%s", dir, JR_LOG_FILE) >= sizeof j->logpath ||
        (size_t)snprintf(j->snappath, sizeof j->snappath, "%s/%s", dir, JR_SNAPSHOT_FILE) >= sizeof j->snappath ||
        (size_t)snprintf(j->tmppath, sizeof j->tmppath, "%s/snapshot.tmp", dir) >= sizeof j->tmppath) {
        LOG_ERROR("journal: path too long: %s", dir);
        goto fail;
    }
    if (load_snapshot(j, &rec->snapshot_seq) == -1) {
        EV_ERROR("journal_corrupt", "file=%s reason=bad_snapshot", j->snappath);
        goto fail;
    }
    j->seq = rec->snapshot_seq;

    /* Replay: a torn or bad-CRC record, a gap in seq or a record that does not fit the state
     * ends it (only the last write can be torn: every earlier one was fsync'ed). */
    int fd = open(j->logpath, O_RDONLY);
    if (fd >= 0) {
        off_t off = 0, size = fstat(fd, &sb) == 0 ? sb.st_size : 0;
        int got;
        while ((got = read_record(fd, &r)) == 1) {
            if (r.seq > j->seq) {
                if (r.seq != j->seq + 1 || r.type == JR_SNAPSHOT || jr_apply(&j->st, &r) == -1)
                    break;
                j->seq = r.seq;
                rec->records++;
            }                       /* else already in the snapshot */
            off += (off_t)sizeof r;
        }
        rec->skipped_bytes = (long)(size - off);
        close(fd);
    } else if (errno != ENOENT) {
        EV_ERROR("journal_open_failed", "file=%s errno=%d", j->logpath, errno);
        goto fail;
    }
    rec->last_seq = j->seq;

    j->fd = open(j->logpath, O_WRONLY | O_CREAT | O_APPEND, 0640);
    if (j->fd < 0 || checkpoint(j) == -1) {     /* recovered state durable, torn tail gone */
        EV_ERROR("journal_open_failed", "file=%s errno=%d", j->logpath, errno);
        goto fail;
    }
    j->checkpoints = 0;
    *state = &j->st;
    return j;
fail:
    if (j->fd >= 0)
        close(j->fd);
    jr_state_free(&j->st);
    free(j);
    return NULL;
}

int journal_append(journal_t *j, jr_type_t type, uint32_t camp, uint32_t id, const int units[JR_NRES],
                   uint64_t *seq)
{
    jr_record_t r;

    if (j->since_ckpt >= JR_CHECKPOINT_EVERY && checkpoint(j) == -1)
        return -1;
    make(&r, j->seq + 1, type, camp, id, units);
    if (apply(&j->st, &r, 1) == -1) {
        errno = EINVAL;
        return -1;
    }
    int64_t t0 = mono_ns();
    if (write_all(j->fd, &r, sizeof r) == -1 || fsync(j->fd) == -1) {
        int saved = errno;
        if (ftruncate(j->fd, j->bytes) == -1)   /* drop a partial record so later ones replay */
            EV_ERROR("journal_truncate_failed", "errno=%d", errno);
        errno = saved;
        return -1;
    }
    int64_t dt = mono_ns() - t0;
    j->sync_ns += dt;
    if (dt > j->sync_max_ns)
        j->sync_max_ns = dt;
    apply(&j->st, &r, 0);           /* durable: now commit it to the state */
    j->bytes += (off_t)sizeof r;
    j->seq = r.seq;
    j->since_ckpt++;
    j->appended++;
    *seq = r.seq;
    return 0;
}

void journal_close(journal_t *j)
{
    if (!j)
        return;
    checkpoint(j);
    close(j->fd);
    EV_INFO("journal_stop", "records=%llu last_seq=%llu checkpoints=%llu fsync_avg_us=%.1f fsync_max_us=%.1f "
            "camps=%d grants=%d", (unsigned long long)j->appended, (unsigned long long)j->seq,
            (unsigned long long)j->checkpoints, j->appended ? (double)j->sync_ns / (double)j->appended / 1e3 : 0.0,
            (double)j->sync_max_ns / 1e3, j->st.nclaim, j->st.ngrant);
    jr_state_free(&j->st);
    free(j);
}
