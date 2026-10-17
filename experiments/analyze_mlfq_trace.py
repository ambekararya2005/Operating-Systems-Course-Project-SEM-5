#!/usr/bin/env python3
"""
OS concept: MLFQ with aging — tracing demotion, promotion and per-level waiting — Unit 2

Reads a `schedsim --policy mlfq --trace FILE` event trace and:
  1. checks the MLFQ rules as invariants on every event (to tell a bug from a real effect):
       - a job enters at its severity level (CRITICAL Q0, URGENT Q1, ROUTINE Q2)
       - a demotion happens exactly one full quantum (Q0: 8, Q1: 16) after the dispatch, and
         moves the job down exactly one level
       - a promotion happens to a WAITING job, more than --age minutes after it entered that
         level, and moves it up exactly one level
       - the waiting time rebuilt from the events equals finish - arrival - burst
  2. reports, per severity: average waiting time spent at each queue level, demotions,
     promotions and burst length;
  3. splits waiting time by burst length, to separate "long job" from "critical job";
  4. prints the timeline of a few CRITICAL jobs.

    python3 experiments/analyze_mlfq_trace.py TRACE.csv [--age 60] [--show 3]
Exit status 1 if any invariant fails. Standard library only.
"""
import argparse
import csv
import sys
from collections import defaultdict

QUANTUM = {0: 8, 1: 16}
ENTRY = {"CRITICAL": 0, "URGENT": 1, "ROUTINE": 2}
SEVS = ["CRITICAL", "URGENT", "ROUTINE"]


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[1].strip())
    ap.add_argument("trace")
    ap.add_argument("--age", type=int, default=60)
    ap.add_argument("--show", type=int, default=3, help="CRITICAL job timelines to print")
    args = ap.parse_args()

    jobs = {}
    errors = []
    with open(args.trace, newline="") as f:
        for r in csv.DictReader(f):
            t, ev, jid = int(r["t"]), r["event"], int(r["id"])
            lvl = int(r["level"])
            j = jobs.setdefault(jid, {"sev": r["severity"], "burst": int(r["burst"]), "events": [],
                                      "wait": defaultdict(int), "demote": 0, "promote": 0,
                                      "state": None, "since": None, "level": None, "disp": None})
            j["events"].append((t, ev, lvl))
            if ev == "arrive":
                if lvl != ENTRY[j["sev"]]:
                    errors.append(f"{jid}: entered Q{lvl}, expected Q{ENTRY[j['sev']]}")
                j.update(state="ready", since=t, level=lvl, arrival=t)
            elif ev == "dispatch":
                if j["state"] != "ready":
                    errors.append(f"{jid}: dispatched while {j['state']}")
                j["wait"][j["level"]] += t - j["since"]
                j.update(state="running", disp=t)
            elif ev == "demote":
                old = j["level"]
                if j["state"] != "running" or lvl != old + 1:
                    errors.append(f"{jid}: bad demotion Q{old}->Q{lvl} while {j['state']} at t={t}")
                elif t - j["disp"] != QUANTUM[old]:
                    errors.append(f"{jid}: demoted after {t - j['disp']} min in Q{old}, quantum {QUANTUM[old]}")
                j["level"] = lvl
                j["demote"] += 1
            elif ev == "preempt":
                if j["state"] != "running":
                    errors.append(f"{jid}: preempted while {j['state']}")
                j.update(state="ready", since=t, level=lvl)
            elif ev == "promote":
                old = j["level"]
                if j["state"] != "ready" or lvl != old - 1:
                    errors.append(f"{jid}: bad promotion Q{old}->Q{lvl} while {j['state']} at t={t}")
                elif t - j["since"] <= args.age:
                    errors.append(f"{jid}: promoted after only {t - j['since']} min in Q{old}")
                j["wait"][old] += t - j["since"]
                j.update(since=t, level=lvl)
                j["promote"] += 1
            elif ev == "finish":
                j.update(state="done", finish=t)

    for jid, j in jobs.items():
        if j["state"] != "done":
            errors.append(f"{jid}: never finished")
            continue
        total = sum(j["wait"].values())
        if total != j["finish"] - j["arrival"] - j["burst"]:
            errors.append(f"{jid}: rebuilt wait {total} != finish-arrival-burst "
                          f"{j['finish'] - j['arrival'] - j['burst']}")

    print(f"invariants over {len(jobs)} jobs: {'all hold' if not errors else f'{len(errors)} VIOLATIONS'}")
    for e in errors[:20]:
        print("  " + e)

    print("\nper severity (averages; waiting time in simulated minutes, split by queue level)")
    print(f"  {'severity':9} {'n':>3} {'burst':>6} {'wait Q0':>8} {'wait Q1':>8} {'wait Q2':>8} {'total':>7}"
          f" {'demotions':>9} {'promotions':>10}")
    for s in SEVS:
        js = [j for j in jobs.values() if j["sev"] == s and j["state"] == "done"]
        if not js:
            continue
        n = len(js)
        avg = lambda f: sum(f(j) for j in js) / n
        print(f"  {s:9} {n:3d} {avg(lambda j: j['burst']):6.1f} {avg(lambda j: j['wait'][0]):8.1f}"
              f" {avg(lambda j: j['wait'][1]):8.1f} {avg(lambda j: j['wait'][2]):8.1f}"
              f" {avg(lambda j: sum(j['wait'].values())):7.1f} {avg(lambda j: j['demote']):9.2f}"
              f" {avg(lambda j: j['promote']):10.2f}")

    print("\naverage waiting time by burst length (n in brackets)")
    buckets = [(1, 10), (11, 20), (21, 40), (41, 999)]
    print("  " + f"{'burst':9}" + "".join(f"{s:>16}" for s in SEVS))
    for lo, hi in buckets:
        cells = []
        for s in SEVS:
            js = [j for j in jobs.values() if j["sev"] == s and lo <= j["burst"] <= hi]
            cells.append(f"{sum(sum(j['wait'].values()) for j in js) / len(js):9.1f} ({len(js):2d})"
                         if js else f"{'-':>9}     ")
        label = f"{lo}-{hi}" if hi < 999 else f">{lo - 1}"
        print("  " + f"{label:9}" + "".join(f"{c:>16}" for c in cells))

    crit = sorted((j for j in jobs.values() if j["sev"] == "CRITICAL"),
                  key=lambda j: -sum(j["wait"].values()))
    for j in crit[:args.show]:
        jid = next(k for k, v in jobs.items() if v is j)
        print(f"\nCRITICAL job {jid}: burst {j['burst']}, waited {sum(j['wait'].values())} "
              f"(Q0 {j['wait'][0]}, Q1 {j['wait'][1]}, Q2 {j['wait'][2]}), "
              f"{j['demote']} demotions, {j['promote']} promotions")
        print("  " + "  ".join(f"t={t}:{ev}->Q{lvl}" for t, ev, lvl in j["events"]))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
