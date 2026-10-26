#!/usr/bin/env python3
"""
OS concept: Disk scheduling as delivery routing — plotting experiment E8 — Unit 5

Reads experiments/results/e8.csv (run_e8.sh) and draws one figure, e8_routing.png, with two
panels for the five policies (mean of the seeds, min-max error bars):
  left   total km travelled (for C-SCAN including the empty return jump)
  right  the worst detour for a stop: km driven before reaching it minus its direct distance
         from the start. (The plain worst wait is always the whole route, because every stop is
         known up front, so it would only repeat the left panel.)

    python3 experiments/plot_e8.py [--csv FILE] [--out-dir DIR]

Needs matplotlib. Two measures, two panels (no dual axis). Colours: validated reference palette
slot 1 (blue) for distance and slot 2 (orange) for waiting; values are printed on the bars.
"""
import argparse
import csv
import os
import sys
from collections import OrderedDict

SURFACE, INK, INK_2, GRID = "#fcfcfb", "#1f1f1e", "#5c5b55", "#e4e3de"
KM, WAIT = "#2a78d6", "#eb6834"
LABEL = {"fcfs": "FCFS", "sstf": "SSTF", "scan": "SCAN", "cscan": "C-SCAN", "lifo": "LIFO"}


def stats(v):
    m = sum(v) / len(v)
    return m, m - min(v), max(v) - m


def panel(ax, data, key, colour, ylabel, title):
    pols = list(data)
    means, lo, hi = zip(*(stats(data[p][key]) for p in pols))
    bars = ax.bar([LABEL.get(p, p) for p in pols], means, width=0.6, color=colour, edgecolor=SURFACE, linewidth=1.5,
                  yerr=[lo, hi], error_kw={"ecolor": INK_2, "elinewidth": 1, "capsize": 3})
    for b, m, h in zip(bars, means, hi):
        ax.annotate(f"{m:.0f}", (b.get_x() + b.get_width() / 2, m + h), xytext=(0, 3), textcoords="offset points",
                    ha="center", va="bottom", fontsize=9, color=INK)
    ax.set_facecolor(SURFACE)
    ax.set_title(title, color=INK, fontsize=11, loc="left", pad=10)
    ax.set_ylabel(ylabel, color=INK_2)
    ax.tick_params(colors=INK_2, length=0)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(INK_2)
    ax.set_ylim(0, max(m + h for m, h in zip(means, hi)) * 1.15)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Plot experiment E8 (disk scheduling as routing).")
    ap.add_argument("--csv", default=os.path.join(root, "experiments", "results", "e8.csv"))
    ap.add_argument("--out-dir", default=os.path.join(root, "experiments", "results"))
    args = ap.parse_args()
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("plot_e8.py needs matplotlib: sudo apt install python3-matplotlib", file=sys.stderr)
        return 3

    data, nstops = OrderedDict(), 0
    with open(args.csv, newline="") as f:
        for row in csv.DictReader(f):
            d = data.setdefault(row["policy"], {"km": [], "wait": []})
            d["km"].append(float(row["total_km"]))
            d["wait"].append(float(row["worst_detour"]))
            nstops = int(row["stops"])
    if not data:
        sys.exit(f"{args.csv}: no rows (run experiments/run_e8.sh first)")
    nseeds = len(next(iter(data.values()))["km"])

    fig, (left, right) = plt.subplots(1, 2, figsize=(11, 4.6), facecolor=SURFACE)
    panel(left, data, "km", KM, "km travelled", "Total distance (C-SCAN incl. the return jump)")
    panel(right, data, "wait", WAIT, "extra km before the stop is reached", "Worst detour for a stop (wait − direct km)")
    fig.suptitle(f"E8: {nstops} random delivery stops on a 0-199 km highway, mean of {nseeds} seeds "
                 "(error bars = min–max)", color=INK, fontsize=12, x=0.01, ha="left")
    fig.tight_layout()
    os.makedirs(args.out_dir, exist_ok=True)
    out = os.path.join(args.out_dir, "e8_routing.png")
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    plt.close(fig)
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
