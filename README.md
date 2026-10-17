# ReliefOS: Linux-Based Smart Disaster Relief Resource Management

Operating Systems course project (Semester 5). ReliefOS models a disaster-relief network as an
operating system. Every feature demonstrates a named syllabus topic, so the project is also a
working demo of the OS syllabus.

| Relief world    | OS concept       |
|-----------------|------------------|
| Relief camps    | Processes        |
| Supplies        | Memory           |
| Trucks / boats  | Shared devices   |
| Rescue requests | Jobs to schedule |

**Status:** Phase 1 (foundation) complete. `reliefd` is the parent of all camp processes. It
fork/execs one camp per line of `scenarios/camps.conf`, reaps them on SIGCHLD, restarts crashed
camps, and shuts them all down cleanly. Camps send their scenario requests over a POSIX message
queue, each at its arrival time on a simulated clock (1 simulated minute = 100 ms by default).
Phase 2 adds a triage scheduler: RESCUE requests are scheduled live onto rescue-team threads inside
reliefd (`--policy`, `--teams`), and `schedsim` replays scenarios offline.

## Components
- `reliefd`: the coordinator daemon (C11, pthreads)
- `camp`: one process per relief camp, started by reliefd. It sends requests to reliefd over `/reliefos_requests`
- `reliefctl`: Bash CLI
- `reliefui`: live ncurses dashboard, like htop (`make ui` or `scripts/reliefctl ui`)
- `scripts/demo.sh`: a guided demo of Phases 1-3, one syllabus topic per step (`-u` adds the dashboard)
- `relief.ko`: kernel module exposing `/proc/relief` (Phase 6)
- `systemd/reliefd.service`: systemd unit template (Phase 6)

## Prerequisites
Ubuntu 24.04 (WSL2 works):
```sh
sudo apt install build-essential valgrind shellcheck libncurses-dev   # ncurses: the reliefui dashboard
```

## Build, run, test
```sh
make                 # builds build/reliefd and build/camp
make test            # tests/sched.sh (textbook FCFS/SJF/SRTF/RR/prio/MLFQ unit tests, all policies),
                     # tests/smoke.sh (IPC, reliefctl), tests/camps.sh (fork/exec, restarts, no zombies),
                     # tests/traffic.sh (200-row paced replay, queue-full retries),
                     # tests/signals.sh (alert, dump, graceful SIGTERM),
                     # tests/status.sh (shared-memory status table, robust mutex recovery),
                     # tests/cli.sh (reliefctl ps/tree/report/simulate, stockcheck.sh),
                     # tests/dispatch.sh (live rescue-team threads: dispatch, preemption, aging),
                     # tests/racelab.sh (critical section: Peterson, mutex, spinlock, semaphore),
                     # tests/sync.sh (bounded buffer, readers-writers, rwlab), tests/dock.sh (loading dock),
                     # tests/banker.sh (Banker's algorithm: textbook example, live grants/releases),
                     # tests/deadlock.sh (deadlock.csv under none/prevent/avoid/detect, waitfor)
make ui              # live ncurses dashboard (build/reliefui); s starts ReliefOS from inside it
make tsan            # ThreadSanitizer build (build/tsan/) + tests
make valgrind        # tests under valgrind (camps too)
make e1              # experiment E1: 9 variants x 5 seeds + E1b load sweep -> experiments/results/e1*.csv + graphs
make e2              # experiment E2: real threads under CFS/EEVDF+nice, SCHED_FIFO, SCHED_RR vs simulated
                     #   (fifo/rr first: sudo setcap cap_sys_nice+ep build/linuxsched)
make e3              # experiment E3: deadlock modes compared with build/dlsim (deterministic) -> graphs
make e4              # experiment E4: every racelab mode x 10 runs -> experiments/results/e4.csv + graphs,
                     #   plus strace futex counts in docs/strace_futex.txt (sudo apt install strace)
make e4b             # experiment E4b: readers-writers writer starvation, rwlab semaphore vs rwlock
make clean
```

One command, end to end (generate a scenario, run it until every camp sends BYE, report):
```sh
scripts/reliefctl simulate flood --camps 8 --seed 42
```

Try it by hand:
```sh
scripts/reliefctl start                    # reliefd + the 8 camps from scenarios/camps.conf
scripts/reliefctl status                   # live table from shared memory (run it again: counts move)
scripts/reliefctl sched                    # live scheduler: policy, ready queue per level, each team's job
build/reliefstat --json                    # the same table as JSON
ps --ppid "$(cat run/reliefd.pid)" -o pid,stat,etime,args   # the 8 camp children
kill -9 <a camp pid>                       # reliefd reaps it and restarts it (max 3 times)
grep -c 'event=request ' run/logs/reliefd.log   # climbs to 200 over ~36 s (360 simulated minutes)
scripts/reliefctl alert 3                  # SIGUSR1: camp 3's next 5 requests become CRITICAL
grep ' camp=3 ' run/logs/reliefd.log | grep alert=1   # ...severity=CRITICAL alert=1 orig_severity=ROUTINE
scripts/reliefctl dump all                 # SIGUSR2: every camp prints its counters
scripts/reliefctl ps                       # PCB fields from /proc: state, threads, RSS, context switches, CPU
scripts/reliefctl tree                     # process tree rooted at reliefd
scripts/reliefctl report --since 10        # requests per camp / severity / resource, busiest camp
grep 'event=camp_' run/logs/reliefd.log    # structured key=value log
scripts/reliefctl stop                     # SIGTERM camps, SIGKILL stragglers after 2 s, reap all
```
Set `RELIEF_TIME_SCALE=10` to replay the scenario 10x faster (10 ms per simulated minute).

### Live scheduler in reliefd
RESCUE requests are scheduled onto `--teams N` rescue-team threads (default 3) with `--policy P` (default
`fcfs`; also `sjf srtf rr prio mlfq`). Each team works `burst × time-scale` in one-minute slices. A clock
thread runs the policy's aging and preemption once per simulated minute. Kit and vehicle requests are only
logged for now (Phase 3/4). Every step is logged as `event=dispatch|preempt|complete|promote|demote`.
```sh
RELIEF_POLICY=mlfq RELIEF_TEAMS=3 scripts/reliefctl start && scripts/reliefctl sched
scripts/reliefctl simulate flood --camps 8 --seed 42 --policy mlfq   # report includes per-severity waits
```

### Scheduling simulator
`build/schedsim` replays a scenario through a scheduling policy on N rescue teams (= CPUs), in simulated
minutes and with no real sleeping, so a run is instant and fully reproducible:
```sh
build/schedsim --policy fcfs --scenario scenarios/flood_pune.csv --teams 3 --gantt --csv /tmp/fcfs.csv
```
It reports the average waiting, turnaround and response time (overall and per severity), the worst-case
wait, throughput, team utilisation, context switches and starved requests (wait > `--starve`, default
120 min). `--gantt` draws the first 60 minutes per team, and `--csv` writes one row per request.
Policies: `fcfs`, `sjf`, `srtf`, `rr` (`--quantum`, default 10), `prio` (by severity, `--preemptive on|off`)
`mlfq` (3 queues, quanta 8/16/FCFS, aging after `--age` minutes, default 60; every promotion is logged on
stderr) and `mlfq-floor` (CRITICAL never demoted below Q1, or `--crit-floor Q`). `--trace F` writes every
scheduling event; `experiments/analyze_mlfq_trace.py F` checks the MLFQ rules and splits waits by queue level. `build/test_sched` prints a textbook Gantt chart for each unit test.

On the flood scenario with 3 teams (`schedsim --policy P --teams 3`):

| Policy | Avg wait (min) | CRITICAL avg wait | Starved (> 120 min) | Context switches |
|--------|---------------:|------------------:|--------------------:|-----------------:|
| fcfs | 401.9 | 382.8 | 166 | 195 |
| sjf | 192.9 | 415.6 | 76 | 195 |
| srtf | 189.6 | 412.2 | 76 | 227 |
| rr | 420.2 | 659.4 | 158 | 428 |
| prio | 514.7 | 32.5 | 161 | 195 |
| prio `--preemptive on` | 520.3 | 28.6 | 164 | 195 |
| mlfq | 464.6 | 525.3 | 180 | 502 |

### Critical section: racelab
Two camps (threads) share one boat and both add N to one inventory counter (read, modify, write):
```sh
build/racelab --mode none               # race condition: about 45% of 2,000,000 updates lost
build/racelab --mode peterson-nofence   # textbook Peterson: loses a few dozen on x86 (store buffers)
build/racelab --mode peterson           # with a seq_cst fence: always 2,000,000
build/racelab --mode mutex|spinlock|semaphore [--iters N] [--threads 1] [--csv F]
strace -f -e trace=futex -c build/racelab --mode mutex   # futex calls only under contention
```
Results and the explanation are in E4 in `docs/EXPERIMENTS.md`.

### Loading dock and inventory (producer-consumer, readers-writers)
Kit requests (FOOD_KIT, MED_KIT, HYGIENE_KIT) become orders. Warehouse producer threads take the kits from
the inventory, pack them and put each package on a bounded loading dock (a circular buffer guarded by the
`empty`, `full` and `mutex` semaphores). Truck threads take packages off and deliver them. The inventory is a
readers-writers structure behind one interface: the textbook semaphore solution (readers preference) or a
writer-preference `pthread_rwlock`.
```sh
build/reliefd --dock 8 --producers 2 --trucks 4 --rw rwlock ...   # the defaults
scripts/reliefctl status        # dock occupancy, produced/consumed/delivered, inventory per warehouse
scripts/reliefctl report        # average dock occupancy, time producers were blocked on a full dock
build/rwlab --rw semaphore      # 8 readers + 1 writer: the writer starves (0 writes in 2 s)
build/rwlab --rw rwlock         # writer preference: ~900 writes, max writer wait ~0.2 ms
```
`RELIEF_DOCK_ARGS="--dock 4 --rw semaphore" scripts/reliefctl start` passes dock options through reliefctl.

### Deadlock avoidance: the Banker's algorithm
TRUCK (3), BOAT (2), MED_KIT (40 crates) and FOOD_KIT (60 crates) requests are decided by the Banker's
algorithm (`reliefd --deadlock avoid`, the default; totals in `scenarios/resources.conf`). Each camp declares
its maximum claim in HELLO, waits for reliefd's GRANT on its own reply queue, holds the resource for the
request's service time and hands it back with RELEASE. A request that would leave the system in an unsafe
state waits until something is released.
```sh
scripts/reliefctl banker        # Allocation, Max, Need, Available per camp + a safe sequence
scripts/reliefctl report        # grants, unsafe/unavailable waits, average wait per resource
build/test_banker               # textbook example: safe sequence <P1, P3, P4, P0, P2>
```

### Live dashboard: reliefui
`scripts/reliefctl ui` (or `make ui`) opens an htop-like view that refreshes every 500 ms:
- **top bar:** daemon pid, uptime, policy, deadlock mode, requests and requests/s;
- **camps:** pid, process state from `/proc/<pid>/stat`, requests sent, restarts, heartbeat (green < 2 s, yellow < 5 s, red);
- **scheduler:** ready-queue bars per level, and what each rescue team is doing;
- **resources:** the Banker's Available/Total bars, safe or unsafe, and the dock's 8 slots;
- **events:** the last 15 events, coloured by type.

Keys: `q` quit, `p` pause, `a` alert a camp, `d` dump, `k` kill -9 a camp (watch reliefd restart it), `s` start/stop, `?` help.
It copies the shared-memory table with the lock-free seqlock and never takes reliefd's mutex.
Without a terminal, `reliefui --batch N` draws N frames to stdout (tests).

### Deadlock: none, prevent, detect
`scenarios/deadlock.csv` is a reproducible circular wait: camps 1 and 2 each hold a truck and a boat and ask
for the other's. Rows sharing the optional 7th CSV column (`task`) are acquired one after another and held
together (hold and wait).
```sh
R="--camps two.conf --scenario scenarios/deadlock.csv --resources scenarios/deadlock_resources.conf"
build/reliefd $R --deadlock none        # hangs: a real deadlock
scripts/reliefctl waitfor               # camp1 -> camp2 [TRUCK], camp2 -> camp1 [BOAT], the cycle
scripts/reliefctl waitfor --dot > waitfor.dot && dot -Tpng waitfor.dot -o waitfor.png   # needs graphviz
build/reliefd $R --deadlock prevent     # resource ordering: out-of-order requests rejected, retried in order
build/reliefd $R --deadlock prevent --all-or-nothing   # each task in one request
build/reliefd $R --deadlock detect --detect-interval 0.2   # cycle found by DFS, a truck preempted
```
`build/dlsim --deadlock MODE --scenario F` replays the same rules deterministically (experiment E3 in
`docs/EXPERIMENTS.md`: prevention wastes about a fifth of vehicle time, avoidance needs claims and delays,
detection keeps re-deadlocking). Which Coffman condition each mode breaks: `docs/ARCHITECTURE.md`. A random mix: `scenarios/deadlock_mix.csv`
(`python3 experiments/gen_deadlock.py`).

### Scenarios
`scenarios/flood_pune.csv` holds 200 requests across the 8 camps: 15% CRITICAL, 35% URGENT and 50% ROUTINE,
with arrivals over 6 simulated hours and a peak in the middle. It is generated, and the same seed gives a
byte-identical file:
```sh
python3 experiments/gen_scenario.py -o scenarios/flood_pune.csv                       # the committed scenario
python3 experiments/gen_scenario.py --rows 2000 --seed 7 -o scenarios/big.csv         # a bigger one
RELIEF_SCENARIO=$PWD/scenarios/big.csv scripts/reliefctl start
```

Runtime files go in `$RELIEF_HOME`, which defaults to `./run`: the pid file, `logs/reliefd.log` (key=value)
and `logs/reliefd.out` (raw stderr). You never need root.

### reliefctl
| Command    | Status |
|------------|--------|
| `start`    | Starts reliefd and its camps in the background |
| `stop`     | Stops reliefd with SIGTERM. reliefd stops and reaps every camp |
| `status [--json]` | Shows whether reliefd is running, then the live camp table from shared memory (`reliefstat`): pid, state, requests sent, heartbeat, restarts |
| `sched [--json]` | Live scheduler: policy, ready queue per level, and what each rescue team is working on |
| `ui` | Live ncurses dashboard (`build/reliefui`); keys `q p a d k s ?`; works before `start` too |
| `waitfor [--dot]` | Wait-for graph: `campX -> campY [RESOURCE]` edges and any cycle; `--dot` prints Graphviz DOT |
| `banker [--json]` | Banker's algorithm in textbook layout: Allocation, Max, Need and Available per camp, and a safe sequence of the current state |
| `alert <id\|all>` | Evacuation alert (SIGUSR1). The camp's next 5 requests are sent as CRITICAL |
| `dump <id\|all>`  | Status dump (SIGUSR2). Prints each camp's counters: sent, by severity, escalations, uptime |
| `ps`       | PID, PPID, state (R/S/D/T/Z), threads, VmRSS, voluntary/nonvoluntary context switches and CPU time, read from `/proc/<pid>/status` and `/proc/<pid>/stat` (the kernel's task_struct) |
| `tree`     | Process tree rooted at reliefd (`ps --forest` and `pstree`) |
| `report [--since MIN] [--last-run]` | awk over `logs/reliefd.log`: requests per camp, severity and resource, the busiest camp, and the scheduler's per-severity wait/turnaround/response |
| `simulate flood [--camps N] [--seed S] [--rows R] [--time-scale MS] [--policy P] [--teams T]` | Generate a scenario, run it until every camp sends BYE and every RESCUE request is served, stop, print the report |

Exit codes: 0 success, 1 runtime error (e.g. reliefd not running), 2 usage error. Long options such as
`--camps=8` are translated to short ones and parsed with `getopts`.

### Cron: stockcheck
`scripts/stockcheck.sh [-t THRESHOLD] [-w WINDOW_MIN] [-q]` counts requests logged in the last window and
warns (stderr, `logs/stockcheck.log`, exit 1) above the threshold. It is not installed automatically. To
run it every 10 minutes, add this with `crontab -e`:
```
*/10 * * * * RELIEF_HOME=$HOME/os-course-project-sem5/run $HOME/os-course-project-sem5/scripts/stockcheck.sh -t 50 -w 10 -q
```

## Syllabus mapping
| Unit | Topics | ReliefOS feature | Phase |
|------|--------|------------------|-------|
| 1 | Shell programming, modular kernels | `reliefctl` (functions, getopts, awk report), `stockcheck.sh` cron job; `relief.ko` | 1, 6 |
| 2 | fork/exec/waitpid, process states/PCB, pthreads | reliefd supervises camp processes (SIGCHLD reaping, restarts); `reliefctl ps`/`tree` read the PCB via `/proc` | 1 |
| 2 | IPC (POSIX message queues) | Camps send paced requests over a bounded queue; mq_timedsend back-pressure | 1 |
| 2, 3 | IPC via POSIX shared memory, process-shared mutex | Live status table `/reliefos_status` read by `reliefstat` (robust mutex + seqlock) | 1 |
| 2 | Signals as IPC, process control | `reliefctl alert` (SIGUSR1), `dump` (SIGUSR2), graceful SIGTERM; flag-only handlers | 1 |
| 2 | FCFS, SJF, RR, Priority, MLFQ with aging | Triage scheduler (`src/sched/`, pluggable policies, `schedsim`), experiment E1 | 2 |
| 2 | Traditional/modern UNIX scheduling: nice, SCHED_FIFO/SCHED_RR, CFS/EEVDF | `build/linuxsched`: real threads on one pinned CPU, experiment E2 | 2 |
| 2, 3 | Threads (pthreads), mutex + condition variable | reliefd's rescue-team threads and clock thread share one ready queue (`src/reliefd/dispatch.c`) | 2 |
| 3 | Critical section, Peterson's solution, mutex, TAS spinlock, semaphores, futex | `build/racelab`: two camps / one boat / one counter, experiment E4, strace of futex | 3 |
| 3 | Producer-consumer (bounded buffer, empty/full/mutex semaphores) | reliefd loading dock: warehouse producer threads → dock → truck threads (`src/reliefd/dock.c`) | 3 |
| 3 | Deadlock avoidance: Banker's algorithm (safety + resource-request) | Trucks, boats and kit crates granted only in safe states; `reliefctl banker` (`src/deadlock/`, `src/reliefd/resmgr.c`) | 3 |
| 3 | Readers-writers (readers preference vs writer preference) | Inventory behind `rwsync` (semaphore or pthread rwlock); `build/rwlab`, experiment E4b | 3 |
| 3 | Deadlock characterization (Coffman conditions), prevention (resource ordering, all-or-nothing), detection (wait-for graph + DFS) and recovery (preemption) | `reliefd --deadlock none/prevent/detect`, `scenarios/deadlock.csv`, `reliefctl waitfor` | 3 |
| 4 | First/best/worst fit, buddy system, slab allocator | Warehouse space, pre-packed relief kits | 4 |
| 4 | FIFO/LRU/Optimal replacement, thrashing | Perishable stock (plus expiry-first) | 4 |
| 5 | FCFS/SSTF/SCAN/C-SCAN/LIFO disk scheduling | Delivery routing along one highway | 5 |
| 5 | Journaling, directory tree, permissions | Write-ahead journal with fsync + crash recovery, per-role access | 5 |
| 6 | User vs kernel space, system calls, boot services | `/proc/relief`, strace of a request, systemd unit, Termux client | 6 |

Roles (`relief-admin`, `relief-officer`, `relief-volunteer`) are enforced as Linux groups.

## Repository layout
See [CLAUDE.md](CLAUDE.md) for the layout and conventions. Design notes are in [docs/](docs/).

## Team
| Name | Roll no. | Responsibilities |
|------|----------|------------------|
| _TBD_ | | |
