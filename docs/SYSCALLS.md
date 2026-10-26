# The system call interface: one request's lifecycle under strace (Unit 6)

`scripts/trace_request.sh` runs reliefd with one camp (Hadapsar) and three requests: a CRITICAL
RESCUE, a TRUCK and 20 MED kits (2 crates). It runs everything under
`strace -f -tt -e trace=process,ipc,signal,desc,memory,futex` and saves the trace to
[`strace_request.txt`](strace_request.txt) (644 lines). `futex` is in none of the named classes,
so it is listed on its own. After the camp has finished, reliefd is stopped with SIGTERM.

Reading a line: the first column is the thread id. 86593 is reliefd's main thread, 86607 its
Banker's allocator thread, and 86608 the camp process. Next comes the time (`-tt`), then the call
with its arguments and `= return value`. `<unfinished ...>` and `<... resumed>` mark a call that
another thread interrupted in the output. Long lines are shortened with `…`.

## Key lines, in order

| # | Line (`strace_request.txt`) | What made it |
|--:|---|---|
| 1 | `86593 .139187 execve("<repo>/build/reliefd", […"--camps"…], …) = 0` (L1) | The shell starts reliefd: the kernel replaces strace's child with the reliefd image. |
| 2 | `86593 .149105 mq_open("reliefos_requests", O_RDONLY\|O_CREAT\|O_EXCL, 0660, {mq_maxmsg=10, mq_msgsize=128…}) = 4` (L49) | `main.c` creates the POSIX request queue that every camp writes to. `O_EXCL` means only one reliefd. |
| 3 | `86593 .149351 openat(AT_FDCWD, "/dev/shm/reliefos_status", O_RDWR\|O_CREAT\|O_EXCL…) = 5` then `mmap(NULL, 9136, PROT_READ\|PROT_WRITE, MAP_SHARED, 5, 0)` (L50, L52) | `shm.c`: `shm_open` is glibc's wrapper for `openat` on `/dev/shm`. `mmap` with `MAP_SHARED` maps the status table that reliefstat, reliefui and the camps read. |
| 4 | `86593 .152524 clone3({flags=CLONE_VM\|CLONE_FS\|CLONE_FILES\|CLONE_SIGHAND\|CLONE_THREAD…}) = 86596` (L69) | `pthread_create` (a rescue team in `dispatch.c`). `CLONE_VM` + `CLONE_THREAD` give a thread that shares the address space. reliefd makes 11 of them. |
| 5 | `86596 .153080 futex(0x634bdec7f6f0, FUTEX_WAIT_BITSET_PRIVATE…, 0, NULL…) <unfinished …>` (L76) | That team blocks in `pthread_cond_wait`: there is no rescue yet, so the kernel puts it to sleep (no busy waiting). |
| 6 | `86593 .168722 ftruncate(5, 0) = 0` (L176) | `journal.c` recovery: the startup checkpoint (snapshot written, fsync'ed, renamed) empties `journal.log`. |
| 7 | `86593 .173771 clone(child_stack=NULL, flags=CLONE_CHILD_CLEARTID\|CLONE_CHILD_SETTID\|SIGCHLD…) = 86608` (L194) | `camps.c` `fork()`: a new process (no `CLONE_VM`) that sends `SIGCHLD` to its parent when it ends. |
| 8 | `86608 .174433 execve("<repo>/build/camp", […"--id", "1"…], …)` (L196) | The child execs the camp program: the PCB stays, the program image is replaced. |
| 9 | `86593 .189197 mq_timedreceive(4, "\2\0\0\0\1\0\0\0…", 128, NULL, {…}) = 128` (L293) | The main loop receives a 128-byte `relief_msg_t`. Kind 2 = `MSG_REQUEST` from camp 1. |
| 10 | `86607 .205144 write(5, "\2\0\0\0…", 40) = 40` then `86607 .205324 fsync(5) = 0` (L314–315) | The write-ahead journal: a 40-byte GRANT record (type 2) for the truck is made durable… |
| 11 | `86607 .208451 mq_timedsend(6, "\6\0\0\0\1\0\0\0…", 128, 0, {…})` (L320) | …and only then is the camp told: kind 6 = `MSG_GRANT` on its reply queue `/reliefos_camp_1`, 3.3 ms after the write. |
| 12 | `86593 .348450 kill(86608, SIGTERM) = 0` (L416) | Shutdown: `camps_terminate_all` asks the camp to exit (it sends BYE first). |
| 13 | `86593 .413086 --- SIGCHLD {si_code=CLD_EXITED, si_pid=86608, si_status=0} ---` (L447) | The kernel delivers SIGCHLD when the camp's `exit_group(0)` (L444) completes. reliefd's handler only sets a flag. |
| 14 | `86593 .414440 <... wait4 resumed>[{WIFEXITED(s) && WEXITSTATUS(s) == 0}], WNOHANG, NULL) = 86608` (L460) | `camps_reap`: `waitpid` reaps the zombie and frees its process-table entry. The next `wait4` returns `ECHILD`, so no zombies are left. |
| 15 | `86593 .456683 exit_group(0) = ?` (L643) | reliefd returns from `main` after the final checkpoint (`ftruncate` again at L543), `mq_unlink` and `shm_unlink`. Every thread ends with it. |

## `strace -c`: the top 10 syscalls by count

A second, identical run under `strace -f -c` counts every syscall of reliefd and the camp
([`strace_count.txt`](strace_count.txt): 649 calls in total).

| Syscall | Calls | Errors | Share of syscall time | Why so many |
|---|--:|--:|--:|---|
| getpid | 94 | | 0.7% | the logger puts `pid=` on every line |
| write | 94 | | 0.9% | one `write(2)` per log line (no interleaving), plus the journal records |
| futex | 74 | 3 | 71.1% | mutexes and condition variables: threads sleeping until there is work |
| rt_sigprocmask | 51 | | 0.4% | signals blocked while threads are created and around `fork` |
| mmap | 32 | | 0.2% | thread stacks, `malloc` arenas, the shared status table, loading libraries |
| close | 25 | | 0.4% | files closed after startup and after each checkpoint |
| openat | 24 | 3 | 0.2% | libraries, config and scenario files, `/dev/shm`, the journal (3 × `ENOENT`: no pid file, snapshot or journal yet, L48, L164–168) |
| clock_nanosleep | 23 | | 16.4% | the camp pacing its scenario (`--time-scale`) and the clock thread's ticks |
| mprotect | 20 | | 0.1% | guard pages of the thread stacks, the dynamic loader |
| mq_timedreceive | 18 | 9 | 0.4% | the camp polling its reply queue and reliefd's 500 ms receive loop; the `ETIMEDOUT` "errors" are idle ticks |

`futex` takes most of the time because a blocked `FUTEX_WAIT` counts until the thread is woken.
That time is spent waiting, not burning CPU.
