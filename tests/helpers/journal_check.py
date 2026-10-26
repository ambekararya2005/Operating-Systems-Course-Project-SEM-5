#!/usr/bin/env python3
"""
OS concept: Crash recovery check for the write-ahead journal — Unit 5

Run after reliefd was killed (kill -9) and started again. An independent re-implementation of
the journal format (struct + zlib.crc32, nothing shared with src/io/journal.c) checks:

  acked      the allocation state reliefd acknowledged before the crash: the crashed run's
             event=recovered alloc= plus every grant / release / recovery / grant_undone /
             release_all line after it (a line is written only after its record was fsync'ed)
  journal    the state replayed from copies of snapshot.bin + journal.log taken before the
             restart (stopping at a torn or bad-CRC record, as recovery does)
  recovered  the alloc= the restarted reliefd logged in event=recovered

  --mode journal: journal (up to the last acknowledged seq) == acked, journal == recovered
  --mode none:    nothing is journaled; recovered is empty
  lost = units acknowledged before the crash that recovery cannot give back: missing from the
         journal at the last acknowledged seq (journal) or from the recovered state (none).

    journal_check.py --mode journal|none --log reliefd.log [--snapshot F --journal F]

Prints one line: acked_seq= journal_seq= acked_units= recovered_units= lost_units= ok=0|1.
Exit status 1 if ok=0 (in --mode journal, any loss or mismatch).
"""
import argparse
import os
import re
import struct
import sys
import zlib

REC = struct.Struct("<QIII4iI")     # seq type camp id units[4] crc32
CLAIM, GRANT, RELEASE, RELEASE_ALL, SNAPSHOT = 1, 2, 3, 4, 5
KV = re.compile(r'(\w+)=("[^"]*"|\S+)')


def records(path):
    """Good records of a file, stopping at a torn or bad-CRC one."""
    if not path or not os.path.exists(path):
        return
    with open(path, "rb") as f:
        data = f.read()
    for off in range(0, len(data) - REC.size + 1, REC.size):
        raw = data[off:off + REC.size]
        if zlib.crc32(raw[:-4]) != REC.unpack(raw)[-1]:
            return
        yield REC.unpack(raw)


def replay(snapshot, journal, upto=None):
    """Grants {(camp, id): units} and the last seq, from the snapshot plus the journal."""
    grants, last = {}, 0
    snap = list(records(snapshot))
    if snap:
        if snap[0][1] != SNAPSHOT or len(snap) != snap[0][3] + 1:
            sys.exit("journal_check: corrupt snapshot")
        last = snap[0][0]
        for _, t, camp, rid, *u, _crc in snap[1:]:
            if t == GRANT:
                grants[(camp, rid)] = u
    for seq, t, camp, rid, *u, _crc in records(journal):
        if seq <= last:
            continue
        if seq != last + 1 or (upto is not None and seq > upto):
            break
        if t == GRANT:
            grants[(camp, rid)] = u
        elif t == RELEASE:
            grants.pop((camp, rid), None)
        elif t == RELEASE_ALL:
            grants = {k: v for k, v in grants.items() if k[0] != camp}
        last = seq
    return grants, last


def alloc_of(grants):
    a = {}
    for (camp, _), u in grants.items():
        a[camp] = [x + y for x, y in zip(a.get(camp, [0] * 4), u)]
    return {c: v for c, v in a.items() if any(v)}


def parse_alloc(s):
    """'2:0,0,7,7/4:3,2,6,0' -> {2: [0,0,7,7], 4: [...]}."""
    if not s or s == "-":
        return {}
    return {int(c): [int(x) for x in v.split(",")] for c, v in (p.split(":") for p in s.split("/"))}


def acked_state(lines):
    """The crashed run's acknowledged alloc and the last seq it logged."""
    alloc, seq = {}, 0
    for kv in lines:
        ev = kv.get("event")
        if ev == "recovered":
            alloc = parse_alloc(kv.get("alloc"))
            seq = int(kv.get("last_seq", 0))
            continue
        if ev not in ("grant", "release", "recovery", "grant_undone", "release_all"):
            continue
        seq = max(seq, int(kv.get("seq", 0)))
        camp = int(kv.get("camp", kv.get("victim", "camp0").replace("camp", "")))
        if ev == "release_all":
            alloc.pop(camp, None)
            continue
        v = [int(x) for x in kv["vec"].split(",")]
        sign = 1 if ev == "grant" else -1
        alloc[camp] = [a + sign * b for a, b in zip(alloc.get(camp, [0] * 4), v)]
    return {c: v for c, v in alloc.items() if any(v)}, seq


def units(alloc):
    return sum(sum(v) for v in alloc.values())


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=("journal", "none"), required=True)
    ap.add_argument("--log", required=True)
    ap.add_argument("--snapshot")
    ap.add_argument("--journal")
    args = ap.parse_args()

    with open(args.log) as f:
        lines = [dict((k, v.strip('"')) for k, v in KV.findall(line)) for line in f if "comp=reliefd" in line]
    starts = [i for i, kv in enumerate(lines) if kv.get("event") == "daemon_start"]
    if len(starts) < 2:
        sys.exit("journal_check: need the crashed run and the restarted run in the log")
    acked, acked_seq = acked_state(lines[starts[-2]:starts[-1]])
    rec = next((kv for kv in lines[starts[-1]:] if kv.get("event") == "recovered"), None)
    if rec is None:
        sys.exit("journal_check: the restarted reliefd logged no event=recovered")
    recovered = parse_alloc(rec.get("alloc"))

    # What recovery can give back as of the last acknowledged change: with a journal, its state
    # at that seq (later records, e.g. a release fsync'ed just before the kill, are legitimate);
    # without one, the recovered state.
    ok, jseq, base = True, 0, recovered
    if args.mode == "journal":
        full, jseq = replay(args.snapshot, args.journal)
        at_ack, _ = replay(args.snapshot, args.journal, upto=acked_seq)
        base = alloc_of(at_ack)
        ok = jseq >= acked_seq and base == acked and alloc_of(full) == recovered and rec.get("journal") == "on"
    else:
        ok = rec.get("journal") == "off" and not recovered
    lost = sum(max(0, a - b) for c, v in acked.items() for a, b in zip(v, base.get(c, [0] * 4)))
    ok = ok and (lost == 0 or args.mode == "none")
    print(f"acked_seq={acked_seq} journal_seq={jseq} acked_units={units(acked)} "
          f"recovered_units={units(recovered)} lost_units={lost} ok={int(ok)}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
