/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#include "FixtureFactoryControl.h"

#include <array>
#include <cctype>
#include <cstdlib>
#include <initializer_list>
#include <limits>
#include <sstream>
#include <string>

#include "Config.h"
#include "Bag.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "PlayerbotFactory.h"
#include "Playerbots.h"
#include "RandomPlayerbotMgr.h"
#include "SharedDefines.h"
#include "World.h"

namespace
{
constexpr uint32 kQualityCount = AutoWowFixture::kMaxQuality - AutoWowFixture::kMinQuality + 1;
std::array<char const*, kQualityCount> const kQualityNames = {"poor", "normal", "uncommon", "rare", "epic", "legendary"};

std::string JsonString(std::string const& value)
{
    std::ostringstream out;
    out << '"';
    for (unsigned char character : value)
    {
        switch (character)
        {
            case '"': out << "\\\""; break;
            case '\\': out << "\\\\"; break;
            case '\b': out << "\\b"; break;
            case '\f': out << "\\f"; break;
            case '\n': out << "\\n"; break;
            case '\r': out << "\\r"; break;
            case '\t': out << "\\t"; break;
            default:
                if (character < 0x20)
                {
                    static char const hex[] = "0123456789abcdef";
                    out << "\\u00" << hex[(character >> 4) & 0x0f] << hex[character & 0x0f];
                }
                else
                    out << static_cast<char>(character);
        }
    }
    out << '"';
    return out.str();
}

std::string Error(std::string const& code)
{
    return "{\"ok\":false,\"error\":" + JsonString(code) + "}";
}

bool ParseAllowlistToken(std::string const& token, uint32 guid)
{
    if (token.empty() || !guid)
        return false;

    for (unsigned char character : token)
        if (!std::isdigit(character))
            return false;

    char* end = nullptr;
    unsigned long long parsed = std::strtoull(token.c_str(), &end, 10);
    if (!end || *end != '\0' || parsed > std::numeric_limits<uint32>::max())
        return false;

    return static_cast<uint32>(parsed) == guid;
}

std::string Normalize(std::string value)
{
    for (char& character : value)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));

    std::size_t const first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos)
        return {};

    std::size_t const last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::string ClassName(uint8 cls)
{
    switch (cls)
    {
        case CLASS_WARRIOR: return "warrior";
        case CLASS_PALADIN: return "paladin";
        case CLASS_HUNTER: return "hunter";
        case CLASS_ROGUE: return "rogue";
        case CLASS_PRIEST: return "priest";
        case CLASS_DEATH_KNIGHT: return "death_knight";
        case CLASS_SHAMAN: return "shaman";
        case CLASS_MAGE: return "mage";
        case CLASS_WARLOCK: return "warlock";
        case CLASS_DRUID: return "druid";
        default: return "unknown";
    }
}

std::string CombatRole(Player* bot)
{
    bool const tank = PlayerbotAI::IsTank(bot);
    bool const healer = PlayerbotAI::IsHeal(bot);
    bool const dps = PlayerbotAI::IsDps(bot);

    std::string role;
    if (tank)
        role = "tank";
    if (healer)
        role += role.empty() ? "healer" : "+healer";
    if (dps)
        role += role.empty() ? "dps" : "+dps";
    return role.empty() ? "none" : role;
}

std::string DiscoverLeagueRole(Player* bot)
{
    uint32 const guid = bot->GetGUID().GetCounter();
    for (char const* key : {"leagueRole", "league_role", "role"})
    {
        std::string role = Normalize(sRandomPlayerbotMgr.GetData(guid, key));
        if (!role.empty())
            return role;
    }

    // This checkout has no typed league-role API. Unknown is explicit so evidence cannot claim
    // that the SQL-only league role was observed when it was not exposed to this module surface.
    return "unknown";
}

bool IsForbiddenLeagueRole(std::string const& role)
{
    return role == "racer" || role == "captain" || role.find("racer") != std::string::npos ||
           role.find("captain") != std::string::npos;
}

struct FixtureTarget
{
    Player* player = nullptr;
    PlayerbotAI* ai = nullptr;
    std::string leagueRole = "unknown";
};

bool ResolveFixtureTarget(uint32 guid, FixtureTarget& target, std::string& error)
{
    std::string const configured = sConfigMgr->GetOption<std::string>(AutoWowFixture::kFixtureGuidsConfigKey, "");
    if (!AutoWowFixture::IsFixtureGuidAllowlisted(configured, guid))
    {
        error = "fixture_guid_not_allowlisted";
        return false;
    }

    Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
    if (!bot)
    {
        error = "fixture_target_not_online";
        return false;
    }

    PlayerbotAI* botAI = PlayerbotsMgr::instance().GetPlayerbotAI(bot);
    // A human observer has no PlayerbotAI; a session-backed real player can still have one, so
    // both checks are required before the fixture path is allowed to continue.
    if (!botAI || botAI->IsRealPlayer())
    {
        error = "fixture_target_not_playerbot";
        return false;
    }

    std::string const leagueRole = DiscoverLeagueRole(bot);
    if (IsForbiddenLeagueRole(leagueRole))
    {
        error = "fixture_forbidden_league_role";
        return false;
    }

    target.player = bot;
    target.ai = botAI;
    target.leagueRole = leagueRole;
    return true;
}

std::string SpecName(uint8 cls, uint32 specIndex)
{
    if (!AutoWowFixture::IsValidSpecIndex(specIndex) || cls >= MAX_CLASSES)
        return "unknown";

    std::string const& configured = sPlayerbotAIConfig.premadeSpecName[cls][specIndex];
    return configured.empty() ? "unconfigured" : configured;
}

struct SpecEvidence
{
    int32 storedIndex = -1;
    std::string name = "unknown";
};

SpecEvidence ReadSpecEvidence(Player* bot)
{
    uint32 const storedSpec = sRandomPlayerbotMgr.GetValue(bot->GetGUID().GetCounter(), "specNo");
    if (!storedSpec || storedSpec > AutoWowFixture::kMaxSpecIndexExclusive)
        return {};

    uint32 const storedIndex = storedSpec - 1;
    return {static_cast<int32>(storedIndex), SpecName(bot->getClass(), storedIndex)};
}

struct GearEvidence
{
    uint32 checkedSlots = 0;
    uint32 occupiedSlots = 0;
    std::array<uint32, kQualityCount> qualityCounts{};
    uint32 otherQualitySlots = 0;
};

struct InventoryEvidence
{
    uint32 backpackCapacity = INVENTORY_SLOT_ITEM_END - INVENTORY_SLOT_ITEM_START;
    uint32 equippedBagCount = 0;
    uint32 bagCapacity = 0;
    uint32 totalCapacity = backpackCapacity;
    uint32 freeSlots = 0;
    uint32 carriedItemCount = 0;
};

InventoryEvidence ReadInventoryEvidence(Player* bot)
{
    InventoryEvidence evidence;
    for (uint8 slot = INVENTORY_SLOT_ITEM_START; slot < INVENTORY_SLOT_ITEM_END; ++slot)
    {
        if (bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot))
            ++evidence.carriedItemCount;
        else
            ++evidence.freeSlots;
    }

    for (uint8 bagSlot = INVENTORY_SLOT_BAG_START; bagSlot < INVENTORY_SLOT_BAG_END; ++bagSlot)
    {
        Bag* bag = bot->GetBagByPos(bagSlot);
        if (!bag)
            continue;

        ++evidence.equippedBagCount;
        evidence.bagCapacity += bag->GetBagSize();
        evidence.totalCapacity += bag->GetBagSize();
        evidence.freeSlots += bag->GetFreeSlots();
        evidence.carriedItemCount += bag->GetBagSize() - bag->GetFreeSlots();
    }

    return evidence;
}

GearEvidence ReadGearEvidence(Player* bot)
{
    GearEvidence evidence;
    for (uint8 slot = EQUIPMENT_SLOT_START; slot < EQUIPMENT_SLOT_END; ++slot)
    {
        if (slot == EQUIPMENT_SLOT_BODY || slot == EQUIPMENT_SLOT_TABARD)
            continue;

        ++evidence.checkedSlots;
        Item* item = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
        if (!item)
            continue;

        ++evidence.occupiedSlots;
        ItemTemplate const* itemTemplate = item->GetTemplate();
        if (!itemTemplate || itemTemplate->Quality < AutoWowFixture::kMinQuality ||
            itemTemplate->Quality > AutoWowFixture::kMaxQuality)
        {
            ++evidence.otherQualitySlots;
            continue;
        }

        ++evidence.qualityCounts[itemTemplate->Quality - AutoWowFixture::kMinQuality];
    }
    return evidence;
}

char const* TeamName(TeamId team)
{
    switch (team)
    {
        case TEAM_ALLIANCE: return "Alliance";
        case TEAM_HORDE: return "Horde";
        default: return "Neutral";
    }
}

bool HasAnySpell(PlayerbotAI* ai, std::initializer_list<char const*> names)
{
    if (!ai)
        return false;

    for (char const* name : names)
        if (ai->HasSpell(name))
            return true;

    return false;
}

struct RoleReadinessEvidence
{
    std::string role = "none";
    bool roleCriticalSpell = false;
    bool mainHand = false;
    bool offHand = false;
    bool ranged = false;
    bool weaponReady = false;
};

RoleReadinessEvidence ReadRoleReadinessEvidence(Player* bot, PlayerbotAI* ai)
{
    RoleReadinessEvidence evidence;
    evidence.role = CombatRole(bot);
    evidence.mainHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND) != nullptr;
    evidence.offHand = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND) != nullptr;
    evidence.ranged = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED) != nullptr;
    evidence.weaponReady = evidence.mainHand || evidence.ranged;

    if (PlayerbotAI::IsTank(bot))
    {
        evidence.roleCriticalSpell = HasAnySpell(
            ai, {"taunt", "growl", "righteous defense", "hand of reckoning", "dark command"});
    }
    else if (PlayerbotAI::IsHeal(bot))
    {
        evidence.roleCriticalSpell = HasAnySpell(
            ai, {"heal", "flash heal", "greater heal", "healing wave", "chain heal", "nourish", "regrowth"});
    }
    else if (PlayerbotAI::IsDps(bot))
    {
        evidence.roleCriticalSpell = HasAnySpell(
            ai, {"heroic strike", "sinister strike", "backstab", "mortal strike", "crusader strike",
                 "steady shot", "arcane shot", "fireball", "shadow bolt", "lightning bolt", "wrath"});
    }

    return evidence;
}

std::string Evidence(Player* bot, PlayerbotAI* ai, std::string const& operation, std::string const& leagueRole,
                     int32 requestedSpecIndex, int32 requestedQuality, bool mutated)
{
    SpecEvidence const spec = ReadSpecEvidence(bot);
    GearEvidence const gear = ReadGearEvidence(bot);
    InventoryEvidence const inventory = ReadInventoryEvidence(bot);
    RoleReadinessEvidence const readiness = ReadRoleReadinessEvidence(bot, ai);

    std::ostringstream out;
    out << "{\"ok\":true,\"operation\":" << JsonString(operation)
        << ",\"guid\":" << bot->GetGUID().GetCounter()
        << ",\"identity\":{\"name\":" << JsonString(bot->GetName())
        << ",\"account_id\":" << bot->GetSession()->GetAccountId()
        << ",\"race_id\":" << static_cast<uint32>(bot->getRace())
        << ",\"faction\":" << JsonString(TeamName(bot->GetTeamId())) << "}"
        << ",\"class\":{\"id\":" << static_cast<uint32>(bot->getClass())
        << ",\"name\":" << JsonString(ClassName(bot->getClass())) << "}"
        << ",\"level\":" << static_cast<uint32>(bot->GetLevel())
        << ",\"role\":{\"combat\":" << JsonString(CombatRole(bot))
        << ",\"league\":" << JsonString(leagueRole) << "}"
        << ",\"spec\":{\"requested_index\":" << requestedSpecIndex
        << ",\"stored_index\":" << spec.storedIndex
        << ",\"name\":" << JsonString(spec.name) << "}"
        << ",\"gear\":{\"requested_quality\":" << requestedQuality
        << ",\"equipped_slots\":" << gear.occupiedSlots
        << ",\"equipment_slots_checked\":" << gear.checkedSlots
        << ",\"quality_counts\":{";

    for (uint32 quality = AutoWowFixture::kMinQuality; quality <= AutoWowFixture::kMaxQuality; ++quality)
    {
        if (quality != AutoWowFixture::kMinQuality)
            out << ',';
        out << JsonString(kQualityNames[quality - AutoWowFixture::kMinQuality]) << ':'
            << gear.qualityCounts[quality - AutoWowFixture::kMinQuality];
    }
    out << "},\"other_quality_slots\":" << gear.otherQualitySlots << "}"
        << ",\"talents\":{\"allocated\":" << bot->GetTalentMap().size()
        << ",\"free_points\":" << bot->GetFreeTalentPoints() << "}"
        << ",\"spells\":{\"known\":" << bot->GetSpellMap().size()
        << ",\"role_critical_ready\":" << (readiness.roleCriticalSpell ? "true" : "false") << "}"
        << ",\"weapons\":{\"main_hand\":" << (readiness.mainHand ? "true" : "false")
        << ",\"off_hand\":" << (readiness.offHand ? "true" : "false")
        << ",\"ranged\":" << (readiness.ranged ? "true" : "false")
        << ",\"ready\":" << (readiness.weaponReady ? "true" : "false") << "}"
        << ",\"inventory\":{\"backpack_capacity\":" << inventory.backpackCapacity
        << ",\"equipped_bag_count\":" << inventory.equippedBagCount
        << ",\"bag_capacity\":" << inventory.bagCapacity
        << ",\"total_capacity\":" << inventory.totalCapacity
        << ",\"free_slots\":" << inventory.freeSlots
        << ",\"carried_item_count\":" << inventory.carriedItemCount
        << ",\"loot_slot_reserve\":" << PlayerbotFactory::kFixtureLootSlotReserve
        << ",\"loot_slot_reserve_met\":"
        << (inventory.freeSlots >= PlayerbotFactory::kFixtureLootSlotReserve ? "true" : "false") << "}"
        << ",\"mutation\":{";

    if (mutated)
    {
        out << "\"attempted\":true,\"factory\":\"PlayerbotFactory::InitializeFixture\","
               "\"save_to_db\":true,\"result\":\"applied\"";
    }
    else
    {
        out << "\"attempted\":false,\"result\":\"read_only\"";
    }

    out << "}}";
    return out.str();
}
}  // namespace

namespace AutoWowFixture
{
bool IsFixtureGuidAllowlisted(std::string const& configured, uint32 guid)
{
    if (!guid || configured.empty())
        return false;

    std::string token;
    bool invalidToken = false;
    for (unsigned char character : configured)
    {
        if (std::isdigit(character))
        {
            if (!invalidToken)
                token.push_back(static_cast<char>(character));
            continue;
        }

        if (character == ',' || std::isspace(character))
        {
            if (!invalidToken && ParseAllowlistToken(token, guid))
                return true;
            token.clear();
            invalidToken = false;
            continue;
        }

        token.clear();
        invalidToken = true;
    }

    return !invalidToken && ParseAllowlistToken(token, guid);
}

bool IsValidLevel(uint32 level, uint32 maxLevel)
{
    return level > 0 && maxLevel > 0 && level <= maxLevel;
}

bool IsValidSpecIndex(uint32 specIndex)
{
    return specIndex < kMaxSpecIndexExclusive;
}

bool IsValidQuality(uint32 quality)
{
    return quality >= kMinQuality && quality <= kMaxQuality;
}

std::string Init(uint32 guid, uint32 level, uint32 specIndex, uint32 quality)
{
    if (!IsValidLevel(level, static_cast<uint32>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))) ||
        !IsValidSpecIndex(specIndex) || !IsValidQuality(quality))
    {
        if (!IsValidLevel(level, static_cast<uint32>(sWorld->getIntConfig(CONFIG_MAX_PLAYER_LEVEL))))
            return Error("fixture_level_out_of_range");
        if (!IsValidSpecIndex(specIndex))
            return Error("fixture_spec_index_out_of_range");
        return Error("fixture_quality_out_of_range");
    }

    FixtureTarget target;
    std::string error;
    if (!ResolveFixtureTarget(guid, target, error))
        return Error(error);

    if (target.player->getClass() >= MAX_CLASSES ||
        sPlayerbotAIConfig.premadeSpecName[target.player->getClass()][specIndex].empty())
    {
        return Error("fixture_spec_not_configured");
    }

    if (target.player->IsInCombat())
        return Error("fixture_target_in_combat");

    // GiveLevel does not prune spells learned at the previous higher fixture level. Refuse the
    // unsafe transition until fixture spell reconstruction is transactional and complete.
    if (target.player->GetLevel() > level)
        return Error("fixture_downlevel_reuse_refused");

    PlayerbotFactory factory(target.player, level, quality);
    if (!factory.InitializeFixture(specIndex, quality))
        return Error("fixture_exact_quality_unavailable");
    // The factory changes talents after the bot AI has already built its class strategies.
    // Rebuild them immediately so tank/healer identity and behavior match the requested spec
    // in the same session instead of remaining stale until the next login.
    target.ai->ResetStrategies(false);

    return Evidence(target.player, target.ai, "fixture_init", target.leagueRole, static_cast<int32>(specIndex),
                    static_cast<int32>(quality), true);
}

std::string Status(uint32 guid)
{
    FixtureTarget target;
    std::string error;
    if (!ResolveFixtureTarget(guid, target, error))
        return Error(error);

    return Evidence(target.player, target.ai, "fixture_status", target.leagueRole, -1, -1, false);
}
}  // namespace AutoWowFixture
