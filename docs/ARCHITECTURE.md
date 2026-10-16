# ReliefOS Architecture

> Placeholder. To be filled in as phases land.

## Components
- **reliefd**: coordinator daemon (triage scheduler, deadlock control, sync layer,
  warehouse allocators, delivery router, write-ahead journal)
- **camp**: one process per relief camp; client of the request queue
- **reliefctl**: Bash control CLI
- **relief.ko**: kernel module exposing `/proc/relief` (Phase 6)

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

## Structured log (`$RELIEF_HOME/logs/reliefd.log`)
One event per line: `ts=<iso8601> level=<lvl> comp=reliefd pid=<pid> event=<name> key=value ...`.
Values contain no spaces, except `msg="..."` on free-form `event=log` lines. Events include
`daemon_start`, `camp_spawn`, `camp_running`, `camp_exit`, `camp_signaled`, `camp_restart`,
`camp_crashed`, `camp_killed`, `camp_final`, `hello`, `request`, `bye`, `shutdown` and `daemon_stop`.

## TODO
- [ ] Component diagram
- [ ] Request lifecycle (camp → scheduler → allocator → router → journal)
- [ ] Thread model inside reliefd
- [ ] Policy plug-in interfaces (function-pointer tables)
