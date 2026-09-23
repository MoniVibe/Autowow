/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TacticalRuntime.h"

#include <algorithm>
#include <mutex>
#include <string>
#include <unordered_map>

#include "AiFactory.h"
#include "AutoWowQuestLedger.h"
#include "CombatPerformanceTelemetry.h"
#include "Config.h"
#include "Creature.h"
#include "EngagementTracker.h"
#include "GameTime.h"
#include "Item.h"
#include "Map.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"

static_assert(AutoWowTactics::kRankNormal == CREATURE_ELITE_NORMAL && AutoWowTactics::kRankElite == CREATURE_ELITE_ELITE &&
              AutoWowTactics::kRankRareElite == CREATURE_ELITE_RAREELITE &&
              AutoWowTactics::kRankBoss == CREATURE_ELITE_WORLDBOSS && AutoWowTactics::kRankRare == CREATURE_ELITE_RARE);

namespace AutoWowTactics
{
namespace
{
constexpr std::uint32_t kSpellPowerWordShieldR1 = 17;
constexpr std::uint32_t kSpellPsychicScreamR1 = 8122;
constexpr std::uint32_t kSpellWeakenedSoul = 6788;
constexpr std::size_t kMaxBots = 2048;  // hard cap; beyond it new bots are not tracked

struct Settings
{
    std::uint32_t reevalMs = 500;
    std::uint32_t armPct = 50;
    std::uint32_t armSalt = 1;
    std::uint32_t linkRadiusYd = 12;
    PriestParams priest;
};
Settings gSettings;

struct BotState
{
    EngagementTracker tracker;
    std::uint64_t nextDueMs = 0;
    bool lastCombat = false;
    std::uint8_t arm = 0;
    std::uint64_t pullGuid = 0;
    std::int32_t pullBand = -1;
};

// ponytail: one global lock (a few map lookups per bot per ReevalMs); shard by guid if it ever shows.
std::mutex gLock;
std::unordered_map<std::uint32_t, BotState> gBots;

std::uint64_t NowMs()
{
    auto const now = GameTime::GetGameTimeMS().count();
    return now > 0 ? static_cast<std::uint64_t>(now) : 0;
}

std::uint32_t Pct(std::uint64_t cur, std::uint64_t max) { return max ? static_cast<std::uint32_t>(cur * 100 / max) : 0; }

bool Eligible(Player* bot)
{
    Map* map = bot->GetMap();
    return !bot->GetGroup() && map && !map->IsDungeon() && !map->IsBattlegroundOrArena();
}

std::uint32_t SpellIdOf(PlayerbotAI* botAI, char const* name)
{
    return botAI->GetAiObjectContext()->GetValue<uint32>("spell id", name)->Get();
}

std::uint32_t ReadinessBits(PlayerbotAI* botAI, Player* bot)
{
    std::uint32_t cds = 0;
    if (std::uint32_t const scream = SpellIdOf(botAI, "psychic scream"); scream && bot->HasSpell(scream))
    {
        cds |= kCdScreamKnown;
        if (!bot->HasSpellCooldown(scream))
            cds |= kCdScream;
    }
    if (std::uint32_t const shield = SpellIdOf(botAI, "power word: shield"); shield && bot->HasSpell(shield))
    {
        cds |= kCdShieldKnown;
        if (!bot->HasAura(kSpellWeakenedSoul))
            cds |= kCdShield;
    }
    if (Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
        if (ranged->GetTemplate()->Class == ITEM_CLASS_WEAPON && ranged->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_WAND)
            cds |= kCdWand;
    return cds;
}

struct MobFacts
{
    std::uint32_t rank = kRankNormal;
    bool caster = false;
};

MobFacts FactsOf(Unit* unit)
{
    MobFacts f;
    if (Creature* creature = unit->ToCreature())
        if (CreatureTemplate const* t = creature->GetCreatureTemplate())
        {
            f.rank = t->rank;
            f.caster = t->unit_class == CLASS_MAGE;
        }
    return f;
}

// The assessment value (plan 2.1). deathEtaMs is filled by the tracker from its own HP samples.
EngagementSnapshot BuildSnapshot(PlayerbotAI* botAI, Player* bot, PriestParams const& p)
{
    EngagementSnapshot s;
    AiObjectContext* context = botAI->GetAiObjectContext();
    std::uint32_t const screamId = SpellIdOf(botAI, "psychic scream");
    SpellInfo const* scream = screamId ? sSpellMgr->GetSpellInfo(screamId) : nullptr;
    std::int32_t const botLevel = static_cast<std::int32_t>(bot->GetLevel());

    GuidVector const attackerGuids = context->GetValue<GuidVector>("attackers")->Get();
    std::vector<Unit*> attackers;
    bool firstLevel = true;
    for (ObjectGuid const guid : attackerGuids)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || !unit->IsAlive())
            continue;
        attackers.push_back(unit);
        MobFacts const f = FactsOf(unit);
        std::int32_t const dl = static_cast<std::int32_t>(unit->GetLevel()) - botLevel;
        ++s.attackers;
        s.load += MobWeight(f.rank, dl, f.caster, p.load);
        s.lvlDmax = firstLevel || dl > s.lvlDmax ? dl : s.lvlDmax;
        firstLevel = false;
        if (f.rank == kRankElite || f.rank == kRankRareElite || f.rank == kRankBoss)
            ++s.eliteN;
        else if (f.rank == kRankRare)
            ++s.rareN;
        if (f.caster)
            ++s.casters;
        if (unit->IsPlayer())
            s.pvp = true;
        if (unit->GetVictim() == bot && unit->IsWithinMeleeRange(bot))
        {
            ++s.melee;
            if (scream && !unit->IsImmunedToSpell(scream, bot))
                ++s.fearableMelee;
        }
        if (unit->HasAuraType(SPELL_AURA_MOD_FEAR))
            ++s.feared;
    }

    // Idle hostiles within LinkRadius of any attacker: social-aggro adds a fear could run into.
    if (gSettings.linkRadiusYd && !attackers.empty())
    {
        GuidVector const pool = context->GetValue<GuidVector>("possible targets")->Get();
        for (ObjectGuid const guid : pool)
        {
            Unit* unit = botAI->GetUnit(guid);
            if (!unit || !unit->IsAlive() || unit->IsInCombat() || !unit->IsCreature() ||
                unit->GetCreatureType() == CREATURE_TYPE_CRITTER ||
                std::find(attackers.begin(), attackers.end(), unit) != attackers.end())
                continue;
            bool linked = false;
            for (Unit* attacker : attackers)
                if (unit->GetDistance(attacker) <= float(gSettings.linkRadiusYd))
                {
                    linked = true;
                    break;
                }
            if (!linked)
                continue;
            MobFacts const f = FactsOf(unit);
            ++s.addsNear;
            s.load += MobWeight(f.rank, static_cast<std::int32_t>(unit->GetLevel()) - botLevel, f.caster, p.load);
        }
    }

    s.hpPct = Pct(bot->GetHealth(), bot->GetMaxHealth());
    s.manaPct = bot->getPowerType() == POWER_MANA ? Pct(bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA)) : 100;
    s.cds = ReadinessBits(botAI, bot);
    if (Unit* target = context->GetValue<Unit*>("current target")->Get())
        s.targetCaster = FactsOf(target).caster;
    return s;
}
}  // namespace

void LoadConfig()
{
    detail::gObserve = sConfigMgr->GetOption<bool>("AutoWow.Tactics.Observe", false);

    Settings s;
    s.reevalMs = std::max<std::uint32_t>(100, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Tactics.ReevalMs", 500));
    s.armPct = std::min<std::uint32_t>(100, sConfigMgr->GetOption<std::uint32_t>("AutoWow.Tactics.ArmPct", 50));
    s.armSalt = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Tactics.ArmSalt", 1);
    s.linkRadiusYd = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Tactics.LinkRadius", 12);
    PriestParams& p = s.priest;
    auto u = [](char const* key, std::uint32_t def) { return sConfigMgr->GetOption<std::uint32_t>(key, def); };
    p.minDwellMs = u("AutoWow.Tactics.MinDwellMs", p.minDwellMs);
    p.deathEtaMs = u("AutoWow.Tactics.DeathEtaMs", p.deathEtaMs);
    p.escapeRatioPct = u("AutoWow.Tactics.EscapeRatioPct", p.escapeRatioPct);
    p.escapeMaxMs = u("AutoWow.Tactics.EscapeMaxMs", p.escapeMaxMs);
    p.load.eliteMulPct = u("AutoWow.Tactics.Load.EliteMulPct", p.load.eliteMulPct);
    p.load.rareMulPct = u("AutoWow.Tactics.Load.RareMulPct", p.load.rareMulPct);
    p.load.lvlStepPct = u("AutoWow.Tactics.Load.LvlStepPct", p.load.lvlStepPct);
    p.load.casterMulPct = u("AutoWow.Tactics.Load.CasterMulPct", p.load.casterMulPct);
    p.singleMax = u("AutoWow.Tactics.Priest.SingleMaxPct", p.singleMax);
    p.base = u("AutoWow.Tactics.Priest.BasePct", p.base);
    p.wandManaPct = u("AutoWow.Tactics.Priest.WandManaPct", p.wandManaPct);
    p.burstExitManaPct = u("AutoWow.Tactics.Priest.BurstExitManaPct", p.burstExitManaPct);
    p.shieldMinManaPct = u("AutoWow.Tactics.Priest.ShieldMinManaPct", p.shieldMinManaPct);
    p.renewHpPct = u("AutoWow.Tactics.Priest.RenewHpPct", p.renewHpPct);
    p.healHpPct = u("AutoWow.Tactics.Priest.HealHpPct", p.healHpPct);
    p.emergencyHpPct = u("AutoWow.Tactics.Priest.EmergencyHpPct", p.emergencyHpPct);
    p.emergencyExitHpPct = u("AutoWow.Tactics.Priest.EmergencyExitHpPct", p.emergencyExitHpPct);
    p.screamMinMelee = u("AutoWow.Tactics.Priest.ScreamMinMelee", p.screamMinMelee);
    p.screamHpPct = u("AutoWow.Tactics.Priest.ScreamHpPct", p.screamHpPct);
    gSettings = s;
}

void Update(PlayerbotAI* botAI)
{
    if (!Tracking() || !botAI)
        return;
    Player* bot = botAI->GetBot();
    if (!bot || bot->getClass() != CLASS_PRIEST || !bot->IsInWorld())
        return;

    std::uint32_t const botGuid = static_cast<std::uint32_t>(bot->GetGUID().GetCounter());
    std::uint64_t const nowMs = NowMs();
    bool const eligible = Eligible(bot);
    bool const inCombat = eligible && bot->IsInCombat();
    {
        std::lock_guard<std::mutex> guard(gLock);
        auto it = gBots.find(botGuid);
        if (it == gBots.end())
        {
            if (gBots.size() >= kMaxBots)
                return;
            it = gBots.emplace(botGuid, BotState{}).first;
            it->second.arm = ArmOf(botGuid, gSettings.armSalt, gSettings.armPct);
        }
        BotState& st = it->second;
        if (nowMs < st.nextDueMs && inCombat == st.lastCombat)
            return;
        st.nextDueMs = nowMs + gSettings.reevalMs;
        st.lastCombat = inCombat;
    }

    // World reads outside the lock (bot map thread).
    TickInput in;
    in.nowMs = nowMs;
    in.inCombat = inCombat;
    in.dead = !bot->IsAlive();
    in.resting = bot->IsSitState();
    in.autoRepeat = bot->GetCurrentSpell(CURRENT_AUTOREPEAT_SPELL) != nullptr;
    in.hpPct = Pct(bot->GetHealth(), bot->GetMaxHealth());
    std::int32_t const absorb = bot->GetTotalAuraModifier(SPELL_AURA_SCHOOL_ABSORB);
    in.hp = bot->GetHealth() + std::uint64_t(absorb > 0 ? absorb : 0);
    in.mana = bot->GetPower(POWER_MANA);
    in.manaPct = Pct(in.mana, bot->GetMaxPower(POWER_MANA));
    if (inCombat)
        in.snap = BuildSnapshot(botAI, bot, gSettings.priest);
    Unit* const target = inCombat ? botAI->GetAiObjectContext()->GetValue<Unit*>("current target")->Get() : nullptr;
    std::uint64_t const targetGuid = target ? target->GetGUID().GetRawValue() : 0;

    EngageRecord rec;
    bool finished = false;
    TacticId creditId = TacticId::None;
    std::uint32_t creditMs = 0;
    std::uint8_t arm = 0;
    {
        std::lock_guard<std::mutex> guard(gLock);
        auto it = gBots.find(botGuid);
        if (it == gBots.end())
            return;
        BotState& st = it->second;
        in.pullRisk = targetGuid && targetGuid == st.pullGuid ? st.pullBand : -1;
        finished = st.tracker.Tick(in, gSettings.priest, rec);
        creditId = st.tracker.CreditId();
        creditMs = st.tracker.CreditMs();
        arm = st.arm;
        if (finished)
        {
            st.pullGuid = 0;
            st.pullBand = -1;
        }
    }

    if (creditId != TacticId::None)
        AutoWowCombatPerformanceTelemetry::RecordTactic(botGuid, static_cast<std::uint8_t>(creditId), creditMs, arm);
    if (finished)
        AutoWowQuestLedger::EmitEngage(bot, FormatEngageFields(rec, bot->getClass(), AiFactory::GetPlayerSpecTab(bot), arm));
}

void NoteKill(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> guard(gLock);
    if (auto it = gBots.find(botGuid); it != gBots.end())
        it->second.tracker.NoteKill();
}

void NoteCast(Player* player, SpellInfo const* spellInfo)
{
    if (!player || !spellInfo || player->getClass() != CLASS_PRIEST)
        return;
    SpellInfo const* first = spellInfo->GetFirstRankSpell();
    std::uint32_t const firstId = first ? first->Id : spellInfo->Id;
    CastKind const kind = firstId == kSpellPowerWordShieldR1 ? CastKind::Shield
                          : firstId == kSpellPsychicScreamR1 ? CastKind::Control
                                                             : CastKind::Other;
    std::lock_guard<std::mutex> guard(gLock);
    if (auto it = gBots.find(static_cast<std::uint32_t>(player->GetGUID().GetCounter())); it != gBots.end())
        it->second.tracker.NoteCast(kind);
}

void Forget(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> guard(gLock);
    gBots.erase(botGuid);
}
}  // namespace AutoWowTactics
