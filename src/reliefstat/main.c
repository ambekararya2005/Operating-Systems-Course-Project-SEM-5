/*
 * OS concept: IPC via POSIX shared memory — a read-only observer process — Unit 2;
 *             lock-free consistent reads with a seqlock — Unit 3
 *
 * reliefstat: print reliefd's live status table.
 *   reliefstat          human-readable table
 *   reliefstat --json   one JSON object (for scripts and later UIs)
 *
 * The segment is mapped PROT_READ: reliefstat can never corrupt it, and it does not take
 * the writers' mutex (locking would write to the mutex). relief_shm_snapshot() copies
 * the table and retries if a writer was active meanwhile.
 *
 * Exit status: 0 = reliefd running, 1 = not running or stale segment, 2 = usage error.
 */
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
    printf("]}\n");
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
    fprintf(stderr, "usage: %s [--json]\n"
                    "Print reliefd's live status table (POSIX shared memory %s).\n",
            prog, RELIEF_SHM_NAME);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "json", no_argument, NULL, 'j' },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    relief_status_t st;
    const relief_shm_t *shm;
    int json = 0, opt, running;
    int64_t now;

    while ((opt = getopt_long(argc, argv, "jh", opts, NULL)) != -1) {
        switch (opt) {
        case 'j': json = 1; break;
        case 'h': usage(argv[0]); return 0;
        default:  usage(argv[0]); return 2;
        }
    }
    if (optind < argc) {
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
    if (json)
        print_json(&st, running, now);
    else
        print_table(&st, running, now);
    return running ? 0 : 1;
}
