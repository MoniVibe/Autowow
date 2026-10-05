#!/usr/bin/env python3
"""Weapon census (lane AY, AutoWow.Gear.NoWhite): read-only DB query -> per-group table of weapon quality and
item level against the NoWhite green curve.

Run inside WSL (needs the mysql client):
    python3 /mnt/d/Games/wowstuff/AutoWoW/scripts/weapon-census.py [--conf /root/p1runtime/worldserver.conf]
        [--group cohort=62955-63004] [--group squad=70573-70582,70763,70764] [--worst 10]

Credentials come from the worldserver.conf CharacterDatabaseInfo / WorldDatabaseInfo and go to mysql through a
0600 temp defaults file (never argv, never printed). Only SELECTs run.

Per group: bots, avg level; main hand counts empty / grey+white / green / blue / epic+; avg main-hand ilvl; avg ratio
to the green curve; bots under the floor (quality <= 1 or ilvl < curve), > 25% and > 50% behind; off-hand weapon and
hunter ranged white/grey counts; then the worst bots by ratio.
"""
import argparse
import os
import re
import subprocess
import sys
import tempfile

DEFAULT_GROUPS = ["cohort=62955-63004", "squad=70573-70582,70763,70764"]


def green_ilvl(level):
    """NoWhitePolicy.h GreenIlvl (integer, truncated)."""
    if level <= 57:
        return level + 5
    if level <= 70:
        return 62 + 9 * (level - 58) // 2
    return 130 + 63 * (level - 71) // 10


def parse_guids(spec):
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            out.extend(range(int(lo), int(hi) + 1))
        else:
            out.append(int(part))
    return out


def db_info(conf, key):
    with open(conf, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = re.match(r'\s*%s\s*=\s*"([^"]*)"' % key, line)
            if m:
                host, port, user, password, db = m.group(1).split(";")
                return host, port, user, password, db
    sys.exit("missing %s in %s" % (key, conf))


def query(conf, sql):
    host, port, user, password, chardb = db_info(conf, "CharacterDatabaseInfo")
    fd, path = tempfile.mkstemp(prefix="wcensus-", suffix=".cnf")
    try:
        os.chmod(path, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write("[client]\nhost=%s\nport=%s\nuser=%s\npassword=%s\n" % (host, port, user, password))
        r = subprocess.run(["mysql", "--defaults-extra-file=" + path, "-B", "-N", chardb, "-e", sql],
                           capture_output=True, text=True)
    finally:
        os.unlink(path)
    if r.returncode:
        sys.exit("mysql failed: " + r.stderr.strip())
    return [line.split("\t") for line in r.stdout.splitlines() if line]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--conf", default="/root/p1runtime/worldserver.conf")
    ap.add_argument("--group", action="append", help="name=guid ranges, e.g. cohort=62955-63004 (repeatable)")
    ap.add_argument("--worst", type=int, default=10)
    args = ap.parse_args()
    groups = [(g.split("=", 1)[0], parse_guids(g.split("=", 1)[1])) for g in (args.group or DEFAULT_GROUPS)]
    worlddb = db_info(args.conf, "WorldDatabaseInfo")[4]
    all_guids = sorted({g for _, gs in groups for g in gs})
    if not all_guids:
        sys.exit("no guids")
    rows = query(args.conf, (
        "SELECT c.guid, c.name, c.level, c.class, ci.slot, it.Quality, it.ItemLevel, it.class "
        "FROM characters c LEFT JOIN character_inventory ci ON ci.guid = c.guid AND ci.bag = 0 AND ci.slot IN (15,16,17) "
        "LEFT JOIN item_instance ii ON ii.guid = ci.item "
        "LEFT JOIN `%s`.item_template it ON it.entry = ii.itemEntry "
        "WHERE c.guid IN (%s)") % (worlddb, ",".join(map(str, all_guids))))
    bots = {}
    for guid, name, level, cls, slot, quality, ilvl, iclass in rows:
        b = bots.setdefault(int(guid), {"name": name, "level": int(level), "cls": int(cls), "slots": {}})
        if slot not in (None, "NULL") and quality not in (None, "NULL"):
            b["slots"][int(slot)] = (int(quality), int(ilvl), int(iclass))
    print("group      bots avgL  mh:empty grey/white green blue epic+  mh_ilvl curve_ratio  under_floor >25% >50%  "
          "oh_white rng_white")
    worst = []
    for gname, gs in groups:
        members = [bots[g] | {"guid": g} for g in gs if g in bots]
        if not members:
            print("%-10s    0" % gname)
            continue
        empty = white = green = blue = epic = under = b25 = b50 = ohw = rngw = 0
        ilvls, ratios = [], []
        for b in members:
            curve = green_ilvl(b["level"])
            mh = b["slots"].get(15)
            if mh is None:
                empty += 1
                q, il = -1, 0
            else:
                q, il = mh[0], mh[1]
                white += q <= 1
                green += q == 2
                blue += q == 3
                epic += q >= 4
                ilvls.append(il)
            ratio = il / curve if curve else 0
            ratios.append(ratio)
            under += q <= 1 or il < curve
            b25 += ratio < 0.75
            b50 += ratio < 0.50
            oh = b["slots"].get(16)
            ohw += oh is not None and oh[2] == 2 and oh[0] <= 1  # ITEM_CLASS_WEAPON = 2
            rng = b["slots"].get(17)
            rngw += b["cls"] == 3 and (rng is None or rng[0] <= 1)  # CLASS_HUNTER = 3
            worst.append((ratio, gname, b["guid"], b["name"], b["level"], q, il, curve))
        n = len(members)
        print("%-10s %4d %4.1f  %8d %10d %5d %4d %5d  %7.1f %11.2f  %11d %4d %4d  %8d %9d" % (
            gname, n, sum(b["level"] for b in members) / n, empty, white, green, blue, epic,
            sum(ilvls) / len(ilvls) if ilvls else 0, sum(ratios) / n, under, b25, b50, ohw, rngw))
    worst.sort(key=lambda w: (w[0], w[2]))
    print("\nworst %d by main-hand ratio (group guid name L quality ilvl curve ratio)" % args.worst)
    for ratio, gname, guid, name, level, q, il, curve in worst[:args.worst]:
        print("  %-8s %6d %-14s L%-2d q=%-2d ilvl=%-4d curve=%-4d %.2f" % (gname, guid, name, level, q, il, curve, ratio))


if __name__ == "__main__":
    main()
