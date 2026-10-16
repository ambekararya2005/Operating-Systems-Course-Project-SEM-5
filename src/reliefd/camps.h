/*
 * OS concept: Process creation and termination — fork/execv, SIGCHLD, waitpid, zombie reaping — Unit 2
 *
 * reliefd is the parent of every camp process. This module keeps the camp
 * table (a user-level analogue of the kernel's process table), spawns camps,
 * reaps them, restarts crashed ones and tears them all down on shutdown.
 */
#ifndef RELIEFD_CAMPS_H
#define RELIEFD_CAMPS_H

#include "relief.h"

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>
#include <time.h>

#define CAMPS_MAX          RELIEF_SHM_CAMPS
#define CAMP_NAME_MAX      RELIEF_CAMP_NAME_MAX
#define CAMP_MAX_RESTARTS  3
#define CAMPS_PATH_MAX     4096

typedef relief_camp_state_t camp_state_t;   /* shared with reliefstat via relief.h */

typedef struct {
    uint32_t        id;
    char            name[CAMP_NAME_MAX];
    pid_t           pid;            /* current/last pid, 0 if never started */
    camp_state_t    state;
    time_t          started;        /* wall-clock time of the latest (re)start */
    struct timespec started_mono;   /* for uptime */
    int             wait_status;    /* raw waitpid() status of the last exit, -1 if none */
    unsigned        restarts;
} camp_t;

typedef struct {
    camp_t        camps[CAMPS_MAX];
    size_t        n;
    char          camp_bin[CAMPS_PATH_MAX];
    char          scenario[CAMPS_PATH_MAX];
    unsigned long seed;
    unsigned long time_scale_ms;    /* real ms per simulated minute, passed to camps */
    int           shutting_down;    /* no restarts once set */
    int           dirty;            /* table changed since the last camps_save() */
    char          pids_path[CAMPS_PATH_MAX];   /* $RELIEF_HOME/camps.pids, "" = don't publish */
} camp_table_t;

const char *camp_state_name(camp_state_t s);

/* Parse "id name" lines ('#' comments and blank lines allowed). Returns 0 or -1. */
int camps_load(camp_table_t *t, const char *conf_path);

/* fork+execv every camp in the table. */
void camps_spawn_all(camp_table_t *t);

/* HELLO from (id, pid): STARTING -> RUNNING. */
void camps_on_hello(camp_table_t *t, uint32_t id, pid_t pid);

/* Reap exited children with waitpid(-1, WNOHANG) (or blocking until ECHILD if block != 0),
 * update the table and restart crashed camps. */
void camps_reap(camp_table_t *t, int block);

/* Publish the table to pids_path as "<id> <pid> <state> <name>" lines, atomically
 * (write a temp file, then rename over the old one). Clears dirty. Returns 0 or -1. */
int camps_save(camp_table_t *t);

/* Remove the published table (on shutdown, once camps are no longer controllable). */
void camps_unpublish(camp_table_t *t);

/* Copy reliefd-owned fields (id, name, pid, state, restarts) into the shared status table.
 * A row whose pid changed (new or restarted camp) gets its camp-owned counters reset. */
void camps_publish_shm(const camp_table_t *t, relief_shm_t *shm);

/* Number of camps with a live process (STARTING or RUNNING). */
size_t camps_live(const camp_table_t *t);

/* Begin shutdown: disable restarts and SIGTERM every live camp. */
void camps_terminate_all(camp_table_t *t);

/* SIGKILL every camp still alive (after the grace period). */
void camps_kill_stragglers(camp_table_t *t);

#endif /* RELIEFD_CAMPS_H */
