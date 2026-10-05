/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "LabControl.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "Chat.h"
#include "Config.h"
#include "Creature.h"
#include "CreatureAI.h"
#include "GameTime.h"
#include "Item.h"
#include "LabPolicy.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Opcodes.h"
#include "Pet.h"
#include "Player.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "ScriptMgr.h"
#include "SoloSpecPolicy.h"
#include "Spell.h"
#include "SpellAuras.h"
#include "SpellInfo.h"
#include "TemporarySummon.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace AutoWowLab
{
namespace
{
bool gEnabled = false;
bool gTrace = false;
std::uint32_t gAccount = 0;
std::uint32_t gLevel = 75;
std::uint32_t gQuality = 2;
std::string gKitIlvl;
Marker gMarker{1, 16226.2f, 16257.0f, 13.2022f, 0.0f};
float gRadius = 60.0f;
float gSpawnRadius = 18.0f;
std::uint32_t gLowHpPct = 35;
std::uint32_t gDespawnMs = 15 * 60 * 1000;
std::string gTracePath;

// ponytail: one lab at a time (one marker, one global run); per-marker runs if a second lab is ever wanted.
struct PlayerAcc
{
    std::uint32_t ms = 0;
    std::uint64_t dealt = 0;
    std::uint64_t taken = 0;
};

std::mutex gMutex;  // map threads (damage, casts, updates) and the world thread (commands) share everything below
std::FILE* gFile = nullptr;
std::string gRun;  // "" = no run open: nothing is traced
std::uint32_t gRunSeq = 0;
std::uint32_t gWave = 0;
std::vector<ObjectGuid> gMobs;  // live lab summons of the open run, spawn order
std::unordered_map<std::uint32_t, PlayerAcc> gAcc;

thread_local std::uint32_t tPendingLevel = 0;  // set only around SummonCreature in HandleSpawn

std::uint64_t NowMs() { return static_cast<std::uint64_t>(GameTime::GetGameTimeMS().count()); }

bool InLab(WorldObject const* obj)
{
    return obj && obj->GetMapId() == gMarker.map &&
           InRadius(obj->GetPositionX() - gMarker.x, obj->GetPositionY() - gMarker.y, gRadius);
}

char const* Ctl(Player const* player)
{
    return GET_PLAYERBOT_AI(const_cast<Player*>(player)) ? "bot" : "human";
}

// Caller holds gMutex.
void WriteLine(std::string const& line)
{
    if (!gFile)
    {
        gFile = std::fopen(gTracePath.c_str(), "a");
        if (!gFile)
        {
            LOG_ERROR("playerbots", "[Lab] cannot open trace file {}", gTracePath);
            return;
        }
    }
    std::fputs(line.c_str(), gFile);
    std::fputc('\n', gFile);
    std::fflush(gFile);
}

// Caller holds gMutex and has checked gRun is open.
std::string Head(char const* ev)
{
    return std::string("{\"ev\":\"") + ev + "\",\"t\":" + std::to_string(NowMs()) + ",\"run\":\"" + gRun + "\"";
}

std::string Who(Player const* player)
{
    return ",\"who\":" + std::to_string(player->GetGUID().GetCounter()) + ",\"name\":\"" + player->GetName() +
           "\",\"cls\":" + std::to_string(player->getClass()) + ",\"ctl\":\"" + Ctl(player) + "\"";
}

// The lab player a unit's action belongs to (the player itself, or the owner of its pet), or nullptr.
Player* LabOwner(Unit const* unit, bool& viaPet)
{
    viaPet = false;
    if (!unit)
        return nullptr;
    Player* owner = unit->GetCharmerOrOwnerPlayerOrPlayerItself();
    if (!owner || !InLab(owner))
        return nullptr;
    viaPet = owner != unit;
    return owner;
}

bool IsLabMob(ObjectGuid guid)
{
    for (ObjectGuid const& g : gMobs)
        if (g == guid)
            return true;
    return false;
}

// Caller holds gMutex.
void CloseRun(char const* reason)
{
    if (gRun.empty())
        return;
    if (gTrace)
        WriteLine(Head("end") + ",\"reason\":\"" + reason + "\"}");
    gRun.clear();
    gWave = 0;
    gAcc.clear();
}

void EmitCast(Spell* spell, Unit* caster, SpellInfo const* info, char const* result)
{
    if (!gTrace || !caster || !info || (spell && spell->IsTriggered()))
        return;
    bool viaPet = false;
    Player* owner = LabOwner(caster, viaPet);
    if (!owner)
        return;
    std::uint32_t const target = spell ? spell->m_targets.GetUnitTargetGUID().GetCounter() : 0;
    SpellInfo const* first = info->GetFirstRankSpell();
    std::lock_guard<std::mutex> lock(gMutex);
    if (gRun.empty())
        return;
    WriteLine(Head("cast") + Who(owner) + ",\"pet\":" + (viaPet ? "1" : "0") + ",\"spell\":" +
              std::to_string(info->Id) + ",\"base\":" + std::to_string(first ? first->Id : info->Id) +
              ",\"tgt\":" + std::to_string(target) + ",\"res\":\"" + result + "\",\"gcd\":" +
              std::to_string(info->StartRecoveryTime) + ",\"cd\":" +
              std::to_string(std::max(info->RecoveryTime, info->CategoryRecoveryTime)) + ",\"ct\":" +
              std::to_string(spell ? std::max(spell->GetCastTime(), 0) : 0) + "}");
}

class LabSpellScript : public AllSpellScript
{
public:
    LabSpellScript()
        : AllSpellScript("AutoWowLabSpell", {ALLSPELLHOOK_ON_CAST, ALLSPELLHOOK_ON_PREPARE, ALLSPELLHOOK_ON_CAST_CANCEL})
    {
    }

    void OnSpellCast(Spell* spell, Unit* caster, SpellInfo const* info, bool /*skipCheck*/) override
    {
        EmitCast(spell, caster, info, "ok");
    }

    // prepare() fires after an instant has already cast; only cast-time spells get a separate "start" row.
    void OnSpellPrepare(Spell* spell, Unit* caster, SpellInfo const* info) override
    {
        if (spell && spell->GetCastTime() > 0)
            EmitCast(spell, caster, info, "start");
    }

    void OnSpellCastCancel(Spell* spell, Unit* caster, SpellInfo const* info, bool /*bySelf*/) override
    {
        EmitCast(spell, caster, info, "cancel");
    }
};

// Failed attempts: SMSG_CAST_FAILED reaches every session (bots included) through this hook.
class LabPacketScript : public PlayerbotScript
{
public:
    LabPacketScript() : PlayerbotScript("AutoWowLabPacket") {}

    void OnPlayerbotPacketSent(Player* player, WorldPacket const* packet) override
    {
        if (!gTrace || !packet || packet->GetOpcode() != SMSG_CAST_FAILED || packet->size() < 6 || !InLab(player))
            return;
        std::uint32_t const spellId = packet->read<std::uint32_t>(1);
        std::uint32_t const result = packet->read<std::uint8_t>(5);
        std::lock_guard<std::mutex> lock(gMutex);
        if (gRun.empty())
            return;
        WriteLine(Head("cast") + Who(player) + ",\"pet\":0,\"spell\":" + std::to_string(spellId) +
                  ",\"base\":" + std::to_string(spellId) + ",\"tgt\":" +
                  std::to_string(player->GetTarget().GetCounter()) + ",\"res\":\"fail\",\"err\":" +
                  std::to_string(result) + "}");
    }
};

class LabUnitScript : public UnitScript
{
public:
    LabUnitScript()
        : UnitScript("AutoWowLabUnit", true,
                     std::vector<uint16>{UNITHOOK_ON_DAMAGE, UNITHOOK_ON_BEFORE_ROLL_MELEE_OUTCOME_AGAINST,
                                         UNITHOOK_ON_UNIT_DEATH})
    {
    }

    void OnDamage(Unit* attacker, Unit* victim, uint32& damage) override
    {
        if (!gTrace || !damage)
            return;
        bool viaPet = false;
        Player* dealer = LabOwner(attacker, viaPet);
        Player* hurt = victim ? victim->ToPlayer() : nullptr;
        if (hurt && !InLab(hurt))
            hurt = nullptr;
        if (!dealer && !hurt)
            return;
        std::lock_guard<std::mutex> lock(gMutex);
        if (gRun.empty())
            return;
        if (dealer && dealer != victim)
            gAcc[dealer->GetGUID().GetCounter()].dealt += damage;
        if (hurt)
        {
            gAcc[hurt->GetGUID().GetCounter()].taken += damage;
            if (CrossesBelowPct(hurt->GetHealth(), damage, hurt->GetMaxHealth(), gLowHpPct))
                WriteLine(Head("lowhp") + Who(hurt) + ",\"pct\":" + std::to_string(gLowHpPct) + "}");
        }
    }

    // One call per white-swing outcome roll (auto attack only; abilities roll elsewhere).
    void OnBeforeRollMeleeOutcomeAgainst(Unit const* attacker, Unit const* victim, WeaponAttackType attType,
                                         int32& /*attackerMaxSkillValueForLevel*/,
                                         int32& /*victimMaxSkillValueForLevel*/, int32& /*attackerWeaponSkill*/,
                                         int32& /*victimDefenseSkill*/, int32& /*crit_chance*/,
                                         int32& /*miss_chance*/, int32& /*dodge_chance*/, int32& /*parry_chance*/,
                                         int32& /*block_chance*/) override
    {
        if (!gTrace)
            return;
        bool viaPet = false;
        Player* owner = LabOwner(attacker, viaPet);
        if (!owner)
            return;
        std::lock_guard<std::mutex> lock(gMutex);
        if (gRun.empty())
            return;
        WriteLine(Head("swing") + Who(owner) + ",\"pet\":" + (viaPet ? "1" : "0") + ",\"tgt\":" +
                  std::to_string(victim ? victim->GetGUID().GetCounter() : 0) + ",\"att\":" +
                  std::to_string(static_cast<int>(attType)) + "}");
    }

    // Run bookkeeping (not only tracing): a run closes when its last lab mob or the lab player dies.
    void OnUnitDeath(Unit* unit, Unit* /*killer*/) override
    {
        if (!gEnabled || !unit)
            return;
        std::lock_guard<std::mutex> lock(gMutex);
        if (gRun.empty())
            return;
        if (Player* player = unit->ToPlayer())
        {
            if (InLab(player))
            {
                if (gTrace)
                    WriteLine(Head("death") + Who(player) + "}");
                CloseRun("death");
            }
            return;
        }
        if (!IsLabMob(unit->GetGUID()))
            return;
        if (gTrace)
            WriteLine(Head("mobdeath") + ",\"mob\":" + std::to_string(unit->GetGUID().GetCounter()) + "}");
        gMobs.erase(std::remove(gMobs.begin(), gMobs.end(), unit->GetGUID()), gMobs.end());
        if (gMobs.empty())
            CloseRun("clear");
    }
};

class LabPlayerScript : public PlayerScript
{
public:
    LabPlayerScript() : PlayerScript("AutoWowLabPlayer", {PLAYERHOOK_ON_UPDATE}) {}

    // Per-second sample: resources, engaged lab mobs, damage dealt/taken in the last second.
    void OnPlayerUpdate(Player* player, uint32 diff) override
    {
        if (!gTrace || !InLab(player))
            return;
        std::lock_guard<std::mutex> lock(gMutex);
        if (gRun.empty())
            return;
        PlayerAcc& acc = gAcc[player->GetGUID().GetCounter()];
        acc.ms += diff;
        if (acc.ms < 1000)
            return;
        acc.ms = std::min<std::uint32_t>(acc.ms - 1000, 1000);
        std::uint32_t engaged = 0;
        for (ObjectGuid const& g : gMobs)
            if (Creature* mob = ObjectAccessor::GetCreature(*player, g))
                if (mob->IsAlive() && mob->IsInCombat())
                    ++engaged;
        Powers const power = player->getPowerType();
        WriteLine(Head("sec") + Who(player) + ",\"hp\":" + std::to_string(player->GetHealth()) + ",\"hpmax\":" +
                  std::to_string(player->GetMaxHealth()) + ",\"ptype\":" + std::to_string(static_cast<int>(power)) +
                  ",\"pw\":" + std::to_string(player->GetPower(power)) + ",\"pwmax\":" +
                  std::to_string(player->GetMaxPower(power)) + ",\"mobs\":" + std::to_string(engaged) +
                  ",\"casting\":" + (player->IsNonMeleeSpellCast(false) ? "1" : "0") + ",\"tgt\":" +
                  std::to_string(player->GetTarget().GetCounter()) + ",\"dd\":" + std::to_string(acc.dealt) +
                  ",\"dt\":" + std::to_string(acc.taken) + "}");
        acc.dealt = 0;
        acc.taken = 0;
    }
};

// Pins the level of a lab summon (SelectLevel would roll minlevel..maxlevel) so every run meets identical mobs.
class LabCreatureScript : public AllCreatureScript
{
public:
    LabCreatureScript() : AllCreatureScript("AutoWowLabCreature") {}

    void OnBeforeCreatureSelectLevel(CreatureTemplate const* /*cinfo*/, Creature* /*creature*/, uint8& level) override
    {
        if (tPendingLevel)
            level = static_cast<uint8>(tPendingLevel);
    }
};

bool Refuse(ChatHandler* handler, char const* cmd, char const* reason)
{
    handler->PSendSysMessage("lab {}: refused reason={}", cmd, reason);
    return true;
}

Player* Invoker(ChatHandler* handler)
{
    return handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr;
}
}  // namespace

void LoadConfig()
{
    gEnabled = sConfigMgr->GetOption<bool>("AutoWow.Lab.Enable", false);
    gTrace = sConfigMgr->GetOption<bool>("AutoWow.Lab.Trace", false);
    gAccount = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Lab.Account", 0);
    gLevel = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Lab.Level", 75);
    gQuality = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Lab.Quality", 2);
    gKitIlvl = sConfigMgr->GetOption<std::string>("AutoWow.Lab.KitIlvl", "");
    std::string const marker =
        sConfigMgr->GetOption<std::string>("AutoWow.Lab.Marker", "1 16226.2 16257.0 13.2022 0");
    if (!ParseMarker(marker, gMarker))
        LOG_ERROR("playerbots", "[Lab] AutoWow.Lab.Marker '{}' is not 'map x y z [o]'; keeping the default", marker);
    gRadius = sConfigMgr->GetOption<float>("AutoWow.Lab.Radius", 60.0f);
    gSpawnRadius = sConfigMgr->GetOption<float>("AutoWow.Lab.SpawnRadius", 18.0f);
    gLowHpPct = sConfigMgr->GetOption<std::uint32_t>("AutoWow.Lab.LowHpPct", 35);
    std::string dir = sConfigMgr->GetOption<std::string>("LogsDir", "");
    if (!dir.empty() && dir.back() != '/')
        dir += '/';
    gTracePath = dir + sConfigMgr->GetOption<std::string>("AutoWow.Lab.TraceFile", "lab-trace.jsonl");
}

void AddScripts()
{
    new LabSpellScript();
    new LabPacketScript();
    new LabUnitScript();
    new LabPlayerScript();
    new LabCreatureScript();
}

// .autowow lab kit [ilvl]: level, solo spec (AutoWowSoloSpec row), spells and an item-level-capped loadout of
// AutoWow.Lab.Quality for the invoking Lab<Class> character on AutoWow.Lab.Account. Same path as bot gearing
// (PlayerbotFactory::AutoGear); re-running re-gears at the same level.
bool HandleKit(ChatHandler* handler, char const* args)
{
    Player* player = Invoker(handler);
    if (!gEnabled)
        return Refuse(handler, "kit", "lab_disabled");
    if (!player)
        return Refuse(handler, "kit", "in_game_only");
    if (!gAccount || player->GetSession()->GetAccountId() != gAccount)
        return Refuse(handler, "kit", "not_lab_account");
    if (!IsLabName(player->GetName()))
        return Refuse(handler, "kit", "not_lab_character");
    if (player->IsInCombat())
        return Refuse(handler, "kit", "in_combat");
    if (player->GetLevel() > gLevel)
        return Refuse(handler, "kit", "above_lab_level");
    std::uint8_t const cls = player->getClass();
    std::int32_t const spec = AutoWowSoloSpec::RowOf(cls).specNo;
    if (spec < 0 || cls >= MAX_CLASSES || sPlayerbotAIConfig.premadeSpecName[cls][spec].empty())
        return Refuse(handler, "kit", "no_solo_spec");
    std::uint32_t ilvl = KitIlvlFor(gKitIlvl, cls);
    if (args && *args)
        ilvl = static_cast<std::uint32_t>(std::strtoul(args, nullptr, 10));

    PlayerbotFactory factory(player, gLevel, gQuality);
    if (player->GetLevel() < gLevel)
    {
        player->GiveLevel(gLevel);
        player->SetUInt32Value(PLAYER_XP, 0);
        player->InitStatsForLevel(true);
    }
    player->LearnDefaultSkills();
    factory.InitSkills();
    factory.InitClassSpells();
    factory.InitAvailableSpells(true);
    PlayerbotFactory::InitTalentsBySpecNo(player, spec, true);
    factory.InitGlyphs(false);
    PlayerbotFactory::DestroyEquippedGear(player);
    PlayerbotFactory::AutoGear(player, gQuality, ilvl, false, false, true);
    factory.InitBags(false);
    factory.InitReagents();
    if (cls == CLASS_HUNTER)
    {
        factory.InitPet();
        factory.InitPetTalents();
    }
    player->SetFullHealth();
    player->SaveToDB(false, false);

    std::uint32_t slots = 0, ilvlSum = 0;
    for (std::uint8_t slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
            continue;
        if (Item* item = player->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
        {
            ++slots;
            ilvlSum += item->GetTemplate()->ItemLevel;
        }
    }
    handler->PSendSysMessage("lab kit: ok name={} level={} spec={} quality={} ilvl_cap={} slots={} avg_ilvl={}",
                             player->GetName(), player->GetLevel(), sPlayerbotAIConfig.premadeSpecName[cls][spec],
                             gQuality, ilvl, slots, slots ? ilvlSum / slots : 0);
    LOG_INFO("playerbots", "[Lab] kit name={} level={} spec={} ilvl_cap={} slots={} avg_ilvl={}", player->GetName(),
             player->GetLevel(), spec, ilvl, slots, slots ? ilvlSum / slots : 0);
    return true;
}

// .autowow lab spawn <entry> <count> [level]: count summons on fixed ring slots around the marker, level pinned,
// each attacking the invoker at once (the fight starts on spawn for human and bot alike). With lab mobs still alive
// the wave is an add to the open run; otherwise a new run starts.
bool HandleSpawn(ChatHandler* handler, char const* args)
{
    Player* player = Invoker(handler);
    if (!gEnabled)
        return Refuse(handler, "spawn", "lab_disabled");
    if (!player)
        return Refuse(handler, "spawn", "in_game_only");
    SpawnArgs a;
    if (!ParseSpawnArgs(args ? args : "", gLevel, a))
    {
        handler->SendSysMessage("Usage: .autowow lab spawn <entry> <count 1-8> [level]");
        return Refuse(handler, "spawn", "usage");
    }
    if (!sObjectMgr->GetCreatureTemplate(a.entry))
        return Refuse(handler, "spawn", "unknown_entry");
    if (!InLab(player))
        return Refuse(handler, "spawn", "outside_lab_use_reset");
    if (!player->IsAlive())
        return Refuse(handler, "spawn", "dead");
    {
        // A run that ended by death leaves its mobs standing; they would leak into the next run.
        std::lock_guard<std::mutex> lock(gMutex);
        if (gRun.empty() && !gMobs.empty())
            return Refuse(handler, "spawn", "reset_first");
    }

    std::vector<ObjectGuid> spawned;
    for (std::uint32_t i = 0; i < a.count; ++i)
    {
        auto const [dx, dy] = RingOffset(i, a.count, gSpawnRadius, gMarker.o);
        float const x = gMarker.x + dx, y = gMarker.y + dy;
        float z = gMarker.z;
        player->UpdateGroundPositionZ(x, y, z);
        tPendingLevel = a.level;
        TempSummon* mob = player->SummonCreature(a.entry, x, y, z, std::atan2(-dy, -dx), TEMPSUMMON_TIMED_DESPAWN,
                                                 gDespawnMs);
        tPendingLevel = 0;
        if (!mob)
            continue;
        spawned.push_back(mob->GetGUID());
        if (mob->AI())
            mob->AI()->AttackStart(player);
    }
    if (spawned.empty())
        return Refuse(handler, "spawn", "summon_failed");

    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gRun.empty())
        {
            gRun = std::to_string(static_cast<long long>(std::time(nullptr))) + "-" + std::to_string(++gRunSeq);
            if (gTrace)
                WriteLine(Head("run") + Who(player) + ",\"level\":" + std::to_string(player->GetLevel()) + "}");
        }
        ++gWave;
        if (gTrace)
            WriteLine(Head("wave") + ",\"wave\":" + std::to_string(gWave) + ",\"part\":\"" + ScenarioPart(a) + "\"}");
        for (std::size_t i = 0; i < spawned.size(); ++i)
        {
            gMobs.push_back(spawned[i]);
            if (gTrace)
                WriteLine(Head("spawn") + ",\"wave\":" + std::to_string(gWave) + ",\"mob\":" +
                          std::to_string(spawned[i].GetCounter()) + ",\"entry\":" + std::to_string(a.entry) +
                          ",\"lvl\":" + std::to_string(a.level) + ",\"slot\":" + std::to_string(i) + "}");
        }
    }
    handler->PSendSysMessage("lab spawn: ok entry={} count={} level={} ctl={}", a.entry, spawned.size(), a.level,
                             Ctl(player));
    return true;
}

// .autowow lab reset: despawn every lab summon, close the run, put the invoker back on the marker alive, full
// health, mana/energy full, rage/runic power empty, no harmful auras and no spell cooldowns (pet healed too).
bool HandleReset(ChatHandler* handler, char const* /*args*/)
{
    Player* player = Invoker(handler);
    if (!gEnabled)
        return Refuse(handler, "reset", "lab_disabled");
    if (!player)
        return Refuse(handler, "reset", "in_game_only");

    std::vector<ObjectGuid> mobs;
    {
        std::lock_guard<std::mutex> lock(gMutex);
        if (gTrace && !gRun.empty())
            WriteLine(Head("reset") + Who(player) + "}");
        CloseRun("reset");
        mobs.swap(gMobs);
    }
    std::uint32_t despawned = 0;
    for (ObjectGuid const& g : mobs)
        if (Creature* mob = ObjectAccessor::GetCreature(*player, g))
        {
            mob->DespawnOrUnsummon();
            ++despawned;
        }

    if (!player->IsAlive())
        player->ResurrectPlayer(1.0f, false);
    player->CombatStop(true);
    Unit::AuraApplicationMap& auras = player->GetAppliedAuras();
    for (Unit::AuraApplicationMap::iterator it = auras.begin(); it != auras.end();)
    {
        if (!it->second->IsPositive() && !it->second->GetBase()->IsPassive())
            player->RemoveAura(it);
        else
            ++it;
    }
    player->RemoveAllSpellCooldown();
    player->SetFullHealth();
    Powers const power = player->getPowerType();
    player->SetPower(power, power == POWER_RAGE || power == POWER_RUNIC_POWER ? 0 : player->GetMaxPower(power));
    if (Pet* pet = player->GetPet())
        pet->SetFullHealth();
    if (player->GetMapId() == gMarker.map)
        player->NearTeleportTo(gMarker.x, gMarker.y, gMarker.z, gMarker.o, false, false, true);
    else
        player->TeleportTo(gMarker.map, gMarker.x, gMarker.y, gMarker.z, gMarker.o);
    handler->PSendSysMessage("lab reset: ok despawned={} ctl={}", despawned, Ctl(player));
    return true;
}
}  // namespace AutoWowLab
