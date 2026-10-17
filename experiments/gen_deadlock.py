#!/usr/bin/env python3
"""
OS concept: Deadlock characterization — a random workload of hold-and-wait tasks — Unit 3

Generates a scenario CSV (with the optional 7th `task` column) in which every camp runs a few
tasks. A task asks for 2-3 Banker's resources one after another (trucks and boats in a random
order, sometimes a crate of kits) and holds them all until the last is granted, so under
--deadlock none circular waits form; the other modes must finish it. Used with the default
scenarios/resources.conf (3 trucks, 2 boats, 40 MED and 60 FOOD crates).

    python3 experiments/gen_deadlock.py [--camps N] [--tasks T] [--seed S] [--duration MIN] [-o FILE]

Deterministic for a given --seed. Standard library only.
"""
import argparse
import random
import sys

SEVERITIES = ["CRITICAL", "URGENT", "ROUTINE"]


def generate(camps, tasks, seed, duration):
    rng = random.Random(seed)
    rows = []
    for camp in range(1, camps + 1):
        for t in range(1, tasks + 1):
            start = rng.randrange(0, duration)
            sev = rng.choices(SEVERITIES, weights=(1, 2, 3))[0]
            service = rng.randint(10, 30)
            vehicles = ["TRUCK", "BOAT"]
            rng.shuffle(vehicles)                   # both orders: the source of circular waits
            parts = vehicles[:]
            if rng.random() < 0.3:
                parts.insert(rng.randrange(0, 3), rng.choice(["MED_KIT", "FOOD_KIT"]))
            at = start
            for res in parts:
                qty = rng.randint(10, 40) if res.endswith("_KIT") else 1
                rows.append((at, camp, sev, res, qty, service, t))
                at += rng.randint(1, 8)             # later rows come while the first are held
    rows.sort(key=lambda r: (r[0], r[1]))
    return rows


def main():
    ap = argparse.ArgumentParser(description="Generate a random hold-and-wait task mix.")
    ap.add_argument("--camps", type=int, default=6)
    ap.add_argument("--tasks", type=int, default=3, help="tasks per camp")
    ap.add_argument("--seed", type=int, default=11)
    ap.add_argument("--duration", type=int, default=120, help="tasks start within this many minutes")
    ap.add_argument("-o", "--output", help="output CSV (default: stdout)")
    args = ap.parse_args()
    if args.camps < 1 or args.tasks < 1 or args.duration < 1:
        ap.error("--camps, --tasks and --duration must be positive")

    lines = ["arrival_min,camp_id,severity,resource,quantity,service_time,task"]
    lines += [",".join(str(v) for v in r) for r in generate(args.camps, args.tasks, args.seed, args.duration)]
    text = "\n".join(lines) + "\n"
    if args.output:
        with open(args.output, "w") as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
