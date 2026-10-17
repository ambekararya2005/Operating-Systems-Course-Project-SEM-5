/*
 * OS concept: Deadlock avoidance in a live system — the Banker's algorithm deciding every
 *             vehicle/kit request; waiting requests on a condition variable — Unit 3
 */
#include "resmgr.h"

#include "../deadlock/deadlock.h"   /* src/deadlock is not on the include path */
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
#define REPLY_TRIES     5           /* x REPLY_WAIT_MS before a grant counts as undeliverable */
#define REPLY_WAIT_MS   100

typedef struct pend {
    relief_request_t req;
    int              slot, bi;
    int64_t          since_ns;
    unsigned         retries;
    unsigned         logged;        /* bit per banker_result_t already logged: each kind once */
    struct pend     *next;
} pend_t;

struct resmgr {
    pthread_mutex_t lock;           /* guards everything below */
    pthread_cond_t  wake;           /* a request arrived, something was released, or stop */
    int             dirty, stop;

    const deadlock_policy_t *policy;
    banker_t        bk;             /* process p = slot p, camp slot_camp[p] */
    uint32_t        slot_camp[NSLOTS];  /* 0 = free */
    mqd_t           reply[NSLOTS];      /* (mqd_t)-1 = no reply queue */
    pend_t         *head, *tail;
    uint32_t        npending;
    uint64_t        grants, denied_unsafe, waits_unavailable, releases, rejects;
    int64_t         scale_ns;

    void          (*on_grant)(void *ctx, const relief_request_t *req);
    void           *grant_ctx;
    pthread_t       thread;
    int             started;
};

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

/* "a,b,c,d" of a vector of RELIEF_BANKER_RES ints. */
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

static int slot_of(const resmgr_t *rm, uint32_t camp)
{
    for (int i = 0; i < NSLOTS; i++)
        if (rm->slot_camp[i] == camp)
            return i;
    return -1;
}

static void need_vec(const resmgr_t *rm, int p, int *out)
{
    for (int j = 0; j < RELIEF_BANKER_RES; j++)
        out[j] = banker_need(&rm->bk, p, j);
}

/* Send one reply to camp slot p. Caller holds the lock. Returns 0, or -1 if undeliverable. */
static int reply_locked(resmgr_t *rm, int p, relief_msg_kind_t kind, const relief_request_t *req)
{
    relief_msg_t m;

    if (rm->reply[p] == (mqd_t)-1)
        return 0;                   /* the camp does not listen (random mode): nothing to tell */
    memset(&m, 0, sizeof m);
    m.kind = kind;
    m.camp_id = req->camp_id;
    m.u.req = *req;
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

static void unlink_pending(resmgr_t *rm, pend_t *prev, pend_t *p)
{
    if (prev)
        prev->next = p->next;
    else
        rm->head = p->next;
    if (rm->tail == p)
        rm->tail = prev;
    rm->npending--;
}

/* Try every waiting request in arrival order. Caller holds the lock. */
static void retry_pending_locked(resmgr_t *rm)
{
    pend_t *prev = NULL, *p = rm->head;
    char a[48], s[512], n[48];

    while (p) {
        pend_t *next = p->next;
        int req[RELIEF_BANKER_RES] = { 0 }, seq[NSLOTS], len = 0;
        const relief_request_t *r = &p->req;

        req[p->bi] = r->units;
        banker_result_t res = rm->policy->request(&rm->bk, p->slot, req, seq, &len);
        if (res == BANKER_GRANT) {
            if (reply_locked(rm, p->slot, MSG_GRANT, r) == -1) {
                /* The camp could not be told, so it would never release: undo the grant
                 * and leave the request waiting. */
                banker_release(&rm->bk, p->slot, req);
                prev = p;
                p = next;
                continue;
            }
            rm->grants++;
            EV_INFO("grant", "camp=%u id=%u resource=%s units=%u qty=%u waited_min=%.1f retries=%u "
                    "available=%s safe_seq=%s",
                    r->camp_id, r->id, relief_banker_name((unsigned)p->bi), r->units, r->quantity,
                    (double)(mono_ns() - p->since_ns) / (double)rm->scale_ns, p->retries,
                    vec(rm->bk.available, a, sizeof a), seq_str(rm, seq, len, s, sizeof s));
            if (rm->on_grant)
                rm->on_grant(rm->grant_ctx, r);
            unlink_pending(rm, prev, p);
            free(p);
            p = next;
            continue;
        }
        if (res == BANKER_OVER_CLAIM || res == BANKER_INACTIVE) {
            int need[RELIEF_BANKER_RES] = { 0 };
            if (res == BANKER_OVER_CLAIM)
                need_vec(rm, p->slot, need);
            rm->rejects++;
            EV_WARN("reject_over_claim", "camp=%u id=%u resource=%s units=%u reason=%s need=%s",
                    r->camp_id, r->id, relief_banker_name((unsigned)p->bi), r->units,
                    banker_result_name(res), vec(need, n, sizeof n));
            reply_locked(rm, p->slot, MSG_REJECT, r);
            unlink_pending(rm, prev, p);
            free(p);
            p = next;
            continue;
        }
        if (!(p->logged & (1u << res))) {   /* log each kind of wait once per request */
            int need[RELIEF_BANKER_RES];
            need_vec(rm, p->slot, need);
            if (res == BANKER_UNSAFE) {
                rm->denied_unsafe++;
                EV_INFO("deny_unsafe", "camp=%u id=%u resource=%s units=%u available=%s need=%s",
                        r->camp_id, r->id, relief_banker_name((unsigned)p->bi), r->units,
                        vec(rm->bk.available, a, sizeof a), vec(need, n, sizeof n));
            } else {
                rm->waits_unavailable++;
                EV_INFO("wait_unavailable", "camp=%u id=%u resource=%s units=%u available=%s",
                        r->camp_id, r->id, relief_banker_name((unsigned)p->bi), r->units,
                        vec(rm->bk.available, a, sizeof a));
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
        retry_pending_locked(rm);
    }
    pthread_mutex_unlock(&rm->lock);
    return NULL;
}

resmgr_t *resmgr_start(const resmgr_config_t *cfg)
{
    const deadlock_policy_t *pol = deadlock_policy_find(cfg->mode);
    char t[48];
    int err;

    if (!pol) {
        LOG_ERROR("unknown --deadlock mode '%s' (modes: %s)", cfg->mode, deadlock_policy_names());
        return NULL;
    }
    if (!pol->request) {
        LOG_ERROR("--deadlock %s (%s) is not implemented yet; use avoid", pol->name, pol->what);
        return NULL;
    }
    resmgr_t *rm = calloc(1, sizeof *rm);
    if (!rm)
        return NULL;
    rm->policy = pol;
    banker_init(&rm->bk, NSLOTS, RELIEF_BANKER_RES, cfg->total);
    for (int i = 0; i < NSLOTS; i++)
        rm->reply[i] = (mqd_t)-1;
    rm->scale_ns = (int64_t)(cfg->time_scale_ms ? cfg->time_scale_ms : 1) * 1000000;
    rm->on_grant = cfg->on_grant;
    rm->grant_ctx = cfg->grant_ctx;
    pthread_mutex_init(&rm->lock, NULL);
    pthread_cond_init(&rm->wake, NULL);

    /* Block every signal while creating the thread (inherited), so SIGCHLD/SIGTERM keep
     * reaching the main thread's mq_timedreceive. */
    sigset_t all, old;
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    err = pthread_create(&rm->thread, NULL, allocator_main, rm);
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (err) {
        LOG_ERROR("resmgr: pthread_create: %s", strerror(err));
        resmgr_stop(rm);
        return NULL;
    }
    rm->started = 1;
    EV_INFO("banker_start", "mode=%s policy=\"%s\" resources=TRUCK,BOAT,MED_KIT,FOOD_KIT total=%s "
            "kits_per_crate=%d", pol->name, pol->what, vec(cfg->total, t, sizeof t), RELIEF_KITS_PER_CRATE);
    return rm;
}

/* Release everything slot p holds, drop its waiting requests. Caller holds the lock. */
static void forget_locked(resmgr_t *rm, int p, const char *why)
{
    int held[RELIEF_BANKER_RES];
    unsigned dropped = 0;
    pend_t *prev = NULL, *q = rm->head;
    char h[48], a[48];

    while (q) {
        pend_t *next = q->next;
        if (q->slot == p) {
            unlink_pending(rm, prev, q);
            free(q);
            dropped++;
        } else {
            prev = q;
        }
        q = next;
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
    int max[RELIEF_BANKER_RES], clamped = 0;
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
    for (int j = 0; j < RELIEF_BANKER_RES; j++) {
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

void resmgr_submit(resmgr_t *rm, const relief_request_t *req)
{
    int bi = relief_banker_index(req->resource);
    pend_t *p;

    if (bi < 0 || req->units == 0)
        return;
    p = calloc(1, sizeof *p);
    if (!p) {
        EV_WARN("banker_submit_failed", "id=%u reason=ENOMEM", req->id);
        return;
    }
    p->req = *req;
    p->bi = bi;
    p->since_ns = mono_ns();
    pthread_mutex_lock(&rm->lock);
    p->slot = slot_of(rm, req->camp_id);
    if (p->slot < 0) {
        rm->rejects++;
        pthread_mutex_unlock(&rm->lock);
        EV_WARN("reject_over_claim", "camp=%u id=%u resource=%s units=%u reason=no_claim need=0,0,0,0",
                req->camp_id, req->id, relief_banker_name((unsigned)bi), req->units);
        free(p);
        return;
    }
    if (rm->tail)
        rm->tail->next = p;
    else
        rm->head = p;
    rm->tail = p;
    rm->npending++;
    rm->dirty = 1;
    pthread_cond_signal(&rm->wake);
    pthread_mutex_unlock(&rm->lock);
}

void resmgr_release(resmgr_t *rm, const relief_request_t *req)
{
    int bi = relief_banker_index(req->resource), rel[RELIEF_BANKER_RES] = { 0 };
    char a[48];

    if (bi < 0)
        return;
    rel[bi] = req->units;
    pthread_mutex_lock(&rm->lock);
    int p = slot_of(rm, req->camp_id);
    if (p < 0 || banker_release(&rm->bk, p, rel) == -1) {
        pthread_mutex_unlock(&rm->lock);
        EV_WARN("bad_release", "camp=%u id=%u resource=%s units=%u", req->camp_id, req->id,
                relief_banker_name((unsigned)bi), req->units);
        return;
    }
    rm->releases++;
    EV_INFO("release", "camp=%u id=%u resource=%s units=%u available=%s", req->camp_id, req->id,
            relief_banker_name((unsigned)bi), req->units, vec(rm->bk.available, a, sizeof a));
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
        for (pend_t *q = rm->head; q; q = q->next)
            snap.bk_proc[n].pending += q->slot == p;
        n++;
    }
    snap.bk_nproc = n;
    snap.bk_grants = rm->grants;
    snap.bk_denied_unsafe = rm->denied_unsafe;
    snap.bk_waits_unavailable = rm->waits_unavailable;
    snap.bk_releases = rm->releases;
    snap.bk_rejects = rm->rejects;
    snap.bk_pending = rm->npending;
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
    pthread_mutex_unlock(&rm->lock);
    if (rm->started)
        pthread_join(rm->thread, NULL);
    while (rm->head) {
        pend_t *p = rm->head;
        rm->head = p->next;
        EV_INFO("banker_abandoned", "camp=%u id=%u resource=%s units=%u", p->req.camp_id, p->req.id,
                relief_banker_name((unsigned)p->bi), p->req.units);
        free(p);
        abandoned++;
    }
    for (int i = 0; i < NSLOTS; i++)
        if (rm->reply[i] != (mqd_t)-1)
            mq_close(rm->reply[i]);
    if (rm->started)
        EV_INFO("banker_stop", "mode=%s grants=%llu denied_unsafe=%llu waits_unavailable=%llu "
                "releases=%llu rejects=%llu abandoned=%u",
                rm->policy->name, (unsigned long long)rm->grants, (unsigned long long)rm->denied_unsafe,
                (unsigned long long)rm->waits_unavailable, (unsigned long long)rm->releases,
                (unsigned long long)rm->rejects, abandoned);
    pthread_cond_destroy(&rm->wake);
    pthread_mutex_destroy(&rm->lock);
    free(rm);
}
