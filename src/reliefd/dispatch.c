/*
 * OS concept: Threads (POSIX pthreads) and live CPU scheduling — Unit 2;
 *             mutex + condition variable protecting a shared ready queue — Unit 3
 */
#include "dispatch.h"

#include "../sched/sched.h"     /* src/sched is not on the include path (see sched.h) */
#include "log.h"
#include "shm.h"

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* A request inside the scheduler. The sched_job_t comes first, so the policy's job
 * pointers can be cast back. Real-time bookkeeping (ms) gives precise wait times. */
typedef struct {
    sched_job_t job;
    int64_t     arrival_ms;         /* CLOCK_MONOTONIC ms when enqueued */
    int64_t     start_ms;           /* first dispatch, -1 before */
    int64_t     ready_since_ms;     /* last time it entered the ready queue */
    int64_t     wait_ms;            /* accumulated time ready but not running */
    unsigned    preemptions;
    int         dispatch_level;     /* queue level when last dispatched (to spot demotions) */
} live_job_t;

typedef struct {
    struct dispatcher *d;
    int                index;
} team_arg_t;

struct dispatcher {
    pthread_mutex_t lock;           /* guards everything below */
    pthread_cond_t  work;           /* signalled when a job becomes ready, or on stop */

    sched_policy_t *policy;
    int             teams;
    int64_t         scale_ms;       /* real ms per simulated minute, >= 1 */
    int64_t         t0_ms;          /* CLOCK_MONOTONIC at start: simulated minute 0 */
    sched_job_t   **running;        /* running[t]: job on team t, NULL if idle */
    int            *preempt;        /* set by on_tick, cleared by the team when it acts */
    int64_t        *busy_since_ms;  /* CLOCK_REALTIME ms of each team's current dispatch */
    uint64_t       *team_done;
    size_t          ready;          /* jobs inside the policy's ready queues */
    uint64_t        seq;
    uint64_t        enqueued, completed, preemptions, promotions, demotions, abandoned;
    int             stop;

    pthread_t      *threads;
    team_arg_t     *args;
    int             nthreads;       /* team threads actually started */
    pthread_t       clock;
    int             clock_started;
    relief_shm_t   *shm;
};

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Current simulated minute. */
static int64_t sim_now(const dispatcher_t *d)
{
    return (mono_ms() - d->t0_ms) / d->scale_ms;
}

static double to_min(const dispatcher_t *d, int64_t ms)
{
    return (double)ms / (double)d->scale_ms;
}

static void sleep_ms(int64_t ms)
{
    struct timespec ts = { (time_t)(ms / 1000), (long)(ms % 1000) * 1000000L };
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR)
        ;
}

/* Copy scheduler state into the shared-memory status table. Caller holds d->lock. */
static void publish_locked(dispatcher_t *d)
{
    uint64_t lens[RELIEF_SHM_LEVELS] = { 0 };
    int levels = 1, rc;

    if (!d->shm)
        return;
    if (d->policy->queue_lengths)
        levels = d->policy->queue_lengths(d->policy, lens, RELIEF_SHM_LEVELS);
    else
        lens[0] = d->ready;

    rc = relief_shm_begin_write(d->shm);
    if (rc == -1)
        return;
    if (rc == 1)
        EV_WARN("status_lock_recovered", "by=dispatcher reason=EOWNERDEAD");
    relief_status_t *st = &d->shm->data;
    snprintf(st->policy, sizeof st->policy, "%s", d->policy->name);
    st->nteams = (uint32_t)d->teams;
    st->nlevels = (uint32_t)levels;
    memcpy(st->queue_len, lens, sizeof lens);
    st->sched_enqueued = d->enqueued;
    st->sched_completed = d->completed;
    st->sched_preemptions = d->preemptions;
    st->sched_promotions = d->promotions;
    for (int t = 0; t < d->teams; t++) {
        relief_shm_team_t *row = &st->teams[t];
        const sched_job_t *j = d->running[t];
        row->state = j ? 1 : 0;
        row->job_id = j ? j->id : 0;
        row->camp = j ? j->camp : 0;
        row->severity = j ? j->severity : 0;
        row->level = j ? (uint8_t)j->queue_level : 0;
        row->remaining = j ? j->remaining : 0;
        row->busy_since_ms = j ? d->busy_since_ms[t] : 0;
        row->completed = d->team_done[t];
    }
    relief_shm_end_write(d->shm);
}

/* Policy events (called by on_tick, i.e. with d->lock held). */
static void on_policy_event(void *ctx, int64_t now, const sched_job_t *job, sched_event_t ev,
                            int from, int to)
{
    dispatcher_t *d = ctx;

    if (ev == SCHED_EV_PROMOTE) {
        d->promotions++;
        EV_INFO("promote", "id=%u camp=%u severity=%s from=Q%d to=Q%d sim_min=%lld", job->id,
                job->camp, relief_severity_name(job->severity), from, to, (long long)now);
    } else {
        d->demotions++;
        EV_INFO("demote", "id=%u camp=%u severity=%s from=Q%d to=Q%d sim_min=%lld", job->id,
                job->camp, relief_severity_name(job->severity), from, to, (long long)now);
    }
}

/* Team t takes job lj. Caller holds d->lock. */
static void dispatch_locked(dispatcher_t *d, int t, live_job_t *lj)
{
    int64_t now_ms = mono_ms();
    sched_job_t *j = &lj->job;

    lj->wait_ms += now_ms - lj->ready_since_ms;
    if (lj->start_ms < 0) {
        lj->start_ms = now_ms;
        j->start = sim_now(d);
    }
    j->slice = 0;
    j->team = t;
    lj->dispatch_level = j->queue_level;
    d->running[t] = j;
    d->preempt[t] = 0;
    d->busy_since_ms[t] = relief_now_ms();
    EV_INFO("dispatch", "team=%d id=%u camp=%u severity=%s level=%d remaining=%lld waited=%.1f",
            t + 1, j->id, j->camp, relief_severity_name(j->severity), j->queue_level,
            (long long)j->remaining, to_min(d, lj->wait_ms));
    publish_locked(d);
}

static void complete_locked(dispatcher_t *d, int t, live_job_t *lj)
{
    sched_job_t *j = &lj->job;
    int64_t now_ms = mono_ms();

    d->running[t] = NULL;
    j->team = -1;
    j->finish = sim_now(d);
    if (d->policy->on_finish)
        d->policy->on_finish(d->policy, j, j->finish);
    d->completed++;
    d->team_done[t]++;
    EV_INFO("complete", "team=%d id=%u camp=%u severity=%s burst=%lld wait=%.1f turnaround=%.1f "
            "response=%.1f preemptions=%u",
            t + 1, j->id, j->camp, relief_severity_name(j->severity), (long long)j->burst,
            to_min(d, lj->wait_ms), to_min(d, now_ms - lj->arrival_ms),
            to_min(d, lj->start_ms - lj->arrival_ms), lj->preemptions);
    publish_locked(d);
    free(lj);
}

/* Why the policy took the job back: MLFQ demotion (its level changed while it ran), an RR
 * quantum rotation, or a more important job (SRTF, preemptive prio, MLFQ higher queue). A
 * demoted job is usually re-dispatched at once if nothing better is waiting. */
static const char *preempt_reason(const dispatcher_t *d, const live_job_t *lj)
{
    if (lj->job.queue_level != lj->dispatch_level)
        return "demote";
    if (d->policy->uses_quantum)
        return "quantum";
    return "priority";
}

static void preempt_locked(dispatcher_t *d, int t, live_job_t *lj)
{
    sched_job_t *j = &lj->job;
    const char *reason = preempt_reason(d, lj);

    d->running[t] = NULL;
    d->preempt[t] = 0;
    j->team = -1;
    j->ready_since = sim_now(d);
    lj->ready_since_ms = mono_ms();
    lj->preemptions++;
    d->preemptions++;
    d->policy->enqueue(d->policy, j, j->ready_since);
    d->ready++;
    EV_INFO("preempt", "team=%d id=%u camp=%u severity=%s remaining=%lld level=%d reason=%s",
            t + 1, j->id, j->camp, relief_severity_name(j->severity), (long long)j->remaining,
            j->queue_level, reason);
    publish_locked(d);
    pthread_cond_broadcast(&d->work);   /* an idle team may take it, or the better job */
}

static void *team_main(void *arg)
{
    team_arg_t *a = arg;
    dispatcher_t *d = a->d;
    int t = a->index;

    pthread_mutex_lock(&d->lock);
    for (;;) {
        live_job_t *lj = NULL;

        /* Wait for work: the condition is re-checked after every wakeup (spurious wakeups,
         * or another team took the job first). */
        while (!d->stop && (d->ready == 0 ||
                            !(lj = (live_job_t *)d->policy->pick_next(d->policy, sim_now(d)))))
            pthread_cond_wait(&d->work, &d->lock);
        if (d->stop)
            break;
        d->ready--;
        dispatch_locked(d, t, lj);

        /* Work one simulated minute at a time, outside the lock. */
        for (;;) {
            pthread_mutex_unlock(&d->lock);
            sleep_ms(d->scale_ms);
            pthread_mutex_lock(&d->lock);
            lj->job.remaining--;
            lj->job.slice++;
            lj->job.last_run = sim_now(d);
            if (lj->job.remaining <= 0) {
                complete_locked(d, t, lj);
                break;
            }
            if (d->stop) {          /* shutting down: abandon the job in progress */
                d->running[t] = NULL;
                d->abandoned++;
                free(lj);
                break;
            }
            if (d->preempt[t]) {    /* on_tick chose something better */
                preempt_locked(d, t, lj);
                break;
            }
            publish_locked(d);      /* remaining time moved */
        }
        if (d->stop)
            break;
    }
    pthread_mutex_unlock(&d->lock);
    return NULL;
}

/* The scheduler clock: once per simulated minute, let the policy age jobs and mark
 * preemptions (teams act on their mark at their next minute boundary). */
static void *clock_main(void *arg)
{
    dispatcher_t *d = arg;
    struct timespec next;

    clock_gettime(CLOCK_MONOTONIC, &next);
    pthread_mutex_lock(&d->lock);
    while (!d->stop) {
        uint64_t before = d->promotions + d->demotions;
        int marked = 0;

        pthread_mutex_unlock(&d->lock);
        next.tv_nsec += (long)(d->scale_ms % 1000) * 1000000L;
        next.tv_sec += (time_t)(d->scale_ms / 1000);
        if (next.tv_nsec >= 1000000000L) {
            next.tv_sec++;
            next.tv_nsec -= 1000000000L;
        }
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL) == EINTR)
            ;
        pthread_mutex_lock(&d->lock);
        if (d->stop || !d->policy->on_tick)
            continue;
        for (int t = 0; t < d->teams; t++)
            marked -= d->preempt[t];
        d->policy->on_tick(d->policy, sim_now(d), d->running, d->teams, d->preempt);
        for (int t = 0; t < d->teams; t++)
            marked += d->preempt[t];
        if (marked > 0 || d->promotions + d->demotions != before)
            publish_locked(d);
    }
    pthread_mutex_unlock(&d->lock);
    return NULL;
}

static void free_dispatcher(dispatcher_t *d)
{
    if (!d)
        return;
    if (d->policy)
        sched_policy_free(d->policy);
    pthread_cond_destroy(&d->work);
    pthread_mutex_destroy(&d->lock);
    free(d->running);
    free(d->preempt);
    free(d->busy_since_ms);
    free(d->team_done);
    free(d->threads);
    free(d->args);
    free(d);
}

dispatcher_t *dispatch_start(const dispatch_config_t *cfg)
{
    dispatcher_t *d = calloc(1, sizeof *d);
    sched_params_t params;
    sigset_t all, old;
    int err = 0;

    if (!d)
        return NULL;
    pthread_mutex_init(&d->lock, NULL);
    pthread_cond_init(&d->work, NULL);
    d->teams = cfg->teams;
    d->scale_ms = cfg->time_scale_ms > 0 ? (int64_t)cfg->time_scale_ms : 1;
    d->t0_ms = mono_ms();
    d->shm = cfg->shm;
    d->running = calloc((size_t)d->teams, sizeof *d->running);
    d->preempt = calloc((size_t)d->teams, sizeof *d->preempt);
    d->busy_since_ms = calloc((size_t)d->teams, sizeof *d->busy_since_ms);
    d->team_done = calloc((size_t)d->teams, sizeof *d->team_done);
    d->threads = calloc((size_t)d->teams, sizeof *d->threads);
    d->args = calloc((size_t)d->teams, sizeof *d->args);
    if (!d->running || !d->preempt || !d->busy_since_ms || !d->team_done || !d->threads || !d->args) {
        LOG_ERROR("dispatcher: out of memory");
        free_dispatcher(d);
        return NULL;
    }

    memset(&params, 0, sizeof params);
    params.teams = cfg->teams;
    params.quantum = cfg->quantum;
    params.preemptive = cfg->preemptive;
    params.age = cfg->age;
    params.on_event = on_policy_event;
    params.event_ctx = d;
    d->policy = sched_policy_create(cfg->policy, &params);
    if (!d->policy) {
        LOG_ERROR("unknown scheduling policy '%s' (available: %s)", cfg->policy, sched_policy_names());
        free_dispatcher(d);
        return NULL;
    }

    /* Worker threads block every signal, so SIGCHLD/SIGTERM/SIGINT are always delivered to
     * the main thread, whose mq_timedreceive relies on EINTR to react promptly. New threads
     * inherit the creating thread's mask. */
    sigfillset(&all);
    pthread_sigmask(SIG_BLOCK, &all, &old);
    for (int t = 0; t < d->teams; t++) {
        d->args[t].d = d;
        d->args[t].index = t;
        if ((err = pthread_create(&d->threads[t], NULL, team_main, &d->args[t])) != 0)
            break;
        d->nthreads++;
    }
    if (d->nthreads == d->teams && (err = pthread_create(&d->clock, NULL, clock_main, d)) == 0)
        d->clock_started = 1;
    pthread_sigmask(SIG_SETMASK, &old, NULL);
    if (d->nthreads != d->teams || !d->clock_started) {
        LOG_ERROR("pthread_create: %s", strerror(err));
        dispatch_stop(d);
        return NULL;
    }

    pthread_mutex_lock(&d->lock);
    EV_INFO("sched_start", "policy=%s teams=%d preemptive=%d quantum=%d age=%lld time_scale_ms=%lld",
            d->policy->name, d->teams, d->policy->preemptive, cfg->quantum, (long long)cfg->age,
            (long long)d->scale_ms);
    publish_locked(d);
    pthread_mutex_unlock(&d->lock);
    return d;
}

int dispatch_submit(dispatcher_t *d, const relief_request_t *req)
{
    live_job_t *lj = calloc(1, sizeof *lj);
    sched_job_t *j;

    if (!lj) {
        LOG_ERROR("dispatcher: out of memory for request %u", req->id);
        return -1;
    }
    j = &lj->job;
    j->id = req->id;
    j->camp = req->camp_id;
    j->severity = req->severity < SEV_COUNT ? req->severity : SEV_ROUTINE;
    j->burst = req->service_time > 0 ? req->service_time : 1;
    lj->start_ms = -1;
    lj->arrival_ms = lj->ready_since_ms = mono_ms();

    pthread_mutex_lock(&d->lock);
    j->arrival = sim_now(d);
    sched_job_reset(j);         /* remaining = burst, start/finish = -1, ready_since = arrival */
    j->seq = d->seq++;
    d->policy->enqueue(d->policy, j, j->arrival);
    d->ready++;
    d->enqueued++;
    publish_locked(d);
    pthread_cond_signal(&d->work);
    pthread_mutex_unlock(&d->lock);
    return 0;
}

void dispatch_stop(dispatcher_t *d)
{
    sched_job_t *j;
    uint64_t pending = 0;

    if (!d)
        return;
    pthread_mutex_lock(&d->lock);
    d->stop = 1;
    pthread_cond_broadcast(&d->work);
    pthread_mutex_unlock(&d->lock);

    for (int t = 0; t < d->nthreads; t++)
        pthread_join(d->threads[t], NULL);
    if (d->clock_started)
        pthread_join(d->clock, NULL);

    /* All threads are gone: no locking needed from here on. */
    if (d->policy) {
        while ((j = d->policy->pick_next(d->policy, 0)) != NULL) {
            free((live_job_t *)j);
            pending++;
        }
        d->abandoned += pending;
        EV_INFO("sched_stop", "policy=%s enqueued=%llu completed=%llu preemptions=%llu "
                "promotions=%llu demotions=%llu abandoned=%llu",
                d->policy->name, (unsigned long long)d->enqueued,
                (unsigned long long)d->completed, (unsigned long long)d->preemptions,
                (unsigned long long)d->promotions, (unsigned long long)d->demotions,
                (unsigned long long)d->abandoned);
        d->ready = 0;
        publish_locked(d);
    }
    free_dispatcher(d);
}
