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

**Status:** Phase 0 skeleton. `reliefd` and `camp` exchange messages over a POSIX message queue,
and `reliefctl` can start, stop and query the daemon. No OS algorithms are implemented yet.

## Components
- `reliefd`: the coordinator daemon (C11, pthreads)
- `camp`: one process per relief camp. It sends requests to reliefd over `/reliefos_requests`
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
make test            # smoke test: IPC, determinism, SHUTDOWN and SIGTERM paths, reliefctl
make tsan            # ThreadSanitizer build (build/tsan/) + smoke test
make valgrind        # smoke test under valgrind
make clean
```

Try it by hand:
```sh
scripts/reliefctl start
scripts/reliefctl status
build/camp --id 1 --count 5 --seed 42      # HELLO, 5 requests, BYE
tail run/reliefd.log
scripts/reliefctl stop
```
Runtime files (the pid file and the log) go in `$RELIEF_HOME`, which defaults to `./run`. You never need root.

### reliefctl
| Command    | Status |
|------------|--------|
| `start`    | Starts reliefd in the background |
| `stop`     | Stops reliefd with SIGTERM |
| `status`   | Shows whether reliefd is running, with its pid and uptime |
| `ps`       | Coming in Phase 1 |
| `simulate` | Coming in Phase 2 |
| `report`   | Coming in Phase 2 |

## Syllabus mapping
| Unit | Topics | ReliefOS feature | Phase |
|------|--------|------------------|-------|
| 1 | Shell programming, modular kernels | `reliefctl`, awk reports, cron, `relief.ko` | 1, 6 |
| 2 | fork/exec/waitpid, process states/PCB, pthreads | Camp processes, `reliefctl ps` via `/proc` | 1 |
| 2 | FCFS, SJF, RR, Priority, MLFQ with aging; SCHED_FIFO/SCHED_RR/nice | Triage scheduler, experiments E1/E2 | 2 |
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
