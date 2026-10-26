/*
 * OS concept: File-system implementation — a write-ahead journal with fsync, checkpointing by
 *             atomic rename, crash recovery by replay — Unit 5
 *
 * reliefd's Banker's allocations (who claimed what, which grants are held) survive a crash:
 *   - Every change is first appended to $RELIEF_HOME/journal.log as one fixed-size record and
 *     made durable with write() + fsync(); only then is it acknowledged (the camp is told).
 *   - Every JR_CHECKPOINT_EVERY records the whole state is written to snapshot.tmp, fsync'ed,
 *     renamed over snapshot.bin (rename() is atomic: a reader sees the old or the new file,
 *     never half of one) and the journal is truncated.
 *   - On startup the snapshot is loaded and the journal replayed. A truncated or bad-CRC record
 *     ends the replay (a torn last write); records already in the snapshot are skipped by seq.
 *     The recovered state is checkpointed at once, which also drops a torn tail.
 * The journal keeps its own copy of the committed state (jr_state_t), so a checkpoint needs
 * nothing from the caller. src/io is not on the include path: include it as "../io/journal.h".
 *
 * On-disk record (40 bytes, little-endian, the same layout in journal.log and snapshot.bin):
 *   u64 seq | u32 type | u32 camp | u32 id | i32 units[4] | u32 crc32 (of the first 36 bytes)
 * units[] is one count per Banker's resource (TRUCK, BOAT, MED_KIT, FOOD_KIT), so a bundle
 * (a whole task's vector) is one record and stays atomic. snapshot.bin starts with a
 * JR_SNAPSHOT record (seq = last record covered, id = number of records that follow), then one
 * CLAIM per camp and one GRANT per held grant.
 */
#ifndef RELIEF_JOURNAL_H
#define RELIEF_JOURNAL_H

#include <stdint.h>

#define JR_NRES              4      /* = RELIEF_BANKER_RES */
#define JR_MAX_CAMPS         64     /* = BANKER_MAX_P */
#define JR_CHECKPOINT_EVERY  100
#define JR_LOG_FILE          "journal.log"
#define JR_SNAPSHOT_FILE     "snapshot.bin"

typedef enum {
    JR_CLAIM = 1,       /* camp declares max claim units[] (HELLO) */
    JR_GRANT,           /* camp is granted units[] for request id */
    JR_RELEASE,         /* camp hands back the grant of request id (or it is revoked) */
    JR_RELEASE_ALL,     /* camp is gone: all its grants and its claim are dropped */
    JR_SNAPSHOT         /* snapshot.bin header only */
} jr_type_t;

typedef struct {
    uint64_t seq;
    uint32_t type, camp, id;
    int32_t  units[JR_NRES];
    uint32_t crc;
} jr_record_t;
_Static_assert(sizeof(jr_record_t) == 40, "journal record is 40 bytes on disk");

typedef struct { uint32_t camp; int max[JR_NRES]; } jr_claim_t;
typedef struct { uint32_t camp, id; int units[JR_NRES]; } jr_grant_t;

/* The committed allocation state. */
typedef struct {
    jr_claim_t  claim[JR_MAX_CAMPS];
    int         nclaim;
    jr_grant_t *grant;
    int         ngrant, cap;
} jr_state_t;

typedef struct {
    uint64_t records;           /* replayed from the journal (after the snapshot) */
    uint64_t snapshot_seq;      /* 0 = no snapshot */
    uint64_t last_seq;
    long     skipped_bytes;     /* a torn or bad-CRC tail that was dropped */
} jr_recovery_t;

typedef struct journal journal_t;

uint32_t jr_crc32(const void *buf, unsigned long len);     /* IEEE 802.3, as zlib.crc32 */

/* Apply one record to a state. 0, or -1 if it does not fit the state (unknown camp or grant). */
int  jr_apply(jr_state_t *s, const jr_record_t *r);
void jr_state_free(jr_state_t *s);

/* Recover from dir (snapshot + journal), checkpoint, open the journal for appending.
 * *state is the recovered state (owned by the journal; valid until journal_close).
 * NULL on error (logged), e.g. an unreadable or corrupt snapshot. */
journal_t *journal_open(const char *dir, const jr_state_t **state, jr_recovery_t *rec);

/* Append one record durably (write + fsync), checkpointing first every JR_CHECKPOINT_EVERY
 * records. *seq = its sequence number. 0, or -1 (errno) if it could not be made durable or
 * does not fit the committed state; the committed state is then unchanged. */
int journal_append(journal_t *j, jr_type_t type, uint32_t camp, uint32_t id, const int units[JR_NRES],
                   uint64_t *seq);

/* Checkpoint, close, log the totals and free. NULL is ignored. */
void journal_close(journal_t *j);

#endif /* RELIEF_JOURNAL_H */
