# ReliefOS Architecture

> Placeholder. To be filled in as phases land.

## Components
- **reliefd**: coordinator daemon (triage scheduler, deadlock control, sync layer,
  warehouse allocators, delivery router, write-ahead journal)
- **camp**: one process per relief camp; client of the request queue
- **reliefctl**: Bash control CLI
- **relief.ko**: kernel module exposing `/proc/relief` (Phase 6)

## Message flow (Phase 0)
```
camp ──HELLO / REQUEST* / BYE──▶  /reliefos_requests (POSIX mq)  ──▶ reliefd
admin ──SHUTDOWN (camp --shutdown) or SIGTERM (reliefctl stop)─────▶ reliefd
```

## TODO
- [ ] Component diagram
- [ ] Request lifecycle (camp → scheduler → allocator → router → journal)
- [ ] Thread model inside reliefd
- [ ] Policy plug-in interfaces (function-pointer tables)
