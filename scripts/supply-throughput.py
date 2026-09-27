#!/usr/bin/env python3
"""Supply-chain throughput scorecard for an AutoWoW ledger (logger `autowow.ledger`, schema v1).

Per product line (bags, potions, raw) and team (A = alliance, H = horde), over the ledger's time span:
  don/h    material units donated to a house (`supply` donate)
  buy/h    material units bought for a house (`supply` buy = rep AH buyouts, `trade` cod_buy = mail orders)
  fed/h    material units mailed rep -> artisan (`supply` feed with an item)
  craft/h  units crafted (`supply` craft; bolts count, they are casts too)
  deliv/h  product units that reached a member (`supply` deliver whose `to` is neither a rep nor an artisan)
  d>c min  median minutes donation -> craft: donated / bought material units queue FIFO per (team, item) and a
           craft consumes them at its recipe (RECIPES); only donated units are timed
  c>d min  median minutes craft -> delivery: crafted product units queue FIFO per (team, item), member
           deliveries pop them
Reps and artisans are read from the ledger: artisans = `to` of order / pay rows, reps = `from` of feed rows with
an item plus every other deliverer. Units from stock older than the ledger never enter a queue (not timed).

Usage: python supply-throughput.py LEDGER [LEDGER ...]
       python supply-throughput.py --selftest
"""
import json
import sys
from collections import defaultdict, deque

# Material units one crafted unit consumes (3.3.5; SupplyPolicy.h kTiers / kCatalog). Crafted reagents (bolts
# into bags, Minor Healing into Lesser) are not donations and are left out.
RECIPES = {
    2996: {2589: 2}, 2997: {2592: 3}, 4305: {4306: 4},  # bolts of linen / wool / silk
    4245: {4234: 2},                                    # Small Silk Pack: 2 Heavy Leather
    118: {2447: 1, 765: 1}, 858: {2450: 1}, 929: {2453: 1, 2450: 1},  # healing potions
}
# Item -> line for `trade` rows (they carry no line field).
ITEM_LINE = {2589: "bags", 2592: "bags", 4306: "bags", 4234: "bags",
             2447: "potions", 765: "potions", 2450: "potions", 2453: "potions"}
TEAMS = {0: "A", 1: "H"}


def rows(paths):
    for path in paths:
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                i = line.find('{"v":')
                if i < 0:
                    continue
                try:
                    r = json.loads(line[i:])
                except ValueError:
                    continue
                if r.get("ev") in ("supply", "trade"):
                    yield r


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    if not n:
        return None
    return xs[n // 2] if n % 2 else (xs[n // 2 - 1] + xs[n // 2]) / 2


def reduce(rs):
    rs = sorted(rs, key=lambda r: (r.get("ms", 0), r.get("bot", 0)))
    if not rs:
        return [], 0.0
    hours = max(1e-9, (rs[-1]["ms"] - rs[0]["ms"]) / 3.6e6)
    sup = [r for r in rs if r["ev"] == "supply"]
    artisans = {r["to"] for r in sup if r["reason"] in ("order", "pay") and r.get("to")}
    reps = {r["from"] for r in sup if r["reason"] == "feed" and r.get("item") and r.get("from")}
    reps |= {r["from"] for r in sup if r["reason"] == "deliver" and r.get("from") not in artisans}
    bot_line = {r["bot"]: r["line"] for r in sup if r.get("line") in ("bags", "potions")}

    tot = defaultdict(lambda: defaultdict(int))
    lat_dc, lat_cd = defaultdict(list), defaultdict(list)
    mats = defaultdict(deque)   # (team, item) -> [ms or None (bought), ...] per unit
    goods = defaultdict(deque)  # (team, item) -> [craft ms, ...] per unit
    for r in rs:
        team = r.get("team", 0)
        if r["ev"] == "trade":
            if r.get("action") != "cod_buy":
                continue
            line = ITEM_LINE.get(r.get("item"), bot_line.get(r["bot"], "other"))
            tot[(line, team)]["buy"] += r.get("count", 0)
            mats[(team, r.get("item"))].extend([None] * r.get("count", 0))
            continue
        line, reason, item, n = r.get("line", "other"), r["reason"], r.get("item", 0), r.get("count", 0)
        key = (line, team)
        if reason == "donate":
            tot[key]["don"] += n
            mats[(team, item)].extend([r["ms"]] * n)
        elif reason == "buy":
            tot[key]["buy"] += n
            mats[(team, item)].extend([None] * n)
        elif reason == "feed" and item:
            tot[key]["fed"] += n
        elif reason == "craft":
            tot[key]["craft"] += n
            for mat, per in RECIPES.get(item, {}).items():
                q = mats[(team, mat)]
                for _ in range(min(len(q), per * n)):
                    ms = q.popleft()
                    if ms is not None:
                        lat_dc[key].append(r["ms"] - ms)
            goods[(team, item)].extend([r["ms"]] * n)
        elif reason == "deliver" and r.get("to") not in reps and r.get("to") not in artisans:
            tot[key]["deliv"] += n
            q = goods[(team, item)]
            for _ in range(min(len(q), n)):
                lat_cd[key].append(r["ms"] - q.popleft())
    out = []
    for key in sorted(tot, key=lambda k: (k[0], k[1])):
        t = tot[key]
        dc, cd = median(lat_dc[key]), median(lat_cd[key])
        out.append((key[0], TEAMS.get(key[1], str(key[1])), t["don"] / hours, t["buy"] / hours, t["fed"] / hours,
                    t["craft"] / hours, t["deliv"] / hours, None if dc is None else dc / 60000,
                    None if cd is None else cd / 60000, len(lat_dc[key]), len(lat_cd[key])))
    return out, hours


def fmt(v):
    return "-" if v is None else f"{v:.1f}"


def main(paths):
    table, hours = reduce(rows(paths))
    print(f"span {hours:.2f} h; units per hour; latency medians in minutes (n = timed units)")
    print(f"{'line':8} {'team':4} {'don/h':>7} {'buy/h':>7} {'fed/h':>7} {'craft/h':>8} {'deliv/h':>8} "
          f"{'d>c min':>8} {'c>d min':>8} {'n_dc':>5} {'n_cd':>5}")
    for line, team, don, buy, fed, craft, deliv, dc, cd, ndc, ncd in table:
        print(f"{line:8} {team:4} {don:7.1f} {buy:7.1f} {fed:7.1f} {craft:8.1f} {deliv:8.1f} {fmt(dc):>8} "
              f"{fmt(cd):>8} {ndc:5} {ncd:5}")


def selftest():
    def s(ms, reason, frm, to, item, count, team=1, line="bags"):
        return {"ev": "supply", "ms": ms, "bot": frm, "team": team, "reason": reason, "from": frm, "to": to,
                "item": item, "count": count, "line": line}
    rs = [
        s(0, "donate", 10, 90, 2589, 4),              # 4 linen at t=0
        s(0, "order", 80, 0, 4238, 1),                # artisan 80 (order to=80 below)
        s(60000, "feed", 90, 80, 2589, 4),
        s(120000, "craft", 80, 80, 2996, 2),          # 2 bolts eat the 4 linen: 2 min each
        s(180000, "craft", 80, 80, 4238, 1),
        s(240000, "deliver", 80, 90, 4238, 1),        # artisan -> rep: not a member delivery
        s(360000, "deliver", 90, 11, 4238, 1),        # rep -> member 11: 3 min after the craft
        {"ev": "trade", "ms": 3600000, "bot": 90, "team": 1, "action": "cod_buy", "item": 2592, "count": 5},
    ]
    rs[1]["to"] = 80
    table, hours = reduce(rs)
    assert abs(hours - 1.0) < 1e-9, hours
    (line, team, don, buy, fed, craft, deliv, dc, cd, ndc, ncd), = table
    assert (line, team) == ("bags", "H")
    assert (don, buy, fed, craft, deliv) == (4, 5, 4, 3, 1), (don, buy, fed, craft, deliv)
    assert (dc, cd, ndc, ncd) == (2.0, 3.0, 4, 1), (dc, cd, ndc, ncd)
    print("selftest ok")


if __name__ == "__main__":
    if sys.argv[1:] == ["--selftest"]:
        selftest()
    elif len(sys.argv) > 1:
        main(sys.argv[1:])
    else:
        print(__doc__)
        sys.exit(2)
