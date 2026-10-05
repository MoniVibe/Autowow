/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TacticalRuntime.h"

#include <algorithm>
#include <cctype>
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
#include "GearUpgradePolicy.h"
#include "Item.h"
#include "Map.h"
#include "Player.h"
#include "Playerbots.h"
#include "SharedDefines.h"
#include "Spell.h"
#include "SpellInfo.h"
#include "SpellMgr.h"
#include "TacticalClassTables.h"

static_assert(AutoWowTactics::kRankNormal == CREATURE_ELITE_NORMAL && AutoWowTactics::kRankElite == CREATURE_ELITE_ELITE &&
              AutoWowTactics::kRankRareElite == CREATURE_ELITE_RAREELITE &&
              AutoWowTactics::kRankBoss == CREATURE_ELITE_WORLDBOSS && AutoWowTactics::kRankRare == CREATURE_ELITE_RARE);
static_assert(CLASS_WARRIOR == 1 && CLASS_PALADIN == 2 && CLASS_HUNTER == 3 && CLASS_ROGUE == 4 && CLASS_PRIEST == 5 &&
              CLASS_DEATH_KNIGHT == 6 && CLASS_SHAMAN == 7 && CLASS_MAGE == 8 && CLASS_WARLOCK == 9 && CLASS_DRUID == 11);

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
    std::uint32_t pullMinManaPct = 50;  // T2 pre-pull readiness (0 = off)
    std::uint32_t pullMinHpPct = 60;
    std::uint32_t riskYd = 15;          // legacy grind: distance penalty per risk band
    std::uint32_t classMask = 1u << CLASS_PRIEST;  // AutoWow.Tactics.Classes (default "priest")
    std::uint32_t observeMask = 0;                 // AutoWow.Tactics.ObserveClasses (default ""): observe only
    PriestParams priest;
    FactorTable factors[5];             // TacticId 10..14
    ClassParams cls[kFamilies];         // non-priest families (AutoWow.Tactics.<Class>.*)
    FactorTable classFactors[kFamilies][4];  // .Factors.{Single,Multi,Emergency,Escape}
};

// Default action -> factor tables (AutoWow.Tactics.Priest.Factors.<Tactic>). Unlisted actions keep 1.
constexpr char kDefaultWandFactors[] =
    "smite:0,mind blast:0,holy fire:0,mana burn:0,mind flay:0,starshards:0,holy nova:0";
constexpr char kDefaultMultiFactors[] = "smite:0.5,mana burn:0";
constexpr char kDefaultEmergencyFactors[] =
    "smite:0,mind blast:0,holy fire:0,mana burn:0,mind flay:0,starshards:0,holy nova:0,shadow word: pain:0,"
    "shadow word: pain on attacker:0,devouring plague:0,vampiric touch:0,shadow word: death:0,mind sear:0";
constexpr char kDefaultEscapeFactors[] =
    "smite:0,mind blast:0,holy fire:0,mana burn:0,mind flay:0,starshards:0,holy nova:0,shadow word: pain:0,"
    "shadow word: pain on attacker:0,devouring plague:0,vampiric touch:0,shadow word: death:0,mind sear:0,shoot:0";
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

// In AutoWow.Tactics.Classes and the family has tactics (priest, or a kClassTables row).
bool ClassEnabled(std::uint32_t classId)
{
    Family const f = FamilyOfClass(classId);
    return classId < 32 && ((gSettings.classMask >> classId) & 1) &&
           (f == Family::Priest || !kClassTables[static_cast<std::uint32_t>(f)].key.empty());
}

// Tracked (engage rows): Classes with Observe/Enable on, or ObserveClasses; a family without tactics is skipped.
bool ClassTracked(std::uint32_t classId)
{
    Family const f = FamilyOfClass(classId);
    return Tracked(classId, gSettings.classMask, gSettings.observeMask, detail::gObserve || detail::gEnable) &&
           (f == Family::Priest || !kClassTables[static_cast<std::uint32_t>(f)].key.empty());
}

std::uint32_t SpellIdOf(PlayerbotAI* botAI, char const* name)
{
    return botAI->GetAiObjectContext()->GetValue<uint32>("spell id", name)->Get();
}

std::uint32_t SpellIdOf(PlayerbotAI* botAI, std::string_view name) { return SpellIdOf(botAI, std::string(name).c_str()); }

// Non-priest readiness: per tool list, known = any spell of the list learned, ready = any of them off
// cooldown. Plus kCdPet (combat pet alive) and kCdWand. ponytail: cooldown only - a shield-less warrior's
// Shield Wall counts as ready; add per-tool usability checks if the capacity bonus proves misleading.
std::uint32_t ClassReadiness(PlayerbotAI* botAI, Player* bot, Family f)
{
    ClassTable const& t = kClassTables[static_cast<std::uint32_t>(f)];
    std::uint32_t cds = 0;
    auto scan = [&](std::string_view const(&names)[3], std::uint32_t knownBit, std::uint32_t readyBit)
    {
        for (std::string_view const name : names)
        {
            if (name.empty())
                continue;
            std::uint32_t const id = SpellIdOf(botAI, name);
            if (!id || !bot->HasSpell(id))
                continue;
            cds |= knownBit;
            if (!bot->HasSpellCooldown(id))
                cds |= readyBit;
        }
    };
    scan(t.control, kCdControlKnown, kCdControl);
    scan(t.defensive, kCdDefensiveKnown, kCdDefensive);
    scan(t.escape, kCdEscapeKnown, kCdEscape);
    if (detail::gEscapeRelaxed)
        scan(t.escapeRelaxed, kCdEscapeKnown, kCdEscape);
    if (Pet* pet = bot->GetPet(); pet && pet->IsAlive())
        cds |= kCdPet;
    if (Item* ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED))
        if (ranged->GetTemplate()->Class == ITEM_CLASS_WEAPON && ranged->GetTemplate()->SubClass == ITEM_SUBCLASS_WEAPON_WAND)
            cds |= kCdWand;
    return cds;
}

// The first known control tool (immunity check of fearableMelee); nullptr = none known.
SpellInfo const* ControlSpell(PlayerbotAI* botAI, Player* bot, Family f)
{
    if (f == Family::Priest)
    {
        std::uint32_t const id = SpellIdOf(botAI, "psychic scream");
        return id ? sSpellMgr->GetSpellInfo(id) : nullptr;
    }
    for (std::string_view const name : kClassTables[static_cast<std::uint32_t>(f)].control)
        if (!name.empty())
            if (std::uint32_t const id = SpellIdOf(botAI, name); id && bot->HasSpell(id))
                return sSpellMgr->GetSpellInfo(id);
    return nullptr;
}

LoadParams const& LoadOf(Family f)
{
    return f == Family::Priest ? gSettings.priest.load : gSettings.cls[static_cast<std::uint32_t>(f)].load;
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

void AccumulateAssistEligibleLinkedAddImpl(EngagementSnapshot& snapshot, Creature* candidate,
                                           std::vector<Unit*> const& attackers, Player* bot,
                                           std::int32_t botLevel, LoadParams const& load,
                                           std::uint32_t linkRadiusYd)
{
    if (!candidate || !bot || !linkRadiusYd)
        return;

    for (Unit* attacker : attackers)
    {
        if (!attacker || candidate->GetDistance(attacker) > float(linkRadiusYd) ||
            !attacker->IsWithinLOSInMap(candidate) || !candidate->CanAssistTo(attacker, bot))
            continue;

        MobFacts const f = FactsOf(candidate);
        ++snapshot.addsNear;
        snapshot.load +=
            MobWeight(f.rank, static_cast<std::int32_t>(candidate->GetLevel()) - botLevel, f.caster, load);
        return;
    }
}

// The assessment value (plan 2.1). deathEtaMs is filled by the tracker from its own HP samples.
// fearableMelee counts melee attackers not immune to the family's control tool (priest: Psychic Scream).
EngagementSnapshot BuildSnapshot(PlayerbotAI* botAI, Player* bot, Family family)
{
    EngagementSnapshot s;
    AiObjectContext* context = botAI->GetAiObjectContext();
    LoadParams const& load = LoadOf(family);
    SpellInfo const* scream = ControlSpell(botAI, bot, family);
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
        s.load += MobWeight(f.rank, dl, f.caster, load);
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
            detail::AccumulateAssistEligibleLinkedAdd(s, unit->ToCreature(), attackers, bot, botLevel, load,
                                                      gSettings.linkRadiusYd);
        }
    }

    s.hpPct = Pct(bot->GetHealth(), bot->GetMaxHealth());
    // Mana users (druids in a form too); 100 for rage / energy / runic classes.
    s.manaPct = bot->GetMaxPower(POWER_MANA) ? Pct(bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA)) : 100;
    s.cds = family == Family::Priest ? ReadinessBits(botAI, bot) : ClassReadiness(botAI, bot, family);
    if (Unit* target = context->GetValue<Unit*>("current target")->Get())
        s.targetCaster = FactsOf(target).caster;
    return s;
}
}  // namespace

void detail::AccumulateAssistEligibleLinkedAdd(EngagementSnapshot& snapshot, Creature* candidate,
                                               std::vector<Unit*> const& attackers, Player* bot,
                                               std::int32_t botLevel, LoadParams const& load,
                                               std::uint32_t linkRadiusYd)
{
    AccumulateAssistEligibleLinkedAddImpl(snapshot, candidate, attackers, bot, botLevel, load, linkRadiusYd);
}

void LoadConfig()
{
    detail::gObserve = sConfigMgr->GetOption<bool>("AutoWow.Tactics.Observe", false);
    detail::gEnable = sConfigMgr->GetOption<bool>("AutoWow.Tactics.Enable", false);
    detail::gShadowLevelingSpec = sConfigMgr->GetOption<bool>("AutoWow.Tactics.PriestShadowLevelingSpec", false);
    detail::gEscapeRelaxed = sConfigMgr->GetOption<bool>("AutoWow.Tactics.EscapeRelaxed", false);

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
    s.pullMinManaPct = u("AutoWow.Tactics.PullMinManaPct", s.pullMinManaPct);
    s.pullMinHpPct = u("AutoWow.Tactics.PullMinHpPct", s.pullMinHpPct);
    s.riskYd = u("AutoWow.Tactics.RiskYd", s.riskYd);
    // AutoWow.Survival.PackAvoid (PackAvoidPolicy.h): every solo independent bot, outside the tactics arm.
    AutoWowPackAvoid::detail::gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Survival.PackAvoid", false);
    AutoWowPackAvoid::Params& pa = AutoWowPackAvoid::detail::gParams;
    pa.linkYards = u("AutoWow.Survival.PackAvoid.LinkYards", pa.linkYards);
    pa.capacityPct = u("AutoWow.Survival.PackAvoid.CapacityPct", pa.capacityPct);
    pa.minGearPct = std::min<std::uint32_t>(100, u("AutoWow.Survival.PackAvoid.MinGearPct", pa.minGearPct));
    auto f = [](char const* key, char const* def) { return ParseFactors(sConfigMgr->GetOption<std::string>(key, def)); };
    s.factors[0] = f("AutoWow.Tactics.Priest.Factors.Wand", kDefaultWandFactors);
    s.factors[1] = f("AutoWow.Tactics.Priest.Factors.Burst", "");
    s.factors[2] = f("AutoWow.Tactics.Priest.Factors.Multi", kDefaultMultiFactors);
    s.factors[3] = f("AutoWow.Tactics.Priest.Factors.Emergency", kDefaultEmergencyFactors);
    s.factors[4] = f("AutoWow.Tactics.Priest.Factors.Escape", kDefaultEscapeFactors);

    s.classMask = ParseClassMask(sConfigMgr->GetOption<std::string>("AutoWow.Tactics.Classes", "priest"));
    s.observeMask = ParseClassMask(sConfigMgr->GetOption<std::string>("AutoWow.Tactics.ObserveClasses", ""));
    detail::gObserveExtra = s.observeMask != 0;
    for (std::uint32_t k = 0; k < kFamilies; ++k)
    {
        ClassTable const& t = kClassTables[k];
        if (t.key.empty())
            continue;
        std::string const prefix = "AutoWow.Tactics." + std::string(t.key) + ".";
        auto cu = [&](char const* name, std::uint32_t def) { return u((prefix + name).c_str(), def); };
        ClassParams& c = s.cls[k];
        c.healHpPct = t.healHpPct;
        c.minDwellMs = p.minDwellMs;
        c.deathEtaMs = p.deathEtaMs;
        c.escapeRatioPct = p.escapeRatioPct;
        c.escapeMaxMs = p.escapeMaxMs;
        c.escapeRelaxed = detail::gEscapeRelaxed;
        c.relaxedEscapeHpPct = u("AutoWow.Tactics.EscapeRelaxed.HpPct", c.relaxedEscapeHpPct);
        c.load = p.load;
        c.singleMax = cu("SingleMaxPct", c.singleMax);
        c.base = cu("BasePct", c.base);
        c.healHpPct = cu("HealHpPct", c.healHpPct);
        c.lowManaPct = cu("LowManaPct", c.lowManaPct);
        c.emergencyHpPct = cu("EmergencyHpPct", c.emergencyHpPct);
        c.emergencyExitHpPct = cu("EmergencyExitHpPct", c.emergencyExitHpPct);
        c.controlMinMelee = cu("ControlMinMelee", c.controlMinMelee);
        c.controlHpPct = cu("ControlHpPct", c.controlHpPct);
        for (std::uint32_t i = 0; i < 4; ++i)
            s.classFactors[k][i] = ParseFactors(sConfigMgr->GetOption<std::string>(
                prefix + "Factors." + std::string(kClassSlotKeys[i]), std::string(t.factors[i])));
    }
    gSettings = s;
}

void Update(PlayerbotAI* botAI)
{
    if (!Tracking() || !botAI)
        return;
    Player* bot = botAI->GetBot();
    if (!bot || !ClassTracked(bot->getClass()) || !bot->IsInWorld())
        return;
    Family const family = FamilyOfClass(bot->getClass());

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
            // Observe-only classes (ObserveClasses, not in Classes) are never treated: arm 0.
            it->second.arm = ClassEnabled(bot->getClass()) ? ArmOf(botGuid, gSettings.armSalt, gSettings.armPct) : 0;
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
        in.snap = BuildSnapshot(botAI, bot, family);
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
        finished = family == Family::Priest
                       ? st.tracker.Tick(in, gSettings.priest, rec)
                       : st.tracker.Tick(in, family, gSettings.cls[static_cast<std::uint32_t>(family)], rec);
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
    if (!player || !spellInfo || !ClassTracked(player->getClass()))
        return;
    CastKind kind = CastKind::Other;
    if (player->getClass() == CLASS_PRIEST)
    {
        SpellInfo const* first = spellInfo->GetFirstRankSpell();
        std::uint32_t const firstId = first ? first->Id : spellInfo->Id;
        kind = firstId == kSpellPowerWordShieldR1 ? CastKind::Shield
               : firstId == kSpellPsychicScreamR1 ? CastKind::Control
                                                  : CastKind::Other;
    }
    else if (char const* raw = spellInfo->SpellName[LOCALE_enUS])
    {
        std::string name(raw);
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char ch) { return std::tolower(ch); });
        ClassTable const& t = kClassTables[static_cast<std::uint32_t>(FamilyOfClass(player->getClass()))];
        for (std::string_view const tool : t.control)
            if (!tool.empty() && name == tool)
                kind = CastKind::Control;
        for (std::string_view const tool : t.defensive)
            if (!tool.empty() && name == tool)
                kind = CastKind::Shield;
    }
    std::lock_guard<std::mutex> guard(gLock);
    if (auto it = gBots.find(static_cast<std::uint32_t>(player->GetGUID().GetCounter())); it != gBots.end())
        it->second.tracker.NoteCast(kind);
}

void Forget(std::uint32_t botGuid)
{
    std::lock_guard<std::mutex> guard(gLock);
    gBots.erase(botGuid);
}

// ---- T2 --------------------------------------------------------------------------------------------

namespace
{
bool IsPriest(Player* bot) { return bot && bot->getClass() == CLASS_PRIEST; }

bool IsTacticalClass(Player* bot) { return bot && ClassEnabled(bot->getClass()); }

bool TreatmentEligible(PlayerbotAI* botAI)
{
    Player* bot = botAI ? botAI->GetBot() : nullptr;
    return IsTreatment(bot) && Eligible(bot);
}

std::uint32_t ManaPct(Player* bot) { return Pct(bot->GetPower(POWER_MANA), bot->GetMaxPower(POWER_MANA)); }
}  // namespace

bool IsTreatment(Player* bot)
{
    return Enabled() && IsTacticalClass(bot) &&
           ArmOf(static_cast<std::uint32_t>(bot->GetGUID().GetCounter()), gSettings.armSalt, gSettings.armPct) == 1;
}

TacticId Current(Player* bot, EngagementSnapshot* snap)
{
    if (!Enabled() || !IsTacticalClass(bot))
        return TacticId::None;
    std::lock_guard<std::mutex> guard(gLock);
    auto const it = gBots.find(static_cast<std::uint32_t>(bot->GetGUID().GetCounter()));
    if (it == gBots.end() || it->second.arm != 1)
        return TacticId::None;
    if (snap)
        *snap = it->second.tracker.Last();
    return it->second.tracker.Current();
}

PriestParams const& Priest() { return gSettings.priest; }

ClassParams const& Params(Family family)
{
    std::uint32_t const k = static_cast<std::uint32_t>(family);
    return gSettings.cls[k < kFamilies ? k : 0];
}

std::uint32_t FactorPermille(TacticId id, std::string const& action)
{
    std::uint32_t const k = static_cast<std::uint32_t>(id);
    std::uint32_t const first = static_cast<std::uint32_t>(TacticId::PriestWand);
    if (k >= first && k <= static_cast<std::uint32_t>(TacticId::PriestEscape))
        return FactorOf(gSettings.factors[k - first], action);
    std::uint32_t const family = static_cast<std::uint32_t>(FamilyOf(id));
    std::uint32_t const index = FactorIndexOfSlot(SlotOf(id));
    if (id == TacticId::None || family >= kFamilies || index >= 4)
        return 1000;
    return FactorOf(gSettings.classFactors[family][index], action);
}

// Priest-only (the "tactical nc" rest triggers exist for priests); other classes rest via
// AutoWow.Survival.RestGate.
bool HoldProactivePull(PlayerbotAI* botAI)
{
    if (!Enabled() || !botAI || !IsPriest(botAI->GetBot()) || !TreatmentEligible(botAI))
        return false;
    Player* bot = botAI->GetBot();
    return AutoWowPackRisk::HoldPull(Pct(bot->GetHealth(), bot->GetMaxHealth()), ManaPct(bot),
                                     bot->getPowerType() == POWER_MANA, gSettings.pullMinHpPct,
                                     gSettings.pullMinManaPct);
}

bool NeedsRestMana(PlayerbotAI* botAI)
{
    if (!Enabled() || !gSettings.pullMinManaPct || !botAI || !IsPriest(botAI->GetBot()) || !TreatmentEligible(botAI))
        return false;
    Player* bot = botAI->GetBot();
    return !bot->IsInCombat() && bot->getPowerType() == POWER_MANA && ManaPct(bot) < gSettings.pullMinManaPct;
}

bool NeedsRestHealth(PlayerbotAI* botAI)
{
    if (!Enabled() || !gSettings.pullMinHpPct || !botAI || !IsPriest(botAI->GetBot()) || !TreatmentEligible(botAI))
        return false;
    Player* bot = botAI->GetBot();
    return !bot->IsInCombat() && Pct(bot->GetHealth(), bot->GetMaxHealth()) < gSettings.pullMinHpPct;
}

bool PullRiskActive(PlayerbotAI* botAI) { return Enabled() && gSettings.linkRadiusYd && TreatmentEligible(botAI); }

std::uint32_t PullCapacity(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    EngagementSnapshot s;
    s.hpPct = Pct(bot->GetHealth(), bot->GetMaxHealth());
    if (IsPriest(bot))
    {
        s.manaPct = ManaPct(bot);
        s.cds = ReadinessBits(botAI, bot);
        return Capacity(s, gSettings.priest);
    }
    Family const family = FamilyOfClass(bot->getClass());
    s.manaPct = bot->GetMaxPower(POWER_MANA) ? ManaPct(bot) : 100;
    s.cds = ClassReadiness(botAI, bot, family);
    return Capacity(s, Params(family));
}

// ponytail: O(candidates x pool) per grind selection (tens of units); index by grid cell if it ever shows.
AutoWowPackRisk::Verdict ScorePull(PlayerbotAI* botAI, Unit* candidate, std::vector<ObjectGuid> const& pool,
                                   std::uint32_t capacity)
{
    Player* bot = botAI->GetBot();
    std::int32_t const botLevel = static_cast<std::int32_t>(bot->GetLevel());
    Family const family = FamilyOfClass(bot->getClass());
    LoadParams const& load = LoadOf(family);
    std::uint32_t const escapeRatioPct =
        family == Family::Priest ? gSettings.priest.escapeRatioPct : Params(family).escapeRatioPct;
    MobFacts const cf = FactsOf(candidate);
    std::uint32_t const candidateWeight = MobWeight(cf.rank, static_cast<std::int32_t>(candidate->GetLevel()) - botLevel,
                                                    cf.caster, load);
    std::uint32_t neighbours = 0;
    for (ObjectGuid const guid : pool)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || unit == candidate || !unit->IsAlive() || unit->IsInCombat() || !unit->IsCreature() ||
            unit->GetCreatureType() == CREATURE_TYPE_CRITTER ||
            unit->GetDistance(candidate) > float(gSettings.linkRadiusYd))
            continue;
        MobFacts const f = FactsOf(unit);
        neighbours += MobWeight(f.rank, static_cast<std::int32_t>(unit->GetLevel()) - botLevel, f.caster, load);
    }
    return AutoWowPackRisk::Score(candidateWeight, neighbours, capacity, escapeRatioPct);
}

std::uint32_t RiskYd() { return gSettings.riskYd; }

// AutoWow.Survival.PackAvoid: the class capacity above scaled by the weapon's DPS against the level curve.
std::uint32_t PackAvoidCapacity(PlayerbotAI* botAI)
{
    Player* bot = botAI->GetBot();
    std::uint8_t const slot = bot->getClass() == CLASS_HUNTER ? EQUIPMENT_SLOT_RANGED : EQUIPMENT_SLOT_MAINHAND;
    std::uint32_t dpsMilli = 0;
    if (Item* weapon = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        if (ItemTemplate const* proto = weapon->GetTemplate(); proto && proto->Class == ITEM_CLASS_WEAPON)
            dpsMilli = AutoWowGear::DpsMilli(static_cast<std::uint32_t>(std::max(0.0f, proto->Damage[0].DamageMin)),
                                             static_cast<std::uint32_t>(std::max(0.0f, proto->Damage[0].DamageMax)),
                                             proto->Delay);
    AutoWowPackAvoid::Params const& p = AutoWowPackAvoid::Get();
    return AutoWowPackAvoid::Capacity(
        p, PullCapacity(botAI),
        AutoWowPackAvoid::GearPct(p, bot->getClass(), dpsMilli, AutoWowGear::ExpectedDpsMilli(bot->GetLevel())));
}

// As ScorePull, but neighbours are social links of the candidate (friendly to it, idle, alive, not
// critters) within PackAvoid LinkYards, and the verdict is AutoWowPackAvoid::Score (no escape ratio).
AutoWowPackRisk::Verdict ScorePackAvoid(PlayerbotAI* botAI, Unit* candidate, std::vector<ObjectGuid> const& pool,
                                        std::uint32_t capacity)
{
    Player* bot = botAI->GetBot();
    std::int32_t const botLevel = static_cast<std::int32_t>(bot->GetLevel());
    LoadParams const& load = LoadOf(FamilyOfClass(bot->getClass()));
    float const link = float(AutoWowPackAvoid::Get().linkYards);
    MobFacts const cf = FactsOf(candidate);
    std::uint32_t const candidateWeight = MobWeight(cf.rank, static_cast<std::int32_t>(candidate->GetLevel()) - botLevel,
                                                    cf.caster, load);
    std::uint32_t neighbours = 0;
    for (ObjectGuid const guid : pool)
    {
        Unit* unit = botAI->GetUnit(guid);
        if (!unit || unit == candidate || !unit->IsAlive() || unit->IsInCombat() || !unit->IsCreature() ||
            unit->GetCreatureType() == CREATURE_TYPE_CRITTER || unit->GetDistance(candidate) > link ||
            !candidate->IsFriendlyTo(unit))
            continue;
        MobFacts const f = FactsOf(unit);
        neighbours += MobWeight(f.rank, static_cast<std::int32_t>(unit->GetLevel()) - botLevel, f.caster, load);
    }
    return AutoWowPackAvoid::Score(candidateWeight, neighbours, capacity);
}

void NotePullChoice(Player* bot, Unit* target, std::uint32_t band)
{
    if (!bot || !target)
        return;
    std::lock_guard<std::mutex> guard(gLock);
    if (auto it = gBots.find(static_cast<std::uint32_t>(bot->GetGUID().GetCounter())); it != gBots.end())
    {
        it->second.pullGuid = target->GetGUID().GetRawValue();
        it->second.pullBand = static_cast<std::int32_t>(band);
    }
}
}  // namespace AutoWowTactics
