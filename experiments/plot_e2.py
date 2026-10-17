#!/usr/bin/env python3
"""
OS concept: Linux scheduling vs textbook policies — plotting experiment E2 — Unit 2

Reads experiments/results/e2_<mode>.csv (real threads, build/linuxsched) and e2_sim.csv
(build/schedsim --teams 1 on the same scenario) and draws, per severity:

  e2_response.png   average response time (arrival -> first time on the CPU)
  e2_wait.png       average waiting time (time runnable but not running)

Real Linux modes are on the left, simulated policies on the right, separated by a divider.
Linux modes that were not run (fifo/rr need CAP_SYS_NICE) are simply left out. Each value
is a single run (Linux timings vary slightly between runs; the simulation is exact).

    python3 experiments/plot_e2.py [--results DIR] [--out-dir DIR]

Needs matplotlib. Colours: the same validated severity palette as E1 (slots 1-3).
"""
import argparse
import csv
import os
import sys

SURFACE = "#fcfcfb"
INK = "#1f1f1e"
INK_2 = "#5c5b55"
GRID = "#e4e3de"
SEVERITY = [("CRITICAL", "critical", "#2a78d6"),
            ("URGENT", "urgent", "#eb6834"),
            ("ROUTINE", "routine", "#1baf7a")]
LINUX_MODES = ["cfs-flat", "cfs-nice", "fifo", "rr"]


def linux_rows(results):
    """[(label, {SEV: (avg_response, avg_wait)})] for each Linux mode that has a CSV."""
    out = []
    for mode in LINUX_MODES:
        path = os.path.join(results, f"e2_{mode}.csv")
        if not os.path.exists(path):
            continue
        acc = {s: [0.0, 0.0, 0] for s, _, _ in SEVERITY}
        with open(path, newline="") as f:
            for row in csv.DictReader(f):
                a = acc[row["severity"]]
                a[0] += float(row["response_min"])
                a[1] += float(row["wait_min"])
                a[2] += 1
        out.append((f"Linux\n{mode}", {s: (a[0] / a[2], a[1] / a[2]) for s, a in acc.items() if a[2]}))
    return out


def sim_rows(results):
    path = os.path.join(results, "e2_sim.csv")
    out = []
    if not os.path.exists(path):
        return out
    with open(path, newline="") as f:
        for row in csv.DictReader(f):
            out.append((row["label"].replace("sim-", "sim\n", 1),
                        {s: (float(row[f"{k}_avg_response"]), float(row[f"{k}_avg_wait"]))
                         for s, k, _ in SEVERITY}))
    return out


def chart(plt, groups, n_linux, which, ylabel, title, path):
    n = len(SEVERITY)
    width = 0.8 / n
    fig, ax = plt.subplots(figsize=(11, 5.2), facecolor=SURFACE)
    top = max(g[1][s][which] for g in groups for s, _, _ in SEVERITY if s in g[1]) or 1.0
    for i, (sev, _, colour) in enumerate(SEVERITY):
        xs, ys = [], []
        for x, (_, vals) in enumerate(groups):
            if sev in vals:
                xs.append(x + (i - (n - 1) / 2) * width)
                ys.append(vals[sev][which])
        ax.bar(xs, ys, width=width, color=colour, edgecolor=SURFACE, linewidth=1.5, label=sev)
        for x, y in zip(xs, ys):      # label only bars too short to read against the axis
            if y < 0.03 * top:
                ax.annotate(f"{y:.1f}", (x, y), xytext=(0, 3), textcoords="offset points",
                            ha="center", va="bottom", fontsize=8, color=INK_2)
    if 0 < n_linux < len(groups):
        ax.axvline(n_linux - 0.5, color=INK_2, linewidth=0.8, linestyle=(0, (4, 3)))
        ax.text(n_linux - 0.55, top * 1.02, "real Linux threads", ha="right", va="bottom", color=INK_2,
                fontsize=9)
        ax.text(n_linux - 0.45, top * 1.02, "simulated (schedsim, 1 team)", ha="left", va="bottom",
                color=INK_2, fontsize=9)
    ax.set_xticks(range(len(groups)))
    ax.set_xticklabels([g[0] for g in groups])
    ax.set_facecolor(SURFACE)
    ax.set_title(title, color=INK, fontsize=12, loc="left", pad=22)
    ax.set_ylabel(ylabel, color=INK_2)
    ax.tick_params(colors=INK_2, length=0)
    ax.grid(axis="y", color=GRID, linewidth=0.8)
    ax.set_axisbelow(True)
    for side in ("top", "right", "left"):
        ax.spines[side].set_visible(False)
    ax.spines["bottom"].set_color(INK_2)
    ax.set_ylim(0, top * 1.12)
    # Outside the plot area, so it can never cover a bar.
    legend = ax.legend(title="severity", frameon=False, labelcolor=INK, loc="upper left",
                       bbox_to_anchor=(1.0, 1.0))
    legend.get_title().set_color(INK_2)
    fig.tight_layout()
    fig.savefig(path, dpi=150, facecolor=SURFACE)
    plt.close(fig)


def main():
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap = argparse.ArgumentParser(description="Plot experiment E2 (Linux scheduling vs simulation).")
    ap.add_argument("--results", default=os.path.join(root, "experiments", "results"))
    ap.add_argument("--out-dir", default=os.path.join(root, "experiments", "results"))
    args = ap.parse_args()

    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except ImportError:
        print("plot_e2.py needs matplotlib: sudo apt install python3-matplotlib", file=sys.stderr)
        return 3

    linux = linux_rows(args.results)
    groups = linux + sim_rows(args.results)
    if not groups:
        sys.exit(f"no E2 results in {args.results} (run experiments/run_e2.sh first)")
    os.makedirs(args.out_dir, exist_ok=True)
    chart(plt, groups, len(linux), 0, "average response time (simulated minutes)",
          "Response time by severity: arrival to first time on the CPU (one CPU, single run)",
          os.path.join(args.out_dir, "e2_response.png"))
    chart(plt, groups, len(linux), 1, "average waiting time (simulated minutes)",
          "Waiting time by severity: time runnable but not running (one CPU, single run)",
          os.path.join(args.out_dir, "e2_wait.png"))
    for name in ("e2_response.png", "e2_wait.png"):
        print(os.path.join(args.out_dir, name))
    return 0


if __name__ == "__main__":
    sys.exit(main())
