/*
 * OS concept: Memory allocation behind one interface — the allocator registry — Unit 4
 */
#include "mem.h"

#include <string.h>

const int mem_kit_units[MEM_KINDS] = { 4, 2, 1 };
const char *const mem_kind_names[MEM_KINDS] = { "FOOD", "MED", "HYGIENE" };

extern const mem_ops_t mem_first_fit, mem_best_fit, mem_worst_fit, mem_buddy, mem_slab;

static const mem_ops_t *const g_allocators[] = { &mem_first_fit, &mem_best_fit, &mem_worst_fit, &mem_buddy, &mem_slab };

const mem_ops_t *mem_find(const char *name)
{
    for (size_t i = 0; i < sizeof g_allocators / sizeof g_allocators[0]; i++)
        if (strcmp(name, g_allocators[i]->name) == 0)
            return g_allocators[i];
    return NULL;
}

const char *mem_names(void)
{
    return "first, best, worst, buddy, slab";
}

char mem_id_letter(int id)
{
    static const char letters[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    return letters[(id < 0 ? -id : id) % 52];
}

void mem_free_runs(const char *units, int size, mem_stats_t *st)
{
    st->free_total = st->largest_free = st->holes = 0;
    for (int a = 0; a < size;) {
        if (units[a] != '.') {
            a++;
            continue;
        }
        int b = a;
        while (b < size && units[b] == '.')
            b++;
        st->free_total += b - a;
        st->holes++;
        if (b - a > st->largest_free)
            st->largest_free = b - a;
        a = b;
    }
    st->ext_frag = st->free_total ? 1.0 - (double)st->largest_free / st->free_total : 0.0;
}
