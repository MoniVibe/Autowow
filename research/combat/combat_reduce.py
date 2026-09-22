#!/usr/bin/env python3
"""Combat-efficiency reducer for AutoWoW soaks (read-only; stdlib only).

Usage:
  python combat_reduce.py --roster roster.csv [--snap snap.jsonl ...] [--combatlog combatlog.jsonl ...]
  python combat_reduce.py --selftest

Inputs
  snap      : bridge `list` snapshots, one JSON line {utc, bots:[{guid,level,xp,alive,combat,state,...}]}
  combatlog : lines {utc, guid, resp:<bridge combatlog JSON>} (autowow.combat-telemetry.v0 + counters v1)
  roster    : guid,name,class,race,role_note  (role_note 'scout'/'fixture'/'oracle-managed' are excluded
              from the A/B population but still reported)

Reference DPS = EstimatedGroupDpsValue::GetBasicDps(level) ported verbatim from
src/Ai/Base/Value/EstimatedLifetimeValue.cpp. It is the module's own HEURISTIC, not ground truth.
"""
import argparse, collections, csv, json, sys
from datetime import datetime


def basic_dps(level):  # verbatim port of GetBasicDps (heuristic reference)
    if level <= 15: return 5 + level * 1
    if level <= 25: return 20 + (level - 15) * 2
    if level <= 45: return 40 + (level - 25) * 3
    if level <= 55: return 100 + (level - 45) * 20
    if level <= 60: return 300 + (level - 55) * 50
    if level <= 70: return 550 + (level - 60) * 65
    return 1200 + (level - 70) * 200


def ts(s):
    return datetime.fromisoformat(s.replace('Z', '')[:26])


def load_roster(path):
    with open(path, newline='') as f:
        return {int(r['guid']): r for r in csv.DictReader(f)}


def fold_snaps(lines):
    """Per-bot duty cycle from list snapshots. Deaths = alive->dead transitions (lower bound:
    a death+release between two snapshots is invisible)."""
    per = collections.defaultdict(lambda: {'n': 0, 'combat': 0, 'dead': 0, 'deaths': 0,
                                           'first': None, 'last': None})
    prev = {}
    for d in lines:
        t = ts(d['utc'])
        for b in d['bots']:
            p = per[b['guid']]
            p['n'] += 1
            p['combat'] += bool(b['combat'])
            dead = (not b['alive']) or b.get('state') == 'dead'
            p['dead'] += dead
            if prev.get(b['guid']) is False and dead:
                p['deaths'] += 1
            prev[b['guid']] = dead
            snap = (t, b['level'], b['xp'])
            p['first'] = p['first'] or snap
            p['last'] = snap
    out = {}
    for g, p in per.items():
        (t0, l0, x0), (t1, l1, x1) = p['first'], p['last']
        hours = max((t1 - t0).total_seconds() / 3600.0, 1e-9)
        out[g] = {'samples': p['n'], 'combat_share': p['combat'] / p['n'], 'dead_share': p['dead'] / p['n'],
                  'deaths': p['deaths'], 'hours': hours, 'levels_per_h': (l1 - l0) / hours,
                  # same-level XP only; a level-up makes raw xp delta meaningless without the XP table
                  'xp_per_h_same_level': (x1 - x0) / hours if l1 == l0 else None,
                  'level': l1}
    return out


def fold_combatlog(lines):
    """Per-bot counters. Counter windows retire after 30 s idle, so only samples with an active
    window carry damage; those are reported as window DPS (event damage / window seconds)."""
    per = collections.defaultdict(lambda: {'n': 0, 'in_combat': 0, 'active_windows': 0,
                                           'dmg': 0, 'win_ms': 0, 'taken': 0, 'heal': 0,
                                           'mana_pct': [], 'hp_pct': []})
    for r in lines:
        resp = r.get('resp') or {}
        tel = resp.get('telemetry')
        if not tel:
            continue
        p = per[r['guid']]
        p['n'] += 1
        p['in_combat'] += bool(tel.get('in_combat'))
        p['hp_pct'].append(tel['health']['pct'])
        if tel['mana']['max']:
            p['mana_pct'].append(tel['mana']['pct'])
        c = tel['recent']['counters']
        if c.get('active') and c.get('window_ms', 0) > 0:
            p['active_windows'] += 1
            p['dmg'] += c['damage_done']
            p['win_ms'] += c['window_ms']
            p['taken'] += c['damage_taken']
            p['heal'] += c['effective_healing']
    return per


def report(roster, snaps, clog, out=sys.stdout):
    w = out.write
    if snaps:
        w('## Snapshot duty cycle (profile: bridge list snapshots)\n')
        w('| guid | name | class | note | lvl | samples | combat% | dead% | deaths | lvl/h | xp/h(same lvl) |\n')
        w('|---|---|---|---|---|---|---|---|---|---|---|\n')
        byclass = collections.defaultdict(list)
        for g in sorted(snaps):
            s = snaps[g]; r = roster.get(g, {})
            xp = s['xp_per_h_same_level']
            w(f"| {g} | {r.get('name','?')} | {r.get('class','?')} | {r.get('role_note','')} | {s['level']} | "
              f"{s['samples']} | {100*s['combat_share']:.0f} | {100*s['dead_share']:.0f} | {s['deaths']} | "
              f"{s['levels_per_h']:.2f} | {'' if xp is None else f'{xp:.0f}'} |\n")
            byclass[r.get('class', '?')].append(s)
        w('\n| class | bots | mean combat% | deaths/h (sum) |\n|---|---|---|---|\n')
        for c, ss in sorted(byclass.items()):
            h = sum(s['hours'] for s in ss)
            w(f"| {c} | {len(ss)} | {100*sum(s['combat_share'] for s in ss)/len(ss):.0f} | "
              f"{sum(s['deaths'] for s in ss)/h if h else 0:.2f} |\n")
    if clog:
        w('\n## Combat counters (profile: bridge combatlog spot samples)\n')
        w('| guid | class | samples | in_combat | active windows | window DPS | ref DPS (heuristic) | ratio | taken/s |\n')
        w('|---|---|---|---|---|---|---|---|---|\n')
        for g in sorted(clog):
            p = clog[g]; r = roster.get(g, {}); lvl = snaps.get(g, {}).get('level') if snaps else None
            dps = p['dmg'] / (p['win_ms'] / 1000) if p['win_ms'] else None
            ref = basic_dps(lvl) if lvl else None
            ratio = f'{dps/ref:.2f}' if dps is not None and ref else ''
            taken = f"{p['taken']/(p['win_ms']/1000):.1f}" if p['win_ms'] else ''
            w(f"| {g} | {r.get('class','?')} | {p['n']} | {p['in_combat']} | {p['active_windows']} | "
              f"{'' if dps is None else f'{dps:.1f}'} | {ref or ''} | {ratio} | {taken} |\n")


def selftest():
    assert basic_dps(10) == 15 and basic_dps(20) == 30 and basic_dps(80) == 3200 and basic_dps(60) == 550
    snaps = [
        {'utc': '2026-09-23T00:00:00Z', 'bots': [{'guid': 1, 'level': 10, 'xp': 100, 'alive': True, 'combat': True, 'state': 'combat'}]},
        {'utc': '2026-09-23T00:30:00Z', 'bots': [{'guid': 1, 'level': 10, 'xp': 400, 'alive': False, 'combat': False, 'state': 'dead'}]},
        {'utc': '2026-09-23T01:00:00Z', 'bots': [{'guid': 1, 'level': 10, 'xp': 700, 'alive': True, 'combat': False, 'state': 'non-combat'}]},
    ]
    s = fold_snaps(snaps)[1]
    assert s['deaths'] == 1 and abs(s['combat_share'] - 1/3) < 1e-9 and abs(s['xp_per_h_same_level'] - 600) < 1e-6, s
    tel = {'in_combat': True, 'health': {'pct': 50}, 'mana': {'max': 100, 'pct': 40},
           'recent': {'counters': {'active': True, 'window_ms': 10000, 'damage_done': 500,
                                   'damage_taken': 100, 'effective_healing': 0}}}
    c = fold_combatlog([{'guid': 1, 'resp': {'telemetry': tel}}])[1]
    assert c['dmg'] == 500 and c['win_ms'] == 10000 and c['active_windows'] == 1
    print('selftest ok')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--roster'); ap.add_argument('--snap', nargs='*', default=[])
    ap.add_argument('--combatlog', nargs='*', default=[]); ap.add_argument('--selftest', action='store_true')
    a = ap.parse_args()
    if a.selftest:
        return selftest()
    roster = load_roster(a.roster)
    snaps = fold_snaps([json.loads(l) for f in a.snap for l in open(f) if l.strip()]) if a.snap else {}
    clog = fold_combatlog([json.loads(l) for f in a.combatlog for l in open(f) if l.strip()]) if a.combatlog else {}
    report(roster, snaps, clog)


if __name__ == '__main__':
    main()
