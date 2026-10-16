/*
 * OS concept: Inter-process communication (POSIX message queues), shared wire format — Unit 2
 *
 * Everything that crosses a process boundary between reliefd, camp and
 * future clients is defined here and nowhere else. All fields use fixed-width
 * types so the layout is identical in every process.
 */
#ifndef RELIEF_H
#define RELIEF_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#define RELIEF_MQ_NAME      "/reliefos_requests"
#define RELIEF_MQ_MAXMSG    10          /* non-root limit: /proc/sys/fs/mqueue/msg_max */
#define RELIEF_MSG_SIZE     128
#define RELIEF_HOME_DEFAULT "./run"
#define RELIEF_PID_FILE     "reliefd.pid"
#define RELIEF_LOG_DIR      "logs"          /* under $RELIEF_HOME */
#define RELIEF_LOG_FILE     "reliefd.log"   /* structured key=value log */
#define RELIEF_CAMPS_PIDS   "camps.pids"    /* camp table for reliefctl: "<id> <pid> <state> <name>" */

/* Request ids are camp_id * RELIEF_REQ_ID_BASE + sequence, so they are unique and reproducible. */
#define RELIEF_REQ_ID_BASE  1000000U

/* Triage severity: lower value = more severe. */
typedef enum {
    SEV_CRITICAL = 0,
    SEV_URGENT   = 1,
    SEV_ROUTINE  = 2,
    SEV_COUNT
} relief_severity_t;

typedef enum {
    RES_RESCUE      = 0,
    RES_FOOD_KIT    = 1,
    RES_MED_KIT     = 2,
    RES_HYGIENE_KIT = 3,
    RES_TRUCK       = 4,
    RES_BOAT        = 5,
    RES_COUNT
} relief_resource_t;

typedef enum {
    MSG_HELLO    = 1,
    MSG_REQUEST  = 2,
    MSG_BYE      = 3,
    MSG_SHUTDOWN = 4
} relief_msg_kind_t;

/* relief_request_t.flags */
#define REQ_FLAG_ALERT      0x01    /* escalated to CRITICAL by an evacuation alert (SIGUSR1) */

/* One rescue/supply request; times are in simulated minutes. */
typedef struct {
    uint32_t id;
    uint32_t camp_id;
    int32_t  camp_pid;
    uint8_t  severity;      /* relief_severity_t */
    uint8_t  resource;      /* relief_resource_t */
    uint16_t quantity;
    uint32_t service_time;
    uint32_t arrival;
    uint8_t  flags;         /* REQ_FLAG_* */
    uint8_t  orig_severity; /* severity before any escalation */
    uint16_t _pad;
} relief_request_t;

/* Fixed-size message placed on the queue. */
typedef struct {
    uint32_t kind;          /* relief_msg_kind_t */
    uint32_t camp_id;
    int32_t  camp_pid;
    uint32_t _pad;
    union {
        relief_request_t req;   /* valid when kind == MSG_REQUEST */
        uint8_t raw[RELIEF_MSG_SIZE - 16];
    } u;
} relief_msg_t;

_Static_assert(sizeof(relief_msg_t) == RELIEF_MSG_SIZE, "relief_msg_t must be fixed-size");
_Static_assert(sizeof(relief_request_t) == 28, "relief_request_t layout changed: update all senders");

static inline const char *relief_severity_name(unsigned s)
{
    static const char *const names[SEV_COUNT] = { "CRITICAL", "URGENT", "ROUTINE" };
    return s < SEV_COUNT ? names[s] : "?";
}

static inline const char *relief_resource_name(unsigned r)
{
    static const char *const names[RES_COUNT] = {
        "RESCUE", "FOOD_KIT", "MED_KIT", "HYGIENE_KIT", "TRUCK", "BOAT"
    };
    return r < RES_COUNT ? names[r] : "?";
}

/* Name -> enum value (as used in scenario CSVs); -1 if unknown. */
static inline int relief_severity_parse(const char *s)
{
    for (unsigned i = 0; i < SEV_COUNT; i++)
        if (strcmp(s, relief_severity_name(i)) == 0)
            return (int)i;
    return -1;
}

static inline int relief_resource_parse(const char *s)
{
    for (unsigned i = 0; i < RES_COUNT; i++)
        if (strcmp(s, relief_resource_name(i)) == 0)
            return (int)i;
    return -1;
}

/* ---- Live status table in POSIX shared memory (Unit 2: IPC, Unit 3: synchronisation) ----
 *
 * reliefd creates RELIEF_SHM_NAME and owns the daemon fields plus each camp's pid, state
 * and restarts; each camp updates only requests_sent and heartbeat_ms in its own row.
 * Writers hold `lock` (process-shared, robust) and bump `seq` around every write;
 * read-only readers (reliefstat) cannot lock, so they use `seq` as a seqlock.
 * See include/shm.h for the access functions.
 */
#define RELIEF_SHM_NAME      "/reliefos_status"
#define RELIEF_SHM_MAGIC     0x534c4552u     /* "RELS" */
#define RELIEF_SHM_VERSION   1
#define RELIEF_SHM_CAMPS     64
#define RELIEF_CAMP_NAME_MAX 32

/* Camp lifecycle state, owned by reliefd. */
typedef enum {
    CAMP_STARTING = 0,  /* forked, no HELLO received yet */
    CAMP_RUNNING,       /* HELLO received from this pid */
    CAMP_EXITED,        /* terminated normally (or during shutdown) */
    CAMP_CRASHED,       /* killed by a signal more than the restart limit */
    CAMP_STATE_COUNT
} relief_camp_state_t;

static inline const char *relief_camp_state_name(unsigned s)
{
    static const char *const names[CAMP_STATE_COUNT] = { "STARTING", "RUNNING", "EXITED", "CRASHED" };
    return s < CAMP_STATE_COUNT ? names[s] : "?";
}

typedef struct {
    uint32_t id;                            /* reliefd */
    char     name[RELIEF_CAMP_NAME_MAX];    /* reliefd */
    int32_t  pid;                           /* reliefd */
    uint32_t state;                         /* reliefd: relief_camp_state_t */
    uint32_t restarts;                      /* reliefd */
    uint64_t requests_sent;                 /* camp (reset by reliefd on each (re)start) */
    int64_t  heartbeat_ms;                  /* camp: CLOCK_REALTIME in ms, 0 = none yet */
} relief_shm_camp_t;

typedef struct {
    int32_t  daemon_pid;
    uint32_t ncamps;
    int64_t  start_time_ms;                 /* CLOCK_REALTIME in ms */
    uint64_t total_requests;                /* REQUEST messages received by reliefd */
    relief_shm_camp_t camps[RELIEF_SHM_CAMPS];
} relief_status_t;

typedef struct {
    uint32_t         magic;                 /* RELIEF_SHM_MAGIC once initialised */
    uint32_t         version;
    _Atomic uint32_t seq;                   /* seqlock generation: odd while a writer is inside */
    pthread_mutex_t  lock;                  /* PTHREAD_PROCESS_SHARED + PTHREAD_MUTEX_ROBUST */
    relief_status_t  data;
} relief_shm_t;

static inline const char *relief_msg_kind_name(unsigned k)
{
    switch (k) {
    case MSG_HELLO:    return "HELLO";
    case MSG_REQUEST:  return "REQUEST";
    case MSG_BYE:      return "BYE";
    case MSG_SHUTDOWN: return "SHUTDOWN";
    default:           return "?";
    }
}

#endif /* RELIEF_H */
