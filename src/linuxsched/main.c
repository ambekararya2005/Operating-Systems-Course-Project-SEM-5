/*
 * OS concept: Linux scheduling classes — CFS/EEVDF with nice, SCHED_FIFO, SCHED_RR — Unit 2
 *             (Traditional and Modern UNIX scheduling: real threads, real kernel decisions)
 *
 * linuxsched: replay a scenario as real CPU-bound threads and let the Linux scheduler
 * decide. Each RESCUE-style request becomes one thread, created at its (scaled) arrival
 * time, that burns burst x scale of CPU time (CLOCK_THREAD_CPUTIME_ID, so time spent
 * waiting for the CPU does not count as work). Per-thread start/finish/response go to CSV.
 *
 *   linuxsched --mode cfs-flat|cfs-nice|fifo|rr --scenario F [--scale-ms MS] [--out F.csv]
 *              [--max-cpu-s S] [--watchdog-s S] [--cpu N]
 *   linuxsched --check-rt        exit 0 if SCHED_FIFO/RR are allowed, 3 if not
 *
 * Modes (severity -> setting):
 *   cfs-flat  SCHED_OTHER, nice 0 for all (baseline)
 *   cfs-nice  SCHED_OTHER, nice -5 / 0 / +5 (CRITICAL / URGENT / ROUTINE). Without
 *             CAP_SYS_NICE, negative nice is refused, so the same spacing is used shifted
 *             up: 0 / +5 / +10. CFS/EEVDF weights depend only on nice *differences*
 *             (about x1.25 per step), so the CPU shares are identical.
 *   fifo      SCHED_FIFO, real-time priority 30 / 20 / 10
 *   rr        SCHED_RR,   real-time priority 30 / 20 / 10
 *
 * Exit status: 0 ok, 1 runtime error, 2 usage error (including a scenario over the CPU
 * budget), 3 real-time mode without CAP_SYS_NICE, 4 watchdog fired.
 */
#define _GNU_SOURCE     /* Linux-specific: sched_setaffinity, CPU_SET, gettid, sched_getcpu */

#include "../sched/sched.h"     /* src/sched is not on the include path (see sched.h) */
#include "../sched/sim.h"

#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#define EXIT_NO_PRIV   3
#define EXIT_WATCHDOG  4

#define MAIN_RT_PRIO   40      /* thread creator: above every worker, so arrivals are punctual */

typedef enum { MODE_CFS_FLAT, MODE_CFS_NICE, MODE_FIFO, MODE_RR } run_mode_t;

static const char *const g_mode_names[] = { "cfs-flat", "cfs-nice", "fifo", "rr" };
static const int g_rt_prio[3] = { 30, 20, 10 };    /* CRITICAL, URGENT, ROUTINE */
static const int g_nice[3] = { -5, 0, 5 };

typedef struct {
    const sched_job_t *job;
    int      nice;              /* cfs modes */
    int      rt_prio;           /* fifo/rr */
    int64_t  arrival_ns;        /* intended: scenario arrival x scale, relative to t0 */
    int64_t  created_ns;        /* when pthread_create was called */
    int64_t  start_ns;          /* first instruction the thread ran */
    int64_t  finish_ns;
    int64_t  cpu_target_ns;
    int64_t  cpu_used_ns;
    int      cpu;               /* CPU it finished on (should be the pinned one) */
    int      nice_err;          /* errno of setpriority, 0 if fine */
    pthread_t thread;
} worker_t;

static int64_t g_t0;
static int g_watchdog_s = 30;

static int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static int64_t thread_cpu_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void sleep_until_ns(int64_t abs_ns)
{
    struct timespec ts = { (time_t)(abs_ns / 1000000000LL), (long)(abs_ns % 1000000000LL) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR)
        ;
}

/* One rescue request: burn its CPU budget. */
static void *worker_main(void *arg)
{
    worker_t *w = arg;
    int64_t cpu0;
    volatile unsigned long spin = 0;

    w->start_ns = mono_ns() - g_t0;
    /* nice is per-thread on Linux: setpriority() on the thread id, not the process. */
    if (w->nice != 0 && setpriority(PRIO_PROCESS, (id_t)gettid(), w->nice) == -1)
        w->nice_err = errno;

    cpu0 = thread_cpu_ns();
    while (thread_cpu_ns() - cpu0 < w->cpu_target_ns)
        for (int i = 0; i < 2000; i++)
            spin++;
    w->cpu_used_ns = thread_cpu_ns() - cpu0;
    w->finish_ns = mono_ns() - g_t0;
    w->cpu = sched_getcpu();
    return NULL;
}

/* Safety net: whatever happens (a bug, a FIFO thread spinning forever on the only CPU),
 * the run ends after g_watchdog_s seconds. In real-time modes it runs at the maximum
 * SCHED_FIFO priority, so it always gets the CPU when its sleep ends. */
static void *watchdog_main(void *arg)
{
    struct timespec ts = { g_watchdog_s, 0 };
    static const char msg[] = "linuxsched: WATCHDOG fired, aborting the run\n";
    ssize_t r;

    (void)arg;
    clock_nanosleep(CLOCK_MONOTONIC, 0, &ts, NULL);     /* cancellation point */
    r = write(STDERR_FILENO, msg, sizeof msg - 1);
    (void)r;
    _exit(EXIT_WATCHDOG);
}

/* Can this process use SCHED_FIFO? Try it on the calling thread, then undo. */
static int rt_allowed(void)
{
    struct sched_param p = { .sched_priority = 1 }, zero = { .sched_priority = 0 };
    if (pthread_setschedparam(pthread_self(), SCHED_FIFO, &p) != 0)
        return 0;
    pthread_setschedparam(pthread_self(), SCHED_OTHER, &zero);
    return 1;
}

/* Can this process lower nice below 0? Try -1 on the calling thread, then undo. */
static int negative_nice_allowed(void)
{
    if (setpriority(PRIO_PROCESS, (id_t)gettid(), -1) == -1)
        return 0;
    setpriority(PRIO_PROCESS, (id_t)gettid(), 0);
    return 1;
}

static int job_cmp(const void *a, const void *b)
{
    const sched_job_t *x = a, *y = b;
    if (x->arrival != y->arrival)
        return x->arrival < y->arrival ? -1 : 1;
    return x->id < y->id ? -1 : (x->id > y->id);
}

static int write_csv(const char *path, const char *mode, const worker_t *w, size_t n, double scale_ms)
{
    static const char *const sev[3] = { "CRITICAL", "URGENT", "ROUTINE" };
    FILE *f = fopen(path, "w");

    if (!f) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    fprintf(f, "mode,id,camp,severity,arrival_min,burst_min,nice,rt_priority,arrival_ms,created_ms,"
               "start_ms,finish_ms,cpu_target_ms,cpu_used_ms,cpu,response_min,turnaround_min,wait_min\n");
    for (size_t i = 0; i < n; i++) {
        const worker_t *x = &w[i];
        double ms = 1e6, resp = (double)(x->start_ns - x->arrival_ns) / ms / scale_ms,
               turn = (double)(x->finish_ns - x->arrival_ns) / ms / scale_ms,
               wait = turn - (double)x->cpu_used_ns / ms / scale_ms;
        fprintf(f, "%s,%u,%u,%s,%lld,%lld,%d,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d,%.3f,%.3f,%.3f\n",
                mode, x->job->id, x->job->camp, sev[x->job->severity < 3 ? x->job->severity : 2],
                (long long)x->job->arrival, (long long)x->job->burst, x->nice, x->rt_prio,
                (double)x->arrival_ns / ms, (double)x->created_ns / ms, (double)x->start_ns / ms,
                (double)x->finish_ns / ms, (double)x->cpu_target_ns / ms, (double)x->cpu_used_ns / ms,
                x->cpu, resp, turn, wait);
    }
    if (fclose(f) != 0) {
        fprintf(stderr, "cannot write %s: %s\n", path, strerror(errno));
        return -1;
    }
    return 0;
}

static void usage(FILE *out, const char *prog)
{
    fprintf(out,
            "usage: %s --mode cfs-flat|cfs-nice|fifo|rr --scenario FILE [--scale-ms MS] [--out FILE]\n"
            "          [--max-cpu-s S] [--watchdog-s S] [--cpu N]\n"
            "       %s --check-rt\n"
            "  --scale-ms    real ms of CPU per simulated minute of service (default 4)\n"
            "  --out         per-thread CSV (default experiments/results/e2_<mode>.csv)\n"
            "  --max-cpu-s   refuse scenarios needing more CPU than this (default 10)\n"
            "  --watchdog-s  abort the run after this many seconds (default 30)\n"
            "  --cpu         CPU to pin to (default: the highest CPU we may use)\n"
            "fifo and rr need CAP_SYS_NICE: sudo setcap cap_sys_nice+ep %s\n",
            prog, prog, prog);
}

int main(int argc, char **argv)
{
    static const struct option opts[] = {
        { "mode",       required_argument, NULL, 'm' },
        { "scenario",   required_argument, NULL, 'f' },
        { "scale-ms",   required_argument, NULL, 'k' },
        { "out",        required_argument, NULL, 'o' },
        { "max-cpu-s",  required_argument, NULL, 'x' },
        { "watchdog-s", required_argument, NULL, 'w' },
        { "cpu",        required_argument, NULL, 'c' },
        { "check-rt",   no_argument,       NULL, 'r' },
        { "help",       no_argument,       NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    const char *scenario = NULL, *out = NULL;
    char out_default[256];
    int mode = -1, opt, cpu = -1, have_rt, neg_nice, nice_shift = 0, rc = EXIT_FAILURE;
    double scale_ms = 4.0, max_cpu_s = 10.0, total_cpu_s = 0;
    sched_job_t *jobs = NULL;
    worker_t *w = NULL;
    size_t n = 0, created = 0;
    pthread_t watchdog;
    pthread_attr_t attr;
    struct sched_param sp;
    cpu_set_t set;
    struct utsname un;
    int64_t lag_max = 0;

    while ((opt = getopt_long(argc, argv, "m:f:k:o:x:w:c:rh", opts, NULL)) != -1) {
        char *end;
        switch (opt) {
        case 'm':
            for (int i = 0; i < 4; i++)
                if (strcmp(optarg, g_mode_names[i]) == 0)
                    mode = i;
            if (mode < 0) {
                fprintf(stderr, "unknown --mode '%s' (cfs-flat, cfs-nice, fifo, rr)\n", optarg);
                return 2;
            }
            break;
        case 'f': scenario = optarg; break;
        case 'o': out = optarg; break;
        case 'k': case 'x': {
            double v = strtod(optarg, &end);
            if (end == optarg || *end || v <= 0 || v > 1000) {
                fprintf(stderr, "invalid --%s: %s\n", opt == 'k' ? "scale-ms" : "max-cpu-s", optarg);
                return 2;
            }
            if (opt == 'k')
                scale_ms = v;
            else
                max_cpu_s = v;
            break;
        }
        case 'w': case 'c': {
            long v = strtol(optarg, &end, 10);
            if (end == optarg || *end || v < (opt == 'w' ? 1 : 0) || v > 3600) {
                fprintf(stderr, "invalid --%s: %s\n", opt == 'w' ? "watchdog-s" : "cpu", optarg);
                return 2;
            }
            if (opt == 'w')
                g_watchdog_s = (int)v;
            else
                cpu = (int)v;
            break;
        }
        case 'r': return rt_allowed() ? 0 : EXIT_NO_PRIV;
        case 'h': usage(stdout, argv[0]); return 0;
        default:  usage(stderr, argv[0]); return 2;
        }
    }
    if (optind < argc || mode < 0 || !scenario) {
        usage(stderr, argv[0]);
        return 2;
    }

    /* Privilege checks before anything runs. */
    have_rt = rt_allowed();
    neg_nice = negative_nice_allowed();
    if ((mode == MODE_FIFO || mode == MODE_RR) && !have_rt) {
        fprintf(stderr, "linuxsched: --mode %s needs SCHED_%s, which requires CAP_SYS_NICE (or root).\n"
                        "  Grant it once (it stays until the next rebuild):\n"
                        "    sudo setcap cap_sys_nice+ep %s\n",
                g_mode_names[mode], mode == MODE_FIFO ? "FIFO" : "RR", argv[0]);
        return EXIT_NO_PRIV;
    }
    if (mode == MODE_CFS_NICE && !neg_nice) {
        nice_shift = 5;     /* -5/0/+5 -> 0/+5/+10: same differences, same CFS weights */
        printf("note: negative nice needs CAP_SYS_NICE; using nice 0/+5/+10 (same spacing, same "
               "CPU shares)\n");
    }

    if (sim_load_scenario(scenario, &jobs, &n) == -1)
        return 1;
    qsort(jobs, n, sizeof *jobs, job_cmp);
    for (size_t i = 0; i < n; i++)
        total_cpu_s += (double)jobs[i].burst * scale_ms / 1000.0;
    if (total_cpu_s > max_cpu_s) {
        fprintf(stderr, "linuxsched: scenario needs %.1f s of CPU at %.2f ms/minute, over the "
                        "%.1f s budget: lower --scale-ms or use a smaller scenario\n",
                total_cpu_s, scale_ms, max_cpu_s);
        free(jobs);
        return 2;
    }
    w = calloc(n, sizeof *w);
    if (!w) {
        free(jobs);
        return 1;
    }

    /* Pin to ONE CPU. With 16 CPUs the threads would simply run in parallel and the
     * scheduler would rarely have to choose; on one CPU every thread competes for the same
     * run queue, so the order and share each class gives is exactly what we measure (and it
     * matches schedsim --teams 1). sched_setaffinity(0, ...) sets the calling thread's mask,
     * and every thread created afterwards inherits it, so this pins the whole process. */
    if (sched_getaffinity(0, sizeof set, &set) == -1) {
        perror("sched_getaffinity");
        goto out;
    }
    if (cpu < 0)
        for (int c = CPU_SETSIZE - 1; c >= 0 && cpu < 0; c--)
            if (CPU_ISSET(c, &set))
                cpu = c;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (sched_setaffinity(0, sizeof set, &set) == -1) {
        fprintf(stderr, "cannot pin to CPU %d: %s\n", cpu, strerror(errno));
        goto out;
    }

    /* Watchdog: highest real-time priority if we have it, else a normal thread (CFS never
     * starves it, and without privilege no real-time thread can exist to hog the CPU). */
    pthread_attr_init(&attr);
    if (have_rt) {
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, SCHED_FIFO);
        sp.sched_priority = sched_get_priority_max(SCHED_FIFO);
        pthread_attr_setschedparam(&attr, &sp);
    }
    if (pthread_create(&watchdog, &attr, watchdog_main, NULL) != 0) {
        perror("watchdog");
        pthread_attr_destroy(&attr);
        goto out;
    }
    pthread_attr_destroy(&attr);

    /* With privilege, the creating thread runs at SCHED_FIFO 40, above every worker, so each
     * thread is created on time even when real-time workers own the CPU. */
    if (have_rt) {
        sp.sched_priority = MAIN_RT_PRIO;
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }

    uname(&un);
    printf("linuxsched: mode %s, kernel %s, pinned to CPU %d, %zu threads, %.1f s of CPU work, "
           "%.2f ms per simulated minute\n",
           g_mode_names[mode], un.release, cpu, n, total_cpu_s, scale_ms);
    fflush(stdout);

    g_t0 = mono_ns();
    for (size_t i = 0; i < n; i++) {
        worker_t *x = &w[i];
        int s = jobs[i].severity < 3 ? jobs[i].severity : 2, err;

        x->job = &jobs[i];
        x->arrival_ns = (int64_t)((double)jobs[i].arrival * scale_ms * 1e6);
        x->cpu_target_ns = (int64_t)((double)jobs[i].burst * scale_ms * 1e6);
        x->nice = mode == MODE_CFS_NICE ? g_nice[s] + nice_shift : 0;
        x->rt_prio = (mode == MODE_FIFO || mode == MODE_RR) ? g_rt_prio[s] : 0;
        sleep_until_ns(g_t0 + x->arrival_ns);

        /* Scheduling class set at creation (explicit, not inherited from this thread). */
        pthread_attr_init(&attr);
        pthread_attr_setinheritsched(&attr, PTHREAD_EXPLICIT_SCHED);
        pthread_attr_setschedpolicy(&attr, mode == MODE_FIFO ? SCHED_FIFO
                                           : mode == MODE_RR ? SCHED_RR : SCHED_OTHER);
        sp.sched_priority = x->rt_prio;
        pthread_attr_setschedparam(&attr, &sp);
        x->created_ns = mono_ns() - g_t0;
        if (x->created_ns - x->arrival_ns > lag_max)
            lag_max = x->created_ns - x->arrival_ns;
        err = pthread_create(&x->thread, &attr, worker_main, x);
        pthread_attr_destroy(&attr);
        if (err != 0) {
            fprintf(stderr, "pthread_create for request %u: %s\n", jobs[i].id, strerror(err));
            break;
        }
        created++;
    }
    for (size_t i = 0; i < created; i++)
        pthread_join(w[i].thread, NULL);
    pthread_cancel(watchdog);
    pthread_join(watchdog, NULL);
    if (created != n)
        goto out;

    /* Report. */
    {
        double resp[3] = { 0 }, wait[3] = { 0 }, maxr[3] = { 0 };
        size_t cnt[3] = { 0 }, nice_fail = 0, other_cpu = 0;
        int64_t end_ns = 0;
        static const char *const sev[3] = { "CRITICAL", "URGENT", "ROUTINE" };

        for (size_t i = 0; i < n; i++) {
            const worker_t *x = &w[i];
            int s = x->job->severity < 3 ? x->job->severity : 2;
            double r = (double)(x->start_ns - x->arrival_ns) / 1e6 / scale_ms;
            double t = (double)(x->finish_ns - x->arrival_ns) / 1e6 / scale_ms;
            cnt[s]++;
            resp[s] += r;
            wait[s] += t - (double)x->cpu_used_ns / 1e6 / scale_ms;
            if (r > maxr[s])
                maxr[s] = r;
            nice_fail += x->nice_err != 0;
            other_cpu += x->cpu != cpu;
            if (x->finish_ns > end_ns)
                end_ns = x->finish_ns;
        }
        printf("  finished in %.2f s wall; thread creation lag max %.2f ms; threads on another CPU: "
               "%zu; nice failures: %zu\n",
               (double)end_ns / 1e9, (double)lag_max / 1e6, other_cpu, nice_fail);
        printf("  %-9s %5s %14s %14s %14s   (simulated minutes)\n", "severity", "n", "avg response",
               "max response", "avg wait");
        for (int s = 0; s < 3; s++)
            if (cnt[s])
                printf("  %-9s %5zu %14.1f %14.1f %14.1f\n", sev[s], cnt[s], resp[s] / (double)cnt[s],
                       maxr[s], wait[s] / (double)cnt[s]);
    }

    if (!out) {
        snprintf(out_default, sizeof out_default, "experiments/results/e2_%s.csv", g_mode_names[mode]);
        out = out_default;
    }
    if (write_csv(out, g_mode_names[mode], w, n, scale_ms) == -1)
        goto out;
    printf("  wrote %s\n", out);
    rc = 0;
out:
    free(w);
    free(jobs);
    return rc;
}
