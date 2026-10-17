/*
 * OS concept: Deadlock detection — the wait-for graph and cycle detection with DFS — Unit 3
 *
 * Nodes are processes (camps). Process i waits for process k when i is waiting for units of
 * resource j that are not available and k holds some of j: edge i -> k, labelled with j.
 * A deadlock among single-instance resources is exactly a cycle in this graph; with several
 * instances per type a cycle is necessary but not sufficient, so reliefd confirms a cycle
 * with banker_detect() before it recovers. Pure functions: used by reliefd's detector
 * thread and by `reliefctl waitfor` (reliefstat) on a shared-memory snapshot.
 * src/deadlock is not on the include path: include it as "../deadlock/waitfor.h".
 */
#ifndef RELIEF_WAITFOR_H
#define RELIEF_WAITFOR_H

#include "banker.h"

typedef struct {
    int      n;
    unsigned label[BANKER_MAX_P][BANKER_MAX_P];     /* bit j: i waits for k's resource j; 0 = no edge */
} waitfor_t;

/* Build the graph from the Banker's state (Allocation, Available, active) and the Request
 * matrix (what each process is waiting for). */
void waitfor_build(waitfor_t *g, const banker_t *b, const int request[][BANKER_MAX_R]);

/* Number of edges. */
int waitfor_edges(const waitfor_t *g);

/* Depth-first search for a cycle (white/grey/black colouring: an edge to a grey node closes a
 * cycle). Returns 1 and writes the cycle's nodes in order to cycle[0..*len-1] (cycle[0] is
 * the node the cycle returns to), or 0 if the graph is acyclic. */
int waitfor_find_cycle(const waitfor_t *g, int cycle[], int *len);

#endif /* RELIEF_WAITFOR_H */
