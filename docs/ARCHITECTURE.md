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
