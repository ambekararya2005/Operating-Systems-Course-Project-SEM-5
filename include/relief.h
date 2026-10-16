/*
 * OS concept: Inter-process communication (POSIX message queues), shared wire format — Unit 2
 *
 * Everything that crosses a process boundary between reliefd, camp and
 * future clients is defined here and nowhere else. All fields use fixed-width
 * types so the layout is identical in every process.
 */
#ifndef RELIEF_H
#define RELIEF_H

#include <stdint.h>
#include <string.h>

#define RELIEF_MQ_NAME      "/reliefos_requests"
#define RELIEF_MQ_MAXMSG    10          /* non-root limit: /proc/sys/fs/mqueue/msg_max */
#define RELIEF_MSG_SIZE     128
#define RELIEF_HOME_DEFAULT "./run"
#define RELIEF_PID_FILE     "reliefd.pid"
#define RELIEF_LOG_DIR      "logs"          /* under $RELIEF_HOME */
#define RELIEF_LOG_FILE     "reliefd.log"   /* structured key=value log */

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
