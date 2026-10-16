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
No scheduling or resource algorithms are implemented yet.

## Components
- `reliefd`: the coordinator daemon (C11, pthreads)
- `camp`: one process per relief camp, started by reliefd. It sends requests to reliefd over `/reliefos_requests`
- `reliefctl`: Bash CLI
- `relief.ko`: kernel module exposing `/proc/relief` (Phase 6)
- `systemd/reliefd.service`: systemd unit template (Phase 6)

## Prerequisites
Ubuntu 24.04 (WSL2 works):
```sh
sudo apt install build-essential valgrind shellcheck
```

## Build, run, test
```sh
make                 # builds build/reliefd and build/camp
make test            # tests/sched.sh (scheduler unit tests incl. textbook FCFS, schedsim),
                     # tests/smoke.sh (IPC, reliefctl), tests/camps.sh (fork/exec, restarts, no zombies),
                     # tests/traffic.sh (200-row paced replay, queue-full retries),
                     # tests/signals.sh (alert, dump, graceful SIGTERM),
                     # tests/status.sh (shared-memory status table, robust mutex recovery),
                     # tests/cli.sh (reliefctl ps/tree/report/simulate, stockcheck.sh)
make tsan            # ThreadSanitizer build (build/tsan/) + tests
make valgrind        # tests under valgrind (camps too)
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

### Scheduling simulator
`build/schedsim` replays a scenario through a scheduling policy on N rescue teams (= CPUs), in simulated
minutes and with no real sleeping, so a run is instant and fully reproducible:
```sh
build/schedsim --policy fcfs --scenario scenarios/flood_pune.csv --teams 3 --gantt --csv /tmp/fcfs.csv
```
It reports the average waiting, turnaround and response time (overall and per severity), the worst-case
wait, throughput, team utilisation, context switches and starved requests (wait > `--starve`, default
120 min). `--gantt` draws the first 60 minutes per team, and `--csv` writes one row per request.
Policies so far: `fcfs`. SJF, RR, Priority and MLFQ come next in Phase 2.

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
| `alert <id\|all>` | Evacuation alert (SIGUSR1). The camp's next 5 requests are sent as CRITICAL |
| `dump <id\|all>`  | Status dump (SIGUSR2). Prints each camp's counters: sent, by severity, escalations, uptime |
| `ps`       | PID, PPID, state (R/S/D/T/Z), threads, VmRSS, voluntary/nonvoluntary context switches and CPU time, read from `/proc/<pid>/status` and `/proc/<pid>/stat` (the kernel's task_struct) |
| `tree`     | Process tree rooted at reliefd (`ps --forest` and `pstree`) |
| `report [--since MIN] [--last-run]` | awk over `logs/reliefd.log`: requests per camp, severity and resource, plus the busiest camp |
| `simulate flood [--camps N] [--seed S] [--rows R] [--time-scale MS]` | Generate a scenario, run it until every camp sends BYE, stop, print the report |

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
| 2 | FCFS, SJF, RR, Priority, MLFQ with aging; SCHED_FIFO/SCHED_RR/nice | Triage scheduler (`src/sched/`, pluggable policies, `schedsim`), experiments E1/E2 | 2 |
| 3 | Producer-consumer, readers-writers, Peterson's | Warehouse → truck, inventory, two camps / one boat | 3 |
| 3 | Banker's, wait-for-graph detection + recovery, resource ordering | Deadlock control over trucks/boats | 3 |
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
