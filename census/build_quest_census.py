#!/usr/bin/env python3
"""AutoWoW quest census generator (offline, no DB login).

Reads the pinned AzerothCore world-DB SQL dumps + post-base update files,
the server's extracted DBC files (AreaTable/QuestSort/Map/FactionTemplate)
and the extracted .map area grids, and emits:
  census/quest-catalog.json
  census/CENSUS_SUMMARY.md

Usage:  python build_quest_census.py            (paths default to AutoWoW layout)
        python build_quest_census.py --selftest (asserts known live-proven fixtures)
ASCII-only source. Read-only on every input.
"""
import json, os, re, struct, sys, collections

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
AC = os.path.join(ROOT, "azerothcore-wotlk")
BASE = os.path.join(AC, "data", "sql", "base", "db_world")
UPD = os.path.join(AC, "data", "sql", "updates", "db_world")
PENDING = os.path.join(AC, "data", "sql", "updates", "pending_db_world")
DBC = os.path.join(ROOT, "server", "data", "dbc")
MAPS = os.path.join(ROOT, "server", "data", "maps")
SCRIPTS = os.path.join(AC, "src", "server", "scripts")
OUT = os.path.dirname(os.path.abspath(__file__))

# ---------------------------------------------------------------- SQL dump parsing
TOK = re.compile(r"\s*(?:'((?:[^'\\]|\\.)*)'|(NULL)|([-+0-9.eE]+)|([(),;]))", re.S)
ESC = {"n": "\n", "r": "\r", "t": "\t", "0": "\0", "Z": "\x1a"}
UNESC = re.compile(r"\\(.)", re.S)
ROWCHECK = {}  # table -> (parsed rows, independent line count)


def _unescape(s):
    return UNESC.sub(lambda m: ESC.get(m.group(1), m.group(1)), s)


def _num(s):
    try:
        return int(s)
    except ValueError:
        return float(s)


def load(table, want=None):
    """Return list of dicts for `table` from base/db_world/<table>.sql.
    Every row must have exactly len(columns) values or we abort."""
    path = os.path.join(BASE, table + ".sql")
    text = open(path, encoding="utf-8", errors="replace").read()
    m = re.search(r"CREATE TABLE `%s` \((.*?)\n\)" % table, text, re.S)
    cols = re.findall(r"^\s+`(\w+)`", m.group(1), re.M)
    idx = [(c, cols.index(c)) for c in (want or cols)]
    rows = []
    for ins in re.finditer(r"INSERT INTO `%s`(?: \(([^)]*)\))? VALUES" % table, text):
        if ins.group(1):
            raise SystemExit("column-list INSERT not supported in %s" % table)
        pos, row, depth = ins.end(), None, 0
        while True:
            t = TOK.match(text, pos)
            if not t:
                raise SystemExit("parse error in %s at byte %d" % (table, pos))
            pos = t.end()
            s, null, num, p = t.groups()
            if p == "(":
                row = []
            elif p == ")":
                if len(row) != len(cols):
                    raise SystemExit("%s: row width %d != %d columns" % (table, len(row), len(cols)))
                rows.append({c: row[i] for c, i in idx})
                row = None
            elif p == ",":
                continue
            elif p == ";":
                break
            elif null:
                row.append(None)
            elif num is not None:
                row.append(_num(num))
            else:
                row.append(_unescape(s))
    # independent sanity: mysqldump writes one tuple per line starting with "("
    lines = sum(1 for ln in text.splitlines() if ln.startswith("("))
    ROWCHECK[table] = (len(rows), lines)
    if lines != len(rows):
        raise SystemExit("%s: parsed %d rows but %d tuple lines" % (table, len(rows), lines))
    return rows


def post_base_updates(qt, qta):
    """Apply simple numeric UPDATEs to quest_template(_addon) from update files
    newer than the last one recorded in base updates.sql. Anything else that
    touches quest tables is reported as a caveat, not applied."""
    applied, ignored = [], []
    base_upd = open(os.path.join(BASE, "updates.sql"), encoding="utf-8", errors="replace").read()
    last = max(re.findall(r"'(\d{4}_\d\d_\d\d_\d\d)\.sql'", base_upd))
    files = [os.path.join(UPD, f) for f in sorted(os.listdir(UPD)) if f.endswith(".sql") and f[:13] > last]
    if os.path.isdir(PENDING):
        files += [os.path.join(PENDING, f) for f in sorted(os.listdir(PENDING)) if f.endswith(".sql")]
    rel = re.compile(r"quest_template|queststarter|questender|disables|areatrigger_involvedrelation|game_event_\w*quest", re.I)
    upd = re.compile(r"UPDATE `(quest_template|quest_template_addon)` SET (.+?) WHERE \(?`ID`\s*(?:=\s*(\d+)|IN\s*\(([\d,\s]+)\))\)?\s*;", re.I)
    for f in files:
        for stmt in open(f, encoding="utf-8", errors="replace").read().split(";\n"):
            if not rel.search(stmt) or re.search(r"quest_template_locale|`command`", stmt):
                continue
            m = upd.search(stmt.strip() + ";")
            sets = re.findall(r"`(\w+)`\s*=\s*(-?\d+)", m.group(2)) if m else []
            if not m or not sets:
                ignored.append("%s: %s" % (os.path.basename(f), stmt.strip()[:120]))
                continue
            tbl = qt if m.group(1).lower() == "quest_template" else qta
            ids = [int(m.group(3))] if m.group(3) else [int(x) for x in m.group(4).split(",")]
            for i in ids:
                if i in tbl:
                    for c, v in sets:
                        tbl[i][c] = int(v)
            applied.append("%s: %s %s ids=%s" % (os.path.basename(f), m.group(1), sets, ids))
    return last, applied, ignored

# ---------------------------------------------------------------- DBC + map grids

def dbc(name):
    d = open(os.path.join(DBC, name + ".dbc"), "rb").read()
    magic, n, fields, size, ssize = struct.unpack_from("<4s4I", d)
    strs = d[20 + n * size:]
    recs = [struct.unpack_from("<%di" % fields, d, 20 + i * size) for i in range(n)]
    s = lambda off: strs[off:strs.index(b"\0", off)].decode("utf-8", "replace")
    return recs, s


AREA = {}   # id -> (map, parent, name)
recs, S = dbc("AreaTable")
for r in recs:
    AREA[r[0]] = (r[1], r[2], S(r[11]))
recs, S = dbc("QuestSort")
QSORT = {r[0]: S(r[1]) for r in recs}
recs, S = dbc("Map")
MAPTYPE = {r[0]: r[2] for r in recs}          # 0 world, 1 party, 2 raid, 3 bg, 4 arena
MAPNAME = {r[0]: S(r[5]) for r in recs}
recs, _ = dbc("FactionTemplate")
FTPL = {r[0]: (r[3], r[4], r[5]) for r in recs}  # factionGroup, friendGroup, enemyGroup


def zone_of_area(a):
    seen = 0
    while a in AREA and AREA[a][1] and seen < 8:
        a, seen = AREA[a][1], seen + 1
    return a


_GRID = {}
SZ = 533.3333333


def area_at(m, x, y):
    fx, fy = 32 - x / SZ, 32 - y / SZ
    key = (m, int(fx), int(fy))
    if key not in _GRID:
        p = os.path.join(MAPS, "%03d%02d%02d.map" % key)
        _GRID[key] = open(p, "rb").read() if os.path.exists(p) else None
    d = _GRID[key]
    if d is None:
        return 0
    off = struct.unpack_from("<11I", d)[3]
    _, fl, ga = struct.unpack_from("<IHH", d, off)
    if fl & 1:
        return ga
    return struct.unpack_from("<H", d, off + 8 + 2 * ((int(fx * 16) & 15) * 16 + (int(fy * 16) & 15)))[0]


CONT = {0: "Eastern Kingdoms", 1: "Kalimdor", 530: "Outland", 571: "Northrend", 609: "Ebon Hold (DK start)"}
BC_START = {3430: "Eastern Kingdoms", 3433: "Eastern Kingdoms", 3487: "Eastern Kingdoms", 4080: "Eastern Kingdoms",
            3524: "Kalimdor", 3525: "Kalimdor", 3557: "Kalimdor"}  # Eversong/Ghostlands/Silvermoon/QuelDanas; Azuremyst/Bloodmyst/Exodar


def continent(m, zone):
    if m == 530 and zone in BC_START:
        return BC_START[zone] + " (map 530)"
    if m in CONT:
        return CONT[m]
    t = MAPTYPE.get(m)
    return {1: "Instance (dungeon)", 2: "Instance (raid)", 3: "Battleground", 4: "Arena"}.get(t, "Unknown map %s" % m)

# ---------------------------------------------------------------- constants
ALLI, HORDE = 1 | 4 | 8 | 64 | 1024, 2 | 16 | 32 | 128 | 512
LIVE_PROVEN = {"TALK_ONLY", "KILL", "COLLECT_DROP", "GAMEOBJECT_USE", "GAMEOBJECT_COLLECT", "ITEM_USE_ON_TARGET"}
BOT_READY_BLOCKERS = {"GROUP_ELITE", "DUNGEON", "RAID", "VEHICLE_HEURISTIC", "PVP", "HOLIDAY", "EVENT_GATED",
                      "PROFESSION", "TIMED"}
DEPRECATED = re.compile(r"<\s*(UNUSED|NYI|TXT|TEST|PH)\s*>|\[(DEPRECATED|UNUSED|PH|NYI)\]|\bDEPRECATED\b|^\s*UNUSED\b|\bZZ?OLD\b|^\s*test\b|\btest quest\b|\(TEST\)|\bREUSE\b|<\s*nyi", re.I)
HOLIDAY_SORT = re.compile(r"Seasonal|Darkmoon|Lunar|Midsummer|Brewfest|Noblegarden|Pilgrim|Love|Dead|Children|Hallow|Winter|Harvest|Fireworks|Special", re.I)
CLASSES = re.compile(r"^(Warlock|Warrior|Shaman|Paladin|Mage|Rogue|Hunter|Priest|Druid|Death Knight)$", re.I)
PROFS = re.compile(r"Herbalism|Fishing|Blacksmithing|Alchemy|Leatherworking|Engineering|Tailoring|Cooking|First Aid|Inscription|Jewelcrafting|Enchanting|Mining|Skinning|Archaeology", re.I)
PVP_SORT = re.compile(r"Battleground|Alterac|Warsong|Arathi Basin|Eye of the Storm|Strand|Isle of Conquest|Wintergrasp", re.I)
CREDIT_NAME = re.compile(r"credit|bunny|trigger|invisible|\bdummy\b|\bKC\b|\(quest|quest\)|event|target|marker|\bspot\b|\bloc\b", re.I)
VEHICLE_TEXT = re.compile(r"\bvehicle\b|\bcatapult\b|\bdemolisher\b|\bsiege\b|\bcannon\b|\bturret\b|\bbombing\b|\bbomb run|\bgryphon\b.*\b(ride|fly)|\bdrake\b.*\b(ride|fly|mount)|\bmount up\b|\btake the reins\b|\bflying machine\b|\bshredder\b", re.I)


def main():
    selftest = "--selftest" in sys.argv
    qt = {r["ID"]: r for r in load("quest_template")}
    qta = {r["ID"]: r for r in load("quest_template_addon")}
    last_base_update, applied, ignored = post_base_updates(qt, qta)

    def multimap(rows, k, v):
        d = collections.defaultdict(list)
        for r in rows:
            d[r[k]].append(r[v])
        return d

    c_start = multimap(load("creature_queststarter"), "quest", "id")
    c_end = multimap(load("creature_questender"), "quest", "id")
    g_start = multimap(load("gameobject_queststarter"), "quest", "id")
    g_end = multimap(load("gameobject_questender"), "quest", "id")
    at_rel = multimap(load("areatrigger_involvedrelation"), "quest", "id")
    ev_seasonal = multimap(load("game_event_seasonal_questrelation"), "questId", "eventEntry")
    ev_cq = load("game_event_creature_quest")
    ev_gq = load("game_event_gameobject_quest")
    ev_start = collections.defaultdict(list)
    for r in ev_cq:
        ev_start[r["quest"]].append(("creature", r["id"], r["eventEntry"]))
    for r in ev_gq:
        ev_start[r["quest"]].append(("gameobject", r["id"], r["eventEntry"]))
    disabled = {r["entry"]: r["comment"] for r in load("disables") if r["sourceType"] == 1}

    ct = {r["entry"]: r for r in load("creature_template", ["entry", "name", "rank", "faction", "VehicleId", "flags_extra", "ScriptName", "npcflag"])}
    gt = {r["entry"]: r for r in load("gameobject_template", ["entry", "name", "type"])}
    items = load("item_template", ["entry", "name", "startquest"])
    item_start = collections.defaultdict(list)
    for r in items:
        if r["startquest"]:
            item_start[r["startquest"]].append(r["entry"])

    spawn_c = collections.defaultdict(list)
    for r in load("creature", ["id1", "map", "zoneId", "areaId", "position_x", "position_y"]):
        spawn_c[r["id1"]].append(r)
    spawn_g = collections.defaultdict(list)
    for r in load("gameobject", ["id", "map", "zoneId", "areaId", "position_x", "position_y"]):
        spawn_g[r["id"]].append(r)

    drop_items = {r["Item"] for r in load("creature_loot_template", ["Item"])}
    drop_items |= {r["ItemId"] for r in load("creature_questitem", ["ItemId"])}
    ref_items = {r["Item"] for r in load("reference_loot_template", ["Item"])}
    go_items = {r["Item"] for r in load("gameobject_loot_template", ["Item"])}
    go_items |= {r["ItemId"] for r in load("gameobject_questitem", ["ItemId"])}
    spellclick = {r["npc_entry"] for r in load("npc_spellclick_spells", ["npc_entry"])}
    # spellclick (vehicle boarding) gated on a quest being taken/complete -> that quest uses a vehicle
    click_quest = set()
    for r in load("conditions", ["SourceTypeOrReferenceId", "SourceGroup", "ConditionTypeOrReference", "ConditionValue1", "NegativeCondition"]):
        if r["SourceTypeOrReferenceId"] == 18 and r["ConditionTypeOrReference"] in (9, 28, 47) and not r["NegativeCondition"]:
            if ct.get(r["SourceGroup"], {}).get("VehicleId"):
                click_quest.add(r["ConditionValue1"])

    smart_escort, smart_offer, smart_event_credit = set(), collections.defaultdict(list), set()
    for r in load("smart_scripts", ["entryorguid", "source_type", "event_type", "event_param1", "action_type", "action_param1", "action_param4"]):
        a = r["action_type"]
        if a == 53 and r["action_param4"]:
            smart_escort.add(r["action_param4"])
        if a == 53 and r["event_type"] == 19 and r["event_param1"]:
            smart_escort.add(r["event_param1"])
        if a == 7 and r["action_param1"]:
            smart_offer[r["action_param1"]].append((r["source_type"], r["entryorguid"]))
        if a in (15, 26) and r["action_param1"]:
            smart_event_credit.add(r["action_param1"])

    # C++ escort scripts: quest constants in files that use EscortAI/FollowerAI (heuristic)
    cpp_escort = set()
    for dp, _, fs in os.walk(SCRIPTS):
        for f in fs:
            if f.endswith(".cpp"):
                src = open(os.path.join(dp, f), encoding="utf-8", errors="replace").read()
                if re.search(r"EscortAI|FollowerAI", src):
                    cpp_escort |= {int(x) for x in re.findall(r"\bQUEST_\w+\s*=\s*(\d+)", src)}

    chain_prev = collections.defaultdict(list)
    for q in qt.values():
        if q["RewardNextQuest"]:
            chain_prev[q["RewardNextQuest"]].append(q["ID"])

    zc = {}

    def spawn_zone(sp):
        key = (sp["map"], round(sp["position_x"]), round(sp["position_y"]))
        if key not in zc:
            a = sp["areaId"] or sp["zoneId"] or area_at(sp["map"], sp["position_x"], sp["position_y"])
            zc[key] = zone_of_area(a)
        return zc[key]

    def first_spawn(kind, entries):
        tab = spawn_c if kind == "creature" else spawn_g
        for e in entries:
            if tab.get(e):
                return e, tab[e][0]
        return (entries[0] if entries else 0), None

    def faction_ok(entry):
        """(alliance_ok, horde_ok) for a starter creature's faction template."""
        f = FTPL.get(ct.get(entry, {}).get("faction", 0))
        if not f:
            return True, True
        grp, _, enemy = f
        return not (enemy & 2) and not (grp & 4 and not grp & 2), not (enemy & 4) and not (grp & 2 and not grp & 4)

    catalog = []
    for qid in sorted(qt):
        q, a = qt[qid], qta.get(qid, {})
        title = q["LogTitle"] or ""
        flags, reasons, objectives = [], [], []
        sort = q["QuestSortID"]
        sortname = QSORT.get(-sort, "") if sort < 0 else ""
        spf = a.get("SpecialFlags", 0) or 0
        qflags = q["Flags"]

        # ---- starter / ender
        if c_start.get(qid):
            e, sp = first_spawn("creature", c_start[qid])
            starter = {"type": "creature", "entry": e, "spawned": sp is not None}
        elif g_start.get(qid):
            e, sp = first_spawn("gameobject", g_start[qid])
            starter = {"type": "gameobject", "entry": e, "spawned": sp is not None}
        elif ev_start.get(qid):
            k, e, ev = ev_start[qid][0]
            e, sp = first_spawn(k, [x[1] for x in ev_start[qid]])
            starter = {"type": k + "_event", "entry": e, "event": ev, "spawned": sp is not None}
        elif item_start.get(qid):
            starter, sp = {"type": "item", "entry": item_start[qid][0]}, None
        elif chain_prev.get(qid):
            starter, sp = {"type": "chain", "entry": chain_prev[qid][0]}, None
        elif smart_offer.get(qid):
            st, e = smart_offer[qid][0]
            starter, sp = {"type": "script", "entry": e}, None
        else:
            starter, sp = {"type": "none", "entry": 0}, None
        if c_end.get(qid):
            e, esp = first_spawn("creature", c_end[qid])
            ender = {"type": "creature", "entry": e, "spawned": esp is not None}
        elif g_end.get(qid):
            e, esp = first_spawn("gameobject", g_end[qid])
            ender = {"type": "gameobject", "entry": e, "spawned": esp is not None}
        else:
            ender, esp = {"type": "none", "entry": 0}, None

        # ---- zone / continent
        zone_src = "QuestSortID"
        if sort > 0:
            zone = sort
        elif sp is not None:
            zone, zone_src = spawn_zone(sp), "starter_spawn"
        elif esp is not None:
            zone, zone_src = spawn_zone(esp), "ender_spawn"
        else:
            zone, zone_src = 0, "none"
        where = sp or esp
        zmap = AREA.get(zone, (None,))[0]
        if zmap is None and where is not None:
            zmap = where["map"]
        cont = continent(zmap, zone) if zmap is not None else "Unknown"
        scont = continent(where["map"], spawn_zone(where)) if where is not None else "Unknown"

        # ---- faction
        races = q["AllowableRaces"]
        al, ho = (races == 0 or races & ALLI != 0), (races == 0 or races & HORDE != 0)
        fsrc = "AllowableRaces"
        if al and ho and starter["type"] in ("creature", "creature_event"):
            fa, fh = faction_ok(starter["entry"])
            if fa != fh:
                al, ho, fsrc = fa, fh, "starter_faction_template"
        faction = "Both" if al and ho else "Alliance" if al else "Horde" if ho else "None"

        # ---- objectives
        provided = {q["StartItem"]} | {q["ItemDrop%d" % i] for i in range(1, 5)}
        provided.discard(0)
        npcs = [q["RequiredNpcOrGo%d" % i] for i in range(1, 5) if q["RequiredNpcOrGo%d" % i]]
        creds = [n for n in npcs if n > 0]
        gos = [-n for n in npcs if n < 0]
        credit_bunny = [n for n in creds if CREDIT_NAME.search(ct.get(n, {}).get("name", "")) or (ct.get(n, {}).get("flags_extra", 0) & 0x80)]
        real_kills = [n for n in creds if n not in credit_bunny]
        req_items = [q["RequiredItemId%d" % i] for i in range(1, 7) if q["RequiredItemId%d" % i]]
        for it in req_items:
            if it in provided:
                objectives.append("DELIVER_PROVIDED_ITEM")
            elif it in go_items:
                objectives.append("GAMEOBJECT_COLLECT")
            elif it in drop_items or it in ref_items:
                objectives.append("COLLECT_DROP")
            else:
                objectives.append("COLLECT_OTHER")
        cast = bool(spf & 0x20)
        use_item = bool(creds or gos) and bool(provided)
        if use_item:
            objectives.append("ITEM_USE_ON_TARGET")
        if credit_bunny or cast:
            objectives.append("SPELL_CREDIT")
        if real_kills and not cast:
            objectives.append("KILL")
        if gos:
            objectives.append("GAMEOBJECT_USE")
        if at_rel.get(qid):
            objectives.append("EXPLORE")
        if q["RequiredPlayerKills"]:
            objectives.append("PVP_KILL")
        escort = q["QuestInfoID"] == 84 or qid in smart_escort or (qid in cpp_escort and spf & 2 and not npcs and not req_items)
        if escort:
            objectives.append("ESCORT")
        if spf & 2 and not at_rel.get(qid) and not escort:
            objectives.append("EVENT_CREDIT")
        order = ["ESCORT", "EVENT_CREDIT", "PVP_KILL", "ITEM_USE_ON_TARGET", "SPELL_CREDIT", "EXPLORE", "GAMEOBJECT_USE",
                 "GAMEOBJECT_COLLECT", "COLLECT_OTHER", "COLLECT_DROP", "KILL"]
        family = next((f for f in order if f in objectives), "TALK_ONLY")
        # SPELL_CREDIT with a provided item is item-use (e.g. q5441): keep ITEM_USE_ON_TARGET ahead of it (order above)

        # ---- flags
        if q["TimeAllowed"]:
            flags.append("TIMED")
        targets = [ct.get(n, {}) for n in creds]
        elite = any(t.get("rank", 0) in (1, 2, 3) for t in targets)
        if q["SuggestedGroupNum"] > 1 or q["QuestInfoID"] == 1 or elite:
            flags.append("GROUP_ELITE")
        tmaps = [s["map"] for n in creds for s in spawn_c.get(n, [])[:3]] + [s["map"] for n in gos for s in spawn_g.get(n, [])[:3]]
        inst_types = {MAPTYPE.get(m) for m in tmaps} | ({MAPTYPE.get(zmap)} if zmap is not None else set())
        if q["QuestInfoID"] in (81, 85) or 1 in inst_types:
            flags.append("DUNGEON")
        if q["QuestInfoID"] in (62, 88, 89) or 2 in inst_types:
            flags.append("RAID")
        if (a.get("AllowableClasses", 0) or 0) or CLASSES.match(sortname):
            flags.append("CLASS")
        if (a.get("RequiredSkillID", 0) or 0) or PROFS.search(sortname):
            flags.append("PROFESSION")
        if qflags & 0x1000:
            flags.append("DAILY")
        if qflags & 0x8000:
            flags.append("WEEKLY")
        if spf & 0x10:
            flags.append("MONTHLY")
        if spf & 1:
            flags.append("REPEATABLE")
        if ev_seasonal.get(qid) or (sort < 0 and HOLIDAY_SORT.search(sortname)):
            flags.append("HOLIDAY")
        elif ev_start.get(qid) and starter["type"].endswith("_event"):
            flags.append("EVENT_GATED")
        if q["QuestInfoID"] == 41 or qflags & 0x2000 or q["RequiredPlayerKills"] or (sort < 0 and PVP_SORT.search(sortname)) or 3 in inst_types:
            flags.append("PVP")
        text = " ".join(str(q.get(k) or "") for k in ("LogTitle", "LogDescription", "ObjectiveText1", "ObjectiveText2", "ObjectiveText3", "ObjectiveText4"))
        if qid in click_quest or any(t.get("VehicleId") for t in targets) or any(n in spellclick for n in creds) or VEHICLE_TEXT.search(text):
            flags.append("VEHICLE_HEURISTIC")
        if qflags & 0x10000 or q["QuestType"] == 0:
            flags.append("AUTOCOMPLETE")
        if qflags & 0x400:
            flags.append("TRACKING_HIDDEN")

        # ---- availability
        if qid in disabled:
            reasons.append("DISABLES_TABLE")
        if DEPRECATED.search(title):
            reasons.append("DEPRECATED_TITLE")
        if q["QuestType"] == 1:
            reasons.append("QUESTTYPE_1_DISABLED")
        if qflags & 0x4000:
            reasons.append("FLAG_UNAVAILABLE")
        if faction == "None":
            reasons.append("NO_PLAYABLE_RACE")
        if starter["type"] == "none":
            reasons.append("NO_STARTER")
        elif starter.get("spawned") is False and starter["type"] in ("creature", "gameobject"):
            reasons.append("STARTER_NOT_SPAWNED")
        if qflags & 0x400 and "NO_STARTER" in reasons:
            pass
        available = not reasons
        catalog.append({
            "id": qid, "title": title, "family": family, "objectives": sorted(set(objectives)), "flags": flags,
            "faction": faction, "factionSource": fsrc, "raceMask": races,
            "minLevel": q["MinLevel"], "questLevel": q["QuestLevel"],
            "questSortId": sort, "questSortName": sortname,
            "zoneId": zone, "zoneName": AREA.get(zone, (0, 0, ""))[2] or (sortname if sort < 0 else ""), "zoneSource": zone_src,
            "continent": cont, "starterContinent": scont,
            "starter": starter, "ender": ender,
            "available": available, "unavailableReason": "+".join(reasons) or None,
        })

    by_id = {r["id"]: r for r in catalog}
    for r in catalog:
        r["botReadyPrediction"] = (r["available"] and r["family"] in LIVE_PROVEN and not (set(r["flags"]) & BOT_READY_BLOCKERS)
                                   and r["continent"] in ("Eastern Kingdoms", "Kalimdor", "Outland", "Northrend", "Ebon Hold (DK start)",
                                                          "Eastern Kingdoms (map 530)", "Kalimdor (map 530)"))

    # ---- self-check against live-proven fixtures (QUEST_TYPE_COVERAGE_REPORT.md)
    expect = {783: "TALK_ONLY", 792: "KILL", 789: "COLLECT_DROP", 459: "COLLECT_DROP", 916: "COLLECT_DROP",
              917: "GAMEOBJECT_COLLECT", 786: "GAMEOBJECT_USE", 5441: "ITEM_USE_ON_TARGET", 435: "ESCORT"}
    got = {k: by_id[k]["family"] for k in expect}
    bad = {k: (expect[k], got[k]) for k in expect if got[k] != expect[k]}
    assert len(catalog) == len(qt) == ROWCHECK["quest_template"][0]
    if selftest:
        assert not bad, bad
        print("selftest OK", got)
        return

    json.dump({"generator": "census/build_quest_census.py", "source": "azerothcore-wotlk " + os.popen('git -C "%s" rev-parse --short=8 HEAD' % AC).read().strip(),
               "count": len(catalog), "quests": catalog},
              open(os.path.join(OUT, "quest-catalog.json"), "w", encoding="utf-8"), indent=1, ensure_ascii=True)
    write_summary(catalog, bad, last_base_update, applied, ignored, len(disabled))


def write_summary(cat, bad, last, applied, ignored, ndis):
    N = len(cat)
    L = []
    w = L.append
    C = collections.Counter
    avail = [r for r in cat if r["available"]]
    w("# AutoWoW Quest Census (static, offline)\n")
    w("Generated by `census/build_quest_census.py` from azerothcore-wotlk SQL base dumps (+ post-base updates newer than `%s`), "
      "server DBCs (AreaTable, QuestSort, Map, FactionTemplate) and extracted `.map` area grids. No database login. "
      "Everything here is a **static prediction from DB content**, not live proof.\n" % last)
    w("## 1. Totals\n")
    w("- quest_template rows: **%d** (= catalog size)" % N)
    w("  - available: **%d**" % len(avail))
    w("  - unavailable: **%d** (reason breakdown below)\n" % (N - len(avail)))
    w("## 2. Unavailable by primary reason (sums to %d)\n" % (N - len(avail)))
    w("Primary reason = first in precedence DISABLES_TABLE > DEPRECATED_TITLE > QUESTTYPE_1_DISABLED > FLAG_UNAVAILABLE > NO_PLAYABLE_RACE > NO_STARTER > STARTER_NOT_SPAWNED. Full reason chain is in the JSON.\n")
    w("| primary reason | quests |\n|---|---:|")
    for k, v in C(r["unavailableReason"].split("+")[0] for r in cat if not r["available"]).most_common():
        w("| %s | %d |" % (k, v))
    w("")
    w("## 3. Primary family (all quests, sums to %d; of which available)\n" % N)
    w("| family | all | of which available | of which bot-ready prediction |\n|---|---:|---:|---:|")
    fa, fv, fb = C(r["family"] for r in cat), C(r["family"] for r in avail), C(r["family"] for r in cat if r["botReadyPrediction"])
    for k, v in fa.most_common():
        w("| %s | %d | %d | %d |" % (k, v, fv[k], fb[k]))
    w("| **total** | **%d** | **%d** | **%d** |\n" % (N, len(avail), sum(fb.values())))
    w("Primary family = hardest objective present, precedence ESCORT > EVENT_CREDIT > PVP_KILL > ITEM_USE_ON_TARGET > SPELL_CREDIT > EXPLORE > GAMEOBJECT_USE > GAMEOBJECT_COLLECT > COLLECT_OTHER > COLLECT_DROP > KILL > TALK_ONLY. `objectives[]` in the JSON lists every family present.\n")
    w("## 4. Flags on available quests (non-exclusive, do not sum)\n")
    w("| flag | available quests |\n|---|---:|")
    for k, v in C(f for r in avail for f in r["flags"]).most_common():
        w("| %s | %d |" % (k, v))
    w("")
    w("## 5. Continent x faction (available quests; sums to %d)\n" % len(avail))
    conts = sorted({r["continent"] for r in avail})
    w("| continent | Alliance-only | Horde-only | Both | total |\n|---|---:|---:|---:|---:|")
    tot = C()
    for c in conts:
        cc = C(r["faction"] for r in avail if r["continent"] == c)
        tot.update(cc)
        w("| %s | %d | %d | %d | %d |" % (c, cc["Alliance"], cc["Horde"], cc["Both"], sum(cc.values())))
    w("| **total** | **%d** | **%d** | **%d** | **%d** |\n" % (tot["Alliance"], tot["Horde"], tot["Both"], sum(tot.values())))
    br = [r for r in cat if r["botReadyPrediction"]]
    w("## 6. Bot-ready-now (STATIC PREDICTION, not proof)\n")
    w("Predicate: available AND primary family in the live-proven set {%s} (QUEST_TYPE_COVERAGE_REPORT.md 2026-07-15) AND none of flags {%s} AND on an open-world continent map. "
      "Escort is excluded (only one conventional escort proven); GAMEOBJECT_COLLECT is included (q917 Webwood Egg is a GO-loot collect); COLLECT_OTHER is excluded (vendor/craft sourcing not proven). Cross-map travel is NOT excluded although it is only PARTIAL.\n"
      % (", ".join(sorted(LIVE_PROVEN)), ", ".join(sorted(BOT_READY_BLOCKERS))))
    bf = C(r["faction"] for r in br)
    w("- bot-ready prediction: **%d** of %d available (%.1f%%); of which Alliance-only %d, Horde-only %d, Both %d\n"
      % (len(br), len(avail), 100.0 * len(br) / max(1, len(avail)), bf["Alliance"], bf["Horde"], bf["Both"]))
    # per-zone
    OPEN = {"DUNGEON", "RAID", "HOLIDAY", "EVENT_GATED", "CLASS", "PROFESSION", "PVP"}
    w("## 7. Questing zones per faction\n")
    w("A **questing zone** for a faction = zone with >= 5 available open-world quests (none of flags %s) accessible to that faction (Both counts for each side).\n" % ", ".join(sorted(OPEN)))
    zone = collections.defaultdict(list)
    for r in avail:
        zone[(r["zoneId"], r["zoneName"])].append(r)
    rows = []
    for (zid, zn), rs in zone.items():
        c = rs[0]["continent"]
        ow = [r for r in rs if not set(r["flags"]) & OPEN]
        a = [r for r in ow if r["faction"] in ("Alliance", "Both")]
        h = [r for r in ow if r["faction"] in ("Horde", "Both")]
        lv = sorted(r["questLevel"] if r["questLevel"] > 0 else r["minLevel"] for r in (ow or rs))
        band = "%d-%d" % (lv[len(lv) // 10], lv[(len(lv) * 9) // 10]) if lv else "-"
        mix = ", ".join("%s %d" % (k, v) for k, v in C(r["family"] for r in rs).most_common(4))
        rows.append((c, zn or "(zone %d)" % zid, zid, len(rs), len(a), len(h), sum(r["botReadyPrediction"] for r in rs), band, mix))
    qa = sum(1 for r in rows if r[4] >= 5)
    qh = sum(1 for r in rows if r[5] >= 5)
    w("- Alliance questing zones: **%d**; Horde questing zones: **%d** (of %d zone buckets holding any available quest)\n" % (qa, qh, len(rows)))
    w("## 8. Per-zone table (available quests)\n")
    w("`all` = every available quest attributed to the zone (sums to %d across rows). `A open`/`H open` = open-world quests accessible to that faction (Both counted on each side, so they do not sum). "
      "Level band = P10-P90 of QuestLevel (MinLevel if QuestLevel<=0) over open-world quests.\n" % len(avail))
    w("| continent | zone | zoneId | all | A open | H open | bot-ready | level band | family mix (top 4) |\n|---|---|---:|---:|---:|---:|---:|---|---|")
    for r in sorted(rows, key=lambda x: (x[0], -x[3])):
        w("| %s | %s | %d | %d | %d | %d | %d | %s | %s |" % r)
    w("| **total** | | | **%d** | | | **%d** | | |\n" % (sum(r[3] for r in rows), sum(r[6] for r in rows)))
    zs = C(r["zoneSource"] for r in cat)
    w("## 9. Method, caveats and assumptions\n")
    w("- **Parsing.** Custom tokenizer over mysqldump multi-row INSERTs (quoted strings with backslash escapes, NULL, numbers). Every row is checked to have exactly the CREATE TABLE column count, and every table's parsed row count is checked against an independent count of `(`-prefixed tuple lines. All %d tables passed (quest_template %d rows, quest_template_addon %d rows)."
      % (len(ROWCHECK), ROWCHECK["quest_template"][0], ROWCHECK["quest_template_addon"][0]))
    w("- **Post-base updates.** Base dumps include updates up to `%s`. Newer update files were scanned; %d numeric quest_template/_addon UPDATEs applied (%s). Statements touching quest tables but not applied: %d%s. INSERT/DELETE in other tables (spawns, loot, scripts) after the base are NOT applied."
      % (last, len(applied), "; ".join(applied) or "none", len(ignored), (" (" + "; ".join(ignored) + ")") if ignored else ""))
    w("- **Zones.** Zone = QuestSortID when > 0 (DB-authoritative). Otherwise zone of the starter's first spawn, then ender's, computed from the extracted server `.map` area grid (same formula as GridTerrainData::getArea) and rolled up to the top-level AreaTable parent. zoneSource counts: %s. Names from server `dbc/AreaTable.dbc` and `QuestSort.dbc` (the SQL *_dbc tables are empty)." % dict(zs))
    w("- **Continent** = continent of the zone's AreaTable map (so a quest sorted to an instance zone reports `Instance (...)` even when its giver stands outside); `starterContinent` in the JSON holds the giver's (else ender's) first-spawn continent. Map 530 is split: Eversong/Ghostlands/Silvermoon/Quel'Danas -> `Eastern Kingdoms (map 530)`, Azuremyst/Bloodmyst/Exodar -> `Kalimdor (map 530)`. Only the first spawn of multi-spawn starters is used.")
    w("- **Faction** from AllowableRaces (0 = all). When that says Both and the starter creature's FactionTemplate is hostile to / grouped only with one side, faction is narrowed (factionSource=starter_faction_template). Neutral starters that are actually faction-gated by reputation or phasing are not detected.")
    w("- **Availability** is conservative-static: disables table (%d quest entries), deprecated-title regex, QuestType=1 (TrinityCore/AC 'disabled' method), Flags&UNAVAILABLE, no playable race, no starter (none of creature/gameobject/event/item startquest/RewardNextQuest chain/SmartAI OFFER_QUEST), or starter entry with zero spawns (may be script-summoned: false negatives possible). Quests started by spells, C++ scripts or phasing without any of these links count as NO_STARTER." % ndis)
    w("- **Family heuristics.** Creature objectives whose name matches credit/bunny/trigger/etc. or flags_extra TRIGGER count as SPELL_CREDIT; any provided item (StartItem/ItemDrop) plus a creature/GO objective = ITEM_USE_ON_TARGET (may over-count quests where the provided item is incidental). Required items are sourced by creature loot/questitem/reference loot (COLLECT_DROP), gameobject loot/questitem (GAMEOBJECT_COLLECT), else COLLECT_OTHER (vendor/craft/container/pickpocket...). ESCORT = QuestInfoID 84, SmartAI ESCORT_START tied to the quest, or a QUEST_ constant in a C++ file using EscortAI/FollowerAI when the quest has EXPLORATION_OR_EVENT and no objectives. EVENT_CREDIT = SpecialFlags EXPLORATION_OR_EVENT without an areatrigger (scripted credit).")
    w("- **VEHICLE_HEURISTIC** (a vehicle creature's spellclick is condition-gated on this quest being taken/complete, or a target creature has VehicleId or spellclick, or quest-text keywords; vehicles summoned by item/gossip spells are missed) is a heuristic with both false positives and negatives. GROUP_ELITE = SuggestedGroupNum>1, QuestInfoID Group, or an elite/boss objective creature. DUNGEON/RAID = QuestInfoID or objective spawns / zone on party/raid instance maps.")
    w("- **Self-check** vs live-proven fixtures (783 talk, 792 kill, 789/459/916 collect, 917 GO-collect, 786 GO-use, 5441 item-use, 435 escort): %s." % ("all match" if not bad else "MISMATCHES %s" % bad))
    open(os.path.join(OUT, "CENSUS_SUMMARY.md"), "w", encoding="utf-8").write("\n".join(L) + "\n")


if __name__ == "__main__":
    main()
