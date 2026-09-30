/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "WarsongFixtureControl.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "BattlegroundMgr.h"
#include "BattlegroundQueue.h"
#include "BattlegroundScore.h"
#include "BattlegroundWS.h"
#include "DatabaseEnv.h"
#include "DBCStores.h"
#include "DisableMgr.h"
#include "ObjectAccessor.h"
#include "Opcodes.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotAIConfig.h"
#include "Playerbots.h"
#include "PlayerbotsDatabase.h"
#include "WorldPacket.h"
#include "WorldSession.h"

namespace
{
struct FixtureOwnership
{
    bool active = false;
    bool leaveIssued = false;
    uint32 level = 0;
    uint8 bracket = 0;
    std::vector<uint32> roster;
};

// Every caller reaches this state through AutoWowBridgeOperation on the world thread.
FixtureOwnership fixture;

struct QueueContext
{
    Battleground* bgTemplate = nullptr;
    BattlegroundBracketId bracket = BG_BRACKET_ID_FIRST;
    uint32 level = 0;
    std::vector<Player*> players;
    std::set<uint32> guids;
};

std::vector<uint32> SortedRoster(std::vector<uint32> roster)
{
    std::sort(roster.begin(), roster.end());
    return roster;
}

std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool ParseWireGuid(std::string const& value, uint32& guid)
{
    if (value.empty())
        return false;
    for (unsigned char character : value)
        if (!std::isdigit(character))
            return false;

    char* end = nullptr;
    unsigned long long const parsed = std::strtoull(value.c_str(), &end, 10);
    if (!end || *end != '\0' || !parsed || parsed > std::numeric_limits<uint32>::max())
        return false;
    guid = static_cast<uint32>(parsed);
    return true;
}

std::set<uint32> RosterSet(std::vector<uint32> const& roster)
{
    return std::set<uint32>(roster.begin(), roster.end());
}

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

std::string Error(std::string const& operation, std::string const& error, uint32 guid = 0)
{
    std::ostringstream out;
    out << "{\"ok\":false,\"operation\":" << JsonString(operation)
        << ",\"error\":" << JsonString(error);
    if (guid)
        out << ",\"guid\":" << guid;
    out << '}';
    return out.str();
}

char const* TeamName(TeamId team)
{
    switch (team)
    {
        case TEAM_ALLIANCE: return "alliance";
        case TEAM_HORDE: return "horde";
        default: return "neutral";
    }
}

char const* WinnerName(PvPTeamId winner)
{
    switch (winner)
    {
        case PVP_TEAM_ALLIANCE: return "alliance";
        case PVP_TEAM_HORDE: return "horde";
        default: return "none";
    }
}

char const* BattlegroundStateName(BattlegroundStatus status)
{
    switch (status)
    {
        case STATUS_WAIT_QUEUE: return "wait_queue";
        case STATUS_WAIT_JOIN: return "wait_join";
        case STATUS_IN_PROGRESS: return "in_progress";
        case STATUS_WAIT_LEAVE: return "complete";
        default: return "none";
    }
}

char const* FlagStateName(uint8 state)
{
    switch (state)
    {
        case BG_WS_FLAG_STATE_ON_BASE: return "base";
        case BG_WS_FLAG_STATE_ON_PLAYER: return "carried";
        case BG_WS_FLAG_STATE_ON_GROUND: return "ground";
        default: return "unknown";
    }
}

bool IsWsg(Battleground const* bg)
{
    return bg && bg->GetBgTypeID(true) == BATTLEGROUND_WS;
}

bool IsPlayerbot(Player* player)
{
    PlayerbotAI* botAI = player ? PlayerbotsMgr::instance().GetPlayerbotAI(player) : nullptr;
    return botAI && !botAI->IsRealPlayer();
}

bool LoadLeagueMembers(std::set<uint32> const& expected, std::set<uint32>& enrolled)
{
    std::ostringstream ids;
    bool first = true;
    for (uint32 guid : expected)
    {
        if (!first)
            ids << ',';
        ids << guid;
        first = false;
    }

    QueryResult result = PlayerbotsDatabase.Query(
        "SELECT character_guid FROM autowow_league_member "
        "WHERE retired_at IS NULL AND character_guid IN ({})", ids.str());
    if (!result)
        return false;

    do
    {
        Field* fields = result->Fetch();
        enrolled.insert(fields[0].Get<uint32>());
    } while (result->NextRow());

    return true;
}

BattlegroundQueue& WsgQueue()
{
    return sBattlegroundMgr->GetBattlegroundQueue(BATTLEGROUND_QUEUE_WS);
}

std::set<uint32> QueueRoster(BattlegroundBracketId bracket)
{
    std::set<uint32> roster;
    for (auto const& [guid, groupInfo] : WsgQueue().m_QueuedPlayers)
    {
        if (groupInfo && groupInfo->BracketId == static_cast<uint8>(bracket))
            roster.insert(guid.GetCounter());
    }
    return roster;
}

bool IsIndividualWsgQueueEntry(Player* player, BattlegroundBracketId bracket)
{
    GroupQueueInfo info;
    if (!player || !WsgQueue().GetPlayerGroupInfoData(player->GetGUID(), &info))
        return false;

    return info.BgTypeId == BATTLEGROUND_WS && info.BracketId == static_cast<uint8>(bracket) &&
           info.Players.size() == 1 && info.Players.count(player->GetGUID()) == 1;
}

void CanonicalJoin(Player* player)
{
    uint32 const instanceId = 0; // first available
    uint8 const joinAsGroup = 0;
    WorldPacket packet(CMSG_BATTLEMASTER_JOIN, 20);
    packet << player->GetGUID() << uint32(BATTLEGROUND_WS) << instanceId << joinAsGroup;
    player->GetSession()->HandleBattlemasterJoinOpcode(packet);
}

void CanonicalLeaveQueue(Player* player)
{
    uint8 const arenaType = 0;
    uint8 const unknown = 0;
    uint16 const portConstant = 0x1F90;
    uint8 const leaveQueue = 0;
    WorldPacket packet(CMSG_BATTLEFIELD_PORT, 20);
    packet << arenaType << unknown << uint32(BATTLEGROUND_WS) << portConstant << leaveQueue;
    player->GetSession()->HandleBattleFieldPortOpcode(packet);
}

void CanonicalLeaveBattleground(Player* player)
{
    WorldPacket packet(CMSG_LEAVE_BATTLEFIELD, 8);
    packet << uint8(0) << uint8(0) << uint32(BATTLEGROUND_WS) << uint16(0x1F90);
    player->GetSession()->HandleBattlefieldLeaveOpcode(packet);
}

bool RollBackJoined(std::vector<Player*> const& joined)
{
    for (auto itr = joined.rbegin(); itr != joined.rend(); ++itr)
    {
        if (WsgQueue().m_QueuedPlayers.count((*itr)->GetGUID()))
            CanonicalLeaveQueue(*itr);
    }

    for (Player* player : joined)
        if (WsgQueue().m_QueuedPlayers.count(player->GetGUID()))
            return false;
    return true;
}

bool ResolveQueueContext(std::vector<uint32> const& roster, QueueContext& context,
                         std::string& error, uint32& errorGuid)
{
    if (sPlayerbotAIConfig.randomBotJoinBG || sPlayerbotAIConfig.randomBotAutoJoinBG)
    {
        error = "wsg_random_bg_automation_enabled";
        return false;
    }

    if (sBattlegroundMgr->isTesting() || sBattlegroundMgr->isArenaTesting())
    {
        error = "wsg_battleground_debug_enabled";
        return false;
    }

    if (DisableMgr::IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, BATTLEGROUND_WS, nullptr))
    {
        error = "wsg_disabled";
        return false;
    }

    context.bgTemplate = sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_WS);
    if (!context.bgTemplate)
    {
        error = "wsg_template_missing";
        return false;
    }
    if (context.bgTemplate->GetMaxPlayersPerTeam() != AutoWowWarsong::kFactionSize)
    {
        error = "wsg_template_not_10v10";
        return false;
    }

    context.guids = RosterSet(roster);
    std::set<uint32> enrolled;
    if (!LoadLeagueMembers(context.guids, enrolled))
    {
        error = "wsg_league_roster_not_found";
        return false;
    }
    for (uint32 guid : context.guids)
    {
        if (!enrolled.contains(guid))
        {
            error = "wsg_bot_not_enrolled_in_league";
            errorGuid = guid;
            return false;
        }
    }

    std::size_t alliance = 0;
    std::size_t horde = 0;
    bool first = true;
    context.players.reserve(roster.size());
    for (uint32 guid : roster)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        if (!bot || !bot->IsInWorld())
        {
            error = "wsg_bot_not_online";
            errorGuid = guid;
            return false;
        }
        if (!IsPlayerbot(bot))
        {
            error = "wsg_roster_contains_non_playerbot";
            errorGuid = guid;
            return false;
        }
        if (!bot->GetSession())
        {
            error = "wsg_bot_session_missing";
            errorGuid = guid;
            return false;
        }
        if (bot->InBattleground() || bot->InBattlegroundQueue())
        {
            error = "wsg_bot_already_queued_or_in_battleground";
            errorGuid = guid;
            return false;
        }
        // A rollback uses the canonical queue-leave opcode, which itself rejects combat/charm.
        if (bot->IsInCombat() || bot->GetCharmGUID())
        {
            error = "wsg_bot_in_combat_or_charmed";
            errorGuid = guid;
            return false;
        }
        if (!bot->HasFreeBattlegroundQueueId() || !bot->GetBGAccessByLevel(BATTLEGROUND_WS) ||
            !bot->CanJoinToBattleground(context.bgTemplate))
        {
            error = "wsg_bot_not_eligible";
            errorGuid = guid;
            return false;
        }

        PvPDifficultyEntry const* bracket =
            GetBattlegroundBracketByLevel(context.bgTemplate->GetMapId(), bot->GetLevel());
        if (!bracket)
        {
            error = "wsg_bracket_missing";
            errorGuid = guid;
            return false;
        }

        if (first)
        {
            context.level = bot->GetLevel();
            context.bracket = bracket->GetBracketId();
            first = false;
        }
        else if (bot->GetLevel() != context.level)
        {
            error = "wsg_roster_levels_differ";
            errorGuid = guid;
            return false;
        }
        else if (bracket->GetBracketId() != context.bracket)
        {
            error = "wsg_roster_brackets_differ";
            errorGuid = guid;
            return false;
        }

        if (bot->GetTeamId() == TEAM_ALLIANCE)
            ++alliance;
        else if (bot->GetTeamId() == TEAM_HORDE)
            ++horde;
        else
        {
            error = "wsg_roster_neutral_team";
            errorGuid = guid;
            return false;
        }

        context.players.push_back(bot);
    }

    if (alliance != AutoWowWarsong::kFactionSize || horde != AutoWowWarsong::kFactionSize)
    {
        error = "wsg_roster_requires_10_alliance_10_horde";
        return false;
    }

    if (!QueueRoster(context.bracket).empty())
    {
        error = "wsg_foreign_queue_players_in_bracket";
        return false;
    }

    for (Battleground const* bg : sBattlegroundMgr->GetActiveBattlegrounds())
    {
        if (!IsWsg(bg) || bg->GetBracketId() != context.bracket)
            continue;

        for (auto const& [guid, player] : bg->GetPlayers())
        {
            (void)player;
            if (!context.guids.contains(guid.GetCounter()))
            {
                error = "wsg_foreign_active_players_in_bracket";
                errorGuid = guid.GetCounter();
                return false;
            }
        }
    }

    return true;
}

struct PlayerScoreSnapshot
{
    uint32 kills = 0;
    uint32 honorableKills = 0;
    uint32 deaths = 0;
    uint32 damage = 0;
    uint32 healing = 0;
    uint32 captures = 0;
    uint32 returns = 0;
};

std::map<uint32, PlayerScoreSnapshot> ReadWsgScores(Battleground const* bg)
{
    std::map<uint32, PlayerScoreSnapshot> scores;
    if (!bg)
        return scores;

    // AzerothCore exposes all score fields through its canonical PvP-log packet. The score class
    // deliberately keeps deaths/HKs/objectives protected, so decode the packet it already builds
    // instead of reaching into protected storage or mirroring core internals.
    WorldPacket packet;
    const_cast<Battleground*>(bg)->BuildPvPLogDataPacket(packet);

    uint8 arena = 0;
    packet >> arena;
    if (arena)
        return scores;

    uint8 ended = 0;
    packet >> ended;
    if (ended)
    {
        uint8 winner = 0;
        packet >> winner;
    }

    uint32 count = 0;
    packet >> count;
    for (uint32 index = 0; index < count; ++index)
    {
        ObjectGuid guid;
        PlayerScoreSnapshot score;
        uint32 bonusHonor = 0;
        uint32 objectiveCount = 0;
        packet >> guid >> score.kills >> score.honorableKills >> score.deaths >> bonusHonor >>
            score.damage >> score.healing >> objectiveCount;

        for (uint32 objective = 0; objective < objectiveCount; ++objective)
        {
            uint32 value = 0;
            packet >> value;
            if (objective == 0)
                score.captures = value;
            else if (objective == 1)
                score.returns = value;
        }
        scores.emplace(guid.GetCounter(), score);
    }
    return scores;
}

std::string BuildStatus(std::string const& operation)
{
    std::set<uint32> const expected = RosterSet(fixture.roster);
    BattlegroundBracketId const bracket = static_cast<BattlegroundBracketId>(fixture.bracket);
    std::set<uint32> const queued = QueueRoster(bracket);
    std::set<uint32> observed = queued;
    std::set<Battleground const*> relevantBattlegrounds;
    Battleground const* representative = nullptr;

    for (uint32 guid : fixture.roster)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        Battleground const* bg = bot && bot->InBattleground() ? bot->GetBattleground(true) : nullptr;
        if (!IsWsg(bg) || bg->GetBracketId() != bracket)
            continue;
        if (!representative)
            representative = bg;
        relevantBattlegrounds.insert(bg);
    }

    for (Battleground const* bg : sBattlegroundMgr->GetActiveBattlegrounds())
    {
        if (IsWsg(bg) && bg->GetBracketId() == bracket)
            relevantBattlegrounds.insert(bg);
    }

    if (!representative && !relevantBattlegrounds.empty())
        representative = *relevantBattlegrounds.begin();

    for (Battleground const* bg : relevantBattlegrounds)
        for (auto const& [guid, player] : bg->GetPlayers())
        {
            (void)player;
            observed.insert(guid.GetCounter());
        }

    bool sameLevel = true;
    bool sameBracket = true;
    bool sameInstance = true;
    uint32 sharedInstance = 0;
    std::size_t rosterInBattleground = 0;
    bool anyInvite = false;

    for (uint32 guid : fixture.roster)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        if (!bot)
        {
            sameLevel = false;
            sameBracket = false;
            sameInstance = false;
            continue;
        }

        sameLevel = sameLevel && bot->GetLevel() == fixture.level;
        Battleground const* bracketSource = representative ? representative :
            sBattlegroundMgr->GetBattlegroundTemplate(BATTLEGROUND_WS);
        PvPDifficultyEntry const* botBracket = bracketSource ?
            GetBattlegroundBracketByLevel(bracketSource->GetMapId(), bot->GetLevel()) : nullptr;
        sameBracket = sameBracket && botBracket && botBracket->GetBracketId() == bracket;

        GroupQueueInfo queueInfo;
        if (WsgQueue().GetPlayerGroupInfoData(bot->GetGUID(), &queueInfo))
            anyInvite = anyInvite || queueInfo.IsInvitedToBGInstanceGUID != 0;

        Battleground const* bg = bot->InBattleground() ? bot->GetBattleground(true) : nullptr;
        bool const participating = IsWsg(bg) && bg->GetBracketId() == bracket &&
                                   bg->GetPlayers().count(bot->GetGUID()) == 1;
        if (!participating)
        {
            sameInstance = false;
            continue;
        }

        ++rosterInBattleground;
        if (!sharedInstance)
            sharedInstance = bg->GetInstanceID();
        else if (sharedInstance != bg->GetInstanceID())
            sameInstance = false;
    }

    sameInstance = sameInstance && rosterInBattleground == AutoWowWarsong::kRosterSize;
    bool const exactRoster = observed == expected;
    bool const exactQueueRoster = queued == expected;
    uint32 const alliancePlayers = representative ? representative->GetPlayersCountByTeam(TEAM_ALLIANCE) : 0;
    uint32 const hordePlayers = representative ? representative->GetPlayersCountByTeam(TEAM_HORDE) : 0;
    bool const tenVTen = exactRoster && sameInstance && representative &&
                         representative->GetPlayersSize() == AutoWowWarsong::kRosterSize &&
                         alliancePlayers == AutoWowWarsong::kFactionSize &&
                         hordePlayers == AutoWowWarsong::kFactionSize;

    std::string state;
    if (representative)
        state = BattlegroundStateName(representative->GetStatus());
    else if (anyInvite)
        state = "invited";
    else if (!queued.empty())
        state = "queued";
    else if (fixture.leaveIssued)
        state = "complete";
    else
        state = "idle";

    BattlegroundWS const* wsg = representative ? representative->ToBattlegroundWS() : nullptr;
    std::map<uint32, PlayerScoreSnapshot> const scores = ReadWsgScores(representative);
    uint32 const instanceId = representative ? representative->GetInstanceID() : 0;
    uint32 const allianceScore = representative ? representative->GetTeamScore(TEAM_ALLIANCE) : 0;
    uint32 const hordeScore = representative ? representative->GetTeamScore(TEAM_HORDE) : 0;
    uint8 const allianceFlagState = wsg ? wsg->GetFlagState(TEAM_ALLIANCE) : 0;
    uint8 const hordeFlagState = wsg ? wsg->GetFlagState(TEAM_HORDE) : 0;
    uint32 const allianceCarrier = wsg ? wsg->GetFlagPickerGUID(TEAM_ALLIANCE).GetCounter() : 0;
    uint32 const hordeCarrier = wsg ? wsg->GetFlagPickerGUID(TEAM_HORDE).GetCounter() : 0;
    uint32 allianceCaptures = 0;
    uint32 allianceReturns = 0;
    uint32 hordeCaptures = 0;
    uint32 hordeReturns = 0;
    for (uint32 guid : fixture.roster)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        auto const scoreItr = scores.find(guid);
        if (!bot || scoreItr == scores.end())
            continue;

        if (bot->GetTeamId() == TEAM_ALLIANCE)
        {
            allianceCaptures += scoreItr->second.captures;
            allianceReturns += scoreItr->second.returns;
        }
        else if (bot->GetTeamId() == TEAM_HORDE)
        {
            hordeCaptures += scoreItr->second.captures;
            hordeReturns += scoreItr->second.returns;
        }
    }

    std::ostringstream out;
    out << "{\"ok\":true,\"operation\":" << JsonString(operation)
        << ",\"instance_id\":" << instanceId
        << ",\"battleground_state\":" << JsonString(state)
        << ",\"state\":" << JsonString(state)
        << ",\"score\":{\"alliance\":" << allianceScore << ",\"horde\":" << hordeScore << '}'
        << ",\"flags\":{\"alliance\":{\"state\":" << JsonString(FlagStateName(allianceFlagState))
        << ",\"captures\":" << allianceCaptures << ",\"returns\":" << allianceReturns
        << ",\"carrier_guid\":" << allianceCarrier
        << "},\"horde\":{\"state\":" << JsonString(FlagStateName(hordeFlagState))
        << ",\"captures\":" << hordeCaptures << ",\"returns\":" << hordeReturns
        << ",\"carrier_guid\":" << hordeCarrier << "}}"
        << ",\"winner\":" << JsonString(representative ? WinnerName(representative->GetWinner()) : "none")
        << ",\"proof\":{\"expected_roster_count\":" << expected.size()
        << ",\"observed_roster_count\":" << observed.size()
        << ",\"queue_count\":" << queued.size()
        << ",\"active_count\":" << rosterInBattleground
        << ",\"exact_roster\":" << (exactRoster ? "true" : "false")
        << ",\"exact_queue_roster\":" << (exactQueueRoster ? "true" : "false")
        << ",\"same_level\":" << (sameLevel ? "true" : "false")
        << ",\"same_bracket\":" << (sameBracket ? "true" : "false")
        << ",\"same_instance\":" << (sameInstance ? "true" : "false")
        << ",\"ten_v_ten\":" << (tenVTen ? "true" : "false")
        << ",\"alliance_active\":" << alliancePlayers
        << ",\"horde_active\":" << hordePlayers << '}';

    out << ",\"wsg\":{\"available\":" << (wsg ? "true" : "false")
        << ",\"map\":" << (representative ? representative->GetMapId() : 0)
        << ",\"type\":" << uint32(BATTLEGROUND_WS)
        << ",\"bracket\":" << uint32(bracket)
        << ",\"instance\":" << instanceId
        << ",\"score\":{\"alliance\":" << allianceScore
        << ",\"horde\":" << hordeScore << '}';

    out << ",\"flags\":{\"alliance\":{\"state\":" << JsonString(FlagStateName(allianceFlagState))
        << ",\"captures\":" << allianceCaptures << ",\"returns\":" << allianceReturns
        << ",\"carrier_guid\":" << allianceCarrier
        << "},\"horde\":{\"state\":" << JsonString(FlagStateName(hordeFlagState))
        << ",\"captures\":" << hordeCaptures << ",\"returns\":" << hordeReturns
        << ",\"carrier_guid\":" << hordeCarrier
        << "}}}";

    out << ",\"roster\":[";
    bool firstPlayer = true;
    for (uint32 guid : fixture.roster)
    {
        if (!firstPlayer)
            out << ',';
        firstPlayer = false;

        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        GroupQueueInfo queueInfo;
        bool const inQueue = bot && WsgQueue().GetPlayerGroupInfoData(bot->GetGUID(), &queueInfo);
        bool const invited = inQueue && queueInfo.IsInvitedToBGInstanceGUID != 0;
        Battleground const* bg = bot && bot->InBattleground() ? bot->GetBattleground(true) : nullptr;
        bool const inWsg = IsWsg(bg);
        TeamId const team = inWsg ? bot->GetBgTeamId() : inQueue ? queueInfo.teamId :
                            bot ? bot->GetTeamId() : TEAM_NEUTRAL;
        auto const scoreItr = scores.find(guid);
        PlayerScoreSnapshot const* score = scoreItr == scores.end() ? nullptr : &scoreItr->second;
        uint32 const queueSlot = inQueue ? bot->GetBattlegroundQueueIndex(BATTLEGROUND_QUEUE_WS) :
                                          PLAYER_MAX_BATTLEGROUND_QUEUES;

        char const* playerState = !bot ? "offline" : inWsg ? "in_battleground" : invited ? "invited" :
                                  inQueue ? "queued" : bot->IsBeingTeleported() ? "leaving" : "idle";
        uint32 const playerInstance = inWsg ? bg->GetInstanceID() :
                                      invited ? queueInfo.IsInvitedToBGInstanceGUID : 0;
        char const* queueState = inWsg ? BattlegroundStateName(bg->GetStatus()) : invited ? "invite" :
                                 inQueue ? "queued" : "none";
        char const* battlegroundState = inWsg ? BattlegroundStateName(bg->GetStatus()) :
                                        invited ? "wait_join" : fixture.leaveIssued ? "complete" : "none";
        char const* flagState = guid == allianceCarrier || guid == hordeCarrier ? "carrying" : "none";

        out << "{\"guid\":" << guid
            << ",\"name\":" << JsonString(bot ? bot->GetName() : "")
            << ",\"online\":" << (bot ? "true" : "false")
            << ",\"playerbot\":" << (IsPlayerbot(bot) ? "true" : "false")
            << ",\"faction\":" << JsonString(TeamName(team))
            << ",\"team\":" << JsonString(TeamName(team))
            << ",\"instance_id\":" << playerInstance
            << ",\"queue_state\":" << JsonString(queueState)
            << ",\"battleground_state\":" << JsonString(battlegroundState)
            << ",\"kills\":" << (score ? score->kills : 0)
            << ",\"deaths\":" << (score ? score->deaths : 0)
            << ",\"honorable_kills\":" << (score ? score->honorableKills : 0)
            << ",\"damage\":" << (score ? score->damage : 0)
            << ",\"healing\":" << (score ? score->healing : 0)
            << ",\"flag_state\":" << JsonString(flagState)
            << ",\"flag_captures\":" << (score ? score->captures : 0)
            << ",\"flag_returns\":" << (score ? score->returns : 0)
            << ",\"state\":" << JsonString(playerState)
            << ",\"level\":" << (bot ? uint32(bot->GetLevel()) : 0)
            << ",\"world_map\":" << (bot ? bot->GetMapId() : 0)
            << ",\"queue\":{\"joined\":" << (inQueue ? "true" : "false")
            << ",\"invite\":" << (invited ? "true" : "false")
            << ",\"type\":" << uint32(BATTLEGROUND_QUEUE_WS)
            << ",\"slot\":";
        if (queueSlot < PLAYER_MAX_BATTLEGROUND_QUEUES)
            out << queueSlot;
        else
            out << -1;
        out << ",\"bracket\":" << (inQueue ? uint32(queueInfo.BracketId) : uint32(bracket))
            << ",\"instance\":" << (invited ? queueInfo.IsInvitedToBGInstanceGUID : 0) << '}'
            << ",\"battleground\":{\"inside\":" << (inWsg ? "true" : "false")
            << ",\"map\":" << (inWsg ? bg->GetMapId() : 0)
            << ",\"type\":" << (inWsg ? uint32(bg->GetBgTypeID(true)) : 0)
            << ",\"bracket\":" << (inWsg ? uint32(bg->GetBracketId()) : uint32(bracket))
            << ",\"instance\":" << (inWsg ? bg->GetInstanceID() : 0)
            << ",\"team\":" << JsonString(TeamName(team)) << '}'
            << ",\"stats\":{\"kills\":" << (score ? score->kills : 0)
            << ",\"deaths\":" << (score ? score->deaths : 0)
            << ",\"honorable_kills\":" << (score ? score->honorableKills : 0)
            << ",\"damage\":" << (score ? score->damage : 0)
            << ",\"healing\":" << (score ? score->healing : 0)
            << ",\"captures\":" << (score ? score->captures : 0)
            << ",\"returns\":" << (score ? score->returns : 0) << "}}";
    }
    out << "]}";
    return out.str();
}

bool RosterOwned(std::vector<uint32> const& roster)
{
    return fixture.active && AutoWowWarsong::IsSameRoster(fixture.roster, roster);
}

bool LeavePreflight(std::string& error, uint32& errorGuid)
{
    BattlegroundBracketId const bracket = static_cast<BattlegroundBracketId>(fixture.bracket);
    for (uint32 guid : fixture.roster)
    {
        Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        if (!bot || !bot->GetSession())
        {
            error = "wsg_leave_bot_not_online";
            errorGuid = guid;
            return false;
        }
        if (!IsPlayerbot(bot))
        {
            error = "wsg_leave_target_not_playerbot";
            errorGuid = guid;
            return false;
        }

        GroupQueueInfo info;
        if (WsgQueue().GetPlayerGroupInfoData(bot->GetGUID(), &info))
        {
            if (info.BgTypeId != BATTLEGROUND_WS || info.BracketId != fixture.bracket ||
                info.Players.size() != 1 || info.Players.count(bot->GetGUID()) != 1)
            {
                error = "wsg_leave_queue_entry_not_individual";
                errorGuid = guid;
                return false;
            }
            if (bot->GetCharmGUID())
            {
                error = "wsg_leave_queue_target_charmed";
                errorGuid = guid;
                return false;
            }
        }

        if (bot->InBattleground())
        {
            Battleground* bg = bot->GetBattleground(true);
            if (!IsWsg(bg) || bg->GetBracketId() != bracket)
            {
                error = "wsg_leave_target_in_foreign_battleground";
                errorGuid = guid;
                return false;
            }
        }
    }
    return true;
}

struct LeaveVerification
{
    bool queueClear = true;
    bool canonicalAccepted = true;
    bool allClear = true;
    uint32 teleportsPending = 0;
};

LeaveVerification VerifyLeave()
{
    LeaveVerification verification;
    for (uint32 guid : fixture.roster)
    {
        ObjectGuid const objectGuid = ObjectGuid::Create<HighGuid::Player>(guid);
        if (WsgQueue().m_QueuedPlayers.count(objectGuid))
        {
            verification.queueClear = false;
            verification.allClear = false;
        }

        Player* bot = ObjectAccessor::FindConnectedPlayer(objectGuid);
        if (!bot)
        {
            verification.canonicalAccepted = false;
            verification.allClear = false;
            continue;
        }

        if (bot->InBattleground())
        {
            verification.allClear = false;
            if (bot->IsBeingTeleported())
                ++verification.teleportsPending;
            else
                verification.canonicalAccepted = false;
        }
    }
    return verification;
}
}  // namespace

namespace AutoWowWarsong
{
bool ParseWireRequest(std::string const& requestText, WireOperation& operation,
                      std::vector<uint32>& roster, std::string& error)
{
    std::istringstream input(requestText);
    std::string command;
    std::string subcommand;
    input >> command;
    command = Lowercase(command);

    if (command == "wsg")
    {
        input >> subcommand;
        subcommand = Lowercase(subcommand);
    }
    else if (command == "wsg-queue")
        subcommand = "queue";
    else if (command == "wsg-status")
        subcommand = "status";
    else if (command == "wsg-leave")
        subcommand = "leave";

    if (subcommand != "queue" && subcommand != "status" && subcommand != "leave")
    {
        error = "wsg requires queue, status, or leave followed by exactly 20 GUIDs";
        return false;
    }

    roster.clear();
    std::string guidToken;
    while (input >> guidToken)
    {
        uint32 guid = 0;
        if (!ParseWireGuid(guidToken, guid))
        {
            error = "wsg " + subcommand + " GUIDs must be positive numeric values";
            return false;
        }
        roster.push_back(guid);
    }

    if (!ValidateRosterShape(roster, error))
        return false;

    operation = subcommand == "queue" ? WireOperation::Queue :
                subcommand == "status" ? WireOperation::Status : WireOperation::Leave;
    error.clear();
    return true;
}

bool ValidateRosterShape(std::vector<uint32> const& roster, std::string& error)
{
    if (roster.size() != kRosterSize)
    {
        error = "wsg_requires_exactly_20_guids";
        return false;
    }

    std::set<uint32> unique;
    for (uint32 guid : roster)
    {
        if (!guid)
        {
            error = "wsg_guids_must_be_positive";
            return false;
        }
        if (!unique.insert(guid).second)
        {
            error = "wsg_guids_must_be_unique";
            return false;
        }
    }

    error.clear();
    return true;
}

bool IsSameRoster(std::vector<uint32> const& left, std::vector<uint32> const& right)
{
    return left.size() == right.size() && SortedRoster(left) == SortedRoster(right);
}

std::string Queue(std::vector<uint32> const& roster)
{
    std::string shapeError;
    if (!ValidateRosterShape(roster, shapeError))
        return Error("wsg_queue", shapeError);
    if (fixture.active)
    {
        if (!fixture.leaveIssued)
            return Error("wsg_queue", "wsg_fixture_already_owned");

        LeaveVerification const priorLeave = VerifyLeave();
        if (!priorLeave.queueClear || !priorLeave.allClear)
            return Error("wsg_queue", "wsg_fixture_leave_incomplete");
        fixture = FixtureOwnership{};
    }

    QueueContext context;
    std::string error;
    uint32 errorGuid = 0;
    if (!ResolveQueueContext(roster, context, error, errorGuid))
        return Error("wsg_queue", error, errorGuid);

    std::vector<Player*> joined;
    joined.reserve(context.players.size());
    for (Player* bot : context.players)
    {
        CanonicalJoin(bot);
        if (!IsIndividualWsgQueueEntry(bot, context.bracket))
        {
            if (WsgQueue().m_QueuedPlayers.count(bot->GetGUID()))
                joined.push_back(bot);
            bool const rolledBack = RollBackJoined(joined);
            return Error("wsg_queue", rolledBack ? "wsg_individual_join_rejected" :
                                                   "wsg_queue_rollback_failed",
                         bot->GetGUID().GetCounter());
        }
        joined.push_back(bot);
    }

    if (QueueRoster(context.bracket) != context.guids)
    {
        bool const rolledBack = RollBackJoined(joined);
        return Error("wsg_queue", rolledBack ? "wsg_exact_queue_roster_verification_failed" :
                                               "wsg_queue_rollback_failed");
    }

    fixture.active = true;
    fixture.leaveIssued = false;
    fixture.level = context.level;
    fixture.bracket = static_cast<uint8>(context.bracket);
    fixture.roster = roster;
    return BuildStatus("wsg_queue");
}

std::string Status(std::vector<uint32> const& roster)
{
    std::string shapeError;
    if (!ValidateRosterShape(roster, shapeError))
        return Error("wsg_status", shapeError);
    if (!RosterOwned(roster))
        return Error("wsg_status", fixture.active ? "wsg_roster_mismatch" : "wsg_fixture_not_owned");
    return BuildStatus("wsg_status");
}

std::string Leave(std::vector<uint32> const& roster)
{
    std::string shapeError;
    if (!ValidateRosterShape(roster, shapeError))
        return Error("wsg_leave", shapeError);
    if (!RosterOwned(roster))
        return Error("wsg_leave", fixture.active ? "wsg_roster_mismatch" : "wsg_fixture_not_owned");

    bool const previouslyIssued = fixture.leaveIssued;
    if (!fixture.leaveIssued)
    {
        std::string error;
        uint32 errorGuid = 0;
        if (!LeavePreflight(error, errorGuid))
            return Error("wsg_leave", error, errorGuid);

        for (uint32 guid : fixture.roster)
        {
            Player* bot = ObjectAccessor::FindConnectedPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
            if (WsgQueue().m_QueuedPlayers.count(bot->GetGUID()))
                CanonicalLeaveQueue(bot);
            else if (bot->InBattleground())
                CanonicalLeaveBattleground(bot);
        }
        fixture.leaveIssued = true;
    }

    LeaveVerification const verification = VerifyLeave();
    bool const ok = verification.queueClear && verification.canonicalAccepted;
    std::string const status = BuildStatus("wsg_status");
    bool const releaseOwnership = ok && verification.allClear;

    std::ostringstream out;
    out << "{\"ok\":" << (ok ? "true" : "false")
        << ",\"operation\":\"wsg_leave\",\"verification\":{"
        << "\"canonical_leave_issued\":" << (!previouslyIssued ? "true" : "false")
        << ",\"queue_clear\":" << (verification.queueClear ? "true" : "false")
        << ",\"battleground_clear\":" << (verification.allClear ? "true" : "false")
        << ",\"teleports_pending\":" << verification.teleportsPending
        << ",\"canonical_leave_accepted\":" << (verification.canonicalAccepted ? "true" : "false")
        << ",\"ownership_released\":" << (releaseOwnership ? "true" : "false")
        << ",\"status_receipt_retained\":true"
        << "},\"status\":" << status << '}';

    return out.str();
}
}  // namespace AutoWowWarsong
