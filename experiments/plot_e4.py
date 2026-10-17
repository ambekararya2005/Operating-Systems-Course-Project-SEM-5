#!/usr/bin/env python3
"""
OS concept: The critical-section problem — plotting experiment E4 (Peterson, mutex, spinlock,
semaphore) — Unit 3

Reads experiments/results/e4.csv (written by run_e4.sh) and draws two bar charts, one bar per
racelab mode, each the mean over runs with min-max error bars and the value printed on the bar:

  e4_error.png    lost updates (expected 2N - final count); symmetric-log scale, because
                  `none` loses about a million updates and peterson-nofence a few dozen
  e4_runtime.png  wall-clock runtime of the 2N increments

    python3 experiments/plot_e4.py [--csv FILE] [--out-dir DIR]

Needs matplotlib. Colours encode one thing, whether the mode guarantees mutual exclusion:
reference palette slot 8 (red) for the unsafe modes, slot 1 (blue) for the correct ones.
"""
import argparse
import csv
import os
import sys
from collections import OrderedDict

SURFACE = "#fcfcfb"
INK = "#1f1f1e"
INK_2 = "#5c5b55"
GRID = "#e4e3de"
UNSAFE = "#e34948"       # slot 8 (red): no mutual exclusion
SAFE = "#2a78d6"         # slot 1 (blue): mutual exclusion holds
UNSAFE_MODES = {"none", "peterson-nofence"}


def load(path):
    """{mode: {column: [values over runs]}} in first-seen order."""
    data = OrderedDict()
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            cols = data.setdefault(row["mode"], {})
            for k, v in row.items():
                try:
                    cols.setdefault(k, []).append(float(v))
                except ValueError:
                    pass
    if not data:
        sys.exit(f"{path}: no rows (run experiments/run_e4.sh first)")
    return data


def stats(values):
    mean = sum(values) / len(values)
    return mean, mean - min(values), max(values) - mean


def chart(plt, data, column, ylabel, title, path, fmt, log=False, legend_loc="upper right"):
    from matplotlib.patches import Patch
    modes = list(data)
    means, lo, hi = zip(*(stats(data[m][column]) for m in modes))
    colours = [UNSAFE if m in UNSAFE_MODES else SAFE for m in modes]
    fig, ax = plt.subplots(figsize=(9, 4.8), facecolor=SURFACE)
    bars = ax.bar(modes, means, width=0.6, color=colours, edgecolor=SURFACE, linewidth=1.5,
                  yerr=[lo, hi], error_kw={"ecolor": INK_2, "elinewidth": 1, "capsize": 3})
    if log:
        ax.set_yscale("symlog", linthresh=1)
    for bar, m, mean, h in zip(bars, modes, means, hi):
        ax.annotate(fmt(mean), (bar.get_x() + bar.get_width() / 2, mean + h),
                    xytext=(0, 4), textcoords="offset points", ha="center", va="bottom",
                    color=INK, fontsize=9)
    nruns = len(data[modes[0]]["run"])
    iters = int(data[modes[0]]["iters"][0])
    ax.set_facecolor(SURFACE)
    ax.set_title(f"{title}\n2 threads x {iters:,} increments, mean of {nruns} runs, "
                 "error bars = min–max", color=INK, fontsize=12, loc="left", pad=12)
    ax.set_ylabel(ylabel, color=INK_2)
    ax.set_xlabel("racelab --mode (critical-section protection)", color=INK_2)
    ax.tick_params(colors=INK_2, length=0)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(INK_2)
    ax.set_ylim(bottom=0, top=(max(m + h for m, h in zip(means, hi)) * (8 if log else 1.3)) or 1)
    ax.legend(handles=[Patch(color=UNSAFE, label="no mutual exclusion"),
                       Patch(color=SAFE, label="mutual exclusion holds")],
              frameon=False, labelcolor=INK, loc=legend_loc)
    fig.tight_layout()
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Plot experiment E4 (critical-section protection).")
    ap.add_argument("--csv", default=os.path.join(root, "experiments", "results", "e4.csv"))
    ap.add_argument("--out-dir", default=os.path.join(root, "experiments", "results"))
    args = ap.parse_args()

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("plot_e4.py needs matplotlib: sudo apt install python3-matplotlib", file=sys.stderr)
        return 3

    data = load(args.csv)
    os.makedirs(args.out_dir, exist_ok=True)
    out = lambda name: os.path.join(args.out_dir, name)
    chart(plt, data, "error", "lost updates (expected 2N − final count, symlog scale)",
          "Lost updates: final count error", out("e4_error.png"), lambda v: f"{v:,.0f}", log=True)
    chart(plt, data, "runtime_ms", "runtime (ms)", "Runtime of the 2N increments",
          out("e4_runtime.png"), lambda v: f"{v:.0f} ms", legend_loc="upper left")
    for name in ("e4_error.png", "e4_runtime.png"):
        print(out(name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
