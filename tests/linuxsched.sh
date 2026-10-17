#!/usr/bin/env bash
# OS concept: Linux scheduling classes (CFS/EEVDF + nice, SCHED_FIFO, SCHED_RR) — Unit 2
#
# Tests build/linuxsched on a tiny scenario (about 0.1 s of CPU):
#   1. cfs-flat: every thread runs on the one pinned CPU and burns its full CPU budget
#   2. cfs-nice: nice spacing is 5 per severity step (-5/0/+5, or 0/+5/+10 unprivileged)
#   3. fifo/rr: with CAP_SYS_NICE, RT priorities 30/20/10 and CRITICAL first; without it,
#      a clear message and exit 3 (skipped)
#   4. safety: a scenario over the CPU budget is refused (exit 2); the watchdog aborts a
#      run that takes too long (exit 4); usage errors exit 2
#   BIN_DIR       directory holding the binaries (default: build)
#   RELIEF_WRAP   optional command prefix, e.g. valgrind
set -euo pipefail

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
BIN=$(cd "$ROOT/${BIN_DIR:-build}" && pwd)
read -r -a WRAP <<< "${RELIEF_WRAP:-}"
TMP=$(mktemp -d "${TMPDIR:-/tmp}/reliefos-test.XXXXXX")
trap 'rm -rf "$TMP"' EXIT

fail() { echo "FAIL: $*" >&2; exit 1; }
pass() { echo "ok   $*"; }
linuxsched() { "${WRAP[@]}" "$BIN/linuxsched" "$@"; }
col() { awk -F, -v name="$2" 'NR == 1 { for (i = 1; i <= NF; i++) if ($i == name) c = i; next } { print $c }' "$1"; }

cat > "$TMP/tiny.csv" <<'EOF'
arrival_min,camp_id,severity,resource,quantity,service_time
0,1,ROUTINE,FOOD_KIT,10,8
1,2,URGENT,MED_KIT,5,6
2,3,CRITICAL,RESCUE,3,7
3,1,ROUTINE,HYGIENE_KIT,10,5
4,2,CRITICAL,RESCUE,2,6
5,3,URGENT,TRUCK,1,8
6,1,ROUTINE,FOOD_KIT,10,5
7,2,CRITICAL,BOAT,1,6
EOF

# 1. cfs-flat
out=$(linuxsched --mode cfs-flat --scenario "$TMP/tiny.csv" --scale-ms 2 --out "$TMP/flat.csv") \
    || fail "cfs-flat run failed"
[[ $(awk 'NR > 1' "$TMP/flat.csv" | wc -l) -eq 8 ]] || fail "cfs-flat: expected 8 thread rows"
[[ $(col "$TMP/flat.csv" cpu | sort -u | wc -l) -eq 1 ]] || fail "threads ran on more than one CPU (not pinned)"
grep -q 'threads on another CPU: 0; nice failures: 0' <<< "$out" || fail "cfs-flat report: $out"
awk -F, 'NR > 1 && ($14 < 0.98 * $13 || $12 < $11 || $11 < $10) { bad++ } END { exit bad > 0 }' "$TMP/flat.csv" \
    || fail "a thread did not burn its CPU budget, or finish < start, or start < created"
[[ $(col "$TMP/flat.csv" nice | sort -u) == 0 ]] || fail "cfs-flat must use nice 0 for all"
pass "cfs-flat: 8 threads on one pinned CPU, each burned its full CPU-time budget"

# 2. cfs-nice: spacing of 5 per severity step.
linuxsched --mode cfs-nice --scenario "$TMP/tiny.csv" --scale-ms 2 --out "$TMP/nice.csv" > "$TMP/nice.out" \
    || fail "cfs-nice run failed"
read -r nc nu nr < <(awk -F, 'NR > 1 { n[$4] = $7 } END { print n["CRITICAL"], n["URGENT"], n["ROUTINE"] }' "$TMP/nice.csv")
[[ $((nu - nc)) -eq 5 && $((nr - nu)) -eq 5 ]] || fail "nice spacing should be 5: CRITICAL $nc URGENT $nu ROUTINE $nr"
grep -q 'nice failures: 0' "$TMP/nice.out" || fail "setpriority failed for some thread"
pass "cfs-nice: nice CRITICAL/URGENT/ROUTINE = $nc/$nu/$nr (per-thread setpriority on the TID)"

# 3. Real-time modes.
rc=0; linuxsched --check-rt || rc=$?
if [[ $rc -eq 0 ]]; then
    for m in fifo rr; do
        linuxsched --mode "$m" --scenario "$TMP/tiny.csv" --scale-ms 2 --out "$TMP/$m.csv" >/dev/null \
            || fail "$m run failed"
        read -r pc pu pr < <(awk -F, 'NR > 1 { p[$4] = $8 } END { print p["CRITICAL"], p["URGENT"], p["ROUTINE"] }' "$TMP/$m.csv")
        [[ "$pc $pu $pr" == "30 20 10" ]] || fail "$m priorities should be 30/20/10, got $pc/$pu/$pr"
    done
    pass "fifo/rr: RT priorities 30/20/10 applied (CAP_SYS_NICE present)"
elif [[ $rc -eq 3 ]]; then
    rc=0; msg=$(linuxsched --mode fifo --scenario "$TMP/tiny.csv" 2>&1) || rc=$?
    if [[ $rc -ne 3 ]] || ! grep -q 'setcap cap_sys_nice+ep' <<< "$msg"; then
        fail "fifo without privilege: rc=$rc msg=$msg"
    fi
    pass "fifo/rr skipped: no CAP_SYS_NICE; linuxsched exits 3 with the setcap hint"
else
    fail "--check-rt exited $rc"
fi

# 4. Safety and usage.
rc=0; linuxsched --mode cfs-flat --scenario "$TMP/tiny.csv" --max-cpu-s 0.01 >/dev/null 2>&1 || rc=$?
[[ $rc -eq 2 ]] || fail "over-budget scenario should exit 2, got $rc"
printf 'arrival_min,camp_id,severity,resource,quantity,service_time\n0,1,ROUTINE,FOOD_KIT,1,1000\n' > "$TMP/long.csv"
rc=0; msg=$(linuxsched --mode cfs-flat --scenario "$TMP/long.csv" --scale-ms 3 --watchdog-s 1 --out "$TMP/long_out.csv" 2>&1) || rc=$?
if [[ $rc -ne 4 ]] || ! grep -q 'WATCHDOG fired' <<< "$msg"; then
    fail "watchdog should abort a 3 s run after 1 s (rc=$rc)"
fi
for bad in "--mode nope --scenario $TMP/tiny.csv" "--mode cfs-flat" "--mode cfs-flat --scenario $TMP/tiny.csv --scale-ms 0"; do
    rc=0; read -r -a a <<< "$bad"; linuxsched "${a[@]}" >/dev/null 2>&1 || rc=$?
    [[ $rc -eq 2 ]] || fail "linuxsched $bad should exit 2, got $rc"
done
pass "safety: over-budget scenario refused (exit 2), watchdog aborted a runaway run (exit 4), usage errors exit 2"

echo "all linuxsched tests passed"
