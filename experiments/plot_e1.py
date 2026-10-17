#!/usr/bin/env python3
"""
OS concept: Scheduling criteria — plotting experiment E1 (policy comparison) — Unit 2

Reads experiments/results/e1.csv (written by run_e1.sh) and draws four bar charts, each
bar the mean over seeds with min-max error bars:

  e1_wait_by_severity.png   average waiting time per policy, bars grouped by severity
  e1_critical_response.png  CRITICAL requests' average response time per policy
  e1_starved.png            starved requests (wait > threshold) per policy
  e1_worst_wait.png         worst-case wait per policy

and, from experiments/results/e1b.csv (the E1b load sweep), two line charts against offered load,
each point the mean over seeds with min-max error bars, with a dashed line at the first load
where mlfq starves a request in any seed:

  e1b_starved_vs_load.png             starved requests
  e1b_critical_response_vs_load.png   CRITICAL requests' average response time

    python3 experiments/plot_e1.py [--csv FILE] [--sweep-csv FILE] [--out-dir DIR]

Needs matplotlib (sudo apt install python3-matplotlib). Colours: the validated reference
categorical palette, slots 1-3 in fixed order (validated with the dataviz skill's
validate_palette.js); the aqua slot sits below 3:1 contrast, so EXPERIMENTS.md carries
the full results table as the accessible table view. The E1b lines use slots 7 (violet),
8 (red) and 6 (green), not the severity colours, since they encode policies; mlfq and
mlfq-floor nearly coincide, so they also differ by line style and marker and are labelled
directly at the line ends.
"""
import argparse
import csv
import os
import sys
from collections import OrderedDict

SURFACE = "#fcfcfb"
INK = "#1f1f1e"          # primary text
INK_2 = "#5c5b55"        # secondary text, error bars
GRID = "#e4e3de"
SEVERITY = [("critical", "CRITICAL", "#2a78d6"),     # categorical slot 1 (blue)
            ("urgent", "URGENT", "#eb6834"),         # slot 2 (orange)
            ("routine", "ROUTINE", "#1baf7a")]       # slot 3 (aqua)
SINGLE = "#2a78d6"       # one series: slot 1, no legend (the title names it)
SWEEP = [("prio-preempt", "#6250d6", "-", "o"),        # slot 7 (violet)
         ("mlfq", "#e34948", "-", "s"),                # slot 8 (red)
         ("mlfq-floor", "#008300", "--", "^")]         # slot 6 (green), dashed


def load(path):
    """{variant: {column: [values over seeds]}} in first-seen variant order."""
    data = OrderedDict()
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            cols = data.setdefault(row["label"], {})
            for k, v in row.items():
                try:
                    cols.setdefault(k, []).append(float(v))
                except ValueError:
                    pass
    if not data:
        sys.exit(f"{path}: no rows (run experiments/run_e1.sh first)")
    return data


def stats(values):
    mean = sum(values) / len(values)
    return mean, mean - min(values), max(values) - mean


def style(ax, ylabel, title, plt):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, color=INK, fontsize=12, loc="left", pad=12)
    ax.set_ylabel(ylabel, color=INK_2)
    ax.set_xlabel("scheduling policy (3 rescue teams)", color=INK_2)
    ax.tick_params(colors=INK_2, length=0)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(INK_2)
    ax.set_ylim(bottom=0)


def bar_chart(plt, data, column, ylabel, title, path, nseeds):
    labels = list(data)
    means, lo, hi = zip(*(stats(data[l][column]) for l in labels))
    fig, ax = plt.subplots(figsize=(9, 4.8), facecolor=SURFACE)
    ax.bar(labels, means, width=0.6, color=SINGLE, edgecolor=SURFACE, linewidth=1.5,
           yerr=[lo, hi], error_kw={"ecolor": INK_2, "elinewidth": 1, "capsize": 3})
    style(ax, ylabel, f"{title}\nmean of {nseeds} seeds, error bars = min–max", plt)
    fig.tight_layout()
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def grouped_chart(plt, data, path, nseeds):
    labels = list(data)
    n = len(SEVERITY)
    width = 0.8 / n
    fig, ax = plt.subplots(figsize=(10, 5), facecolor=SURFACE)
    for i, (key, name, colour) in enumerate(SEVERITY):
        means, lo, hi = zip(*(stats(data[l][f"{key}_avg_wait"]) for l in labels))
        xs = [x + (i - (n - 1) / 2) * width for x in range(len(labels))]
        ax.bar(xs, means, width=width, color=colour, edgecolor=SURFACE, linewidth=1.5, label=name,
               yerr=[lo, hi], error_kw={"ecolor": INK_2, "elinewidth": 1, "capsize": 2})
    ax.set_xticks(range(len(labels)))
    ax.set_xticklabels(labels)
    style(ax, "average waiting time (simulated minutes)",
          f"Average waiting time by severity\nmean of {nseeds} seeds, error bars = min–max", plt)
    legend = ax.legend(title="severity", frameon=False, labelcolor=INK, loc="upper left")
    legend.get_title().set_color(INK_2)
    fig.tight_layout()
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def load_sweep(path):
    """{label: {target_load: {column: [values over seeds]}}}, or None if the file is missing."""
    if not os.path.exists(path):
        return None
    data = {}
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            cols = data.setdefault(row["label"], {}).setdefault(float(row["target_load"]), {})
            for k, v in row.items():
                try:
                    cols.setdefault(k, []).append(float(v))
                except ValueError:
                    pass
    return data or None


def sweep_chart(plt, data, column, ylabel, title, path, nseeds, threshold):
    fig, ax = plt.subplots(figsize=(9, 5), facecolor=SURFACE)
    ends = []
    for label, colour, ls, marker in SWEEP:
        if label not in data:
            continue
        loads = sorted(data[label])
        means, lo, hi = zip(*(stats(data[label][x][column]) for x in loads))
        ax.errorbar(loads, means, yerr=[lo, hi], color=colour, linestyle=ls, marker=marker,
                    markersize=5, linewidth=2, capsize=3, elinewidth=1, label=label)
        ends.append((means[-1], label, colour, loads[-1]))
    # Direct labels at the right end, nudged apart so overlapping lines stay readable.
    ends.sort()
    span = ax.get_ylim()[1] - ax.get_ylim()[0]
    last = None
    for y, label, colour, x in ends:
        if last is not None and y - last < 0.06 * span:
            y = last + 0.06 * span
        ax.annotate(label, (x, y), xytext=(8, 0), textcoords="offset points", color=colour,
                    va="center", fontsize=9)
        last = y
    if threshold is not None:
        ax.axvline(threshold, color=INK_2, linestyle=":", linewidth=1.2)
        ax.annotate(f"mlfq first starves\na request at {threshold:g}% load", (threshold, 1),
                    xycoords=("data", "axes fraction"), xytext=(6, -6), textcoords="offset points",
                    color=INK_2, fontsize=9, va="top")
    style(ax, ylabel, f"{title}\nmean of {nseeds} seeds, error bars = min–max", plt)
    ax.set_xlabel("offered load (total work / (3 teams × duration), %)", color=INK_2)
    ax.set_xticks(sorted(next(iter(data.values()))))
    ax.set_xlim(right=max(next(iter(data.values()))) + 12)
    ax.legend(frameon=False, labelcolor=INK, loc="upper left", bbox_to_anchor=(0, 0.85))
    fig.tight_layout()
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Plot experiment E1 (scheduling policy comparison).")
    ap.add_argument("--csv", default=os.path.join(root, "experiments", "results", "e1.csv"))
    ap.add_argument("--sweep-csv", default=os.path.join(root, "experiments", "results", "e1b.csv"))
    ap.add_argument("--out-dir", default=os.path.join(root, "experiments", "results"))
    args = ap.parse_args()

    try:
        import matplotlib
        matplotlib.use("Agg")       # files only, no display needed
        import matplotlib.pyplot as plt
    except ImportError:
        print("plot_e1.py needs matplotlib: sudo apt install python3-matplotlib", file=sys.stderr)
        return 3

    data = load(args.csv)
    nseeds = len(next(iter(data.values()))["seed"])
    starve = int(next(iter(data.values()))["starve_threshold"][0])
    os.makedirs(args.out_dir, exist_ok=True)
    out = lambda name: os.path.join(args.out_dir, name)

    grouped_chart(plt, data, out("e1_wait_by_severity.png"), nseeds)
    bar_chart(plt, data, "critical_avg_response",
              "average response time (simulated minutes)",
              "CRITICAL requests: time from arrival to first service", out("e1_critical_response.png"),
              nseeds)
    bar_chart(plt, data, "starved", "starved requests (count, of 500)",
              f"Starved requests (waited more than {starve} min)", out("e1_starved.png"), nseeds)
    bar_chart(plt, data, "max_wait", "worst-case waiting time (simulated minutes)",
              "Worst-case wait (longest any request waited)", out("e1_worst_wait.png"), nseeds)
    for name in ("e1_wait_by_severity.png", "e1_critical_response.png", "e1_starved.png",
                 "e1_worst_wait.png"):
        print(out(name))

    sweep = load_sweep(args.sweep_csv)
    if sweep is None:
        print(f"{args.sweep_csv}: missing, skipping the E1b charts", file=sys.stderr)
        return 0
    any_label = next(iter(sweep.values()))
    nseeds_b = len(next(iter(any_label.values()))["seed"])
    starved_at = [x for x in sorted(sweep.get("mlfq", {})) if max(sweep["mlfq"][x]["starved"]) > 0]
    threshold = starved_at[0] if starved_at else None
    sweep_chart(plt, sweep, "starved", "starved requests (count, of 500)",
                f"Starved requests (waited more than {starve} min) against load", out("e1b_starved_vs_load.png"),
                nseeds_b, threshold)
    sweep_chart(plt, sweep, "critical_avg_response", "average response time (simulated minutes)",
                "CRITICAL requests: time from arrival to first service, against load",
                out("e1b_critical_response_vs_load.png"), nseeds_b, threshold)
    for name in ("e1b_starved_vs_load.png", "e1b_critical_response_vs_load.png"):
        print(out(name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
