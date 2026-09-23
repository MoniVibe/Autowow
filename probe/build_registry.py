#!/usr/bin/env python3
"""AutoWoW quest completability registry (offline).

Joins the static census (census/quest-catalog.json) with live ledger evidence (folded by
scripts/ledger-reduce.py, imported, never re-implemented) and prior fixture proofs, and assigns
one verdict per quest:

  PROVEN              >= 1 clean reward (ledger outcome `rewarded`) or a named prior fixture proof
  ASSISTED            rewarded only after an assist teleport (`rewarded_assisted`), never clean
  AVOID_UNAVAILABLE   census says unavailable (disabled / no starter / ...) and no reward seen
  FAILING             no reward, >= FAIL_MIN_ROWS failing bot-attempts (rows) and one reason
                      covering >= FAIL_CONSISTENCY of them
  UNSUPPORTED_FAMILY  primary family has no proven executor (census live-proven set) and no reward
  UNTESTED            everything else (includes weak evidence: see confidence note)

Precedence is the order above. Failing rows = outcomes open_stalled, blocked, deferred, abandoned.
open_progressing (inconclusive), contested (died to a player) and contaminated rows are evidence
of attempts but never of failure.

`avoid` is separate from the verdict: a quest is on the suggested AVOID list when it is
AVOID_UNAVAILABLE, UNSUPPORTED_FAMILY, carries a structural flag (group/instance/vehicle/seasonal/
PvP/profession), or is FAILING with a structural reason (executor/oracle/data gaps).

Usage: python build_registry.py [LOG ...] [--catalog PATH] [--out-dir DIR]
       python build_registry.py --selftest
Default logs: the WSL soak archives + live ledger (read over \\\\wsl.localhost). Output is
byte-identical for identical inputs (idempotent; safe to re-run after every soak).
"""

import argparse
import glob
import importlib.util
import json
import os
import sys
import tempfile
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
DEFAULT_CATALOG = os.path.join(ROOT, "census", "quest-catalog.json")
WSL_SOAK = r"\\wsl.localhost\Ubuntu-24.04\root\autowow-soak"
STALL_MS = 600000

VERDICTS = ["PROVEN", "ASSISTED", "FAILING", "UNSUPPORTED_FAMILY", "UNTESTED", "AVOID_UNAVAILABLE"]
FAIL_OUTCOMES = {"open_stalled", "blocked", "deferred", "abandoned"}
FAIL_MIN_ROWS = 3
FAIL_CONSISTENCY = 0.5
# Census section 6: families with a live-proven executor (QUEST_TYPE_COVERAGE_REPORT.md 2026-07-15).
# ESCORT has one proven instance (q435) but no general executor, so it stays unsupported as a family.
SUPPORTED_FAMILIES = {"COLLECT_DROP", "GAMEOBJECT_COLLECT", "GAMEOBJECT_USE", "ITEM_USE_ON_TARGET", "KILL", "TALK_ONLY"}
STRUCTURAL_FLAGS = {"DUNGEON", "RAID", "GROUP_ELITE", "VEHICLE_HEURISTIC", "HOLIDAY", "EVENT_GATED", "PVP", "PROFESSION"}
# Reasons that describe the quest/executor/data, not transient bot state (inventory, pathing jam, respawn race).
STRUCTURAL_REASONS = {"executor_unsupported", "oracle_unrunnable", "oracle_no_candidate", "no_source_spawn",
                      "no_finisher_relation", "interaction_rejected"}
# Named fixture receipts (QUEST_TYPE_COVERAGE_REPORT.md, QUEST_CAPABILITY_MATRIX.md): accepted and rewarded
# through normal play by a fixture bot. Counted as PROVEN evidence when the soak ledger has no clean reward.
PRIOR_PROOFS = {435: "escort fixture", 459: "collect fixture", 783: "talk fixture", 786: "GO-credit fixture",
                788: "turn-in chain fixture", 789: "collect fixture", 792: "kill fixture", 916: "collect fixture",
                917: "GO-collect fixture", 5441: "item-use fixture"}
LEVEL_BANDS = [(1, 10), (11, 20), (21, 30), (31, 40), (41, 50), (51, 60), (61, 70), (71, 80)]


def load_reducer():
    path = os.path.join(ROOT, "scripts", "ledger-reduce.py")
    spec = importlib.util.spec_from_file_location("ledger_reduce", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def default_logs():
    logs = sorted(glob.glob(os.path.join(WSL_SOAK, "archive", "*", "ledger.log")))
    live = os.path.join(WSL_SOAK, "logs", "ledger.log")
    return logs + ([live] if os.path.exists(live) else [])


def quest_level(q):
    return q["questLevel"] if q.get("questLevel", 0) > 0 else q.get("minLevel", 0)


def level_band(lvl):
    for lo, hi in LEVEL_BANDS:
        if lo <= lvl <= hi:
            return "%d-%d" % (lo, hi)
    return "unknown"


# Pipeline stages (shared vocabulary with probe-quest.ps1). Oracle phase -> stage.
STAGES = ["select", "accept", "travel_to_source", "find_target", "interact_credit", "loot",
          "travel_to_finisher", "turn_in", "silent"]
PHASE_STAGE = {"resolve_objective": "select", "travel_to_source": "travel_to_source",
               "acquire_target": "find_target", "wait_for_respawn": "find_target", "self_defense": "find_target",
               "engage_target": "interact_credit", "interact_source": "interact_credit",
               "use_quest_item": "interact_credit", "escort_event": "interact_credit",
               "verify_progress": "interact_credit", "loot_source": "loot", "resolve_finisher": "travel_to_finisher",
               "travel_to_finisher": "travel_to_finisher", "interact_finisher": "turn_in", "verify_reward": "turn_in"}
# A reason overrides the phase when it names the real stage (resolve_objective is a catch-all phase).
REASON_STAGE = {"inventory_full": "loot", "no_source_spawn": "find_target", "no_live_candidate": "find_target",
                "interaction_rejected": "interact_credit", "progress_did_not_change": "interact_credit",
                "no_finisher_relation": "travel_to_finisher", "travel_no_progress": None,
                "drop_unworthy_or_failed": "select"}


def row_signature(r, activity):
    """(stage, reason) for one failing row. Silent open_stalled rows (no reason ever emitted) split into
    fixation_starved (same bot rewarded/progressed OTHER quests after this accept while this row never
    moved), silent_after_progress, and silent_no_progress (waypoint-bounce suspect: never emits a reason)."""
    o = r["outcome"]
    reason = r["blocked_last_reason"] if o == "blocked" else r["deferred_reason"] if o in ("deferred", "abandoned") \
        else r["last_reason"]
    if not reason:
        if r["first_progress_at"] is not None or r.get("progress_events"):
            return "silent", "silent_after_progress"
        acc = r["accepted_at"] or 0
        if any(ms > acc and q != r["quest"] for ms, q in activity.get((r["run"], r["bot"]), ())):
            return "select", "fixation_starved"
        return "silent", "silent_no_progress"
    stage = REASON_STAGE.get(reason) or PHASE_STAGE.get(r["last_phase"] or "", None)
    if stage is None:
        stage = "travel_to_source" if reason in ("travel_no_progress", "movement_stuck_no_teleport",
                                                  "oracle_route_blocked") else "select"
    return stage, reason


def annotate(rows, progress):
    """Attach `sig` to failing rows. progress: {(run, bot, quest): [ms, ...]} from ledger `progress` events."""
    activity = defaultdict(list)
    for r in rows:
        key = (r["run"], r["bot"])
        for f in ("rewarded_at", "last_progress_at"):
            if r[f] is not None:
                activity[key].append((r[f], r["quest"]))
        r["progress_events"] = len(progress.get((r["run"], r["bot"], r["quest"]), ()))
    for (run_, bot, quest), times in progress.items():
        activity[(run_, bot)] += [(ms, quest) for ms in times]
    for r in rows:
        r["sig"] = "%s:%s" % row_signature(r, activity) if r["outcome"] in FAIL_OUTCOMES else None
    return rows


def collect_progress(events):
    """Ledger `progress` events (event id 9, objective counter changes; module main 286b5511+).
    The v1 reducer ignores unknown event names, so they are folded here."""
    out = defaultdict(list)
    for ev in events:
        if ev.get("ev") == "progress" and ev.get("quest"):
            out[(ev.get("run", ""), ev["bot"], ev["quest"])].append(ev["ms"])
    return out


def classify(q, ev):
    """Return (verdict, confidence note, avoid reasons) for one catalog quest + aggregated evidence."""
    fail_rows, top_reason, top_n = ev["failing"], None, 0
    if ev["reasons"]:
        top_reason, top_n = sorted(ev["reasons"].items(), key=lambda kv: (-kv[1], kv[0]))[0]
    prior = PRIOR_PROOFS.get(q["id"])
    if ev["rewarded"] >= 1:
        verdict, note = "PROVEN", "%d clean reward(s) by %d bot(s)" % (ev["rewarded"], len(ev["rewarded_bots"]))
    elif prior:
        verdict, note = "PROVEN", "prior %s receipt (no clean soak reward yet)" % prior
    elif ev["assisted"] >= 1:
        verdict, note = "ASSISTED", "%d reward(s), all after an assist teleport" % ev["assisted"]
    elif not q.get("available", True):
        verdict, note = "AVOID_UNAVAILABLE", "census: %s" % q.get("unavailableReason", "")
    elif fail_rows >= FAIL_MIN_ROWS and top_n >= FAIL_CONSISTENCY * fail_rows:
        verdict, note = "FAILING", "%d failing bot-attempts, %d/%d %s" % (fail_rows, top_n, fail_rows, top_reason)
    elif q["family"] not in SUPPORTED_FAMILIES:
        verdict, note = "UNSUPPORTED_FAMILY", "family %s has no proven executor" % q["family"]
    elif fail_rows:
        verdict, note = "UNTESTED", "weak: %d failing bot-attempt(s), below FAILING bar" % fail_rows
    elif ev["rows"]:
        verdict, note = "UNTESTED", "inconclusive: %d attempt row(s), none terminal" % ev["rows"]
    else:
        verdict, note = "UNTESTED", "no live evidence"
    avoid = []
    if verdict == "AVOID_UNAVAILABLE":
        avoid.append("unavailable")
    if verdict == "UNSUPPORTED_FAMILY":
        avoid.append("family:" + q["family"])
    if verdict not in ("PROVEN", "ASSISTED"):
        avoid += ["flag:" + f for f in sorted(set(q.get("flags", [])) & STRUCTURAL_FLAGS)]
        if verdict == "FAILING" and top_reason.split(":")[-1] in STRUCTURAL_REASONS:
            avoid.append("failing:" + top_reason.split(":")[-1])
    return verdict, note, avoid


def aggregate(rows):
    ev = defaultdict(lambda: {"rows": 0, "attempts": 0, "bots": set(), "runs": set(), "rewarded": 0,
                              "rewarded_bots": set(), "assisted": 0, "failing": 0, "reasons": Counter(),
                              "outcomes": Counter(), "progress_events": 0})
    for r in rows:
        e = ev[r["quest"]]
        e["rows"] += 1
        e["attempts"] += r["attempts"]
        e["bots"].add(r["bot"])
        e["runs"].add(r["run"])
        e["outcomes"][r["outcome"]] += 1
        if r["outcome"] == "rewarded":
            e["rewarded"] += 1
            e["rewarded_bots"].add(r["bot"])
        elif r["outcome"] == "rewarded_assisted":
            e["assisted"] += 1
        elif r["outcome"] in FAIL_OUTCOMES:
            e["failing"] += 1
            e["reasons"][r["sig"]] += 1
        e["progress_events"] += r.get("progress_events", 0)
    return ev


def build(catalog, rows):
    ev = aggregate(rows)
    out = []
    for q in sorted(catalog, key=lambda q: q["id"]):
        e = ev[q["id"]]
        verdict, note, avoid = classify(q, e)
        out.append({
            "id": q["id"], "title": q.get("title", ""), "verdict": verdict, "confidence": note,
            "avoid": bool(avoid), "avoidReasons": avoid,
            "family": q["family"], "flags": q.get("flags", []), "level": quest_level(q),
            "minLevel": q.get("minLevel", 0), "zoneId": q.get("zoneId", 0), "zone": q.get("zoneName", ""),
            "faction": q.get("faction", ""), "available": q.get("available", True),
            "evidence": {
                "rows": e["rows"], "attempts": e["attempts"], "bots": len(e["bots"]), "runs": sorted(e["runs"]),
                "rewarded": e["rewarded"], "assisted": e["assisted"], "failing": e["failing"],
                "progressEvents": e["progress_events"],
                "outcomes": dict(sorted(e["outcomes"].items())),
                "topReasons": [[k, n] for k, n in sorted(e["reasons"].items(), key=lambda kv: (-kv[1], kv[0]))[:3]],
                "priorProof": PRIOR_PROOFS.get(q["id"]),
            },
        })
    return out


def table(title, rows_keys, cols, cell, note=""):
    lines = ["", "## " + title, ""] + ([note, ""] if note else [])
    lines.append("| | " + " | ".join(cols) + " | total |")
    lines.append("|---|" + "---:|" * (len(cols) + 1))
    col_tot = Counter()
    for k in rows_keys:
        vals = [cell(k, c) for c in cols]
        for c, v in zip(cols, vals):
            col_tot[c] += v
        lines.append("| %s | %s | %d |" % (k, " | ".join(str(v) for v in vals), sum(vals)))
    lines.append("| **total** | %s | **%d** |" % (" | ".join("**%d**" % col_tot[c] for c in cols), sum(col_tot.values())))
    return lines, sum(col_tot.values())


# Known cross-type defects (research/quests/STARTER_STALLS.md) -> signature predicate on (family, stage, reason).
KNOWN_ISSUES = [
    ("travel waypoint bounce (no reason emitted; travel_no_progress after 286b5511)",
     lambda fam, st, rs: rs in ("silent_no_progress", "travel_no_progress", "movement_stuck_no_teleport")),
    ("quest fixation (one directive starves the rest of the log)", lambda fam, st, rs: rs == "fixation_starved"),
    ("quest-item target conditions (sleeping peon 5441, spawnless credit NPC 9303)",
     lambda fam, st, rs: fam == "ITEM_USE_ON_TARGET" and st in ("find_target", "interact_credit")),
    ("friendly-NPC spell credit (9283)", lambda fam, st, rs: fam == "SPELL_CREDIT"),
    ("route/navmesh blocked (oracle_route_blocked)", lambda fam, st, rs: rs == "oracle_route_blocked"),
    ("respawn/claim contention (no_live_candidate)", lambda fam, st, rs: rs == "no_live_candidate"),
]


def signature_sections(reg, rows):
    fam_of = {r["id"]: r["family"] for r in reg}
    failing_q = {r["id"]: r for r in reg if r["verdict"] == "FAILING"}
    sig_rows, sig_q, sig_f = Counter(), defaultdict(set), defaultdict(Counter)
    for r in rows:
        if not r.get("sig"):
            continue
        sig_rows[r["sig"]] += 1
        sig_q[r["sig"]].add(r["quest"])
        sig_f[r["sig"]][fam_of.get(r["quest"], "UNKNOWN")] += 1
    total = sum(sig_rows.values())
    dom = Counter(q["evidence"]["topReasons"][0][0] for q in failing_q.values())
    lines = ["", "## Cross-family failure signatures (stage x reason)", "",
             "Every failing ledger row (open_stalled, blocked, deferred, abandoned) gets one signature. Stage = oracle phase "
             "(reason overrides when it names the real stage). Silent rows never emit a reason: `fixation_starved` = the same "
             "bot rewarded or progressed another quest after accepting this one; `silent_no_progress` = no counter ever moved "
             "(waypoint-bounce suspect); `silent_after_progress` = moved then went quiet. Sorted by families spanned: a fix at "
             "that stage hits every family listed.", "",
             "| stage | reason | rows | quests | families | FAILING quests (dominant) | family mix (rows) |",
             "|---|---|---:|---:|---:|---:|---|"]
    for sig in sorted(sig_rows, key=lambda k: (-len(sig_f[k]), -sig_rows[k], k)):
        st, rs = sig.split(":", 1)
        mix = ", ".join("%s %d" % kv for kv in sorted(sig_f[sig].items(), key=lambda kv: (-kv[1], kv[0])))
        lines.append("| %s | %s | %d | %d | %d | %d | %s |" % (st, rs, sig_rows[sig], len(sig_q[sig]), len(sig_f[sig]),
                                                            dom[sig], mix))
    lines.append("| **total** | | **%d** | | | **%d** | |" % (total, sum(dom.values())))
    assert sum(dom.values()) == len(failing_q)
    st_rows = Counter()
    for sig, n in sig_rows.items():
        st_rows[sig.split(":", 1)[0]] += n
    lines += ["", "Failing rows by stage (sums to %d): " % total +
              ", ".join("%s %d" % (st, st_rows[st]) for st in STAGES if st_rows[st]) + "."]

    lines += ["", "## Known cross-type defects mapped to signatures", "",
              "`FAILING (dominant)` = FAILING quests whose dominant signature matches (no quest counted twice within a row; "
              "rows of this table overlap by design and do not sum). `touched` = quests with >= 1 matching failing row "
              "(any verdict). `rows` = matching failing rows.", "",
              "| defect | FAILING (dominant) | touched quests | rows | families |", "|---|---:|---:|---:|---:|"]
    for name, pred in KNOWN_ISSUES:
        d = sum(1 for q in failing_q.values()
                if pred(q["family"], *q["evidence"]["topReasons"][0][0].split(":", 1)))
        touched, n, fams = set(), 0, set()
        for r in rows:
            if r.get("sig") and pred(fam_of.get(r["quest"], "UNKNOWN"), *r["sig"].split(":", 1)):
                touched.add(r["quest"])
                n += 1
                fams.add(fam_of.get(r["quest"], "UNKNOWN"))
        lines.append("| %s | %d | %d | %d | %d |" % (name, d, len(touched), n, len(fams)))
    covered = sum(1 for q in failing_q.values()
                  if any(pred(q["family"], *q["evidence"]["topReasons"][0][0].split(":", 1)) for _, pred in KNOWN_ISSUES))
    lines += ["", "FAILING quests covered by at least one known defect: %d of %d (uncovered %d)." % (
        covered, len(failing_q), len(failing_q) - covered)]
    return lines


def summarize(reg, sources, rows=()):
    n = len(reg)
    by_v = Counter(r["verdict"] for r in reg)
    lines = ["# AutoWoW quest completability registry", "",
             "Generated by `probe/build_registry.py` (offline; census + ledger + prior fixture receipts). "
             "Verdict rules are in the script docstring. Evidence sources:", ""]
    lines += ["- `%s`" % s for s in sources]
    lines += ["", "Profile: every run folded by `scripts/ledger-reduce.py` (stall-ms %d; setup teleports voided). "
              "S1-S4 were random-bot soaks with forced-teleport setup noise; S5 = 50-bot cohort. "
              "A live soak still running makes its open rows `open_progressing` (inconclusive, not failing)." % STALL_MS]
    lines += ["", "## Verdict totals", "", "| verdict | quests |", "|---|---:|"]
    for v in VERDICTS:
        lines.append("| %s | %d |" % (v, by_v[v]))
    lines.append("| **total** | **%d** |" % n)
    assert sum(by_v.values()) == n

    fams = sorted({r["family"] for r in reg}, key=lambda f: (-sum(1 for r in reg if r["family"] == f), f))
    fv = Counter((r["family"], r["verdict"]) for r in reg)
    t, tot = table("Family x verdict", fams, VERDICTS, lambda k, c: fv[(k, c)])
    assert tot == n
    lines += t
    bands = ["%d-%d" % b for b in LEVEL_BANDS] + ["unknown"]
    bv = Counter((level_band(r["level"]), r["verdict"]) for r in reg)
    t, tot = table("Level band x verdict", bands, VERDICTS, lambda k, c: bv[(k, c)],
                   "Level = QuestLevel, or MinLevel when QuestLevel <= 0 (scaling quests).")
    assert tot == n
    lines += t

    tested = [r for r in reg if r["evidence"]["rows"]]
    lines += ["", "## Live-tested quests by verdict", "",
              "%d quests have >= 1 ledger row; of which PROVEN %d, ASSISTED %d, FAILING %d, UNTESTED (weak or "
              "inconclusive) %d, other %d." % (
                  len(tested), sum(r["verdict"] == "PROVEN" for r in tested), sum(r["verdict"] == "ASSISTED" for r in tested),
                  sum(r["verdict"] == "FAILING" for r in tested), sum(r["verdict"] == "UNTESTED" for r in tested),
                  sum(r["verdict"] in ("UNSUPPORTED_FAMILY", "AVOID_UNAVAILABLE") for r in tested))]

    failing = sorted((r for r in reg if r["verdict"] == "FAILING"),
                     key=lambda r: (r["family"], -r["evidence"]["failing"], r["id"]))
    top = sorted(failing, key=lambda r: (-r["evidence"]["failing"], r["id"]))[:30]
    top_ids = {r["id"] for r in top}
    lines += ["", "## Fix queue: top 30 FAILING quests (by failing bot-attempts), grouped by family", "",
              "Showing %d of %d FAILING quests." % (len(top), len(failing)), "",
              "| family | quest | title | lvl | zone | fail rows | bots | top reasons | avoid |",
              "|---|---:|---|---:|---|---:|---:|---|---|"]
    for r in failing:
        if r["id"] not in top_ids:
            continue
        e = r["evidence"]
        lines.append("| %s | %d | %s | %d | %s | %d | %d | %s | %s |" % (
            r["family"], r["id"], r["title"].replace("|", "/"), r["level"], r["zone"], e["failing"], e["bots"],
            ", ".join("%s %d" % (k, v) for k, v in e["topReasons"]), "yes" if r["avoid"] else ""))
    fr = Counter()
    for r in failing:
        fr[(r["family"], r["evidence"]["topReasons"][0][0])] += 1
    lines += ["", "FAILING quests by family x dominant signature (sums to %d):" % len(failing), "",
              "| family | dominant signature | quests |", "|---|---|---:|"]
    for (f, rsn), k in sorted(fr.items(), key=lambda kv: (kv[0][0], -kv[1], kv[0][1])):
        lines.append("| %s | %s | %d |" % (f, rsn, k))
    lines.append("| **total** | | **%d** |" % sum(fr.values()))

    lines += signature_sections(reg, rows)

    avoid = [r for r in reg if r["avoid"]]
    primary = Counter(r["avoidReasons"][0].split(":")[0] for r in avoid)
    lines += ["", "## Suggested AVOID list", "",
              "%d quests (full list = `avoid: true` in quest-registry.json; `avoidReasons[0]` is the primary)." % len(avoid),
              "", "| primary reason | quests |", "|---|---:|"]
    for k in sorted(primary):
        lines.append("| %s | %d |" % (k, primary[k]))
    lines.append("| **total** | **%d** |" % len(avoid))
    av = Counter(r["verdict"] for r in avoid)
    lines += ["", "AVOID by verdict: " + ", ".join("%s %d" % (v, av[v]) for v in VERDICTS if av[v]) +
              " (sum %d). Available-but-avoided (what a selector would actually skip): %d." % (
                  sum(av.values()), sum(1 for r in avoid if r["available"]))]
    fa = [r for r in failing if r["avoid"]]
    if fa:
        lines += ["", "FAILING quests on the AVOID list (structural reason or flag): " +
                  ", ".join("%d (%s)" % (r["id"], ",".join(r["avoidReasons"])) for r in fa)]
    return "\n".join(lines) + "\n"


def run(logs, catalog_path, out_dir):
    red = load_reducer()
    with open(catalog_path, encoding="utf-8") as fh:
        catalog = json.load(fh)["quests"]
    events = red.read_events(logs)
    folded, run_end, _, _, _ = red.fold(events)
    rows = red.reduce_rows(folded, run_end, {q["id"]: q for q in catalog}, STALL_MS)
    annotate(rows, collect_progress(events))
    reg = build(catalog, rows)
    sources = [os.path.basename(os.path.dirname(p)) + "/" + os.path.basename(p) for p in logs] + \
              ["census/quest-catalog.json (%d quests)" % len(catalog), "PRIOR_PROOFS (%d fixture receipts)" % len(PRIOR_PROOFS)]
    os.makedirs(out_dir, exist_ok=True)
    doc = {"schema": 1, "generator": "probe/build_registry.py", "sources": sources,
           "rules": {"failMinRows": FAIL_MIN_ROWS, "failConsistency": FAIL_CONSISTENCY, "stallMs": STALL_MS,
                     "supportedFamilies": sorted(SUPPORTED_FAMILIES), "structuralFlags": sorted(STRUCTURAL_FLAGS),
                     "structuralReasons": sorted(STRUCTURAL_REASONS)},
           "count": len(reg), "ledgerRows": len(rows), "quests": reg}
    with open(os.path.join(out_dir, "quest-registry.json"), "w", encoding="ascii", newline="\n") as fh:
        json.dump(doc, fh, indent=1, ensure_ascii=True)
        fh.write("\n")
    with open(os.path.join(out_dir, "avoid-quest-ids.txt"), "w", encoding="ascii", newline="\n") as fh:
        fh.write(",".join(str(r["id"]) for r in reg if r["avoid"] and r["available"]) + "\n")
    with open(os.path.join(out_dir, "REGISTRY_SUMMARY.md"), "w", encoding="ascii", newline="\n") as fh:
        fh.write(summarize(reg, sources, rows))
    return reg, rows


def selftest():
    def q(i, fam="KILL", avail=True, flags=(), lvl=5):
        return {"id": i, "title": "q%d" % i, "family": fam, "flags": list(flags), "questLevel": lvl, "minLevel": 1,
                "zoneId": 12, "zoneName": "Elwynn Forest", "faction": "Alliance", "available": avail,
                "unavailableReason": "" if avail else "DISABLES_TABLE"}

    def line(ms, ev, bot, quest, reason="", run="t", **kw):
        o = {"v": 1, "run": run, "ms": ms, "ev": ev, "bot": bot, "team": 0, "lvl": 5, "quest": quest, "map": 0,
             "zone": 12, "x": 0, "y": 0, "c": [0, 0, 0, 0], "i": [0] * 6, "reason": reason, "phase": ""}
        o.update(kw)
        return "2026-09-23 01:00:00 " + json.dumps(o)

    cat = [q(1), q(2), q(3, fam="ESCORT"), q(4, avail=False), q(5), q(6, flags=["GROUP_ELITE"]), q(7), q(8),
           q(783), q(9, fam="EVENT_CREDIT"), q(10, lvl=0)]
    log = [line(1, "accepted", 1, 1), line(2, "rewarded", 1, 1)]                       # 1 PROVEN
    log += [line(1, "accepted", 50, 2), line(2, "contaminated", 50, 0, reason="zone_travel_assist"),
            line(3, "rewarded", 50, 2)]                                                  # 2 ASSISTED
    for b in (1, 2, 3):                                                                 # 5 FAILING structural
        log += [line(1, "accepted", b, 5), line(2, "deferred", b, 5, reason="executor_unsupported")]
    for b in (1, 2, 3):                                                                 # 7: 3 rows, no consistent reason
        log += [line(1, "accepted", b, 7), line(2, "blocked", b, 7, reason="r%d" % b)]
    log += [line(1, "accepted", 1, 8), line(2, "blocked", 1, 8, reason="inventory_full")]  # 8 weak UNTESTED
    for b in (1, 2, 3):                                                                 # 9 FAILING beats family
        log += [line(1, "accepted", b, 9), line(2, "blocked", b, 9, reason="no_live_candidate")]
    log += [line(1, "accepted", 60, 7001), line(1, "accepted", 60, 7002),              # fixation: 7002 rewarded,
            line(5, "rewarded", 60, 7002)]                                              # 7001 starved
    log += [line(1, "accepted", 61, 7003), line(2, "progress", 61, 7003, c=[1, 0, 0, 0])]  # progress then silence
    log += [line(1, "accepted", 62, 7004),
            line(2, "blocked", 62, 7004, reason="travel_no_progress", phase="travel_to_source")]
    log.append(line(10 ** 7, "accepted", 99, 999))                                      # push run end
    with tempfile.TemporaryDirectory() as td:
        lp, cp = os.path.join(td, "l.log"), os.path.join(td, "c.json")
        open(lp, "w", encoding="ascii").write("\n".join(log) + "\n")
        json.dump({"quests": cat}, open(cp, "w", encoding="ascii"))
        reg, rows = run([lp], cp, td)
        got = {r["id"]: r for r in reg}
        expect = {1: "PROVEN", 2: "ASSISTED", 3: "UNSUPPORTED_FAMILY", 4: "AVOID_UNAVAILABLE", 5: "FAILING",
                  6: "UNTESTED", 7: "UNTESTED", 8: "UNTESTED", 783: "PROVEN", 9: "FAILING", 10: "UNTESTED"}
        for k, v in expect.items():
            assert got[k]["verdict"] == v, (k, got[k]["verdict"], v)
        assert got[5]["avoidReasons"] == ["failing:executor_unsupported"]
        assert got[9]["avoidReasons"] == [] and not got[9]["avoid"]
        assert got[6]["avoidReasons"] == ["flag:GROUP_ELITE"]
        assert got[3]["avoid"] and got[4]["avoid"] and not got[1]["avoid"]
        assert got[5]["evidence"]["failing"] == 3 and got[5]["evidence"]["bots"] == 3
        assert got[783]["evidence"]["priorProof"] == "talk fixture"
        assert got[10]["level"] == 1
        sig = {r["quest"]: r["sig"] for r in rows}
        assert sig[7001] == "select:fixation_starved", sig[7001]
        assert sig[7003] == "silent:silent_after_progress", sig[7003]
        assert sig[7004] == "travel_to_source:travel_no_progress", sig[7004]
        assert sig[5] == "select:executor_unsupported" and sig[8] == "loot:inventory_full"
        assert got[5]["confidence"].endswith("select:executor_unsupported")
        md = open(os.path.join(td, "REGISTRY_SUMMARY.md"), encoding="ascii").read()
        assert "| **total** | **%d** |" % len(cat) in md
        assert "## Cross-family failure signatures" in md and "fixation_starved" in md
        ids = open(os.path.join(td, "avoid-quest-ids.txt")).read().strip()
        assert ids == "3,5,6", ids  # 4 is unavailable (never offered), 9 FAILING non-structural
        first = open(os.path.join(td, "quest-registry.json"), "rb").read()
        run([lp], cp, td)
        assert open(os.path.join(td, "quest-registry.json"), "rb").read() == first  # idempotent
    print("selftest OK")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("logs", nargs="*")
    ap.add_argument("--catalog", default=DEFAULT_CATALOG)
    ap.add_argument("--out-dir", default=HERE)
    ap.add_argument("--selftest", action="store_true")
    a = ap.parse_args(argv)
    if a.selftest:
        return selftest()
    logs = a.logs or default_logs()
    if not logs:
        ap.error("no ledger logs given and none found under " + WSL_SOAK)
    reg, rows = run(logs, a.catalog, a.out_dir)
    print("quests=%d ledger_rows=%d logs=%d out=%s" % (len(reg), len(rows), len(logs), os.path.abspath(a.out_dir)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
