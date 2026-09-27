#!/usr/bin/env python3
"""XP efficiency per activity: which earns more XP per bot-hour -- dungeons, hunt contracts, questing, or grinding?
Stdlib only, read-only.

Usage:
  python3 xp-efficiency.py --snapshots cohort-s51.jsonl --ledger ledger.log [--tz-hours 3]
  python3 xp-efficiency.py --selftest

XP source: the cohort snapshots (one JSONL line per ~5 min: utc, bots[] with guid/level/xp/map/alive). The ledger
carries no XP counters (combat telemetry = kills/dead_ms/deaths only; progress = quest objective counts), so XP is
snapshot-only. For each bot and each consecutive snapshot pair:
  gained = sum(XP_FOR_LEVEL[L] for L in [L0, L1)) - xp0 + xp1
  levels = gained / XP_FOR_LEVEL[L0]   (normalized progress; summed per cell, then / hours)

Clock alignment: each ledger line is '<local YYYY-MM-DD HH:MM:SS> {json}' with ms = ms since world start.
world_start_local = median(prefix - ms/1000). The prefix is the host's local wall clock; the UTC offset is
--tz-hours, else inferred as round((first ledger prefix - first snapshot utc) / 1h) (valid when snapshotting
starts < 30 min after the soak). Row UTC = world_start_local - tz + ms/1000.

Attribution (first match wins, per bot per interval [t0, t1)):
  offline        no `combat` telemetry row for the bot in the interval (telemetry is per-minute for every online
                 bot); excluded from bot-hours. Skipped when the ledger has no combat rows at all.
  dungeon_party  bot inside a cohort party dungeon run (`party` formed with dmap>0 until that pid's next event,
                 or any `dungeon` row for the bot in the interval)
  dungeon        either endpoint snapshot on a non-continent map (not 0/1/530/571)
  contract       hunt contract active >= 50% of the interval (issued -> done/expired/abandoned by cid), or a
                 contract `done` in the interval (the reward lands there)
  quest          any accepted / progress / rewarded row for the bot in the interval
  supply         any `supply` row (AutoWow.Supply bag-chain role: craft/deliver/order...) or `skill_up` row. Supply
                 bots are paid XP by the module (`supply` reason=xp, copper = XP amount): granted, NOT earned XP --
                 never compare it with the activity categories.
  overhead       dead_ms (telemetry) + zone_move travel_ms + errand travel_ms/return_ms >= 50% of the interval,
                 or dead at both endpoints
  grind          telemetry kills > 0 in the interval (killing with no contract / quest signal)
  other          none of the above
Deaths/h = `died` rows in the interval. Dungeon probe bots (dprobe, guids 72385-72394) are not cohort and are
levelled to dungeon max + 2: their XP is never comparable and is only counted, not tabled.
"""
import argparse
import json
import os
import statistics
import sys
import tempfile
from collections import defaultdict
from datetime import datetime, timezone

# AzerothCore player_xp_for_level (stock 3.3.5; verified equal to acore_world 2026-09-27).
XP_FOR_LEVEL = [0, 400, 900, 1400, 2100, 2800, 3600, 4500, 5400, 6500, 7600, 8700, 9800, 11000, 12300, 13600,
                15000, 16400, 17800, 19300, 20800, 22400, 24000, 25500, 27200, 28900, 30500, 32200, 33900, 36300,
                38800, 41600, 44600, 48000, 51400, 55000, 58700, 62400, 66200, 70200, 74300, 78500, 82800, 87100,
                91600, 96300, 101000, 105800, 110700, 115700, 120900, 126100, 131500, 137000, 142500, 148200,
                154000, 159900, 165800, 172000, 290000, 317000, 349000, 386000, 428000, 475000, 527000, 585000,
                648000, 717000, 1523800, 1539600, 1555700, 1571800, 1587900, 1604200, 1620700, 1637400, 1653900,
                1670800]
CONTINENTS = {0, 1, 530, 571}
PROBES = set(range(72385, 72395))
CATS = ["dungeon_party", "dungeon", "contract", "quest", "supply", "grind", "overhead", "other"]
BANDS = [(1, 9, "1-9"), (10, 19, "10-19"), (20, 29, "20-29"), (30, 39, "30-39"), (40, 99, "40+")]
QUEST_EVS = {"accepted", "progress", "rewarded"}


def band(lvl):
    return next(n for lo, hi, n in BANDS if lo <= lvl <= hi)


def utc(s):
    return datetime.fromisoformat(s.rstrip("Z")[:26]).replace(tzinfo=timezone.utc).timestamp()


def naive(s):
    return datetime.strptime(s, "%Y-%m-%d %H:%M:%S").replace(tzinfo=timezone.utc).timestamp()


def load_snapshots(path):
    snaps = []
    with open(path, encoding="utf-8") as f:
        for line in f:
            if line.strip():
                d = json.loads(line)
                snaps.append((utc(d["utc"]), {b["guid"]: b for b in d["bots"]}))
    snaps.sort(key=lambda s: s[0])
    return snaps


def load_ledger(path, keep):
    rows, offs, run = [], [], None
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            i = line.find('{"v"')
            if i < 0:
                continue
            try:
                r = json.loads(line[i:])
            except ValueError:
                continue
            run = run or r.get("run")
            if i >= 19:
                try:
                    offs.append(naive(line[:i].strip()[-19:]) - r["ms"] / 1000.0)
                except ValueError:
                    pass
            if r.get("bot") in keep or r.get("ev") == "dprobe":
                rows.append(r)
    return rows, offs, run


def reduce(snaps, rows, world_start):
    t = lambda r: world_start + r["ms"] / 1000.0
    by_bot = defaultdict(list)
    for r in rows:
        by_bot[r["bot"]].append(r)
    have_telemetry = any(r["ev"] == "combat" for r in rows)

    # per-bot spans and point signals
    contract_spans, party_spans = defaultdict(list), defaultdict(list)
    open_c, open_p = {}, {}
    end_t = max([t(r) for r in rows] + [snaps[-1][0]])
    for r in sorted(rows, key=lambda r: r["ms"]):
        ev, bot = r["ev"], r["bot"]
        if ev == "contract":
            key = (bot, r.get("cid"))
            if r.get("reason") == "issued":
                open_c[key] = t(r)
            elif key in open_c:
                contract_spans[bot].append((open_c.pop(key), t(r)))
        elif ev == "party":
            pid = r.get("pid")
            if r.get("reason") == "formed":
                if r.get("dmap", 0) > 0:
                    open_p[pid] = (t(r), r.get("members", []))
            elif pid in open_p:
                t0, members = open_p.pop(pid)
                for m in members:
                    party_spans[m].append((t0, t(r)))
    for (bot, _), t0 in open_c.items():
        contract_spans[bot].append((t0, end_t))
    for t0, members in open_p.values():
        for m in members:
            party_spans[m].append((t0, end_t))
    party_runs = sum(1 for r in rows if r["ev"] == "party" and r.get("reason") == "formed" and r.get("dmap", 0) > 0)

    def overlap(spans, a, b):
        return sum(max(0.0, min(b, e) - max(a, s)) for s, e in spans)

    # combat telemetry deltas (cumulative per login; a decrease = new session, baseline resets)
    tele = defaultdict(list)  # bot -> [(t, dkills, ddead_ms)]
    for bot, rs in by_bot.items():
        prev = None
        for r in rs:
            if r["ev"] != "combat":
                continue
            k, d = r.get("kills", 0), r.get("dead_ms", 0)
            if prev is None or k < prev[0] or d < prev[1] or r.get("wall_ms", 0) < prev[2]:
                prev = (0, 0, 0)
            tele[bot].append((t(r), k - prev[0], d - prev[1]))
            prev = (k, d, r.get("wall_ms", 0))

    cells = defaultdict(lambda: defaultdict(float))  # (cat, band) -> sums
    skipped = defaultdict(int)
    for (ta, sa), (tb, sb) in zip(snaps, snaps[1:]):
        span = tb - ta
        for guid, b0 in sa.items():
            b1 = sb.get(guid)
            if b1 is None:
                skipped["missing"] += 1
                continue
            l0, l1 = b0["level"], b1["level"]
            if l1 < l0 or l0 < 1 or l1 >= len(XP_FOR_LEVEL) + 1:
                skipped["level_drop"] += 1
                continue
            gained = sum(XP_FOR_LEVEL[L] for L in range(l0, l1)) - b0["xp"] + b1["xp"]
            if gained < 0:
                skipped["xp_drop"] += 1
                continue
            ev = [r for r in by_bot.get(guid, ()) if ta <= t(r) < tb]
            te = [x for x in tele.get(guid, ()) if ta <= x[0] < tb]
            kills, dead_ms = sum(x[1] for x in te), sum(x[2] for x in te)
            travel = sum(r.get("travel_ms", 0) for r in ev if r["ev"] == "zone_move") + \
                sum(r.get("travel_ms", 0) + r.get("return_ms", 0) for r in ev if r["ev"] == "errand")
            if have_telemetry and not te:
                cat = "offline"
            elif overlap(party_spans.get(guid, ()), ta, tb) > 0 or any(r["ev"] == "dungeon" for r in ev):
                cat = "dungeon_party"
            elif b0["map"] not in CONTINENTS or b1["map"] not in CONTINENTS:
                cat = "dungeon"
            elif overlap(contract_spans.get(guid, ()), ta, tb) >= span / 2 or \
                    any(r["ev"] == "contract" and r.get("reason") == "done" for r in ev):
                cat = "contract"
            elif any(r["ev"] in QUEST_EVS for r in ev):
                cat = "quest"
            elif any(r["ev"] in ("supply", "skill_up") for r in ev):
                cat = "supply"
            elif (dead_ms + travel) / 1000.0 >= span / 2 or (not b0["alive"] and not b1["alive"]):
                cat = "overhead"
            elif kills > 0:
                cat = "grind"
            else:
                cat = "other"
            c = cells[(cat, band(l0))]
            c["h"] += span / 3600.0
            c["xp"] += gained
            c["lv"] += gained / XP_FOR_LEVEL[l0] if l0 < len(XP_FOR_LEVEL) else 0.0
            c["deaths"] += sum(1 for r in ev if r["ev"] == "died")
            c["kills"] += kills
            c["n"] += 1
    dprobe = sum(1 for r in rows if r["ev"] == "dprobe")
    granted = sum(r.get("copper", 0) for r in rows if r["ev"] == "supply" and r.get("reason") == "xp"
                  and snaps[0][0] <= t(r) < snaps[-1][0])
    return cells, skipped, party_runs, dprobe, granted


def table(cells, cats, key_band=None):
    tot = defaultdict(float)
    agg = defaultdict(lambda: defaultdict(float))
    for (cat, b), c in cells.items():
        if cat == "offline" or (key_band and b != key_band):
            continue
        for k, v in c.items():
            agg[cat][k] += v
            tot[k] += v
    out = ["| category | n | bot-h | XP | XP share | XP/h | levels/h | kills/h | deaths/h |",
           "|---|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for cat in cats + ["TOTAL"]:
        c = tot if cat == "TOTAL" else agg.get(cat)
        if not c or not c["n"]:
            continue
        h = c["h"] or 1e-9
        share = 100.0 * c["xp"] / tot["xp"] if tot["xp"] else 0.0
        out.append("| %s | %d | %.1f | %d | %.1f%% | %d | %.3f | %.1f | %.2f |" % (
            cat, c["n"], c["h"], c["xp"], share, c["xp"] / h, c["lv"] / h, c["kills"] / h, c["deaths"] / h))
    return "\n".join(out)


def report(cells, skipped, party_runs, dprobe, granted, header):
    lines = [header, "", "### All levels (offline intervals excluded)", "", table(cells, CATS)]
    for _, _, b in BANDS:
        if any(k[1] == b and k[0] != "offline" for k in cells):
            lines += ["", "### Level band %s" % b, "", table(cells, CATS, b)]
    off = sum(c["h"] for (cat, _), c in cells.items() if cat == "offline")
    dg = [c for (cat, _), c in cells.items() if cat == "dungeon"]
    if dg and not sum(c["kills"] + c["xp"] for c in dg):
        lines += ["", "NOTE: `dungeon` intervals have 0 kills and 0 XP -- bots parked idle on instance maps, not runs."]
    lines += ["", "offline bot-h excluded: %.1f; skipped intervals: %s" % (off, dict(skipped) or "none"),
              "cohort party dungeon runs (party dmap>0): %d%s" % (
                  party_runs, "" if party_runs else " -- no dungeon_party rows; `dungeon` = cohort bot on an "
                  "instance map without a party run"),
              "supply-role XP granted by the module in the window (supply reason=xp): %d" % granted,
              "dprobe rows: %d -- probe parties are levelled to dungeon max + 2; probe dungeon XP is NOT comparable "
              "and is excluded (probes are not cohort)." % dprobe]
    return "\n".join(lines)


def run(snap_path, ledger_path, tz_hours):
    snaps = load_snapshots(snap_path)
    keep = set().union(*(s[1].keys() for s in snaps)) | PROBES
    rows, offs, run_name = load_ledger(ledger_path, keep)
    ws_local = statistics.median(offs)
    if tz_hours is None:
        tz_hours = round((ws_local + min(r["ms"] for r in rows) / 1000.0 - snaps[0][0]) / 3600.0) if rows else 0
    world_start = ws_local - tz_hours * 3600
    iso = lambda x: datetime.fromtimestamp(x, timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    led_end = world_start + max(r["ms"] for r in rows) / 1000.0 if rows else world_start
    header = ("## XP efficiency -- run %s\n\nsnapshots %s: %d samples %s .. %s | ledger %s: world start %s "
              "(local prefix - ms, tz %+dh), cohort rows to %s" % (
                  run_name, os.path.basename(snap_path), len(snaps), iso(snaps[0][0]), iso(snaps[-1][0]),
                  ledger_path, iso(world_start), tz_hours, iso(led_end)))
    return report(*reduce(snaps, rows, world_start), header)


def selftest():
    d = tempfile.mkdtemp()
    snap = os.path.join(d, "s.jsonl")
    led = os.path.join(d, "l.log")
    bot = lambda g, lvl, xp, m=1, alive=True: {"guid": g, "level": lvl, "xp": xp, "map": m, "alive": alive}
    # 3 samples, 5 min apart; world start 10:00:00 local = 07:00:00Z (tz +3), snapshots from 07:00:00Z
    snaps = [
        {"utc": "2026-01-01T07:00:00Z", "bots": [bot(1, 10, 7000), bot(2, 20, 0), bot(3, 24, 100, 43), bot(4, 30, 0), bot(5, 30, 0)]},
        {"utc": "2026-01-01T07:05:00Z", "bots": [bot(1, 11, 400), bot(2, 20, 2000), bot(3, 24, 100, 43), bot(4, 30, 900), bot(5, 30, 0)]},
        {"utc": "2026-01-01T07:10:00Z", "bots": [bot(1, 11, 1400), bot(2, 20, 2000), bot(3, 24, 100, 43), bot(4, 30, 900), bot(5, 30, 500)]},
    ]
    with open(snap, "w") as f:
        f.write("\n".join(json.dumps(s) for s in snaps) + "\n")

    def row(ms, ev, b, **kw):
        s = 10 * 3600 + ms // 1000
        pre = "2026-01-01 %02d:%02d:%02d " % (s // 3600, s // 60 % 60, s % 60)
        return pre + json.dumps(dict({"v": 1, "run": "selftest", "ms": ms, "ev": ev, "bot": b}, **kw))
    L = []
    for m in range(0, 600000, 60000):  # telemetry for bots 1,2,4 (3 has none -> offline); bot 5 kills
        for b in (1, 2, 4, 5):
            L.append(row(m + 1000, "combat", b, kills=m // 60000, dead_ms=0, wall_ms=m + 1000))
    L.append(row(1000, "contract", 1, reason="issued", cid=7))
    L.append(row(290000, "contract", 1, reason="done", cid=7))     # bot1 interval 1 = contract
    L.append(row(320000, "rewarded", 1, quest=5))                  # bot1 interval 2 = quest
    L.append(row(30000, "progress", 2, quest=9))                   # bot2 interval 1 = quest
    L.append(row(310000, "died", 2))
    L.append(row(305000, "zone_move", 2, travel_ms=200000))        # bot2 interval 2 = overhead
    L.append(row(330000, "supply", 5, reason="xp", copper=500))   # bot5 interval 2 = supply (interval 1 = grind)
    L.append(row(5000, "party", 4, reason="formed", pid=1, dmap=36, members=[4]))
    L.append(row(400000, "party", 4, reason="disbanded", pid=1))   # bot4 both intervals = dungeon_party
    with open(led, "w") as f:
        f.write("\n".join(L) + "\n")
    snaps_l = load_snapshots(snap)
    rows, offs, _ = load_ledger(led, {1, 2, 3, 4, 5})
    ws = statistics.median(offs) - 3 * 3600
    assert abs(ws - utc("2026-01-01T07:00:00Z")) < 1, ws
    cells, skipped, runs, _, granted = reduce(snaps_l, rows, ws)
    got = {k: (round(v["xp"]), v["n"]) for k, v in cells.items()}
    exp = {("contract", "10-19"): (1000, 1), ("quest", "10-19"): (1000, 1), ("quest", "20-29"): (2000, 1),
           ("overhead", "20-29"): (0, 1), ("offline", "20-29"): (0, 2), ("dungeon_party", "30-39"): (900, 2),
           ("grind", "30-39"): (0, 1), ("supply", "30-39"): (500, 1)}
    assert got == exp, got
    assert cells[("overhead", "20-29")]["deaths"] == 1
    assert abs(cells[("contract", "10-19")]["lv"] - 1000 / 7600) < 1e-9
    assert runs == 1 and not skipped and granted == 500
    assert "offline" not in table(cells, CATS)
    out = run(snap, led, None)  # tz inference path
    assert "tz +3h" in out and "| TOTAL | 8 |" in out, out
    print("selftest OK")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--snapshots")
    ap.add_argument("--ledger")
    ap.add_argument("--tz-hours", type=int, help="ledger prefix local-time UTC offset (default: inferred)")
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    if not (a.snapshots and a.ledger):
        ap.error("--snapshots and --ledger required")
    print(run(a.snapshots, a.ledger, a.tz_hours))


if __name__ == "__main__":
    sys.exit(main())
