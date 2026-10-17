# ReliefOS Experiments

> Placeholder. Experiments start in Phase 2.

Every experiment is deterministic for a given `--seed` and writes CSV to `experiments/results/`.

| ID | Topic (Unit) | Question | Status |
|----|--------------|----------|--------|
| E1 | CPU scheduling (Unit 2) | TBD in Phase 2 | planned |
| E2 | CPU scheduling (Unit 2) | TBD in Phase 2 | planned |

## Baseline (Phase 2 step 2)
`schedsim --teams 3` on `scenarios/flood_pune.csv`. The full table is in README.md under "Scheduling
simulator". Key points: FCFS averages 402 min of waiting; SJF/SRTF about 190; priority gives CRITICAL
about 30 min but starves the rest; MLFQ with age 60 collapses under this overload (CRITICAL 525 min,
522 promotions). These are the starting points for E1 (policy comparison) and E2 (sensitivity to
teams, quantum and age).

## Workloads
Scenarios come from `experiments/gen_scenario.py`, which is seeded and deterministic: the same arguments
always produce a byte-identical CSV. Defaults: 200 rows, 8 camps, seed 2019, 360 simulated minutes,
mix 15/35/50 (CRITICAL/URGENT/ROUTINE, exact quotas). Arrivals follow a Poisson process with a 3x
flood peak in the middle third. Flood-plain camps carry more load, and the resource, quantity and
service time depend on severity. Record the generator command line with every experiment.

For each experiment, record: hypothesis, setup (scenario, seed, policies), metrics,
CSV schema, the command to reproduce it, and the results and analysis.
