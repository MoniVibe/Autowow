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
JEV_URL = "https://jevtypesafeai.com/api/v1/decide"

RUN_SECONDS = int(sys.argv[1]) if len(sys.argv) > 1 else 3300
MAX_CALLS, MAX_COST = 40, 2.0
JEV_MIN_GAP = 5.0          # hard rate limit between Jev calls
PLAIN_GAP = 75.0           # spread plain decisions across the run
BRIDGE_MIN_GAP = 1.1       # <= 1 bridge call/sec
STALL_N = 5
PERSONA_EVERY = 5          # every 5th plain decision with >=3 options gets persona calls
PERSONAS = ["cautious", "greedy", "reckless explorer"]
PERSONA_POINTS_MAX = 4     # 4 points x 3 personas = 12 calls
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


def call_jev(dp, opts, persona=None):
    state = {"bot_level": dp["lvl"], "zone": ZONE.get(dp["zone"], str(dp["zone"])),
             "trigger": dp["kind"], "trigger_quest": qfacts(dp["quest"]),
             "options": {k: {kk: vv for kk, vv in v.items() if kk != "desc"} for k, v in opts.items()}}
    if dp["kind"] == "stall":
        state["stall"] = {"reason": dp["reason"], "phase": dp["phase"], "repeats": dp["count"]}
    if dp["kind"] in ("deferred", "abandoned"):
        state["reason"] = dp["reason"]
    instr = ("Choose exactly one next action for a WoW WotLK leveling bot. Goal: efficient leveling "
             "(XP per minute, avoid stalls and wasted travel). SHADOW advisory only.")
    if persona:
        state["persona"] = persona
        instr = (f"Choose exactly one next action for a WoW WotLK leveling bot whose personality is "
                 f"'{persona}'. Play in character; the choice should reflect that personality.")
    req = {"model": "jev-latest", "state": state,
           "questions": {"decision": {"type": "choice", "instructions": instr,
                                      "criteria": {k: v["desc"] for k, v in opts.items()}}}}
    rec = {"t": time.time(), "dp_id": dp["id"], "persona": persona, "allow": list(opts), "request_state": state}
    t0 = time.time()
    try:
        r = urllib.request.Request(JEV_URL, data=json.dumps(req).encode(), method="POST",
                                   headers={"Authorization": f"Bearer {API_KEY}", "Content-Type": "application/json"})
        with urllib.request.urlopen(r, timeout=30) as resp:
            body = json.loads(resp.read().decode())
        rec["latency_s"] = round(time.time() - t0, 3)
        ans = body.get("answers", {}).get("decision", {})
        rec.update(model=body.get("model"), usage=body.get("usage"),
                   cost_usd=(body.get("usage") or {}).get("cost_usd"),
                   choice=ans.get("choice"), confidence=ans.get("confidence"),
                   probabilities=ans.get("probabilities"),
                   extra={k: v for k, v in ans.items() if k not in ("choice", "confidence", "probabilities", "type")})
        rec["valid"] = ans.get("type") == "choice" and ans.get("choice") in opts
    except urllib.error.HTTPError as e:
        rec.update(latency_s=round(time.time() - t0, 3), valid=False,
                   error=scrub(f"HTTP {e.code}: {e.read()[:300]!r}"))
    except Exception as e:
        rec.update(latency_s=round(time.time() - t0, 3), valid=False, error=scrub(repr(e))[:300])
    with open(os.path.join(HERE, "calls.jsonl"), "a", encoding="utf-8") as f:
        f.write(json.dumps(rec) + "\n")
    return rec


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
    calls, cost, plain_n, persona_points = 0, 0.0, 0, 0
    last_jev, last_plain = 0.0, 0.0
    streak = {}                       # bot -> (quest, reason, count, fired)
    bot_last_dp = defaultdict(lambda: -10**12)
    dp_seq = 0
    f = open(LEDGER, encoding="utf-8")
    f.seek(0, 2)
    buf = ""
    dout = open(os.path.join(HERE, "decisions.jsonl"), "a", encoding="utf-8")
    print(f"shadow harness up; run {RUN_SECONDS}s", flush=True)
    while time.time() - start < RUN_SECONDS and calls < MAX_CALLS and cost < MAX_COST:
        chunk = f.read()
        if not chunk:
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
                        and calls < MAX_CALLS and cost < MAX_COST)
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
            if len(opts) >= 3 and plain_n % PERSONA_EVERY == 0 and persona_points < PERSONA_POINTS_MAX \
                    and calls + 4 <= MAX_CALLS:
                runs += PERSONAS
                persona_points += 1
            if DRY:  # key unusable: record the candidate set the Jev call would have received
                with open(os.path.join(HERE, "dry_candidates.jsonl"), "a", encoding="utf-8") as fo:
                    fo.write(json.dumps({"dp_id": dp["id"], "kind": dp["kind"], "bot": bot, "lvl": dp["lvl"],
                                         "quest": dp["quest"], "reason": dp["reason"], "options": opts}) + "\n")
                print(f"dry dp{dp['id']} {dp['kind']} bot{bot} opts={list(opts)}", flush=True)
                continue
            for persona in runs:
                if calls >= MAX_CALLS or cost >= MAX_COST:
                    break
                wait = JEV_MIN_GAP - (time.time() - last_jev)
                if wait > 0:
                    time.sleep(wait)
                last_jev = time.time()
                rec = call_jev(dp, opts, persona)
                calls += 1
                cost += float(rec.get("cost_usd") or 0)
                print(f"call {calls} dp{dp['id']} {dp['kind']} bot{bot} persona={persona} -> "
                      f"{rec.get('choice')} valid={rec['valid']} {rec['latency_s']}s ${cost:.4f}"
                      + (f" ERR {rec['error']}" if 'error' in rec else ""), flush=True)
    print(f"done: calls={calls} cost=${cost:.4f} elapsed={time.time()-start:.0f}s", flush=True)


if __name__ == "__main__":
    main()
