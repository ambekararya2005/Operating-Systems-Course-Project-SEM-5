/*
 * OS concept: Deadlock handling in a live system — avoidance (Banker's), prevention, detection
 *             and recovery; waiting requests on a condition variable — Unit 3
 */
#include "resmgr.h"

#include "../deadlock/deadlock.h"   /* src/deadlock is not on the include path */
#include "../deadlock/waitfor.h"
#include "log.h"
#include "shm.h"

#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define NSLOTS          BANKER_MAX_P
#define NRES            RELIEF_BANKER_RES
#define REPLY_TRIES     5           /* x REPLY_WAIT_MS before a reply counts as undeliverable */
#define REPLY_WAIT_MS   100

/* A request waiting in reliefd, or a grant a camp holds. */
typedef struct item {
    relief_request_t req;           /* as received (for a bundle: the task's first row) */
    int              vec[NRES];     /* units asked for / held, per resource */
    int              bundle;
    uint32_t         task;          /* the camp's task id, 0 = a row on its own */
    int              slot;
    int64_t          since_ns;      /* queued (request) or granted (grant) */
    unsigned         retries;
    unsigned         logged;        /* request: bit per banker_result_t already logged */
    struct item     *next;
} item_t;

typedef struct {
    item_t  *head, *tail;
    uint32_t n;
} list_t;

struct resmgr {
    pthread_mutex_t lock;           /* guards everything below */
    pthread_cond_t  wake;           /* allocator: a request arrived, something was released, or stop */
    pthread_cond_t  tick;           /* detector: stop */
    int             dirty, stop;

    const deadlock_policy_t *policy;
    banker_t        bk;             /* process p = slot p, camp slot_camp[p] */
    uint32_t        slot_camp[NSLOTS];  /* 0 = free */
    mqd_t           reply[NSLOTS];      /* (mqd_t)-1 = no reply queue */
    list_t          waiting;        /* requests not granted yet, in arrival order */
    list_t          granted;        /* grants not released yet */
    uint64_t        grants, denied_unsafe, waits_unavailable, releases, rejects, order_rejects;
    uint64_t        detected, recoveries;
    char            last_cycle[128];
    int64_t         scale_ns;
    long            detect_ms;

    void          (*on_grant)(void *ctx, const relief_request_t *req);
    void           *grant_ctx;
    pthread_t       thread, detector;
    int             started, detector_started;
};

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* "a,b,c,d" of a resource vector. */
static const char *vec(const int *v, char *buf, size_t len)
{
    snprintf(buf, len, "%d,%d,%d,%d", v[0], v[1], v[2], v[3]);
    return buf;
}

/* "P3,P1,P2" from slot indexes. */
static const char *seq_str(const resmgr_t *rm, const int *seq, int n, char *buf, size_t len)
{
    size_t o = 0;
    buf[0] = '\0';
    for (int i = 0; i < n && o < len; i++)
        o += (size_t)snprintf(buf + o, len - o, "%sP%u", i ? "," : "", rm->slot_camp[seq[i]]);
    if (n == 0)
        snprintf(buf, len, "-");
    return buf;
}

/* Resource name of an item for the log: the single resource, or BUNDLE. */
static const char *item_res(const item_t *it)
{
    if (it->bundle)
        return "BUNDLE";
    for (int j = 0; j < NRES; j++)
        if (it->vec[j])
            return relief_banker_name((unsigned)j);
    return "?";
}

static int item_units(const item_t *it)
{
    int u = 0;
    for (int j = 0; j < NRES; j++)
        u += it->vec[j];
    return u;
}

static int slot_of(const resmgr_t *rm, uint32_t camp)
{
    for (int i = 0; i < NSLOTS; i++)
        if (rm->slot_camp[i] == camp)
            return i;
    return -1;
}

static void need_vec(const resmgr_t *rm, int p, int *out)
{
    for (int j = 0; j < NRES; j++)
        out[j] = banker_need(&rm->bk, p, j);
}

static void list_append(list_t *l, item_t *it)
{
    it->next = NULL;
    if (l->tail)
        l->tail->next = it;
    else
        l->head = it;
    l->tail = it;
    l->n++;
}

static void list_unlink(list_t *l, item_t *prev, item_t *it)
{
    if (prev)
        prev->next = it->next;
    else
        l->head = it->next;
    if (l->tail == it)
        l->tail = prev;
    l->n--;
}

/* Send one reply to camp slot p. Caller holds the lock. Returns 0, or -1 if undeliverable. */
static int reply_locked(resmgr_t *rm, int p, relief_msg_kind_t kind, const relief_request_t *req,
                        uint8_t flags)
{
    relief_msg_t m;

    if (rm->reply[p] == (mqd_t)-1)
        return 0;                   /* the camp does not listen (random mode): nothing to tell */
    memset(&m, 0, sizeof m);
    m.kind = kind;
    m.camp_id = req->camp_id;
    m.u.req = *req;
    m.u.req.flags = flags;
    for (int i = 0; i < REPLY_TRIES; i++) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += REPLY_WAIT_MS * 1000000L;
        if (dl.tv_nsec >= 1000000000L) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000L;
        }
        if (mq_timedsend(rm->reply[p], (const char *)&m, sizeof m, 0, &dl) == 0)
            return 0;
        if (errno != ETIMEDOUT && errno != EINTR)
            break;
    }
    EV_WARN("reply_failed", "camp=%u id=%u kind=%s errno=%d", req->camp_id, req->id,
            relief_msg_kind_name(kind), errno);
    return -1;
}

/* Try every waiting request in arrival order. Caller holds the lock. */
static void retry_waiting_locked(resmgr_t *rm)
{
    item_t *prev = NULL, *p = rm->waiting.head;
    char a[48], s[512], n[48], v[48];

    while (p) {
        item_t *next = p->next;
        int seq[NSLOTS], len = 0;
        const relief_request_t *r = &p->req;

        /* What this job holds: the grants of the same task (a lone row holds nothing yet). */
        int held[NRES] = { 0 };
        for (const item_t *g = rm->granted.head; g && p->task; g = g->next)
            if (g->slot == p->slot && g->task == p->task)
                for (int j = 0; j < NRES; j++)
                    held[j] += g->vec[j];
        banker_result_t res = rm->policy->request(&rm->bk, p->slot, p->vec, held, seq, &len);
        if (res == BANKER_GRANT) {
            if (reply_locked(rm, p->slot, MSG_GRANT, r, r->flags & REQ_FLAG_BUNDLE) == -1) {
                /* The camp could not be told, so it would never release: undo the grant
                 * and leave the request waiting. */
                banker_release(&rm->bk, p->slot, p->vec);
                prev = p;
                p = next;
                continue;
            }
            rm->grants++;
            EV_INFO("grant", "camp=%u id=%u resource=%s units=%d vec=%s qty=%u waited_min=%.1f retries=%u "
                    "available=%s safe_seq=%s",
                    r->camp_id, r->id, item_res(p), p->bundle ? 0 : item_units(p), vec(p->vec, v, sizeof v),
                    r->quantity, (double)(mono_ns() - p->since_ns) / (double)rm->scale_ns, p->retries,
                    vec(rm->bk.available, a, sizeof a), seq_str(rm, seq, len, s, sizeof s));
            if (rm->on_grant && !p->bundle)
                rm->on_grant(rm->grant_ctx, r);
            list_unlink(&rm->waiting, prev, p);
            p->since_ns = mono_ns();
            list_append(&rm->granted, p);
            p = next;
            continue;
        }
        if (res == BANKER_OVER_CLAIM || res == BANKER_INACTIVE || res == BANKER_ORDER) {
            int need[NRES] = { 0 };
            if (res == BANKER_ORDER) {
                rm->order_rejects++;
                EV_INFO("reject_order", "camp=%u id=%u resource=%s units=%d held=%s "
                        "rule=TRUCK<BOAT<MED_KIT<FOOD_KIT", r->camp_id, r->id, item_res(p), item_units(p),
                        vec(rm->bk.alloc[p->slot], n, sizeof n));
            } else {
                if (res == BANKER_OVER_CLAIM)
                    need_vec(rm, p->slot, need);
                rm->rejects++;
                EV_WARN("reject_over_claim", "camp=%u id=%u resource=%s units=%d reason=%s need=%s",
                        r->camp_id, r->id, item_res(p), item_units(p), banker_result_name(res),
                        vec(need, n, sizeof n));
            }
            reply_locked(rm, p->slot, MSG_REJECT, r, res == BANKER_ORDER ? REQ_FLAG_ORDER : 0);
            list_unlink(&rm->waiting, prev, p);
            free(p);
            p = next;
            continue;
        }
        if (!(p->logged & (1u << res))) {   /* log each kind of wait once per request */
            int need[NRES];
            need_vec(rm, p->slot, need);
            if (res == BANKER_UNSAFE) {
                rm->denied_unsafe++;
                EV_INFO("deny_unsafe", "camp=%u id=%u resource=%s units=%d available=%s need=%s",
                        r->camp_id, r->id, item_res(p), item_units(p), vec(rm->bk.available, a, sizeof a),
                        vec(need, n, sizeof n));
            } else {
                rm->waits_unavailable++;
                EV_INFO("wait_unavailable", "camp=%u id=%u resource=%s units=%d available=%s",
                        r->camp_id, r->id, item_res(p), item_units(p), vec(rm->bk.available, a, sizeof a));
            }
            p->logged |= 1u << res;
        }
        p->retries++;
        prev = p;
        p = next;
    }
}

static void *allocator_main(void *arg)
{
    resmgr_t *rm = arg;

    pthread_mutex_lock(&rm->lock);
    for (;;) {
        while (!rm->dirty && !rm->stop)
            pthread_cond_wait(&rm->wake, &rm->lock);    /* waiting requests sleep here */
        if (rm->stop)
            break;
        rm->dirty = 0;
        retry_waiting_locked(rm);
    }
    pthread_mutex_unlock(&rm->lock);
    return NULL;
}

/* ---- Detection and recovery (--deadlock detect) ------------------------------------------ */

/* Request matrix: what each slot's waiting requests ask for. Caller holds the lock. */
static void request_matrix_locked(const resmgr_t *rm, int req[][BANKER_MAX_R])
{
    memset(req, 0, sizeof(int) * BANKER_MAX_R * NSLOTS);
    for (const item_t *p = rm->waiting.head; p; p = p->next)
        for (int j = 0; j < NRES; j++)
            req[p->slot][j] += p->vec[j];
}

/* Severity value of slot p's least severe waiting request (higher = less severe), -1 if none. */
static int lowest_severity_locked(const resmgr_t *rm, int p)
{
    int worst = -1;
    for (const item_t *q = rm->waiting.head; q; q = q->next)
        if (q->slot == p && (int)q->req.severity > worst)
            worst = q->req.severity;
    return worst;
}

/* Break the deadlock on `cycle`: preempt one grant of its lowest-severity member that the
 * previous member of the cycle is waiting for (a vehicle if possible), roll it back and
 * queue its request again. Caller holds the lock. */
static void recover_locked(resmgr_t *rm, const waitfor_t *g, const int *cycle, int len)
{
    int victim = cycle[0], vsev = -1, pos = 0;
    char a[48], v[48];

    for (int i = 0; i < len; i++) {
        int sev = lowest_severity_locked(rm, cycle[i]);
        if (sev > vsev || (sev == vsev && rm->slot_camp[cycle[i]] > rm->slot_camp[victim])) {
            vsev = sev;
            victim = cycle[i];
            pos = i;
        }
    }
    unsigned wanted = g->label[cycle[(pos + len - 1) % len]][victim];
    item_t *prev = NULL, *pick = NULL, *pick_prev = NULL;
    int pick_vehicle = 0;
    for (item_t *q = rm->granted.head; q; prev = q, q = q->next) {
        if (q->slot != victim)
            continue;
        unsigned has = 0;
        for (int j = 0; j < NRES; j++)
            if (q->vec[j] > 0)
                has |= 1u << j;
        if (!(has & wanted))
            continue;
        int vehicle = (has & wanted & ((1u << BK_TRUCK) | (1u << BK_BOAT))) != 0;
        if (!pick || vehicle >= pick_vehicle) {     /* prefer a vehicle, then the latest grant */
            pick = q;
            pick_prev = prev;
            pick_vehicle = vehicle;
        }
    }
    if (!pick) {
        EV_WARN("recovery_failed", "victim=camp%u reason=no_grant_to_preempt", rm->slot_camp[victim]);
        return;
    }
    if (reply_locked(rm, victim, MSG_REVOKE, &pick->req, pick->bundle ? REQ_FLAG_BUNDLE : 0) == -1)
        return;                     /* the camp would not know: keep the state consistent */
    banker_release(&rm->bk, victim, pick->vec);
    list_unlink(&rm->granted, pick_prev, pick);
    pick->since_ns = mono_ns();
    pick->retries = 0;
    pick->logged = 0;
    list_append(&rm->waiting, pick);        /* roll the request back and queue it again */
    rm->recoveries++;
    EV_WARN("recovery", "victim=camp%u severity=%s preempted_id=%u resource=%s units=%d vec=%s requeued=1 "
            "available=%s", rm->slot_camp[victim], relief_severity_name((unsigned)(vsev < 0 ? 0 : vsev)),
            pick->req.id, item_res(pick), item_units(pick), vec(pick->vec, v, sizeof v),
            vec(rm->bk.available, a, sizeof a));
    rm->dirty = 1;
    pthread_cond_signal(&rm->wake);
}

/* One detector pass. Caller holds the lock. */
static void detect_locked(resmgr_t *rm)
{
    static int req[NSLOTS][BANKER_MAX_R];   /* detector thread only */
    static waitfor_t g;
    unsigned char dead[NSLOTS];
    int cycle[NSLOTS], len;
    char buf[sizeof rm->last_cycle];

    request_matrix_locked(rm, req);
    waitfor_build(&g, &rm->bk, (const int (*)[BANKER_MAX_R])req);
    if (!waitfor_find_cycle(&g, cycle, &len))
        return;
    /* With several instances per resource a cycle is not enough: confirm with the detection
     * algorithm that every camp on the cycle is really deadlocked. */
    int ndead = banker_detect(&rm->bk, (const int (*)[BANKER_MAX_R])req, dead);
    for (int i = 0; i < len; i++)
        if (!dead[cycle[i]])
            return;
    size_t o = 0;
    for (int i = 0; i <= len && o < sizeof buf; i++)
        o += (size_t)snprintf(buf + o, sizeof buf - o, "%scamp%u", i ? "->" : "",
                              rm->slot_camp[cycle[i % len]]);
    snprintf(rm->last_cycle, sizeof rm->last_cycle, "%s", buf);
    rm->detected++;
    EV_WARN("deadlock_detected", "cycle=%s length=%d deadlocked=%d edges=%d", buf, len, ndead,
            waitfor_edges(&g));
    recover_locked(rm, &g, cycle, len);
}

static void *detector_main(void *arg)
{
    resmgr_t *rm = arg;

    pthread_mutex_lock(&rm->lock);
    while (!rm->stop) {
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_sec += rm->detect_ms / 1000;
        dl.tv_nsec += (rm->detect_ms % 1000) * 1000000L;
        if (dl.tv_nsec >= 1000000000L) {
            dl.tv_sec++;
            dl.tv_nsec -= 1000000000L;
        }
        int rc = 0;
        while (!rm->stop && rc != ETIMEDOUT)
            rc = pthread_cond_timedwait(&rm->tick, &rm->lock, &dl);
        if (!rm->stop)
            detect_locked(rm);
    }
    pthread_mutex_unlock(&rm->lock);
    return NULL;
}

/* ---- Lifecycle and the main thread's calls ------------------------------------------------ */

resmgr_t *resmgr_start(const resmgr_config_t *cfg)
{
    const deadlock_policy_t *pol = deadlock_policy_find(cfg->mode);
    char t[48];
    int err;

    if (!pol) {
        LOG_ERROR("unknown --deadlock mode '%s' (modes: %s)", cfg->mode, deadlock_policy_names());
        return NULL;
    }
    resmgr_t *rm = calloc(1, sizeof *rm);
    if (!rm)
        return NULL;
    rm->policy = pol;
    banker_init(&rm->bk, NSLOTS, NRES, cfg->total);
    for (int i = 0; i < NSLOTS; i++)
        rm->reply[i] = (mqd_t)-1;
    rm->scale_ns = (int64_t)(cfg->time_scale_ms ? cfg->time_scale_ms : 1) * 1000000;
    rm->detect_ms = cfg->detect_interval_ms > 0 ? cfg->detect_interval_ms : 1000;
    rm->on_grant = cfg->on_grant;
    rm->grant_ctx = cfg->grant_ctx;
    snprintf(rm->last_cycle, sizeof rm->last_cycle, "-");
    pthread_mutex_init(&rm->lock, NULL);
    pthread_cond_init(&rm->wake, NULL);
    pthread_cond_init(&rm->tick, NULL);

    /* Block every signal while creating the threads (inherited), so SIGCHLD/SIGTERM keep
     * reaching the main thread's mq_timedreceive. */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    err = pthread_create(&rm->thread, NULL, allocator_main, rm);
    if (!err) {
        rm->started = 1;
        if (pol->detector && (err = pthread_create(&rm->detector, NULL, detector_main, rm)) == 0)
            rm->detector_started = 1;
    }
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (err) {
        LOG_ERROR("resmgr: pthread_create: %s", strerror(err));
        resmgr_stop(rm);
        return NULL;
    }
    EV_INFO("banker_start", "mode=%s policy=\"%s\" breaks=\"%s\" resources=TRUCK,BOAT,MED_KIT,FOOD_KIT "
            "total=%s kits_per_crate=%d detect_interval_ms=%ld", pol->name, pol->what, pol->breaks,
            vec(cfg->total, t, sizeof t), RELIEF_KITS_PER_CRATE, pol->detector ? rm->detect_ms : 0L);
    return rm;
}

/* Release everything slot p holds, drop its waiting requests. Caller holds the lock. */
static void forget_locked(resmgr_t *rm, int p, const char *why)
{
    int held[NRES];
    unsigned dropped = 0;
    char h[48], a[48];

    for (int pass = 0; pass < 2; pass++) {
        list_t *l = pass ? &rm->granted : &rm->waiting;
        item_t *prev = NULL, *q = l->head;
        while (q) {
            item_t *next = q->next;
            if (q->slot == p) {
                list_unlink(l, prev, q);
                dropped += !pass;
                free(q);
            } else {
                prev = q;
            }
            q = next;
        }
    }
    memcpy(held, rm->bk.alloc[p], sizeof held);
    if (rm->bk.active[p] || dropped) {
        banker_remove(&rm->bk, p);
        EV_INFO("release_all", "camp=%u reason=%s returned=%s dropped_waiting=%u available=%s",
                rm->slot_camp[p], why, vec(held, h, sizeof h), dropped,
                vec(rm->bk.available, a, sizeof a));
    }
    rm->dirty = 1;
    pthread_cond_signal(&rm->wake);
}

void resmgr_hello(resmgr_t *rm, uint32_t camp, const relief_hello_t *hello)
{
    int max[NRES], clamped = 0;
    char m[48], name[64];

    pthread_mutex_lock(&rm->lock);
    int p = slot_of(rm, camp);
    if (p < 0)
        p = slot_of(rm, 0);         /* a free slot */
    if (p < 0) {
        pthread_mutex_unlock(&rm->lock);
        EV_WARN("banker_full", "camp=%u slots=%d", camp, NSLOTS);
        return;
    }
    if (rm->slot_camp[p] == camp)
        forget_locked(rm, p, "rehello");    /* a restarted camp starts from nothing */
    rm->slot_camp[p] = camp;
    for (int j = 0; j < NRES; j++) {
        max[j] = hello->max_claim[j];
        if (max[j] > rm->bk.total[j]) {
            max[j] = rm->bk.total[j];
            clamped = 1;
        }
    }
    banker_set_max(&rm->bk, p, max);
    if (rm->reply[p] != (mqd_t)-1) {
        mq_close(rm->reply[p]);
        rm->reply[p] = (mqd_t)-1;
    }
    if (hello->flags & HELLO_FLAG_REPLY_QUEUE) {
        snprintf(name, sizeof name, RELIEF_REPLY_MQ_FMT, camp);
        rm->reply[p] = mq_open(name, O_WRONLY);
        if (rm->reply[p] == (mqd_t)-1)
            EV_WARN("reply_queue_missing", "camp=%u queue=%s errno=%d", camp, name, errno);
    }
    EV_INFO("claim", "camp=%u max=%s clamped=%d reply_queue=%d", camp, vec(max, m, sizeof m), clamped,
            rm->reply[p] != (mqd_t)-1);
    rm->dirty = 1;
    pthread_cond_signal(&rm->wake);
    pthread_mutex_unlock(&rm->lock);
}

void resmgr_submit(resmgr_t *rm, const relief_msg_t *msg)
{
    const relief_request_t *req = &msg->u.req;
    item_t *p = calloc(1, sizeof *p);

    if (!p) {
        EV_WARN("banker_submit_failed", "id=%u reason=ENOMEM", req->id);
        return;
    }
    p->req = *req;
    p->task = msg->u.bundle.task;
    if (req->flags & REQ_FLAG_BUNDLE) {
        p->bundle = 1;
        for (int j = 0; j < NRES; j++)
            p->vec[j] = msg->u.bundle.units[j];
    } else {
        int bi = relief_banker_index(req->resource);
        if (bi < 0 || req->units == 0) {
            free(p);
            return;
        }
        p->vec[bi] = req->units;
    }
    p->since_ns = mono_ns();
    pthread_mutex_lock(&rm->lock);
    p->slot = slot_of(rm, req->camp_id);
    if (p->slot < 0) {
        rm->rejects++;
        pthread_mutex_unlock(&rm->lock);
        EV_WARN("reject_over_claim", "camp=%u id=%u resource=%s units=%d reason=no_claim need=0,0,0,0",
                req->camp_id, req->id, item_res(p), item_units(p));
        free(p);
        return;
    }
    list_append(&rm->waiting, p);
    rm->dirty = 1;
    pthread_cond_signal(&rm->wake);
    pthread_mutex_unlock(&rm->lock);
}

void resmgr_release(resmgr_t *rm, const relief_request_t *req)
{
    char a[48], v[48];
    item_t *prev = NULL, *q = NULL;

    pthread_mutex_lock(&rm->lock);
    int p = slot_of(rm, req->camp_id);
    if (p >= 0)
        for (q = rm->granted.head; q; prev = q, q = q->next)
            if (q->slot == p && q->req.id == req->id)
                break;
    if (!q) {
        /* Not held (e.g. preempted by recovery just before the camp released it): drop a
         * requeued copy too, the camp is done with it. */
        unsigned dropped = 0;
        prev = NULL;
        for (q = rm->waiting.head; q && p >= 0;) {
            item_t *next = q->next;
            if (q->slot == p && q->req.id == req->id) {
                list_unlink(&rm->waiting, prev, q);
                free(q);
                dropped++;
            } else {
                prev = q;
            }
            q = next;
        }
        pthread_mutex_unlock(&rm->lock);
        EV_WARN("stray_release", "camp=%u id=%u dropped_waiting=%u", req->camp_id, req->id, dropped);
        return;
    }
    banker_release(&rm->bk, p, q->vec);
    list_unlink(&rm->granted, prev, q);
    rm->releases++;
    EV_INFO("release", "camp=%u id=%u resource=%s units=%d vec=%s available=%s", req->camp_id, req->id,
            item_res(q), q->bundle ? 0 : item_units(q), vec(q->vec, v, sizeof v),
            vec(rm->bk.available, a, sizeof a));
    free(q);
    rm->dirty = 1;
    pthread_cond_signal(&rm->wake);   /* waiting requests are retried */
    pthread_mutex_unlock(&rm->lock);
}

void resmgr_camp_gone(resmgr_t *rm, uint32_t camp, const char *why, int unlink_queue)
{
    char name[64];

    pthread_mutex_lock(&rm->lock);
    int p = slot_of(rm, camp);
    if (p >= 0) {
        forget_locked(rm, p, why);
        if (rm->reply[p] != (mqd_t)-1) {
            mq_close(rm->reply[p]);
            rm->reply[p] = (mqd_t)-1;
        }
    }
    pthread_mutex_unlock(&rm->lock);
    if (unlink_queue) {
        snprintf(name, sizeof name, RELIEF_REPLY_MQ_FMT, camp);
        mq_unlink(name);            /* a crashed camp could not remove its own queue */
    }
}

void resmgr_publish(resmgr_t *rm, relief_shm_t *shm)
{
    static relief_status_t snap;    /* main thread only; too big for comfort on the stack */
    int rc;

    if (!shm)
        return;
    pthread_mutex_lock(&rm->lock);
    snprintf(snap.dl_mode, sizeof snap.dl_mode, "%s", rm->policy->name);
    memcpy(snap.bk_total, rm->bk.total, sizeof snap.bk_total);
    memcpy(snap.bk_available, rm->bk.available, sizeof snap.bk_available);
    uint32_t n = 0;
    for (int p = 0; p < NSLOTS; p++) {
        if (!rm->slot_camp[p])
            continue;
        snap.bk_proc[n].camp = rm->slot_camp[p];
        snap.bk_proc[n].active = rm->bk.active[p];
        memcpy(snap.bk_proc[n].max, rm->bk.max[p], sizeof snap.bk_proc[n].max);
        memcpy(snap.bk_proc[n].alloc, rm->bk.alloc[p], sizeof snap.bk_proc[n].alloc);
        snap.bk_proc[n].pending = 0;
        memset(snap.bk_proc[n].request, 0, sizeof snap.bk_proc[n].request);
        for (const item_t *q = rm->waiting.head; q; q = q->next) {
            if (q->slot != p)
                continue;
            snap.bk_proc[n].pending++;
            for (int j = 0; j < NRES; j++)
                snap.bk_proc[n].request[j] += q->vec[j];
        }
        n++;
    }
    snap.bk_nproc = n;
    snap.bk_grants = rm->grants;
    snap.bk_denied_unsafe = rm->denied_unsafe;
    snap.bk_waits_unavailable = rm->waits_unavailable;
    snap.bk_releases = rm->releases;
    snap.bk_rejects = rm->rejects + rm->order_rejects;
    snap.bk_pending = rm->waiting.n;
    snap.dl_detected = rm->detected;
    snap.dl_recoveries = rm->recoveries;
    memcpy(snap.dl_last_cycle, rm->last_cycle, sizeof snap.dl_last_cycle);
    pthread_mutex_unlock(&rm->lock);

    rc = relief_shm_begin_write(shm);       /* resmgr lock released: no nesting */
    if (rc == -1)
        return;
    if (rc == 1)
        EV_WARN("status_lock_recovered", "by=resmgr reason=EOWNERDEAD");
    relief_status_t *st = &shm->data;
    memcpy(st->dl_mode, snap.dl_mode, sizeof st->dl_mode);
    memcpy(st->bk_total, snap.bk_total, sizeof st->bk_total);
    memcpy(st->bk_available, snap.bk_available, sizeof st->bk_available);
    memcpy(st->bk_proc, snap.bk_proc, sizeof st->bk_proc);
    st->bk_nproc = snap.bk_nproc;
    st->bk_grants = snap.bk_grants;
    st->bk_denied_unsafe = snap.bk_denied_unsafe;
    st->bk_waits_unavailable = snap.bk_waits_unavailable;
    st->bk_releases = snap.bk_releases;
    st->bk_rejects = snap.bk_rejects;
    st->bk_pending = snap.bk_pending;
    st->dl_detected = snap.dl_detected;
    st->dl_recoveries = snap.dl_recoveries;
    memcpy(st->dl_last_cycle, snap.dl_last_cycle, sizeof st->dl_last_cycle);
    relief_shm_end_write(shm);
}

void resmgr_stop(resmgr_t *rm)
{
    unsigned abandoned = 0;

    if (!rm)
        return;
    pthread_mutex_lock(&rm->lock);
    rm->stop = 1;
    pthread_cond_broadcast(&rm->wake);
    pthread_cond_broadcast(&rm->tick);
    pthread_mutex_unlock(&rm->lock);
    if (rm->started)
        pthread_join(rm->thread, NULL);
    if (rm->detector_started)
        pthread_join(rm->detector, NULL);
    while (rm->waiting.head) {
        item_t *p = rm->waiting.head;
        rm->waiting.head = p->next;
        EV_INFO("banker_abandoned", "camp=%u id=%u resource=%s units=%d", p->req.camp_id, p->req.id,
                item_res(p), item_units(p));
        free(p);
        abandoned++;
    }
    while (rm->granted.head) {
        item_t *p = rm->granted.head;
        rm->granted.head = p->next;
        free(p);
    }
    for (int i = 0; i < NSLOTS; i++)
        if (rm->reply[i] != (mqd_t)-1)
            mq_close(rm->reply[i]);
    if (rm->started)
        EV_INFO("banker_stop", "mode=%s grants=%llu denied_unsafe=%llu waits_unavailable=%llu "
                "releases=%llu rejects=%llu order_rejects=%llu deadlocks=%llu recoveries=%llu abandoned=%u",
                rm->policy->name, (unsigned long long)rm->grants, (unsigned long long)rm->denied_unsafe,
                (unsigned long long)rm->waits_unavailable, (unsigned long long)rm->releases,
                (unsigned long long)rm->rejects, (unsigned long long)rm->order_rejects,
                (unsigned long long)rm->detected, (unsigned long long)rm->recoveries, abandoned);
    pthread_cond_destroy(&rm->tick);
    pthread_cond_destroy(&rm->wake);
    pthread_mutex_destroy(&rm->lock);
    free(rm);
}
