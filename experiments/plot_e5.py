#!/usr/bin/env python3
"""
OS concept: Memory allocation strategies and fragmentation — plotting experiment E5 — Unit 4

Reads experiments/results/e5.csv (run_e5.sh) and draws one figure, e5_fragmentation.png, with
two panels for the five allocators (mean of the seeds, min-max error bars):
  left   average external and internal fragmentation (grouped bars)
  right  allocation failures, as a share of allocation requests

    python3 experiments/plot_e5.py [--csv FILE] [--out-dir DIR]

Needs matplotlib. Colours: validated reference palette slots 1 (blue: external fragmentation),
2 (orange: internal fragmentation) and 3 (aqua: failures), each always the same entity; values
are printed on the bars, so identity is never colour alone.
"""
import argparse
import csv
import os
import sys
from collections import OrderedDict

SURFACE, INK, INK_2, GRID = "#fcfcfb", "#1f1f1e", "#5c5b55", "#e4e3de"
EXT, INT, FAIL = "#2a78d6", "#eb6834", "#1baf7a"
LABEL = {"first": "first fit", "best": "best fit", "worst": "worst fit", "buddy": "buddy", "slab": "slab"}


def load(path):
    data = OrderedDict()
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            d = data.setdefault(row["alloc"], {"ext": [], "int": [], "fail": []})
            d["ext"].append(100 * float(row["ext_frag_avg"]))
            d["int"].append(100 * float(row["int_frag_avg"]))
            d["fail"].append(100 * float(row["failures"]) / float(row["allocs"]))
    if not data:
        sys.exit(f"{path}: no rows (run experiments/run_e5.sh first)")
    return data


def stats(v):
    m = sum(v) / len(v)
    return m, m - min(v), max(v) - m


def style(ax, ylabel, title):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, color=INK, fontsize=11, loc="left", pad=10)
    ax.set_ylabel(ylabel, color=INK_2)
    ax.tick_params(colors=INK_2, length=0)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(INK_2)


def label_bars(ax, bars, means, his):
    for b, m, h in zip(bars, means, his):
        ax.annotate(f"{m:.0f}%", (b.get_x() + b.get_width() / 2, m + h), xytext=(0, 3), textcoords="offset points",
                    ha="center", va="bottom", fontsize=8, color=INK)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Plot experiment E5 (allocation strategies).")
    ap.add_argument("--csv", default=os.path.join(root, "experiments", "results", "e5.csv"))
    ap.add_argument("--out-dir", default=os.path.join(root, "experiments", "results"))
    args = ap.parse_args()
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("plot_e5.py needs matplotlib: sudo apt install python3-matplotlib", file=sys.stderr)
        return 3

    data = load(args.csv)
    allocs = list(data)
    labels = [LABEL.get(a, a) for a in allocs]
    nseeds = len(data[allocs[0]]["ext"])
    fig, (left, right) = plt.subplots(1, 2, figsize=(12, 4.8), facecolor=SURFACE, gridspec_kw={"width_ratios": [3, 2]})

    w = 0.38
    xs = range(len(allocs))
    for off, key, colour, name in ((-w / 2, "ext", EXT, "external (free space split into pieces)"),
                                   (w / 2, "int", INT, "internal (wasted inside blocks / slabs)")):
        means, lo, hi = zip(*(stats(data[a][key]) for a in allocs))
        bars = left.bar([x + off for x in xs], means, width=w, color=colour, edgecolor=SURFACE, linewidth=1.5, label=name,
                        yerr=[lo, hi], error_kw={"ecolor": INK_2, "elinewidth": 1, "capsize": 2})
        label_bars(left, bars, means, hi)
    left.set_xticks(list(xs))
    left.set_xticklabels(labels)
    style(left, "average fragmentation (%)", "Fragmentation, averaged over the trace")
    left.set_ylim(0, 100)
    left.legend(frameon=False, labelcolor=INK, loc="upper right", fontsize=9)

    means, lo, hi = zip(*(stats(data[a]["fail"]) for a in allocs))
    bars = right.bar(labels, means, width=0.6, color=FAIL, edgecolor=SURFACE, linewidth=1.5,
                     yerr=[lo, hi], error_kw={"ecolor": INK_2, "elinewidth": 1, "capsize": 2})
    label_bars(right, bars, means, hi)
    style(right, "failed allocations (% of requests)", "Allocation failures")
    right.set_ylim(0, max(m + h for m, h in zip(means, hi)) * 1.2)
    right.tick_params(axis="x", labelrotation=0)

    fig.suptitle(f"E5: 1024-unit warehouse floor, the same 2000-operation trace per seed, mean of {nseeds} seeds "
                 "(error bars = min–max)", color=INK, fontsize=12, x=0.01, ha="left")
    fig.tight_layout()
    os.makedirs(args.out_dir, exist_ok=True)
    out = os.path.join(args.out_dir, "e5_fragmentation.png")
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    plt.close(fig)
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
