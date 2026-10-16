#!/usr/bin/env python3
"""
OS concept: Deterministic workload generation for reproducible scheduling experiments — Unit 2

Generates a ReliefOS scenario CSV:
    arrival_min,camp_id,severity,resource,quantity,service_time

The output is fully determined by the arguments: the same --seed always produces a
byte-identical file. scenarios/flood_pune.csv is produced by the defaults:

    python3 experiments/gen_scenario.py > scenarios/flood_pune.csv

Model:
  - Severity mix uses exact quotas (default 15% CRITICAL / 35% URGENT / 50% ROUTINE), shuffled.
  - Arrivals form a Poisson process over --duration simulated minutes with a flood peak:
    the rate is 3x higher in the middle third of the window.
  - Camps on the Mula-Mutha flood plain (see scenarios/camps.conf) get more requests.
  - Resource, quantity and service time depend on severity (rescue and boats are
    critical, hygiene kits routine).
Standard library only.
"""
import argparse
import random
import sys

SEVERITIES = ("CRITICAL", "URGENT", "ROUTINE")

# Relative request weight per camp id (1-8, matching scenarios/camps.conf).
# Flood-plain localities (Hadapsar, Shivajinagar, Yerawada, Sangamwadi) are busier.
CAMP_WEIGHTS = {1: 1.5, 2: 0.8, 3: 1.4, 4: 1.5, 5: 1.8, 6: 0.7, 7: 0.6, 8: 0.7}

# Resource choice by severity: (resource, weight).
RESOURCE_MIX = {
    "CRITICAL": [("RESCUE", 5), ("MED_KIT", 3), ("BOAT", 2)],
    "URGENT":   [("MED_KIT", 3), ("FOOD_KIT", 3), ("TRUCK", 2), ("BOAT", 1), ("RESCUE", 1)],
    "ROUTINE":  [("FOOD_KIT", 5), ("HYGIENE_KIT", 4), ("TRUCK", 1)],
}

# (quantity range, service time range in simulated minutes) per resource.
RESOURCE_PARAMS = {
    "RESCUE":      ((1, 25), (15, 45)),     # people to evacuate
    "BOAT":        ((1, 3), (30, 60)),
    "TRUCK":       ((1, 4), (20, 50)),
    "MED_KIT":     ((5, 60), (5, 15)),
    "FOOD_KIT":    ((20, 150), (5, 12)),
    "HYGIENE_KIT": ((20, 100), (3, 8)),
}


def severity_quotas(rows, pct):
    """Exact per-severity counts for the given percentages; rounding goes to ROUTINE."""
    crit = round(rows * pct[0] / 100)
    urg = round(rows * pct[1] / 100)
    return {"CRITICAL": crit, "URGENT": urg, "ROUTINE": rows - crit - urg}


def arrivals(rng, rows, duration):
    """Sorted integer arrival minutes from a piecewise-constant-rate Poisson process."""
    third = duration / 3
    # Rate weights: before peak, peak (3x), after peak.
    weights = (1.0, 3.0, 1.0)
    times = []
    for _ in range(rows):
        seg = rng.choices((0, 1, 2), weights=weights)[0]
        times.append(seg * third + rng.random() * third)
    return sorted(min(int(t), duration - 1) for t in times)


def generate(rows, camps, seed, duration, pct):
    rng = random.Random(seed)
    camp_ids = list(range(1, camps + 1))
    camp_w = [CAMP_WEIGHTS.get(c, 1.0) for c in camp_ids]

    quotas = severity_quotas(rows, pct)
    severities = [s for s in SEVERITIES for _ in range(quotas[s])]
    rng.shuffle(severities)

    # Every camp gets at least one request; the rest are drawn by weight.
    camp_of = camp_ids[:rows] + rng.choices(camp_ids, weights=camp_w, k=max(0, rows - camps))
    rng.shuffle(camp_of)

    out = []
    for arrival, camp, sev in zip(arrivals(rng, rows, duration), camp_of, severities):
        names, w = zip(*RESOURCE_MIX[sev])
        res = rng.choices(names, weights=w)[0]
        (qlo, qhi), (slo, shi) = RESOURCE_PARAMS[res]
        out.append((arrival, camp, sev, res, rng.randint(qlo, qhi), rng.randint(slo, shi)))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1].strip())
    ap.add_argument("--rows", type=int, default=200, help="number of requests (default 200)")
    ap.add_argument("--camps", type=int, default=8, help="camp ids 1..N (default 8)")
    ap.add_argument("--seed", type=int, default=2019, help="RNG seed (default 2019)")
    ap.add_argument("--duration", type=int, default=360,
                    help="scenario length in simulated minutes (default 360)")
    ap.add_argument("--mix", default="15,35,50",
                    help="CRITICAL,URGENT,ROUTINE percentages (default 15,35,50)")
    ap.add_argument("-o", "--output", help="output file (default stdout)")
    args = ap.parse_args()

    pct = [float(x) for x in args.mix.split(",")]
    if len(pct) != 3 or abs(sum(pct) - 100) > 1e-9 or min(pct) < 0:
        ap.error("--mix needs three non-negative percentages summing to 100")
    if args.rows < args.camps or args.camps < 1 or args.duration < 1:
        ap.error("need rows >= camps >= 1 and duration >= 1")

    rows = generate(args.rows, args.camps, args.seed, args.duration, pct)
    lines = ["arrival_min,camp_id,severity,resource,quantity,service_time"]
    lines += [",".join(str(v) for v in r) for r in rows]
    text = "\n".join(lines) + "\n"
    if args.output:
        with open(args.output, "w", newline="\n") as f:
            f.write(text)
    else:
        sys.stdout.write(text)


if __name__ == "__main__":
    main()
