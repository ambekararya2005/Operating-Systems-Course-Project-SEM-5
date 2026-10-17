/*
 * OS concept: The readers-writers problem — a shared inventory read often, written rarely — Unit 3
 *
 * Stock of each kit type in each warehouse. Many threads read it (the status publisher on
 * reliefd's main thread, every truck writing its delivery note, later the Banker's safety
 * checks); few write it (a warehouse producer packing a kit, a restock). All access goes
 * through an rwsync_t (include/rwsync.h), so --rw semaphore|rwlock swaps the readers-writers
 * solution without touching the callers.
 */
#ifndef RELIEFD_INVENTORY_H
#define RELIEFD_INVENTORY_H

#include "relief.h"

#include <stdint.h>

#define INV_INITIAL_STOCK   1500    /* kits of each type per warehouse at start */
#define INV_RESTOCK_AMOUNT  2000    /* added when a pack would take a warehouse below zero */

typedef struct inventory inventory_t;

/* NULL on error (logged). rw_kind: "semaphore" or "rwlock". */
inventory_t *inventory_new(const char *rw_kind);
void inventory_free(inventory_t *inv);

/* Warehouse that serves a camp, and the kit index (0..RELIEF_KIT_KINDS-1) of a resource
 * (-1 if the resource is not a kit). */
int inventory_warehouse_of(uint32_t camp);
int inventory_kit_index(unsigned resource);

/* Writer: take qty kits of kit type k from warehouse w, restocking first if there are not
 * enough. Returns the stock left; *restocked is set to the amount added (0 if none). */
int64_t inventory_take(inventory_t *inv, int w, int k, uint32_t qty, int64_t *restocked);

/* Reader: copy the whole table. */
void inventory_snapshot(inventory_t *inv, int64_t out[RELIEF_WAREHOUSES][RELIEF_KIT_KINDS]);

/* Reader: the stock of one kit type in one warehouse. */
int64_t inventory_stock(inventory_t *inv, int w, int k);

/* Name of the readers-writers implementation in use. */
const char *inventory_rw_kind(const inventory_t *inv);

/* Reader: copy the table into the shared-memory status table (caller does not hold the
 * shm lock). */
void inventory_publish(inventory_t *inv, relief_shm_t *shm);

#endif /* RELIEFD_INVENTORY_H */
