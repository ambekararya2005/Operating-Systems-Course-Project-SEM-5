# ReliefOS: Linux-Based Smart Disaster Relief Resource Management

ReliefOS is an Operating Systems course project (Semester 5) that runs a disaster-relief network
as if it were an operating system, in C11 on Linux. A coordinator daemon (`reliefd`) fork/execs
one process per relief camp. The camps send rescue and supply requests over a POSIX message queue.
Inside reliefd, rescue requests are scheduled onto team threads (FCFS … MLFQ). Supplies move
through a producer-consumer loading dock, vehicles and kit crates are granted with the Banker's
algorithm, and every allocation is journaled with fsync so a crash loses nothing. Standalone
simulators cover memory allocation, page replacement and disk scheduling, and reliefd also runs as
a systemd service. Every feature demonstrates a named syllabus topic (Units 1–6). Nine experiments
(E1–E9) measure the trade-offs with reproducible CSVs and plots.

| Relief world    | OS concept       |
|-----------------|------------------|
| Relief camps    | Processes        |
| Supplies        | Memory           |
| Trucks / boats  | Shared devices   |
| Rescue requests | Jobs to schedule |
| Highway stops   | Disk requests    |

## Quick start
Ubuntu 24.04 (WSL2 works). Nothing needs root.
```sh
sudo apt install build-essential valgrind shellcheck libncurses-dev strace python3-matplotlib
make                                         # all binaries into build/ (-Wall -Wextra -Werror)
scripts/reliefctl simulate flood --camps 8 --seed 42   # run a generated flood scenario to the end, then a report
scripts/reliefctl start; scripts/reliefctl ui           # the live system and its ncurses dashboard (q quits)
scripts/reliefctl stop
scripts/demo.sh                              # guided tour, one syllabus topic per step (--auto: no pauses, -u: dashboard)
make test                                    # all tests (make tsan / make valgrind: the same under the sanitizers)
```

## Syllabus coverage
| Unit | Topic | File / command | Experiment |
|------|-------|----------------|------------|
| 1 | Shell programming (functions, getopts, awk), cron | `scripts/reliefctl`, `scripts/stockcheck.sh`, `scripts/demo.sh` | – |
| 1 | Modular kernels | `docs/KERNEL.md` (`/proc/relief` module, design only) | – |
| 2 | fork/exec/waitpid, process states, PCB via `/proc`, signals | `src/reliefd/camps.c`, `reliefctl ps / tree / alert / dump` | – |
| 2 | IPC: POSIX message queue, shared memory | `include/relief.h`, `src/common/shm.c`, `build/reliefstat` | – |
| 2 | Threads (pthreads), mutex + condition variable | `src/reliefd/dispatch.c` (rescue teams + clock thread) | – |
| 2 | FCFS, SJF, SRTF, RR, Priority, MLFQ with aging | `src/sched/`, `build/schedsim`, `reliefd --policy` | E1, E1b |
| 2 | Linux scheduling: nice, SCHED_FIFO, SCHED_RR, CFS/EEVDF | `build/linuxsched` | E2 |
| 3 | Critical section, Peterson, TAS, mutex, semaphore, futex | `build/racelab` | E4 |
| 3 | Producer-consumer (bounded buffer, 3 semaphores) | `src/reliefd/dock.c` | – |
| 3 | Readers-writers | `src/reliefd/inventory.c`, `build/rwlab` | E4b |
| 3 | Deadlock: Banker's avoidance, prevention, wait-for-graph detection + recovery | `src/deadlock/`, `src/reliefd/resmgr.c`, `reliefd --deadlock`, `reliefctl banker / waitfor`, `build/dlsim` | E3 |
| 4 | First/best/worst fit, buddy system, slab allocator, fragmentation | `src/mem/`, `build/memsim` | E5 |
| 4 | Page replacement: FIFO, LRU, Optimal (+ expiry-first), Belady's anomaly | `src/mem/paging.c`, `build/pagesim` | E6 |
| 5 | Disk scheduling: FCFS, SSTF, SCAN, C-SCAN, LIFO | `src/io/disk.c`, `build/routesim` | E8 |
| 5 | File-system implementation: write-ahead journal, fsync, atomic rename, crash recovery | `src/io/journal.c`, `reliefd --no-journal`, `tests/crash_test.sh` | E9 |
| 6 | Boot process, systemd service | `systemd/reliefd.service`, `scripts/install_service.sh` | – |
| 6 | System call interface, user vs kernel space | `scripts/trace_request.sh`, `docs/SYSCALLS.md`, `docs/KERNEL.md` | – |

## Experiments
Details, tables and figures are in [docs/EXPERIMENTS.md](docs/EXPERIMENTS.md). Run them with `make e1` … `make e9`.

| | Question | Key finding |
|---|---|---|
| E1 | Which triage policy serves CRITICAL fastest without starving the rest? | Priority protects CRITICAL (9–14 min wait). MLFQ with aging does not: aging floods Q0 during the rush (25.6 min, the most starvation). |
| E1b | When does MLFQ stop reaching zero starvation? | Only up to 30% offered load. From 45% it starves, and above 60% worse than preemptive priority. |
| E2 | How does real Linux (EEVDF + nice, SCHED_FIFO, SCHED_RR) treat the same workload? | FIFO is strict priority (best for CRITICAL, starves the rest). The fair class with nice is proportional share: nobody starves, but CRITICAL still waits about two hours. |
| E3 | What does each way of handling deadlock cost? | Doing nothing freezes the fleet. Prevention gives the best throughput (it idles 19–22% of capacity). Avoidance delays requests. Detection wastes work on 572 preemptions. |
| E4 | Which entry/exit sections keep a shared counter correct? | No lock loses about 45% of updates. Textbook Peterson fails on x86 without a fence. The mutex, the spinlock and the semaphore are exact, and the mutex enters the kernel only when contended. |
| E4b | Does readers-preference starve writers? | The semaphore version starves the writer completely (0 writes). The writer-preference rwlock fixes it. |
| E5 | How much space does each allocator waste? | The fits shatter free space (external fragmentation). Buddy wastes 22% inside blocks. Slab fails least (3.8%). |
| E6 | How do FIFO, LRU, Optimal and expiry-first trade stock-outs against expired stock? | Optimal has the fewest faults but lets the most stock expire. Expiry-first has almost no waste while shelves are scarce. |
| E8 | How far does the truck drive under each disk-scheduling policy? | SSTF 245 km vs FCFS 1,497 on 20 stops. SSTF can still strand far stops (detour 324 vs SCAN's 70). |
| E9 | What does the journal buy and cost? | 10 kill -9 crashes lose 0 units with the journal and all 232 without it, for about 2.3 ms of fsync per grant. |

## Not implemented / future work
- **Kernel module `relief.ko` (`/proc/relief`):** design and a code sketch only (`docs/KERNEL.md`).
  WSL2's kernel ships no matching headers, so building it needs a VM or a self-built kernel.
- **Android client:** design only (`docs/KERNEL.md`: where it would sit on the Android stack, and the HTTPS gateway).
- **Thrashing demo (Unit 4):** `pagesim` shows faults vs frames but has no multi-process load where
  CPU utilisation collapses as the degree of multiprogramming grows.
- **Paging hardware and segmentation:** page tables, the TLB, address translation and segments are not
  simulated. `pagesim` models only the replacement policy.
- **Directory tree with permissions per role (Unit 5):** the `relief-admin` / `relief-officer` /
  `relief-volunteer` groups are not enforced. Only the systemd service runs with group `relief-admin`.

## Usage reference
### All make targets
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
                     # tests/deadlock.sh (deadlock.csv under none/prevent/avoid/detect, waitfor),
                     # tests/mem.sh, tests/page.sh, tests/route.sh (simulators vs textbook values),
                     # tests/crash_test.sh (kill -9 x 10: the journal loses nothing), and more
make ui              # live ncurses dashboard (build/reliefui); s starts ReliefOS from inside it
make tsan            # ThreadSanitizer build (build/tsan/) + tests
make valgrind        # tests under valgrind (camps too)
make e1              # experiment E1: 9 variants x 5 seeds + E1b load sweep -> experiments/results/e1*.csv + graphs
make e2              # experiment E2: real threads under CFS/EEVDF+nice, SCHED_FIFO, SCHED_RR vs simulated
                     #   (fifo/rr first: sudo setcap cap_sys_nice+ep build/linuxsched)
make e3              # experiment E3: deadlock modes compared with build/dlsim (deterministic) -> graphs
make e4              # experiment E4: every racelab mode x 10 runs -> experiments/results/e4.csv + graphs,
                     #   plus strace futex counts in docs/strace_futex.txt (sudo apt install strace)
make e5              # experiment E5: first/best/worst fit, buddy, slab on one shipment trace -> graph
make e6              # experiment E6: page faults vs frames, FIFO/LRU/Optimal/expiry-first -> graph
make e8              # experiment E8: delivery routing FCFS/SSTF/SCAN/C-SCAN/LIFO, 20 stops x 5 seeds -> graph
make e9              # experiment E9: journal on/off, kill -9 x 10 (allocations lost) and fsync cost per grant
make e4b             # experiment E4b: readers-writers writer starvation, rwlab semaphore vs rwlock
make clean
```

### Running the system by hand
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

### Memory: warehouse placement (memsim)
`build/memsim` models the warehouse floor (1024 units) as memory, with relief shipments arriving and leaving:
```sh
build/memsim --alloc first|best|worst|buddy|slab [--seed S --ops N | --trace F] [--show N --width W]
build/memsim --alloc buddy --ops 60 --show 20      # ASCII floor map every 20 operations ('~' = waste in a block)
```
It reports external fragmentation, internal fragmentation, failures and, for slab, objects per slab and
full/partial/empty slabs. It reads or writes CSV traces (`alloc,<id>,<FOOD|MED|HYGIENE>,<count>` and
`free,<id>`). Experiment E5 is in `docs/EXPERIMENTS.md`.

### Paging: perishables on a camp's shelves (pagesim)
A camp's F shelf slots are page frames, the items it needs are the reference string, and a stock-out is a page fault:
```sh
build/pagesim --policy lru --frames 3 --refs 7,0,1,2,0,3,0,4,2,3,0,3,2,1,2,0,1,7,0,1 --verbose   # textbook table: 12 faults
build/pagesim --policy fifo|lru|optimal|expiry --frames F (--refs LIST | --trace F | --seed S)
```
Expiry-first evicts the stock closest to its expiry date. Every run reports faults, the hit ratio and the units
that expired on the shelf. Experiment E6 is in `docs/EXPERIMENTS.md`.

### Disk scheduling: delivery routing on one highway (routesim)
A truck on one highway (km 0–199) is a disk head on cylinders 0–199, and its delivery stops are I/O requests:
```sh
build/routesim --policy cscan --start 53 --stops 98,183,37,122,14,124,65,67   # 382 km incl. the 199 km return jump
build/routesim --policy fcfs|sstf|scan|cscan|lifo [--start KM --dir up|down] (--stops LIST | --seed S [--count N])
```
It prints the visiting order, the total km, the worst and average wait and the worst detour, plus an ASCII
track diagram of the truck's path (`--quiet` leaves the diagram out). Experiment E8 is in `docs/EXPERIMENTS.md`.

### Journaling: the Banker's allocations survive a crash
reliefd appends every claim, grant and release to `$RELIEF_HOME/journal.log` (40-byte records with
a CRC32, `write()` + `fsync()`) before it tells the camp. Every 100 records it writes `snapshot.bin`
(temp file, fsync, atomic `rename()`) and truncates the journal. On startup it replays them and logs
`event=recovered records=N ... alloc=camp:units/...`. `reliefd --no-journal` turns this off.
`tests/crash_test.sh` kills reliefd with `kill -9` 10 times and checks that nothing acknowledged is
lost. Experiment E9 is in `docs/EXPERIMENTS.md`.

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

### Web dashboard
```sh
scripts/reliefctl web               # prints: ReliefOS web dashboard: http://127.0.0.1:8080/   (--port N to change)
```
Open that URL in a browser on the same machine. With WSL2, a Windows browser reaches it too, because
localhost is forwarded.
- **Live tab:**
  - camp cards (state, `/proc` state, requests, heartbeat);
  - two hand-drawn canvas charts of the last 2 minutes: requests/s and ready-queue length, hover for values;
  - the Banker's Available/Total bars with safe/unsafe, and the dock's 8 slots;
  - a colour-coded live event feed (Server-Sent Events from `reliefd.log`);
  - buttons: Start, Stop, Evacuation alert, Status dump and Run flood simulation.
- **Experiments tab:** every figure of `docs/EXPERIMENTS.md` with its caption.
- **Light and dark themes** follow the system, with a toggle.
- **Safety:**
  - the server binds to 127.0.0.1 only;
  - actions go through an allow-list to `reliefctl` as an argument list, never a shell;
  - requests with a foreign Host or Origin, or a POST that is not JSON, are refused.

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
| `web [--port N]` | Local web dashboard on http://127.0.0.1:N/ (default 8080), offline, Ctrl-C stops it |
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

### systemd service and strace (Phase 6)
```sh
scripts/install_service.sh -n      # print the sudo commands only; without -n it runs them (you type the password)
systemctl status reliefd           # running as a service, RELIEF_HOME=/var/lib/reliefos
journalctl -u reliefd -n 20        # reliefd's stderr, captured by journald
sudo kill -9 "$(systemctl show -p MainPID --value reliefd)"   # systemd restarts it 2 s later; the journal recovers
scripts/uninstall_service.sh [-p]  # remove it (-p: also /var/lib/reliefos and the relief-admin group)
scripts/trace_request.sh           # 1 camp, 3 requests under strace -> docs/strace_request.txt, docs/SYSCALLS.md
```
The boot sequence (firmware → bootloader → kernel → systemd → this service) is described in
`systemd/reliefd.service`. The user/kernel boundary, and the `/proc/relief` module and Android
client (both design only), are in `docs/KERNEL.md`.

### Cron: stockcheck
`scripts/stockcheck.sh [-t THRESHOLD] [-w WINDOW_MIN] [-q]` counts requests logged in the last window and
warns (stderr, `logs/stockcheck.log`, exit 1) above the threshold. It is not installed automatically. To
run it every 10 minutes, add this with `crontab -e`:
```
*/10 * * * * RELIEF_HOME=$HOME/os-course-project-sem5/run $HOME/os-course-project-sem5/scripts/stockcheck.sh -t 50 -w 10 -q
```




