/*
 * OS concept: Deadlock characterization, prevention, avoidance, detection and recovery — a
 *             deterministic discrete-time simulator (experiment E3) — Unit 3
 *
 * dlsim replays a scenario's Banker's resources (TRUCK, BOAT, MED_KIT, FOOD_KIT) on a
 * one-minute simulated clock, with the same rules as reliefd + camps but no threads, no IPC
 * and no timing noise, so a run is a pure function of (scenario, resources, mode):
 *   - camps declare the same maximum claims (peak concurrent demand, tasks held until they
 *     complete, capped at the totals) and never ask beyond them;
 *   - a task's rows (optional 7th CSV column) are acquired one after another, held together,
 *     released together after the task's longest service time; a lone row is held for its
 *     service time;
 *   - requests wait in one FIFO queue and are retried after every change, through the same
 *     --deadlock policies (src/deadlock/deadlock.c): none, prevent (per-task resource
 *     ordering; a rejected task releases and retries in order), prevent --all-or-nothing
 *     (one bundle per task), avoid (Banker's), detect (every --detect-interval minutes: the
 *     wait-for graph, DFS, the detection algorithm, recovery by preempting a vehicle of the
 *     lowest-severity camp on the cycle and queueing its request again).
 * Deadlocks are counted at the level of jobs (a task or a lone row), the units that hold and
 * wait: the detection algorithm on jobs, every minute (a camp's timed holds always end, so
 * they never count). A run ends when every request is done, or when nothing can change any
 * more (frozen: a deadlock with no recovery).
 *
 *   dlsim --deadlock none|prevent|avoid|detect --scenario F [--all-or-nothing]
 *         [--resources F] [--seed S] [--detect-interval MIN] [--max-minutes N]
 *         [--timeline F] [--summary-csv F] [--label L]
 *
 * --seed only labels the run: the simulator itself draws no random numbers (the scenario is
 * generated from a seed). --timeline writes one row per minute (resources held, requests
 * waiting, deadlocked jobs, completed); --summary-csv appends one row of totals. Vehicle
 * utilisation counts only vehicles held by complete tasks or lone rows (doing the work);
 * vehicles held by a task that still waits for its next row are reported as idle-held.
 * Exit status: 0 ok, 1 runtime error, 2 usage error.
 */
#include "../deadlock/deadlock.h"   /* src/deadlock is not on the include path */
#include "../deadlock/waitfor.h"
#include "relief.h"
#include "resconf.h"

#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define NRES          RELIEF_BANKER_RES
#define MAX_CAMPS     BANKER_MAX_P
#define TASK_MAX_ROWS 8
#define NO_TIMER      INT64_MAX
#define DEFAULT_DETECT_INTERVAL 5           /* simulated minutes */

enum { R_WAITING = 0, R_DEFERRED, R_PENDING, R_HELD, R_DONE };

typedef struct {
    uint32_t id, camp, arrival, service, qty;
    int      sev, res, bi, units, task;     /* task: index into jobs */
    int      slot;                          /* camp slot */
    int      state;
    int64_t  due, grant, release;
} row_t;

/* A job: a task (several rows) or a lone row. */
typedef struct {
    int      slot, is_task, n, rows[TASK_MAX_ROWS];
    uint32_t id, hold;
    int      bundle;                        /* all-or-nothing: 0 not sent, 1 waiting, 2 held */
} job_t;

/* A request waiting in the allocator, or a grant. */
typedef struct sreq {
    int      bundle, idx;                   /* idx: row (single) or job (bundle) */
    int      job, slot, vec[NRES];
    int64_t  since;
    unsigned logged;
    struct sreq *next;
} sreq_t;

typedef struct { sreq_t *head, *tail; } list_t;

typedef struct {
    const deadlock_policy_t *pol;
    banker_t  bk;
    row_t    *row;
    int       nrows;
    job_t    *job;
    int       njobs;
    uint32_t  camp_id[MAX_CAMPS];
    int       ncamps;
    int       claim[MAX_CAMPS][NRES], outstanding[MAX_CAMPS][NRES];
    list_t    waiting, granted;
    int       all_or_nothing;
    int64_t   t;
    /* results */
    long      completed, unsafe_denials, preemptions, order_rejects, rejects, deadlocks;
    long      held_vehicle_minutes, idle_vehicle_minutes;   /* idle: held by a task still waiting */
    int64_t   last_event;
    unsigned char *was_dead;                /* per job, previous minute */
} sim_t;

/* ---- scenario ------------------------------------------------------------------------------ */

static int slot_for(sim_t *s, uint32_t camp)
{
    for (int i = 0; i < s->ncamps; i++)
        if (s->camp_id[i] == camp)
            return i;
    if (s->ncamps == MAX_CAMPS)
        return -1;
    s->camp_id[s->ncamps] = camp;
    return s->ncamps++;
}

static int row_cmp(const void *a, const void *b)
{
    const row_t *x = a, *y = b;
    if (x->arrival != y->arrival)
        return x->arrival < y->arrival ? -1 : 1;
    return x->id < y->id ? -1 : (x->id > y->id);
}

/* Load the Banker's rows (other resources are ignored). Returns 0 or -1. */
static int load(sim_t *s, const char *path, const int *total)
{
    FILE *f = fopen(path, "r");
    char line[256];
    unsigned lineno = 0;
    int cap = 0;
    uint32_t *task_of = NULL;

    if (!f) {
        fprintf(stderr, "dlsim: %s: %s\n", path, strerror(errno));
        return -1;
    }
    while (fgets(line, sizeof line, f)) {
        unsigned arrival, camp, qty, service, task = 0;
        char sev[16], res[16];
        lineno++;
        if (lineno == 1 || line[0] == '#' || line[0] == '\n')
            continue;
        int nf = sscanf(line, "%u,%u,%15[^,],%15[^,],%u,%u,%u", &arrival, &camp, sev, res, &qty, &service, &task);
        int sv = relief_severity_parse(sev), r = relief_resource_parse(res);
        if ((nf != 6 && nf != 7) || sv < 0 || r < 0 || qty == 0) {
            fprintf(stderr, "dlsim: %s:%u: malformed row\n", path, lineno);
            fclose(f);
            free(task_of);
            return -1;
        }
        int bi = relief_banker_index((unsigned)r);
        if (bi < 0)
            continue;
        if (s->nrows == cap) {
            cap = cap ? cap * 2 : 256;
            row_t *nr = realloc(s->row, (size_t)cap * sizeof *nr);
            uint32_t *nt = realloc(task_of, (size_t)cap * sizeof *nt);
            if (!nr || !nt) {
                fprintf(stderr, "dlsim: out of memory\n");
                free(nr ? nr : s->row);
                free(nt ? nt : task_of);
                s->row = NULL;
                fclose(f);
                return -1;
            }
            s->row = nr;
            task_of = nt;
        }
        row_t *x = &s->row[s->nrows];
        memset(x, 0, sizeof *x);
        x->id = camp * RELIEF_REQ_ID_BASE + lineno;
        x->camp = camp;
        x->arrival = arrival;
        x->service = service;
        x->qty = qty;
        x->sev = sv;
        x->res = r;
        x->bi = bi;
        unsigned u = relief_banker_units((unsigned)r, qty);
        x->units = (int)(u > (unsigned)total[bi] ? (unsigned)total[bi] : u);
        x->release = NO_TIMER;
        task_of[s->nrows] = task;
        s->nrows++;
    }
    fclose(f);
    if (!s->nrows) {
        fprintf(stderr, "dlsim: %s: no TRUCK/BOAT/MED_KIT/FOOD_KIT rows\n", path);
        free(task_of);
        return -1;
    }
    /* Sort rows by arrival (stable by id), keeping each row's task id with it. */
    for (int i = 0; i < s->nrows; i++)
        s->row[i].task = (int)task_of[i];      /* temporarily the scenario's task id */
    free(task_of);
    qsort(s->row, (size_t)s->nrows, sizeof *s->row, row_cmp);

    /* Jobs: one per (camp, task id), or per lone row. */
    s->job = calloc((size_t)s->nrows, sizeof *s->job);
    if (!s->job)
        return -1;
    for (int i = 0; i < s->nrows; i++) {
        row_t *x = &s->row[i];
        uint32_t tid = (uint32_t)x->task;
        x->slot = slot_for(s, x->camp);
        if (x->slot < 0) {
            fprintf(stderr, "dlsim: more than %d camps\n", MAX_CAMPS);
            return -1;
        }
        int j = -1;
        if (tid)
            for (int k = 0; k < s->njobs; k++)
                if (s->job[k].is_task && s->job[k].slot == x->slot && s->job[k].id == tid)
                    j = k;
        if (j < 0) {
            j = s->njobs++;
            s->job[j].slot = x->slot;
            s->job[j].is_task = tid != 0;
            s->job[j].id = tid;
        }
        job_t *jb = &s->job[j];
        if (jb->n == TASK_MAX_ROWS) {
            fprintf(stderr, "dlsim: task %u of camp %u has more than %d rows\n", tid, x->camp, TASK_MAX_ROWS);
            return -1;
        }
        jb->rows[jb->n++] = i;
        if (x->service > jb->hold)
            jb->hold = x->service;
        x->task = j;
    }
    return 0;
}

/* The camps' maximum claims: as src/camp/main.c (peak concurrent demand, a task's rows held
 * until its last row has arrived plus its longest service time), capped at the totals. */
static void claims(sim_t *s, const int *total)
{
    for (int i = 0; i < s->nrows; i++) {
        const row_t *x = &s->row[i];
        int held = 0;
        for (int k = 0; k < s->nrows; k++) {
            const row_t *y = &s->row[k];
            if (y->slot != x->slot || y->bi != x->bi)
                continue;
            const job_t *jb = &s->job[y->task];
            uint32_t end = y->arrival + (y->service ? y->service : 1);
            if (jb->is_task) {
                uint32_t last = 0;
                for (int r = 0; r < jb->n; r++)
                    if (s->row[jb->rows[r]].arrival > last)
                        last = s->row[jb->rows[r]].arrival;
                end = last + (jb->hold ? jb->hold : 1);
            }
            if (y->arrival <= x->arrival && x->arrival < end)
                held += y->units;
        }
        if (held > s->claim[x->slot][x->bi])
            s->claim[x->slot][x->bi] = held;
    }
    for (int c = 0; c < s->ncamps; c++) {
        for (int j = 0; j < NRES; j++)
            if (s->claim[c][j] > total[j])
                s->claim[c][j] = total[j];
        banker_set_max(&s->bk, c, s->claim[c]);
    }
}

/* ---- lists ---------------------------------------------------------------------------------- */

static void append(list_t *l, sreq_t *q)
{
    q->next = NULL;
    if (l->tail)
        l->tail->next = q;
    else
        l->head = q;
    l->tail = q;
}

static void unlink_(list_t *l, sreq_t *prev, sreq_t *q)
{
    if (prev)
        prev->next = q->next;
    else
        l->head = q->next;
    if (l->tail == q)
        l->tail = prev;
}

static void free_list(list_t *l)
{
    while (l->head) {
        sreq_t *q = l->head;
        l->head = q->next;
        free(q);
    }
    l->tail = NULL;
}

/* ---- camp side ------------------------------------------------------------------------------- */

static int task_next(const sim_t *s, const job_t *jb)
{
    for (int r = 0; r < jb->n; r++)
        if (s->row[jb->rows[r]].state != R_HELD)
            return jb->rows[r];
    return -1;
}

static int job_has(const sim_t *s, const job_t *jb, int state)
{
    for (int r = 0; r < jb->n; r++)
        if (s->row[jb->rows[r]].state == state)
            return 1;
    return 0;
}

static void job_sort(sim_t *s, job_t *jb)
{
    for (int a = 1; a < jb->n; a++)
        for (int b = a; b > 0 && s->row[jb->rows[b - 1]].bi > s->row[jb->rows[b]].bi; b--) {
            int tmp = jb->rows[b];
            jb->rows[b] = jb->rows[b - 1];
            jb->rows[b - 1] = tmp;
        }
}

static sreq_t *new_req(sim_t *s, int bundle, int idx, int job, int slot)
{
    sreq_t *q = calloc(1, sizeof *q);
    if (!q) {
        fprintf(stderr, "dlsim: out of memory\n");
        exit(1);
    }
    q->bundle = bundle;
    q->idx = idx;
    q->job = job;
    q->slot = slot;
    q->since = s->t;
    return q;
}

static int job_started(const sim_t *s, const job_t *jb)
{
    return job_has(s, jb, R_PENDING) || job_has(s, jb, R_HELD);
}

/* Units of resource j a camp has committed: lone rows asked for or held, plus the whole of
 * every task it has started (reserved until the task ends). A camp starts a task only when
 * the whole task fits in its claim, so a started task can always finish within the claim:
 * no deadlock between two tasks of the same camp. */
static int committed(const sim_t *s, int slot, int j)
{
    int u = 0;
    for (int k = 0; k < s->njobs; k++) {
        const job_t *jb = &s->job[k];
        if (jb->slot != slot)
            continue;
        int started = job_started(s, jb);
        for (int r = 0; r < jb->n; r++) {
            const row_t *y = &s->row[jb->rows[r]];
            if (y->bi == j && y->state != R_DONE &&
                (y->state == R_PENDING || y->state == R_HELD || (jb->is_task && started)))
                u += y->units;
        }
    }
    return u;
}

/* Camps send what they may (the rules of src/camp/main.c). Returns 1 if anything was sent. */
static int send_pass(sim_t *s)
{
    int changed = 0;
    for (int i = 0; i < s->nrows; i++) {
        row_t *x = &s->row[i];
        job_t *jb = &s->job[x->task];
        if (x->state != R_DEFERRED)
            continue;
        if (s->all_or_nothing && jb->is_task) {
            if (jb->bundle || jb->rows[0] != i)
                continue;
            int vec[NRES] = { 0 }, ok = 1;
            for (int r = 0; r < jb->n; r++) {
                const row_t *y = &s->row[jb->rows[r]];
                ok &= y->state == R_DEFERRED;
                vec[y->bi] += y->units;
            }
            for (int j = 0; j < NRES && ok; j++)
                ok = committed(s, x->slot, j) + vec[j] <= s->claim[x->slot][j];
            if (!ok)
                continue;
            sreq_t *q = new_req(s, 1, x->task, x->task, x->slot);
            memcpy(q->vec, vec, sizeof vec);
            append(&s->waiting, q);
            for (int r = 0; r < jb->n; r++)
                s->row[jb->rows[r]].state = R_PENDING;
            for (int j = 0; j < NRES; j++)
                s->outstanding[x->slot][j] += vec[j];
            jb->bundle = 1;
            changed = 1;
            continue;
        }
        if (jb->is_task && (job_has(s, jb, R_PENDING) || task_next(s, jb) != i))
            continue;
        if (jb->is_task && !job_started(s, jb)) {
            int ok = 1, need[NRES] = { 0 };
            for (int r = 0; r < jb->n; r++)
                need[s->row[jb->rows[r]].bi] += s->row[jb->rows[r]].units;
            for (int j = 0; j < NRES && ok; j++)
                ok = committed(s, x->slot, j) + need[j] <= s->claim[x->slot][j];
            if (!ok)
                continue;               /* the whole task must fit in the claim */
        } else if (!jb->is_task && committed(s, x->slot, x->bi) + x->units > s->claim[x->slot][x->bi]) {
            continue;
        }
        sreq_t *q = new_req(s, 0, i, x->task, x->slot);
        q->vec[x->bi] = x->units;
        append(&s->waiting, q);
        x->state = R_PENDING;
        s->outstanding[x->slot][x->bi] += x->units;
        changed = 1;
    }
    return changed;
}

/* A grant reached the camp. */
static void granted(sim_t *s, sreq_t *q)
{
    job_t *jb = &s->job[q->job];
    if (q->bundle) {
        for (int r = 0; r < jb->n; r++) {
            row_t *y = &s->row[jb->rows[r]];
            y->state = R_HELD;
            y->grant = s->t;
            y->release = s->t + jb->hold;
        }
        jb->bundle = 2;
        return;
    }
    row_t *x = &s->row[q->idx];
    x->state = R_HELD;
    x->grant = s->t;
    if (!jb->is_task) {
        x->release = s->t + x->service;
    } else {
        x->release = NO_TIMER;
        if (task_next(s, jb) < 0)
            for (int r = 0; r < jb->n; r++)
                s->row[jb->rows[r]].release = s->t + jb->hold;
    }
}

/* Remove and hand back the grant of row/job idx. */
static void release_grant(sim_t *s, int bundle, int idx)
{
    sreq_t *prev = NULL;
    for (sreq_t *q = s->granted.head; q; prev = q, q = q->next) {
        if (q->bundle != bundle || q->idx != idx)
            continue;
        banker_release(&s->bk, q->slot, q->vec);
        for (int j = 0; j < NRES; j++)
            s->outstanding[q->slot][j] -= q->vec[j];
        unlink_(&s->granted, prev, q);
        free(q);
        return;
    }
}

/* ---- allocator (the rules of src/reliefd/resmgr.c) ----------------------------------------- */

static int alloc_pass(sim_t *s)
{
    int changed = 0;
    sreq_t *prev = NULL, *q = s->waiting.head;
    while (q) {
        sreq_t *next = q->next;
        int held[NRES] = { 0 }, seq[BANKER_MAX_P], len;
        if (s->job[q->job].is_task)
            for (const sreq_t *g = s->granted.head; g; g = g->next)
                if (g->job == q->job)
                    for (int j = 0; j < NRES; j++)
                        held[j] += g->vec[j];
        banker_result_t res = s->pol->request(&s->bk, q->slot, q->vec, held, seq, &len);
        if (res == BANKER_GRANT) {
            unlink_(&s->waiting, prev, q);
            append(&s->granted, q);
            granted(s, q);
            changed = 1;
        } else if (res == BANKER_ORDER) {
            /* The camp releases what the task holds and retries it in resource order. */
            job_t *jb = &s->job[q->job];
            unlink_(&s->waiting, prev, q);
            s->row[q->idx].state = R_DEFERRED;
            s->outstanding[q->slot][s->row[q->idx].bi] -= s->row[q->idx].units;
            free(q);
            for (int r = 0; r < jb->n; r++) {
                row_t *y = &s->row[jb->rows[r]];
                if (y->state == R_HELD) {
                    release_grant(s, 0, jb->rows[r]);
                    y->state = R_DEFERRED;
                    y->release = NO_TIMER;
                }
            }
            job_sort(s, jb);
            s->order_rejects++;
            changed = 1;
            q = next;
            continue;
        } else if (res == BANKER_OVER_CLAIM || res == BANKER_INACTIVE) {
            unlink_(&s->waiting, prev, q);
            job_t *jb = &s->job[q->job];
            for (int r = 0; r < (q->bundle ? jb->n : 1); r++)
                s->row[q->bundle ? jb->rows[r] : q->idx].state = R_DONE;
            for (int j = 0; j < NRES; j++)
                s->outstanding[q->slot][j] -= q->vec[j];
            free(q);
            s->rejects++;
            changed = 1;
            q = next;
            continue;
        } else {
            if (res == BANKER_UNSAFE && !(q->logged & 1u)) {
                s->unsafe_denials++;
                q->logged |= 1u;
            }
            prev = q;
        }
        q = next;
    }
    return changed;
}

/* ---- detection and recovery (detect) ---------------------------------------------------------- */

static int detect_and_recover(sim_t *s)
{
    static int req[BANKER_MAX_P][BANKER_MAX_R];
    static waitfor_t g;
    unsigned char dead[BANKER_MAX_P];
    int cycle[BANKER_MAX_P], len;

    memset(req, 0, sizeof req);
    for (const sreq_t *q = s->waiting.head; q; q = q->next)
        for (int j = 0; j < NRES; j++)
            req[q->slot][j] += q->vec[j];
    waitfor_build(&g, &s->bk, (const int (*)[BANKER_MAX_R])req);
    if (!waitfor_find_cycle(&g, cycle, &len))
        return 0;
    banker_detect(&s->bk, (const int (*)[BANKER_MAX_R])req, dead);
    for (int i = 0; i < len; i++)
        if (!dead[cycle[i]])
            return 0;
    /* Victim: the cycle member whose waiting request is least severe. */
    int victim = cycle[0], vsev = -1, pos = 0;
    for (int i = 0; i < len; i++) {
        int worst = -1;
        for (const sreq_t *q = s->waiting.head; q; q = q->next)
            if (q->slot == cycle[i]) {
                int sv = q->bundle ? s->row[s->job[q->idx].rows[0]].sev : s->row[q->idx].sev;
                if (sv > worst)
                    worst = sv;
            }
        if (worst > vsev || (worst == vsev && s->camp_id[cycle[i]] > s->camp_id[victim])) {
            vsev = worst;
            victim = cycle[i];
            pos = i;
        }
    }
    unsigned wanted = g.label[cycle[(pos + len - 1) % len]][victim];
    sreq_t *prev = NULL, *pick = NULL, *pick_prev = NULL;
    int pick_vehicle = 0;
    for (sreq_t *q = s->granted.head; q; prev = q, q = q->next) {
        if (q->slot != victim)
            continue;
        unsigned has = 0;
        for (int j = 0; j < NRES; j++)
            if (q->vec[j] > 0)
                has |= 1u << j;
        if (!(has & wanted))
            continue;
        int vehicle = (has & wanted & ((1u << BK_TRUCK) | (1u << BK_BOAT))) != 0;
        if (!pick || vehicle >= pick_vehicle) {
            pick = q;
            pick_prev = prev;
            pick_vehicle = vehicle;
        }
    }
    if (!pick)
        return 0;
    /* Roll the grant back and queue the request again; the camp waits for it again. */
    banker_release(&s->bk, victim, pick->vec);
    unlink_(&s->granted, pick_prev, pick);
    job_t *jb = &s->job[pick->job];
    if (pick->bundle) {
        for (int r = 0; r < jb->n; r++) {
            s->row[jb->rows[r]].state = R_PENDING;
            s->row[jb->rows[r]].release = NO_TIMER;
        }
        jb->bundle = 1;
    } else {
        s->row[pick->idx].state = R_PENDING;
        if (jb->is_task)
            for (int r = 0; r < jb->n; r++)
                s->row[jb->rows[r]].release = NO_TIMER;
    }
    pick->since = s->t;
    pick->logged = 0;
    append(&s->waiting, pick);
    s->preemptions++;
    return 1;
}

/* ---- deadlock truth: the detection algorithm over jobs ----------------------------------------- */

/* Marks dead[job]; returns the number of deadlocked jobs. A job that waits for nothing (or
 * only holds things on a timer) can always finish. */
static int deadlocked_jobs(sim_t *s, unsigned char *dead)
{
    int work[NRES], n = s->njobs, stuck = 0, progress;
    int (*alloc)[NRES] = calloc((size_t)n, sizeof *alloc), (*want)[NRES] = calloc((size_t)n, sizeof *want);
    unsigned char *fin = calloc((size_t)n, 1);
    if (!alloc || !want || !fin) {
        fprintf(stderr, "dlsim: out of memory\n");
        exit(1);
    }
    for (int j = 0; j < NRES; j++)
        work[j] = s->bk.available[j];
    for (const sreq_t *q = s->granted.head; q; q = q->next)
        for (int j = 0; j < NRES; j++)
            alloc[q->job][j] += q->vec[j];
    for (const sreq_t *q = s->waiting.head; q; q = q->next)
        for (int j = 0; j < NRES; j++)
            want[q->job][j] += q->vec[j];
    for (int k = 0; k < n; k++) {
        int wants = 0, holds = 0;
        for (int j = 0; j < NRES; j++) {
            wants |= want[k][j] > 0;
            holds |= alloc[k][j] > 0;
        }
        fin[k] = !wants || !holds;
        if (!wants)                         /* its hold time ends: what it holds comes back */
            for (int j = 0; j < NRES; j++)
                work[j] += alloc[k][j];
    }
    do {
        progress = 0;
        for (int k = 0; k < n; k++) {
            if (fin[k])
                continue;
            int fits = 1;
            for (int j = 0; j < NRES && fits; j++)
                fits = want[k][j] <= work[j];
            if (!fits)
                continue;
            for (int j = 0; j < NRES; j++)
                work[j] += alloc[k][j];
            fin[k] = 1;
            progress = 1;
        }
    } while (progress);
    /* Jobs that hold nothing but wait behind deadlocked ones are blocked, not deadlocked
     * (Finish = true for them from the start). */
    for (int k = 0; k < n; k++) {
        dead[k] = !fin[k];
        stuck += dead[k];
    }
    free(alloc);
    free(want);
    free(fin);
    return stuck;
}

/* ---- main loop ----------------------------------------------------------------------------- */

static void usage(FILE *f)
{
    fprintf(f, "usage: dlsim --deadlock none|prevent|avoid|detect --scenario F [--all-or-nothing]\n"
               "             [--resources F] [--seed S] [--detect-interval MIN] [--max-minutes N]\n"
               "             [--timeline F] [--summary-csv F] [--label L]\n");
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "deadlock", required_argument, NULL, 'd' }, { "scenario", required_argument, NULL, 's' },
        { "all-or-nothing", no_argument, NULL, 'a' }, { "resources", required_argument, NULL, 'r' },
        { "seed", required_argument, NULL, 'S' }, { "detect-interval", required_argument, NULL, 'i' },
        { "max-minutes", required_argument, NULL, 'm' }, { "timeline", required_argument, NULL, 't' },
        { "summary-csv", required_argument, NULL, 'c' }, { "label", required_argument, NULL, 'l' },
        { "help", no_argument, NULL, 'h' }, { NULL, 0, NULL, 0 }
    };
    const char *mode = NULL, *scenario = NULL, *resources = RELIEF_RESOURCES_CONF, *timeline = NULL;
    const char *summary = NULL, *label = NULL;
    long seed = 0, detect_every = DEFAULT_DETECT_INTERVAL, max_minutes = 1000000;
    int all_or_nothing = 0, c, total[NRES];
    char err[512], *end;
    sim_t s;

    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'd': mode = optarg; break;
        case 's': scenario = optarg; break;
        case 'a': all_or_nothing = 1; break;
        case 'r': resources = optarg; break;
        case 't': timeline = optarg; break;
        case 'c': summary = optarg; break;
        case 'l': label = optarg; break;
        case 'S': case 'i': case 'm': {
            errno = 0;
            long v = strtol(optarg, &end, 10);
            if (errno || *end || end == optarg || v < (c == 'S' ? 0 : 1)) {
                fprintf(stderr, "dlsim: bad --%s '%s'\n", c == 'S' ? "seed" : c == 'i' ? "detect-interval"
                        : "max-minutes", optarg);
                return 2;
            }
            if (c == 'S') seed = v; else if (c == 'i') detect_every = v; else max_minutes = v;
            break;
        }
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
    }
    if (!mode || !scenario || optind < argc) {
        usage(stderr);
        return 2;
    }
    if (all_or_nothing) {
        if (strcmp(mode, "prevent")) {
            fprintf(stderr, "dlsim: --all-or-nothing needs --deadlock prevent\n");
            return 2;
        }
        mode = "prevent-all";
    }
    memset(&s, 0, sizeof s);
    s.pol = deadlock_policy_find(mode);
    if (!s.pol) {
        fprintf(stderr, "dlsim: unknown --deadlock '%s' (%s)\n", mode, deadlock_policy_names());
        return 2;
    }
    s.all_or_nothing = s.pol->all_or_nothing;
    if (relief_resources_load(resources, total, err, sizeof err) == -1) {
        fprintf(stderr, "dlsim: %s\n", err);
        return 1;
    }
    if (load(&s, scenario, total) == -1) {
        free(s.row);
        free(s.job);
        return 1;
    }
    banker_init(&s.bk, s.ncamps, NRES, total);
    claims(&s, total);
    s.was_dead = calloc((size_t)s.njobs, 1);
    unsigned char *dead = calloc((size_t)s.njobs, 1);
    FILE *tl = timeline ? fopen(timeline, "w") : NULL;
    if (!s.was_dead || !dead || (timeline && !tl)) {
        fprintf(stderr, "dlsim: %s\n", timeline && !tl ? strerror(errno) : "out of memory");
        return 1;
    }
    if (tl)
        fprintf(tl, "t,trucks_held,boats_held,med_held,food_held,waiting,deadlocked_jobs,completed\n");

    int next = 0, frozen = 0;
    int64_t last_arrival = s.row[s.nrows - 1].arrival, freeze_at = -1, delay_sum = 0, delay_max = 0;
    for (s.t = 0; s.t <= max_minutes; s.t++) {
        int changed = 0;
        /* 1. Hold times that end now. */
        for (int i = 0; i < s.nrows; i++) {
            row_t *x = &s.row[i];
            if (x->state != R_HELD || x->release > s.t)
                continue;
            job_t *jb = &s.job[x->task];
            if (jb->bundle == 2) {
                release_grant(&s, 1, x->task);
                for (int r = 0; r < jb->n; r++)
                    s.row[jb->rows[r]].state = R_DONE;
                jb->bundle = 3;
            } else {
                release_grant(&s, 0, i);
                x->state = R_DONE;
            }
            changed = 1;
        }
        /* 2. Rows that arrive now. */
        while (next < s.nrows && s.row[next].arrival <= s.t) {
            s.row[next].state = R_DEFERRED;
            s.row[next].due = s.t;
            next++;
            changed = 1;
        }
        /* 3. Camps send, the allocator decides, until nothing changes. */
        for (int guard = 0; guard < 10000; guard++) {
            int a = send_pass(&s), b = alloc_pass(&s);
            if (!a && !b)
                break;
            changed = 1;
        }
        /* 4. The detector. */
        if (s.pol->detector && s.t % detect_every == 0 && detect_and_recover(&s)) {
            changed = 1;
            for (int guard = 0; guard < 10000 && (send_pass(&s) | alloc_pass(&s)); guard++)
                ;
        }
        /* 5. Statistics: deadlocked jobs, resources held. */
        int nd = deadlocked_jobs(&s, dead), fresh = 0;
        for (int k = 0; k < s.njobs; k++) {
            fresh |= dead[k] && !s.was_dead[k];
            s.was_dead[k] = dead[k];
        }
        s.deadlocks += fresh;
        int held[NRES] = { 0 }, waiting = 0;
        for (const sreq_t *q = s.granted.head; q; q = q->next)
            for (int j = 0; j < NRES; j++)
                held[j] += q->vec[j];
        for (const sreq_t *q = s.waiting.head; q; q = q->next)
            waiting++;
        s.held_vehicle_minutes += held[BK_TRUCK] + held[BK_BOAT];
        for (const sreq_t *q = s.granted.head; q; q = q->next)
            if (s.job[q->job].is_task && !q->bundle && task_next(&s, &s.job[q->job]) >= 0)
                s.idle_vehicle_minutes += q->vec[BK_TRUCK] + q->vec[BK_BOAT];
        long done = 0;
        for (int i = 0; i < s.nrows; i++)
            done += s.row[i].state == R_DONE;
        if (changed)
            s.last_event = s.t;
        if (tl)
            fprintf(tl, "%lld,%d,%d,%d,%d,%d,%d,%ld\n", (long long)s.t, held[0], held[1], held[2], held[3],
                    waiting, nd, done);
        if (done == s.nrows)
            break;
        /* 6. Frozen? Nothing arrives any more, no hold time is running, and nothing changed. */
        int timers = 0;
        for (int i = 0; i < s.nrows && !timers; i++)
            timers = s.row[i].state == R_HELD && s.row[i].release != NO_TIMER;
        if (!changed && next == s.nrows && !timers && s.t > last_arrival &&
            !(s.pol->detector && s.t % detect_every != 0)) {
            frozen = 1;
            freeze_at = s.last_event;
            break;
        }
    }
    if (tl && fclose(tl) != 0) {
        perror("dlsim: timeline");
        return 1;
    }

    for (int i = 0; i < s.nrows; i++) {
        const row_t *x = &s.row[i];
        if (x->state != R_DONE || x->grant < x->due)
            continue;
        s.completed++;
        int64_t d = x->grant - x->due;
        delay_sum += d;
        if (d > delay_max)
            delay_max = d;
    }
    int64_t makespan = s.last_event > 0 ? s.last_event : 1;
    double thr = 60.0 * (double)s.completed / (double)makespan;
    double avg = s.completed ? (double)delay_sum / (double)s.completed : 0.0;
    double cap = (double)(total[BK_TRUCK] + total[BK_BOAT]) * (double)(s.t + 1);
    double util = (double)(s.held_vehicle_minutes - s.idle_vehicle_minutes) / cap;   /* useful work */
    double idle = (double)s.idle_vehicle_minutes / cap;
    const char *name = s.pol->name;

    printf("dlsim: --deadlock %s, %s (%d requests, %d jobs, %d camps), resources %d/%d/%d/%d\n", name, scenario,
           s.nrows, s.njobs, s.ncamps, total[0], total[1], total[2], total[3]);
    printf("  completed %ld/%d, deadlocks reached %ld%s, makespan %lld min, throughput %.2f requests/hour\n",
           s.completed, s.nrows, s.deadlocks, frozen ? " (FROZEN)" : "", (long long)makespan, thr);
    printf("  delay (request due -> granted): average %.1f min, max %lld min\n", avg, (long long)delay_max);
    printf("  unsafe denials %ld, preemptions %ld, out-of-order rejects %ld, rejects %ld\n",
           s.unsafe_denials, s.preemptions, s.order_rejects, s.rejects);
    printf("  vehicles: %.1f%% busy on complete tasks, %.1f%% held idle by tasks still waiting, %.1f%% free\n",
           100.0 * util, 100.0 * idle, 100.0 * (1.0 - util - idle));
    if (frozen)
        printf("  frozen at minute %lld: %d jobs deadlocked, nothing can change\n", (long long)freeze_at,
               deadlocked_jobs(&s, dead));

    int rc = 0;
    if (summary) {
        FILE *f = fopen(summary, "a");
        if (!f) {
            fprintf(stderr, "dlsim: %s: %s\n", summary, strerror(errno));
            rc = 1;
        } else {
            if (ftell(f) == 0)
                fprintf(f, "label,mode,scenario,seed,requests,completed,deadlocks,frozen,freeze_min,makespan,"
                           "throughput_per_hour,avg_delay,max_delay,unsafe_denials,preemptions,order_rejects,"
                           "vehicle_utilisation,vehicle_idle_held\n");
            const char *base = strrchr(scenario, '/');
            fprintf(f, "%s,%s,%s,%ld,%d,%ld,%ld,%d,%lld,%lld,%.3f,%.2f,%lld,%ld,%ld,%ld,%.4f,%.4f\n",
                    label ? label : name, name, base ? base + 1 : scenario, seed, s.nrows, s.completed,
                    s.deadlocks, frozen, (long long)freeze_at, (long long)makespan, thr, avg,
                    (long long)delay_max, s.unsafe_denials, s.preemptions, s.order_rejects, util, idle);
            if (fclose(f) != 0)
                rc = 1;
        }
    }
    free_list(&s.waiting);
    free_list(&s.granted);
    free(dead);
    free(s.was_dead);
    free(s.row);
    free(s.job);
    return rc;
}
