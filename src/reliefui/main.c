/*
 * OS concept: A read-only observer of shared memory (IPC) with lock-free seqlock reads, and
 *             process states from the PCB via /proc/<pid>/stat — Units 2 and 3
 *
 * reliefui: a live ncurses dashboard for ReliefOS (like htop). Every 500 ms it copies
 * reliefd's status table (/reliefos_status) and draws:
 *   - top bar:   daemon pid, uptime, scheduling policy, deadlock mode, requests, requests/s
 *   - camps:     id, name, pid, process state (R/S/D/T/Z from /proc/<pid>/stat), requests sent,
 *                restarts, heartbeat age (green < 2 s, yellow < 5 s, red otherwise)
 *   - scheduler: ready-queue length per level as bars, what each rescue team is doing
 *   - resources: Banker's Available vs Total per resource as bars, safe/unsafe (the safety
 *                algorithm run on the copy), the loading dock's slots
 *   - events:    the last lines of $RELIEF_HOME/logs/reliefd.log, coloured by event
 * Keys: q quit, p pause, a alert a camp, d dump a camp, k kill -9 a camp (watch it restart),
 * s start/stop ReliefOS (via reliefctl), ? help.
 *
 *   reliefui [--batch N]     --batch: draw N frames to stdout without a keyboard (tests);
 *                            the size comes from $LINES / $COLUMNS (default 45 x 130)
 * Environment: RELIEF_HOME (default <repo>/run), RELIEFCTL (default <repo>/scripts/reliefctl).
 *
 * How the status table is read, and why no lock is taken:
 * reliefd's writers (the dispatcher, the dock, the resource manager, the camps) update the
 * table under its process-shared robust mutex and bump a sequence counter around each write.
 * The UI maps the segment read-only and copies it with relief_shm_snapshot(): copy the
 * whole struct, and retry if the counter was odd or changed meanwhile (a seqlock). Then
 * everything is drawn from that private copy. This is stricter than copying under the mutex:
 * the UI never takes the mutex at all, so it can never delay reliefd's critical paths. That
 * matters because drawing is slow (terminal I/O), and a UI stopped by Ctrl-Z in the middle of
 * a copy would otherwise hold the lock indefinitely. A robust mutex only recovers when its holder
 * dies, not when it is stopped. A read-only mapping also means the UI can never corrupt the
 * table. See the lock hierarchy in docs/ARCHITECTURE.md: the UI is outside it.
 */
#define _GNU_SOURCE     /* readlink of /proc/self/exe, kill */

#include "../deadlock/banker.h"     /* src/deadlock is not on the include path */
#include "relief.h"
#include "shm.h"

#include <curses.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define REFRESH_MS      500
#define LOG_LINES_MAX   15
#define LOG_TAIL_BYTES  16384

enum { C_OK = 1, C_WARN, C_BAD, C_INFO, C_ACCENT, C_DIM, C_BAR, C_TITLE };

typedef struct {
    relief_status_t st;
    int      have;              /* a snapshot was taken */
    int      running;           /* the daemon pid is alive */
    int64_t  now_ms;
    double   req_rate;          /* requests per second, smoothed */
    uint64_t last_total;
    int64_t  last_ms;
    int      paused, help;
    char     msg[160];
    char     home[PATH_MAX], ctl[PATH_MAX];
} ui_t;

static volatile sig_atomic_t g_quit;

static void on_term(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* ---- data --------------------------------------------------------------------------------- */

/* Take a fresh snapshot. Opening the segment each time follows a reliefd restart. */
static void refresh_snapshot(ui_t *u)
{
    relief_shm_t *shm = relief_shm_open(0);    /* read-only mapping */
    u->now_ms = relief_now_ms();
    if (!shm) {
        u->have = u->running = 0;
        return;
    }
    if (relief_shm_snapshot(shm, &u->st) == 0) {     /* seqlock copy, no lock taken */
        u->have = 1;
        u->running = u->st.daemon_pid > 0 && (kill((pid_t)u->st.daemon_pid, 0) == 0 || errno == EPERM);
        if (u->last_ms && u->now_ms > u->last_ms && u->st.total_requests >= u->last_total) {
            double inst = (double)(u->st.total_requests - u->last_total) * 1000.0 / (double)(u->now_ms - u->last_ms);
            u->req_rate = u->req_rate * 0.6 + inst * 0.4;
        }
        u->last_total = u->st.total_requests;
        u->last_ms = u->now_ms;
    }
    relief_shm_close(shm);
}

/* Process state letter from /proc/<pid>/stat (the third field, after the "(comm)"). */
static char proc_state(int32_t pid)
{
    char path[64], buf[512];
    if (pid <= 0)
        return '-';
    snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    int fd = open(path, O_RDONLY);
    if (fd == -1)
        return '-';
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0)
        return '-';
    buf[n] = '\0';
    char *rp = strrchr(buf, ')');
    return rp && rp[1] == ' ' && rp[2] ? rp[2] : '?';
}

/* The last `want` lines of the event log. Returns how many were stored. */
static int tail_log(const ui_t *u, char lines[][256], int want)
{
    char path[PATH_MAX + 32], buf[LOG_TAIL_BYTES + 1];
    snprintf(path, sizeof path, "%s/logs/reliefd.log", u->home);
    int fd = open(path, O_RDONLY);
    if (fd == -1)
        return 0;
    off_t end = lseek(fd, 0, SEEK_END);
    off_t from = end > LOG_TAIL_BYTES ? end - LOG_TAIL_BYTES : 0;
    ssize_t n = pread(fd, buf, LOG_TAIL_BYTES, from);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    char *starts[LOG_LINES_MAX + 1];
    int count = 0;
    for (char *p = buf + n - 1; p >= buf && count <= want; p--) {
        if (*p == '\n')
            *p = '\0';
        if ((p == buf || p[-1] == '\n') && *p)
            starts[count++] = p;
        if (count == want)
            break;
    }
    if (from > 0 && count && starts[count - 1] == buf)
        count--;                        /* the first line may be cut */
    for (int i = 0; i < count; i++)
        snprintf(lines[i], 256, "%s", starts[count - 1 - i]);
    return count;
}

/* ---- drawing helpers -------------------------------------------------------------------------- */

static void box_at(int y, int x, int h, int w, const char *title)
{
    if (h < 2 || w < 4)
        return;
    attron(COLOR_PAIR(C_DIM));
    mvhline(y, x + 1, ACS_HLINE, w - 2);
    mvhline(y + h - 1, x + 1, ACS_HLINE, w - 2);
    mvvline(y + 1, x, ACS_VLINE, h - 2);
    mvvline(y + 1, x + w - 1, ACS_VLINE, h - 2);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x + w - 1, ACS_URCORNER);
    mvaddch(y + h - 1, x, ACS_LLCORNER);
    mvaddch(y + h - 1, x + w - 1, ACS_LRCORNER);
    attroff(COLOR_PAIR(C_DIM));
    attron(A_BOLD | COLOR_PAIR(C_TITLE));
    mvprintw(y, x + 2, " %s ", title);
    attroff(A_BOLD | COLOR_PAIR(C_TITLE));
}

/* A bar of `width` cells, `filled` of `total` drawn as '#', the rest as '.'. */
static void bar(int y, int x, int width, long filled, long total, int pair)
{
    int n = total > 0 ? (int)((filled * width + total - 1) / total) : 0;
    if (n > width)
        n = width;
    mvaddch(y, x, '[');
    attron(COLOR_PAIR(pair));
    for (int i = 0; i < n; i++)
        mvaddch(y, x + 1 + i, '#');
    attroff(COLOR_PAIR(pair));
    attron(COLOR_PAIR(C_DIM));
    for (int i = n; i < width; i++)
        mvaddch(y, x + 1 + i, '.');
    attroff(COLOR_PAIR(C_DIM));
    mvaddch(y, x + 1 + width, ']');
}

static void fmt_uptime(char *buf, size_t len, int64_t ms)
{
    long long s = ms / 1000;
    snprintf(buf, len, "%02lld:%02lld:%02lld", s / 3600, (s / 60) % 60, s % 60);
}

/* ---- panels ------------------------------------------------------------------------------------- */

static void draw_top(const ui_t *u, int cols)
{
    char up[32];
    const relief_status_t *st = &u->st;
    fmt_uptime(up, sizeof up, u->now_ms - st->start_time_ms);
    attron(A_REVERSE | A_BOLD);
    mvhline(0, 0, ' ', cols);
    mvprintw(0, 1, "ReliefOS  reliefd %d %s  up %s  sched %s  deadlock %s  requests %llu  %.1f req/s%s",
             st->daemon_pid, u->running ? "running" : "STOPPED", up, st->policy[0] ? st->policy : "-",
             st->dl_mode[0] ? st->dl_mode : "-", (unsigned long long)st->total_requests, u->req_rate,
             u->paused ? "  [PAUSED]" : "");
    attroff(A_REVERSE | A_BOLD);
}

static int draw_camps(const ui_t *u, int y, int cols)
{
    const relief_status_t *st = &u->st;
    int n = (int)(st->ncamps < RELIEF_SHM_CAMPS ? st->ncamps : RELIEF_SHM_CAMPS);
    int h = n + 3;
    box_at(y, 0, h, cols, "Camps (processes)");
    attron(A_BOLD);
    mvprintw(y + 1, 2, "%3s  %-14s %7s %5s %-9s %6s %8s  %s", "ID", "NAME", "PID", "PROC", "STATUS", "SENT",
             "RESTARTS", "HEARTBEAT");
    attroff(A_BOLD);
    for (int i = 0; i < n; i++) {
        const relief_shm_camp_t *c = &st->camps[i];
        int live = c->state == CAMP_STARTING || c->state == CAMP_RUNNING;
        char ps = live ? proc_state(c->pid) : '-';
        int row = y + 2 + i;
        mvprintw(row, 2, "%3u  %-14.14s %7d ", c->id, c->name, c->pid);
        int sp = ps == 'R' ? C_OK : ps == 'Z' || ps == 'T' ? C_BAD : 0;
        if (sp)
            attron(COLOR_PAIR(sp) | A_BOLD);
        printw("%5c", ps);
        if (sp)
            attroff(COLOR_PAIR(sp) | A_BOLD);
        int cp = c->state == CAMP_RUNNING ? C_OK : c->state == CAMP_CRASHED ? C_BAD : C_WARN;
        attron(COLOR_PAIR(cp));
        printw(" %-9s", relief_camp_state_name(c->state));
        attroff(COLOR_PAIR(cp));
        printw(" %6llu %8u  ", (unsigned long long)c->requests_sent, c->restarts);
        if (!c->heartbeat_ms || !live) {
            printw("-");
        } else {
            int64_t age = u->now_ms - c->heartbeat_ms;
            int hp = age < 2000 ? C_OK : age < 5000 ? C_WARN : C_BAD;
            attron(COLOR_PAIR(hp) | A_BOLD);
            printw("%5.1fs ago", (double)age / 1000.0);
            attroff(COLOR_PAIR(hp) | A_BOLD);
        }
    }
    return h;
}

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

static void draw_sched(const ui_t *u, int y, int x, int h, int w)
{
    const relief_status_t *st = &u->st;
    char buf[16];
    box_at(y, x, h, w, "Scheduler (rescue teams = CPUs)");
    mvprintw(y + 1, x + 2, "policy %s: enqueued %llu, completed %llu, preemptions %llu, promotions %llu",
             st->policy[0] ? st->policy : "-", (unsigned long long)st->sched_enqueued,
             (unsigned long long)st->sched_completed, (unsigned long long)st->sched_preemptions,
             (unsigned long long)st->sched_promotions);
    uint64_t maxq = 10;
    for (uint32_t l = 0; l < st->nlevels && l < RELIEF_SHM_LEVELS; l++)
        if (st->queue_len[l] > maxq)
            maxq = st->queue_len[l];
    int row = y + 2, bw = w - 22 > 40 ? 40 : w - 22;
    for (uint32_t l = 0; l < st->nlevels && l < RELIEF_SHM_LEVELS && row < y + h - 1; l++, row++) {
        mvprintw(row, x + 2, "%-8s", level_name(st, l, buf, sizeof buf));
        bar(row, x + 11, bw, (long)st->queue_len[l], (long)maxq, l == 0 ? C_BAD : l == 1 ? C_WARN : C_INFO);
        printw(" %llu", (unsigned long long)st->queue_len[l]);
    }
    for (uint32_t t = 0; t < st->nteams && t < RELIEF_SHM_TEAMS && row < y + h - 1; t++, row++) {
        const relief_shm_team_t *tm = &st->teams[t];
        mvprintw(row, x + 2, "team %-2u ", t + 1);
        if (!tm->state) {
            attron(COLOR_PAIR(C_DIM));
            printw("idle  (%llu done)", (unsigned long long)tm->completed);
            attroff(COLOR_PAIR(C_DIM));
            continue;
        }
        int sp = tm->severity == SEV_CRITICAL ? C_BAD : tm->severity == SEV_URGENT ? C_WARN : C_INFO;
        printw("job %-8u ", tm->job_id);
        attron(COLOR_PAIR(sp) | A_BOLD);
        printw("%-8s", relief_severity_name(tm->severity));
        attroff(COLOR_PAIR(sp) | A_BOLD);
        printw(" camp %-2u %3lld min left  (%llu done)", tm->camp, (long long)tm->remaining,
               (unsigned long long)tm->completed);
    }
}

static void draw_resources(const ui_t *u, int y, int x, int h, int w)
{
    const relief_status_t *st = &u->st;
    box_at(y, x, h, w, "Resources (Banker's) and loading dock");
    int bw = w - 34 > 30 ? 30 : w - 34;
    if (bw < 5)
        bw = 5;
    int row = y + 1;
    for (int j = 0; j < RELIEF_BANKER_RES && row < y + h - 1; j++, row++) {
        int total = st->bk_total[j], avail = st->bk_available[j];
        mvprintw(row, x + 2, "%-9s", relief_banker_name((unsigned)j));
        bar(row, x + 12, bw, avail, total, avail == 0 ? C_BAD : avail * 3 < total ? C_WARN : C_OK);
        printw(" %3d/%-3d free", avail, total);
    }
    /* Safe or unsafe: the safety algorithm on the copy (active camps only). */
    banker_t b;
    int rows[RELIEF_SHM_CAMPS], seq[RELIEF_SHM_CAMPS], n = 0, len = 0;
    for (uint32_t i = 0; i < st->bk_nproc && i < RELIEF_SHM_CAMPS; i++)
        if (st->bk_proc[i].active)
            rows[n++] = (int)i;
    banker_init(&b, n, RELIEF_BANKER_RES, st->bk_total);
    for (int p = 0; p < n; p++) {
        banker_set_max(&b, p, st->bk_proc[rows[p]].max);
        banker_set_alloc(&b, p, st->bk_proc[rows[p]].alloc);
    }
    int safe = banker_is_safe(&b, seq, &len);
    if (row < y + h - 1) {
        mvprintw(row, x + 2, "state    ");
        attron(COLOR_PAIR(safe ? C_OK : C_BAD) | A_BOLD);
        printw("%s", safe ? "SAFE" : "UNSAFE");
        attroff(COLOR_PAIR(safe ? C_OK : C_BAD) | A_BOLD);
        if (safe && len) {
            printw("  <");
            for (int i = 0; i < len && getcurx(stdscr) < x + w - 8; i++)
                printw("%sP%u", i ? "," : "", st->bk_proc[rows[seq[i]]].camp);
            printw(">");
        }
        row++;
    }
    if (row < y + h - 1) {
        mvprintw(row, x + 2, "waiting  %u request%s, %llu unsafe denials, %llu deadlocks detected", st->bk_pending,
                 st->bk_pending == 1 ? "" : "s", (unsigned long long)st->bk_denied_unsafe,
                 (unsigned long long)st->dl_detected);
        row++;
    }
    if (row < y + h - 1) {
        int cap = (int)st->dock_capacity, occ = (int)st->dock_occupancy;
        mvprintw(row, x + 2, "dock     ");
        addch('[');
        for (int i = 0; i < cap && i < 64; i++) {
            int full = i < occ;
            attron(COLOR_PAIR(full ? (occ == cap ? C_BAD : C_ACCENT) : C_DIM));
            addch(full ? '#' : '.');
            attroff(COLOR_PAIR(full ? (occ == cap ? C_BAD : C_ACCENT) : C_DIM));
        }
        printw("] %d/%d  produced %llu, delivered %llu", occ, cap, (unsigned long long)st->dock_produced,
               (unsigned long long)st->dock_delivered);
    }
}

static int event_colour(const char *line, int *bold)
{
    static const struct { const char *ev; int pair, bold; } map[] = {
        { " event=deadlock_detected ", C_BAD, 1 }, { " event=recovery ", C_BAD, 0 },
        { " event=deny_unsafe ", C_ACCENT, 0 }, { " event=grant ", C_OK, 0 },
        { " event=preempt ", C_WARN, 0 }, { " event=dispatch ", C_INFO, 0 },
    };
    *bold = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++)
        if (strstr(line, map[i].ev)) {
            *bold = map[i].bold;
            return map[i].pair;
        }
    return strstr(line, " level=warn ") || strstr(line, " level=error ") ? C_WARN : 0;
}

static void draw_events(const ui_t *u, int y, int h, int cols)
{
    char lines[LOG_LINES_MAX][256];
    int want = h - 2 > LOG_LINES_MAX ? LOG_LINES_MAX : h - 2;
    box_at(y, 0, h, cols, "Events (reliefd.log)");
    if (want <= 0)
        return;
    int n = tail_log(u, lines, want);
    for (int i = 0; i < n; i++) {
        /* "ts=2026-10-17T13:11:20.956 level=info comp=reliefd pid=1 event=grant ..." -> "13:11:20.956 grant ..." */
        const char *l = lines[i], *ts = strstr(l, "T"), *ev = strstr(l, " event=");
        char shown[256];
        snprintf(shown, sizeof shown, "%.12s %.240s", ts ? ts + 1 : "", ev ? ev + 7 : l);
        int bold, pair = event_colour(l, &bold);
        if (pair)
            attron(COLOR_PAIR(pair) | (bold ? A_BOLD : 0));
        mvaddnstr(y + 1 + i, 2, shown, cols - 4);
        if (pair)
            attroff(COLOR_PAIR(pair) | (bold ? A_BOLD : 0));
    }
}

static void draw_help(int rows, int cols)
{
    static const char *const text[] = {
        "ReliefOS dashboard: keys",
        "",
        "  q   quit                 p   pause / resume",
        "  a   alert a camp (SIGUSR1: its next 5 requests CRITICAL)",
        "  d   dump a camp's counters (SIGUSR2)",
        "  k   kill -9 a camp: reliefd reaps it and restarts it",
        "  s   start / stop ReliefOS (reliefctl start | stop)",
        "  ?   this help (any key closes it)",
        "",
        "The table is copied lock-free (seqlock) every 500 ms;",
        "reliefd never waits for this UI.",
    };
    int n = (int)(sizeof text / sizeof text[0]), w = 62, h = n + 2;
    int y = (rows - h) / 2, x = (cols - w) / 2;
    if (y < 0 || x < 0)
        return;
    for (int i = 0; i < h; i++)
        mvhline(y + i, x, ' ', w);
    box_at(y, x, h, w, "Help");
    for (int i = 0; i < n; i++)
        mvprintw(y + 1 + i, x + 2, "%s", text[i]);
}

static void draw(ui_t *u)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    erase();
    if (!u->have || !u->running) {
        const char *m = "ReliefOS not running - press s to start";
        attron(A_BOLD);
        mvprintw(rows / 2, (cols - (int)strlen(m)) / 2 > 0 ? (cols - (int)strlen(m)) / 2 : 0, "%s", m);
        attroff(A_BOLD);
        mvprintw(rows / 2 + 2, cols / 2 - 12 > 0 ? cols / 2 - 12 : 0, "q quit   ? help   s start");
    } else {
        draw_top(u, cols);
        int y = 1;
        y += draw_camps(u, y, cols);
        int nlev = (int)(u->st.nlevels < RELIEF_SHM_LEVELS ? u->st.nlevels : RELIEF_SHM_LEVELS);
        int sh = 3 + nlev + (int)(u->st.nteams < RELIEF_SHM_TEAMS ? u->st.nteams : RELIEF_SHM_TEAMS);
        int rh = 2 + RELIEF_BANKER_RES + 3;
        if (cols >= 120) {                              /* side by side */
            int h = sh > rh ? sh : rh;
            draw_sched(u, y, 0, h, cols / 2);
            draw_resources(u, y, cols / 2, h, cols - cols / 2);
            y += h;
        } else {                                        /* stacked */
            draw_sched(u, y, 0, sh, cols);
            y += sh;
            draw_resources(u, y, 0, rh, cols);
            y += rh;
        }
        if (rows - 1 - y >= 3)
            draw_events(u, y, rows - 1 - y > LOG_LINES_MAX + 2 ? LOG_LINES_MAX + 2 : rows - 1 - y, cols);
    }
    attron(COLOR_PAIR(C_DIM));
    mvhline(rows - 1, 0, ' ', cols);
    mvaddnstr(rows - 1, 0, u->msg[0] ? u->msg : "q quit  p pause  a alert  d dump  k kill -9  s start/stop  ? help",
              cols - 1);
    attroff(COLOR_PAIR(C_DIM));
    if (u->help)
        draw_help(rows, cols);
    refresh();
}

/* ---- actions ------------------------------------------------------------------------------------ */

/* Run reliefctl with args, capture the first line of its output into u->msg. */
static void run_ctl(ui_t *u, const char *a1, const char *a2)
{
    int fd[2];
    if (pipe(fd) == -1) {
        snprintf(u->msg, sizeof u->msg, "pipe: %s", strerror(errno));
        return;
    }
    pid_t pid = fork();
    if (pid == -1) {
        snprintf(u->msg, sizeof u->msg, "fork: %s", strerror(errno));
        close(fd[0]);
        close(fd[1]);
        return;
    }
    if (pid == 0) {
        int devnull = open("/dev/null", O_RDONLY);
        if (devnull != -1)
            dup2(devnull, STDIN_FILENO);
        dup2(fd[1], STDOUT_FILENO);
        dup2(fd[1], STDERR_FILENO);
        close(fd[0]);
        close(fd[1]);
        char *argv[] = { u->ctl, (char *)a1, (char *)a2, NULL };
        execv(u->ctl, argv);
        _exit(127);
    }
    close(fd[1]);
    char out[512];
    size_t got = 0;
    ssize_t r;
    while (got < sizeof out - 1 && (r = read(fd[0], out + got, sizeof out - 1 - got)) > 0)
        got += (size_t)r;
    out[got] = '\0';
    char sink[256];
    while (read(fd[0], sink, sizeof sink) > 0)     /* drain the rest so the child can finish */
        ;
    close(fd[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) == -1 && errno == EINTR)
        ;
    char *nl = strchr(out, '\n');
    if (nl)
        *nl = '\0';
    snprintf(u->msg, sizeof u->msg, "reliefctl %.8s%s%.15s: %.100s%s", a1, a2 ? " " : "", a2 ? a2 : "",
             out[0] ? out : "(no output)",
             WIFEXITED(status) && WEXITSTATUS(status) == 0 ? "" : "  [failed]");
}

/* Prompt on the status line for a short answer. Returns 0 if one was entered. */
static int prompt(const char *question, char *buf, int len)
{
    int rows, cols;
    getmaxyx(stdscr, rows, cols);
    mvhline(rows - 1, 0, ' ', cols);
    attron(A_BOLD);
    mvprintw(rows - 1, 0, "%s", question);
    attroff(A_BOLD);
    echo();
    curs_set(1);
    timeout(-1);
    int rc = getnstr(buf, len - 1);
    noecho();
    curs_set(0);
    timeout(REFRESH_MS);
    return rc == ERR || !buf[0] ? -1 : 0;
}

static void action_kill(ui_t *u)
{
    char id[16];
    if (!u->running || prompt("kill -9 camp id: ", id, sizeof id) == -1) {
        snprintf(u->msg, sizeof u->msg, "%s", u->running ? "cancelled" : "ReliefOS is not running");
        return;
    }
    char *end;
    unsigned long want = strtoul(id, &end, 10);
    for (uint32_t i = 0; i < u->st.ncamps && i < RELIEF_SHM_CAMPS; i++) {
        const relief_shm_camp_t *c = &u->st.camps[i];
        if (*end || c->id != want)
            continue;
        if (c->pid <= 0 || (c->state != CAMP_RUNNING && c->state != CAMP_STARTING)) {
            snprintf(u->msg, sizeof u->msg, "camp %lu is not running", want);
            return;
        }
        if (kill((pid_t)c->pid, SIGKILL) == 0)
            snprintf(u->msg, sizeof u->msg, "camp %lu (pid %d) killed with SIGKILL: watch reliefd restart it", want,
                     c->pid);
        else
            snprintf(u->msg, sizeof u->msg, "kill %d: %s", c->pid, strerror(errno));
        return;
    }
    snprintf(u->msg, sizeof u->msg, "no camp %s", id);
}

static void key(ui_t *u, int ch)
{
    char id[16];
    if (u->help) {
        u->help = 0;
        return;
    }
    switch (ch) {
    case 'q': case 'Q': g_quit = 1; break;
    case 'p': case 'P':
        u->paused = !u->paused;
        snprintf(u->msg, sizeof u->msg, "%s", u->paused ? "paused (p resumes)" : "");
        break;
    case '?': u->help = 1; break;
    case 'a': case 'd':
        if (!u->running) {
            snprintf(u->msg, sizeof u->msg, "ReliefOS is not running");
        } else if (prompt(ch == 'a' ? "alert camp id (or all): " : "dump camp id (or all): ", id, sizeof id) == 0) {
            run_ctl(u, ch == 'a' ? "alert" : "dump", id);
        } else {
            snprintf(u->msg, sizeof u->msg, "cancelled");
        }
        break;
    case 'k': action_kill(u); break;
    case 's':
        snprintf(u->msg, sizeof u->msg, "%s ReliefOS...", u->running ? "stopping" : "starting");
        draw(u);
        run_ctl(u, u->running ? "stop" : "start", NULL);
        break;
    default: break;
    }
}

/* ---- setup ------------------------------------------------------------------------------------ */

/* <repo> = the directory above the one holding this binary (build/ or build/tsan/). */
static void locate(ui_t *u)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof exe - 1);
    exe[n > 0 ? n : 0] = '\0';
    char *slash = strrchr(exe, '/');
    if (slash)
        *slash = '\0';
    if (strlen(exe) >= 5 && strcmp(exe + strlen(exe) - 5, "/tsan") == 0)
        exe[strlen(exe) - 5] = '\0';
    slash = strrchr(exe, '/');
    if (slash)
        *slash = '\0';                  /* <repo> */
    const char *home = getenv("RELIEF_HOME"), *ctl = getenv("RELIEFCTL");
    if (home && *home)
        snprintf(u->home, sizeof u->home, "%s", home);
    else
        snprintf(u->home, sizeof u->home, "%.*s/run", (int)(sizeof u->home - 5), exe);
    if (ctl && *ctl)
        snprintf(u->ctl, sizeof u->ctl, "%s", ctl);
    else
        snprintf(u->ctl, sizeof u->ctl, "%.*s/scripts/reliefctl", (int)(sizeof u->ctl - 20), exe);
}

static void colours(void)
{
    if (!has_colors())
        return;
    start_color();
    use_default_colors();
    init_pair(C_OK, COLOR_GREEN, -1);
    init_pair(C_WARN, COLOR_YELLOW, -1);
    init_pair(C_BAD, COLOR_RED, -1);
    init_pair(C_INFO, COLOR_CYAN, -1);
    init_pair(C_ACCENT, COLOR_MAGENTA, -1);
    init_pair(C_DIM, COLOR_BLUE, -1);
    init_pair(C_BAR, COLOR_GREEN, -1);
    init_pair(C_TITLE, COLOR_CYAN, -1);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "batch", required_argument, NULL, 'b' }, { "help", no_argument, NULL, 'h' }, { NULL, 0, NULL, 0 }
    };
    static ui_t u;
    long batch = 0;
    int c;
    char *end;

    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'b':
            batch = strtol(optarg, &end, 10);
            if (*end || batch < 1) {
                fprintf(stderr, "reliefui: bad --batch '%s'\n", optarg);
                return 2;
            }
            break;
        case 'h':
            printf("usage: reliefui [--batch N]\n  live ReliefOS dashboard; keys: q p a d k s ?\n");
            return 0;
        default:
            fprintf(stderr, "usage: reliefui [--batch N]\n");
            return 2;
        }
    }
    if (optind < argc) {
        fprintf(stderr, "usage: reliefui [--batch N]\n");
        return 2;
    }
    locate(&u);

    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_term;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    SCREEN *scr = NULL;
    FILE *in = NULL;
    if (batch) {
        /* No keyboard: draw to stdout with a fixed size, for tests and valgrind/TSan runs. */
        if (!getenv("LINES"))
            setenv("LINES", "45", 1);
        if (!getenv("COLUMNS"))
            setenv("COLUMNS", "130", 1);
        const char *term = getenv("TERM");
        in = fopen("/dev/null", "r");
        scr = in ? newterm(term && *term && strcmp(term, "dumb") ? (char *)term : "xterm", stdout, in) : NULL;
        if (!scr) {
            fprintf(stderr, "reliefui: cannot set up the terminal\n");
            if (in)
                fclose(in);
            return 1;
        }
    } else {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            fprintf(stderr, "reliefui: needs a terminal (or use --batch N)\n");
            return 2;
        }
        initscr();
    }
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    colours();
    timeout(REFRESH_MS);

    for (long frame = 0; !g_quit; frame++) {
        if (!u.paused)
            refresh_snapshot(&u);
        draw(&u);
        if (batch) {
            if (frame + 1 >= batch)
                break;
            struct timespec ts = { 0, 100 * 1000000L };
            nanosleep(&ts, NULL);
            continue;
        }
        int ch = getch();               /* waits up to REFRESH_MS */
        if (ch == KEY_RESIZE || ch == ERR)
            continue;
        key(&u, ch);
    }
    endwin();
    if (scr)
        delscreen(scr);
    if (in)
        fclose(in);
    return 0;
}
