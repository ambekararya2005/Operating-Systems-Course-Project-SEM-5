#!/usr/bin/env python3
"""
OS concept: Page replacement — faults vs number of frames — plotting experiment E6 — Unit 4

Reads experiments/results/e6.csv (run_e6.sh) and draws e6_faults_vs_frames.png: one line per
policy (FIFO, LRU, Optimal, expiry-first), stock-outs (page faults) against the number of shelf
slots (frames, 1-10), on the same perishables trace.

    python3 experiments/plot_e6.py [--csv FILE] [--out-dir DIR]

Needs matplotlib. Colours: validated reference palette slots 1-4 in fixed order (adjacent
pairs pass the colour-vision check); slots 3 and 4 sit below 3:1 contrast on the light surface,
so every line also has its own marker and a legend entry. The lines converge at 8-10 frames and
FIFO and expiry-first nearly coincide, so direct end labels would pile up: the legend names them.
"""
import argparse
import csv
import os
import sys
from collections import OrderedDict

SURFACE, INK, INK_2, GRID = "#fcfcfb", "#1f1f1e", "#5c5b55", "#e4e3de"
SERIES = [("fifo", "FIFO", "#2a78d6", "o"), ("lru", "LRU", "#eb6834", "s"),
          ("optimal", "Optimal (OPT)", "#1baf7a", "^"), ("expiry", "expiry-first", "#eda100", "D")]


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Plot experiment E6 (page replacement).")
    ap.add_argument("--csv", default=os.path.join(root, "experiments", "results", "e6.csv"))
    ap.add_argument("--out-dir", default=os.path.join(root, "experiments", "results"))
    args = ap.parse_args()
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("plot_e6.py needs matplotlib: sudo apt install python3-matplotlib", file=sys.stderr)
        return 3

    data, refs = OrderedDict(), 0
    with open(args.csv, newline="") as f:
        for row in csv.DictReader(f):
            data.setdefault(row["policy"], []).append((int(row["frames"]), int(row["faults"])))
            refs = int(row["references"])
    if not data:
        sys.exit(f"{args.csv}: no rows (run experiments/run_e6.sh first)")

    fig, ax = plt.subplots(figsize=(9, 5), facecolor=SURFACE)
    for key, name, colour, marker in SERIES:
        if key not in data:
            continue
        pts = sorted(data[key])
        xs, ys = [p[0] for p in pts], [p[1] for p in pts]
        ax.plot(xs, ys, color=colour, linewidth=2, marker=marker, markersize=6, label=name)
    ax.set_facecolor(SURFACE)
    ax.set_title(f"Stock-outs (page faults) vs shelf slots (frames)\nthe same perishables trace, {refs} references",
                 color=INK, fontsize=12, loc="left", pad=12)
    ax.set_xlabel("shelf slots (frames)", color=INK_2)
    ax.set_ylabel("stock-outs (page faults)", color=INK_2)
    ax.set_xticks(range(1, 11))
    ax.set_xlim(0.7, 10.3)
    ax.set_ylim(bottom=0)
    ax.tick_params(colors=INK_2, length=0)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(INK_2)
    ax.legend(frameon=False, labelcolor=INK, loc="upper right")
    fig.tight_layout()
    os.makedirs(args.out_dir, exist_ok=True)
    out = os.path.join(args.out_dir, "e6_faults_vs_frames.png")
    fig.savefig(out, dpi=150, facecolor=SURFACE)
    plt.close(fig)
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
