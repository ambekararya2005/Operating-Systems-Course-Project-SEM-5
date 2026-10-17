# ReliefOS Experiments

Every experiment is deterministic for a given `--seed` and writes CSV to `experiments/results/`
(git-ignored). Figures used here are copied to `docs/figures/` so they are committed.

| ID | Topic (Unit) | Question | Status |
|----|--------------|----------|--------|
| E1 | CPU scheduling (Unit 2) | Which triage policy serves CRITICAL requests fastest without starving the rest? | done |
| E2 | Traditional and modern UNIX scheduling (Unit 2) | How do Linux's CFS/EEVDF + nice, SCHED_FIFO and SCHED_RR treat the same rescue workload, compared with textbook priority and our MLFQ? | CFS modes done; FIFO/RR need `CAP_SYS_NICE` |

## E1: triage scheduling comparison

**Hypothesis (PRD):** *MLFQ with aging matches Priority on critical response time while bringing starved
requests to zero.*

### Setup
- **Workload:** 500 requests from `experiments/gen_scenario.py --rows 500 --seed S --duration 3000 --rush 60-120`.
  - The mix is 15% CRITICAL, 35% URGENT and 50% ROUTINE, across 8 camps.
  - The **rush hour** makes arrivals 10× denser in minutes 60–120. About 17% of requests (86 for seed 1)
    land there, roughly 1,400 team-minutes of work in one hour against 180 team-minutes of capacity.
  - Over the full 3,000 minutes the work is about 8,000 team-minutes, about **90% average load** on
    3 teams. So the system copes on average, but the rush builds a backlog that takes many hours to drain.
  - *Why 3,000 minutes:* 500 requests in 360 minutes would overload 3 teams about 7× for the whole run.
    Nearly every request would then starve under every policy, and the comparison would say little. To
    rerun with a different length: `experiments/run_e1.sh -d 360`.
- **Seeds 1–5.** The seed drives both the generated scenario and `schedsim`'s same-minute tie order, so
  every seed is a different but reproducible workload. Each policy runs on the same 5 scenarios.
- **Policies:**

  | Variant | Policy |
  |---------|--------|
  | `fcfs`, `sjf`, `srtf` | As named |
  | `rr-q5`, `rr-q15` | Round robin with a 5 or 15 min quantum |
  | `prio` | Severity priority, non-preemptive |
  | `prio-preempt` | Severity priority, preemptive |
  | `mlfq` | Q0 RR 8, Q1 RR 16, Q2 FCFS; entry level by severity; aging after 60 min |

  All run on 3 rescue teams.
- **Metrics** (simulated minutes; definitions in `src/sched/sim.h`):

  | Metric | Definition |
  |--------|------------|
  | Waiting | turnaround − service time |
  | Response | arrival → *first* service |
  | Starved | waited more than 120 min |
  | Worst-case wait | the single longest wait |
- **Reproduce:** `make e1`. This runs `experiments/run_e1.sh` (40 `schedsim` runs, under a second) and then
  `experiments/plot_e1.py` (needs `python3-matplotlib`). Two runs give byte-identical `e1.csv`.
- **CSV:** `experiments/results/e1.csv`, one row per (variant, seed): label, policy, teams, quantum, age,
  seed, averages, `max_wait`, `starved`, throughput, utilisation, context switches, promotions,
  demotions, and per severity `*_n`, `*_avg_wait`, `*_avg_response`, `*_max_wait`.

### Results
Mean over the 5 seeds, with the min–max range in brackets:

| Policy | Avg wait | CRITICAL wait | URGENT wait | ROUTINE wait | CRITICAL response | Starved (>120 min) | Worst-case wait | Promotions |
|--------|---------:|--------------:|------------:|-------------:|------------------:|-------------------:|----------------:|-----------:|
| `fcfs` | 168.1 (100.1–228.3) | 166.6 (112.2–241.0) | 164.2 (86.5–229.7) | 171.2 (106.0–223.5) | 166.6 (112.2–241.0) | 260 (161–315) | 455 (365–562) | 0 |
| `sjf` | 70.4 (42.6–99.0) | 143.7 (68.1–225.5) | 97.5 (65.8–165.9) | 29.3 (18.7–39.7) | 143.7 (68.1–225.5) | 57 (39–72) | 1593 (1201–2201) | 0 |
| `srtf` | 67.0 (39.0–95.1) | 144.0 (72.2–224.5) | 96.2 (62.5–168.7) | 23.5 (12.6–35.0) | 135.2 (61.8–217.7) | 56 (41–67) | 1604 (1225–2204) | 0 |
| `rr-q5` | 180.8 (105.4–256.3) | 286.1 (145.1–392.5) | 215.3 (127.0–324.8) | 125.1 (78.3–167.6) | 48.5 (33.1–70.9) | 230 (141–288) | 1020 (771–1252) | 0 |
| `rr-q15` | 169.7 (97.9–240.7) | 247.7 (133.6–349.6) | 190.6 (110.3–284.7) | 131.6 (78.6–177.3) | 106.6 (72.2–156.0) | 243 (143–319) | 873 (655–1105) | 0 |
| `prio` | 230.4 (132.8–315.6) | 14.3 (8.4–22.2) | 75.7 (45.9–113.8) | 403.5 (229.7–549.1) | 14.3 (8.4–22.2) | 211 (173–244) | 882 (641–1116) | 0 |
| `prio-preempt` | 233.2 (134.8–318.8) | **9.0** (2.8–17.0) | 74.0 (43.3–111.9) | 411.9 (237.7–558.4) | **9.0** (2.8–17.0) | 214 (179–244) | 886 (642–1118) | 0 |
| `mlfq` | 191.0 (113.8–267.3) | 229.0 (106.3–329.9) | 202.5 (111.5–305.6) | 171.5 (117.6–221.7) | 25.6 (12.3–49.5) | 264 (165–334) | 973 (691–1197) | 767 (480–967) |

![Average waiting time by severity](figures/e1/e1_wait_by_severity.png)
![CRITICAL response time](figures/e1/e1_critical_response.png)
![Starved requests](figures/e1/e1_starved.png)
![Worst-case wait](figures/e1/e1_worst_wait.png)

### What the data shows
- **Priority protects CRITICAL almost completely.** CRITICAL requests wait 9–14 min on average, and only
  1 of 375 (over the 5 seeds) starves: one request in seed 4 waited 126 min, just over the 120-min
  threshold. ROUTINE pays for it: 404–412 min average wait, and 57–75% of ROUTINE requests starve.
- **SJF/SRTF have the best averages and the fewest starved requests** (about 56), because most requests
  are short ROUTINE kits. But they ignore severity (CRITICAL waits about 144 min) and have the worst
  worst-case wait (about 1,600 min, a long rescue postponed again and again).
- **MLFQ moves starvation around instead of removing it.** It improves ROUTINE (172 vs 404 min under prio)
  at the expense of CRITICAL (229 vs 14 min), and its mean starved count, 264, is the highest of any policy
  (FCFS, at 260, is within its range).
  Two mechanisms explain it, both visible in seed 1:
  - **Aging floods the top queue.** There were 814 promotions, 620 of them Q1 → Q0. The rush backlog lasts
    far longer than the 60-minute aging threshold, so most URGENT and ROUTINE requests climb into Q0 and
    compete with CRITICAL ones as equals.
  - **Long critical rescues get demoted.** Rescues take 15–45 min but Q0's quantum is 8 min, so they sink
    to lower queues partway through. CRITICAL requests still *start* quickly (response 25.6 min) but then
    wait a long time to finish: 195 of 375 CRITICAL requests (52%, all seeds) starved under MLFQ, versus 1
    under priority.

### Conclusion
The hypothesis is **not supported**. MLFQ with aging does not match Priority on critical response time
(25.6 vs 9.0–14.3 min), and it does not bring starvation to zero: its mean of 264 starved requests is the
highest of all policies.
The cause is the setup, not a bug. With a rush backlog far longer than the 60-minute aging threshold,
aging promotes almost everything into Q0, and the 8-minute Q0 quantum demotes long CRITICAL rescues.
That erases the priority MLFQ starts with. Aging bounds how long one job can be overtaken; it cannot
make waits short when demand exceeds capacity. E2 should test a longer aging threshold, never demoting
CRITICAL work, and more teams.

## E2: real Linux scheduling vs textbook priority

**Question:** E1 compared *simulated* policies. How does the real Linux scheduler treat the same kind of
workload, and how close do textbook priority scheduling and our MLFQ come to it?

### Kernel
`uname -r` gives `6.18.33.2-microsoft-standard-WSL2`. Since Linux 6.6, the normal scheduling class
(`SCHED_OTHER`, still called the "fair" class) chooses tasks with **EEVDF** (Earliest Eligible Virtual
Deadline First), which replaced CFS's leftmost-vruntime pick. **This kernel therefore uses EEVDF.** nice still
maps to the same weights (nice 0 = 1024, about ×1.25 per step), so "CFS" below means the fair class as
implemented by EEVDF. The machine has 16 CPUs. Real-time throttling is on (`sched_rt_runtime_us` =
950000: real-time tasks may use at most 95% of each second), and `sched_rr_timeslice_ms` is 100.

### Setup
- **Workload:** `gen_scenario.py --rows 120 --seed 1 --duration 2000 --rush 60-120`. That is about 1,800
  simulated minutes of work in 2,000 minutes, so **~90% load on one CPU**, with a rush-hour backlog.
- **`build/linuxsched`:** one real thread per request, created at its arrival time (1 simulated minute =
  4 ms). Each thread burns `burst × 4 ms` of its own CPU time (`CLOCK_THREAD_CPUTIME_ID`, so waiting
  doesn't count as work), 7.2 s of CPU in total.
- **Pinning:** the process is pinned to **one CPU** with `sched_setaffinity`. On 16 CPUs the threads
  would mostly run in parallel and the scheduler would rarely have to choose.
- **Recording:** every thread records its start (first instruction), finish and CPU time. Response =
  start − arrival; wait = turnaround − CPU time. Both are converted back to simulated minutes.
- **Linux modes:**

  | Mode | Class | CRITICAL / URGENT / ROUTINE |
  |------|-------|-----------------------------|
  | `cfs-flat` | `SCHED_OTHER` | nice 0 / 0 / 0 |
  | `cfs-nice` | `SCHED_OTHER` | nice −5 / 0 / +5, via `setpriority()` on each thread's TID (nice is per-thread on Linux) |
  | `fifo` | `SCHED_FIFO` | real-time priority 30 / 20 / 10 |
  | `rr` | `SCHED_RR` | real-time priority 30 / 20 / 10 |

  Without `CAP_SYS_NICE`, negative nice is refused (`RLIMIT_NICE` = 0), so `cfs-nice` uses **0 / +5 / +10**.
  The spacing is the same, and fair-class weights depend only on nice *differences*, so the CPU shares
  are identical. FIFO/RR always need `CAP_SYS_NICE`.
- **Simulated counterparts:** `schedsim --teams 1` on the same scenario: `prio`, `prio` preemptive, and `mlfq`
  with age 60. E1's numbers are not reused here, because E1 used 3 teams on a different load.
- **Safety:**
  - A watchdog thread (at the highest `SCHED_FIFO` priority when allowed) aborts any run after 30 s.
  - Scenarios needing more than 10 s of CPU are refused.
  - With privilege, the thread that creates the workers runs at `SCHED_FIFO` 40, above every worker,
    so arrivals stay on time.
  - `tests/linuxsched.sh` checks the pinning, the CPU-time budget, the nice spacing, the budget
    refusal, and that the watchdog really aborts a runaway run.
- **Reproduce:** `make e2` (or `experiments/run_e2.sh` then `experiments/plot_e2.py`). For FIFO/RR, first run
  `sudo setcap cap_sys_nice+ep build/linuxsched`; every rebuild drops this.

### Results
Simulated minutes; a single run each. Linux timings vary slightly between runs: a rerun of `cfs-flat`
gave a CRITICAL wait of 235 instead of 237.

| Variant | Response CRIT | Response URG | Response ROUT | Wait CRIT | Wait URG | Wait ROUT |
|---------|-------------:|------------:|-------------:|---------:|--------:|---------:|
| Linux `cfs-flat` | 0.4 | 0.5 | 0.4 | 237.0 | 131.5 | 76.8 |
| Linux `cfs-nice` | 0.2 | 0.1 | 0.2 | 120.2 | 139.5 | 189.2 |
| Linux `fifo` | *not yet measured* | | | | | |
| Linux `rr` | *not yet measured* | | | | | |
| sim `prio` | 35.5 | 94.4 | 240.0 | 35.5 | 94.4 | 240.0 |
| sim `prio` preemptive | 27.3 | 88.9 | 240.0 | 27.3 | 96.4 | 247.6 |
| sim `mlfq` | 16.1 | 51.1 | 99.5 | 184.2 | 122.6 | 129.2 |

![Response time by severity, Linux vs simulated](figures/e2/e2_response.png)
![Waiting time by severity, Linux vs simulated](figures/e2/e2_wait.png)

### How CFS/EEVDF and nice differ from textbook priority
- **Textbook priority is strict:** a higher-priority job always runs first, and lower ones wait
  (possibly forever). **The fair class is proportional:** every runnable thread gets CPU in proportion
  to its weight, and none is starved.
- **Response is almost zero in both fair modes**, 0.1–0.5 simulated minutes (under 2 ms of real time).
  EEVDF gives every runnable thread a short slice soon after it wakes, so nice barely affects *when* a
  thread first runs.
- **nice changes the share, so it shows up in the wait.**
  - With nice 0 for everyone (`cfs-flat`), the CPU is shared equally. Long jobs then finish last, so
    CRITICAL rescues (15–45 min of work) wait the *longest* (237 min) and short ROUTINE kits the shortest (77).
  - With nice spacing of 5 (`cfs-nice`), the weights are 1024 / 335 / 110, so a CRITICAL thread gets
    about 9× the share of a ROUTINE one. That halves the CRITICAL wait (237 → 120) and roughly doubles
    the ROUTINE wait (77 → 189).
  - It is still far from textbook priority (CRITICAL waits 27–36 min), because ROUTINE threads keep
    getting their share.

### What SCHED_FIFO does to low priorities
Not measured yet: this account has no `CAP_SYS_NICE`. This section will be completed with the `fifo`
and `rr` rows above once `sudo setcap cap_sys_nice+ep build/linuxsched` has been run and `make e2` rerun.
The question it answers: SCHED_FIFO is strict priority with no time slicing. A thread runs until it
blocks, yields or is preempted by a higher real-time priority, so lower priorities get the CPU only when
no higher one is runnable. SCHED_RR adds a 100 ms time slice, but only among threads of the *same*
priority. RT throttling reserves 5% of each second for non-real-time tasks, not for lower real-time
priorities.

### How close our MLFQ gets
- **Response:** MLFQ gives a much better response than textbook priority (16 / 51 / 100 vs 27–36 / 89–94 / 240).
  It is nowhere near the fair class, where everyone starts within about a minute.
- **Wait:** MLFQ's pattern resembles `cfs-flat`, not priority: CRITICAL waits longest (184 vs 237 in
  `cfs-flat`), while URGENT and ROUTINE are close (123/129 vs 132/77). At 90% load, aging and demotion
  turn MLFQ into something close to fair sharing, which is the same effect E1 found.
- **Overall:** of the variants measured so far, `cfs-nice` is the most balanced: the lowest CRITICAL
  wait outside strict priority, and nobody starves. Strict priority (sim `prio`) remains the only way to
  keep CRITICAL waits around half an hour (27–36 min), and it pays with ROUTINE starvation.

## Baseline (Phase 2 step 2)
`schedsim --teams 3` on `scenarios/flood_pune.csv` (200 requests in 360 min, permanently overloaded). The
full table is in README.md under "Scheduling simulator". Key points: FCFS averages 402 min of waiting;
SJF/SRTF about 190; priority gives CRITICAL about 30 min but starves the rest; MLFQ with age 60 collapses
under this overload (CRITICAL 525 min, 522 promotions). E1 reproduces the MLFQ effect at 90% load.

## Workloads
Scenarios come from `experiments/gen_scenario.py`, which is seeded and deterministic: the same arguments
always produce a byte-identical CSV. Defaults: 200 rows, 8 camps, seed 2019, 360 simulated minutes,
mix 15/35/50 (CRITICAL/URGENT/ROUTINE, exact quotas). Arrivals follow a Poisson process with a 3x
flood peak in the middle third, or with `--rush START-END` a rush hour `--rush-factor` (default 10) times
denser than the rest. Flood-plain camps carry more load, and the resource, quantity and service time
depend on severity. Record the generator command line with every experiment.

For each experiment, record: hypothesis, setup (scenario, seed, policies), metrics,
CSV schema, the command to reproduce it, and the results and analysis.
