# ReliefOS Experiments

> Placeholder. Experiments start in Phase 2.

Every experiment is deterministic for a given `--seed` and writes CSV to `experiments/results/`.

| ID | Topic (Unit) | Question | Status |
|----|--------------|----------|--------|
| E1 | CPU scheduling (Unit 2) | TBD in Phase 2 | planned |
| E2 | CPU scheduling (Unit 2) | TBD in Phase 2 | planned |

## Workloads
Scenarios come from `experiments/gen_scenario.py`, which is seeded and deterministic: the same arguments
always produce a byte-identical CSV. Defaults: 200 rows, 8 camps, seed 2019, 360 simulated minutes,
mix 15/35/50 (CRITICAL/URGENT/ROUTINE, exact quotas). Arrivals follow a Poisson process with a 3x
flood peak in the middle third. Flood-plain camps carry more load, and the resource, quantity and
service time depend on severity. Record the generator command line with every experiment.

For each experiment, record: hypothesis, setup (scenario, seed, policies), metrics,
CSV schema, the command to reproduce it, and the results and analysis.
