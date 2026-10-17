# ReliefOS Architecture

> Updated as phases land. Phase 1 (Foundation) is complete; later phases add the scheduler,
> synchronisation, memory management, journaling and the kernel module inside or around reliefd.

## Components
- **reliefd**: coordinator daemon (triage scheduler, deadlock control, sync layer,
  warehouse allocators, delivery router, write-ahead journal)
- **camp**: one process per relief camp; client of the request queue
- **reliefctl**: Bash control CLI
- **reliefstat**: read-only viewer of the live status table
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

## Structured log (`$RELIEF_HOME/logs/reliefd.log`)
One event per line: `ts=<iso8601> level=<lvl> comp=reliefd pid=<pid> event=<name> key=value ...`.
Values contain no spaces, except `msg="..."` on free-form `event=log` lines. Events include
`daemon_start`, `camp_spawn`, `camp_running`, `camp_exit`, `camp_signaled`, `camp_restart`,
`camp_crashed`, `camp_killed`, `camp_final`, `hello`, `request`, `bye`, `shutdown` and `daemon_stop`.

## TODO
- [x] Component diagram (Phase 1 process model above)
- [ ] Request lifecycle (camp → scheduler → allocator → router → journal)
- [ ] Thread model inside reliefd
- [x] Policy plug-in interfaces (function-pointer tables): `sched_policy_t`, Phase 2 section above
