#!/usr/bin/env python3
"""Combat lab reducer: compare a human run and a bot run of the same lab scenario.

Input: lab-trace.jsonl written by mod-playerbots AutoWow.Lab.Trace (one JSON object per line):
  run    {t, run, who, name, cls, ctl, level}            run opened by .autowow lab spawn
  wave   {t, run, wave, part "<entry>x<count>@<level>"}  one per spawn command (wave >= 2 = adds)
  spawn  {t, run, wave, mob, entry, lvl, slot}
  cast   {t, run, who, name, cls, ctl, pet, spell, base, tgt, res ok|start|cancel|fail, gcd, cd, ct[, err]}
  swing  {t, run, who, ..., pet, tgt, att}                white swing roll
  sec    {t, run, who, ..., hp, hpmax, ptype, pw, pwmax, mobs, casting, tgt, dd, dt}   every second
  lowhp  {t, run, who, ..., pct}                          health crossed below pct
  mobdeath {t, run, mob} / death {t, run, who} / reset {t, run, who} / end {t, run, reason clear|death|reset}

Usage:
  lab-compare.py TRACE --list
  lab-compare.py TRACE --human RUN --bot RUN
  lab-compare.py TRACE --name Labrogue [--scenario 27260x2@75]   (latest human vs latest bot run)
  add --json for machine output.
Times are server game-time ms; DPS = damage dealt (player + pet) / run seconds.
"""
import argparse
import collections
import json
import sys

IDLE_GAP_MS = 1500
CD_MIN_MS = 30000
AUTO_SHOT = 75

# First-rank spell ids counted as a defensive / survival answer to low health (heals, shields, escapes, CC).
DEFENSIVE = {
    5277, 31224, 1856, 2983, 408, 2094, 1776,                # rogue
    871, 12975, 55694, 5246, 20230,                          # warrior
    30823, 331, 8004, 2645,                                  # shaman
    22812, 61336, 22842, 8936, 774, 5185,                    # druid
    45438, 122, 1463, 11426, 1953, 11958,                    # mage
    17, 47585, 8122, 2061, 139, 19236,                       # priest
    6789, 5484, 5782, 689, 6229,                             # warlock
    642, 498, 633, 1022, 19750, 635, 853,                    # paladin
    5384, 19263, 781, 1499,                                  # hunter
    43185, 28495, 17534,                                     # healing potions
}


def load(lines):
    rows = []
    for line in lines:
        line = line.strip()
        if not line:
            continue
        try:
            rows.append(json.loads(line))
        except ValueError:
            continue
    return rows


def group_runs(rows):
    runs = collections.OrderedDict()
    for r in rows:
        run = r.get('run')
        if run:
            runs.setdefault(run, []).append(r)
    return runs


def _busy_intervals(casts, swings, secs):
    out = []
    for c in casts:
        if c['res'] == 'ok':
            ct = int(c.get('ct', 0))
            out.append((c['t'] - ct, c['t'] + max(int(c.get('gcd', 0)), 1)))
        elif c['res'] == 'start':
            out.append((c['t'], c['t'] + 1))
    for s in swings:
        out.append((s['t'], s['t'] + 1))
    for s in secs:
        if s.get('casting'):
            out.append((s['t'] - 500, s['t'] + 500))
    return sorted(out)


def idle_gaps(intervals, start, end, gap_ms=IDLE_GAP_MS):
    """Gaps longer than gap_ms between the union of busy intervals inside [start, end]."""
    gaps = []
    cursor = start
    for a, b in intervals:
        if a > cursor and a - cursor > gap_ms:
            gaps.append((cursor, a))
        cursor = max(cursor, b)
    if end - cursor > gap_ms:
        gaps.append((cursor, end))
    return gaps


def summarize(run_rows):
    head = next((r for r in run_rows if r['ev'] == 'run'), None)
    if not head:
        return None
    who = head['who']
    start = head['t']
    end_row = next((r for r in run_rows if r['ev'] == 'end'), None)
    end = end_row['t'] if end_row else max(r['t'] for r in run_rows)
    reason = end_row['reason'] if end_row else 'open'
    dur_s = max(end - start, 1) / 1000.0

    waves = sorted((r for r in run_rows if r['ev'] == 'wave'), key=lambda r: r['wave'])
    spawns = [r for r in run_rows if r['ev'] == 'spawn']
    mine = [r for r in run_rows if r.get('who') == who]
    casts = [r for r in mine if r['ev'] == 'cast' and not r.get('pet')]
    swings = [r for r in mine if r['ev'] == 'swing' and not r.get('pet')]
    secs = [r for r in mine if r['ev'] == 'sec']
    ok = [c for c in casts if c['res'] == 'ok' and c['base'] != AUTO_SHOT]
    ctls = {r['ctl'] for r in mine if 'ctl' in r}

    dealt = sum(s.get('dd', 0) for s in secs)
    taken = sum(s.get('dt', 0) for s in secs)
    gcd_ms = sum(int(c.get('gcd', 0)) for c in ok)
    deaths = {r['mob']: r['t'] for r in run_rows if r['ev'] == 'mobdeath'}

    usage = collections.Counter(c['base'] for c in ok)
    share = {str(k): round(100.0 * v / len(ok), 1) for k, v in usage.most_common()} if ok else {}

    reactions = []
    for sp in spawns:
        acted = [a['t'] for a in casts + swings
                 if a['tgt'] == sp['mob'] and a['t'] >= sp['t'] and a.get('res', 'ok') in ('ok', 'start')]
        reactions.append({'mob': sp['mob'], 'wave': sp['wave'],
                          'ms': (min(acted) - sp['t']) if acted else None})

    low = []
    for lh in (r for r in mine if r['ev'] == 'lowhp'):
        after = [c for c in casts if c['t'] >= lh['t'] and c['res'] in ('ok', 'start')]
        dfn = [c for c in after if c['base'] in DEFENSIVE]
        low.append({'t': lh['t'] - start,
                    'first_action_ms': (after[0]['t'] - lh['t']) if after else None,
                    'defensive_ms': (dfn[0]['t'] - lh['t']) if dfn else None,
                    'defensive': dfn[0]['base'] if dfn else None})

    gaps = idle_gaps(_busy_intervals(casts, swings, secs), start, end)
    return {
        'run': head['run'], 'name': head['name'], 'cls': head['cls'], 'ctl': head['ctl'],
        'ctl_mixed': len(ctls) > 1,
        'scenario': '+'.join(w['part'] for w in waves),
        'end': reason, 'duration_s': round(dur_s, 1),
        'dps': round(dealt / dur_s, 1), 'damage_dealt': dealt, 'damage_taken': taken,
        'dtps': round(taken / dur_s, 1),
        'ttk_s': round((end - start) / 1000.0, 1) if reason == 'clear' else None,
        'mob_ttk_s': {str(m): round((t - start) / 1000.0, 1) for m, t in sorted(deaths.items(), key=lambda kv: kv[1])},
        'casts': len(ok), 'fails': sum(1 for c in casts if c['res'] == 'fail'),
        'swings': len(swings),
        'gcd_busy_pct': round(min(100.0, 100.0 * gcd_ms / (dur_s * 1000.0)), 1),
        'opener': [c['base'] for c in ok[:10]],
        'spell_share': share,
        'cooldowns': [{'spell': c['base'], 't_s': round((c['t'] - start) / 1000.0, 1)}
                      for c in ok if int(c.get('cd', 0)) >= CD_MIN_MS],
        'reaction_first_wave_ms': _avg([x['ms'] for x in reactions if x['wave'] == 1]),
        'reaction_adds_ms': _avg([x['ms'] for x in reactions if x['wave'] > 1]),
        'reactions': reactions,
        'low_hp': low,
        'idle_gaps': len(gaps), 'idle_ms': sum(b - a for a, b in gaps),
        'longest_idle_ms': max((b - a for a, b in gaps), default=0),
    }


def _avg(values):
    vals = [v for v in values if v is not None]
    return round(sum(vals) / len(vals)) if vals else None


def pick_latest(summaries, name, scenario, ctl):
    cands = [s for s in summaries if s['name'] == name and s['ctl'] == ctl and not s['ctl_mixed']
             and (scenario is None or s['scenario'] == scenario)]
    return cands[-1] if cands else None


SCALARS = ['scenario', 'end', 'duration_s', 'ttk_s', 'dps', 'damage_dealt', 'damage_taken', 'dtps', 'casts',
           'fails', 'swings', 'gcd_busy_pct', 'reaction_first_wave_ms', 'reaction_adds_ms', 'idle_gaps', 'idle_ms',
           'longest_idle_ms']


def render(h, b):
    out = ['%-24s %-22s %-22s %s' % ('metric', 'human ' + h['run'], 'bot ' + b['run'], 'bot-human')]
    for k in SCALARS:
        hv, bv = h[k], b[k]
        delta = round(bv - hv, 1) if isinstance(hv, (int, float)) and isinstance(bv, (int, float)) else ''
        out.append('%-24s %-22s %-22s %s' % (k, hv, bv, delta))
    out.append('opener human: %s' % h['opener'])
    out.append('opener bot:   %s' % b['opener'])
    spells = sorted(set(h['spell_share']) | set(b['spell_share']),
                    key=lambda s: -(h['spell_share'].get(s, 0) + b['spell_share'].get(s, 0)))
    out.append('spell share %%: %s' % ', '.join(
        '%s %s/%s' % (s, h['spell_share'].get(s, 0), b['spell_share'].get(s, 0)) for s in spells))
    out.append('cooldowns human: %s' % [(c['spell'], c['t_s']) for c in h['cooldowns']])
    out.append('cooldowns bot:   %s' % [(c['spell'], c['t_s']) for c in b['cooldowns']])
    out.append('low hp human: %s' % h['low_hp'])
    out.append('low hp bot:   %s' % b['low_hp'])
    return '\n'.join(out)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('trace')
    ap.add_argument('--list', action='store_true')
    ap.add_argument('--human')
    ap.add_argument('--bot')
    ap.add_argument('--name')
    ap.add_argument('--scenario')
    ap.add_argument('--json', action='store_true')
    a = ap.parse_args(argv)
    with open(a.trace, encoding='utf-8', errors='replace') as f:
        runs = group_runs(load(f))
    sums = [s for s in (summarize(r) for r in runs.values()) if s]
    if a.list:
        for s in sums:
            print('%s %-10s %-5s %-24s end=%s %ss dps=%s' % (s['run'], s['name'], s['ctl'], s['scenario'], s['end'],
                                                            s['duration_s'], s['dps']))
        return 0
    by_run = {s['run']: s for s in sums}
    if a.human and a.bot:
        h, b = by_run.get(a.human), by_run.get(a.bot)
    elif a.name:
        h = pick_latest(sums, a.name, a.scenario, 'human')
        b = pick_latest(sums, a.name, a.scenario if a.scenario else (h['scenario'] if h else None), 'bot')
    else:
        ap.error('give --list, --human/--bot, or --name')
    if not h or not b:
        print('missing run: human=%s bot=%s' % (h and h['run'], b and b['run']), file=sys.stderr)
        return 1
    if h['scenario'] != b['scenario']:
        print('warning: scenarios differ (%s vs %s)' % (h['scenario'], b['scenario']), file=sys.stderr)
    print(json.dumps({'human': h, 'bot': b}, indent=1) if a.json else render(h, b))
    return 0


if __name__ == '__main__':
    sys.exit(main())
