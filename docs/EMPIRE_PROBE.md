# Empire / reputation probe (2026-09-27)

Owner vibe ("Warcraft redefined"): 10 race empires, each run by its own overlord. Standing exists per character and collectively per race. When a race's standing with a city collapses, the city refuses it services and its guards turn hostile. Alliance and Horde remain as natural blocs.

## Capital faction ids (verified against Faction.dbc)

- **Alliance** (parent faction 469): Stormwind 72, Ironforge 47, Darnassus 69, Gnomeregan Exiles 54, Exodar 930.
- **Horde** (parent faction 67): Orgrimmar 76, Undercity 68, Thunder Bluff 81, Darkspear Trolls 530, Silvermoon City 911.

## What the stock engine does

| Mechanism | Stock behaviour | Evidence |
|---|---|---|
| Hostility from rep in your own team's capitals (own race or another race) | **No.** At Hated the guard counts the player as hostile, but the attack check fails because the player still sees the guard as friendly (faction-template masks) | Unit.cpp:7218-7260, 10826-10846. Live: scout 112 at Hated Stormwind walked the Trade District for 2.5 min with no combat |
| Hostility in an enemy capital | Yes | base rep -42000, at-war, hostile masks |
| Service refusal (vendor, repair, trainer, flight) | **Yes, at Unfriendly or worse**, and the bots' own actions go through the same check | Player.cpp:2101 `GetNPCIfCanInteractWith` |
| At-war toggle for same-team capitals | Blocked by FACTION_FLAG_PEACE_FORCED (0x10) | ReputationMgr.cpp:520, 432 |
| Spillover between sister capitals | Yes, 25%, from `reputation_spillover_template`; losses spill too, only through SetReputation | ReputationMgr.cpp:301-357 |
| Forced hostility | `ReputationMgr::ApplyForceReaction(faction, REP_HOSTILE, true)` is checked first in both directions, so guards attack on sight. In memory only, so it must be re-applied at login | Unit.cpp:7112-7123 |

Verdict: about 70% stock, with no core changes needed.

## Minimal empire design

1. **Ours:** empire state (one standing per race per capital, persisted, with decay and thresholds), kept by a module WorldScript and owned by 10 overlords.
2. **Output through stock reputation:** write each bot's rep with a capital from the empire standing (SetReputation, spillover off). Service refusal and loss of discounts then come free at Unfriendly.
3. **Collapse tier:** `ApplyForceReaction(capital, REP_HOSTILE, true)`, applied at login and whenever the tier changes, so guards attack on sight.
4. **Fallback** for anything the forced reaction cannot express: the `UnitScript::IfNormalReaction` hook. Never swap a bot's faction template.
5. **Deeds:** killing a city's people (only possible in designated PvP zones and under the forced reaction), raiding, trade and aid adjust the standing, routed through SetReputation so the 25% spillover to sister cities comes free.

## Risks

- A forced reaction also makes that city's vendors hostile; they are civilians, so this is mostly harmless.
- Guard call-for-help and patrol AI under a forced reaction have not been tested live yet.
