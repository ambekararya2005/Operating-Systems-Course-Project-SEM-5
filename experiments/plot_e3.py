#!/usr/bin/env python3
"""
OS concept: Deadlock prevention, avoidance, detection and recovery — plotting experiment E3 — Unit 3

Reads experiments/results/e3.csv and the timelines written by run_e3.sh and draws:

  e3_completed.png      requests completed per mode (300-request mix; mean of the seeds, min-max bars)
  e3_delay.png          average request delay (due -> granted) per mode
  e3_vehicle_time.png   where the vehicles' time went per mode: busy on complete tasks, held idle by
                        tasks still waiting (hold and wait), free
  e3_timeline_none.png  the --deadlock none run of seed 1, minute by minute: trucks and boats held,
                        requests waiting and requests completed, with the minute it froze

    python3 experiments/plot_e3.py [--results DIR] [--out-dir DIR]

Needs matplotlib. Colours: validated reference palette slots 1 (blue), 2 (orange) and 3 (aqua) in
fixed order; a neutral grey for "free"; one series per bar chart, no legend needed.
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
BLUE, ORANGE, AQUA, GREY = "#2a78d6", "#eb6834", "#1baf7a", "#c9c8c2"
MODES = ["none", "prevent", "prevent-all", "avoid", "detect"]
MODE_LABEL = {"none": "none", "prevent": "prevent\n(ordering)", "prevent-all": "prevent\n(all-or-nothing)",
              "avoid": "avoid\n(Banker's)", "detect": "detect\n(+ recovery)"}


def load(path):
    """{label: {column: [values over seeds]}} for the random-mix rows."""
    data = OrderedDict()
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            if row["scenario"] == "deadlock.csv":
                continue
            cols = data.setdefault(row["label"], {})
            for k, v in row.items():
                try:
                    cols.setdefault(k, []).append(float(v))
                except ValueError:
                    pass
    if not data:
        sys.exit(f"{path}: no rows (run experiments/run_e3.sh first)")
    return data


def stats(values):
    mean = sum(values) / len(values)
    return mean, mean - min(values), max(values) - mean


def style(ax, ylabel, title):
    ax.set_facecolor(SURFACE)
    ax.set_title(title, color=INK, fontsize=12, loc="left", pad=12)
    ax.set_ylabel(ylabel, color=INK_2)
    ax.tick_params(colors=INK_2, length=0)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(INK_2)


def bar_chart(plt, data, column, ylabel, title, path, fmt):
    modes = [m for m in MODES if m in data]
    means, lo, hi = zip(*(stats(data[m][column]) for m in modes))
    fig, ax = plt.subplots(figsize=(9, 4.8), facecolor=SURFACE)
    bars = ax.bar([MODE_LABEL[m] for m in modes], means, width=0.6, color=BLUE, edgecolor=SURFACE,
                  yerr=[lo, hi], error_kw={"ecolor": INK_2, "elinewidth": 1, "capsize": 3})
    for bar, mean, h in zip(bars, means, hi):
        ax.annotate(fmt(mean), (bar.get_x() + bar.get_width() / 2, mean + h), xytext=(0, 4),
                    textcoords="offset points", ha="center", va="bottom", color=INK, fontsize=9)
    n = len(data[modes[0]]["seed"])
    style(ax, ylabel, f"{title}\n300-request task mix, mean of {n} seeds, error bars = min–max")
    ax.set_ylim(0, max(m + h for m, h in zip(means, hi)) * 1.15)
    fig.tight_layout()
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def vehicle_time(plt, data, path):
    modes = [m for m in MODES if m in data]
    busy = [100 * stats(data[m]["vehicle_utilisation"])[0] for m in modes]
    idle = [100 * stats(data[m]["vehicle_idle_held"])[0] for m in modes]
    free = [max(0.0, 100 - b - i) for b, i in zip(busy, idle)]
    labels = [MODE_LABEL[m] for m in modes]
    fig, ax = plt.subplots(figsize=(9, 4.8), facecolor=SURFACE)
    bottom = [0.0] * len(modes)
    for vals, colour, name in ((busy, BLUE, "busy: complete tasks"), (idle, ORANGE, "held idle: task still waiting"),
                               (free, GREY, "free")):
        ax.bar(labels, vals, width=0.6, bottom=bottom, color=colour, edgecolor=SURFACE, linewidth=1.5, label=name)
        for x, (v, b) in enumerate(zip(vals, bottom)):
            if v >= 6:
                ax.text(x, b + v / 2, f"{v:.0f}%", ha="center", va="center", fontsize=9,
                        color="white" if colour != GREY else INK)
        bottom = [b + v for b, v in zip(bottom, vals)]
    style(ax, "share of vehicle time (%)", "Where the trucks' and boats' time went\n300-request task mix, mean over seeds")
    ax.set_ylim(0, 100)
    ax.legend(frameon=False, labelcolor=INK, loc="upper center", bbox_to_anchor=(0.5, -0.18), ncol=3)
    fig.tight_layout()
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def timeline(plt, path_csv, path_png, total_trucks=3, total_boats=2):
    t, trucks, boats, waiting, done = [], [], [], [], []
    with open(path_csv, newline="") as f:
        for row in csv.DictReader(f):
            t.append(int(row["t"]))
            trucks.append(int(row["trucks_held"]))
            boats.append(int(row["boats_held"]))
            waiting.append(int(row["waiting"]))
            done.append(int(row["completed"]))
    # The freeze: the last minute the number of completed requests went up.
    freeze = max((i for i in range(1, len(done)) if done[i] > done[i - 1]), default=0)
    fig, (top, bottom) = plt.subplots(2, 1, figsize=(10, 6.2), sharex=True, facecolor=SURFACE,
                                      gridspec_kw={"height_ratios": [1.3, 1]})
    top.step(t, trucks, where="post", color=BLUE, linewidth=1.8, label=f"trucks held (of {total_trucks})")
    top.step(t, boats, where="post", color=ORANGE, linewidth=1.8, label=f"boats held (of {total_boats})")
    style(top, "vehicles held", "--deadlock none, seed 1: vehicles held, requests waiting and completed")
    top.set_ylim(0, total_trucks + 0.6)
    top.legend(frameon=False, labelcolor=INK, loc="upper right")
    bottom.plot(t, done, color=AQUA, linewidth=1.8, label="requests completed")
    bottom.plot(t, waiting, color=INK_2, linewidth=1.2, linestyle="--", label="requests waiting")
    style(bottom, "requests", "")
    bottom.set_xlabel("simulated minute", color=INK_2)
    bottom.legend(frameon=False, labelcolor=INK, loc="upper left")
    for ax in (top, bottom):
        ax.axvline(freeze, color=INK, linestyle=":", linewidth=1.2)
    top.annotate(f"last request completed: minute {freeze}\nafter this, held vehicles never come back",
                 (freeze, total_trucks + 0.5), xytext=(6, 0), textcoords="offset points", color=INK, fontsize=9,
                 va="top")
    fig.tight_layout()
    fig.savefig(path_png, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Plot experiment E3 (deadlock modes).")
    ap.add_argument("--results", default=os.path.join(root, "experiments", "results"))
    ap.add_argument("--out-dir", default=os.path.join(root, "experiments", "results"))
    args = ap.parse_args()
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("plot_e3.py needs matplotlib: sudo apt install python3-matplotlib", file=sys.stderr)
        return 3

    data = load(os.path.join(args.results, "e3.csv"))
    os.makedirs(args.out_dir, exist_ok=True)
    out = lambda name: os.path.join(args.out_dir, name)
    bar_chart(plt, data, "completed", "requests completed (of 300)", "Requests completed", out("e3_completed.png"),
              lambda v: f"{v:.0f}")
    bar_chart(plt, data, "avg_delay", "average delay (simulated minutes)",
              "Average request delay (due -> granted, completed requests)", out("e3_delay.png"),
              lambda v: f"{v:.0f} min")
    vehicle_time(plt, data, out("e3_vehicle_time.png"))
    timeline(plt, os.path.join(args.results, "e3_timeline_none.csv"), out("e3_timeline_none.png"))
    for name in ("e3_completed.png", "e3_delay.png", "e3_vehicle_time.png", "e3_timeline_none.png"):
        print(out(name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
