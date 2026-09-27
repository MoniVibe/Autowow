#!/usr/bin/env python3
"""Completability table from the AutoWoW dungeon probe ledger rows (`dprobe`, event 24, schema v1).

Input: worldserver ledger logs (logger `autowow.ledger`); each line may carry a log prefix, the JSON object
starting at '{"v":' is extracted. Only ev == "dprobe" rows are read:
  run    one per finished probe run: dmap, plvl, bosses/total, wipes, revives, stucks, deaths, dur_ms,
         end (completed|abandoned|stuck|timeout|wiped_out|enter_failed|prepare_failed), party, rid
  stuck  one per stuck event: smap, sx, sy, sz (leader, integer yards), next (encounter index), boss (credit entry)

Output (markdown, stdout or --out): one table row per (dungeon map, probe level):
  dungeon, level, runs, completed, best bosses x/y, wipes (total), median time (all runs), ends, stuck points
Stuck points are bucketed to BUCKET_YARDS and listed most frequent first (count x (x,y,z) next=enc).

Usage: python dungeon-probe-report.py LOG [LOG ...] [--out FILE] [--party NAME] [--run RUNID]
       python dungeon-probe-report.py --selftest
"""

import argparse
import json
import statistics
import sys
from collections import Counter, defaultdict

SCHEMA_VERSION = 1
BUCKET_YARDS = 20
TOP_STUCK = 3
NAMES = {
    389: "Ragefire Chasm", 43: "Wailing Caverns", 36: "The Deadmines", 33: "Shadowfang Keep",
    48: "Blackfathom Deeps", 34: "The Stockade", 90: "Gnomeregan", 47: "Razorfen Kraul", 189: "Scarlet Monastery",
    129: "Razorfen Downs", 70: "Uldaman", 209: "Zul'Farrak", 349: "Maraudon", 109: "Sunken Temple",
    230: "Blackrock Depths", 229: "Blackrock Spire", 429: "Dire Maul", 329: "Stratholme", 289: "Scholomance",
}
_DECODER = json.JSONDecoder()


def parse_line(line):
    i = line.find('{"v":')
    if i < 0:
        return None
    try:
        obj, _ = _DECODER.raw_decode(line, i)
    except ValueError:
        return None
    if obj.get("v") != SCHEMA_VERSION or obj.get("ev") != "dprobe":
        return None
    return obj


def read_rows(lines, party=None, run=None):
    rows = []
    for line in lines:
        ev = parse_line(line)
        if ev is None or (party and ev.get("party") != party) or (run and ev.get("run") != run):
            continue
        rows.append(ev)
    rows.sort(key=lambda e: (e.get("run", ""), e.get("ms", 0), e.get("rid", 0)))
    return rows


def fmt_ms(ms):
    s = int(ms // 1000)
    return "%d:%02d" % (s // 60, s % 60)


def reduce_rows(rows):
    """Return table rows (dicts) sorted by (first-seen queue order, level)."""
    runs = defaultdict(list)
    stucks = defaultdict(Counter)
    order = {}
    for e in rows:
        key = (e.get("dmap", 0), e.get("plvl", 0))
        order.setdefault(key, len(order))
        if e.get("reason") == "run":
            runs[key].append(e)
        elif e.get("reason") == "stuck":
            b = BUCKET_YARDS
            point = (e.get("sx", 0) // b * b, e.get("sy", 0) // b * b, e.get("sz", 0) // b * b, e.get("next", -1))
            stucks[key][point] += 1
    table = []
    for key in sorted(set(runs) | set(stucks), key=lambda k: (order[k], k)):
        rs = runs.get(key, [])
        best = max(rs, key=lambda r: (r.get("bosses", 0), -r.get("dur_ms", 0)), default=None)
        points = sorted(stucks[key].items(), key=lambda kv: (-kv[1], kv[0]))[:TOP_STUCK]
        table.append({
            "dmap": key[0],
            "level": key[1],
            "runs": len(rs),
            "completed": sum(1 for r in rs if r.get("end") == "completed"),
            "best": "%d/%d" % (best.get("bosses", 0), best.get("total", 0)) if best else "-",
            "wipes": sum(r.get("wipes", 0) for r in rs),
            "median_ms": statistics.median([r.get("dur_ms", 0) for r in rs]) if rs else None,
            "ends": dict(sorted(Counter(r.get("end", "") for r in rs).items())),
            "stuck": ["%dx (%d,%d,%d) next=%d" % (n, p[0], p[1], p[2], p[3]) for p, n in points],
        })
    return table


def render(table):
    out = ["| dungeon | level | runs | completed | best bosses | wipes | median time | ends | stuck points |",
           "|---|---:|---:|---:|---:|---:|---:|---|---|"]
    for t in table:
        name = "%s (%d)" % (NAMES.get(t["dmap"], "map"), t["dmap"])
        median = fmt_ms(t["median_ms"]) if t["median_ms"] is not None else "-"
        ends = ", ".join("%s %d" % kv for kv in t["ends"].items()) or "-"
        out.append("| %s | %d | %d | %d | %s | %d | %s | %s | %s |" % (
            name, t["level"], t["runs"], t["completed"], t["best"], t["wipes"], median, ends,
            "; ".join(t["stuck"]) or "-"))
    return "\n".join(out) + "\n"


def selftest():
    def line(reason, dmap, plvl, **kw):
        row = {"v": 1, "run": "t", "ms": kw.pop("ms", 0), "ev": "dprobe", "bot": 1, "reason": reason, "party": "a",
               "rid": kw.pop("rid", 1), "dmap": dmap, "plvl": plvl}
        row.update(kw)
        return "2026-09-27 12:00:00 " + json.dumps(row)

    lines = [
        line("entered", 36, 28, ms=1),
        line("stuck", 36, 28, ms=2, sx=-105, sy=31, sz=5, next=2),
        line("stuck", 36, 28, ms=3, sx=-101, sy=39, sz=9, next=2),
        line("run", 36, 28, ms=4, bosses=3, total=6, wipes=1, dur_ms=600000, end="stuck"),
        line("run", 36, 28, ms=5, rid=2, bosses=6, total=6, wipes=0, dur_ms=1200000, end="completed"),
        line("run", 43, 26, ms=6, rid=3, bosses=2, total=8, wipes=4, dur_ms=300000, end="wiped_out"),
        '{"v":1,"ev":"dungeon","reason":"run"}',
        "garbage",
    ]
    table = reduce_rows(read_rows(lines))
    assert [(t["dmap"], t["runs"], t["completed"], t["best"], t["wipes"]) for t in table] == \
        [(36, 2, 1, "6/6", 1), (43, 1, 0, "2/8", 4)], table
    assert table[0]["median_ms"] == 900000 and table[0]["ends"] == {"completed": 1, "stuck": 1}
    assert table[0]["stuck"] == ["2x (-120,20,0) next=2"], table[0]["stuck"]
    md = render(table)
    assert "| The Deadmines (36) | 28 | 2 | 1 | 6/6 | 1 | 15:00 | completed 1, stuck 1 |" in md, md
    assert read_rows(lines, party="zzz") == []
    print("selftest OK")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--out")
    ap.add_argument("--party")
    ap.add_argument("--run", help="ledger run id (AutoWow.Ledger.RunId)")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.logs:
        ap.error("no ledger logs given")
    lines = []
    for path in args.logs:
        with open(path, encoding="utf-8", errors="replace") as fh:
            lines.extend(fh)
    md = render(reduce_rows(read_rows(lines, args.party, args.run)))
    if args.out:
        with open(args.out, "w", encoding="utf-8") as fh:
            fh.write(md)
    else:
        sys.stdout.write(md)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
