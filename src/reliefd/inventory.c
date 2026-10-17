/*
 * OS concept: The readers-writers problem — a shared inventory read often, written rarely — Unit 3
 */
#include "inventory.h"

#include "log.h"
#include "rwsync.h"
#include "shm.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct inventory {
    rwsync_t rw;                                            /* guards stock */
    int64_t  stock[RELIEF_WAREHOUSES][RELIEF_KIT_KINDS];
};

inventory_t *inventory_new(const char *rw_kind)
{
    inventory_t *inv = calloc(1, sizeof *inv);
    if (!inv)
        return NULL;
    if (rwsync_init(&inv->rw, rw_kind) == -1) {
        LOG_ERROR("inventory: readers-writers lock '%s': %s (kinds: %s)", rw_kind,
                  errno == EINVAL ? "unknown kind" : strerror(errno), rwsync_kinds());
        free(inv);
        return NULL;
    }
    for (int w = 0; w < RELIEF_WAREHOUSES; w++)
        for (int k = 0; k < RELIEF_KIT_KINDS; k++)
            inv->stock[w][k] = INV_INITIAL_STOCK;
    return inv;
}

void inventory_free(inventory_t *inv)
{
    if (!inv)
        return;
    rwsync_destroy(&inv->rw);
    free(inv);
}

int inventory_warehouse_of(uint32_t camp)
{
    return (int)((camp + RELIEF_WAREHOUSES - 1) % RELIEF_WAREHOUSES);    /* camp 1 -> 0, 2 -> 1, ... */
}

int inventory_kit_index(unsigned resource)
{
    switch (resource) {
    case RES_FOOD_KIT:    return 0;
    case RES_MED_KIT:     return 1;
    case RES_HYGIENE_KIT: return 2;
    default:              return -1;
    }
}

int64_t inventory_take(inventory_t *inv, int w, int k, uint32_t qty, int64_t *restocked)
{
    int64_t left, added = 0;

    rwsync_write_lock(&inv->rw);
    while (inv->stock[w][k] < (int64_t)qty) {
        inv->stock[w][k] += INV_RESTOCK_AMOUNT;
        added += INV_RESTOCK_AMOUNT;
    }
    inv->stock[w][k] -= qty;
    left = inv->stock[w][k];
    rwsync_write_unlock(&inv->rw);
    *restocked = added;
    return left;
}

void inventory_snapshot(inventory_t *inv, int64_t out[RELIEF_WAREHOUSES][RELIEF_KIT_KINDS])
{
    rwsync_read_lock(&inv->rw);
    memcpy(out, inv->stock, sizeof inv->stock);
    rwsync_read_unlock(&inv->rw);
}

int64_t inventory_stock(inventory_t *inv, int w, int k)
{
    rwsync_read_lock(&inv->rw);
    int64_t v = inv->stock[w][k];
    rwsync_read_unlock(&inv->rw);
    return v;
}

const char *inventory_rw_kind(const inventory_t *inv)
{
    return inv->rw.ops->name;
}

void inventory_publish(inventory_t *inv, relief_shm_t *shm)
{
    int64_t snap[RELIEF_WAREHOUSES][RELIEF_KIT_KINDS];
    int rc;

    if (!shm)
        return;
    inventory_snapshot(inv, snap);          /* never hold the inventory lock and the shm lock together */
    rc = relief_shm_begin_write(shm);
    if (rc == -1)
        return;
    if (rc == 1)
        EV_WARN("status_lock_recovered", "by=inventory reason=EOWNERDEAD");
    memcpy(shm->data.inventory, snap, sizeof snap);
    snprintf(shm->data.inv_lock, sizeof shm->data.inv_lock, "%s", inventory_rw_kind(inv));
    relief_shm_end_write(shm);
}
