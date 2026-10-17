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
#define RELIEF_REPLY_MQ_FMT "/reliefos_camp_%u"  /* per-camp reply queue (reliefd -> camp), by camp id */
#define RELIEF_RESOURCES_CONF "scenarios/resources.conf"

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
    MSG_HELLO    = 1,       /* camp -> reliefd: u.hello (maximum claim) */
    MSG_REQUEST  = 2,       /* camp -> reliefd: u.req */
    MSG_BYE      = 3,       /* camp -> reliefd */
    MSG_SHUTDOWN = 4,       /* admin -> reliefd */
    MSG_RELEASE  = 5,       /* camp -> reliefd: u.req (id, resource, units) handed back */
    MSG_GRANT    = 6,       /* reliefd -> camp reply queue: u.req of the granted request */
    MSG_REJECT   = 7,       /* reliefd -> camp reply queue: u.req of a request that exceeds the claim */
    MSG_KIND_COUNT
} relief_msg_kind_t;

/* ---- Banker's algorithm resources (Phase 3, deadlock avoidance) ----------------------------
 * Four of the request resources are managed by the Banker's algorithm, in this order (the
 * column order of every Allocation/Max/Need/Available vector). Vehicles count one unit per
 * vehicle; kits count in crates of RELIEF_KITS_PER_CRATE kits. Totals come from
 * scenarios/resources.conf (defaults below). */
#define RELIEF_BANKER_RES       4
#define RELIEF_KITS_PER_CRATE   10
enum { BK_TRUCK = 0, BK_BOAT = 1, BK_MED_KIT = 2, BK_FOOD_KIT = 3 };
#define RELIEF_BANKER_DEFAULT_TOTALS { 3, 2, 40, 60 }

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
    uint16_t units;         /* Banker's units asked for (vehicles, kit crates); 0 = not a Banker resource */
} relief_request_t;

/* HELLO payload: the camp's maximum claim per Banker's resource, declared up front. */
#define HELLO_FLAG_REPLY_QUEUE  0x01    /* the camp reads grants on its RELIEF_REPLY_MQ_FMT queue */
typedef struct {
    uint16_t max_claim[RELIEF_BANKER_RES];
    uint32_t flags;         /* HELLO_FLAG_* */
} relief_hello_t;

/* Fixed-size message placed on the queue. */
typedef struct {
    uint32_t kind;          /* relief_msg_kind_t */
    uint32_t camp_id;
    int32_t  camp_pid;
    uint32_t _pad;
    union {
        relief_request_t req;   /* MSG_REQUEST, MSG_RELEASE, MSG_GRANT, MSG_REJECT */
        relief_hello_t   hello; /* MSG_HELLO */
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

static inline const char *relief_banker_name(unsigned b)
{
    static const char *const names[RELIEF_BANKER_RES] = { "TRUCK", "BOAT", "MED_KIT", "FOOD_KIT" };
    return b < RELIEF_BANKER_RES ? names[b] : "?";
}

/* Banker's column of a request resource, or -1 if the resource is not Banker-managed. */
static inline int relief_banker_index(unsigned resource)
{
    switch (resource) {
    case RES_TRUCK:    return BK_TRUCK;
    case RES_BOAT:     return BK_BOAT;
    case RES_MED_KIT:  return BK_MED_KIT;
    case RES_FOOD_KIT: return BK_FOOD_KIT;
    default:           return -1;
    }
}

/* Banker's units for a request of `quantity` (vehicles 1:1, kits rounded up to whole crates). */
static inline unsigned relief_banker_units(unsigned resource, unsigned quantity)
{
    if (resource == RES_MED_KIT || resource == RES_FOOD_KIT)
        return (quantity + RELIEF_KITS_PER_CRATE - 1) / RELIEF_KITS_PER_CRATE;
    return quantity;
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
#define RELIEF_SHM_VERSION   4
#define RELIEF_SHM_CAMPS     64
#define RELIEF_CAMP_NAME_MAX 32
#define RELIEF_SHM_TEAMS     16     /* max rescue team threads (reliefd --teams) */
#define RELIEF_SHM_LEVELS    3      /* ready-queue levels shown (MLFQ Q0..Q2, prio severities) */
#define RELIEF_POLICY_MAX    32
#define RELIEF_WAREHOUSES    2      /* inventory: camp c is served by warehouse (c - 1) % 2 */
#define RELIEF_KIT_KINDS     3      /* FOOD_KIT, MED_KIT, HYGIENE_KIT, in that order */

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

/* One rescue team thread of reliefd's live scheduler (Phase 2). Written by reliefd only. */
typedef struct {
    uint32_t state;                         /* 0 = idle, 1 = busy */
    uint32_t job_id;                        /* request being worked on (busy) */
    uint32_t camp;
    uint8_t  severity;                      /* relief_severity_t */
    uint8_t  level;                         /* policy queue level of the job (MLFQ) */
    uint16_t _pad;
    int64_t  remaining;                     /* simulated minutes of work left */
    int64_t  busy_since_ms;                 /* CLOCK_REALTIME ms of this dispatch */
    uint64_t completed;                     /* requests this team has finished */
} relief_shm_team_t;

typedef struct {
    int32_t  daemon_pid;
    uint32_t ncamps;
    int64_t  start_time_ms;                 /* CLOCK_REALTIME in ms */
    uint64_t total_requests;                /* REQUEST messages received by reliefd */
    relief_shm_camp_t camps[RELIEF_SHM_CAMPS];
    /* Live scheduler (reliefd dispatcher, Phase 2) */
    char     policy[RELIEF_POLICY_MAX];
    uint32_t nteams;
    uint32_t nlevels;                       /* entries used in queue_len */
    uint64_t queue_len[RELIEF_SHM_LEVELS];  /* ready requests per level, highest first */
    uint64_t sched_enqueued, sched_completed, sched_preemptions, sched_promotions;
    relief_shm_team_t teams[RELIEF_SHM_TEAMS];
    /* Loading dock: kit packages, warehouse producers -> trucks (Phase 3, bounded buffer) */
    uint32_t dock_capacity, dock_occupancy, dock_max_occupancy, dock_producers, dock_trucks;
    uint32_t _pad2;
    uint64_t dock_orders, dock_produced, dock_consumed, dock_delivered;
    uint64_t dock_blocked_puts;             /* puts that found the dock full */
    double   dock_blocked_min;              /* simulated minutes producers spent blocked on it */
    /* Inventory (Phase 3, readers-writers): stock per warehouse and kit type */
    char     inv_lock[16];                  /* readers-writers implementation: semaphore|rwlock */
    int64_t  inventory[RELIEF_WAREHOUSES][RELIEF_KIT_KINDS];
    /* Deadlock handling (Phase 3): the Banker's state, one row per process (camp) slot */
    char     dl_mode[16];                   /* --deadlock: avoid | none | prevent | detect */
    uint32_t bk_nproc;                      /* rows used in bk_proc */
    int32_t  bk_total[RELIEF_BANKER_RES];
    int32_t  bk_available[RELIEF_BANKER_RES];
    struct {
        uint32_t camp;                      /* camp id, shown as P<camp> */
        uint32_t active;                    /* 1 between HELLO and BYE/exit */
        int32_t  max[RELIEF_BANKER_RES];
        int32_t  alloc[RELIEF_BANKER_RES];
        uint32_t pending;                   /* requests of this camp waiting in reliefd */
    } bk_proc[RELIEF_SHM_CAMPS];
    uint64_t bk_grants, bk_denied_unsafe, bk_waits_unavailable, bk_releases, bk_rejects;
    uint32_t bk_pending;                    /* requests waiting (unsafe or unavailable) */
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
    case MSG_RELEASE:  return "RELEASE";
    case MSG_GRANT:    return "GRANT";
    case MSG_REJECT:   return "REJECT";
    default:           return "?";
    }
}

#endif /* RELIEF_H */
