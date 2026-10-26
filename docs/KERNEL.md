# User space and kernel space in ReliefOS (Unit 6)

All of ReliefOS runs in **user space**: reliefd, the camps, reliefctl, reliefui and the
simulators. Each one is an ordinary process with its own virtual address space and no direct
access to hardware or to other processes. Whatever crosses a process boundary, touches a file
or waits for time goes through the **kernel**, by one of three paths.

```
 user space     reliefd (main+11 thr.) camp x8     reliefctl / reliefui / reliefstat
 ───────────────────│──────────────────────│──────────────────│───────────────────────
                syscalls ↓↑           syscalls ↓↑      /proc reads ↓↑     signals ↑
 kernel space   scheduler (CFS/EEVDF) · futex wait queues · POSIX mqueue fs · tmpfs (/dev/shm)
                ext4 + page cache (journal, fsync) · process table (PCB = task_struct) · procfs
```

## 1. System calls: the requests user space makes
`docs/SYSCALLS.md` traces one request.
- **Processes:** `clone(…SIGCHLD)` is a fork and `execve` loads the camp. `clone3(CLONE_THREAD)` is a
  thread, and `wait4` reaps a child.
- **IPC:** `mq_open` / `mq_timedsend` / `mq_timedreceive` for the request queue. `openat` +
  `mmap(MAP_SHARED)` for the status table.
- **Synchronisation:** `futex`. An uncontended `pthread_mutex_lock` never enters the kernel; only a
  thread that must sleep does (E4 counted this).
- **Durability:** `write` + `fsync` on the journal. `fsync` is the moment the kernel promises the
  data is on disk.

Each call is a controlled entry into kernel mode: a `syscall` instruction, a number and its
arguments. The kernel checks every argument; a bad pointer returns `EFAULT` instead of crashing
the kernel.

## 2. `/proc`: the kernel's process table, read as files
`reliefctl ps` reads `/proc/<pid>/status` and `/proc/<pid>/stat` for reliefd and every camp. It
shows State, PPid, Threads, VmRSS, voluntary and involuntary context switches, and utime/stime.
reliefui reads the state letter (R/S/D/T/Z) from `/proc/<pid>/stat`. These files are not stored
anywhere: procfs generates their text from the kernel's `task_struct` (the PCB) at the moment
of the `read()`.

## 3. Signals: the kernel interrupts a process
- `reliefctl alert|dump` calls `kill(pid, SIGUSR1|SIGUSR2)`. The kernel marks the signal pending
  and runs the camp's handler the next time that process returns to user mode.
- A camp's death reaches reliefd as **SIGCHLD**, which the kernel generates when the child's
  `exit_group` completes (SYSCALLS.md line 13).
- If reliefd is killed with kill -9, each camp gets a SIGTERM from the kernel itself
  (`prctl(PR_SET_PDEATHSIG)`, `camps.c`).
- The kernel does not deliver SIGKILL to the process at all: it ends it, which is why
  reliefd needs the write-ahead journal (E9).

## Design only: `relief.ko` exposing `/proc/relief`
**Not built.** WSL2 runs Microsoft's own kernel (`uname -r` = `6.18.33.2-microsoft-standard-WSL2`)
and ships no matching headers: `/lib/modules/$(uname -r)/build` does not exist. Ubuntu's
`linux-headers-*` packages match Ubuntu's kernels, not this one, and a module must be compiled
against the exact kernel it loads into. Building it would mean compiling the WSL kernel from
source or using a VM. The project keeps everything in user space instead.

How it would work: reliefd already publishes its status in shared memory. A module could not
read another process's memory safely, so reliefd would push a summary with
`write("/proc/relief", …)`. `cat /proc/relief` would then print it, via the seq_file interface:

```c
/* OS concept: Loadable kernel module, procfs — Unit 1/6. DESIGN SKETCH, NOT BUILT. */
#include <linux/module.h>
#include <linux/proc_fs.h>
#include <linux/seq_file.h>
#include <linux/uaccess.h>
static char summary[256] = "no data\n";   /* last line written by reliefd */
static DEFINE_MUTEX(lock);
static int relief_show(struct seq_file *m, void *v)
{ mutex_lock(&lock); seq_puts(m, summary); mutex_unlock(&lock); return 0; }
static int relief_open(struct inode *i, struct file *f) { return single_open(f, relief_show, NULL); }
static ssize_t relief_write(struct file *f, const char __user *buf, size_t n, loff_t *off)
{ char tmp[sizeof summary];
  if (n >= sizeof tmp || copy_from_user(tmp, buf, n)) return -EFAULT;   /* never trust a user pointer */
  tmp[n] = '\0'; mutex_lock(&lock); strscpy(summary, tmp, sizeof summary); mutex_unlock(&lock); return n; }
static const struct proc_ops relief_ops = { .proc_open = relief_open, .proc_read = seq_read,
    .proc_lseek = seq_lseek, .proc_release = single_release, .proc_write = relief_write };
static int __init relief_init(void) { return proc_create("relief", 0664, NULL, &relief_ops) ? 0 : -ENOMEM; }
static void __exit relief_exit(void) { remove_proc_entry("relief", NULL); }
module_init(relief_init); module_exit(relief_exit); MODULE_LICENSE("GPL");
```

Lifecycle: `make -C /lib/modules/$(uname -r)/build M=$PWD modules`, then `sudo insmod relief.ko`
(`relief_init` runs and `/proc/relief` appears; `dmesg` shows its messages), then
`sudo rmmod relief`. This is the modular kernel of Unit 1: code added to a running kernel without
a reboot. It runs in kernel mode, so a bug there crashes the whole machine, not one process.
That is the main reason the design keeps the module this small.

## Design only: an Android client
**Not built.** A phone app for field volunteers would sit at the top of the Android stack and
reach reliefd over the network, never through the IPC above. POSIX message queues and shared
memory work only between processes on the same machine.

```
 ┌────────────────────────────────────────────────────────────┐
 │ App: ReliefOS Volunteer (Kotlin) — send a request, see camp │  ← our code
 ├────────────────────────────────────────────────────────────┤
 │ Java API framework: Activity, ViewModel, WorkManager,       │
 │ ConnectivityManager, NotificationManager                    │
 ├──────────────────────────────┬─────────────────────────────┤
 │ Native C/C++ libraries       │ Android Runtime (ART):      │
 │ (libc = Bionic, SQLite, SSL) │ app's dex code, JIT/AOT, GC │
 ├──────────────────────────────┴─────────────────────────────┤
 │ HAL: Wi-Fi/modem, GPS, camera — vendor drivers behind a     │
 │ standard interface                                          │
 ├────────────────────────────────────────────────────────────┤
 │ Linux kernel: process scheduler, memory, binder IPC,        │
 │ network stack, drivers, power management                    │
 └────────────────────────────────────────────────────────────┘
          │ HTTPS / JSON over Wi-Fi or mobile data
          ▼
   web/server.py-style gateway on the coordinator → POSIX mq → reliefd
```

Each app is a Linux process with its own UID (the sandbox). Inside the phone it talks through
**binder**, Android's kernel IPC, instead of mqueues. The kernel is the same Linux kernel that
runs ReliefOS, with Android-specific additions (binder, low-memory killer). The client would
only be a thin UI. A gateway on the coordinator would accept a few allow-listed actions, as
`web/server.py` does today, and turn them into `relief_msg_t` messages for reliefd.
