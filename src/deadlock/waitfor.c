/*
 * OS concept: Deadlock detection — the wait-for graph and cycle detection with DFS — Unit 3
 */
#include "waitfor.h"

#include <string.h>

void waitfor_build(waitfor_t *g, const banker_t *b, const int request[][BANKER_MAX_R])
{
    memset(g, 0, sizeof *g);
    g->n = b->n;
    for (int i = 0; i < b->n; i++) {
        if (!b->active[i])
            continue;
        for (int j = 0; j < b->m; j++) {
            if (request[i][j] <= 0 || request[i][j] <= b->available[j])
                continue;               /* not blocked on this resource */
            for (int k = 0; k < b->n; k++)
                if (k != i && b->active[k] && b->alloc[k][j] > 0)
                    g->label[i][k] |= 1u << j;
        }
    }
}

int waitfor_edges(const waitfor_t *g)
{
    int e = 0;
    for (int i = 0; i < g->n; i++)
        for (int k = 0; k < g->n; k++)
            e += g->label[i][k] != 0;
    return e;
}

enum { WHITE = 0, GREY, BLACK };

/* DFS from u along the current path stack. Returns 1 when an edge to a grey node is found. */
static int dfs(const waitfor_t *g, int u, unsigned char *colour, int *stack, int *depth, int *close)
{
    colour[u] = GREY;
    stack[(*depth)++] = u;
    for (int v = 0; v < g->n; v++) {
        if (!g->label[u][v])
            continue;
        if (colour[v] == GREY) {        /* back edge: v is on the current path */
            *close = v;
            return 1;
        }
        if (colour[v] == WHITE && dfs(g, v, colour, stack, depth, close))
            return 1;
    }
    colour[u] = BLACK;
    (*depth)--;
    return 0;
}

int waitfor_find_cycle(const waitfor_t *g, int cycle[], int *len)
{
    unsigned char colour[BANKER_MAX_P] = { 0 };
    int stack[BANKER_MAX_P], depth, close;

    for (int s = 0; s < g->n; s++) {
        if (colour[s] != WHITE)
            continue;
        depth = 0;
        if (dfs(g, s, colour, stack, &depth, &close)) {
            int start = 0;
            while (stack[start] != close)
                start++;
            *len = depth - start;
            memcpy(cycle, stack + start, (size_t)*len * sizeof *cycle);
            return 1;
        }
    }
    *len = 0;
    return 0;
}
