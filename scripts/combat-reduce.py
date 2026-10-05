#!/usr/bin/env python3
"""Fold AutoWoW ledger `combat` events (combat telemetry cv=1 / cv=2) and tactical `engage` events into
per-bot / per-class / per-level-band combat efficiency tables, per-tactic time tables and per-engagement
tactic cells. Stdlib only, read-only, idempotent.

Usage:
  python combat-reduce.py ledger.log [more.log ...] [--out-dir DIR] [--exclude 101,112,...] [--profile NAME]
  python combat-reduce.py --selftest

Input: the autowow.ledger log (worldserver AutoWow.Ledger.Enable=1 plus AutoWow.CombatTelemetry.Enable=1).
Each `combat` line carries CUMULATIVE per-login counters for one bot (quest 0):
  cv, cls, wall_ms, combat_ms, dead_ms, starved_ms, fights, kills, deaths, dmg, taken, heal,
  casts, gcd_casts, dot_skips, ttk_n, ttk_drop, ttk=[[ms, dlvl], ...]  (ttk: kills since the previous line)
cv=2 (AutoWow.Tactics.Observe/Enable) = every cv=1 field unchanged, plus tac_ms=[[tactic id, ms], ...]
(cumulative in-engagement ms per tactic) and arm (0 control, 1 treatment). cv=2 lines also feed the
`class_band_arm` cells and the per-tactic time table. Any other cv is ignored, never guessed.
cv=3 / cv=4 (AutoWow.CombatTelemetry.Reactivity bots, every human row) = the cv=1 / cv=2 line plus
busy_ms (cumulative cast-lock ms), spells=[[id, casts], ...] (window top-8), rl_att / rl_hp = window
[samples, p50, p90, misses] and human=1 on real-player rows. busy share = d(busy_ms) / d(combat_ms) over
cv>=3 lines only; reaction p50 is a sample-weighted mean of window p50s (not a true pooled median).
Human rows fold only into `human` / `human_class` cells, never into bot tables.
`died` lines (killer kind/id/level) are joined for deaths-by-killer.
`engage` lines (ecv=1, one per solo engagement of a tactic-tracked class, AutoWow.Tactics.Classes) fold
into cells class/band/arm/tac0 (tac0 = the first tactic the policy chose): engagements (= sum over
outcomes: sterile), outcome counts, duration median/p90, kills, mana_spent, hp_lost, wand_ms, cc_n
(control-tool casts), shield_n (PW:Shield / the class's defensive cooldown), gap_rest_ms. Tactic names:
priest p-wand/p-burst/p-multi/p-emergency/p-escape (10-14); other classes <tag>-single/-multi/-emergency/
-escape with tag wl 2x, m 3x, h 4x, r 5x, w 6x, pa 7x, d 8x, s 9x, dk 10x. The tactic time share is one
table per class.

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
TACTICS = {0: "none", 10: "p-wand", 11: "p-burst", 12: "p-multi", 13: "p-emergency", 14: "p-escape"}
# Non-priest families (TacticalPolicy.h): id = 10 x family + slot (0 single, 2 multi, 3 emergency, 4 escape).
for _fam, _tag in ((2, "wl"), (3, "m"), (4, "h"), (5, "r"), (6, "w"), (7, "pa"), (8, "d"), (9, "s"), (10, "dk")):
    for _slot, _name in ((0, "single"), (2, "multi"), (3, "emergency"), (4, "escape")):
        TACTICS[_fam * 10 + _slot] = "%s-%s" % (_tag, _name)
OUTCOMES = {0: "win", 1: "died", 2: "escaped", 3: "died_after_escape", 4: "no_kill"}
ENGAGE_SUMS = ["kills", "mana_spent", "hp_lost", "wand_ms", "cc_n", "shield_n"]


def tactic_name(tid):
    return TACTICS.get(tid, "t%s" % tid)


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
                if ev is not None and ev["ev"] in ("combat", "died", "engage"):
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
    c["tac_ms"] = Counter()
    c["rx_combat_ms"] = 0
    c["busy_ms"] = 0
    c["spells"] = Counter()
    c["rl_att"] = [0, 0, 0]  # samples, sum(p50 x samples), misses
    c["rl_hp"] = [0, 0, 0]
    return c


def new_engage_cell():
    c = {k: 0 for k in ENGAGE_SUMS}
    c.update({"n": 0, "outcomes": Counter(), "dur": [], "gap_rest_ms": 0, "bots": set()})
    return c


def fold_engage(ev, engage):
    cls = CLASSES.get(ev.get("cls"), "cls%s" % ev.get("cls"))
    key = (cls, band(ev.get("lvl", 0)), "arm%s" % ev.get("arm", "?"), tactic_name(ev.get("tac0", 0)))
    cell = engage[key]
    cell["n"] += 1
    cell["outcomes"][OUTCOMES.get(ev.get("outcome"), "o%s" % ev.get("outcome"))] += 1
    cell["dur"].append(int(ev.get("dur_ms", 0)))
    for k in ENGAGE_SUMS:
        cell[k] += int(ev.get(k, 0))
    cell["gap_rest_ms"] += max(0, int(ev.get("gap_rest_ms", -1)))
    cell["bots"].add(ev["bot"])


def fold(events, exclude, engage=None):
    prev = {}                        # (run, bot) -> last cumulative counters
    cells = defaultdict(new_cell)    # (kind, key) -> cell
    for ev in events:
        run, bot = ev.get("run", ""), ev["bot"]
        if bot in exclude:
            continue
        if ev["ev"] == "engage":
            if engage is not None and ev.get("ecv") == 1:
                fold_engage(ev, engage)
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
        cv = ev.get("cv")
        if cv not in (1, 2, 3, 4):
            continue  # unknown combat schema: ignore, never guess
        rx = cv in (3, 4)
        cur = {k: int(ev.get(k, 0)) for k in COUNTERS}
        cur["_busy"] = int(ev.get("busy_ms", 0)) if rx else 0
        tac = {int(t[0]): int(t[1]) for t in ev.get("tac_ms", [])} if cv in (2, 4) else {}
        base = prev.get((run, bot))
        if (base is None or any(cur[k] < base[k] for k in COUNTERS) or cur["_busy"] < base["_busy"]
                or any(tac.get(t, 0) < ms for t, ms in base["_tac"].items())):
            base = {k: 0 for k in COUNTERS}
            base["_tac"] = {}
            base["_busy"] = 0
        delta = {k: cur[k] - base[k] for k in COUNTERS}
        tac_delta = {t: ms - base["_tac"].get(t, 0) for t, ms in tac.items()}
        b = band(ev.get("lvl", 0))
        if ev.get("human") == 1:
            keys = [("human", str(bot)), ("human_class", cls)]
        else:
            keys = [("bot", str(bot)), ("class", cls), ("band", b), ("class_band", "%s/%s" % (cls, b)),
                    ("total", "all")]
        if cv in (2, 4) and ev.get("human") != 1:
            keys.append(("class_band_arm", "%s/%s/arm%s" % (cls, b, ev.get("arm", "?"))))
        for key in keys:
            cell = cells[key]
            for k in COUNTERS:
                cell[k] += delta[k]
            for t, ms in tac_delta.items():
                if ms:
                    cell["tac_ms"][t] += ms
            cell["ttk"].extend(int(s[0]) for s in ev.get("ttk", []))
            cell["bots"].add(bot)
            if rx:
                cell["rx_combat_ms"] += delta["combat_ms"]
                cell["busy_ms"] += cur["_busy"] - base["_busy"]
                for sid, n in ev.get("spells", []):
                    cell["spells"][int(sid)] += int(n)
                for kind in ("rl_att", "rl_hp"):
                    w = ev.get(kind) or [0, 0, 0, 0]
                    cell[kind][0] += int(w[0])
                    cell[kind][1] += int(w[0]) * int(w[1])
                    cell[kind][2] += int(w[3])
        cur["_cls"] = cls
        cur["_tac"] = tac
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
    tac_total = sum(c["tac_ms"].values())
    row["tac_ms"] = {tactic_name(t): ms for t, ms in sorted(c["tac_ms"].items())}
    row["tac_share"] = ({tactic_name(t): round(ms / tac_total, 3) for t, ms in sorted(c["tac_ms"].items())}
                        if tac_total else {})
    row["busy_share"] = round(c["busy_ms"] / c["rx_combat_ms"], 3) if c["rx_combat_ms"] else None
    row["rx_combat_ms"] = c["rx_combat_ms"]
    row["spells"] = [[s, n] for s, n in sorted(c["spells"].items(), key=lambda kv: (-kv[1], kv[0]))[:8]]
    for kind in ("rl_att", "rl_hp"):
        n, wsum, miss = c[kind]
        row[kind] = {"samples": n, "p50_wmean_ms": round(wsum / n) if n else None, "misses": miss}
    return row


def derive_engage(key, c, profile):
    cls, b, arm, tac0 = key
    dur = sorted(c["dur"])
    row = {"profile": profile, "kind": "engage", "key": "/".join(key), "class": cls, "band": b, "arm": arm,
           "tac0": tac0, "bots": len(c["bots"]), "engagements": c["n"],
           "outcomes": dict(sorted(c["outcomes"].items())), "dur_median_ms": pct(dur, 0.5),
           "dur_p90_ms": pct(dur, 0.9), "gap_rest_ms": c["gap_rest_ms"]}
    row.update({k: c[k] for k in ENGAGE_SUMS})
    row["deaths_per_100"] = round(100.0 * (c["outcomes"]["died"] + c["outcomes"]["died_after_escape"]) / c["n"], 2)
    row["mana_per_kill"] = round(c["mana_spent"] / c["kills"], 1) if c["kills"] else None
    return row


def summarize(rows, profile):
    lines = ["# Combat reduce (%s)" % profile, "",
             "Deltas of cumulative `combat` lines (cv=1); rates from summed deltas. dps_combat = dmg / in-combat s;",
             "dps_sustained = dmg / wall s. TTK = bot's first damage on a creature to its killing blow (bot or pet).",
             "gcd_util_est = gcd_casts x 1.5 s / combat s (upper-bound style estimate, ignores haste).", ""]
    for kind in ("total", "band", "class", "class_band", "class_band_arm", "bot", "human_class", "human"):
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
    tac = [r for r in rows if r["kind"] == "class_band_arm"]
    if tac:
        lines += ["## tactic time share (cv=2, in-engagement ms; one table per class)", ""]
        for cls in sorted({r["key"].split("/")[0] for r in tac}):
            sel = [r for r in tac if r["key"].split("/")[0] == cls]
            names = sorted({n for r in sel for n in r["tac_ms"]})
            lines += ["### %s" % cls, "",
                      "| class_band_arm | bots | wall h | deaths/h | starved | tactic s | %s |" % " | ".join(names),
                      "|---|---|---|---|---|---|%s" % ("---|" * len(names))]
            for r in sel:
                f = lambda v, d=3: "-" if v is None else ("%.*f" % (d, v))
                lines.append("| %s | %d | %.2f | %s | %s | %.0f | %s |" % (
                    r["key"], r["bots"], r["wall_ms"] / 3.6e6, f(r["deaths_per_hour"], 2), f(r["starved_share"]),
                    sum(r["tac_ms"].values()) / 1000.0, " | ".join(f(r["tac_share"].get(n)) for n in names)))
            lines.append("")
    rx = [r for r in rows if r["kind"] in ("class", "human_class", "bot", "human") and r["rx_combat_ms"]]
    if rx:
        lines += ["## reactivity (cv=3/4 lines only)", "",
                  "busy = cast-lock ms / in-combat ms. att/hp = reaction to new attacker / own HP < 35%:",
                  "samples, sample-weighted mean of window p50 ms, misses (no cast within 10 s).", "",
                  "| cell | bots | rx combat s | busy | att n | att p50 | att miss | hp n | hp p50 | hp miss | top spells |",
                  "|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in rx:
            a, h = r["rl_att"], r["rl_hp"]
            f = lambda v: "-" if v is None else str(v)
            lines.append("| %s/%s | %d | %.0f | %s | %d | %s | %d | %d | %s | %d | %s |" % (
                r["kind"], r["key"], r["bots"], r["rx_combat_ms"] / 1000.0, f(r["busy_share"]),
                a["samples"], f(a["p50_wmean_ms"]), a["misses"], h["samples"], f(h["p50_wmean_ms"]), h["misses"],
                " ".join("%d:%d" % (s, n) for s, n in r["spells"][:5])))
        lines.append("")
    eng = [r for r in rows if r["kind"] == "engage"]
    if eng:
        lines += ["## engagements (engage, cell = class/band/arm/tac0)", "",
                  "| cell | bots | n | win | no_kill | escaped | died | deaths/100 | dur med s | dur p90 s | kills | mana/kill | cc | shield | rest s |",
                  "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        for r in eng:
            o = r["outcomes"]
            f = lambda v, d=1: "-" if v is None else ("%.*f" % (d, v))
            lines.append("| %s | %d | %d | %d | %d | %d | %d | %s | %s | %s | %d | %s | %d | %d | %.0f |" % (
                r["key"], r["bots"], r["engagements"], o.get("win", 0), o.get("no_kill", 0), o.get("escaped", 0),
                o.get("died", 0) + o.get("died_after_escape", 0), f(r["deaths_per_100"], 2),
                f(None if r["dur_median_ms"] is None else r["dur_median_ms"] / 1000.0),
                f(None if r["dur_p90_ms"] is None else r["dur_p90_ms"] / 1000.0), r["kills"],
                f(r["mana_per_kill"]), r["cc_n"], r["shield_n"], r["gap_rest_ms"] / 1000.0))
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
    engage = defaultdict(new_engage_cell)
    cells = fold(events, exclude, engage)
    order = {"total": 0, "band": 1, "class": 2, "class_band": 3, "class_band_arm": 4, "bot": 5,
             "human_class": 6, "human": 7}
    keyf = lambda kv: (order[kv[0][0]], (int(kv[0][1]) if kv[0][0] in ("bot", "human") else 0), kv[0][1])
    rows = [derive(kind, key, c, profile) for (kind, key), c in sorted(cells.items(), key=keyf)]
    rows += [derive_engage(key, c, profile) for key, c in sorted(engage.items())]
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

    def engage(ms, bot, lvl, arm, tac0, outcome, dur, kills, mana, run="t", cls=5):
        obj = {"v": 1, "run": run, "ms": ms, "ev": "engage", "bot": bot, "lvl": lvl, "quest": 0, "ecv": 1,
               "cls": cls, "tab": 2, "arm": arm, "eng": 1, "tac0": tac0, "tacs": [[tac0, dur]], "dur_ms": dur,
               "kills": kills, "outcome": outcome, "mana_spent": mana, "hp_lost": 10, "wand_ms": dur // 2,
               "cc_n": 0, "shield_n": 1, "gap_ms": -1, "gap_rest_ms": 4000}
        return "2026-09-23 01:00:00 " + json.dumps(obj)

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
        '{"v":1,"run":"t","ms":1,"ev":"combat","bot":3,"lvl":5,"cv":5,"dmg":5}',  # unknown schema -> ignored
        # cv=3 reactivity bot 7 (mage, two windows) and a cv=3 human row (guid 21, hunter)
        line(60000, 7, 15, cv=3, wall_ms=60000, combat_ms=20000, dmg=100, busy_ms=12000,
             spells=[[133, 5], [116, 2]], rl_att=[2, 400, 900, 0], rl_hp=[1, 300, 300, 1]),
        line(120000, 7, 15, cv=3, wall_ms=120000, combat_ms=40000, dmg=200, busy_ms=30000,
             spells=[[133, 3]], rl_att=[2, 800, 900, 1], rl_hp=[0, 0, 0, 0]),
        line(60000, 21, 80, cls=3, cv=3, human=1, wall_ms=60000, combat_ms=30000, dmg=9000, busy_ms=27000,
             spells=[[49050, 4]], rl_att=[1, 200, 200, 0], rl_hp=[0, 0, 0, 0]),
        # cv=2 (tactics): cv=1 fields + cumulative tac_ms + arm; priest 4 in treatment, 5 in control
        line(60000, 4, 12, cls=5, cv=2, arm=1, wall_ms=60000, combat_ms=20000, dmg=500, kills=2, fights=2,
             tac_ms=[[10, 15000], [12, 5000]]),
        line(120000, 4, 12, cls=5, cv=2, arm=1, wall_ms=120000, combat_ms=40000, dmg=900, kills=4, fights=4,
             tac_ms=[[10, 25000], [12, 5000], [13, 10000]]),
        line(60000, 5, 12, cls=5, cv=2, arm=0, wall_ms=60000, combat_ms=30000, dmg=400, kills=1, fights=1,
             tac_ms=[[11, 30000]]),
        engage(30000, 4, 12, 1, 10, 0, 8000, 1, 120),
        engage(50000, 4, 12, 1, 10, 0, 6000, 1, 80),
        engage(90000, 4, 12, 1, 12, 1, 12000, 0, 300),
        engage(40000, 5, 12, 0, 11, 4, 9000, 0, 200),
        # a tactic-tracked warrior (AutoWow.Tactics.Classes): class-family ids 60/62, own time-share table
        line(60000, 6, 12, cls=1, cv=2, arm=1, wall_ms=60000, tac_ms=[[60, 20000], [62, 4000]]),
        engage(45000, 6, 12, 1, 60, 0, 7000, 1, 0, cls=1),
        '{"v":1,"run":"t","ms":2,"ev":"engage","bot":5,"lvl":12,"ecv":9}',  # unknown engage schema -> ignored
    ]
    with tempfile.TemporaryDirectory() as td:
        lp = os.path.join(td, "ledger.log")
        with open(lp, "w", encoding="ascii") as fh:
            fh.write("\n".join(log) + "\n")
        rows = run([lp], td, {9}, "selftest")
        by = {(r["kind"], r["key"]): r for r in rows}
        tot = by[("total", "all")]
        assert tot["dmg"] == 1000 + 300 + 3000 + 900 + 400 + 200, tot["dmg"]  # human 21 excluded
        assert tot["wall_ms"] == 120000 + 30000 + 60000 + 120000 + 60000 + 60000 + 120000
        b7 = by[("bot", "7")]
        assert b7["busy_share"] == 0.75 and b7["spells"] == [[133, 8], [116, 2]], (b7["busy_share"], b7["spells"])
        assert b7["rl_att"] == {"samples": 4, "p50_wmean_ms": 600, "misses": 1}, b7["rl_att"]
        assert b7["rl_hp"] == {"samples": 1, "p50_wmean_ms": 300, "misses": 1}
        assert by[("bot", "1")]["busy_share"] is None and ("bot", "21") not in by
        hu = by[("human_class", "hunter")]
        assert hu["busy_share"] == 0.9 and hu["dmg"] == 9000 and by[("human", "21")]["bots"] == 1
        assert tot["kills"] == 8 + 4 + 1 and tot["ttk_n"] == 8 and tot["ttk_samples"] == 8
        # cv=2: tactic deltas fold like counters; arm cells split treatment/control; cv=1 cells carry none
        arm1 = by[("class_band_arm", "priest/1-20/arm1")]
        assert arm1["tac_ms"] == {"p-wand": 25000, "p-multi": 5000, "p-emergency": 10000}, arm1["tac_ms"]
        assert arm1["tac_share"]["p-wand"] == round(25000 / 40000, 3) and arm1["dmg"] == 900
        assert by[("class_band_arm", "priest/1-20/arm0")]["tac_ms"] == {"p-burst": 30000}
        assert by[("bot", "1")]["tac_ms"] == {} and ("class_band_arm", "mage/1-20/arm?") not in by
        # engage cells: sterile (n == sum of outcomes), keyed class/band/arm/tac0
        eng = {r["key"]: r for r in rows if r["kind"] == "engage"}
        w = eng["priest/1-20/arm1/p-wand"]
        assert w["engagements"] == 2 and w["outcomes"] == {"win": 2} and w["kills"] == 2 and w["mana_per_kill"] == 100.0
        assert w["dur_median_ms"] == 6000 and w["gap_rest_ms"] == 8000
        assert eng["priest/1-20/arm1/p-multi"]["deaths_per_100"] == 100.0
        assert eng["priest/1-20/arm0/p-burst"]["outcomes"] == {"no_kill": 1}
        assert sum(r["engagements"] for r in eng.values()) == 5
        assert by[("class_band_arm", "warrior/1-20/arm1")]["tac_ms"] == {"w-single": 20000, "w-multi": 4000}
        assert eng["warrior/1-20/arm1/w-single"]["outcomes"] == {"win": 1}
        assert all(r["engagements"] == sum(r["outcomes"].values()) for r in eng.values())
        b1 = by[("bot", "1")]
        assert b1["dmg"] == 1300 and b1["combat_ms"] == 60000 and b1["deaths"] == 1
        assert b1["dps_combat"] == round(1300 / 60.0, 1)
        assert b1["killers"] == {"creature:3382(L13,d+3)": 1}, b1["killers"]
        assert b1["ttk_median_ms"] == 9000 and b1["ttk_p90_ms"] == 12000, (b1["ttk_median_ms"], b1["ttk_p90_ms"])
        assert abs(b1["combat_share"] + b1["dead_share"] + b1["downtime_share"] - 1.0) < 0.002
        assert b1["starved_share"] == round(5000 / 60000, 3)
        assert b1["gcd_util_est"] == round(20 * 1500 / 60000, 3)
        # band split: bot 1's lvl-10 and lvl-11 deltas (and the lvl-12 priests) land in 1-20; bot 2 in 21-40
        assert by[("band", "1-20")]["dmg"] == 1300 + 900 + 400 + 200 and by[("band", "21-40")]["dmg"] == 3000
        assert by[("class", "warrior")]["dot_skips"] == 4 and ("bot", "9") not in by and ("bot", "3") not in by
        # sterile: per-bot dmg sums to total
        assert sum(r["dmg"] for r in rows if r["kind"] == "bot") == tot["dmg"]
        first = open(os.path.join(td, "combat-cells.jsonl"), "rb").read()
        run([lp], td, {9}, "selftest")
        assert open(os.path.join(td, "combat-cells.jsonl"), "rb").read() == first  # idempotent
        md = open(os.path.join(td, "combat-summary.md"), encoding="ascii").read()
        assert "## by class_band" in md and "creature:3382(L13,d+3)" in md
        assert "## tactic time share" in md and "## engagements" in md and "priest/1-20/arm1/p-wand" in md
        war = md.split("### warrior")[1].split("\n\n")[1]  # per-class table: only that class's tactics
        assert "w-multi | w-single" in war and "p-wand" not in war, war
        assert "### priest" in md and "warrior/1-20/arm1/w-single" in md
        assert "## reactivity" in md and "human_class/hunter" in md and "bot/7" in md
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
