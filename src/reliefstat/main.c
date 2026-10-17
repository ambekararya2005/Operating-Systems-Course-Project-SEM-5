/*
 * OS concept: IPC via POSIX shared memory — a read-only observer process — Unit 2;
 *             lock-free consistent reads with a seqlock — Unit 3
 *
 * reliefstat: print reliefd's live status table.
 *   reliefstat          human-readable tables: camps, the live scheduler, the loading dock
 *                       and the inventory
 *   reliefstat --sched  only the scheduler: policy, ready queue per level, each team's work
 *   reliefstat --json   one JSON object (for scripts and later UIs); with --sched, only the
 *                       scheduler object
 *   reliefstat --banker the Banker's state in textbook layout: Allocation, Max, Need and
 *                       Available per camp (P<id>), plus a safe sequence of the current state
 *                       (the safety algorithm run on the snapshot); with --json, as JSON
 *
 * The segment is mapped PROT_READ: reliefstat can never corrupt it, and it does not take
 * the writers' mutex (locking would write to the mutex). relief_shm_snapshot() copies
 * the table and retries if a writer was active meanwhile.
 *
 * Exit status: 0 = reliefd running, 1 = not running or stale segment, 2 = usage error.
 */
#include "../deadlock/banker.h"     /* src/deadlock is not on the include path */
#include "relief.h"
#include "shm.h"

#include <errno.h>
#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>

#define HEARTBEAT_STALE_MS 3000     /* camps beat every second */

static int pid_alive(pid_t pid)
{
    return pid > 0 && (kill(pid, 0) == 0 || errno == EPERM);
}

static void print_json_string(const char *s)
{
    putchar('"');
    for (; *s; s++) {
        unsigned char ch = (unsigned char)*s;
        if (ch == '"' || ch == '\\')
            printf("\\%c", ch);
        else if (ch < 0x20)
            printf("\\u%04x", ch);
        else
            putchar(ch);
    }
    putchar('"');
}

/* Name of ready-queue level l for this policy. */
static const char *level_name(const relief_status_t *st, unsigned l, char *buf, size_t len)
{
    if (strncmp(st->policy, "prio", 4) == 0)
        return relief_severity_name(l);
    if (st->nlevels > 1)
        snprintf(buf, len, "Q%u", l);
    else
        snprintf(buf, len, "ready");
    return buf;
}

static void print_sched_json(const relief_status_t *st, int64_t now)
{
    printf("{\"policy\":");
    print_json_string(st->policy);
    printf(",\"teams\":%u,\"enqueued\":%llu,\"completed\":%llu,\"preemptions\":%llu,"
           "\"promotions\":%llu,\"queue\":[",
           st->nteams, (unsigned long long)st->sched_enqueued,
           (unsigned long long)st->sched_completed, (unsigned long long)st->sched_preemptions,
           (unsigned long long)st->sched_promotions);
    for (uint32_t l = 0; l < st->nlevels && l < RELIEF_SHM_LEVELS; l++) {
        char buf[16];
        printf("%s{\"level\":\"%s\",\"ready\":%llu}", l ? "," : "", level_name(st, l, buf, sizeof buf),
               (unsigned long long)st->queue_len[l]);
    }
    printf("],\"team_list\":[");
    for (uint32_t t = 0; t < st->nteams && t < RELIEF_SHM_TEAMS; t++) {
        const relief_shm_team_t *tm = &st->teams[t];
        printf("%s{\"team\":%u,\"busy\":%s,\"request\":%u,\"camp\":%u,\"severity\":\"%s\","
               "\"level\":%u,\"remaining\":%lld,\"busy_ms\":%lld,\"completed\":%llu}",
               t ? "," : "", t + 1, tm->state ? "true" : "false", tm->job_id, tm->camp,
               tm->state ? relief_severity_name(tm->severity) : "", tm->level,
               (long long)tm->remaining, tm->state ? (long long)(now - tm->busy_since_ms) : 0LL,
               (unsigned long long)tm->completed);
    }
    printf("]}");
}

static const char *const g_kits[RELIEF_KIT_KINDS] = { "FOOD_KIT", "MED_KIT", "HYGIENE_KIT" };

static void print_dock_json(const relief_status_t *st)
{
    printf("{\"capacity\":%u,\"occupancy\":%u,\"max_occupancy\":%u,\"producers\":%u,\"trucks\":%u,"
           "\"orders\":%llu,\"produced\":%llu,\"consumed\":%llu,\"delivered\":%llu,"
           "\"blocked_puts\":%llu,\"blocked_min\":%.1f,\"inventory_lock\":",
           st->dock_capacity, st->dock_occupancy, st->dock_max_occupancy, st->dock_producers,
           st->dock_trucks, (unsigned long long)st->dock_orders, (unsigned long long)st->dock_produced,
           (unsigned long long)st->dock_consumed, (unsigned long long)st->dock_delivered,
           (unsigned long long)st->dock_blocked_puts, st->dock_blocked_min);
    print_json_string(st->inv_lock);
    printf(",\"inventory\":[");
    for (int w = 0; w < RELIEF_WAREHOUSES; w++) {
        printf("%s{\"warehouse\":%d", w ? "," : "", w + 1);
        for (int k = 0; k < RELIEF_KIT_KINDS; k++)
            printf(",\"%s\":%lld", g_kits[k], (long long)st->inventory[w][k]);
        printf("}");
    }
    printf("]}");
}

static void print_dock_table(const relief_status_t *st)
{
    printf("loading dock: %u/%u packages (max %u), %u producers, %u trucks; orders %llu, produced %llu, "
           "consumed %llu, delivered %llu\n",
           st->dock_occupancy, st->dock_capacity, st->dock_max_occupancy, st->dock_producers,
           st->dock_trucks, (unsigned long long)st->dock_orders, (unsigned long long)st->dock_produced,
           (unsigned long long)st->dock_consumed, (unsigned long long)st->dock_delivered);
    printf("  producers blocked on a full dock: %llu puts, %.1f simulated min\n",
           (unsigned long long)st->dock_blocked_puts, st->dock_blocked_min);
    printf("inventory (lock: %s)\n  %-9s %9s %9s %12s\n", st->inv_lock[0] ? st->inv_lock : "-",
           "WAREHOUSE", g_kits[0], g_kits[1], g_kits[2]);
    for (int w = 0; w < RELIEF_WAREHOUSES; w++)
        printf("  %-9d %9lld %9lld %12lld\n", w + 1, (long long)st->inventory[w][0],
               (long long)st->inventory[w][1], (long long)st->inventory[w][2]);
}

/* Rebuild the Banker's state from the snapshot (active rows only) and run the safety
 * algorithm. Returns the number of active rows; rows[] maps banker process -> bk_proc index. */
static int banker_from_snapshot(const relief_status_t *st, banker_t *b, int rows[], int seq[], int *len,
                                int *safe)
{
    int n = 0;
    for (uint32_t i = 0; i < st->bk_nproc && i < RELIEF_SHM_CAMPS; i++)
        if (st->bk_proc[i].active)
            rows[n++] = (int)i;
    banker_init(b, n, RELIEF_BANKER_RES, st->bk_total);
    for (int p = 0; p < n; p++) {
        banker_set_max(b, p, st->bk_proc[rows[p]].max);
        banker_set_alloc(b, p, st->bk_proc[rows[p]].alloc);
    }
    *safe = banker_is_safe(b, seq, len);
    return n;
}

static void print_banker_table(const relief_status_t *st)
{
    banker_t b;
    int rows[RELIEF_SHM_CAMPS], seq[RELIEF_SHM_CAMPS], len = 0, safe, n;
    static const char *const hdr = " TRUCK BOAT  MED FOOD";

    n = banker_from_snapshot(st, &b, rows, seq, &len, &safe);
    printf("Banker's algorithm (--deadlock %s): resources TRUCK, BOAT, MED_KIT, FOOD_KIT "
           "(kits in crates of %d)\n", st->dl_mode[0] ? st->dl_mode : "-", RELIEF_KITS_PER_CRATE);
    printf("  grants %llu, denied as unsafe %llu, waited for availability %llu, releases %llu, "
           "rejected %llu; waiting now %u\n\n",
           (unsigned long long)st->bk_grants, (unsigned long long)st->bk_denied_unsafe,
           (unsigned long long)st->bk_waits_unavailable, (unsigned long long)st->bk_releases,
           (unsigned long long)st->bk_rejects, st->bk_pending);
    printf("  %-6s %-21s   %-21s   %-21s   %-21s %s\n", "", "Allocation", "Max", "Need", "Available",
           "WAITING");
    printf("  %-6s %s   %s   %s   %s\n", "", hdr, hdr, hdr, hdr);
    for (int p = 0; p < n; p++) {
        char name[16];
        snprintf(name, sizeof name, "P%u", st->bk_proc[rows[p]].camp);
        printf("  %-6s", name);
        for (int part = 0; part < 3; part++) {
            for (int j = 0; j < RELIEF_BANKER_RES; j++) {
                int v = part == 0 ? b.alloc[p][j] : part == 1 ? b.max[p][j] : banker_need(&b, p, j);
                printf(j ? " %4d" : " %5d", v);
            }
            printf("  ");
        }
        if (p == 0)
            printf(" %5d %4d %4d %4d", b.available[0], b.available[1], b.available[2], b.available[3]);
        else
            printf(" %21s", "");
        printf(" %7u\n", st->bk_proc[rows[p]].pending);
    }
    if (n == 0)
        printf("  (no active camps)\n");
    printf("  %-6s %21s   %21s   %21s   %5d %4d %4d %4d\n", "Total", "", "", "", st->bk_total[0],
           st->bk_total[1], st->bk_total[2], st->bk_total[3]);
    printf("\nSafe sequence: ");
    if (safe) {
        printf("<");
        for (int i = 0; i < len; i++)
            printf("%sP%u", i ? ", " : "", st->bk_proc[rows[seq[i]]].camp);
        printf(">  (the current state is SAFE)\n");
    } else {
        printf("none: the current state is UNSAFE\n");
    }
}

static void print_banker_json(const relief_status_t *st)
{
    banker_t b;
    int rows[RELIEF_SHM_CAMPS], seq[RELIEF_SHM_CAMPS], len = 0, safe, n;

    n = banker_from_snapshot(st, &b, rows, seq, &len, &safe);
    printf("{\"mode\":");
    print_json_string(st->dl_mode);
    printf(",\"resources\":[\"TRUCK\",\"BOAT\",\"MED_KIT\",\"FOOD_KIT\"],\"total\":[%d,%d,%d,%d],"
           "\"available\":[%d,%d,%d,%d],\"grants\":%llu,\"denied_unsafe\":%llu,\"waits_unavailable\":%llu,"
           "\"releases\":%llu,\"rejects\":%llu,\"waiting\":%u,\"safe\":%s,\"safe_sequence\":[",
           st->bk_total[0], st->bk_total[1], st->bk_total[2], st->bk_total[3], st->bk_available[0],
           st->bk_available[1], st->bk_available[2], st->bk_available[3],
           (unsigned long long)st->bk_grants, (unsigned long long)st->bk_denied_unsafe,
           (unsigned long long)st->bk_waits_unavailable, (unsigned long long)st->bk_releases,
           (unsigned long long)st->bk_rejects, st->bk_pending, safe ? "true" : "false");
    for (int i = 0; safe && i < len; i++)
        printf("%s%u", i ? "," : "", st->bk_proc[rows[seq[i]]].camp);
    printf("],\"processes\":[");
    for (int p = 0; p < n; p++) {
        printf("%s{\"camp\":%u,\"allocation\":[%d,%d,%d,%d],\"max\":[%d,%d,%d,%d],\"need\":[%d,%d,%d,%d],"
               "\"waiting\":%u}", p ? "," : "", st->bk_proc[rows[p]].camp, b.alloc[p][0], b.alloc[p][1],
               b.alloc[p][2], b.alloc[p][3], b.max[p][0], b.max[p][1], b.max[p][2], b.max[p][3],
               banker_need(&b, p, 0), banker_need(&b, p, 1), banker_need(&b, p, 2), banker_need(&b, p, 3),
               st->bk_proc[rows[p]].pending);
    }
    printf("]}");
}

static void print_json(const relief_status_t *st, int running, int64_t now)
{
    printf("{\"running\":%s,\"daemon_pid\":%d,\"start_time_ms\":%lld,\"uptime_ms\":%lld,"
           "\"total_requests\":%llu,\"now_ms\":%lld,\"camps\":[",
           running ? "true" : "false", st->daemon_pid, (long long)st->start_time_ms,
           (long long)(now - st->start_time_ms), (unsigned long long)st->total_requests,
           (long long)now);
    for (uint32_t i = 0; i < st->ncamps && i < RELIEF_SHM_CAMPS; i++) {
        const relief_shm_camp_t *c = &st->camps[i];
        printf("%s{\"id\":%u,\"name\":", i ? "," : "", c->id);
        print_json_string(c->name);
        printf(",\"pid\":%d,\"state\":\"%s\",\"requests_sent\":%llu,\"heartbeat_ms\":%lld,"
               "\"heartbeat_age_ms\":%lld,\"restarts\":%u}",
               c->pid, relief_camp_state_name(c->state), (unsigned long long)c->requests_sent,
               (long long)c->heartbeat_ms,
               c->heartbeat_ms ? (long long)(now - c->heartbeat_ms) : -1LL, c->restarts);
    }
    printf("],\"scheduler\":");
    print_sched_json(st, now);
    printf(",\"dock\":");
    print_dock_json(st);
    printf(",\"banker\":");
    print_banker_json(st);
    printf("}\n");
}

static void print_sched_table(const relief_status_t *st, int64_t now)
{
    printf("scheduler: policy %s, %u team%s; RESCUE requests enqueued %llu, completed %llu, "
           "preemptions %llu, promotions %llu\n",
           st->policy[0] ? st->policy : "-", st->nteams, st->nteams == 1 ? "" : "s",
           (unsigned long long)st->sched_enqueued, (unsigned long long)st->sched_completed,
           (unsigned long long)st->sched_preemptions, (unsigned long long)st->sched_promotions);
    printf("  ready queue:");
    for (uint32_t l = 0; l < st->nlevels && l < RELIEF_SHM_LEVELS; l++) {
        char buf[16];
        printf(" %s=%llu", level_name(st, l, buf, sizeof buf), (unsigned long long)st->queue_len[l]);
    }
    printf("\n  %4s  %-5s %9s %5s  %-9s %5s %5s %9s %5s\n", "TEAM", "STATE", "REQUEST", "CAMP",
           "SEVERITY", "LEVEL", "LEFT", "BUSY FOR", "DONE");
    for (uint32_t t = 0; t < st->nteams && t < RELIEF_SHM_TEAMS; t++) {
        const relief_shm_team_t *tm = &st->teams[t];
        if (!tm->state) {
            printf("  %4u  %-5s %9s %5s  %-9s %5s %5s %9s %5llu\n", t + 1, "idle", "-", "-", "-", "-",
                   "-", "-", (unsigned long long)tm->completed);
            continue;
        }
        printf("  %4u  %-5s %9u %5u  %-9s %5u %5lld %8.1fs %5llu\n", t + 1, "busy", tm->job_id,
               tm->camp, relief_severity_name(tm->severity), tm->level, (long long)tm->remaining,
               (double)(now - tm->busy_since_ms) / 1000.0, (unsigned long long)tm->completed);
    }
}

static void format_duration(char *buf, size_t len, int64_t ms)
{
    long long s = ms / 1000;
    snprintf(buf, len, "%02lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
}

static void print_table(const relief_status_t *st, int running, int64_t now)
{
    char up[32];

    format_duration(up, sizeof up, now - st->start_time_ms);
    printf("reliefd pid %d %s, up %s, requests received %llu\n", st->daemon_pid,
           running ? "running" : "NOT RUNNING (stale table)", up,
           (unsigned long long)st->total_requests);
    printf("%3s  %-14s %7s  %-8s %6s  %-10s %8s\n",
           "ID", "NAME", "PID", "STATE", "SENT", "HEARTBEAT", "RESTARTS");
    for (uint32_t i = 0; i < st->ncamps && i < RELIEF_SHM_CAMPS; i++) {
        const relief_shm_camp_t *c = &st->camps[i];
        char hb[24];
        int live = c->state == CAMP_STARTING || c->state == CAMP_RUNNING;

        if (c->heartbeat_ms == 0)
            snprintf(hb, sizeof hb, "-");
        else
            snprintf(hb, sizeof hb, "%.1fs ago%s", (double)(now - c->heartbeat_ms) / 1000.0,
                     live && now - c->heartbeat_ms > HEARTBEAT_STALE_MS ? "!" : "");
        printf("%3u  %-14s %7d  %-8s %6llu  %-10s %8u\n", c->id, c->name, c->pid,
               relief_camp_state_name(c->state), (unsigned long long)c->requests_sent, hb,
               c->restarts);
    }
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [--sched | --banker] [--json]\n"
                    "Print reliefd's live status table (POSIX shared memory %s).\n"
                    "  --sched  only the live scheduler: policy, ready queue, teams\n"
                    "  --banker only the Banker's state: Allocation, Max, Need, Available, safe sequence\n",
            prog, RELIEF_SHM_NAME);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "json", no_argument, NULL, 'j' },
        { "sched", no_argument, NULL, 's' },
        { "banker", no_argument, NULL, 'b' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    relief_status_t st;
    const relief_shm_t *shm;
    int json = 0, sched_only = 0, banker_only = 0, opt, running;
    int64_t now;

    while ((opt = getopt_long(argc, argv, "jsbh", opts, NULL)) != -1) {
        switch (opt) {
        case 'j': json = 1; break;
        case 's': sched_only = 1; break;
        case 'b': banker_only = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (optind < argc || (sched_only && banker_only)) {
        usage(argv[0]);
        return 2;
    }

    shm = relief_shm_open(0);   /* read-only */
    if (!shm) {
        if (json)
            printf("{\"running\":false}\n");
        else if (errno == ENOENT)
            fprintf(stderr, "reliefd not running (no shared memory %s)\n", RELIEF_SHM_NAME);
        else
            fprintf(stderr, "cannot read %s: %s\n", RELIEF_SHM_NAME, strerror(errno));
        return 1;
    }
    if (relief_shm_snapshot(shm, &st) == -1) {
        fprintf(stderr, "status table busy, try again\n");
        relief_shm_close((relief_shm_t *)shm);
        return 1;
    }
    relief_shm_close((relief_shm_t *)shm);

    now = relief_now_ms();
    running = pid_alive((pid_t)st.daemon_pid);
    if (banker_only && json) {
        print_banker_json(&st);
        printf("\n");
    } else if (banker_only) {
        print_banker_table(&st);
    } else if (sched_only && json) {
        print_sched_json(&st, now);
        printf("\n");
    } else if (sched_only) {
        print_sched_table(&st, now);
    } else if (json) {
        print_json(&st, running, now);
    } else {
        print_table(&st, running, now);
        printf("\n");
        print_sched_table(&st, now);
        printf("\n");
        print_dock_table(&st);
    }
    return running ? 0 : 1;
}
