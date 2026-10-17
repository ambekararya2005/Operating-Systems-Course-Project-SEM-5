#!/usr/bin/env bash
# OS concept: CPU scheduling criteria — experiment E1, comparing scheduling policies — Unit 2
#
# E1: triage scheduling comparison. For each seed, generate a 500-request scenario with a
# rush hour (arrivals 10x denser in minutes 60-120) and run every policy variant through
# build/schedsim on 3 teams. One CSV row per (variant, seed) goes to experiments/results/e1.csv.
#
# E1b: load sweep. The same 500 requests per seed (identical service times: the generator draws
# the same random numbers per request whatever the duration) are spread over a duration chosen
# so the offered load (total work / (teams x duration)) is 30, 45, 60, 75, 90 and 105%. E1b uses
# the generator's default arrival profile (rate 3x higher in the middle third), not E1's rush
# hour: a 10x rush hour overloads the teams at every average load, so MLFQ starves requests
# even at 30% (7 on average; see docs/EXPERIMENTS.md) and there is no threshold to find.
# prio-preempt, mlfq and mlfq-floor run at each load; rows go to experiments/results/e1b.csv
# with target_load and offered_load columns in front.
#
#   usage: run_e1.sh [-s "1 2 3 4 5"] [-d DURATION] [-n TEAMS]
#     -s  seeds (default "1 2 3 4 5"); the seed drives both the scenario and schedsim's
#         same-minute tie order, so each seed is a different but reproducible workload
#     -d  scenario length in simulated minutes (default 3000: about 90% average load on 3
#         teams, so the rush hour creates a backlog that later drains)
#     -n  rescue teams (default 3)
#   env: BIN_DIR (default build), RESULTS (default experiments/results)
#
# Deterministic: the same arguments always produce a byte-identical e1.csv.
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN="$ROOT/${BIN_DIR:-build}"
RESULTS=${RESULTS:-$ROOT/experiments/results}
PROG=${0##*/}

seeds="1 2 3 4 5"
duration=3000
teams=3
while getopts ":s:d:n:h" opt; do
    case "$opt" in
        s) seeds=$OPTARG ;;
        d) duration=$OPTARG ;;
        n) teams=$OPTARG ;;
        h) sed -n 's/^#   //p; s/^#     /  /p' "${BASH_SOURCE[0]}"; exit 0 ;;
        :) echo "$PROG: -$OPTARG needs a value" >&2; exit 2 ;;
        *) echo "$PROG: unknown option -$OPTARG" >&2; exit 2 ;;
    esac
done
shift $((OPTIND - 1))
(( $# == 0 )) || { echo "$PROG: unexpected argument '$1'" >&2; exit 2; }
[[ "$duration" =~ ^[0-9]+$ && "$teams" =~ ^[0-9]+$ && "$seeds" =~ ^[0-9\ ]+$ ]] \
    || { echo "$PROG: -s, -d and -n need whole numbers" >&2; exit 2; }
[[ -x "$BIN/schedsim" ]] || { echo "$PROG: $BIN/schedsim not built; run 'make'" >&2; exit 1; }

ROWS=500
RUSH=60-120
# label|schedsim options
VARIANTS=(
    "fcfs|--policy fcfs"
    "sjf|--policy sjf"
    "srtf|--policy srtf"
    "rr-q5|--policy rr --quantum 5"
    "rr-q15|--policy rr --quantum 15"
    "prio|--policy prio --preemptive off"
    "prio-preempt|--policy prio --preemptive on"
    "mlfq|--policy mlfq --age 60"
    "mlfq-floor|--policy mlfq-floor --age 60"
)

mkdir -p "$RESULTS"
OUT="$RESULTS/e1.csv"
rm -f "$OUT"
echo "E1: $ROWS requests, rush hour $RUSH, duration $duration min, $teams teams, seeds: $seeds"
for seed in $seeds; do
    scenario="$RESULTS/e1_scenario_s$seed.csv"
    python3 -I "$ROOT/experiments/gen_scenario.py" --rows "$ROWS" --seed "$seed" \
        --duration "$duration" --rush "$RUSH" -o "$scenario"
    for v in "${VARIANTS[@]}"; do
        label=${v%%|*}
        read -r -a opts <<< "${v#*|}"
        # Promotions are logged on stderr one per line; the summary row has the count.
        "$BIN/schedsim" "${opts[@]}" --scenario "$scenario" --teams "$teams" --seed "$seed" \
            --summary-csv "$OUT" --label "$label" > /dev/null 2>&1 \
            || { echo "$PROG: schedsim failed for $label seed $seed" >&2; exit 1; }
    done
    echo "  seed $seed: ${#VARIANTS[@]} variants done"
done

# Compact summary: mean over seeds per variant.
awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
    { l = $col["label"]; if (!(l in n)) order[++k] = l; n[l]++
      w[l] += $col["avg_wait"]; cr[l] += $col["critical_avg_response"]
      st[l] += $col["starved"]; mx[l] += $col["max_wait"] }
    END {
        printf "\n%-13s %10s %15s %9s %10s   (means over seeds, simulated minutes)\n",
               "variant", "avg wait", "crit response", "starved", "max wait"
        for (i = 1; i <= k; i++) { l = order[i]
            printf "%-13s %10.1f %15.1f %9.1f %10.1f\n", l, w[l] / n[l], cr[l] / n[l], st[l] / n[l], mx[l] / n[l] }
    }' "$OUT"
echo
echo "wrote $OUT ($(($(wc -l < "$OUT") - 1)) runs)"

# ---- E1b: load sweep --------------------------------------------------------------------
LOADS="30 45 60 75 90 105"
SWEEP=(
    "prio-preempt|--policy prio --preemptive on"
    "mlfq|--policy mlfq --age 60"
    "mlfq-floor|--policy mlfq-floor --age 60"
)
OUTB="$RESULTS/e1b.csv"
rm -f "$OUTB"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-e1b.XXXXXX")
trap 'rm -rf "$TMP"' EXIT
echo
echo "E1b: load sweep at $LOADS % offered load, $ROWS requests, default arrival profile, $teams teams, seeds: $seeds"
for seed in $seeds; do
    # Total work of this seed's requests (independent of the duration).
    python3 -I "$ROOT/experiments/gen_scenario.py" --rows "$ROWS" --seed "$seed" --duration 3000 \
        -o "$TMP/base.csv"
    work=$(awk -F, 'NR > 1 { w += $6 } END { print w }' "$TMP/base.csv")
    for load in $LOADS; do
        dur=$(( (work * 100 + teams * load / 2) / (teams * load) ))
        scenario="$TMP/l${load}_s${seed}.csv"
        python3 -I "$ROOT/experiments/gen_scenario.py" --rows "$ROWS" --seed "$seed" --duration "$dur" \
            -o "$scenario"
        w2=$(awk -F, 'NR > 1 { w += $6 } END { print w }' "$scenario")
        [[ "$w2" == "$work" ]] || { echo "$PROG: work changed with duration ($w2 vs $work)" >&2; exit 1; }
        offered=$(awk -v w="$work" -v t="$teams" -v d="$dur" 'BEGIN { printf "%.1f", 100 * w / (t * d) }')
        for v in "${SWEEP[@]}"; do
            label=${v%%|*}
            read -r -a opts <<< "${v#*|}"
            rm -f "$TMP/row.csv"
            "$BIN/schedsim" "${opts[@]}" --scenario "$scenario" --teams "$teams" --seed "$seed" \
                --summary-csv "$TMP/row.csv" --label "$label" > /dev/null 2>&1 \
                || { echo "$PROG: schedsim failed for $label load $load seed $seed" >&2; exit 1; }
            [[ -s "$OUTB" ]] || sed -n '1s/^/target_load,offered_load,/p' "$TMP/row.csv" > "$OUTB"
            sed -n "2s/^/$load,$offered,/p" "$TMP/row.csv" >> "$OUTB"
        done
    done
    echo "  seed $seed: ${#SWEEP[@]} variants x $(wc -w <<< "$LOADS") loads done"
done

awk -F, 'NR == 1 { for (i = 1; i <= NF; i++) col[$i] = i; next }
    { k = $col["target_load"] SUBSEP $col["label"]; n[k]++
      st[k] += $col["starved"]; cr[k] += $col["critical_avg_response"]
      if ($col["starved"] > mx[k]) mx[k] = $col["starved"]
      if (!($col["target_load"] in seen)) { seen[$col["target_load"]] = 1; loads[++nl] = $col["target_load"] }
      if (!($col["label"] in seenl)) { seenl[$col["label"]] = 1; labels[++nv] = $col["label"] } }
    END {
        printf "\n%-6s", "load"
        for (v = 1; v <= nv; v++) printf " %24s", labels[v] " starved/resp"
        printf "   (mean over seeds; starved max in brackets)\n"
        for (i = 1; i <= nl; i++) { L = loads[i]; printf "%4s%% ", L
            for (v = 1; v <= nv; v++) { k = L SUBSEP labels[v]
                printf " %10.1f [%3d] / %6.1f", st[k] / n[k], mx[k], cr[k] / n[k] }
            printf "\n" }
    }' "$OUTB"
echo
echo "wrote $OUTB ($(($(wc -l < "$OUTB") - 1)) runs)"
