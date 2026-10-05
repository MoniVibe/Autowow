"""Unit tests for lab-compare.py (python -m unittest discover -s scripts/lab)."""
import importlib.util
import json
import os
import unittest

_spec = importlib.util.spec_from_file_location('lab_compare', os.path.join(os.path.dirname(__file__), 'lab-compare.py'))
lc = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(lc)

WHO = {'who': 7, 'name': 'Labrogue', 'cls': 4}


def row(ev, t, run='R1', **kw):
    r = {'ev': ev, 't': t, 'run': run}
    r.update(kw)
    return r


def cast(t, base, tgt, res='ok', gcd=1000, cd=0, ct=0, ctl='human', run='R1'):
    return row('cast', t, run, ctl=ctl, pet=0, spell=base, base=base, tgt=tgt, res=res, gcd=gcd, cd=cd, ct=ct, **WHO)


def fight(ctl='human', run='R1'):
    """10 s fight vs mob 100, an add (mob 200) at +4 s, low hp at +6 s, all dead at +10 s."""
    rows = [row('run', 1000, run, ctl=ctl, level=75, **WHO),
            row('wave', 1000, run, wave=1, part='27260x1@75'),
            row('spawn', 1000, run, wave=1, mob=100, entry=27260, lvl=75, slot=0),
            cast(1300, 1752, 100, ctl=ctl, run=run),               # reaction 300 ms
            cast(2300, 1752, 100, ctl=ctl, run=run),
            cast(3300, 13750, 100, gcd=0, cd=180000, ctl=ctl, run=run),
            row('wave', 5000, run, wave=2, part='27260x1@75'),
            row('spawn', 5000, run, wave=2, mob=200, entry=27260, lvl=75, slot=0),
            row('swing', 5800, run, ctl=ctl, pet=0, tgt=200, att=0, **WHO),   # add reaction 800 ms
            row('lowhp', 7000, run, ctl=ctl, pct=35, **WHO),
            cast(7500, 1752, 200, ctl=ctl, run=run),               # first action 500 ms
            cast(7600, 999, 200, res='fail', ctl=ctl, run=run),
            cast(8000, 5277, 200, gcd=0, cd=180000, ctl=ctl, run=run),   # Evasion 1000 ms after low hp
            row('mobdeath', 9000, run, mob=100),
            row('mobdeath', 11000, run, mob=200),
            row('end', 11000, run, reason='clear')]
    for k in range(1, 11):
        rows.append(row('sec', 1000 + 1000 * k, run, ctl=ctl, hp=1, hpmax=1, ptype=3, pw=0, pwmax=100, mobs=1,
                        casting=0, tgt=100, dd=500, dt=100, **WHO))
    return rows


class LabCompareTest(unittest.TestCase):
    def test_summary_math(self):
        s = lc.summarize(fight())
        self.assertEqual(s['scenario'], '27260x1@75+27260x1@75')
        self.assertEqual(s['duration_s'], 10.0)
        self.assertEqual(s['ttk_s'], 10.0)
        self.assertEqual(s['dps'], 500.0)          # 10 x 500 / 10 s
        self.assertEqual(s['dtps'], 100.0)
        self.assertEqual(s['casts'], 5)
        self.assertEqual(s['fails'], 1)
        self.assertEqual(s['gcd_busy_pct'], 30.0)  # 3 GCD casts x 1000 ms / 10 s
        self.assertEqual(s['opener'], [1752, 1752, 13750, 1752, 5277])
        self.assertEqual(s['spell_share']['1752'], 60.0)
        self.assertEqual([c['t_s'] for c in s['cooldowns']], [2.3, 7.0])
        self.assertEqual(s['reaction_first_wave_ms'], 300)
        self.assertEqual(s['reaction_adds_ms'], 800)
        self.assertEqual(s['low_hp'][0]['first_action_ms'], 500)
        self.assertEqual(s['low_hp'][0]['defensive_ms'], 1000)
        self.assertEqual(s['low_hp'][0]['defensive'], 5277)
        self.assertEqual(s['mob_ttk_s'], {'100': 8.0, '200': 10.0})

    def test_idle_gaps(self):
        # busy [0,1000] [3000,3500] [3600,4000]; window 0..6000 -> gaps 1000-3000 and 4000-6000
        gaps = lc.idle_gaps([(0, 1000), (3000, 3500), (3600, 4000)], 0, 6000)
        self.assertEqual(gaps, [(1000, 3000), (4000, 6000)])
        self.assertEqual(lc.idle_gaps([(0, 1000), (2400, 3000)], 0, 3000), [])  # 1.4 s is not idle
        # a 2.5 s cast is busy from its start: ok row at t with ct=2500 covers (t-2500, t)
        busy = lc._busy_intervals([{'res': 'ok', 't': 4000, 'ct': 2500, 'gcd': 1500}], [], [])
        self.assertEqual(lc.idle_gaps(busy, 1500, 5500), [])
        s = lc.summarize(fight())
        self.assertEqual(s['idle_gaps'], 3)       # 3301 -> 5800, 5801 -> 7500, 8500 -> 11000
        self.assertEqual(s['longest_idle_ms'], 2500)

    def test_pick_and_render(self):
        rows = fight('human', 'H1') + fight('bot', 'B1')
        sums = [lc.summarize(r) for r in lc.group_runs(rows).values()]
        h = lc.pick_latest(sums, 'Labrogue', None, 'human')
        b = lc.pick_latest(sums, 'Labrogue', h['scenario'], 'bot')
        self.assertEqual((h['run'], b['run']), ('H1', 'B1'))
        text = lc.render(h, b)
        self.assertIn('dps', text)
        self.assertIn('opener bot:', text)

    def test_load_skips_garbage_and_open_run(self):
        rows = lc.load([json.dumps(r) for r in fight()[:5]] + ['not json', ''])
        s = lc.summarize(rows)
        self.assertEqual(s['end'], 'open')
        self.assertIsNone(s['ttk_s'])


if __name__ == '__main__':
    unittest.main()
