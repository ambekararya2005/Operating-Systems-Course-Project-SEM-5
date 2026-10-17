/*
 * OS concept: Process as an IPC client (POSIX message queues) and signals as IPC — Unit 2
 *
 * A relief camp. Two modes:
 *   --scenario FILE  (how reliefd starts camps) replay the rows of the scenario CSV whose
 *                    camp_id matches --id, each at its arrival time on a simulated clock
 *                    (--time-scale ms of real time per simulated minute, default 100;
 *                    0 = no pacing), then stay resident until SIGTERM/SIGINT, send BYE, exit 0.
 *                    With --exit-when-done, send BYE and exit 0 as soon as the replay ends.
 *   --count N        (manual/testing) send N random requests (deterministic for --seed
 *                    and --id) immediately, then BYE, and exit.
 * Both send HELLO first, with the camp's maximum claim of each Banker's resource (TRUCK, BOAT,
 * MED_KIT and FOOD_KIT crates, see include/relief.h). --shutdown instead sends a single
 * SHUTDOWN message (admin/test hook).
 *
 * Deadlock avoidance (Unit 3, scenario mode): the claim is the camp's peak concurrent demand
 * (every row held from its arrival for service_time minutes), capped at the totals of
 * --resources (default scenarios/resources.conf). The camp creates its reply queue
 * RELIEF_REPLY_MQ_FMT, on which reliefd answers MSG_GRANT or MSG_REJECT. A granted request is
 * held for service_time simulated minutes, then handed back with MSG_RELEASE (the truck or
 * boat is back, the crates are returned). The camp never asks for more than its claim: a
 * request that would take it over waits in the camp until its own releases make room. With
 * --exit-when-done it says BYE only once everything granted has been released.
 * Sends never hang on a full queue: mq_timedsend retries with a "queue full, retrying" warning.
 *
 * Signals (sent by `reliefctl alert|dump` or by reliefd):
 *   SIGUSR1  evacuation alert: the next ALERT_ESCALATIONS requests are sent as CRITICAL
 *   SIGUSR2  status dump: log counters (sent, by severity, escalations, uptime)
 *   SIGTERM  graceful exit: finish the send in progress, send BYE, exit 0 (SIGINT likewise)
 *   SIGALRM  1 s POSIX timer: heartbeat into the shared status table
 *
 * Live status (Unit 2/3): the camp attaches to reliefd's POSIX shared memory table and
 * updates only its own row (requests_sent, heartbeat_ms) under the process-shared mutex.
 */
#include "log.h"
#include "relief.h"
#include "resconf.h"
#include "rng.h"
#include "shm.h"

#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <mqueue.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define SEND_RETRY_MS        500     /* mq_timedsend deadline before warning and retrying */
#define BYE_MAX_TRIES        3       /* BYE/SHUTDOWN give up after this (reliefd may be gone) */
#define STOP_SEND_TRIES      3       /* extra attempts to finish a send after SIGTERM */
#define DEFAULT_TIME_SCALE   100     /* ms of real time per simulated minute */
#define ALERT_ESCALATIONS    5       /* requests escalated to CRITICAL per SIGUSR1 */
#define HEARTBEAT_SEC        1       /* status table heartbeat period */

/*
 * Signal handlers only set these flags; the main loop does the real work.
 *
 * Why not printf/malloc (or LOG_*) inside a handler? A signal can arrive at any
 * instruction, including while the main flow is itself inside printf or malloc.
 * At that moment stdio holds its internal lock / half-filled buffer and malloc's
 * free lists may be mid-update. Re-entering them from the handler would deadlock on
 * the lock (our logger also takes a pthread mutex) or corrupt the heap/buffers.
 * POSIX only guarantees "async-signal-safe" functions (write, _exit, kill, ...) inside
 * handlers; printf and malloc are not among them. Writing a volatile sig_atomic_t is
 * the one thing a handler can always do safely.
 */
static volatile sig_atomic_t g_stop = 0;    /* SIGTERM/SIGINT: signal number */
static volatile sig_atomic_t g_alert = 0;   /* SIGUSR1 pending */
static volatile sig_atomic_t g_dump = 0;    /* SIGUSR2 pending */
static volatile sig_atomic_t g_tick = 0;    /* SIGALRM: heartbeat due */

static void on_stop(int sig)  { g_stop = sig; }
static void on_alert(int sig) { (void)sig; g_alert = 1; }
static void on_dump(int sig)  { (void)sig; g_dump = 1; }
static void on_tick(int sig)  { (void)sig; g_tick = 1; }

/* Camp state; touched only by the main flow, never by handlers. */
typedef struct {
    mqd_t           mq;
    relief_msg_t    msg;
    uint32_t        id;
    struct timespec started;
    unsigned long   sent;
    unsigned long   by_sev[SEV_COUNT];
    unsigned long   escalated;          /* requests sent CRITICAL because of an alert */
    unsigned long   alerts;
    unsigned long   queue_full_retries;
    unsigned        escalate_left;      /* remaining escalations from the latest alert */
    long            rows_total;         /* scenario rows for this camp; -1 in random mode */
    relief_shm_t   *shm;                /* live status table, NULL if unavailable */
    int             shm_row;            /* cached index of our row, -1 = not found yet */
    timer_t         heartbeat_timer;
    int             have_timer;
    /* Banker's resources */
    mqd_t           reply;              /* reliefd -> camp: MSG_GRANT / MSG_REJECT */
    int             have_reply;
    char            reply_name[64];
    int             total[RELIEF_BANKER_RES];
    int             claim[RELIEF_BANKER_RES];
    unsigned long   granted, released, rejected, deferred;
} camp_ctx_t;

static void usage(const char *prog)
{
    fprintf(stderr,
            "usage: %s --id N --scenario FILE [--time-scale MS] [--seed S] [--exit-when-done]\n"
            "          [--resources FILE] [--all-or-nothing]\n"
            "          (resident until SIGTERM, or until the replay ends with --exit-when-done)\n"
            "       %s --id N [--count N] [--seed S]   (send N random requests and exit)\n"
            "       %s --shutdown\n"
            "  --time-scale MS  real milliseconds per simulated minute (default %d, 0 = no pacing)\n"
            "  --resources FILE Banker's totals that cap the declared claim (default: built-in 3/2/40/60)\n"
            "  --all-or-nothing request each task (rows sharing the optional 7th CSV column) in one\n"
            "                   message (reliefd passes it with --deadlock prevent --all-or-nothing)\n"
            "signals: SIGUSR1 = evacuation alert, SIGUSR2 = status dump, SIGTERM = graceful exit\n",
            prog, prog, prog, DEFAULT_TIME_SCALE);
}

static int parse_ulong(const char *s, unsigned long max, unsigned long *out)
{
    char *end;
    unsigned long v;

    errno = 0;
    v = strtoul(s, &end, 10);
    if (errno || end == s || *end || v > max)
        return -1;
    *out = v;
    return 0;
}

/*
 * SA_RESTART on every camp handler: a signal arriving while mq_timedsend waits for
 * queue space must not abort that send, so the kernel restarts it transparently
 * ("finish the current send"). Sleeps still wake immediately, because
 * clock_nanosleep() and sigsuspend() are never restarted (they return EINTR), so
 * alerts, dumps and SIGTERM are handled promptly while the camp is idle.
 */
static void install_signal_handlers(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof sa);
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;

    sa.sa_handler = on_stop;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT, &sa, NULL);
    sa.sa_handler = on_alert;
    sigaction(SIGUSR1, &sa, NULL);
    sa.sa_handler = on_dump;
    sigaction(SIGUSR2, &sa, NULL);
    sa.sa_handler = on_tick;
    sigaction(SIGALRM, &sa, NULL);
}

static long uptime_ms(const camp_ctx_t *c)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - c->started.tv_sec) * 1000L +
           (now.tv_nsec - c->started.tv_nsec) / 1000000L;
}

/*
 * Write our counters into our row of the shared status table. The row is found by
 * (id, pid): reliefd fills in the pid right after fork(), so a freshly started camp may
 * not see its row on the first try and simply retries on the next heartbeat. Matching
 * the pid also stops a manually started camp from overwriting a supervised camp's row.
 */
static void status_update(camp_ctx_t *c)
{
    relief_shm_camp_t *rows;
    int32_t self = (int32_t)getpid();
    int rc;

    if (!c->shm || (rc = relief_shm_begin_write(c->shm)) == -1)
        return;
    if (rc == 1)    /* logging is fine here: we are in the main flow, not a handler */
        log_event(LOG_LVL_WARN, "status_lock_recovered", "by=camp camp=%u reason=EOWNERDEAD", c->id);
    rows = c->shm->data.camps;
    if (c->shm_row < 0 || rows[c->shm_row].id != c->id || rows[c->shm_row].pid != self) {
        c->shm_row = -1;
        for (uint32_t i = 0; i < c->shm->data.ncamps && i < RELIEF_SHM_CAMPS; i++) {
            if (rows[i].id == c->id && rows[i].pid == self) {
                c->shm_row = (int)i;
                break;
            }
        }
    }
    if (c->shm_row >= 0) {
        rows[c->shm_row].requests_sent = c->sent;
        rows[c->shm_row].heartbeat_ms = relief_now_ms();
    }
    relief_shm_end_write(c->shm);
}

/* Attach to the status table and start the heartbeat timer (best effort). */
static void status_start(camp_ctx_t *c)
{
    struct sigevent sev;
    struct itimerspec its;

    c->shm_row = -1;
    c->shm = relief_shm_open(1);
    if (!c->shm) {
        LOG_WARN("status table %s unavailable (%s): running without live status",
                 RELIEF_SHM_NAME, strerror(errno));
        return;
    }
    memset(&sev, 0, sizeof sev);
    sev.sigev_notify = SIGEV_SIGNAL;
    sev.sigev_signo = SIGALRM;
    if (timer_create(CLOCK_MONOTONIC, &sev, &c->heartbeat_timer) == -1) {
        LOG_WARN("timer_create: %s: no heartbeat", strerror(errno));
        return;
    }
    c->have_timer = 1;
    memset(&its, 0, sizeof its);
    its.it_value.tv_sec = HEARTBEAT_SEC;
    its.it_interval.tv_sec = HEARTBEAT_SEC;
    timer_settime(c->heartbeat_timer, 0, &its, NULL);
    status_update(c);
}

static void status_stop(camp_ctx_t *c)
{
    if (c->have_timer)
        timer_delete(c->heartbeat_timer);
    c->have_timer = 0;
    if (c->shm) {
        status_update(c);
        relief_shm_close(c->shm);
    }
    c->shm = NULL;
}

/* Act on pending SIGUSR1/SIGUSR2/SIGALRM. Called from the main flow only. */
static void service_signals(camp_ctx_t *c)
{
    if (g_tick) {
        g_tick = 0;
        status_update(c);
    }
    if (g_alert) {
        g_alert = 0;
        c->alerts++;
        c->escalate_left = ALERT_ESCALATIONS;
        log_event(LOG_LVL_WARN, "alert", "camp=%u signal=USR1 escalate_next=%d alerts=%lu",
                  c->id, ALERT_ESCALATIONS, c->alerts);
    }
    if (g_dump) {
        g_dump = 0;
        EV_INFO("status", "camp=%u pid=%d sent=%lu critical=%lu urgent=%lu routine=%lu "
                "escalated=%lu alerts=%lu escalate_pending=%u queue_full_retries=%lu "
                "rows_left=%ld uptime_ms=%ld",
                c->id, (int)getpid(), c->sent, c->by_sev[SEV_CRITICAL], c->by_sev[SEV_URGENT],
                c->by_sev[SEV_ROUTINE], c->escalated, c->alerts, c->escalate_left,
                c->queue_full_retries,
                c->rows_total < 0 ? -1L : c->rows_total - (long)c->sent, uptime_ms(c));
    }
}

static void deadline_after_ms(struct timespec *ts, long ms)
{
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec += ms / 1000;
    ts->tv_nsec += (ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec++;
        ts->tv_nsec -= 1000000000L;
    }
}

/*
 * Send c->msg without ever blocking indefinitely. When the queue is full,
 * mq_timedsend times out after SEND_RETRY_MS; we log and retry.
 *   max_tries == 0: retry until stopped; once SIGTERM arrives, the send in progress
 *                   still gets STOP_SEND_TRIES more attempts (graceful: finish it)
 *   max_tries  > 0: give up after that many attempts (BYE, SHUTDOWN: reliefd may be gone)
 */
static int send_msg(camp_ctx_t *c, int max_tries)
{
    const char *kind = relief_msg_kind_name(c->msg.kind);
    int limit = max_tries;

    for (int attempt = 1;; attempt++) {
        struct timespec deadline;

        service_signals(c);
        if (g_stop && limit == 0)
            limit = attempt + STOP_SEND_TRIES - 1;

        deadline_after_ms(&deadline, SEND_RETRY_MS);
        if (mq_timedsend(c->mq, (const char *)&c->msg, sizeof c->msg, 0, &deadline) == 0)
            return 0;
        if (errno != ETIMEDOUT && errno != EINTR) {
            LOG_ERROR("mq_timedsend %s: %s", kind, strerror(errno));
            return -1;
        }
        if (limit > 0 && attempt >= limit) {
            LOG_ERROR("queue full, giving up on %s after %d attempts", kind, attempt);
            return -1;
        }
        if (errno == ETIMEDOUT) {
            c->queue_full_retries++;
            LOG_WARN("queue full, retrying %s (attempt %d)", kind, attempt);
        }
    }
}

static int send_simple(camp_ctx_t *c, relief_msg_kind_t kind, int max_tries)
{
    memset(&c->msg.u, 0, sizeof c->msg.u);
    c->msg.kind = kind;
    return send_msg(c, max_tries);
}

/* Send one request, applying any pending alert escalation. bundle != NULL: an all-or-nothing
 * request for a whole task (REQ_FLAG_BUNDLE, the task's units per Banker's resource). */
static int send_request_ex(camp_ctx_t *c, const relief_request_t *req, const uint16_t *bundle,
                           uint32_t task)
{
    relief_request_t *q = &c->msg.u.req;

    service_signals(c);     /* an alert that just arrived applies to this request */
    memset(&c->msg.u, 0, sizeof c->msg.u);
    c->msg.kind = MSG_REQUEST;
    *q = *req;
    q->camp_id = c->id;
    q->camp_pid = c->msg.camp_pid;
    q->orig_severity = req->severity;
    q->flags = 0;
    c->msg.u.bundle.task = task;
    if (bundle) {
        q->flags |= REQ_FLAG_BUNDLE;
        memcpy(c->msg.u.bundle.units, bundle, sizeof c->msg.u.bundle.units);
    }
    if (c->escalate_left > 0) {
        c->escalate_left--;
        c->escalated++;
        q->severity = SEV_CRITICAL;
        q->flags |= REQ_FLAG_ALERT;
    }
    if (send_msg(c, 0) == -1)
        return -1;

    c->sent++;
    c->by_sev[q->severity]++;
    status_update(c);
    LOG_INFO("sent REQUEST id=%u severity=%s resource=%s qty=%u service=%u arrival=%u%s",
             q->id, relief_severity_name(q->severity), relief_resource_name(q->resource),
             q->quantity, q->service_time, q->arrival,
             (q->flags & REQ_FLAG_ALERT) ? " alert=1" : "");
    return 0;
}

static int send_request(camp_ctx_t *c, const relief_request_t *req)
{
    return send_request_ex(c, req, NULL, 0);
}

/* Send the random workload immediately. Returns number sent, or -1 on error. */
static long send_random(camp_ctx_t *c, unsigned long count, unsigned long seed)
{
    uint32_t arrival = 0;
    rng_t rng;

    /* Mix the camp id into the seed so camps differ but each stays reproducible. */
    rng_seed(&rng, (uint64_t)seed ^ ((uint64_t)c->id * 0x9E3779B97F4A7C15ULL));
    for (unsigned long i = 0; i < count && !g_stop; i++) {
        relief_request_t r;

        memset(&r, 0, sizeof r);
        arrival += rng_range(&rng, 0, 10);
        r.id = c->id * RELIEF_REQ_ID_BASE + (uint32_t)i + 1;
        r.severity = (uint8_t)rng_range(&rng, 0, SEV_COUNT - 1);
        r.resource = (uint8_t)rng_range(&rng, 0, RES_COUNT - 1);
        r.quantity = (uint16_t)rng_range(&rng, 1, 50);
        r.service_time = rng_range(&rng, 1, 30);
        r.arrival = arrival;
        int bi = relief_banker_index(r.resource);
        if (bi >= 0) {
            unsigned u = relief_banker_units(r.resource, r.quantity);
            r.units = (uint16_t)(u > (unsigned)c->claim[bi] ? (unsigned)c->claim[bi] : u);
        }
        if (send_request(c, &r) == -1)
            return g_stop ? (long)c->sent : -1;
    }
    return (long)c->sent;
}

typedef struct {
    relief_request_t req;
    unsigned lineno;
    uint32_t task;          /* optional 7th CSV column: rows of one task share it; 0 = none */
} scenario_row_t;

static int row_cmp(const void *a, const void *b)
{
    const scenario_row_t *x = a, *y = b;
    if (x->req.arrival != y->req.arrival)
        return x->req.arrival < y->req.arrival ? -1 : 1;
    return x->lineno < y->lineno ? -1 : (x->lineno > y->lineno);
}

/* Load the scenario rows whose camp_id == id, sorted by arrival. Returns count or -1. */
static long load_scenario(const char *path, uint32_t id, scenario_row_t **rows_out)
{
    FILE *f = fopen(path, "r");
    scenario_row_t *rows = NULL;
    size_t n = 0, cap = 0;
    char line[256];
    unsigned lineno = 0;

    if (!f) {
        LOG_ERROR("cannot open scenario %s: %s", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        unsigned arrival, camp, qty, service, task = 0;
        char sev[16], res[16];
        int s, r, nf;

        lineno++;
        if (lineno == 1 || line[0] == '#' || line[0] == '\n')
            continue;   /* header / comments / blank lines */
        nf = sscanf(line, "%u,%u,%15[^,],%15[^,],%u,%u,%u", &arrival, &camp, sev, res, &qty, &service,
                    &task);
        if ((nf != 6 && nf != 7) ||
            (s = relief_severity_parse(sev)) < 0 || (r = relief_resource_parse(res)) < 0 ||
            qty == 0 || qty > UINT16_MAX) {
            LOG_WARN("%s:%u: skipping malformed row", path, lineno);
            continue;
        }
        if (camp != id)
            continue;
        if (n == cap) {
            size_t ncap = cap ? cap * 2 : 32;
            scenario_row_t *tmp = realloc(rows, ncap * sizeof *rows);
            if (!tmp) {
                LOG_ERROR("out of memory loading scenario");
                free(rows);
                fclose(f);
                return -1;
            }
            rows = tmp;
            cap = ncap;
        }
        scenario_row_t *row = &rows[n++];
        memset(row, 0, sizeof *row);
        row->lineno = lineno;
        row->req.id = id * RELIEF_REQ_ID_BASE + lineno;   /* stable across restarts */
        row->req.severity = (uint8_t)s;
        row->req.resource = (uint8_t)r;
        row->req.quantity = (uint16_t)qty;
        row->req.service_time = service;
        row->req.arrival = arrival;
        row->task = task;
    }
    fclose(f);
    if (n > 1)
        qsort(rows, n, sizeof *rows, row_cmp);
    *rows_out = rows;
    return (long)n;
}

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* Banker's units of each row (vehicles capped at the fleet size), then the claim: for every
 * resource, the most units this camp would hold at once if each row were granted on arrival
 * and held for its service time, capped at the total. */
static void compute_claim(camp_ctx_t *c, scenario_row_t *rows, long n)
{
    for (int j = 0; j < RELIEF_BANKER_RES; j++)
        c->claim[j] = 0;
    for (long k = 0; k < n; k++) {
        relief_request_t *r = &rows[k].req;
        int bi = relief_banker_index(r->resource);
        if (bi < 0)
            continue;
        unsigned u = relief_banker_units(r->resource, r->quantity);
        r->units = (uint16_t)(u > (unsigned)c->total[bi] ? (unsigned)c->total[bi] : u);
    }
    /* A row is held from its arrival until it is released: a lone row for its service time,
     * a task's row until the task's last row has arrived plus the task's longest service time. */
    for (long k = 0; k < n; k++) {
        int bi = relief_banker_index(rows[k].req.resource), held = 0;
        if (bi < 0)
            continue;
        uint32_t t = rows[k].req.arrival;
        for (long i = 0; i < n; i++) {
            const relief_request_t *r = &rows[i].req;
            uint32_t end = r->arrival + (r->service_time ? r->service_time : 1);
            if (rows[i].task) {
                uint32_t last = 0, hold = 1;
                for (long x = 0; x < n; x++) {
                    if (rows[x].task != rows[i].task)
                        continue;
                    if (rows[x].req.arrival > last)
                        last = rows[x].req.arrival;
                    if (rows[x].req.service_time > hold)
                        hold = rows[x].req.service_time;
                }
                end = last + hold;
            }
            if (relief_banker_index(r->resource) == bi && r->arrival <= t && t < end)
                held += r->units;
        }
        if (held > c->claim[bi])
            c->claim[bi] = held;
    }
    for (int j = 0; j < RELIEF_BANKER_RES; j++)
        if (c->claim[j] > c->total[j])
            c->claim[j] = c->total[j];
}

/* Create this camp's reply queue (a stale one from a crashed run is replaced). */
static void open_reply_queue(camp_ctx_t *c)
{
    struct mq_attr attr;

    snprintf(c->reply_name, sizeof c->reply_name, RELIEF_REPLY_MQ_FMT, c->id);
    mq_unlink(c->reply_name);
    memset(&attr, 0, sizeof attr);
    attr.mq_maxmsg = RELIEF_MQ_MAXMSG;
    attr.mq_msgsize = sizeof(relief_msg_t);
    c->reply = mq_open(c->reply_name, O_CREAT | O_EXCL | O_RDONLY, 0600, &attr);
    c->have_reply = c->reply != (mqd_t)-1;
    if (!c->have_reply)
        LOG_WARN("mq_open %s: %s: requests will not be tracked", c->reply_name, strerror(errno));
}

static void close_reply_queue(camp_ctx_t *c)
{
    if (!c->have_reply)
        return;
    mq_close(c->reply);
    mq_unlink(c->reply_name);
    c->have_reply = 0;
}

static int send_hello(camp_ctx_t *c)
{
    memset(&c->msg.u, 0, sizeof c->msg.u);
    c->msg.kind = MSG_HELLO;
    for (int j = 0; j < RELIEF_BANKER_RES; j++)
        c->msg.u.hello.max_claim[j] = (uint16_t)c->claim[j];
    c->msg.u.hello.flags = c->have_reply ? HELLO_FLAG_REPLY_QUEUE : 0;
    LOG_INFO("HELLO max claim TRUCK=%d BOAT=%d MED_KIT=%d FOOD_KIT=%d crates%s", c->claim[0], c->claim[1],
             c->claim[2], c->claim[3], c->have_reply ? "" : " (no reply queue)");
    return send_msg(c, 0);
}

static int send_release(camp_ctx_t *c, const relief_request_t *req)
{
    memset(&c->msg.u, 0, sizeof c->msg.u);
    c->msg.kind = MSG_RELEASE;
    c->msg.u.req = *req;
    c->msg.u.req.camp_id = c->id;
    c->msg.u.req.camp_pid = c->msg.camp_pid;
    if (send_msg(c, 0) == -1)
        return -1;
    c->released++;
    LOG_INFO("sent RELEASE id=%u resource=%s units=%u", req->id, relief_resource_name(req->resource),
             req->units);
    return 0;
}

enum { IT_WAITING = 0, IT_DEFERRED, IT_PENDING, IT_HELD, IT_DONE };
#define TASK_MAX_ROWS   8
#define NO_TIMER        INT64_MAX

typedef struct {
    const relief_request_t *req;
    int     bi;             /* Banker's column, -1 for other resources (or no reply queue) */
    int     task;           /* index into the task table, -1 for a row that is a job on its own */
    int     state;
    int64_t release_ns;     /* HELD: when to hand it back; NO_TIMER while its task is incomplete */
    int     noted;          /* "holding back" already logged */
} item_t;

/* A task: rows (same `task` column) that are acquired one after another, held together until
 * the last is granted, and released together after the task's longest service time. While
 * it waits for its next row, a task holds the rows it already has: hold and wait. */
typedef struct {
    uint32_t id;
    int      n, rows[TASK_MAX_ROWS];    /* item indexes, in acquisition order */
    uint32_t hold;
    int      bundle;                    /* all-or-nothing: 0 not sent, 1 waiting, 2 held */
} task_t;

typedef struct {
    camp_ctx_t *c;
    item_t     *it;
    long        n;
    task_t     *tk;
    int         ntasks;
    int         outstanding[RELIEF_BANKER_RES];  /* units asked for and not handed back */
    int         all_or_nothing; /* send each task as one bundle */
    int64_t     scale_ns;
} replay_t;

/* The task's next row to acquire (first one not held), or -1 if it holds them all. */
static int task_next(const replay_t *rp, const task_t *t)
{
    for (int r = 0; r < t->n; r++)
        if (rp->it[t->rows[r]].state != IT_HELD)
            return t->rows[r];
    return -1;
}

static int task_has_state(const replay_t *rp, const task_t *t, int state)
{
    for (int r = 0; r < t->n; r++)
        if (rp->it[t->rows[r]].state == state)
            return 1;
    return 0;
}

/* Every row held: start the task's hold time. */
static void task_start_hold(replay_t *rp, task_t *t, int64_t now)
{
    for (int r = 0; r < t->n; r++)
        rp->it[t->rows[r]].release_ns = now + (int64_t)t->hold * rp->scale_ns;
}

/* Acquisition order = resource order (stable): what --deadlock prevent asks for. */
static void task_sort(replay_t *rp, task_t *t)
{
    for (int a = 1; a < t->n; a++)
        for (int b = a; b > 0 && rp->it[t->rows[b - 1]].bi > rp->it[t->rows[b]].bi; b--) {
            int tmp = t->rows[b];
            t->rows[b] = t->rows[b - 1];
            t->rows[b - 1] = tmp;
        }
}

static int replay_release(replay_t *rp, item_t *x)
{
    x->state = IT_DONE;
    rp->outstanding[x->bi] -= x->req->units;
    return send_release(rp->c, x->req);
}

/* Handle one reply from reliefd. Returns -1 if a send failed. */
static int on_reply(replay_t *rp, const relief_msg_t *m)
{
    camp_ctx_t *c = rp->c;
    uint32_t id = m->u.req.id;
    int64_t now = mono_ns();
    item_t *x = NULL;
    task_t *t = NULL;

    for (long k = 0; k < rp->n && !x; k++)
        if (rp->it[k].req->id == id)
            x = &rp->it[k];
    if (x && x->task >= 0)
        t = &rp->tk[x->task];

    if (m->kind == MSG_GRANT) {
        if (t && rp->all_or_nothing && t->bundle == 1 && (m->u.req.flags & REQ_FLAG_BUNDLE)) {
            for (int r = 0; r < t->n; r++)
                rp->it[t->rows[r]].state = IT_HELD;
            t->bundle = 2;
            task_start_hold(rp, t, now);
            c->granted++;
            LOG_INFO("GRANT task %u (all %d rows at once), holding for %u min", t->id, t->n, t->hold);
            return 0;
        }
        if (!x || x->state != IT_PENDING) {     /* e.g. requeued after we released it: give it back */
            LOG_WARN("GRANT for request %u that is not waiting: releasing it at once", id);
            return send_release(c, &m->u.req);
        }
        x->state = IT_HELD;
        c->granted++;
        if (t) {
            x->release_ns = NO_TIMER;
            if (task_next(rp, t) < 0)
                task_start_hold(rp, t, now);
            LOG_INFO("GRANT id=%u resource=%s units=%u (task %u: %s)", id, relief_resource_name(x->req->resource),
                     x->req->units, t->id, task_next(rp, t) < 0 ? "complete, holding" : "holding, wants more");
        } else {
            x->release_ns = now + (int64_t)x->req->service_time * rp->scale_ns;
            LOG_INFO("GRANT id=%u resource=%s units=%u, holding for %u min", id,
                     relief_resource_name(x->req->resource), x->req->units, x->req->service_time);
        }
        return 0;
    }

    if (m->kind == MSG_REVOKE) {
        if (t && t->bundle == 2) {
            for (int r = 0; r < t->n; r++) {
                rp->it[t->rows[r]].state = IT_PENDING;
                rp->it[t->rows[r]].release_ns = NO_TIMER;
            }
            t->bundle = 1;
        } else if (x && x->state == IT_HELD) {
            x->state = IT_PENDING;          /* reliefd queued the request again */
            if (t)
                for (int r = 0; r < t->n; r++)
                    rp->it[t->rows[r]].release_ns = NO_TIMER;   /* the task is incomplete again */
        } else {
            LOG_WARN("REVOKE for request %u that is not held", id);
            return 0;
        }
        LOG_WARN("REVOKE id=%u: preempted to break a deadlock, waiting for it again", id);
        return 0;
    }

    /* MSG_REJECT */
    if (!x || (x->state != IT_PENDING && !(t && t->bundle == 1))) {
        LOG_WARN("REJECT for request %u that is not waiting", id);
        return 0;
    }
    if (m->u.req.flags & REQ_FLAG_ORDER) {
        /* Out of resource order: give back what this task holds and acquire it in order. */
        x->state = IT_DEFERRED;
        rp->outstanding[x->bi] -= x->req->units;
        c->rejected++;
        if (t) {
            for (int r = 0; r < t->n; r++) {
                item_t *y = &rp->it[t->rows[r]];
                if (y->state != IT_HELD)
                    continue;
                if (replay_release(rp, y) == -1)
                    return -1;
                y->state = IT_DEFERRED;
            }
            task_sort(rp, t);
            LOG_WARN("REJECT id=%u: out of resource order; released task %u, retrying it in the order "
                     "TRUCK < BOAT < MED_KIT < FOOD_KIT", id, t->id);
        } else {
            LOG_WARN("REJECT id=%u: out of resource order; retrying", id);
        }
        return 0;
    }
    if (t && t->bundle == 1) {
        for (int r = 0; r < t->n; r++) {
            rp->it[t->rows[r]].state = IT_DONE;
            rp->outstanding[rp->it[t->rows[r]].bi] -= rp->it[t->rows[r]].req->units;
        }
        t->bundle = 0;
    } else {
        x->state = IT_DONE;
        rp->outstanding[x->bi] -= x->req->units;
    }
    c->rejected++;
    LOG_WARN("REJECT id=%u resource=%s units=%u (over the declared claim)", id,
             relief_resource_name(m->u.req.resource), m->u.req.units);
    return 0;
}

/* Can banker row x be sent now? (claim, resource order, task sequence) */
static int may_send(const replay_t *rp, const item_t *x)
{
    camp_ctx_t *c = rp->c;
    if (rp->outstanding[x->bi] + x->req->units > c->claim[x->bi])
        return 0;
    if (x->task >= 0) {
        const task_t *t = &rp->tk[x->task];
        if (task_has_state(rp, t, IT_PENDING) || task_next(rp, t) != x - rp->it)
            return 0;               /* one row at a time, in the task's order */
    }
    return 1;
}

/* All-or-nothing: send task t as one bundle once all its rows are due. */
static int send_bundle(replay_t *rp, task_t *t)
{
    uint16_t units[RELIEF_BANKER_RES] = { 0 };
    for (int r = 0; r < t->n; r++) {
        const item_t *y = &rp->it[t->rows[r]];
        if (y->state != IT_DEFERRED)
            return 0;
        units[y->bi] += y->req->units;
    }
    for (int j = 0; j < RELIEF_BANKER_RES; j++)
        if (rp->outstanding[j] + units[j] > rp->c->claim[j])
            return 0;
    if (send_request_ex(rp->c, rp->it[t->rows[0]].req, units, t->id) == -1)
        return -1;
    for (int r = 0; r < t->n; r++)
        rp->it[t->rows[r]].state = IT_PENDING;
    for (int j = 0; j < RELIEF_BANKER_RES; j++)
        rp->outstanding[j] += units[j];
    t->bundle = 1;
    LOG_INFO("sent task %u as one all-or-nothing request: TRUCK=%u BOAT=%u MED_KIT=%u FOOD_KIT=%u", t->id,
             units[0], units[1], units[2], units[3]);
    return 1;
}

/* Build the task table from the rows' task column. Returns the number of tasks, or -1. */
static int build_tasks(replay_t *rp, scenario_row_t *rows)
{
    for (long k = 0; k < rp->n; k++) {
        rp->it[k].task = -1;
        if (!rows[k].task || rp->it[k].bi < 0)
            continue;
        int t = 0;
        while (t < rp->ntasks && rp->tk[t].id != rows[k].task)
            t++;
        if (t == rp->ntasks) {
            rp->tk[t].id = rows[k].task;
            rp->ntasks++;
        }
        task_t *tk = &rp->tk[t];
        if (tk->n == TASK_MAX_ROWS) {
            LOG_ERROR("task %u has more than %d Banker's rows", tk->id, TASK_MAX_ROWS);
            return -1;
        }
        tk->rows[tk->n++] = (int)k;
        if (rows[k].req.service_time > tk->hold)
            tk->hold = rows[k].req.service_time;
        rp->it[k].task = t;
    }
    return rp->ntasks;
}

/*
 * Replay this camp's scenario rows on a simulated clock (a row with arrival A minutes is due
 * at replay start + A * time_scale_ms) and look after the Banker's requests: send a due row unless
 * it would take the camp over its claim (then it waits here), wait for reliefd's GRANT, hold,
 * RELEASE. Rows of a task are acquired one after another and released together. Ends when
 * every row is done (exit_when_done) or on SIGTERM/SIGINT.
 * Returns the number of requests sent, or -1 on error.
 */
static long run_scenario(camp_ctx_t *c, scenario_row_t *rows, long n, unsigned long time_scale_ms,
                         int exit_when_done, int all_or_nothing)
{
    replay_t rp;
    int64_t t0 = mono_ns();     /* simulated minute 0: the replay starts now, after HELLO, so
                                 * startup cost (e.g. under valgrind) does not compress the rows */
    long next = 0, rc = 0;

    memset(&rp, 0, sizeof rp);
    rp.c = c;
    rp.n = n;
    rp.scale_ns = (int64_t)time_scale_ms * 1000000;
    rp.all_or_nothing = all_or_nothing;
    rp.it = calloc((size_t)(n ? n : 1), sizeof *rp.it);
    rp.tk = calloc((size_t)(n ? n : 1), sizeof *rp.tk);
    if (!rp.it || !rp.tk) {
        LOG_ERROR("out of memory");
        free(rp.it);
        free(rp.tk);
        return -1;
    }
    for (long k = 0; k < n; k++) {
        rp.it[k].req = &rows[k].req;
        rp.it[k].bi = c->have_reply ? relief_banker_index(rows[k].req.resource) : -1;
    }
    if (build_tasks(&rp, rows) < 0) {
        free(rp.it);
        free(rp.tk);
        return -1;
    }
    c->rows_total = n;
    LOG_INFO("camp %u: %ld scenario rows (%d tasks), time scale %lu ms per simulated minute%s", c->id, n,
             rp.ntasks, time_scale_ms, all_or_nothing ? ", tasks all-or-nothing" : "");

    while (!g_stop) {
        int64_t now = mono_ns(), wake = now + 100000000;    /* re-check signals every 100 ms */
        int busy = next < n;

        service_signals(c);
        /* Hold times that are over: hand the resources back (a bundle in one message). */
        for (long k = 0; k < next && rc == 0 && !g_stop; k++) {
            item_t *x = &rp.it[k];
            if (x->state != IT_HELD || x->release_ns > now)
                continue;
            task_t *t = x->task >= 0 ? &rp.tk[x->task] : NULL;
            if (t && t->bundle == 2) {
                if (send_release(c, rp.it[t->rows[0]].req) == -1)
                    rc = -1;
                for (int r = 0; r < t->n; r++) {
                    rp.it[t->rows[r]].state = IT_DONE;
                    rp.outstanding[rp.it[t->rows[r]].bi] -= rp.it[t->rows[r]].req->units;
                }
                t->bundle = 3;
            } else if (replay_release(&rp, x) == -1) {
                rc = -1;
            }
        }
        /* Rows that are due. */
        while (rc == 0 && !g_stop && next < n &&
               (rp.scale_ns == 0 || now >= t0 + (int64_t)rp.it[next].req->arrival * rp.scale_ns)) {
            item_t *x = &rp.it[next++];
            if (x->bi < 0) {
                if (send_request(c, x->req) == -1)
                    rc = -1;
                x->state = IT_DONE;
            } else {
                x->state = IT_DEFERRED;
            }
        }
        /* Banker's requests that may go out now, in arrival order. */
        for (long k = 0; k < next && rc == 0 && !g_stop; k++) {
            item_t *x = &rp.it[k];
            if (x->state != IT_DEFERRED)
                continue;
            if (rp.all_or_nothing && x->task >= 0) {
                task_t *t = &rp.tk[x->task];
                if (t->bundle == 0 && t->rows[0] == k && send_bundle(&rp, t) == -1)
                    rc = -1;
                continue;
            }
            if (!may_send(&rp, x)) {
                if (!x->noted && rp.outstanding[x->bi] + x->req->units > c->claim[x->bi]) {
                    x->noted = 1;
                    c->deferred++;
                    LOG_INFO("holding back id=%u resource=%s units=%u: would exceed the claim of %d",
                             x->req->id, relief_resource_name(x->req->resource), x->req->units,
                             c->claim[x->bi]);
                }
                continue;
            }
            if (send_request_ex(c, x->req, NULL, x->task >= 0 ? rp.tk[x->task].id : 0) == -1)
                rc = -1;
            x->state = IT_PENDING;
            rp.outstanding[x->bi] += x->req->units;
        }
        if (rc)
            break;
        for (long k = 0; k < next; k++) {
            item_t *x = &rp.it[k];
            if (x->state == IT_DEFERRED || x->state == IT_PENDING || x->state == IT_WAITING)
                busy = 1;
            if (x->state == IT_HELD) {
                busy = 1;
                if (x->release_ns < wake)
                    wake = x->release_ns;
            }
        }
        if (next < n && rp.scale_ns) {
            int64_t due = t0 + (int64_t)rp.it[next].req->arrival * rp.scale_ns;
            if (due < wake)
                wake = due;
        }
        if (!busy && exit_when_done)
            break;

        /* Sleep until the next event, reading reliefd's replies meanwhile. */
        int64_t left = wake - mono_ns();
        if (left <= 0)
            continue;
        if (!c->have_reply) {
            struct timespec ts = { (time_t)(left / 1000000000), (long)(left % 1000000000) };
            nanosleep(&ts, NULL);
            continue;
        }
        struct timespec dl;
        relief_msg_t m;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += (time_t)(left / 1000000000);
        dl.tv_nsec += (long)(left % 1000000000);
        if (dl.tv_nsec >= 1000000000L) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000L;
        }
        ssize_t got = mq_timedreceive(c->reply, (char *)&m, sizeof m, NULL, &dl);
        if (got == (ssize_t)sizeof m &&
            (m.kind == MSG_GRANT || m.kind == MSG_REJECT || m.kind == MSG_REVOKE)) {
            if (on_reply(&rp, &m) == -1) {
                rc = -1;
                break;
            }
        } else if (got == -1 && errno != ETIMEDOUT && errno != EINTR) {
            LOG_ERROR("mq_timedreceive %s: %s", c->reply_name, strerror(errno));
            rc = -1;
            break;
        }
    }
    free(rp.it);
    free(rp.tk);
    return rc < 0 ? -1 : (long)c->sent;
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "id",         required_argument, NULL, 'i' },
        { "count",      required_argument, NULL, 'c' },
        { "seed",       required_argument, NULL, 's' },
        { "scenario",   required_argument, NULL, 'f' },
        { "time-scale", required_argument, NULL, 't' },
        { "exit-when-done", no_argument,   NULL, 'x' },
        { "shutdown",   no_argument,       NULL, 'S' },
        { "resources",  required_argument, NULL, 'r' },
        { "all-or-nothing", no_argument,   NULL, 'a' },
        { "help",       no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    static camp_ctx_t camp;
    unsigned long id = 0, count = 5, seed = 1, time_scale = DEFAULT_TIME_SCALE;
    int have_id = 0, have_count = 0, shutdown_only = 0, exit_when_done = 0, all_or_nothing = 0, opt;
    const char *scenario = NULL, *resources = NULL;
    scenario_row_t *rows = NULL;
    long sent, nrows = 0;
    int rc = EXIT_SUCCESS;

    log_init("camp");

    while ((opt = getopt_long(argc, argv, "i:c:s:f:t:xSr:ah", opts, NULL)) != -1) {
        switch (opt) {
        case 'i':
            if (parse_ulong(optarg, UINT32_MAX / RELIEF_REQ_ID_BASE - 1, &id) == -1 || id == 0) {
                fprintf(stderr, "invalid --id: %s\n", optarg);
                return 2;
            }
            have_id = 1;
            break;
        case 'c':
            if (parse_ulong(optarg, RELIEF_REQ_ID_BASE - 1, &count) == -1) {
                fprintf(stderr, "invalid --count: %s\n", optarg);
                return 2;
            }
            have_count = 1;
            break;
        case 's':
            if (parse_ulong(optarg, ULONG_MAX, &seed) == -1) {
                fprintf(stderr, "invalid --seed: %s\n", optarg);
                return 2;
            }
            break;
        case 'f':
            scenario = optarg;
            break;
        case 't':
            if (parse_ulong(optarg, 60000UL, &time_scale) == -1) {
                fprintf(stderr, "invalid --time-scale (0-60000 ms per simulated minute): %s\n", optarg);
                return 2;
            }
            break;
        case 'x':
            exit_when_done = 1;
            break;
        case 'S':
            shutdown_only = 1;
            break;
        case 'r':
            resources = optarg;
            break;
        case 'a':
            all_or_nothing = 1;
            break;
        case 'h':
            usage(argv[0]);
            return 0;
        default:
            usage(argv[0]);
            return 2;
        }
    }
    if (optind < argc || (!have_id && !shutdown_only) || (scenario && have_count)) {
        usage(argv[0]);
        return 2;
    }

    install_signal_handlers();
    clock_gettime(CLOCK_MONOTONIC, &camp.started);  /* for uptime */
    camp.id = (uint32_t)id;
    camp.rows_total = -1;
    camp.reply = (mqd_t)-1;
    {
        static const int defaults[RELIEF_BANKER_RES] = RELIEF_BANKER_DEFAULT_TOTALS;
        char err[512];
        memcpy(camp.total, defaults, sizeof defaults);
        if (resources && relief_resources_load(resources, camp.total, err, sizeof err) == -1) {
            LOG_ERROR("%s", err);
            return EXIT_FAILURE;
        }
        memcpy(camp.claim, camp.total, sizeof camp.claim);  /* random mode: claim everything */
    }

    camp.mq = mq_open(RELIEF_MQ_NAME, O_WRONLY);
    if (camp.mq == (mqd_t)-1) {
        LOG_ERROR("mq_open %s: %s (is reliefd running?)", RELIEF_MQ_NAME, strerror(errno));
        return EXIT_FAILURE;
    }
    camp.msg.camp_id = camp.id;
    camp.msg.camp_pid = (int32_t)getpid();

    if (shutdown_only) {
        rc = send_simple(&camp, MSG_SHUTDOWN, BYE_MAX_TRIES) == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
        if (rc == EXIT_SUCCESS)
            LOG_INFO("sent SHUTDOWN");
        mq_close(camp.mq);
        return rc;
    }

    if (scenario) {
        nrows = load_scenario(scenario, camp.id, &rows);
        if (nrows < 0) {
            mq_close(camp.mq);
            return EXIT_FAILURE;
        }
        compute_claim(&camp, rows, nrows);
        open_reply_queue(&camp);
    }
    status_start(&camp);
    if (send_hello(&camp) == -1) {
        status_stop(&camp);
        close_reply_queue(&camp);
        free(rows);
        mq_close(camp.mq);
        return g_stop ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    if (scenario) {
        if (!exit_when_done)
            LOG_INFO("camp %u: resident after the replay, until SIGTERM", camp.id);
        sent = run_scenario(&camp, rows, nrows, time_scale, exit_when_done, all_or_nothing);
    } else {
        sent = send_random(&camp, count, seed);
    }
    if (sent < 0 && !g_stop)
        rc = EXIT_FAILURE;
    if (scenario && rc == EXIT_SUCCESS && !g_stop)
        LOG_INFO("camp %u: replay done (%lu requests, %lu granted, %lu released), exiting", camp.id,
                 camp.sent, camp.granted, camp.released);

    if (send_simple(&camp, MSG_BYE, BYE_MAX_TRIES) == -1 && !g_stop)
        rc = EXIT_FAILURE;
    LOG_INFO("done: camp=%u sent=%lu escalated=%lu%s", camp.id, camp.sent, camp.escalated,
             g_stop ? " (stopped by signal)" : "");
    status_stop(&camp);
    close_reply_queue(&camp);
    free(rows);
    mq_close(camp.mq);
    return rc;
}
