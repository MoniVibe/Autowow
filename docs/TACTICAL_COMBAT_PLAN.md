# Tactical combat layer for solo-leveling bots (v1 design)

Status: **design only. Nothing built, nothing applied.** This is a read-only lane:
- no server, config, bridge, DB or git mutations
- no builds

Evidence sources:
- **Module source:** WSL `/root/autowow-advisor-t1-module` `main`. It moved from `ef8c9302` to `2333c993` while I was reading. The intervening commits touch travel-intent, the quest scheduler, the ledger `.cpp` (+2 lines) and `PlayerbotAIConfig.*`. None of the other files cited below changed. Paths are relative to `src/`.
- **Live config:** `/usr/local/etc/modules/playerbots.conf`. Only the keys named here were read.
- **DBC:** `/root/p1data/dbc/{Talent,TalentTab,Spell}.dbc`, decoded with a stdlib script.
- **Telemetry:** profile `soak-s6-cohort-r1 / combat-reduce cv=1`, `work/soak-20260923/combat-s6/combat-summary.md`.

Evidence tags: **[S]** source, **[M]** measured, **[D]** DBC, **[K]** game knowledge that has not been checked against the trainer DB (verify in the build lane), **[H]** heuristic.

---

## 0. The headline facts (read these first)

1. **Wand Specialization does not exist in WotLK 3.3.5** [D].
   - Spell 14524 "Wand Specialization" is still in `Spell.dbc`, but no row in `Talent.dbc` for any class references it.
   - Discipline tier 1 is now Unbreakable Will / Twin Disciplines.
   - The owner's "spec Wand Spec + PW:S + wand" loop is the Vanilla/TBC idiom.
   - The 3.3.5 equivalents for the sustain half are:
     - Spirit Tap 3/3 + Improved Spirit Tap 2/2 (Shadow row 0: post-kill mana regen)
     - Meditation (Disc r2)
     - Improved PW:Shield (Disc r2)
     - Vampiric Embrace (Shadow r4: self-heal while doing damage)
   - The wand-cycle tactic itself still works. Only the talent premise changes. §3.4 has a validated leveling template.
2. **Priests fight in the reverse order of the owner's intent** [S+M].
   - The default action list ranks Smite above the wand. The engine casts Smite until it is out of mana, and only then falls through to `shoot` (details in §1.2).
   - They drink only at 15 % mana (`LowMana = 15`, `UseFoodStrategy.cpp:22-23`), so they re-pull at about 16 %.
   - Result [M] for priest/1-20: **starved 42.8 % of combat time**, lowest in-combat DPS of any class (9.5), highest TTK (median 8.3 s, p90 14.9 s), 0 deaths. They are safe but slow, and slow because they are out of mana, not because of danger.
3. **Psychic Scream is never cast in solo combat** [S].
   - The action exists (`PriestAiObjectContext.cpp:199`, `PriestActions.h:124`).
   - A node exists with a Fade alternative (`GenericPriestStrategyActionNodeFactory.h:202-208`).
   - **No priest strategy has a trigger for it.**
4. **Pull selection has no pack awareness** [S]. `GrindTargetValue.cpp` screens on:
   - level delta > 4
   - elite (only when the bot is not in a group that can fight elites)
   - LOS
   - Z delta
   - distance

   It never counts the hostiles standing next to the candidate. The strict quest branch (`:147-270`) and the legacy branch (`:305-396`) both pick the nearest or path-best candidate. An existing "possible adds" value (`AttackersValue.cpp:245-277`) is wired only into the tank `PullStrategy.cpp:260`.
5. **A shadow priest has no self-heal in combat** [S].
   - `ShadowPriestStrategy` / `GenericPriestStrategy` heal only with PW:S (`GenericPriestStrategy.cpp:25-29,43-44`) and Desperate Prayer.
   - In 3.3.5, Desperate Prayer is a Holy talent (r2c0) [D], and it is **not in any shipped template**.
   - Self-heals come only from `HealPriestStrategy`, i.e. the disc/holy "heal" strategies.
   - So switching leveling priests to Shadow (the right leveling spec) **without** the tactical layer would remove their heals.

---

## 1. How stock playerbots combat works today

### 1.1 The engine model (strategy → trigger → action with relevance)

- **Strategies** are named bundles that are added to a per-state engine (combat / non-combat) at login by `AiFactory::AddDefaultCombatStrategies` (`Bot/Factory/AiFactory.cpp:285-518`). `Engine::Init` (`Bot/Engine/Engine.cpp:118-135`) walks every active strategy and collects:
  - `InitTriggers` → `TriggerNode(trigger name, {NextAction(action, relevance)...})`
  - `InitMultipliers` → `Multiplier*` list
  - `getDefaultActions()` → the actions used when nothing fires, around `ACTION_DEFAULT = 5`
  - `actionNodeFactories` → per-action **prerequisites / alternatives / continuers**
- **Relevance scale** (`Strategy/Strategy.h:53-61`):
  - `IDLE 0, DEFAULT 5, NORMAL 10, HIGH 20, MOVE 30, INTERRUPT 40, DISPEL 50, RAID 60`
  - heal constants: `LIGHT_HEAL 10, MEDIUM_HEAL 20, CRITICAL_HEAL 30, EMERGENCY 90`
- **Each tick** (`Engine::DoNextAction`, `Engine.cpp:~150-252`):
  1. Fired triggers push their NextActions into a priority queue.
  2. The engine pops the highest relevance.
  3. If `isUseful()`, **every multiplier** scales the relevance (`:184-193`). A result of 0 kills the action.
  4. If `isPossible()`, prerequisites are pushed at +0.002 and the action executes.
  5. On failure or impossibility, the **alternatives** are pushed at +0.003 (`:226,232`).

  This alternatives chain is how "Smite → Shoot" fallback works.
- **Values** (`AI_VALUE(...)`) are cached computed state such as "attackers", "possible adds", "my attacker count" or "estimated lifetime" (`Ai/Base/ValueContext.h:125-327`).
- **Consequence for design:** changing strategies re-runs `Engine::Init`, which resets triggers, multipliers and the queue. So **a tactic must not be a strategy swap**. A tactic is a *value*. A static strategy's triggers read that value, and one multiplier reads a data table. §2 is built on this.

### 1.2 What a solo leveling priest actually runs

**Spec tab** (`AiFactory.cpp:70-107`):
- Below level 10, every priest is treated as `PRIEST_TAB_HOLY`.
- From level 10, the tab with the most talent points wins.

**Random-bot spec draw** (live conf `RandomClassSpecProb.5.{0,1,2}`): disc 40, holy 35, shadow 25. **So about 75 % of priests level as healer specs.** The shipped templates exist only at levels 60 and 80 (`PremadeSpecLink.5.{0..5}.{60,80}`).

**Combat strategies:**

| Bot kind | Strategies (file:line) | Effective rotation |
|---|---|---|
| Random bot, solo, disc/holy (includes all levels < 10) | `holy heal` (`AiFactory.cpp:325`), `dps assist, cure` (`:327`), `save mana` and `healer dps` (`:427-432`), and the solo block `holy dps, shadow debuff, shadow aoe` (`:446-449`) | Triggers: SW:P at `ACTION_HIGH+1 = 21` (`ShadowPriestStrategy.cpp:127-133`), DP 22, VT 23; SW:P on other attackers at 15 (`:75-81`). Defaults: **Smite 5.2 > Mana Burn 5.1 > Starshards 5.0** (`HolyPriestStrategy.cpp:33-40`) plus `holy heal`'s Shoot 5.0 (`:85-88`). Smite's alternative is Shoot (`:17-24`). |
| Cohort / non-random bot, no group (`IsRandomBot` false, so the solo block at `:435-470` is skipped) | `holy heal`, `healer dps` | `HealerShouldAttackTrigger` is always true when alone (`Ai/Base/Trigger/GenericTriggers.cpp:399-402`) → SW:P 5.5 > Holy Fire 5.4 > **Smite 5.3** > Mind Blast 5.2 > Shoot 5.0 (`GenericPriestStrategy.cpp:78-87`). |
| Shadow tab | `dps, shadow debuff, shadow aoe` (`AiFactory.cpp:321`) | Mind Blast 5.3 > Mind Flay 5.2 > SW:Death 5.1 > Shoot 5.0 (`ShadowPriestStrategy.cpp:17-25`); alternatives MB→MF→Smite→Shoot (`ShadowPriestStrategyActionNodeFactory.h:26-48`) |

**Existing priest defensive and utility wiring** (`Strategy/GenericPriestStrategy.cpp`):

| Owner intent | Stock status |
|---|---|
| PW:Shield | Three triggers: `being attacked` → 21 (`:43-44`, fires whenever ≥ 1 attacker); `low health` (< 45) → 20 (`:28-29`); `critical health` → 10 (`:25-26`). **No mana gate.** At low levels this is a large mana sink that is recast every time Weakened Soul drops. |
| Wand | Only as the lowest default / alternative (5.0). Used **after** mana is gone. `CastShootAction` (`Ai/Base/Actions/GenericSpellActions.cpp:599-642`) with a wand casts "shoot" (5019) by name. |
| Renew / heals | Solo holy/disc only. `PartyMemberToHeal` returns the bot itself when ungrouped (`Ai/Base/Value/PartyMemberToHeal.cpp:127-129`), so `party member {almost full/medium/low/critical} health` → Renew / PW:S / Penance / Flash→Greater→Heal→Lesser chain (`HealPriestStrategy.cpp:50-95`; alternatives in `GenericPriestStrategyActionNodeFactory.h:130-199`). Thresholds come from the global config (`AlmostFull 85 / Medium 65 / Low 45 / Critical 25`). **Shadow: none** (fact 5). |
| DoTs | SW:P/VT/DP at 21-23 on the current target and SW:P on other attackers at 15. These are now **gated by the C5 lifetime gate** (`AutoWow.Combat.DotLifetimeGate = 1` live; `AutoWow/DotLifetimeGate.h`) [M: priest `dot_skips` 130]. |
| Psychic Scream / Fade | **Not triggered** (fact 3). `medium threat` → Fade (`:22`) needs a main tank (`GenericTriggers.cpp:222-228`), so it is dead solo. |
| Mana management | `medium mana` → Shadowfiend / Inner Focus (level 66 / talent); `low mana` → Hymn of Hope (64); shadow → Dispersion (60); **holy `low mana` → Mana Burn at 20** (`HolyPriestStrategy.cpp:70-77`), which burns the last mana on a mob that usually has none. **Below level 60 nothing is mana-aware except drinking at 15 %.** |
| Too-close handling | `enemy too close for spell` → flee at 39 (`:40-41`). It fires only if the target is *not* on the bot, or is frozen/rooted (`Ai/Base/Trigger/RangeTriggers.cpp:15-19`), so it rarely fires solo. |
| Avoid bad pulls | None (fact 4). `can fight elite` is false when solo (`Ai/Base/Value/MaintenanceValues.cpp:104-108`), so elites are skipped. That is the only danger screen. |
| Rest | Non-cheat: eat below `LowHealth 45`, drink below `LowMana 15` (`UseFoodStrategy.cpp:20-24`; live `BotCheats = "taxi,raid"`). |

### 1.3 Talent templates vs leveling (decoded from DBC)

Links are applied **row-major within a tab, in link order** (`PlayerbotAIConfig::ParseTempTalentsOrder`). `InitTalentsByTemplate` (`Bot/Factory/PlayerbotFactory.cpp:3546+`) starts from the highest level ≤ the bot's level that has a link, then walks up until the points run out. **Staged per-level links are therefore supported** and give a curated learning order.

- `disc pve 60` (5.0): Twin Disciplines 5, Imp Inner Fire 3, Imp Fort 2, Meditation 3, Inner Focus, **Imp PW:S 3**, Mental Agility … Penance. It is a healer build. At low levels it takes TD, IIF, Fort, then Meditation and Imp PW:S (row 2). That is survivable but gives no damage.
- `holy pve 60` (5.1): Imp Renew 3, Holy Spec 5, Divine Fury 5 (a smite cast-time talent) … **no Desperate Prayer**.
- `shadow pve 60` (5.2): Spirit Tap 3, **Improved Spirit Tap 2**, Darkness 5, Shadow Focus 3, Imp MB 4, Mind Flay … Dispersion. It is the closest to a leveling build, but in row-major order Mind Flay arrives only at about level 29, and it has **no Improved Psychic Scream**.
- **Wand Spec:** not present (fact 1).

---

## 2. Architecture: tactics as engine citizens

```
            re-eval every ReevalMs (+ on attacker-count change / fear break / HP-mana band cross)
                               │
  [EngagementSnapshot value] ──┼──> TacticPolicy (pure, per class family, hysteresis) ──> [tactic value]
   attackers, adds, levels,    │                                                           │
   elite/caster flags, own     │        ┌──────────────── static "tactical" strategy ──────┤
   hp/mana/slope, cds, pvp     │        │  triggers "tactic is X [& condition]" → NextActions
                               │        │  TacticMultiplier: data table action→factor per tactic
                               │        └──────────────────────────────────────────────────
   GrindTargetValue ── PackRisk(candidate, class capacity) ── reject/penalize bad packs
   engagement ledger ── one `engage` line per engagement (tactic path + outcome)
```

### 2.1 EngagementSnapshot (the assessment value)

This is a new `CalculatedValue<EngagementSnapshot>` named `"engagement"`, recomputed at most every `ReevalMs`. It uses only existing values and unit state; no new core hooks are needed.

| Field | Source |
|---|---|
| `attackers`, `melee_attackers` (within 5 yd), `ranged_casters` | `"attackers"` + creature `unit_class`/power type (mana-user creature = caster) |
| `adds_near` (idle hostiles within `LinkRadius` of any attacker) | same logic as `PossibleAddsValue` (`AttackersValue.cpp:245-277`), but **counted**, not just a bool |
| `patrol_near` (moving, not-engaged hostile within `PatrolRadius`) | `"possible targets no los"` + `isMoving()` |
| `lvl_dmax`, `elite_n`, `rare_n` | creature template rank / level vs bot |
| `load` | Σ over attackers + adds_near of `w(m) = rankMul × clamp(1 + LvlStep·Δlvl, 0.4, 2.0) × (caster ? CasterMul : 1)`. Defaults: rankMul normal 1 / rare 1.5 / elite 3; LvlStep 0.15; CasterMul 1.2 [H]. Every weight is a config value. |
| `hp_pct`, `mana_pct` (or rage/energy/focus/runic) | self |
| `hp_slope` → `death_eta_ms` | HP samples taken by the value itself over the last ~3 s. It is deterministic and does not depend on the telemetry store. |
| `cds` bitmask | class-specific readiness, e.g. `SCREAM_READY`, `SHIELD_READY` (no Weakened Soul), `DP_READY`, `PET_ALIVE` |
| `feared_n`, `cc_n` | attackers with a fear/stun/poly aura (used to detect a fear break) |
| `pvp_threat` | a hostile player within `SightDistance` who is targeting the bot or is ≥ level-2 and in combat nearby |

### 2.2 Tactic policy (pure, deterministic, unit-tested)

`src/AutoWow/TacticalPolicy.h`, in the style of `DotLifetimeGate.h`: header-only, no core types.

- `enum class TacticId : uint8_t`: **wire-stable, append-only, ids never reused**. Ids are namespaced by class family, e.g. priest 10-19, warlock 20-29. `0 = None/stock`.
- `TacticId Choose(ClassFamily, const EngagementSnapshot&, const TacticParams&, TacticId current, uint32 msInCurrent)`.
- **Hysteresis:**
  - Downgrade (e.g. Multi → Single) requires `MinDwellMs` (default 3000) plus the exit threshold.
  - Upgrade to Emergency / Escape is immediate.
  - Enter and exit thresholds differ, for example Emergency enters at `hp < 30` and exits at `hp > 45`.
- **Capacity** `C(class, snapshot)` = base per class, plus a bonus per ready control cooldown (Scream +1.0, Shield +0.5), × `hp_pct/100` × `(0.5 + 0.5·mana_pct/100)`. Decision ladder:
  - `load ≤ SingleMax` → Single (Wand-cycle or Burst by mana)
  - `load ≤ C` → Multi-control
  - `death_eta < DeathEtaMs` or `hp < EmergencyHp` → Emergency
  - `load > C·EscapeRatio` **and** emergency tools spent → Escape (at most **one** escape attempt per engagement, so escape cannot become a loop)
- The result goes into a `"tactic"` value (`{id, sinceMs, engagementId}`).

### 2.3 How a tactic drives the engine (no parallel brain)

This needs one static strategy per class, e.g. `"tactical"`, registered in the class strategy context and added in `AiFactory` behind the flag and arm. It contributes two things.

1. **Triggers** named `"tactic <name>"`, optionally with a condition (e.g. `"tactic multi & scream ready & melee>=2"`). Each pushes a small NextAction list at a tactic-specific relevance. This is how tactic-only actions enter the queue: Psychic Scream, "target lowest-hp attacker", "retreat to pull origin".
2. **`TacticMultiplier`**. Following the precedent of `HealerAutoSaveManaMultiplier` (`Ai/Base/Strategy/ConserveManaStrategy.cpp:93-135`), it reads a per-tactic **action→factor table** from config. A factor of 0 suppresses the action (e.g. Smite in Wand-cycle) and a factor > 1 promotes it. Actions not in the table get 1.0, so every stock trigger keeps working: dispels, interrupts, avoid-aoe, potions.

Stock behaviour is untouched when the flag is off (the strategy is not added) or the bot is in the control arm.

### 2.4 Pull selection (bad-pack avoidance)

`src/AutoWow/PackRisk.h` (pure) provides `PackRisk Score(candidate w, neighbors[], capacity)`. It adds up the `w(m)` of the candidate plus idle hostiles within `LinkRadius` (default 12 yd [H]; social aggro), plus patrols within `PatrolRadius` whose path crosses the pull point.

Hooks in `Ai/Base/Value/GrindTargetValue.cpp`:
- **Strict branch** (`:147-270`):
  - Hard-reject when `risk > C·EscapeRatio`.
  - Otherwise, add `risk_band` as a sort key **after** `shared` and **before** `pathDist` (`:235-241`).
  - Consequence: a bot prefers a lone quest mob 10 yd further away over a trio next to it.
  - It never broadens legality. If everything is rejected, the branch returns nullptr, as it already does today, and the quest phase machine waits or moves.
- **Legacy branch** (`:305-396`): the same reject rule, plus a `distance + RiskYd·risk_band` score.

All tie-breaks stay deterministic: the GUID counter is already the last key.

### 2.5 Config (all default off; OFF means one cached bool per hook)

```
AutoWow.Tactics.Enable            = 0      # master
AutoWow.Tactics.Observe           = 0      # compute + label + ledger only; no behaviour (lane T1)
AutoWow.Tactics.Classes           = "priest"
AutoWow.Tactics.ArmPct            = 50     # % of eligible bots in treatment
AutoWow.Tactics.ArmSalt           = 1      # arm = hash32(guidCounter ^ salt) % 100 < ArmPct
AutoWow.Tactics.ReevalMs          = 500
AutoWow.Tactics.MinDwellMs        = 3000
AutoWow.Tactics.DeathEtaMs        = 6000
AutoWow.Tactics.EscapeRatio       = 1.6
AutoWow.Tactics.Load.{EliteMul,RareMul,LvlStep,CasterMul} = 3,1.5,0.15,1.2
AutoWow.Tactics.Priest.{SingleMax,Base}                   = 1.2,1.0
AutoWow.Tactics.Priest.WandManaPct       = 60   # below: Wand-cycle; above: Burst allowed
AutoWow.Tactics.Priest.ShieldMinManaPct  = 20
AutoWow.Tactics.Priest.RenewHpPct        = 70
AutoWow.Tactics.Priest.HealHpPct         = 45
AutoWow.Tactics.Priest.EmergencyHpPct    = 30
AutoWow.Tactics.Priest.ScreamMinMelee    = 2
AutoWow.Tactics.Priest.ScreamHpPct       = 60
AutoWow.Tactics.Priest.Factors.<tactic>  = "smite:0,mind blast:0.5,shoot:3"   # multiplier table
AutoWow.PullRisk.Enable = 0 ; .LinkRadius = 12 ; .PatrolRadius = 25 ; .RiskYd = 15
```

Retrofit-proofing:
- Tactic ids and ledger fields are append-only.
- The schema version (`ecv`) is in every line.
- There are no strings in the policy core. Action names live only in the adapter tables.
- Arm assignment is a pure hash, so it is reproducible across restarts.
- All re-evaluation runs on the bot's own AI tick, so there is no new threading.

---

## 3. Priest template in detail

### 3.1 Tactics

| Id | Tactic | Enter when | Sequence (by relevance; stock heals/dispels still apply) |
|---|---|---|---|
| 10 | **P-WAND** (single, sustain) | `load ≤ SingleMax`, `mana < WandManaPct` or mob normal ≤ +1 lvl | 1) **PW:S** if a melee attacker is on the bot, `SHIELD_READY` and `mana ≥ ShieldMinManaPct`. 2) **SW:P** once, via the C5 gate. 3) **Wand** to death (`shoot` ×3). **Smite ×0**, Mind Blast ×0.5 (opener only when the target HP > 70 %), Mana Burn ×0, Holy Fire ×0. Renew at `hp < RenewHpPct`, heal at `hp < HealHpPct`. |
| 11 | **P-BURST** (single, fast) | single and (`mana ≥ WandManaPct` or target is a caster, since casters don't melee so the shield is weak, or the target is rare) | Holy Fire / Mind Blast opener → SW:P → Smite / Mind Flay until `mana < WandManaPct`, then hand off to P-WAND (hysteresis) |
| 12 | **P-MULTI** (2-3 mobs, control) | `SingleMax < load ≤ C` | 1) PW:S immediately. 2) SW:P spread over attackers (the stock `on attacker` trigger, promoted ×2). 3) Renew self. 4) **Psychic Scream** when `melee ≥ ScreamMinMelee` and (`hp < ScreamHpPct` or `attackers ≥ 3`) and `adds_near == 0` (so fear does not run mobs into a new pack). 5) While feared: **wand / Mind Flay the lowest-HP attacker**; the new action `"select lowest hp attacker"` sets the current target. 6) On a fear break (`feared_n` drops) the tactic re-evaluates right away. Smite ×0.5. Heals at +10 pp (Renew 80 / Heal 55). |
| 13 | **P-EMERGENCY** | `hp < EmergencyHpPct` or `death_eta < DeathEtaMs` | Desperate Prayer (if talented) → Scream (any melee) → PW:S → Flash Heal (→ Heal → Lesser via stock alternatives) → Renew → racials / potion (stock `potions`). Damage ×0 except wand. |
| 14 | **P-ESCAPE** | load > `C·EscapeRatio` and Scream + Shield spent, or `hp < EscapeHpPct` with ≥ 2 attackers | Scream if ready → move toward the **pull origin** (bot position recorded at engagement start) for up to 30 yd or 8 s → re-evaluate. Mobs that leash evade, which ends the engagement as outcome `escaped`. One attempt per engagement. |
| — | **Pre-pull** (non-combat, same value) | a pull target is chosen | PW:S pre-cast only if `mana ≥ 80` and the target is not a caster; SW:P or Holy Fire opener from max range; skip a pack when PackRisk rejects it (§2.4) |

### 3.2 Rest coupling (the downtime vs starvation trade)

The 42.8 % starvation comes from re-pulling at 16 % mana. The tactic layer adds a **pre-pull readiness gate** in the tactical non-combat piece:
- Do not start a *proactive* pull (grind or quest) below `PullMinManaPct` (default 50) or `PullMinHpPct` (default 60).
- Drink or eat instead.
- Self-defence is unaffected.

With Spirit Tap and P-WAND mana spend, this should cost less rest than it saves in TTK. It is a measured claim, not an assumed one (§4). It overlaps K1/K2 in `COMBAT_REPORT.md` §5b. Keep this gate as the *code* alternative to the global `LowMana` knob, which also changes in-combat triggers.

### 3.3 Spells by level band [K: verify against the trainer table in lane T2]

| Band | New tools | Tactics available |
|---|---|---|
| 1-5 | Smite, Lesser Heal, PW:Fort, SW:P (4), wand if equipped (bots get the Wands skill, `PlayerbotFactory.cpp:3220-3242`) | WAND (without the shield), BURST |
| 6-9 | PW:Shield (6), Renew (8), Fade (8) | full WAND; MULTI has no control yet, so it is shield + renew + kite-free wand |
| 10-13 | Mind Blast (10), Inner Fire (12); talents start (Spirit Tap) | + BURST opener |
| 14-19 | **Psychic Scream (14)**, Heal (16), Cure Disease, Dispel (18) | + MULTI with control, EMERGENCY |
| 20-29 | Flash Heal, Holy Fire, Devouring Plague, Holy Nova, Shackle (20), Mana Burn (24); **Mind Flay (talent, level 20 in the staged template)** | MULTI swaps the wand filler for Mind Flay when mana allows |
| 30-39 | Vampiric Embrace (talent), Improved Psychic Scream (talent), Prayer of Healing, Mind Control (30) | VE = passive self-heal; scream CD 26 s |
| 40-49 | Shadowform (talent), Greater Heal (40) | Shadowform: heals need `remove shadowform`, which the stock prerequisite already handles, so EMERGENCY accepts it |
| 50-59 | Vampiric Touch (talent) | mana return lowers the WandManaPct need |
| 60-80 | Dispersion (60), SW:Death (62), Hymn (64), Shadowfiend (66), Mind Sear (75) | stock endgame triggers already exist |

### 3.4 Leveling talent template (validated against Talent.dbc)

Priest spec index `5.6` "shadow leveling" is proposed with **staged links**. Every point was checked: the row gate (5 × row points), the max rank, and a total of 51 at level 60.

```
AiPlayerbot.PremadeSpecName.5.6     = shadow leveling
AiPlayerbot.PremadeSpecLink.5.6.20  = --325000001000000000000000000   # ST3 IST2 Darkness5 MindFlay1 (11)
AiPlayerbot.PremadeSpecLink.5.6.30  = --325020251000010000000000000   # +ImpSWP2 ImpScream2 ImpMB5 VE1 (21)
AiPlayerbot.PremadeSpecLink.5.6.40  = --325020251200012320100000000   # +Veiled2 ImpVE2 FocMind3 MindMelt2 Shadowform (31)
AiPlayerbot.PremadeSpecLink.5.6.50  = --325020251200012320152300000   # +ShadowPower5 ImpSF2 Misery3 (41)
AiPlayerbot.PremadeSpecLink.5.6.60  = --325020251200012320152301351   # +VT1 P&S3 TwistedFaith5 Dispersion1 (51)
AiPlayerbot.PremadeSpecLink.5.6.80  = (reuse 5.2.80)
```

- Sustain picks that replace Wand Spec: Spirit Tap / Improved Spirit Tap (row 0, **first 5 points**) and Vampiric Embrace + Improved VE.
- Control picks: Improved Psychic Scream (a shorter CD for P-MULTI).
- Dropped versus 5.2: Shadow Focus, Shadow Weaving, Shadow Reach (these are group, hit or range items).
- **This changes spec distribution, which is an owner/product call.** Options:
  - (a) raise `RandomClassSpecProb.5.6` for all priests; this also turns 80-level dungeon healers shadow
  - (b) apply only to leveling cohorts via the cohort/AutoMaintenance path
- Existing bots keep their tab. The increment path uses the current tab (`PlayerbotFactory.cpp:1569-1575`), so a re-spec needs a reset.
- Verify in T0 that the config loader reads spec numbers up to `MAX_SPECNO = 20` (`PlayerbotAIConfig.h:79`).

---

## 4. Telemetry and A/B

### 4.1 New ledger event `engage` (reserve id **11** plus `ecv=1` on LANE_BOARD CLAIMS before code)

There is one line per engagement. An engagement starts on combat false→true (the existing C1 hook) and ends 1 s after combat true→false (debounced). Fields are append-only.

```
ev=engage ecv=1 bot cls lvl tab arm eng_id
tac0            initial tactic id
tacs            [[id,ms],...] tactic path (dwell per tactic, in order)
mobs_max adds elite_n lvl_dmax load_max pvp
dur_ms kills outcome   # 0 win, 1 died, 2 escaped, 3 died_after_escape, 4 evade/leash, 5 abandoned
hp0 hp1 mp0 mp1        # pct at start/end (resource of class)
mana_spent dmg_taken heal_self casts wand_ms
cc_n (scream/fear/nova/trap/sap uses) shield_n
gap_ms gap_rest_ms     # time since previous engagement end, of which rest (sit + food/drink aura)
pull_risk              # PackRisk band at pull time (-1 = not a proactive pull)
```

- `combat` line **cv=2**: append `tac_ms=[[id,ms],...]` and `arm`. Fields stay append-only, and `combat-reduce.py` reads both cv=1 and cv=2.
- Rest time is not in C2 today. `gap_rest_ms` closes the `restMs` gap noted in the combat report C2.
- Reducer: `scripts/engage-reduce.py` (stdlib, `--selftest`).
  - Cells: `class × band × arm × tac0`.
  - Metrics: TTK, mana spent per kill, deaths per 100 engagements, escape success, `gap_rest_ms` per kill, and **XP/h joined from bridge snapshots** (the product metric).
  - Sums stay sterile: engagements in cell = Σ by outcome.

### 4.2 A/B design

- **Per-bot deterministic arms in the same run** (`ArmPct`, `ArmSalt`) replace ABAB segments. This removes the segment and time confound that §5b of the combat report had to accept.
  - Control arm: the flag is on, but the bot is not given the strategy. In `Observe` mode it is still *labelled* with the tactic that would have been chosen, so both arms carry comparable `tac0` cells.
- **Stage 0, Observe:** there is no behaviour change. This gives the stock baseline per tactic-situation. For example, "how often does a stock priest face `load > C`, and what happens to it".
- **Stage 1, priest treatment:** compare arms.
  - Primary metric: XP/h per band.
  - Guardrails: deaths/h, starved share, downtime.
- **Sizing [H]:** per-engagement TTK CV ≈ 0.5 → about 175 engagements per arm to detect 15 %. At about 44 kills per bot-hour for priests [M: s6, 129 kills / 2.95 bot-h], that is **≈ 4 bot-h per arm**. Deaths still need about 20 bot-h per arm (combat report §5d). So run ≥ 10 priests per arm for ≥ 2 h.
- **Single-knob follow-ups** such as `WandManaPct` 40/60/80 reuse the arm hash with a different salt.

---

## 5. Framework: other classes

The same value, policy, `"tactical"` strategy and multiplier apply to every class. Only the tactic set, the cooldown bits and the factor tables differ. "Stock" is the result of a grep of `Ai/Class/<Class>/Strategy` for a trigger or NextAction that uses the spell.

| Class | Tactics (defining nuance) | Stock wiring |
|---|---|---|
| **Warlock** | **Drain-tank** (pet tanks, Corruption/Agony via the gate, **Drain Life** when hp < 70, Life Tap at hp > 60); **Multi-fear** (Fear the second mob, dots on all, re-fear on break, never fear with `adds_near`); **Pet-save** (Health Funnel when the pet is low, Soul Link); **Emergency** (Death Coil, Howl of Terror) | fear wired as `fear on cc` 32 (`GenericWarlockStrategy.cpp:128-133`); **Drain Life NONE**, **Health Funnel NONE**; Drain Soul only in affliction (`AfflictionWarlockStrategy.cpp:87`) |
| **Mage** | **Frost-kite** (Frostbolt → Nova → step out → Frostbolt; frost only); **Burst-single** (fire/arcane at high mana, wand at low mana); **Multi-control** (Poly the second mob, Nova + Cone/AE, Blink out); **Emergency** (Ice Block, Mana Shield, Evocation after) | Nova 50 / Blink-back 35 (`GenericMageStrategy.cpp:104-105`), Ice Block 90, Mana Shield 85, Poly (`:171`) |
| **Hunter** | **Pet-tank** (send pet, Growl hold, auto-shot, Mend Pet at < 50); **Trap-and-shoot** (Freezing Trap the second mob, then kill the first); **Kite** (Concussive/Wing Clip + Disengage on melee without pet aggro); **Feign-reset** (FD when the pack is too big) | Wing Clip/Disengage (`GenericHunterStrategy.cpp:82-84`), Freezing Trap (`:101`), Mend Pet (`:72`); pet-tank threat pacing and FD-escape not tactic-driven |
| **Rogue** | **Opener-single** (stealth, Cheap Shot / Ambush, finishers); **Sap-split** (Sap the add pre-pull, then fight the mark); **Multi-survive** (Evasion, Gouge → Blind, Kidney on the second); **Vanish-escape** | Sap (`DpsRogueStrategy.cpp:398`), Evasion (`:142`); **Gouge NONE**; Blind/Vanish escape not tactic-driven |
| **Warrior** | **Rage-pace single** (charge, Rend via the gate, Heroic Strike only on rage surplus, Victory Rush heal); **Multi** (Thunder Clap + Demo Shout, Sweeping/Cleave, Intimidating Shout at 3+); **Emergency** (Shield Wall / Last Stand / Retaliation, Intimidating Shout + run) | Victory Rush (`FuryWarriorStrategy.cpp:125`, `ArmsWarriorStrategy.cpp:194`), Intimidating Shout (`ArmsWarriorStrategy.cpp:230`), Demo (`GenericWarriorStrategy.cpp:66`); Thunder Clap only in tank (`TankWarriorStrategy.cpp:148`) |
| **Paladin** | **Seal-judge single** (Seal of Righteousness/Command, Judgement on CD, no heal cost until hp < 50); **Self-heal pacing** (Flash of Light vs Holy Light by mana, Divine Plea); **Multi** (Consecration at mana > 50, Hammer of Justice on the second); **Emergency** (Divine Protection/Shield → heal, LoH) | HoJ, LoH, FoL (`GenericPaladinStrategy.cpp:20-31`); Divine Protection only as a node (`GenericPaladinStrategyActionNodeFactory.h:151`) |
| **Druid** | **Form-switch** (caster pull with Wrath/Moonfire → Cat/Bear to kill → leave form to heal at < 50 → return); **Bear-multi** (Bear at `load > SingleMax`, Swipe, Demo Roar); **Root-kite** (Entangling Roots + Wrath outdoors); **Emergency** (Barkskin, Regrowth/Rejuv, Frenzied Regen) | roots (`GenericDruidStrategy.cpp:125,136`); healing only in resto (`RestoDruidStrategy.cpp:35-103`); **no in-combat form-switch-to-heal for feral/balance solo** |
| **Shaman** | **Shock-melee single** (Flame Shock via the gate, Earth Shock interrupt, LS up, Rockbiter/Flametongue); **Totem-multi** (Stoneclaw for aggro soak, Searing, Magma at 3+); **Heal-weave** (Healing Wave / Lesser HW at < 50 between shocks); **Emergency** (Earthbind + retreat, Nature's Swiftness HW) | Stoneclaw (`ElementalShamanStrategy.cpp:42`), Earthbind (`TotemsShamanStrategy.cpp:55`), Healing Wave only in resto (`RestoShamanStrategy.cpp:30-34`) |
| **Death Knight** (55+) | **Disease-single** (IT/PS via the gate, Obliterate/Scourge, Death Strike heal); **Multi** (Pestilence + Blood Boil / DnD); **Emergency** (IBF, AMS on casters, Death Pact with the ghoul) | stock has dedicated aoe strategies (`AiFactory.cpp:413-417`) |

A class gets its tactics only when its policy family and factor tables exist. Unknown classes fall back to `TacticId::None` (stock).

---

## 6. Phased build plan

All flags default to off. Each lane has a pure-header seam with unit tests, a runtime adapter, and a disjoint write set. The orchestrator serializes lanes that touch the shared registration files, which are marked with a star (*). The build lanes currently active also edit `PlayerbotAIConfig.*`, the ledger and `AiFactory`, so **rebase first**.

| Lane | Scope | Write set | Proof |
|---|---|---|---|
| **T0** (config, orchestrator; no build) | Staged `5.6` template (§3.4). Owner call on spec distribution. | live conf (restart window) | `.playerbots` talent dump on 3 priests at levels 15/25/45 matches the decoded list |
| **T1** (first build lane) | **Observe-only assessment + engagement ledger.** `TacticalPolicy.h` (snapshot, load, capacity, Choose + hysteresis, ArmOf hash), `PackRisk.h` (pure), `EngagementValue` adapter (no strategy, no behaviour), `engage` ledger event id 11, `combat` cv=2 `tac_ms`/`arm` | new: `AutoWow/TacticalPolicy.h`, `AutoWow/PackRisk.h`, `AutoWow/EngagementTracker.{h,cpp}`, `tests/TacticalPolicyTest.cpp`, `tests/PackRiskTest.cpp`, `tests/EngagementTrackerTest.cpp`, `AutoWow-repo scripts/engage-reduce.py`; *`AutoWowQuestLedger.{h,cpp}` (+1 enum, +1 formatter), *`PlayerbotAIConfig.{h,cpp}` (keys), *`CombatPerformanceTelemetry.cpp` (cv=2 fields) | unit tests: hysteresis, dwell, immediate emergency, load weights, pack reject, arm hash stability, line format; reducer `--selftest`; runtime: `engage` lines appear with sane `tac0` distribution; flag OFF ⇒ byte-identical `combat` cv=1 |
| **T2** | **Priest tactical strategy.** `"tactical"` strategy + `TacticMultiplier` + triggers (scream, lowest-hp target, pull origin, pre-pull readiness) + factor tables; AiFactory hook (flag + arm) | new: `Ai/Class/Priest/Strategy/TacticalPriestStrategy.{h,cpp}`, `Ai/Base/Strategy/TacticMultiplier.{h,cpp}`; `Ai/Class/Priest/PriestTriggers.{h,cpp}`, `PriestAiObjectContext.cpp`; *`AiFactory.cpp` (≤ 10 lines); `tests/TacticalPriestContractTest.cpp` | contract test: factor tables resolve to registered actions; runtime A/B per §4.2 (≥ 10 priests per arm, 2 h) → XP/h, starved, TTK, deaths |
| **T3** | **Pull risk in GrindTargetValue** | `Ai/Base/Value/GrindTargetValue.cpp` only (+ uses `PackRisk.h`) | unit tests on PackRisk; source-contract test that the strict branch still never admits non-objective entries; runtime `pull_risk` histogram, deaths/h |
| **T4** | **Escape + pull origin** (one attempt, leash detect) | new `Ai/Base/Actions/TacticalRetreatAction.{h,cpp}`; value `pull origin` | test: at most 1 escape per engagement; runtime escape success rate |
| **T5..T12** | One lane per class family, in the order **warlock → mage → hunter → warrior → paladin → druid → shaman → rogue → DK**. That order is by expected solo gain [H]: pet/CC classes first. | `Ai/Class/<Class>/Strategy/Tactical<Class>Strategy.*` + that class's triggers/context + factor tables in conf | per-class A/B cell |

**Deliberately skipped:** a generic "tactic language" or scripting layer. Tactics are C++ triggers plus config factor tables. Add a DSL only if a fourth class shows repetitive boilerplate that tables cannot express.

**Residual risks:**
- Psychic Scream on non-fearable or immune mobs: the adapter must check the creature's mechanic immunity before counting `SCREAM_READY` as capacity.
- A fear pulling adds is mitigated only by the `adds_near == 0` rule.
- `death_eta` from a 3 s HP slope is noisy at low levels. The shield absorb makes the slope look safe, so count the absorb.
- Re-casting `shoot` while the wand is already auto-repeating: confirm in T2 that `CastSpell("shoot")` does not restart the auto-repeat cycle. July captures show Shoot as the top healer spell, which suggests it works.
- The class spell-level bands above are [K] and must be checked against the trainer DB in T2.
