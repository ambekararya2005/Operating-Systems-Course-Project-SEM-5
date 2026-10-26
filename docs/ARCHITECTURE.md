# ReliefOS Architecture

> Updated as phases land. Phase 1 (Foundation) is complete; later phases add the scheduler,
> synchronisation, memory management, journaling and the kernel module inside or around reliefd.

## Components
- **reliefd**: coordinator daemon (triage scheduler, deadlock control, sync layer,
  warehouse allocators, delivery router, write-ahead journal)
- **camp**: one process per relief camp; client of the request queue
- **reliefctl**: Bash control CLI
- **reliefstat**: read-only viewer of the live status table
- **reliefui**: live ncurses dashboard over the same table (`src/reliefui/main.c`)
- **web/server.py**: local web dashboard (127.0.0.1 only). It reads the table through `reliefstat --json`,
  tails `reliefd.log` as Server-Sent Events, and runs allow-listed `reliefctl` commands
- **schedsim**: offline, deterministic scheduling simulator (Phase 2)
- **linuxsched**: real threads under the Linux scheduler (CFS/EEVDF + nice, SCHED_FIFO, SCHED_RR) for E2
- **relief.ko**: kernel module exposing `/proc/relief` (Phase 6)

## Phase 1: process model and IPC

### Process model
reliefd is a long-running daemon and the **parent of every camp**. At startup it reads
`scenarios/camps.conf` and `fork()` + `execv()`s one `camp` process per line, so each relief camp is a
real Linux process with its own pid, address space and PCB (`reliefctl ps` reads that PCB from
`/proc/<pid>/{status,stat}`). reliefd keeps a camp table (its own small "process table"): it reaps
children on SIGCHLD with `waitpid(-1, WNOHANG)`, tells normal exits from signal deaths, restarts a crashed
camp up to 3 times, and on shutdown escalates SIGTERM → (2 s grace) → SIGKILL and reaps until `ECHILD`,
so no zombies or orphans survive. Camps also set `PR_SET_PDEATHSIG`, so they die if reliefd is SIGKILLed.

```
  reliefctl (start/stop/ps/alert/dump)   reliefstat           report / stockcheck.sh
        │ kill(2) via reliefd.pid,        │ mmap PROT_READ       │ awk
        │ camps.pids                      ▼                      ▼
        │                     ┌────────────────────────┐   logs/reliefd.log
        │                     │ shm /reliefos_status   │         ▲
        │                     │ daemon + camp state    │         │ key=value events
        │                     └────▲──────────────▲────┘         │
        │       pid, state, restarts│              │ sent, heartbeat (own row)
        ▼                           │              │
  ┌───────────────────────────────────────┐        │
  │ reliefd   camp table · SIGCHLD reaper  │────────┼────────────┘
  │           mq receive loop · shm owner  │        │
  └──┬──────────────────────────────▲──────┘        │
     │ fork+execv, SIGTERM/SIGKILL  │ mq /reliefos_requests
     │ (SIGCHLD back on exit)       │ HELLO · REQUEST · BYE
     ▼                              │                │
  ┌──────────────────────────────────┴────────────────┴───┐
  │  camp 1 · camp 2 · … · camp 8   (one process each)    │◀── SIGUSR1 alert / SIGUSR2 dump
  └───────────────────────────────────────────────────────┘     (reliefctl, via camps.pids)
```

### IPC choices and why each fits
| Mechanism | Used for | Why it fits |
|-----------|----------|-------------|
| **POSIX message queue** `/reliefos_requests` | Camps → reliefd: HELLO, REQUEST, BYE, SHUTDOWN | Requests are discrete, fixed-size records (`relief_msg_t`, 128 bytes) from many producers to one consumer. The kernel keeps message boundaries, serialises the concurrent senders and provides a **bounded buffer** (10 slots): a full queue makes `mq_timedsend` wait, which is natural back-pressure. Priorities are available for the Phase 2 triage scheduler. |
| **POSIX shared memory** `/reliefos_status` | Live status table: camp pid, state, requests sent, heartbeat | State that many processes read often and update in place. Copying it through a queue would be wasteful, while shared memory lets any process read it without a system call per read. It needs its own synchronisation: a process-shared, robust pthread mutex for writers and a seqlock so `reliefstat` can read from a read-only mapping. |
| **Signals** | Control: SIGTERM stop, SIGUSR1 alert, SIGUSR2 dump, SIGCHLD child exit, SIGALRM heartbeat | Asynchronous notifications that carry no data except "something happened". They interrupt a sleeping process immediately (`clock_nanosleep`, `sigsuspend` and `mq_timedreceive` all wake up). Handlers only set `sig_atomic_t` flags; the work happens in the main loop. |
| **Files in `$RELIEF_HOME`** | `reliefd.pid`, `camps.pids` (atomic rename), `logs/reliefd.log` | Discovery and history for shell tools: plain text that `kill`, `awk` and `grep` can use, so reliefctl needs no C client. |

A simple pipe would not fit the request stream: it is a byte stream with no message boundaries, and
anonymous pipes only connect related processes. A socket would work but adds connection management
that a single-host, fixed-format protocol does not need.

### Verified by
`tests/phase1_smoke.sh` (gate M1 "camps reach reliefd") runs `reliefctl simulate flood --camps 8 --seed 42`.
It checks 8 HELLO, 8 BYE and 200 requests, no zombies during or after the run, and that the queue,
the shared memory segment and the pid files are removed. It also checks the report. The other
`tests/*.sh` cover each mechanism in depth, and every test runs under ThreadSanitizer and Valgrind.

## Message flow
```
camp ──HELLO / REQUEST* / BYE──▶  /reliefos_requests (POSIX mq)  ──▶ reliefd
admin ──SHUTDOWN (camp --shutdown) or SIGTERM (reliefctl stop)─────▶ reliefd
```

## Camp traffic (Phase 1, `src/camp/main.c`)
- A camp loads the scenario rows whose `camp_id` matches its `--id` and sorts them by arrival. It then sends
  each one at `camp start + arrival_min × time_scale_ms`, sleeping with
  `clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME)`. The absolute deadlines mean no drift, and SIGTERM
  interrupts the sleep.
- The queue has 10 slots (the non-root `msg_max`). Every send is an `mq_timedsend` with a 500 ms deadline.
  On a timeout the camp logs `queue full, retrying` and tries again until stopped. BYE and SHUTDOWN give up
  after 3 attempts, because reliefd may already be gone.
- Each camp's simulated clock starts when that camp starts, so a restarted camp replays its rows from
  minute 0. The `arrival` field always carries the scenario's simulated minute.

## Signals (Phase 1, `src/camp/main.c`, `scripts/reliefctl`)
| Signal  | Sender | Camp behaviour |
|---------|--------|----------------|
| SIGUSR1 | `reliefctl alert <id\|all>` | Evacuation alert. The next 5 requests are sent as CRITICAL with `flags=REQ_FLAG_ALERT` and the original severity in `orig_severity`. reliefd logs `alert=1 orig_severity=...` |
| SIGUSR2 | `reliefctl dump <id\|all>` | Logs `event=status` with the sent count, counts by severity, escalations, alerts, queue-full retries, rows left and uptime |
| SIGTERM / SIGINT | reliefd shutdown, `reliefctl stop` | Finishes the send in progress (up to 3 more attempts), sends BYE and exits 0 |

- Handlers only set `volatile sig_atomic_t` flags. `service_signals()` does the work in the main flow,
  before each request, when a sleep is interrupted, and on every send retry. printf, malloc and the
  logger (which takes a mutex) are not async-signal-safe, so they must never run inside a handler.
- The camp uses SA_RESTART, so an `mq_timedsend` waiting for queue space resumes after a signal
  (this is how the current send gets finished). `clock_nanosleep` and `sigsuspend` are never
  restarted, so an idle camp still wakes immediately. The resident wait blocks all four signals
  between checking the flags and `sigsuspend()`, which closes the lost-wakeup race.
- reliefd uses no SA_RESTART on its own handlers: it wants EINTR so it can collect exited camps promptly.
- An alert applies to requests sent after the camp's main flow sees it. A request already inside
  `mq_timedsend` when SIGUSR1 arrives goes out unescalated.
- reliefctl gets pids from `$RELIEF_HOME/camps.pids`, which reliefd rewrites atomically
  (write a temp file, then `rename(2)`). It only signals pids whose parent is reliefd.

## Live status table (Phase 1, `include/shm.h`, `src/common/shm.c`, `src/reliefstat/`)
reliefd creates the POSIX shared memory object `/reliefos_status` (`shm_open` + `ftruncate` + `mmap`, mode 0644)
before it spawns any camp. The layout is `relief_shm_t` in `relief.h`.

| Field | Written by |
|-------|------------|
| `daemon_pid`, `start_time_ms`, `total_requests` (+1 per REQUEST received) | reliefd |
| per camp: `id`, `name`, `pid`, `state`, `restarts` (on every camp-table change) | reliefd |
| per camp: `requests_sent`, `heartbeat_ms` (after every send and once a second) | that camp, in its own row only |

- **Writers** lock a `pthread_mutex_t` that lives in the segment, created with `PTHREAD_PROCESS_SHARED` and
  `PTHREAD_MUTEX_ROBUST`. A camp killed while holding it does not freeze the table: the next locker gets
  `EOWNERDEAD`, repairs it and logs `event=status_lock_recovered`. `tests/helpers/shm_die_locked` tests this.
- **Readers** (`reliefstat`) map the segment `PROT_READ` and cannot lock, because locking writes to the
  mutex. Writers also bump a sequence counter (odd while writing), and the reader copies the table and
  retries if the counter changed. This is a seqlock. Phase 3 will compare this design with POSIX semaphores.
- A camp finds its row by (id, pid), so a manually started camp can never overwrite a supervised camp's row.
  A restarted camp's counters are reset by reliefd when the row's pid changes.
- The camp heartbeat comes from a `timer_create` timer (SIGALRM every second, flag-only handler), so it
  keeps beating while the camp is sleeping or idle in `sigsuspend`.
- reliefd unlinks the segment on shutdown. If reliefd is SIGKILLed, `reliefstat` reports a stale table,
  and the next reliefd removes the segment and creates a fresh one.

## Camp process lifecycle (Phase 1, `src/reliefd/camps.c`)
```
fork+execv ──▶ STARTING ──HELLO──▶ RUNNING ──exit(n)──────────────▶ EXITED
                  ▲                    │
                  └── restart (≤3) ◀───┴──killed by signal──▶ CRASHED (after 3 restarts)
```
- The SIGCHLD handler only sets a flag. The main loop runs `waitpid(-1, WNOHANG)` until nothing is
  left and updates the camp table. SIGCHLD interrupts `mq_timedreceive` (no SA_RESTART), and the
  500 ms receive timeout bounds the check-then-block race.
- The child calls `prctl(PR_SET_PDEATHSIG, SIGTERM)` before `execv`, so camps never outlive a SIGKILLed reliefd.
- Shutdown: SIGTERM to all camps, then up to 2 s spent reaping while still draining the queue, then
  SIGKILL any stragglers and a blocking `waitpid` until ECHILD (no zombies).
- A restarted camp replays its scenario rows. Request ids (`camp_id * 1000000 + csv line`) are
  stable, so the scheduler can de-duplicate them later.

## Phase 2: scheduling (overview)
Phase 2 adds CPU scheduling. Rescue **teams are CPUs** and RESCUE **requests are jobs**, with burst = the
request's service time. The same policy code runs offline in a simulator and live inside reliefd, and
E2 measures the real Linux scheduler for comparison.

### The scheduler interface (`src/sched/sched.h`)
A policy is a table of function pointers, so callers never know which algorithm they are using:

```c
struct sched_policy {
    const char *name;  int preemptive;  int uses_quantum;  void *state;  sched_params_t params;
    int          (*init)(sched_policy_t *, const sched_params_t *);
    void         (*enqueue)(sched_policy_t *, sched_job_t *, int64_t now);   /* arrival or preempted */
    sched_job_t *(*pick_next)(sched_policy_t *, int64_t now);                /* for an idle team */
    void         (*on_tick)(sched_policy_t *, int64_t now,                   /* aging + preemption marks */
                            sched_job_t *const *running, int teams, int *preempt);
    void         (*on_finish)(sched_policy_t *, sched_job_t *, int64_t now);
    void         (*destroy)(sched_policy_t *);
    int          (*queue_lengths)(sched_policy_t *, uint64_t *out, int max); /* optional, for display */
};
```
There are six policies in six files (`fcfs`, `sjf`, `srtf`, `rr`, `prio`, `mlfq`), registered by name in
`sched.c`. The policies never sleep, never lock and never do I/O. They report promotions and demotions
through a callback (`params.on_event`), so the same code works in a single-threaded simulator and
under the dispatcher's mutex.

### One policy library, three users
```
                       src/sched/  (fcfs sjf srtf rr prio mlfq, sched.c registry)
                         |  same function-pointer table  |
            +------------+                               +--------------+
            v                                                           v
  schedsim / test_sched  (sim.c)                         reliefd dispatcher  (dispatch.c)
  simulated clock, 1-minute ticks,                       real time: N team pthreads +
  no sleeping, deterministic                             1 clock thread, mutex + condvar
  -> E1, unit tests, phase2_check                        -> live RESCUE requests from the mq

  linuxsched  (src/linuxsched/)  uses NO policy from src/sched: one real thread per request on one
  pinned CPU, and the KERNEL schedules them (CFS/EEVDF + nice, SCHED_FIFO, SCHED_RR) -> E2
```

### Simulator vs live
| | `schedsim` (offline) | reliefd dispatcher (live) |
|---|---|---|
| Time | Simulated minutes; the loop jumps over idle gaps | Real time: 1 simulated minute = `--time-scale` ms |
| Teams | An array of slots in one thread | One pthread per team |
| `on_tick` | Called by the loop once per minute | Called by the clock thread once per minute |
| Preemption | Takes effect at the same minute boundary | The team acts at its next minute boundary (≤ 1 minute later) |
| Determinism | Byte-identical for a given `--seed` | Varies slightly with thread timing |
| Output | Report, per-request CSV, `--summary-csv` row | `event=dispatch/preempt/complete/promote` log, `reliefctl sched`, `report` |
| Used for | E1, unit tests, `tests/phase2_check.sh` | The running system (`reliefctl simulate --policy P`) |

### Thread model of reliefd
```
                 SIGCHLD, SIGTERM (only the main thread: workers block all signals)
                         |
  camps --mq--> [ main thread ] --RESCUE--> dispatch_submit ---+
                  fork/exec, reap                              |  lock(d->lock)
                                                               v
       +------------- d->lock (mutex) guards everything below -------------+
       |  policy ready queues | running[t] | preempt[t] | job fields      |
       +------^-------------------^---------------------^------------------+
              | on_tick each      | pick_next / enqueue  | cond_wait(d->work)
              | simulated minute  | (preempted job back) | when idle
       [ clock thread ]     [ team 1 ] [ team 2 ] ... [ team N ]
                              work: nanosleep 1 minute OUTSIDE the lock, then update job inside it
       publish_locked() --> shm /reliefos_status   (lock order: d->lock, then the shm lock)
```
- **One mutex, one condition variable.** Idle teams wait on `d->work`, which is signalled on enqueue and
  broadcast on preemption or stop. Teams never sleep while holding the lock.
- **Lock order** is always the dispatcher mutex, then the shared-memory status lock. The main thread
  takes the shm lock without the dispatcher mutex, so there is no cycle.
- **Fork safety:** camps are forked only by the main thread, and the child calls only async-signal-safe
  functions before `execv`.

### Verified by
- `tests/test_sched.c` (138 checks): textbook answers worked out by hand (FCFS 17 / 3, SJF 7.75, SRTF 6.5,
  RR q=4 5.67, priority, MLFQ demotion and aging) and a Gantt chart for each.
- `tests/sched.sh`: every policy on the flood scenario.
- `tests/phase2_check.sh` (gate M2): every policy completes each request exactly once, never finishes
  before arrival + burst, and is byte-identical across same-seed runs; MLFQ starves no one at light load.
- `tests/dispatch.sh`: the live threads (FCFS order, priority preemption, MLFQ aging, clean stop).
  It fails on any ThreadSanitizer report.
- `tests/linuxsched.sh`: pinning, CPU-time budget, nice spacing, real-time priorities, watchdog.
- Results: E1 and E2 in `docs/EXPERIMENTS.md`.

## Phase 2: triage scheduler (`src/sched/`)
The scheduler is a library with no IPC. Rescue teams play the CPUs and requests the jobs (burst = the
request's `service_time`). Both `schedsim` (offline experiments) and, later, reliefd (live triage) drive it.

- **`sched_job_t`** holds the request id, camp, severity, arrival, burst, remaining, start, finish, wait,
  last_run, queue level and team. An intrusive `next` link lets policies queue jobs without allocating.
- **`sched_policy_t`** is a table of function pointers plus a name and a `preemptive` flag:
  `init`, `enqueue(job, now)`, `pick_next(now)`, `on_tick(now, running[], teams, preempt[])`,
  `on_finish(job, now)` and `destroy`. `on_tick` ages ready jobs and marks teams to preempt; it is NULL for
  FCFS. Policies are registered by name in `sched.c`, so experiments swap them with `--policy`.
- **`sim_run()`** is a discrete-time simulation with a 1-minute clock that jumps over idle gaps. Each
  minute it admits arrivals, calls `on_tick` (re-enqueueing preempted jobs), fills idle teams with
  `pick_next` (team 1 first) and runs every busy team for one minute. Same-minute arrivals keep scenario
  order unless `--seed` asks for a seeded but reproducible order.
- **Metrics:**

  | Metric | Definition |
  |--------|------------|
  | Turnaround | finish − arrival |
  | Waiting | turnaround − burst |
  | Response | first start − arrival |
  | Throughput | requests per hour of makespan |
  | Utilisation | busy team-minutes ÷ (teams × makespan) |
  | Context switch | A team hands over directly from one job to another. Picking up work after being idle does not count. |
  | Starved | waiting > `--starve` |
- **FCFS baseline** (flood scenario, 3 teams): average wait 401.9 min, CRITICAL 382.8 min, 166 of 200 starved,
  98.5% utilisation. The scenario oversubscribes 3 teams, and FCFS ignores severity, which motivates the
  priority-based policies and experiments E1/E2.
- **Policies** (one file each, registered by name in `sched.c`). Every file starts with a comment on the
  algorithm, its trade-off and what it means for disaster relief.

  | Policy | Preemptive | Picks | Notes |
  |--------|------------|-------|-------|
  | `fcfs` | no | Earliest arrival | Convoy effect |
  | `sjf` | no | Shortest burst | Minimum average wait among non-preemptive policies; long rescues starve |
  | `srtf` | yes | Least remaining time | Optimal average wait; more switches; strongest starvation of long jobs |
  | `rr` | yes | FIFO, `--quantum` slices | A request that uses its quantum goes to the back only if others wait |
  | `prio` | `--preemptive on\|off` | Severity (CRITICAL > URGENT > ROUTINE) | FCFS ties; no aging, so it starves on purpose |
  | `mlfq` | yes | Highest non-empty queue | Q0 RR 8, Q1 RR 16, Q2 FCFS; entry level by severity; demote on full quantum; aging promotes after `--age` min |

  The simulator keeps `slice` (minutes since dispatch), `ready_since` and `seq` (arrival order, the FCFS
  tie-break) on every job. `sched_preempt_worst()` gives SRTF, preemptive prio and MLFQ the same
  multi-team rule: the best ready jobs fill idle teams first, then each one that is *strictly* better than
  the worst running job replaces it. Policies report promotions and demotions through
  `params.on_event`; the scheduler itself does no I/O.
- **Unit tests** (`tests/test_sched.c`, 138 checks, a Gantt chart per test). Every answer is worked out
  by hand in a comment and asserted exactly: FCFS 24/3/3 gives 17 (reordered: 3), SJF on the standard
  4-process example gives 7.75, SRTF 6.5, RR with quantum 4 gives 5.67, preemptive and non-preemptive
  priority, MLFQ demotion Q0 → Q1 → Q2, and starvation. Under a steady critical stream, plain
  priority makes a ROUTINE job wait 499 min, while MLFQ promotes it at t=62 and finishes it at t=119
  (wait 113), matching a minute-by-minute hand trace.
- **Flood scenario, 3 teams.** SJF/SRTF halve the average wait but do nothing for CRITICAL. Priority
  cuts CRITICAL waits from 383 to about 30 minutes while everything else waits longer. MLFQ does
  *worse* than FCFS for CRITICAL (525 min), because the scenario overloads 3 teams for hours:
  nearly every waiting request ages past 60 minutes (522 promotions) and floods Q0, and long critical
  rescues are demoted for using their 8-minute quantum. Under sustained overload, aging erodes the
  priority it was meant to protect. Experiments E1/E2 should quantify this (age, quanta, team count).

## Phase 2: live scheduling in reliefd (`src/reliefd/dispatch.c`)
Thread model of reliefd:

| Thread | Job | Blocks in |
|--------|-----|-----------|
| main | mq receive loop, SIGCHLD reaping, fork/exec of camps; hands RESCUE requests to the dispatcher | `mq_timedreceive` |
| team 1..N (`--teams`) | take a job (`pick_next`), work it one simulated minute at a time, act on preemption | `pthread_cond_wait` (idle), `nanosleep` (working) |
| clock | once per simulated minute: the policy's `on_tick` (aging, preemption marks) | `clock_nanosleep` (absolute) |

- **One mutex** (`d->lock`) guards the policy and its ready queues, the `running[]`/`preempt[]` arrays,
  every job field and the counters. Teams sleep *outside* it and update their job inside it. Idle
  teams wait on one **condition variable**, signalled on enqueue and broadcast on preemption or stop.
  The wait re-checks its condition in a loop, which handles spurious wakeups and the case where
  another team took the job first.
- **Preemption** is decided by the policy in `on_tick` on the clock thread, which marks `preempt[t]`.
  Team t acts at its next minute boundary: it puts the job back with its remaining time and picks
  again. Each `event=preempt` has a `reason`: `priority` (something more important), `quantum` (RR
  rotation) or `demote` (MLFQ level change; the same job often resumes at once if nothing better waits).
- **Lock order** is always the dispatcher mutex, then the shared-memory status lock (to publish
  `reliefctl sched` data), never the reverse. The main thread takes the status lock without the
  dispatcher mutex. There is no cycle, so no deadlock.
- **Signals and fork:** worker threads are created with every signal blocked, so SIGCHLD and SIGTERM
  still reach the main thread, whose `mq_timedreceive` relies on EINTR. Camps are forked only by the
  main thread (`PR_SET_PDEATHSIG` follows the forking thread), and the child calls only
  async-signal-safe functions before `execv`. That is the rule for `fork()` in a multithreaded process.
- **Time:** times are in simulated minutes (`--time-scale` ms each; 0 is treated as 1 ms). Wait,
  turnaround and response are measured in milliseconds and logged in minutes. Live runs are not
  bit-for-bit reproducible because thread timing varies; `schedsim` is the deterministic counterpart.
- **Verified:** `tests/dispatch.sh` forces each behaviour with small scenarios: FCFS dispatches in
  arrival order, CRITICAL preempts ROUTINE under `prio --preemptive on`, MLFQ aging promotes a
  starving job, and stopping abandons queued work cleanly. It fails on any ThreadSanitizer report.

## Phase 3: critical section (`src/racelab/`)
`build/racelab` is a standalone program with no IPC: two threads, "camps", share one counter, "the
boat's inventory". Each entry/exit section sits behind a `lock_ops_t` (`init`, `lock(self)`,
`unlock(self)`, `destroy`), so `--mode` swaps the protocol without touching the increment loop:

| Mode | Mechanism | Kernel involvement |
|------|-----------|--------------------|
| `none` | none | none |
| `peterson-nofence` | flag[2] + turn, plain volatile ints | none (fails on x86: store buffers reorder store → load) |
| `peterson` | flag[2] + turn as C11 atomics, `atomic_thread_fence(seq_cst)` after the stores | none |
| `spinlock` | `atomic_flag_test_and_set` (XCHG), `pause` while spinning | none |
| `mutex` | `pthread_mutex_t` (glibc: CAS in user space, futex wait/wake when contended) | futex, only under contention |
| `semaphore` | `sem_init(&s, 0, 1)`, `sem_wait` / `sem_post` | futex, only under contention |

- **Counter:** `volatile`, so each increment is a real load and store that the compiler cannot merge.
- **Start:** a `pthread_barrier_t` releases both threads together.
- **ThreadSanitizer:** `none` and `peterson-nofence` race on purpose, so `tests/racelab.sh` skips them under
  TSan. `peterson` uses release/acquire on the flags in addition to the fence. TSan does not model
  fences, and release/acquire gives it the happens-before edge it needs.
- **Results:** experiment E4 in EXPERIMENTS.md. `docs/strace_futex.txt` has the futex counts.

## Phase 3: loading dock and inventory (`src/reliefd/dock.c`, `inventory.c`, `src/common/rwsync.c`)
### Producer–consumer: warehouses → loading dock → trucks
```
main thread (mq loop)             P producer threads                  C truck threads
 kit request ──dock_submit──► orders FIFO ──► take kits from inventory ──► dock ──► deliver (sleep distance)
                              (mutex+cond)    (write lock), pack                    read stock (read lock)
                                              1 + qty/50 min        bounded buffer:
                                                                    empty / full / mutex semaphores
```
- **Which requests:** FOOD_KIT, MED_KIT and HYGIENE_KIT requests go to the dock (`dock=1` on the
  `event=request` line). RESCUE requests go to the scheduler. TRUCK and BOAT are logged only.
- **The dock** is a circular buffer of `--dock` slots (default 8), protected by the three
  textbook semaphores: `empty` = free slots, `full` = packages, `mutex` = buffer update. The
  textbook pseudocode sits next to `dockbuf_put` / `dockbuf_take`.
  - **Wait order:** `wait(empty)` comes before `wait(mutex)`. The other order deadlocks when the
    dock is full.
  - **Logging:** each put or take logs `event=produce` / `event=consume` with `occupancy=` from
    inside the critical section, so the log's occupancy sequence is the buffer's exact history
    (`tests/dock.sh` replays it).
  - **Blocked time:** a producer that finds the dock full (`sem_trywait` fails) times its
    `wait(empty)`. That time goes into `blocked_min=` on its produce line and into the
    `blocked_puts` / `blocked_min` totals.
- **Threads:** `--producers` (default 2) and `--trucks` (default 4). A trip to camp c takes
  `dock_distance_min(c)` simulated minutes, a fixed table of 4–16 min for the 8 Pune camps.
  Producers and trucks block every signal, as the rescue teams do, so SIGCHLD and SIGTERM always
  interrupt the main thread's `mq_timedreceive`.
- **Stop** (`dock_stop`, after the scheduler stops):
  1. Pending orders are abandoned, and packing and trips in progress are cut short (logged).
  2. The producers are joined.
  3. One poison pill (package id 0) per truck goes through the same buffer, and the trucks are
     joined.
  4. `event=dock_stop` logs the totals, including the time-weighted average occupancy.
- **Status:** the main loop copies the dock counters into the shm status table, which
  `reliefstat` / `reliefctl status` print. `reliefctl report` shows the dock's average occupancy
  and the time producers spent blocked on a full dock.

### Readers–writers: the inventory
- **Data:** stock of 3 kit types in 2 warehouses (camp c is served by warehouse (c − 1) mod 2).
- **Readers:** the main thread's status publisher and every truck's delivery note (`stock_left=`).
  The Banker's checks will join them later.
- **Writers:** producers deducting the kits they pack, and restocks (+2000 when a pack would go
  below zero).
- **One interface, two implementations** (`include/rwsync.h`, chosen with `reliefd --rw`):
  - `semaphore`: the textbook first readers-writers solution, with `read_count`, `mutex` and
    `rw_mutex`. `rw_mutex` must be a semaphore, not a mutex: the first reader takes it and the last
    reader, a different thread, releases it. Readers have preference, so overlapping readers
    starve a writer.
  - `rwlock` (the default): `pthread_rwlock_t` with `PTHREAD_RWLOCK_PREFER_WRITER_NONRECURSIVE_NP`.
    A waiting writer blocks new readers.
- **`build/rwlab`** measures the difference (E4b).
- **Lock order:** the inventory lock and the dock `mutex` are never held together with the shm
  lock. Each publisher copies under its own lock, releases it, then writes the shm.

## Phase 3: deadlock avoidance (`src/deadlock/`, `src/reliefd/resmgr.c`)
### Resources and units
The Banker's algorithm manages four resource types, in this column order everywhere:

| Resource | Total (`scenarios/resources.conf`) | Unit |
|----------|-----------------------------------:|------|
| TRUCK | 3 | one vehicle |
| BOAT | 2 | one vehicle |
| MED_KIT | 40 | a crate of 10 kits |
| FOOD_KIT | 60 | a crate of 10 kits |

- **Units of a request:** `relief_banker_units()`: vehicles 1:1, kits rounded up to whole crates.
- **Oversized vehicle requests:** a request for more vehicles than the fleet (the scenario has up
  to 4 trucks and 3 boats) is capped at the fleet size by the camp.
- **Other requests:** RESCUE goes to the scheduler and HYGIENE_KIT to the loading dock, as before.

### Protocol (`include/relief.h`)
```
camp                                        reliefd
 HELLO   max_claim[4], flags ─────────────►  claim: Max row of the camp (banker_set_max)
 REQUEST units ──────────────────────────►  resmgr queue ─► allocator thread: banker_request
         ◄──────────── MSG_GRANT / MSG_REJECT on /reliefos_camp_<id>   (kits ─► loading dock)
 ... hold service_time simulated minutes ...
 RELEASE units ──────────────────────────►  banker_release, retry waiting requests
 BYE (or the process dies) ──────────────►  banker_remove: everything it held comes back
```
- **Max claim:** each camp computes it from its own scenario rows: the most units it would hold
  at once if every row were granted on arrival and held for its service time, capped at the total.
  The camp never exceeds the claim. A request that would take it over waits *in the camp* until
  the camp's own releases make room, so `request > need` (a rejection) cannot happen in normal
  operation.
- **Reply queue:** each camp creates its own reply queue (`RELIEF_REPLY_MQ_FMT`) before HELLO,
  replacing any stale one, and removes it at exit. reliefd removes the queue of a camp that died.
- **Random mode:** camps started with `--count N` claim the totals, have no reply queue, and send
  BYE at once, so their grants are returned by the BYE.

### Allocator (`resmgr.c`)
- **Threads:** the main thread only queues requests, releases and claims. One allocator thread
  owns the decisions: it sleeps on a condition variable while nothing has changed, then retries
  every waiting request in arrival order through the `--deadlock` policy (`src/deadlock/deadlock.c`,
  a function-pointer table; only `avoid` = `banker_request` is implemented).
- **Logging:** each grant logs the safe sequence found. A refusal logs `deny_unsafe` or
  `wait_unavailable`, each at most once per request.
- **Undeliverable grants:** if a GRANT cannot be delivered (the reply queue stays full), the
  allocation is undone and the request keeps waiting, so a camp can never hold resources it does
  not know about.
- **Process termination:** BYE and the death of a camp process (`camps.c` calls `on_exit` from
  `waitpid`, before any restart) call `banker_remove`, the textbook "process terminates and
  returns all its resources".

### Safety algorithm (`banker.c`)
- **Algorithm:** the textbook Work/Finish algorithm.
- **Search order:** "find an i" is done as repeated passes over P0..Pn-1 that carry on after a
  find instead of restarting at P0. This is the usual hand-worked order, and it reproduces the
  textbook's `<P1, P3, P4, P0, P2>` (`tests/test_banker.c`).
- **Reuse:** `banker.c` is pure (no threads, no IPC). `reliefstat --banker` / `reliefctl banker`
  rebuild the state from shared memory (`bk_*` fields, shm version 4) and run the same safety
  algorithm to print a safe sequence of the current state, under the Allocation, Max, Need and
  Available matrices.

## Phase 3: deadlock characterization, prevention, detection and recovery (`--deadlock`)
### Tasks: where hold-and-wait comes from
A lone scenario row is a job on its own: the camp holds the resource for the row's service time and
never waits while holding, so lone rows cannot deadlock. A scenario may add an optional 7th CSV column,
`task`:
- **Acquisition:** rows of one camp that share it form a task, and its rows are requested one after
  another, each once it is due and the previous one is granted.
- **Holding:** a task keeps everything it got until the last row is granted, then holds it all for
  the task's longest service time and releases it together. While it waits for its next row it
  holds the earlier ones, which is hold and wait.
- **The camp's claim** covers a task's rows for the whole time the task holds them.

`scenarios/deadlock.csv` (run with `--resources scenarios/deadlock_resources.conf`: 2 trucks, 2 boats):
1. By minute 2, camps 1 and 2 each hold a truck and a boat.
2. At minute 10, camp 1 asks for a truck and camp 2 for a boat. Each wants what the other holds.

`scenarios/deadlock_mix.csv` (`experiments/gen_deadlock.py`): 6 camps × 3 random tasks of 2–3
resources, with trucks and boats in both orders.

### The four Coffman conditions per mode
| `--deadlock` | Mutual exclusion | Hold and wait | No preemption | Circular wait | Deadlock possible? |
|--------------|:---:|:---:|:---:|:---:|---|
| `none` | holds | holds | holds | holds | **yes**: `deadlock.csv` hangs |
| `avoid` | holds | holds | holds | holds | no: the Banker's algorithm never enters an unsafe state, so the conditions never combine into a deadlock |
| `prevent` | holds | holds | holds | **broken** | no: each task acquires in the order TRUCK < BOAT < MED_KIT < FOOD_KIT |
| `prevent --all-or-nothing` | holds | **broken** | holds | holds | no: a task gets everything in one request or nothing |
| `detect` | holds | holds | **broken** (on recovery) | holds | yes, but it is found and broken |

Mutual exclusion is never broken: a truck or a boat cannot be shared.

### How each mode works (`src/deadlock/deadlock.c`, `src/reliefd/resmgr.c`)
- **`none`** (`deadlock_grant_if_free`): grant whenever the request fits Available, otherwise the
  request waits.
- **`prevent`** (`deadlock_ordered`):
  - A request is rejected (`event=reject_order`, `MSG_REJECT` with `REQ_FLAG_ORDER`) while its
    *task* holds a resource ordered after it. Every request carries its task id in
    `relief_bundle_t.task`.
  - The camp then releases that task's rows and acquires them again in order.
  - The order is per task, not per camp. A camp runs independent tasks, and enforcing it per camp
    let one task block behind another task of the same camp, which hung the random mix.
- **`prevent --all-or-nothing`:**
  - reliefd starts the camps with `--all-or-nothing`, and each task is sent once all its rows are
    due, as one `REQ_FLAG_BUNDLE` request with the whole vector. It is granted only all at once.
  - A bundle's kit crates go on to the loading dock as one package per kit type.
- **`detect`:** grant-if-free plus a detector thread that wakes every `--detect-interval` seconds
  (default 1):
  1. It builds the **wait-for graph** (`src/deadlock/waitfor.c`) from the Allocation and Request
     tables. There is an edge X → Y when X waits for units of a resource that is not available and
     Y holds some of it.
  2. It looks for a cycle with **DFS** (white/grey/black colouring: an edge to a grey node closes a
     cycle).
  3. With several instances per type, a cycle is necessary but not sufficient. So the cycle is
     confirmed with the multi-instance **detection algorithm** (`banker_detect`: Work/Finish with
     the Request matrix).
  4. On a confirmed deadlock it logs `event=deadlock_detected cycle=camp1->camp2->camp1` and
     recovers.
  5. **Recovery:** the camp in the cycle whose waiting request has the lowest severity is the
     victim. Its grant of a vehicle that the previous camp in the cycle waits for is rolled back
     and the request is queued again (`event=recovery ... requeued=1`). The camp gets `MSG_REVOKE`
     and waits for a new GRANT.
  6. **Edge case:** a camp that already released a revoked grant just gets the requeued copy
     dropped (`stray_release`), and a GRANT for something it no longer wants is handed straight
     back.
- **Message order:** before reaping exited camps, reliefd reads what is still in the request
  queue. A camp's last RELEASEs and BYE are handled before the exit releases what it still holds.

**`build/dlsim`** (`src/dlsim/dlsim.c`, experiment E3) replays a scenario's Banker's resources with the
same policies and camp rules on a one-minute clock, with no threads or IPC, so every mode can be
compared deterministically.
- **Deadlock count:** it counts deadlocks with the detection algorithm over *jobs* (tasks and lone
  rows), every minute.
- **Task reservation:** a camp starts a task only when the whole task fits in its claim, and
  reserves it until the task ends, in both dlsim and `src/camp/main.c`. Otherwise two tasks of the
  same camp can each take part of the claim and wait for the rest, a deadlock *inside* the camp.
  E3's first runs showed exactly that.

`reliefctl waitfor` prints the graph from shared memory (`bk_proc[].request`) as edges, plus any
cycle. With `--dot` it prints Graphviz DOT, and `docs/waitfor.dot` is a live capture of
`deadlock.csv` under `none`:
```sh
sudo apt install graphviz
dot -Tpng docs/waitfor.dot -o docs/waitfor.png
```

## Phase 3: the lock hierarchy (why reliefd itself cannot deadlock)
reliefd runs many threads:
- the main thread;
- the rescue teams and the clock;
- the dock's producers and trucks;
- the Banker's allocator and the detector.

The camps are separate processes. The rule that keeps our own code free of deadlocks is resource
ordering, the same technique as `--deadlock prevent`, applied to locks. Every lock has a level. A
thread may take a lock only if every lock it already holds has a *lower* level. A cycle of threads
each waiting for a lock the next one holds would need L1 < L2 < … < L1, which is impossible, so the
circular-wait condition can never hold.

| Level | Lock | Kind | Guards | Taken by |
|------:|------|------|--------|----------|
| 1 | `resmgr.lock` (+ condvars `wake`, `tick`) | pthread mutex | Banker's state, waiting/granted lists, reply queues | main thread, allocator, detector |
| 1 | `dispatcher.lock` (+ condvar `work`) | pthread mutex | ready queue, running jobs, counters | main thread, teams, clock |
| 2 | `dock.olock` (+ condvar `ocond`) | pthread mutex | the dock's order FIFO | main thread, allocator (via `on_grant`), producers |
| 3 | dock `mutex` (and the counting semaphores `empty`, `full`) | POSIX semaphores | the circular buffer and its statistics | producers, trucks, main thread (`dock_publish`) |
| 3 | `inventory.rw` | rwsync: semaphores or pthread rwlock | stock per warehouse | producers (write), trucks + main thread (read) |
| 4 | shm `lock` (`/reliefos_status`) | process-shared, robust pthread mutex | the status table | reliefd's publishers, every camp (its own row) |
| 5 | `g_log_lock` | pthread mutex | the log sinks | everyone; always last |

The two locks at level 1 are never held together, and neither are the two at level 3.

```
   level 1   resmgr.lock ───────────────┐            dispatcher.lock
                │ on_grant: kits → dock  │                  │ publish_locked
                ▼                        │                  │
   level 2   dock.olock                  │                  │
                                         │                  │
   level 3   dock mutex (sem)   inventory.rw                │
                │ note_produce/consume   (taken alone)      │
                │                        │                  ▼
   level 4      │                        │            shm status lock ◄── camps (alone)
                ▼                        ▼                  │
   level 5   g_log_lock ◄─────────────────────────────────────┘   (leaf: never takes another lock)
```
Every arrow points down. These are the only places that take a second lock while holding one,
checked by reading every lock site:
- **resmgr → dock order queue:** `resmgr.lock → dock.olock`: a granted kit request goes on to the
  dock (`on_grant` → `dock_submit`).
- **dispatcher → shm:** `dispatcher.lock → shm lock`: `publish_locked` copies the scheduler state
  into the status table.
- **anything → log:** `dock mutex → log` (produce/consume are logged inside the buffer's critical
  section, so the log is the buffer's exact history), `resmgr.lock → log`, `dispatcher.lock → log`,
  `shm lock → log` (a robust-mutex recovery warning).

**Copy, release, publish.** The dock, the inventory and the resource manager never nest with the
shm lock. Each copies its state under its own lock, releases it, then takes the shm lock
(`dock_publish`, `inventory_publish`, `resmgr_publish`), which keeps the graph shallow.

**Waits that are not locks:**
- **Dock semaphores:** a producer waits on `empty` and a truck on `full` *before* taking the
  buffer's `mutex`, and never while holding any other lock. Taking `mutex` first would deadlock a
  full dock: the textbook ordering note next to `dockbuf_put`.
- **Condition variables** release their mutex while waiting.
- **Reply queues:** the allocator sends replies to the camps' queues while holding
  `resmgr.lock`, but with a bounded `mq_timedsend` (5 × 100 ms). A camp that stops reading can
  delay the allocator, never block it forever. An undeliverable grant is undone, so the Banker's
  state stays consistent.

**Readers outside the hierarchy:**
- `reliefstat` and `reliefui` map the status table read-only and copy it with the seqlock
  (`relief_shm_snapshot`). They never take the shm mutex, so neither can delay a writer.
- That matters for the dashboard. Drawing is slow terminal I/O, and a UI stopped with Ctrl-Z in
  the middle of a copy would otherwise hold the lock indefinitely, because a robust mutex only
  recovers when its holder dies, not when it is stopped.
- The UI also reads `/proc/<pid>/stat` for the camps' states and the tail of `reliefd.log`. It
  runs `reliefctl` for start/stop/alert/dump, and `kill(SIGKILL)` for `k`.

**Signals and processes:** worker threads block every signal, so handlers (which only set flags)
always run on the main thread, where no lock is held at that point. A camp is single-threaded. It takes the shm lock to update its own row, and inside it at most its
own log lock (level 4 → 5, a recovery warning). It never waits for reliefd while holding them. The robust mutex
(`EOWNERDEAD`) means a camp that dies holding it cannot block reliefd.

## Phase 4: memory management (`src/mem/`)
Two standalone simulators; reliefd is unchanged. Each one's algorithms sit behind a function-pointer
table: `mem_ops_t` in `mem.h`, `page_policy_t` in `paging.h`.

| Relief world | Memory concept | Where |
|--------------|----------------|-------|
| Warehouse floor (1024 units) | Physical memory | `memsim --size` |
| Shipment arriving / leaving | Process allocating / freeing memory | `memsim` trace: `alloc` / `free` |
| Free floor between shipments | Holes in the free list | `fit.c` (first / best / worst fit) |
| Power-of-two floor block | Buddy block | `buddy.c` |
| Kit cache (FOOD, MED, HYGIENE) | Slab cache of fixed-size objects | `slab.c` (16-unit slabs, full/partial/empty lists) |
| Camp shelf slot | Page frame | `pagesim --frames` |
| Item the camp needs | Page reference | `pagesim --refs / --trace / --seed` |
| Stock-out | Page fault | `paging.c` (FIFO, LRU, Optimal, expiry-first) |
| Stock expiring on the shelf | (no OS analogue: perishables) | expiry-first policy, `expired` count |

Results: E5 (fragmentation and failures) and E6 (faults vs frames) in `docs/EXPERIMENTS.md`.

## Phase 5: disk scheduling and the journal (`src/io/`)
**Disk scheduling: a standalone simulator.** `routesim` plans a truck's route along one highway.
The policies sit behind `disk_policy_t` in `disk.h`.

| Relief world | Disk concept | Where |
|--------------|--------------|-------|
| Highway, km 0–199 | Disk, cylinders 0–199 | `routesim --max` |
| Truck | Disk head | `--start KM --dir up|down` |
| Delivery stop | I/O request | `--stops LIST` or `--seed S` |
| Order of the stops | Disk scheduling policy | `disk.c`: FCFS, SSTF, SCAN, C-SCAN, LIFO |
| km driven | Seek distance | `total` (C-SCAN's empty return jump counted, shown separately) |
| Km driven before a stop is reached | Request wait | `worst_wait`, `worst detour` |

SCAN and C-SCAN move up and go to the end of the highway. C-SCAN's jump back to 0 counts as
distance. On the textbook queue from 53: FCFS 640, SSTF 236, SCAN 331, C-SCAN 382.

**The journal: inside reliefd.** `journal.c` makes `resmgr.c`'s Banker's state survive a crash.

Record (40 bytes, little-endian; the same layout in `journal.log` and `snapshot.bin`):

| Bytes | Field | Meaning |
|------:|-------|---------|
| 0–7 | `u64 seq` | 1, 2, 3, ... with no gaps; continues across checkpoints |
| 8–11 | `u32 type` | 1 CLAIM (max claim), 2 GRANT, 3 RELEASE (also a revoke), 4 RELEASE_ALL (camp gone), 5 SNAPSHOT (header) |
| 12–15 | `u32 camp` | camp id |
| 16–19 | `u32 id` | request id (GRANT and RELEASE); for SNAPSHOT, the number of records that follow |
| 20–35 | `i32 units[4]` | TRUCK, BOAT, MED_KIT, FOOD_KIT (a bundle is one record, so it stays atomic) |
| 36–39 | `u32 crc32` | IEEE CRC-32 of bytes 0–35 (the same as `zlib.crc32`) |

Write path, under resmgr's lock:
1. The Banker's check grants tentatively.
2. The record is appended with `write()` and `fsync()`.
3. The camp is told and the log line (`seq=`) is written.

If step 2 fails, the grant is rolled back. A lost release or RELEASE_ALL only over-reserves after
a crash, so it is never unsafe.

Every 100 records, a checkpoint:
1. Write the state (a SNAPSHOT header, one CLAIM per camp, one GRANT per held grant) to `snapshot.tmp`, then `fsync`.
2. `rename()` it over `snapshot.bin`, which is atomic, then `fsync` the directory.
3. Truncate `journal.log`.

Recovery (`journal_open`, called from `resmgr_start` before any thread or camp exists):
1. Load `snapshot.bin`: its header seq S, then each claim and grant. A corrupt snapshot stops reliefd.
2. Replay `journal.log`: skip records with seq ≤ S. Stop at a torn or bad-CRC record, a seq gap,
   or a record that does not fit the state. Only the last write can be torn, since every earlier
   one was fsync'ed.
3. Rebuild the Banker's tables: `banker_set_max` per claim, `banker_set_alloc` per camp, and the held-grant list.
4. Log `event=recovered records= snapshot_seq= last_seq= skipped_bytes= alloc=`, then checkpoint at
   once, which drops a torn tail.
5. When each camp process is restarted and says HELLO again, its recovered grants are released.
   This is process termination, as for any camp that exits.

`reliefd --no-journal` turns all of this off (experiment E9). Results: E8 (routing) and E9
(journal: allocations lost, fsync cost) in `docs/EXPERIMENTS.md`.

## Structured log (`$RELIEF_HOME/logs/reliefd.log`)
One event per line: `ts=<iso8601> level=<lvl> comp=reliefd pid=<pid> event=<name> key=value ...`.
Values contain no spaces, except `msg="..."` on free-form `event=log` lines. Events include
`daemon_start`, `camp_spawn`, `camp_running`, `camp_exit`, `camp_signaled`, `camp_restart`,
`camp_crashed`, `camp_killed`, `camp_final`, `hello`, `request` (with `sched=1` for RESCUE), `bye`, `shutdown`,
`daemon_stop`, and from the scheduler `sched_start`, `dispatch`, `preempt` (`reason=`), `complete` (`wait=`,
`turnaround=`, `response=` in simulated minutes), `promote`, `demote` and `sched_stop`.

## TODO
- [x] Component diagram (Phase 1 process model above)
- [ ] Request lifecycle (camp → scheduler → allocator → journal); the router (`routesim`) is offline
- [x] Thread model inside reliefd (Phase 2 live scheduling section)
- [x] Policy plug-in interfaces (function-pointer tables): `sched_policy_t`, Phase 2 section above
