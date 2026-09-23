"""SHADOW-MODE Jev decision harness for AutoWoW.

Jev decides; NOTHING is applied to the game. Bridge access is restricted to the
read-only 'questlog' verb. Outputs: decisions.jsonl (every detected decision
point) and calls.jsonl (every Jev call). Baselines are resolved later by
analyze.py from what the ledger shows the deterministic system did next.
"""
import json, os, socket, sys, time, urllib.request, urllib.error
from collections import defaultdict

HERE = os.path.dirname(os.path.abspath(__file__))
LEDGER = r"\\wsl.localhost\Ubuntu-24.04\root\autowow-soak\logs\ledger.log"
CATALOG = r"D:\Games\wowstuff\AutoWoW\census\quest-catalog.json"
KEY_FILE = r"C:\dev\SGSurvivors\docs\jevapi.md"
JEV_URL = "https://api.typesafe.ai/v1/systemone"

RUN_SECONDS = int(sys.argv[1]) if len(sys.argv) > 1 else 2700
MAX_CALLS, MAX_TOKENS = 30, 38_000  # 10 HTTP calls (2 ok s1, 7 CF-403, 1 probe) / ~1.7k tokens already spent
JEV_MIN_GAP = 5.0          # hard rate limit between Jev calls
PLAIN_GAP = 60.0           # spread plain decisions across the run
BRIDGE_MIN_GAP = 1.1       # <= 1 bridge call/sec
STALL_N = 5
PERSONA_EVERY = 3          # every 3rd decision with >=3 options: one call, neutral + 3 persona questions
PERSONAS = ["cautious", "greedy", "reckless explorer"]
PERSONA_POINTS_MAX = 5
BOT_COOLDOWN_MS = 600_000  # one Jev decision per bot per 10 min (diversity)
READ_ONLY_VERBS = {"questlog"}
DRY = os.environ.get("JEV_SHADOW_DRY") == "1"  # no Jev calls; detector + candidate builder only

API_KEY = open(KEY_FILE, encoding="utf-8").read().strip()


def scrub(s):
    return str(s).replace(API_KEY, "<redacted>") if API_KEY else str(s)


cat = json.load(open(CATALOG, encoding="utf-8"))["quests"]
QUEST = {q["id"]: q for q in cat}
ZONE = {}
for q in cat:
    if q.get("zoneName"):
        ZONE.setdefault(q["zoneId"], q["zoneName"])

_last_bridge = 0.0


def bridge(verb, guid):
    global _last_bridge
    assert verb in READ_ONLY_VERBS, "read-only verbs only"
    wait = BRIDGE_MIN_GAP - (time.time() - _last_bridge)
    if wait > 0:
        time.sleep(wait)
    _last_bridge = time.time()
    with socket.create_connection(("127.0.0.1", 18787), timeout=5) as s:
        s.sendall(f"{verb} {int(guid)}\n".encode())
        f = s.makefile("r", encoding="utf-8")
        return json.loads(f.readline())


def qfacts(qid, logq=None):
    c = QUEST.get(qid, {})
    d = {"id": qid, "title": c.get("title", "?"), "family": c.get("family", "?"),
         "quest_level": c.get("questLevel"), "zone": c.get("zoneName", "?")}
    if logq:
        objs = logq.get("objectives", [])
        d["progress"] = [f'{o.get("kind")}:{o.get("current")}/{o.get("required")}' for o in objs]
        d["complete"] = bool(logq.get("is_complete") or logq.get("objectives_all_done"))
        d["supported"] = logq.get("supported", True)
    return d


def build_options(dp, qlog):
    lvl, q = dp["lvl"], dp["quest"]
    others = [x for x in qlog if x["id"] != q]
    complete = [x for x in others if x.get("is_complete") or x.get("objectives_all_done")]
    openq = [x for x in others if x not in complete and x.get("supported", True)]
    openq.sort(key=lambda x: abs((x.get("level") or lvl) - lvl))
    opts = {}
    if dp["kind"] == "stall":
        cur = next((x for x in qlog if x["id"] == q), None)
        opts["keep_trying"] = {"desc": f"Keep trying quest {q} despite repeated '{dp['reason']}' blocks", **qfacts(q, cur)}
        if dp["reason"] == "inventory_full":
            opts["clear_bags"] = {"desc": "Vendor/destroy junk to free bag space, then resume the same quest"}
        for x in complete[:1]:
            opts[f"turnin_{x['id']}"] = {"desc": f"Go turn in completed quest {x['id']}", **qfacts(x["id"], x)}
        for x in openq[: 5 - len(opts) - 2]:
            opts[f"switch_{x['id']}"] = {"desc": f"Switch to open quest {x['id']}", **qfacts(x["id"], x)}
        opts["defer_quest"] = {"desc": f"Defer quest {q} for later and move on"}
        opts["grind_nearby"] = {"desc": "Grind nearby mobs for XP instead of questing"}
    else:
        for x in complete[:2]:
            opts[f"turnin_{x['id']}"] = {"desc": f"Go turn in completed quest {x['id']}", **qfacts(x["id"], x)}
        for x in openq[: max(0, 3 - len(opts))]:
            opts[f"continue_{x['id']}"] = {"desc": f"Work on open quest {x['id']}", **qfacts(x["id"], x)}
        opts["pickup_new"] = {"desc": "Find a quest giver and accept a new quest"}
        opts["grind_nearby"] = {"desc": "Grind nearby mobs for XP"}
    return dict(list(opts.items())[:5])


def _question(dp, opts, persona):
    if persona:
        instr = (f"Choose exactly one next action for a WoW WotLK leveling bot whose personality is "
                 f"'{persona}'. Play in character; the choice should reflect that personality.")
    else:
        instr = ("Choose exactly one next action for a WoW WotLK leveling bot. Goal: efficient leveling "
                 "(XP per minute, avoid stalls and wasted travel). SHADOW advisory only.")
    return {"type": "choice", "instructions": instr, "criteria": {k: v["desc"] for k, v in opts.items()}}


def call_jev(dp, opts, personas):
    """One HTTP call; one question per persona (None = neutral efficiency question).
    Returns (records, total_tokens). One record per question, sharing call_id."""
    state = {"bot_level": dp["lvl"], "zone": ZONE.get(dp["zone"], str(dp["zone"])),
             "trigger": dp["kind"], "trigger_quest": qfacts(dp["quest"]),
             "options": {k: {kk: vv for kk, vv in v.items() if kk != "desc"} for k, v in opts.items()}}
    if dp["kind"] == "stall":
        state["stall"] = {"reason": dp["reason"], "phase": dp["phase"], "repeats": dp["count"]}
    if dp["kind"] in ("deferred", "abandoned"):
        state["reason"] = dp["reason"]
    qids = {(p or "neutral").replace(" ", "_"): p for p in personas}
    req = {"model": "jev-latest", "state": "AutoWoW bot decision context (JSON): " + json.dumps(state),
           "questions": {q: _question(dp, opts, p) for q, p in qids.items()}}
    base = {"call_id": f"{dp['id']}-{int(time.time())}", "t": time.time(), "dp_id": dp["id"],
            "allow": list(opts), "n_questions": len(qids), "request_state": state}
    body, err, t0 = None, None, time.time()
    for attempt in range(3):
        t0 = time.time()
        try:
            r = urllib.request.Request(JEV_URL, data=json.dumps(req).encode(), method="POST",
                                       headers={"Authorization": f"Bearer {API_KEY}", "Content-Type": "application/json",
                                                "User-Agent": "AutoWoW-JevShadow/1.0"})  # Cloudflare 1010 blocks Python-urllib UA
            with urllib.request.urlopen(r, timeout=30) as resp:
                body = json.loads(resp.read().decode())
            err = None
            break
        except urllib.error.HTTPError as e:
            err = scrub(f"HTTP {e.code}: {e.read()[:300]!r}")
            if e.code not in (429, 529):
                break
            time.sleep(10 * (attempt + 1))
        except Exception as e:
            err = scrub(repr(e))[:300]
            break
    base["latency_s"] = round(time.time() - t0, 3)
    usage = (body or {}).get("usage") or {}
    tokens = int(usage.get("input_tokens", 0)) + int(usage.get("output_tokens", 0))
    base.update(model=(body or {}).get("model"), usage=usage, tokens=tokens)
    recs = []
    for q, p in qids.items():
        rec = dict(base, persona=p)
        if err:
            rec.update(valid=False, error=err)
        else:
            ans = body.get("answers", {}).get(q, {})
            rec.update(choice=ans.get("choice"), confidence=ans.get("confidence"),
                       probabilities=ans.get("probabilities"),
                       extra={k: v for k, v in ans.items() if k not in ("choice", "confidence", "probabilities", "type")})
            rec["valid"] = ans.get("type") == "choice" and ans.get("choice") in opts
        recs.append(rec)
    with open(os.path.join(HERE, "calls.jsonl"), "a", encoding="utf-8") as f:
        for rec in recs:
            f.write(json.dumps(rec) + "\n")
    return recs, tokens


def parse(line):
    i = line.find("{")
    if i < 0:
        return None
    try:
        return json.loads(line[i:])
    except ValueError:
        return None


def main():
    start = time.time()
    calls, tokens, plain_n, persona_points, hard_errs = 0, 0, 0, 0, 0
    last_jev, last_plain = 0.0, 0.0
    streak = {}                       # bot -> (quest, reason, count, fired)
    bot_last_dp = defaultdict(lambda: -10**12)
    dp_seq = 0
    f = open(LEDGER, encoding="utf-8")
    f.seek(0, 2)
    buf = ""
    dout = open(os.path.join(HERE, "decisions.jsonl"), "a", encoding="utf-8")
    print(f"shadow harness up; run {RUN_SECONDS}s", flush=True)
    while time.time() - start < RUN_SECONDS and calls < MAX_CALLS and tokens < MAX_TOKENS:
        chunk = f.read()
        if not chunk:
            if os.path.getsize(LEDGER) < f.tell():  # ledger truncated by a soak restart: follow from 0
                print("ledger truncated; re-reading from start", flush=True)
                f.seek(0); buf = ""; streak.clear(); bot_last_dp.clear()
            time.sleep(2)
            continue
        buf += chunk
        lines = buf.split("\n")
        buf = lines.pop()
        for line in lines:
            e = parse(line)
            if not e or "bot" not in e:
                continue
            bot, ev = e["bot"], e["ev"]
            dp = None
            if ev == "blocked":
                q, rs, n, fired = streak.get(bot, (None, None, 0, False))
                if (q, rs) == (e["quest"], e["reason"]):
                    n += 1
                else:
                    q, rs, n, fired = e["quest"], e["reason"], 1, False
                if n >= STALL_N and not fired:
                    fired = True
                    dp = {"kind": "stall", "count": n}
                streak[bot] = (q, rs, n, fired)
            else:
                streak.pop(bot, None)
                if ev in ("rewarded", "deferred", "abandoned"):
                    dp = {"kind": ev}
            if not dp:
                continue
            dp_seq += 1
            dp.update(id=dp_seq, bot=bot, team=e["team"], lvl=e["lvl"], quest=e["quest"], map=e["map"],
                      zone=e["zone"], ms=e["ms"], reason=e.get("reason", ""), phase=e.get("phase", ""),
                      wall=time.time())
            now = time.time()
            eligible = (e["ms"] - bot_last_dp[bot] >= BOT_COOLDOWN_MS and now - last_plain >= PLAIN_GAP
                        and calls < MAX_CALLS and tokens < MAX_TOKENS)
            dp["jev"] = eligible
            dout.write(json.dumps(dp) + "\n"); dout.flush()
            if not eligible:
                continue
            try:
                qlog = bridge("questlog", bot).get("quests", [])
            except Exception as ex:
                print("bridge err", scrub(ex), flush=True)
                continue
            opts = build_options(dp, qlog)
            if len(opts) < 2:
                continue
            bot_last_dp[bot] = e["ms"]
            last_plain = time.time()
            runs = [None]
            plain_n += 1
            if len(opts) >= 3 and plain_n % PERSONA_EVERY == 0 and persona_points < PERSONA_POINTS_MAX:
                runs += PERSONAS
                persona_points += 1
            if DRY:  # record the candidate set the Jev call would have received
                with open(os.path.join(HERE, "dry_candidates.jsonl"), "a", encoding="utf-8") as fo:
                    fo.write(json.dumps({"dp_id": dp["id"], "kind": dp["kind"], "bot": bot, "lvl": dp["lvl"],
                                         "quest": dp["quest"], "reason": dp["reason"], "options": opts}) + "\n")
                print(f"dry dp{dp['id']} {dp['kind']} bot{bot} opts={list(opts)}", flush=True)
                continue
            wait = JEV_MIN_GAP - (time.time() - last_jev)
            if wait > 0:
                time.sleep(wait)
            last_jev = time.time()
            recs, tk = call_jev(dp, opts, runs)
            calls += 1
            tokens += tk
            r0 = recs[0]
            print(f"call {calls} dp{dp['id']} {dp['kind']} bot{bot} q={len(runs)} -> "
                  f"{[r.get('choice') for r in recs]} valid={all(r['valid'] for r in recs)} "
                  f"{r0['latency_s']}s tok={tk} cum={tokens}" + (f" ERR {r0['error']}" if 'error' in r0 else ""), flush=True)
            hard_errs = hard_errs + 1 if 'error' in r0 else 0
            if hard_errs >= 2:
                print("abort: 2 consecutive API errors", flush=True)
                return
    print(f"done: calls={calls} tokens={tokens} elapsed={time.time()-start:.0f}s", flush=True)


if __name__ == "__main__":
    main()
