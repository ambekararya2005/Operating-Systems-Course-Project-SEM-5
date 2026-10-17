# ReliefOS Experiments

Every experiment is deterministic for a given `--seed` and writes CSV to `experiments/results/`
(git-ignored). Figures used here are copied to `docs/figures/` so they are committed.

| ID | Topic (Unit) | Question | Status |
|----|--------------|----------|--------|
| E1 | CPU scheduling (Unit 2) | Which triage policy serves CRITICAL requests fastest without starving the rest? | done |
| E2 | CPU scheduling (Unit 2) | How sensitive are the results to aging threshold, quanta and number of teams? | planned |

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
