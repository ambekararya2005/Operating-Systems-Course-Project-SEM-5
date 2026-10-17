/*
 * OS concept: Deadlock avoidance — the Banker's algorithm (safety algorithm and
 *             resource-request algorithm) — Unit 3
 *
 * Pure data structure and algorithms, no threads and no IPC: reliefd's resource manager
 * (src/reliefd/resmgr.c) calls it under its own mutex, reliefstat runs the safety algorithm
 * on a shared-memory snapshot, and tests/test_banker.c checks the textbook example.
 *
 * n processes (camps) and m resource types. For every process i and resource j:
 *     Need[i][j] = Max[i][j] - Allocation[i][j]
 *     Available[j] = Total[j] - sum over i of Allocation[i][j]
 * Only active processes (between their HELLO and their exit) take part in the safety check.
 * src/deadlock is not on the include path: include it as "../deadlock/banker.h".
 */
#ifndef RELIEF_BANKER_H
#define RELIEF_BANKER_H

#define BANKER_MAX_P 64
#define BANKER_MAX_R 8

typedef struct {
    int n, m;                               /* processes, resource types */
    int total[BANKER_MAX_R];
    int available[BANKER_MAX_R];
    int max[BANKER_MAX_P][BANKER_MAX_R];
    int alloc[BANKER_MAX_P][BANKER_MAX_R];
    unsigned char active[BANKER_MAX_P];
} banker_t;

typedef enum {
    BANKER_GRANT = 0,       /* granted: the state after the allocation is safe */
    BANKER_OVER_CLAIM,      /* request > Need: the process broke its declared maximum (error) */
    BANKER_UNAVAILABLE,     /* request > Available: the process must wait */
    BANKER_UNSAFE,          /* granting would leave an unsafe state: rolled back, must wait */
    BANKER_INACTIVE         /* no such active process */
} banker_result_t;

const char *banker_result_name(banker_result_t r);

/* n processes (all inactive, nothing allocated), m resource types with these totals. */
void banker_init(banker_t *b, int n, int m, const int total[]);

/* Declare process p's maximum claim and make it active. p must hold nothing and every
 * max[j] must be within 0..total[j]. Returns 0, or -1 if invalid. */
int banker_set_max(banker_t *b, int p, const int max[]);

/* Test/setup helper: set p's allocation directly (alloc <= max, enough available).
 * Returns 0 or -1. */
int banker_set_alloc(banker_t *b, int p, const int alloc[]);

static inline int banker_need(const banker_t *b, int p, int j) { return b->max[p][j] - b->alloc[p][j]; }

/* Safety algorithm. Returns 1 if the state is safe and writes a safe sequence of the active
 * processes to seq[0..*len-1]; returns 0 if unsafe (seq then holds the processes that could
 * finish, *len of them). seq may be NULL. */
int banker_is_safe(const banker_t *b, int seq[], int *len);

/* Resource-request algorithm for process p: check request <= Need and request <= Available,
 * pretend to allocate, run the safety algorithm, keep the allocation if safe (BANKER_GRANT,
 * seq = the safe sequence found) or roll it back (BANKER_UNSAFE). */
banker_result_t banker_request(banker_t *b, int p, const int req[], int seq[], int *len);

/* Process p hands back rel[] (rel <= Allocation). Returns 0, or -1 if it holds less. */
int banker_release(banker_t *b, int p, const int rel[]);

/* Process p terminates: everything it holds is released and it becomes inactive. */
void banker_remove(banker_t *b, int p);

#endif /* RELIEF_BANKER_H */
