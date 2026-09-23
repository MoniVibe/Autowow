#!/usr/bin/env python3
"""Fold AutoWoW ledger `combat` events (combat telemetry cv=1) into per-bot / per-class / per-level-band
combat efficiency tables. Stdlib only, read-only, idempotent.

Usage:
  python combat-reduce.py ledger.log [more.log ...] [--out-dir DIR] [--exclude 101,112,...] [--profile NAME]
  python combat-reduce.py --selftest

Input: the autowow.ledger log (worldserver AutoWow.Ledger.Enable=1 plus AutoWow.CombatTelemetry.Enable=1).
Each `combat` line carries CUMULATIVE per-login counters for one bot (quest 0):
  cv, cls, wall_ms, combat_ms, dead_ms, starved_ms, fights, kills, deaths, dmg, taken, heal,
  casts, gcd_casts, dot_skips, ttk_n, ttk_drop, ttk=[[ms, dlvl], ...]  (ttk: kills since the previous line)
`died` lines (killer kind/id/level) are joined for deaths-by-killer.

Folding law (sterile sums): per (run, bot) the delta of every cumulative counter between consecutive
lines is attributed to the level band of the later line. A counter that decreases means a new login
session (relog / server restart): the baseline resets to zero, so no delta is lost or double counted.
Rates are derived from summed deltas per cell; per-bot ratios are never averaged.

Outputs (in --out-dir): combat-summary.md, combat-cells.jsonl.
"""
import argparse
import json
import os
import sys
import tempfile
from collections import Counter, defaultdict

COUNTERS = ["wall_ms", "combat_ms", "dead_ms", "starved_ms", "fights", "kills", "deaths", "dmg", "taken",
            "heal", "casts", "gcd_casts", "dot_skips", "ttk_n", "ttk_drop"]
CLASSES = {1: "warrior", 2: "paladin", 3: "hunter", 4: "rogue", 5: "priest", 6: "deathknight", 7: "shaman",
           8: "mage", 9: "warlock", 11: "druid"}
BANDS = [(1, 20), (21, 40), (41, 60), (61, 70), (71, 80)]
GCD_MS = 1500


def band(level):
    for lo, hi in BANDS:
        if lo <= level <= hi:
            return "%d-%d" % (lo, hi)
    return "other"


def parse_line(line):
    i = line.find("{")
    if i < 0:
        return None
    try:
        ev = json.loads(line[i:])
    except ValueError:
        return None
    return ev if isinstance(ev, dict) and "ev" in ev and "bot" in ev and "ms" in ev else None


def read_events(paths):
    out = []
    seq = 0
    for p in paths:
        with open(p, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                ev = parse_line(line)
                if ev is not None and ev["ev"] in ("combat", "died"):
                    out.append((ev.get("run", ""), ev["bot"], ev["ms"], seq, ev))
                    seq += 1
    out.sort(key=lambda t: t[:4])
    return [t[4] for t in out]


def pct(sorted_vals, q):
    if not sorted_vals:
        return None
    k = max(0, min(len(sorted_vals) - 1, int(-(-q * len(sorted_vals) // 1)) - 1))  # nearest-rank
    return sorted_vals[k]


def new_cell():
    c = {k: 0 for k in COUNTERS}
    c["ttk"] = []
    c["killers"] = Counter()
    c["bots"] = set()
    return c


def fold(events, exclude):
    prev = {}                        # (run, bot) -> last cumulative counters
    cells = defaultdict(new_cell)    # (kind, key) -> cell
    for ev in events:
        run, bot = ev.get("run", ""), ev["bot"]
        if bot in exclude:
            continue
        cls = CLASSES.get(ev.get("cls"), "cls%s" % ev.get("cls")) if ev["ev"] == "combat" else None
        if ev["ev"] == "died":
            last = prev.get((run, bot))
            keys = [("bot", str(bot))]
            if last is not None:
                keys += [("class", last["_cls"]), ("band", band(ev.get("lvl", 0))),
                         ("class_band", "%s/%s" % (last["_cls"], band(ev.get("lvl", 0))))]
            k = ev.get("killer", "unknown")
            if k == "creature":
                k = "creature:%s(L%s,d%+d)" % (ev.get("kid"), ev.get("klvl"), ev.get("klvl", 0) - ev.get("lvl", 0))
            for key in keys:
                cells[key]["killers"][k] += 1
            continue
        if ev.get("cv") != 1:
            continue  # unknown combat schema: ignore, never guess
        cur = {k: int(ev.get(k, 0)) for k in COUNTERS}
        base = prev.get((run, bot))
        if base is None or any(cur[k] < base[k] for k in COUNTERS):
            base = {k: 0 for k in COUNTERS}
        delta = {k: cur[k] - base[k] for k in COUNTERS}
        b = band(ev.get("lvl", 0))
        for key in (("bot", str(bot)), ("class", cls), ("band", b), ("class_band", "%s/%s" % (cls, b)),
                    ("total", "all")):
            cell = cells[key]
            for k in COUNTERS:
                cell[k] += delta[k]
            cell["ttk"].extend(int(s[0]) for s in ev.get("ttk", []))
            cell["bots"].add(bot)
        cur["_cls"] = cls
        prev[(run, bot)] = cur
    return cells


def derive(kind, key, c, profile):
    wall_s = c["wall_ms"] / 1000.0
    combat_s = c["combat_ms"] / 1000.0
    ttk = sorted(c["ttk"])
    row = {"profile": profile, "kind": kind, "key": key, "bots": len(c["bots"])}
    row.update({k: c[k] for k in COUNTERS})
    row["dps_combat"] = round(c["dmg"] / combat_s, 1) if combat_s else None
    row["dps_sustained"] = round(c["dmg"] / wall_s, 1) if wall_s else None
    row["ttk_median_ms"] = pct(ttk, 0.5)
    row["ttk_p90_ms"] = pct(ttk, 0.9)
    row["ttk_samples"] = len(ttk)
    row["deaths_per_hour"] = round(c["deaths"] / (wall_s / 3600.0), 2) if wall_s else None
    row["combat_share"] = round(c["combat_ms"] / c["wall_ms"], 3) if c["wall_ms"] else None
    row["dead_share"] = round(c["dead_ms"] / c["wall_ms"], 3) if c["wall_ms"] else None
    # Downtime = wall - combat - dead (travel, rest, questing, idling). Sterile: the three shares sum to 1.
    row["downtime_share"] = (round(1.0 - row["combat_share"] - row["dead_share"], 3)
                             if c["wall_ms"] else None)
    row["starved_share"] = round(c["starved_ms"] / c["combat_ms"], 3) if c["combat_ms"] else None
    row["gcd_util_est"] = round(c["gcd_casts"] * GCD_MS / c["combat_ms"], 3) if c["combat_ms"] else None
    row["killers"] = dict(sorted(c["killers"].items()))
    return row


def summarize(rows, profile):
    lines = ["# Combat reduce (%s)" % profile, "",
             "Deltas of cumulative `combat` lines (cv=1); rates from summed deltas. dps_combat = dmg / in-combat s;",
             "dps_sustained = dmg / wall s. TTK = bot's first damage on a creature to its killing blow (bot or pet).",
             "gcd_util_est = gcd_casts x 1.5 s / combat s (upper-bound style estimate, ignores haste).", ""]
    for kind in ("total", "band", "class", "class_band", "bot"):
        sel = [r for r in rows if r["kind"] == kind]
        if not sel:
            continue
        lines += ["## by %s" % kind, "",
                  "| %s | bots | wall h | dps_combat | dps_sust | TTK med s | TTK p90 s | kills | deaths/h | combat | dead | downtime | starved | gcd | dot_skips |" % kind,
                  "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in sel:
            f = lambda v, d=1: "-" if v is None else ("%.*f" % (d, v))
            lines.append("| %s | %d | %.2f | %s | %s | %s | %s | %d | %s | %s | %s | %s | %s | %s | %d |" % (
                r["key"], r["bots"], r["wall_ms"] / 3.6e6, f(r["dps_combat"]), f(r["dps_sustained"]),
                f(None if r["ttk_median_ms"] is None else r["ttk_median_ms"] / 1000.0),
                f(None if r["ttk_p90_ms"] is None else r["ttk_p90_ms"] / 1000.0), r["kills"],
                f(r["deaths_per_hour"], 2), f(r["combat_share"], 3), f(r["dead_share"], 3),
                f(r["downtime_share"], 3), f(r["starved_share"], 3), f(r["gcd_util_est"], 3), r["dot_skips"]))
        lines.append("")
    killers = [r for r in rows if r["kind"] == "bot" and r["killers"]]
    if killers:
        lines += ["## deaths by killer (per bot)", "", "| bot | killer | deaths |", "|---|---|---|"]
        for r in killers:
            for k, n in sorted(r["killers"].items(), key=lambda kv: (-kv[1], kv[0])):
                lines.append("| %s | %s | %d |" % (r["key"], k, n))
        lines.append("")
    return "\n".join(lines)


def run(paths, out_dir, exclude, profile):
    events = read_events(paths)
    cells = fold(events, exclude)
    order = {"total": 0, "band": 1, "class": 2, "class_band": 3, "bot": 4}
    keyf = lambda kv: (order[kv[0][0]], (int(kv[0][1]) if kv[0][0] == "bot" else 0), kv[0][1])
    rows = [derive(kind, key, c, profile) for (kind, key), c in sorted(cells.items(), key=keyf)]
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "combat-cells.jsonl"), "w", encoding="ascii", newline="\n") as fh:
        for r in rows:
            fh.write(json.dumps(r, sort_keys=True) + "\n")
    with open(os.path.join(out_dir, "combat-summary.md"), "w", encoding="ascii", newline="\n") as fh:
        fh.write(summarize(rows, profile) + "\n")
    return rows


def selftest():
    def line(ms, bot, lvl, cls=8, run="t", **c):
        obj = {"v": 1, "run": run, "ms": ms, "ev": "combat", "bot": bot, "team": 0, "lvl": lvl, "quest": 0,
               "map": 0, "zone": 12, "x": 0, "y": 0, "c": [0] * 4, "i": [0] * 6, "reason": "", "phase": "",
               "cv": 1, "cls": cls}
        obj.update({k: 0 for k in COUNTERS})
        obj["ttk"] = []
        obj.update(c)
        return "2026-09-23 01:00:00 " + json.dumps(obj, separators=(",", ":"))

    def died(ms, bot, lvl, kid, klvl, run="t"):
        obj = {"v": 1, "run": run, "ms": ms, "ev": "died", "bot": bot, "lvl": lvl, "quest": 0,
               "killer": "creature", "kid": kid, "klvl": klvl}
        return "2026-09-23 01:00:00 " + json.dumps(obj)

    log = [
        line(60000, 1, 10, wall_ms=60000, combat_ms=20000, dmg=400, kills=2, fights=2, ttk_n=2,
             ttk=[[8000, 0], [12000, 1]], gcd_casts=10),
        line(120000, 1, 10, wall_ms=120000, combat_ms=50000, dmg=1000, kills=4, fights=4, ttk_n=4, deaths=1,
             dead_ms=10000, ttk=[[9000, 0], [11000, 2]], gcd_casts=20, starved_ms=5000),
        died(100000, 1, 10, 3382, 13),
        # relog: counters drop -> new session baseline zero (no negative delta)
        line(180000, 1, 11, wall_ms=30000, combat_ms=10000, dmg=300, kills=1, fights=1, ttk_n=1, ttk=[[7000, 0]]),
        line(60000, 2, 30, cls=1, wall_ms=60000, combat_ms=30000, dmg=3000, kills=3, fights=3, ttk_n=3,
             ttk=[[5000, -1], [6000, 0], [10000, 1]], dot_skips=4),
        line(60000, 9, 30, cls=1, wall_ms=60000, dmg=99999),     # excluded
        "garbage",
        '{"v":1,"run":"t","ms":1,"ev":"combat","bot":3,"lvl":5,"cv":2,"dmg":5}',  # unknown schema -> ignored
    ]
    with tempfile.TemporaryDirectory() as td:
        lp = os.path.join(td, "ledger.log")
        with open(lp, "w", encoding="ascii") as fh:
            fh.write("\n".join(log) + "\n")
        rows = run([lp], td, {9}, "selftest")
        by = {(r["kind"], r["key"]): r for r in rows}
        tot = by[("total", "all")]
        assert tot["dmg"] == 1000 + 300 + 3000, tot["dmg"]
        assert tot["wall_ms"] == 120000 + 30000 + 60000
        assert tot["kills"] == 8 and tot["ttk_n"] == 8 and tot["ttk_samples"] == 8
        b1 = by[("bot", "1")]
        assert b1["dmg"] == 1300 and b1["combat_ms"] == 60000 and b1["deaths"] == 1
        assert b1["dps_combat"] == round(1300 / 60.0, 1)
        assert b1["killers"] == {"creature:3382(L13,d+3)": 1}, b1["killers"]
        assert b1["ttk_median_ms"] == 9000 and b1["ttk_p90_ms"] == 12000, (b1["ttk_median_ms"], b1["ttk_p90_ms"])
        assert abs(b1["combat_share"] + b1["dead_share"] + b1["downtime_share"] - 1.0) < 0.002
        assert b1["starved_share"] == round(5000 / 60000, 3)
        assert b1["gcd_util_est"] == round(20 * 1500 / 60000, 3)
        # band split: bot 1's lvl-10 and lvl-11 deltas both land in 1-20; bot 2 in 21-40
        assert by[("band", "1-20")]["dmg"] == 1300 and by[("band", "21-40")]["dmg"] == 3000
        assert by[("class", "warrior")]["dot_skips"] == 4 and ("bot", "9") not in by and ("bot", "3") not in by
        # sterile: per-bot dmg sums to total
        assert sum(r["dmg"] for r in rows if r["kind"] == "bot") == tot["dmg"]
        first = open(os.path.join(td, "combat-cells.jsonl"), "rb").read()
        run([lp], td, {9}, "selftest")
        assert open(os.path.join(td, "combat-cells.jsonl"), "rb").read() == first  # idempotent
        md = open(os.path.join(td, "combat-summary.md"), encoding="ascii").read()
        assert "## by class_band" in md and "creature:3382(L13,d+3)" in md
    print("selftest OK")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--out-dir", default=".")
    ap.add_argument("--exclude", default="", help="comma-separated bot guids to drop (scouts, fixtures, oracle)")
    ap.add_argument("--profile", default="", help="profile label stamped on every row (run/segment/arm)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.logs:
        ap.error("no ledger logs given")
    exclude = {int(x) for x in args.exclude.split(",") if x.strip()}
    rows = run(args.logs, args.out_dir, exclude, args.profile or "unnamed")
    print("cells=%d out=%s" % (len(rows), os.path.abspath(args.out_dir)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
