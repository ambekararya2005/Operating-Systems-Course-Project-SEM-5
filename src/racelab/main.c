/*
 * OS concept: The critical-section problem — race conditions, Peterson's solution, mutex locks,
 *             a test-and-set spinlock and semaphores; process synchronisation in Linux — Unit 3
 *
 * racelab: two relief camps share one boat and both update the shared inventory counter
 * ("load boat, update count"). Each of 2 threads does N increments of one shared counter,
 * as a separate read, modify and write, inside an entry/exit section chosen by --mode:
 *
 *   none              no protection: increments are lost (the race condition)
 *   peterson          flag[2] + turn, with the seq_cst fence that x86 needs (see below)
 *   peterson-nofence  the textbook Peterson with plain variables: fails on x86
 *   mutex             pthread_mutex_t (a futex underneath: a syscall only under contention)
 *   spinlock          our own test-and-set lock on atomic_flag
 *   semaphore         a POSIX unnamed semaphore (sem_init, value 1) used as a binary semaphore
 *
 *   racelab --mode M [--iters N] [--threads 1|2] [--csv F] [--run K]
 *
 * Prints the final count, its error against the expected threads x N, and the runtime.
 * --csv appends one row (header if the file is new); --run K is copied into the row.
 * --threads 1 runs a single thread (no contention at all; used for the strace futex demo).
 *
 * Why peterson-nofence fails on x86. Peterson's proof assumes sequential consistency: every
 * thread sees every memory operation in one global order that respects program order. x86
 * (TSO) gives that for every pair except store -> load: each core puts its stores in a
 * private store buffer and lets later loads go ahead of them. So in
 *     flag[i] = 1; turn = j;          (stores: still sitting in core i's store buffer)
 *     while (flag[j] && turn == j);   (loads: may read memory before those stores are visible)
 * both threads can read the *old* flag of the other (0), both skip the wait and both enter the
 * critical section; an update is lost. The variables are volatile, so the compiler keeps
 * every access and their order: the reordering is purely the hardware's. Textbooks assume
 * sequential consistency because it makes the algorithm provable on paper; real CPUs drop it
 * for speed. A full fence (atomic_thread_fence(memory_order_seq_cst), an MFENCE or locked
 * instruction on x86) after the two stores drains the store buffer before the loads, which
 * restores the order Peterson needs. Acquire/release alone is not enough: on x86 they are
 * plain MOVs, which is exactly the version that fails.
 *
 * none and peterson-nofence contain deliberate data races (that is the experiment), so they
 * are excluded from the ThreadSanitizer run; the other four modes must be TSan-clean.
 *
 * Exit status: 0 ran (whatever the count), 1 runtime error, 2 usage error.
 */
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DEFAULT_ITERS 1000000L

#if defined(__x86_64__) || defined(__i386__)
#define cpu_relax() __builtin_ia32_pause()     /* tells the core it is spinning */
#else
#define cpu_relax() ((void)0)
#endif

/* The shared inventory counter. volatile: one real load and one real store per increment,
 * so the compiler cannot fold the loop into a single add (and hide the race). */
static volatile long g_counter;

/* ---- lock interface: entry and exit section for thread `self` (0 or 1) ---------------- */
typedef struct {
    const char *name;
    int  (*init)(void);
    void (*lock)(int self);
    void (*unlock)(int self);
    void (*destroy)(void);
} lock_ops_t;

static int  noop_init(void) { return 0; }
static void noop_destroy(void) {}

/* none */
static void none_lock(int self) { (void)self; }
static void none_unlock(int self) { (void)self; }

/* peterson: correct on real hardware. The fence orders the two stores before the two loads;
 * the release/acquire pairs give the happens-before edge that ThreadSanitizer (which does
 * not model fences) needs to see the counter as protected. */
static atomic_int p_flag[2];
static atomic_int p_turn;

static void peterson_lock(int self)
{
    int other = 1 - self;
    atomic_store_explicit(&p_flag[self], 1, memory_order_relaxed);
    atomic_store_explicit(&p_turn, other, memory_order_release);
    atomic_thread_fence(memory_order_seq_cst);          /* store -> load barrier (MFENCE) */
    while (atomic_load_explicit(&p_flag[other], memory_order_acquire)
           && atomic_load_explicit(&p_turn, memory_order_acquire) == other)
        cpu_relax();
}

static void peterson_unlock(int self)
{
    atomic_store_explicit(&p_flag[self], 0, memory_order_release);
}

/* peterson-nofence: the textbook version, plain (volatile) variables, no fence. */
static volatile int n_flag[2];
static volatile int n_turn;

static void nofence_lock(int self)
{
    int other = 1 - self;
    n_flag[self] = 1;
    n_turn = other;
    while (n_flag[other] && n_turn == other)    /* may read n_flag[other] before our stores land */
        cpu_relax();
}

static void nofence_unlock(int self)
{
    n_flag[self] = 0;
}

/* mutex */
static pthread_mutex_t g_mutex = PTHREAD_MUTEX_INITIALIZER;

static void mutex_lock(int self)
{
    (void)self;
    pthread_mutex_lock(&g_mutex);
}

static void mutex_unlock(int self)
{
    (void)self;
    pthread_mutex_unlock(&g_mutex);
}

/* spinlock: test-and-set. atomic_flag_test_and_set is the TSL instruction (XCHG on x86):
 * it sets the flag and returns the old value in one indivisible step. */
static atomic_flag g_spin = ATOMIC_FLAG_INIT;

static void spin_lock(int self)
{
    (void)self;
    while (atomic_flag_test_and_set_explicit(&g_spin, memory_order_acquire))
        cpu_relax();
}

static void spin_unlock(int self)
{
    (void)self;
    atomic_flag_clear_explicit(&g_spin, memory_order_release);
}

/* semaphore: wait() = P, post() = V; initial value 1 makes it a binary semaphore. */
static sem_t g_sem;

static int sem_mode_init(void) { return sem_init(&g_sem, 0, 1); }
static void sem_mode_destroy(void) { sem_destroy(&g_sem); }

static void sem_lock(int self)
{
    (void)self;
    while (sem_wait(&g_sem) == -1 && errno == EINTR)
        ;
}

static void sem_unlock(int self)
{
    (void)self;
    sem_post(&g_sem);
}

static const lock_ops_t g_modes[] = {
    { "none",             noop_init,     none_lock,     none_unlock,     noop_destroy },
    { "peterson",         noop_init,     peterson_lock, peterson_unlock, noop_destroy },
    { "peterson-nofence", noop_init,     nofence_lock,  nofence_unlock,  noop_destroy },
    { "mutex",            noop_init,     mutex_lock,    mutex_unlock,    noop_destroy },
    { "spinlock",         noop_init,     spin_lock,     spin_unlock,     noop_destroy },
    { "semaphore",        sem_mode_init, sem_lock,      sem_unlock,      sem_mode_destroy },
};
#define NMODES (sizeof g_modes / sizeof g_modes[0])

/* ---- the two camps ------------------------------------------------------------------- */
typedef struct {
    int               self;
    long              iters;
    const lock_ops_t *ops;
    pthread_barrier_t *start;
} camp_arg_t;

static void *camp_thread(void *p)
{
    camp_arg_t *a = p;
    pthread_barrier_wait(a->start);         /* both camps start together: maximum overlap */
    for (long k = 0; k < a->iters; k++) {
        a->ops->lock(a->self);              /* entry section */
        long seen = g_counter;              /* read:   count the boat load */
        seen = seen + 1;                    /* modify */
        g_counter = seen;                   /* write:  update the inventory */
        a->ops->unlock(a->self);            /* exit section */
    }
    return NULL;
}

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

static void usage(FILE *f)
{
    fprintf(f, "usage: racelab --mode none|peterson|peterson-nofence|mutex|spinlock|semaphore\n"
               "               [--iters N] [--threads 1|2] [--csv FILE] [--run K]\n");
}

int main(int argc, char **argv)
{
    const lock_ops_t *ops = NULL;
    long iters = DEFAULT_ITERS;
    int nthreads = 2, run = 0;
    const char *csv = NULL;
    static const struct option opts[] = {
        { "mode", required_argument, NULL, 'm' }, { "iters", required_argument, NULL, 'n' },
        { "threads", required_argument, NULL, 't' }, { "csv", required_argument, NULL, 'c' },
        { "run", required_argument, NULL, 'r' }, { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };
    int c;
    char *end;

    while ((c = getopt_long(argc, argv, "", opts, NULL)) != -1) {
        switch (c) {
        case 'm':
            for (size_t i = 0; i < NMODES; i++)
                if (strcmp(optarg, g_modes[i].name) == 0) ops = &g_modes[i];
            if (!ops) { fprintf(stderr, "racelab: unknown mode '%s'\n", optarg); usage(stderr); return 2; }
            break;
        case 'n':
            errno = 0;
            iters = strtol(optarg, &end, 10);
            if (errno || *end || iters < 1) { fprintf(stderr, "racelab: bad --iters '%s'\n", optarg); return 2; }
            break;
        case 't':
            if (strcmp(optarg, "1") && strcmp(optarg, "2")) {
                fprintf(stderr, "racelab: --threads must be 1 or 2\n");
                return 2;
            }
            nthreads = optarg[0] - '0';
            break;
        case 'c': csv = optarg; break;
        case 'r': run = atoi(optarg); break;
        case 'h': usage(stdout); return 0;
        default:  usage(stderr); return 2;
        }
    }
    if (!ops || optind < argc) { usage(stderr); return 2; }

    if (ops->init() != 0) { perror("racelab: init"); return 1; }
    pthread_barrier_t start;
    pthread_barrier_init(&start, NULL, (unsigned)nthreads + 1);
    pthread_t tid[2];
    camp_arg_t arg[2];
    for (int i = 0; i < nthreads; i++) {
        arg[i] = (camp_arg_t){ i, iters, ops, &start };
        int err = pthread_create(&tid[i], NULL, camp_thread, &arg[i]);
        if (err) { fprintf(stderr, "racelab: pthread_create: %s\n", strerror(err)); return 1; }
    }
    pthread_barrier_wait(&start);
    double t0 = now_ms();
    for (int i = 0; i < nthreads; i++)
        pthread_join(tid[i], NULL);
    double ms = now_ms() - t0;
    pthread_barrier_destroy(&start);
    ops->destroy();

    long expected = iters * nthreads, final = g_counter, error = expected - final;
    double ns_op = ms * 1e6 / (double)expected;
    printf("mode=%s threads=%d iters=%ld expected=%ld final=%ld error=%ld lost_pct=%.3f "
           "runtime_ms=%.2f ns_per_increment=%.1f\n", ops->name, nthreads, iters, expected, final,
           error, 100.0 * error / expected, ms, ns_op);

    if (csv) {
        FILE *f = fopen(csv, "a");
        if (!f) { fprintf(stderr, "racelab: %s: %s\n", csv, strerror(errno)); return 1; }
        if (ftell(f) == 0)
            fprintf(f, "mode,run,threads,iters,expected,final,error,lost_pct,runtime_ms,ns_per_increment\n");
        fprintf(f, "%s,%d,%d,%ld,%ld,%ld,%ld,%.4f,%.3f,%.2f\n", ops->name, run, nthreads, iters,
                expected, final, error, 100.0 * error / expected, ms, ns_op);
        if (fclose(f) != 0) { perror("racelab: csv"); return 1; }
    }
    return 0;
}
