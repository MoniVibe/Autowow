"""Resolve deterministic baselines from the ledger and summarise SHADOW Jev calls."""
import json, os, statistics
from collections import Counter, defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
LEDGER = r"\\wsl.localhost\Ubuntu-24.04\root\autowow-soak\logs\ledger.log"
WINDOW_MS = 15 * 60_000


def load(p):
    return [json.loads(l) for l in open(p, encoding="utf-8") if l.strip()]


def baseline(dp, allow, evs):
    """Map what the deterministic system did next onto the option ids (or 'other'/'unobserved')."""
    after = [e for e in evs if dp["ms"] < e["ms"] <= dp["ms"] + WINDOW_MS]
    q = dp["quest"]
    if dp["kind"] == "stall":
        nxt = next((e for e in after if not (e["ev"] == "blocked" and e["quest"] == q and e["reason"] == dp["reason"])), None)
        if nxt is None or nxt["quest"] == q and nxt["ev"] in ("blocked", "rewarded"):
            return "keep_trying", nxt
        if nxt["quest"] == q and nxt["ev"] in ("deferred", "abandoned"):
            return "defer_quest", nxt
    else:
        nxt = after[0] if after else None
        if nxt is None:
            return "unobserved", None
    x = nxt["quest"]
    if nxt["ev"] == "accepted":
        return ("pickup_new" if "pickup_new" in allow else "other"), nxt
    for cand in (f"turnin_{x}", f"continue_{x}", f"switch_{x}"):
        if cand in allow:
            return cand, nxt
    return "other", nxt


def main():
    evs_by_bot = defaultdict(list)
    first_ms = last_ms = None
    for line in open(LEDGER, encoding="utf-8"):
        i = line.find("{")
        try:
            e = json.loads(line[i:])
        except ValueError:
            continue
        evs_by_bot[e["bot"]].append(e)
        first_ms = e["ms"] if first_ms is None else first_ms
        last_ms = e["ms"]
    dps = {d["id"]: d for d in load(os.path.join(HERE, "decisions.jsonl"))}
    cp = os.path.join(HERE, "calls.jsonl")
    calls = load(cp) if os.path.exists(cp) else []
    dp_path = os.path.join(HERE, "dry_candidates.jsonl")
    dry = load(dp_path) if os.path.exists(dp_path) else []
    by_call = {}
    for c in calls:
        by_call.setdefault(c["call_id"], c)
    hc = list(by_call.values())
    out = {"http_calls": len(hc), "questions": len(calls), "valid_questions": sum(c["valid"] for c in calls),
           "http_errors": [c.get("error") for c in hc if c.get("error")]}
    lat = sorted(c["latency_s"] for c in hc if not c.get("error"))
    q = statistics.quantiles(lat, n=10) if len(lat) >= 2 else lat * 9
    out["latency"] = {"p50": statistics.median(lat), "p90": q[8], "max": max(lat)} if lat else {}
    single = [c for c in hc if c["n_questions"] == 1 and not c.get("error")]
    multi = [c for c in hc if c["n_questions"] > 1 and not c.get("error")]
    avg = lambda xs: sum(xs) / len(xs) if xs else None
    out["tokens"] = {"total": sum(c["tokens"] for c in hc),
                     "per_1q_call_in": avg([c["usage"].get("input_tokens", 0) for c in single]),
                     "per_1q_call_out": avg([c["usage"].get("output_tokens", 0) for c in single]),
                     "per_4q_call": avg([c["tokens"] for c in multi])}
    dl = list(dps.values())
    span_h = (max(d["wall"] for d in dl) - min(d["wall"] for d in dl)) / 3600 if len(dl) > 1 else None
    out["decision_points"] = {"total": len(dl), "by_kind": Counter(d["kind"] for d in dl), "span_h": span_h,
                              "bots_with_dp": len({d["bot"] for d in dl}), "fleet_bots": len(evs_by_bot)}
    if span_h:
        rate = len(dl) / span_h / max(len(evs_by_bot), 1)
        out["dp_per_bot_hour"] = rate
        tpc = (out["tokens"]["per_1q_call_in"] or 0) + (out["tokens"]["per_1q_call_out"] or 0)
        out["projected_200_bots"] = {"decisions_per_hour": rate * 200,
                                     "tokens_per_hour": rate * 200 * tpc}
    # confidence usefulness: does confidence separate agree/disagree with baseline?
    confs = [c["confidence"] for c in calls if c["valid"] and c.get("confidence") is not None]
    out["confidence"] = {"min": min(confs, default=None), "median": statistics.median(confs) if confs else None,
                         "max": max(confs, default=None)}
    plain = [c for c in calls if c["persona"] is None and c["valid"]]
    rows, agree, known = [], 0, 0
    for c in plain:
        dp = dps[c["dp_id"]]
        b, nxt = baseline(dp, c["allow"], evs_by_bot[dp["bot"]])
        later = [e for e in evs_by_bot[dp["bot"]] if e["ms"] > dp["ms"]]
        stall_tail = None
        if dp["kind"] == "stall":
            same = [e["ms"] for e in later if e["ev"] == "blocked" and e["quest"] == dp["quest"] and e["reason"] == dp["reason"]]
            stall_tail = round((max(same) - dp["ms"]) / 60000, 1) if same else 0
        nrew = next((e for e in later if e["ev"] == "rewarded"), None)
        if b not in ("unobserved", "other"):
            known += 1
            agree += b == c["choice"]
        rows.append({"dp": dp["id"], "kind": dp["kind"], "bot": dp["bot"], "lvl": dp["lvl"], "quest": dp["quest"],
                     "reason": dp["reason"], "jev": c["choice"], "conf": c.get("confidence"), "baseline": b,
                     "next_ev": nxt and f'{nxt["ev"]} q{nxt["quest"]} {nxt.get("reason","")}',
                     "stall_persisted_min": stall_tail,
                     "min_to_next_reward": nrew and round((nrew["ms"] - dp["ms"]) / 60000, 1),
                     "options": c["allow"], "extra": c.get("extra")})
    for d in dry:
        dp = dps[d["dp_id"]]
        b, nxt = baseline(dp, list(d["options"]), evs_by_bot[dp["bot"]])
        later = [e for e in evs_by_bot[dp["bot"]] if e["ms"] > dp["ms"]]
        same = [e["ms"] for e in later if dp["kind"] == "stall" and e["ev"] == "blocked"
                and e["quest"] == dp["quest"] and e["reason"] == dp["reason"]]
        nrew = next((e for e in later if e["ev"] == "rewarded"), None)
        rows.append({"dp": dp["id"], "kind": dp["kind"], "bot": dp["bot"], "lvl": dp["lvl"], "quest": dp["quest"],
                     "reason": dp["reason"], "jev": None, "baseline": b,
                     "next_ev": nxt and f'{nxt["ev"]} q{nxt["quest"]} {nxt.get("reason","")}',
                     "stall_persisted_min": round((max(same) - dp["ms"]) / 60000, 1) if same else None,
                     "min_to_next_reward": nrew and round((nrew["ms"] - dp["ms"]) / 60000, 1),
                     "options": list(d["options"])})
    out["baseline_dist"] = Counter(r["baseline"] for r in rows)
    out["agreement"] ={"agree": agree, "comparable": known, "pct": 100 * agree / known if known else None}
    per = defaultdict(dict)
    for c in calls:
        if c["valid"]:
            per[c["dp_id"]][c["persona"] or "neutral"] = c["choice"]
    out["persona"] = {k: v for k, v in per.items() if len(v) > 1}
    out["persona_divergent_points"] = sum(len(set(v.values())) > 1 for v in out["persona"].values())
    cmp = [r for r in rows if r["jev"] and r["baseline"] not in ("unobserved", "other")]
    out["conf_by_agreement"] = {
        "agree": [r["conf"] for r in cmp if r["jev"] == r["baseline"]],
        "disagree": [r["conf"] for r in cmp if r["jev"] != r["baseline"]]}
    out["jev_choice_dist"] = Counter(r["jev"].split("_")[0] for r in rows if r["jev"])
    out["errors"] = [c.get("error") for c in calls if not c["valid"]]
    out["ledger_span_min"] = (last_ms - first_ms) / 60000
    out["rows"] = rows
    json.dump(out, open(os.path.join(HERE, "summary.json"), "w"), indent=1, default=str)
    print(json.dumps({k: v for k, v in out.items() if k != "rows"}, indent=1, default=str))
    for r in rows:
        print(json.dumps(r, default=str))


if __name__ == "__main__":
    main()
