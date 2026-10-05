/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_TACTICAL_RUNTIME_H
#define AUTOWOW_TACTICAL_RUNTIME_H

#include <cstdint>
#include <string>
#include <vector>

#include "PackAvoidPolicy.h"
#include "PackRisk.h"
#include "TacticalPolicy.h"

class Creature;
class ObjectGuid;
class Player;
class PlayerbotAI;
class SpellInfo;
class Unit;

// Runtime adapter of the tactical combat layer (docs/TACTICAL_COMBAT_PLAN.md). All default off:
//   AutoWow.Tactics.Observe (T1): assess every solo priest engagement, label the tactic it WOULD run, emit
//     one ledger `engage` line per engagement and the `combat` cv=2 tac_ms/arm fields. No behaviour change.
//   AutoWow.Tactics.Enable (T2): as Observe, plus the treatment arm (hash(guid) % 100 < ArmPct) gets the
//     "tactical" / "tactical nc" priest strategies, the pre-pull readiness gate and pack-risk pull choice.
//     Control-arm bots are tracked and labelled in the same run (same-soak A/B).
//   AutoWow.Tactics.PriestShadowLevelingSpec: non-random priests level with premade spec 5.6.
//   AutoWow.Tactics.EscapeRelaxed: non-priest escape at hp < EscapeRelaxed.HpPct and falling with >= 2
//     attackers and an escape tool ready (TacticalPolicy.h DesiredClass); the escapeRelaxed tool rows of
//     TacticalClassTables.h count as escape tools; druid escape adds Cat Form before Dash, druid emergency
//     Survival Instincts.
//   AutoWow.Tactics.Classes (default "priest"): the classes the layer tracks and treats ("all" or a list:
//     priest, warrior, rogue, mage, shaman, paladin, hunter, druid, warlock, deathknight); a class without a
//     TacticalClassTables.h row is skipped. The arm hash applies within every class.
// Eligible = a listed class, not grouped, not in a dungeon/raid/battleground/arena. Per-bot state is keyed by guid
// counter under one mutex (bots update on map threads); never held while calling other subsystems.
namespace AutoWowTactics
{
namespace detail
{
inline bool gObserve = false;
inline bool gEnable = false;  // T2 master; tracking also runs under it
inline bool gShadowLevelingSpec = false;
inline bool gEscapeRelaxed = false;
inline bool gObserveExtra = false;  // AutoWow.Tactics.ObserveClasses non-empty (TacticalPolicy.h Tracked)

// Add one already-filtered idle creature to the snapshot when it can lawfully assist at least one real attacker.
// Production and the native-object regression share this adapter so assistance, LOS, distance and load stay one path.
void AccumulateAssistEligibleLinkedAdd(EngagementSnapshot& snapshot, Creature* candidate,
                                       std::vector<Unit*> const& attackers, Player* bot, std::int32_t botLevel,
                                       LoadParams const& load, std::uint32_t linkRadiusYd);
}  // namespace detail

inline bool Tracking() { return detail::gObserve || detail::gEnable || detail::gObserveExtra; }
inline bool Enabled() { return detail::gEnable; }
inline bool ShadowLevelingSpec() { return detail::gShadowLevelingSpec; }
inline bool EscapeRelaxed() { return detail::gEscapeRelaxed; }
inline constexpr std::uint32_t kShadowLevelingSpecNo = 6;  // AiPlayerbot.PremadeSpec*.5.6

// Reads AutoWow.Tactics.*. Called once at world init.
void LoadConfig();

// ---- T1: observation (bot AI tick / hook side) ---------------------------------------------------
// Bot AI tick. No-op unless Tracking(); re-evaluates every ReevalMs and on every combat-state change.
void Update(PlayerbotAI* botAI);
// A creature died to the bot's (or its pet's) killing blow.
void NoteKill(std::uint32_t botGuid);
// A non-triggered spell cast by a player (filtered to tracked priests inside).
void NoteCast(Player* player, SpellInfo const* spellInfo);
void Forget(std::uint32_t botGuid);

// ---- T2: treatment (AutoWow.Tactics.Enable) -------------------------------------------------------
// Enable on, listed class, treatment arm. Fixed per guid, so safe to consult when an engine is built.
bool IsTreatment(Player* bot);
// The running tactic of an eligible treatment bot (None otherwise) and the snapshot it was chosen on.
TacticId Current(Player* bot, EngagementSnapshot* snap = nullptr);
PriestParams const& Priest();
// Non-priest family parameters (AutoWow.Tactics.<Class>.*).
ClassParams const& Params(Family family);
// Permille multiplier of an action under a tactic (1000 = unchanged); AutoWow.Tactics.<Class>.Factors.*.
std::uint32_t FactorPermille(TacticId id, std::string const& action);
// Pre-pull readiness gate (priests): true = do not start a proactive pull now (rest first). Self-defence
// unaffected. Other classes rest through AutoWow.Survival.RestGate.
bool HoldProactivePull(PlayerbotAI* botAI);
// Out-of-combat rest triggers of "tactical nc" (eligible treatment bot, below the pull thresholds).
bool NeedsRestMana(PlayerbotAI* botAI);
bool NeedsRestHealth(PlayerbotAI* botAI);

// Pack-risk pull selection. PullRiskActive: treatment + eligible + LinkRadius > 0. Capacity is taken once
// per selection; ScorePull scores one candidate against the idle hostiles of `pool` near it.
bool PullRiskActive(PlayerbotAI* botAI);
std::uint32_t PullCapacity(PlayerbotAI* botAI);
AutoWowPackRisk::Verdict ScorePull(PlayerbotAI* botAI, Unit* candidate, std::vector<ObjectGuid> const& pool,
                                   std::uint32_t capacity);
std::uint32_t RiskYd();
// AutoWow.Survival.PackAvoid (PackAvoidPolicy.h), any class: capacity (class tools x hp/mana x gear) and the
// social-link pack score of one candidate. Usable without AutoWow.Tactics.* (the params load regardless).
std::uint32_t PackAvoidCapacity(PlayerbotAI* botAI);
AutoWowPackRisk::Verdict ScorePackAvoid(PlayerbotAI* botAI, Unit* candidate, std::vector<ObjectGuid> const& pool,
                                        std::uint32_t capacity);
// The proactive pull the selector chose; its band is stamped on the engagement it starts (pull_risk).
void NotePullChoice(Player* bot, Unit* target, std::uint32_t band);
}  // namespace AutoWowTactics

#endif  // AUTOWOW_TACTICAL_RUNTIME_H
