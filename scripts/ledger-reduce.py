#!/usr/bin/env python3
"""Offline reducer for the AutoWoW per-quest ledger (logger `autowow.ledger`, schema v1).

Input: one or more worldserver ledger log files. Each line may carry a log prefix; the JSON object
starting at '{"v":' is extracted. Events are folded into rows keyed by (run, bot, quest).

Output (in --out-dir):
  ledger-rows.jsonl   one row per (run, bot, quest), sorted by key
  ledger-summary.md   outcome counts (sum == rows), per-family pass rate, top stall signatures,
                      deaths by killer kind, PvP kills

Bot-level events (quest 0, never a row): `died` (killer, kid, klvl) and `pvp_kill` (victim, vlvl,
honorable). They feed the death / PvP tables and the `contested` outcome.

Blocked lines may carry `n` (AutoWow.Ledger.BlockedDedupeMs > 0): the number of occurrences the
line stands for. Lines without `n` (older logs, dedupe off) count as one; blocked_count sums n.

Outcome precedence (first match wins):
  contaminated     a contaminated event hit this bot while the quest was open (teleport/revive/randomize)
  rewarded         rewarded event seen
  contested        the bot died to a player (`died` killer=player) while the quest was open
  abandoned        abandoned (deferred_reason says why, when the drop path emitted one)
  deferred         set aside (low-priority / parked) and not rewarded
  blocked          last event for the row is `blocked`
  open_progressing counters changed within --stall-ms of the run's last event
  open_stalled     otherwise

Usage: python ledger-reduce.py LOG [LOG ...] [--out-dir DIR] [--catalog PATH] [--stall-ms N]
       python ledger-reduce.py --selftest
"""

import argparse
import json
import os
import sys
import tempfile
from collections import Counter, defaultdict

SCHEMA_VERSION = 1
DEFAULT_CATALOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "census", "quest-catalog.json")
OUTCOMES = ["rewarded", "rewarded_assisted", "open_progressing", "open_stalled", "blocked", "deferred", "abandoned",
            "contested", "contaminated"]
# Assist relocations (bot-initiated, stock behavior the owner accepts: "as long as bots end up where they need to be").
# A reward after one of these scores rewarded_assisted (reported separately, never as a clean pass).
ASSIST_REASONS = {"rpg_stuck_teleport", "zone_travel_assist"}
KILLER_KINDS = ["player", "creature", "environment", "unknown"]
# Contaminated reasons that reset or relocate the bot (level/gear/quest log/zone): open rows before them are
# voided (dropped, counted per reason in the summary) instead of scored. zone_travel_assist stays contamination.
SETUP_REASONS = {"rndbot_randomize", "setup_reroll", "rndbot_teleport", "bridge_rally_teleport", "bridge_route_teleport",
                 "probe_reset_teleport"}
BUCKET_YARDS = 50
_DECODER = json.JSONDecoder()


def parse_line(line):
    i = line.find('{"v":')
    if i < 0:
        return None
    try:
        obj, _ = _DECODER.raw_decode(line, i)
    except ValueError:
        return None
    if obj.get("v") != SCHEMA_VERSION:
        return None
    return obj


def read_events(paths):
    events = []
    seq = 0
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                ev = parse_line(line)
                if ev is not None:
                    events.append((ev.get("ms", 0), seq, ev))
                    seq += 1
    events.sort(key=lambda t: (t[0], t[1]))
    return [e for _, _, e in events]


def counters(ev):
    return list(ev.get("c", [])) + list(ev.get("i", []))


def new_row(run, bot, quest):
    return {
        "run": run, "bot": bot, "quest": quest, "team": None, "attempts": 0,
        "accepted_at": None, "first_progress_at": None, "last_progress_at": None,
        "last_counters": None, "rewarded_at": None, "abandoned_at": None,
        "deferred_at": None, "deferred_reason": None,
        "blocked_count": 0, "blocked_last_reason": None,
        "contaminated_at": None, "contaminated_reason": None, "contested_at": None,
        "last_ev": None, "last_ms": None, "last_reason": None, "last_phase": None,
        "zone": None, "x": None, "y": None,
    }


def is_open(row):
    return row["rewarded_at"] is None and row["abandoned_at"] is None


def fold(events):
    rows = {}
    open_by_bot = defaultdict(set)  # (run, bot) -> {quest}
    run_end = {}
    deaths = Counter()               # killer kind -> died events
    pvp = Counter()                  # "kills" / "honorable" -> pvp_kill events
    voided = Counter()               # setup reason -> open rows dropped (see SETUP_REASONS)

    def get(run, bot, quest):
        key = (run, bot, quest)
        if key not in rows:
            rows[key] = new_row(run, bot, quest)
        return rows[key]

    def touch(row, ev):
        row["last_ev"] = ev["ev"]
        row["last_ms"] = ev["ms"]
        row["team"] = ev.get("team")
        row["zone"] = ev.get("zone")
        row["x"] = ev.get("x")
        row["y"] = ev.get("y")

    def observe_counters(row, ev):
        cur = counters(ev)
        prev = row["last_counters"]
        if prev is not None and cur != prev and sum(cur) > 0:
            if row["first_progress_at"] is None:
                row["first_progress_at"] = ev["ms"]
            row["last_progress_at"] = ev["ms"]
        row["last_counters"] = cur

    for ev in events:
        run, bot, quest, kind, ms = ev.get("run", ""), ev["bot"], ev["quest"], ev["ev"], ev["ms"]
        run_end[run] = max(run_end.get(run, 0), ms)

        if kind == "contaminated" and ev.get("reason") in SETUP_REASONS:
            # Randomize/reroll resets level, gear and quest log: open attempts before it are setup noise,
            # not quest outcomes. Void them (drop the rows) instead of counting them as contaminated.
            for q in open_by_bot.pop((run, bot), set()):
                if rows.pop((run, bot, q), None) is not None:
                    voided[ev.get("reason")] += 1
            continue

        if kind == "contaminated":
            targets = set(open_by_bot[(run, bot)])
            if quest:
                targets.add(quest)
            for q in targets:
                row = get(run, bot, q)
                if row["contaminated_at"] is None:
                    row["contaminated_at"] = ms
                    row["contaminated_reason"] = ev.get("reason")
            continue

        if kind == "died":
            killer = ev.get("killer", "unknown")
            deaths[killer if killer in KILLER_KINDS else "unknown"] += 1
            if killer == "player":
                for q in open_by_bot[(run, bot)]:
                    row = get(run, bot, q)
                    if row["contested_at"] is None:
                        row["contested_at"] = ms
            continue
        if kind == "pvp_kill":
            pvp["kills"] += 1
            pvp["honorable"] += 1 if ev.get("honorable") else 0
            continue

        row = get(run, bot, quest)
        if kind == "accepted":
            if not is_open(row):
                # Re-accept after a terminal event starts a fresh attempt; outcome describes the latest.
                for f in ("rewarded_at", "abandoned_at", "deferred_at", "deferred_reason",
                          "contaminated_at", "contaminated_reason", "contested_at", "last_reason", "last_phase"):
                    row[f] = None
            row["attempts"] += 1
            row["accepted_at"] = ms
            row["last_counters"] = counters(ev)
            open_by_bot[(run, bot)].add(quest)
        elif kind == "rewarded":
            row["rewarded_at"] = ms
            open_by_bot[(run, bot)].discard(quest)
        elif kind == "abandoned":
            row["abandoned_at"] = ms
            open_by_bot[(run, bot)].discard(quest)
        elif kind == "blocked":
            observe_counters(row, ev)
            row["blocked_count"] += int(ev.get("n", 1))
            row["blocked_last_reason"] = ev.get("reason")
            row["last_reason"], row["last_phase"] = ev.get("reason"), ev.get("phase")
        elif kind == "deferred":
            observe_counters(row, ev)
            row["deferred_at"] = ms
            row["deferred_reason"] = ev.get("reason")
            row["last_reason"], row["last_phase"] = ev.get("reason"), ev.get("phase")
        else:
            continue  # unknown future event name: ignore, never guess
        touch(row, ev)

    return rows, run_end, deaths, pvp, voided


def outcome(row, run_end, stall_ms):
    if row["contaminated_at"] is not None:
        if row["rewarded_at"] is not None and row["contaminated_reason"] in ASSIST_REASONS:
            return "rewarded_assisted"
        return "contaminated"
    if row["rewarded_at"] is not None:
        return "rewarded"
    if row["contested_at"] is not None:
        return "contested"
    if row["abandoned_at"] is not None:
        return "abandoned"
    if row["deferred_at"] is not None:
        return "deferred"
    if row["last_ev"] == "blocked":
        return "blocked"
    last_progress = row["last_progress_at"] or row["accepted_at"] or 0
    if run_end - last_progress <= stall_ms:
        return "open_progressing"
    return "open_stalled"


def stall_signature(row):
    bx = None if row["x"] is None else row["x"] // BUCKET_YARDS
    by = None if row["y"] is None else row["y"] // BUCKET_YARDS
    return [row["last_reason"] or "none", row["last_phase"] or "", row["zone"], [bx, by]]


def load_catalog(path):
    if not path or not os.path.exists(path):
        return {}
    with open(path, encoding="utf-8") as fh:
        data = json.load(fh)
    return {q["id"]: q for q in data.get("quests", [])}


def reduce_rows(rows, run_end, catalog, stall_ms):
    out = []
    for key in sorted(rows):
        row = dict(rows[key])
        meta = catalog.get(row["quest"], {})
        row["family"] = meta.get("family", "UNKNOWN")
        row["quest_zone"] = meta.get("zoneName", "")
        row["title"] = meta.get("title", "")
        row["outcome"] = outcome(row, run_end.get(row["run"], 0), stall_ms)
        row["stall_signature"] = None if row["outcome"] in ("rewarded", "rewarded_assisted") else stall_signature(row)
        out.append(row)
    return out


def summarize(out, deaths=None, pvp=None, voided=None):
    deaths = deaths or Counter()
    pvp = pvp or Counter()
    lines = ["# AutoWoW quest ledger summary", ""]
    total = len(out)
    runs = sorted({r["run"] for r in out})
    lines.append("Runs: " + (", ".join("`%s`" % r for r in runs) if runs else "(none)"))
    lines.append("")
    lines += ["## Outcomes", "", "| outcome | rows |", "|---|---:|"]
    counts = Counter(r["outcome"] for r in out)
    for name in OUTCOMES:
        lines.append("| %s | %d |" % (name, counts.get(name, 0)))
    lines.append("| **total** | **%d** |" % total)
    assert sum(counts.values()) == total

    lines += ["", "## Pass rate by family", "",
              "Pass = rewarded / rows (clean). Assisted = rewarded after an assist teleport (ASSIST_REASONS), "
              "reported separately. Contaminated rows count in rows but never as a pass.", "",
              "| family | rows | rewarded | assisted | pass rate | pass+assisted |", "|---|---:|---:|---:|---:|---:|"]
    fam = defaultdict(lambda: [0, 0, 0])
    for r in out:
        fam[r["family"]][0] += 1
        if r["outcome"] == "rewarded":
            fam[r["family"]][1] += 1
        elif r["outcome"] == "rewarded_assisted":
            fam[r["family"]][2] += 1
    for name in sorted(fam, key=lambda f: (-fam[f][0], f)):
        n, ok, ast = fam[name]
        lines.append("| %s | %d | %d | %d | %.1f%% | %.1f%% |" % (name, n, ok, ast, 100.0 * ok / n, 100.0 * (ok + ast) / n))
    lines.append("| **total** | **%d** | **%d** | **%d** | | |" % (sum(v[0] for v in fam.values()),
                                                          sum(v[1] for v in fam.values()), sum(v[2] for v in fam.values())))

    sig = Counter()
    sig_bots = defaultdict(set)
    for r in out:
        if r["outcome"] in ("rewarded", "rewarded_assisted", "open_progressing"):
            continue
        s = r["stall_signature"]
        k = (r["family"], r["outcome"], s[0], s[1], s[2], s[3][0], s[3][1])
        sig[k] += 1
        sig_bots[k].add(r["bot"])
    top = sorted(sig.items(), key=lambda kv: (-kv[1], kv[0]))[:20]
    lines += ["", "## Top 20 stall signatures (grouped by family)", "",
              "Signature = (last reason, phase, zone, %d-yard bucket); rewarded and open_progressing rows excluded." % BUCKET_YARDS,
              "", "| family | outcome | reason | phase | zone | bucket | rows | bots |",
              "|---|---|---|---|---:|---|---:|---:|"]
    for k, n in sorted(top, key=lambda kv: (kv[0][0], -kv[1], kv[0])):
        lines.append("| %s | %s | %s | %s | %s | %s,%s | %d | %d |" % (k[0], k[1], k[2], k[3], k[4], k[5], k[6], n, len(sig_bots[k])))
    shown = sum(n for _, n in top)
    lines.append("")
    lines.append("Shown %d of %d non-passing, non-progressing rows (%d distinct signatures)." % (shown, sum(sig.values()), len(sig)))

    if voided:
        lines += ["", "Setup-voided open rows (dropped, not counted): " + ", ".join("%s=%d" % kv for kv in sorted(voided.items()))]
    lines += ["", "## Deaths by killer kind", "", "| killer | deaths |", "|---|---:|"]
    for name in KILLER_KINDS:
        lines.append("| %s | %d |" % (name, deaths.get(name, 0)))
    lines.append("| **total** | **%d** |" % sum(deaths.values()))
    lines += ["", "PvP kills by bots: %d (honorable %d)." % (pvp.get("kills", 0), pvp.get("honorable", 0))]
    return "\n".join(lines) + "\n"


def run(paths, out_dir, catalog_path, stall_ms):
    events = read_events(paths)
    rows, run_end, deaths, pvp, voided = fold(events)
    out = reduce_rows(rows, run_end, load_catalog(catalog_path), stall_ms)
    os.makedirs(out_dir, exist_ok=True)
    with open(os.path.join(out_dir, "ledger-rows.jsonl"), "w", encoding="ascii", newline="\n") as fh:
        for r in out:
            fh.write(json.dumps(r, sort_keys=False, ensure_ascii=True) + "\n")
    with open(os.path.join(out_dir, "ledger-summary.md"), "w", encoding="ascii", newline="\n") as fh:
        fh.write(summarize(out, deaths, pvp, voided))
    return events, out


def selftest():
    def line(ms, ev, bot, quest, c=(0, 0, 0, 0), reason="", phase="", zone=12, x=0, y=0, run="t1", **extra):
        obj = {"v": 1, "run": run, "ms": ms, "ev": ev, "bot": bot, "team": 0, "lvl": 5, "quest": quest,
               "map": 0, "zone": zone, "x": x, "y": y, "c": list(c), "i": [0] * 6, "reason": reason, "phase": phase}
        obj.update(extra)
        return "2026-09-23_01:00:00 INFO [autowow.ledger] " + json.dumps(obj, separators=(",", ":"))

    log = [
        line(1000, "accepted", 1, 100),
        line(2000, "blocked", 1, 100, c=(1, 0, 0, 0), reason="progress_did_not_change", phase="engage_target"),
        line(3000, "rewarded", 1, 100),                                           # rewarded
        line(1000, "accepted", 2, 200),
        line(1900, "blocked", 2, 200, reason="no_live_candidate", phase="acquire_target", x=120, y=-10),
        line(2000, "blocked", 2, 200, reason="no_live_candidate", phase="acquire_target", x=120, y=-10, n=119),  # blocked
        line(1000, "accepted", 3, 300),
        line(1500, "deferred", 3, 300, reason="drop_off_zone"),
        line(1500, "abandoned", 3, 300),                                          # abandoned
        line(1000, "accepted", 4, 400),
        line(4000, "deferred", 4, 400, reason="oracle_unrunnable", phase="resolve_objective"),  # deferred
        line(1000, "accepted", 5, 500),
        line(5000, "contaminated", 5, 0, reason="zone_travel_assist"),            # contaminated
        line(6000, "rewarded", 5, 500),
        line(1000, "accepted", 6, 600),                                           # open_stalled (no events)
        "garbage line without json",
        line(1000, "accepted", 7, 700),
        line(700000, "blocked", 7, 700, c=(0, 2, 0, 0), reason="sources_respawning", phase="wait_for_respawn"),
        line(700001, "deferred", 8, 0, reason="x"),  # stray, quest 0 -> its own row
    ]
    # bot 7: last event is blocked -> blocked. Add bot 9 progressing via deferred counters at end.
    log += [line(1000, "accepted", 9, 900), line(699000, "deferred", 9, 900, c=(1, 0, 0, 0), reason="executor_unsupported")]
    log += [line(1000, "accepted", 10, 1000), line(690000, "contaminated", 10, 1000, reason="zone_travel_assist")]
    log += [line(1000, "accepted", 11, 1100), line(1000, "accepted", 11, 1101),
            line(2000, "blocked", 11, 1101, reason="no_live_candidate", phase="acquire_target"),
            line(3000, "died", 11, 0, killer="player", kid=12, klvl=40),
            line(3000, "pvp_kill", 12, 0, victim=11, vlvl=38, honorable=True),
            line(4000, "rewarded", 11, 1100),
            line(1000, "accepted", 12, 1200), line(2500, "died", 12, 0, killer="creature", kid=681, klvl=36),
            line(2600, "died", 12, 0, killer="environment", kid=0, klvl=0)]
    catalog = {"quests": [{"id": 100, "family": "KILL", "zoneName": "Elwynn Forest", "title": "A"},
                          {"id": 200, "family": "KILL", "zoneName": "Elwynn Forest", "title": "B"}]}
    with tempfile.TemporaryDirectory() as td:
        lp = os.path.join(td, "ledger.log")
        cp = os.path.join(td, "cat.json")
        with open(lp, "w", encoding="ascii") as fh:
            fh.write("\n".join(log) + "\n")
        with open(cp, "w", encoding="ascii") as fh:
            json.dump(catalog, fh)
        events, out = run([lp], td, cp, 600000)
        assert len(events) == len(log) - 1, len(events)
        got = {(r["bot"], r["quest"]): r for r in out}
        expect = {(1, 100): "rewarded", (2, 200): "blocked", (3, 300): "abandoned", (4, 400): "deferred",
                  (5, 500): "rewarded_assisted", (6, 600): "open_stalled", (7, 700): "blocked",
                  (8, 0): "deferred", (9, 900): "deferred", (10, 1000): "contaminated",
                  (11, 1100): "rewarded", (11, 1101): "contested", (12, 1200): "open_stalled"}
        for k, v in expect.items():
            assert got[k]["outcome"] == v, (k, got[k]["outcome"], v)
        assert len(out) == len(expect)
        assert got[(1, 100)]["first_progress_at"] == 2000
        assert got[(1, 100)]["blocked_count"] == 1                    # legacy line without n
        assert got[(2, 200)]["blocked_count"] == 1 + 119              # n summed
        assert got[(1, 100)]["family"] == "KILL" and got[(2, 200)]["quest_zone"] == "Elwynn Forest"
        assert got[(2, 200)]["stall_signature"] == ["no_live_candidate", "acquire_target", 12, [2, -1]]
        assert got[(3, 300)]["deferred_reason"] == "drop_off_zone"
        assert got[(9, 900)]["first_progress_at"] == 699000
        assert got[(5, 500)]["rewarded_at"] == 6000 and got[(5, 500)]["contaminated_reason"] == "zone_travel_assist"
        md = open(os.path.join(td, "ledger-summary.md"), encoding="ascii").read()
        assert "| **total** | **13** |" in md, md
        assert "| contested | 1 |" in md, md
        assert "| player | 1 |" in md and "| creature | 1 |" in md and "| environment | 1 |" in md, md
        assert "| **total** | **3** |" in md, md
        assert "PvP kills by bots: 1 (honorable 1)." in md, md
        assert (12, 0) not in got and (11, 0) not in got  # bot-level events never make rows
        # Idempotent: same input -> identical bytes.
        first = open(os.path.join(td, "ledger-rows.jsonl"), "rb").read()
        run([lp], td, cp, 600000)
        assert open(os.path.join(td, "ledger-rows.jsonl"), "rb").read() == first
        # open_progressing: recent progress near run end.
        lp2 = os.path.join(td, "l2.log")
        with open(lp2, "w", encoding="ascii") as fh:
            fh.write(line(1000, "accepted", 1, 1, run="p") + "\n")
            fh.write(line(5000, "blocked", 1, 1, c=(1, 0, 0, 0), reason="r", run="p") + "\n")
            fh.write(line(6000, "accepted", 2, 2, run="p") + "\n")
        _, out2 = run([lp2], os.path.join(td, "o2"), cp, 600000)
        assert {r["quest"]: r["outcome"] for r in out2} == {1: "blocked", 2: "open_progressing"}
    print("selftest OK")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--out-dir", default=".")
    ap.add_argument("--catalog", default=DEFAULT_CATALOG)
    ap.add_argument("--stall-ms", type=int, default=600000,
                    help="open rows without counter change for longer than this before run end are open_stalled")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args(argv)
    if args.selftest:
        return selftest()
    if not args.logs:
        ap.error("no ledger logs given")
    events, out = run(args.logs, args.out_dir, args.catalog, args.stall_ms)
    print("events=%d rows=%d out=%s" % (len(events), len(out), os.path.abspath(args.out_dir)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
