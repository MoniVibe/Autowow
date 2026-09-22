/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "RaidFixtureControl.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <limits>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "DatabaseEnv.h"
#include "Group.h"
#include "GroupMgr.h"
#include "Log.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerbotAI.h"
#include "PlayerbotOperations.h"
#include "Playerbots.h"

namespace
{
struct FixtureOwnership
{
    bool active = false;
    uint32 groupId = 0;
    uint32 leaderGuid = 0;
    uint32 difficulty = 0;
    std::vector<uint32> roster;
};

// All access is through AutoWowBridgeOperation, so this owner receipt is world-thread confined.
FixtureOwnership fixture;

struct CreateContext
{
    Player* leader = nullptr;
    std::vector<Player*> members;
    std::vector<uint32> roster;
    std::set<uint32> rosterSet;
};

std::string Lowercase(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });
    return value;
}

bool ParseGuid(std::string const& value, uint32& guid)
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

std::vector<uint32> OrderedRoster(uint32 leaderGuid, std::vector<uint32> const& memberGuids)
{
    std::vector<uint32> roster;
    roster.reserve(memberGuids.size() + 1);
    roster.push_back(leaderGuid);
    roster.insert(roster.end(), memberGuids.begin(), memberGuids.end());
    return roster;
}

std::vector<uint32> SortedRoster(std::vector<uint32> roster)
{
    std::sort(roster.begin(), roster.end());
    return roster;
}

std::set<uint32> GroupRosterSet(Group const* group)
{
    std::set<uint32> roster;
    if (!group)
        return roster;
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
        roster.insert(slot.guid.GetCounter());
    return roster;
}

bool FindForeignGroupMember(Group const* group, std::set<uint32> const& expected, uint32& foreignGuid)
{
    if (!group)
        return false;
    for (Group::MemberSlot const& slot : group->GetMemberSlots())
    {
        if (!expected.contains(slot.guid.GetCounter()))
        {
            foreignGuid = slot.guid.GetCounter();
            return true;
        }
    }
    return false;
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

bool IsOnlinePlayerbot(Player* player)
{
    PlayerbotAI* botAI = player ? PlayerbotsMgr::instance().GetPlayerbotAI(player) : nullptr;
    return player && player->IsInWorld() && botAI && !botAI->IsRealPlayer();
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

char const* TeamName(TeamId team)
{
    switch (team)
    {
        case TEAM_ALLIANCE: return "alliance";
        case TEAM_HORDE: return "horde";
        default: return "neutral";
    }
}

char const* GroupTypeName(Group const* group)
{
    if (!group)
        return "missing";
    if (group->isBGGroup())
        return group->isRaidGroup() ? "battleground_raid" : "battleground_party";
    if (group->isBFGroup())
        return group->isRaidGroup() ? "battlefield_raid" : "battlefield_party";
    if (group->isLFGGroup())
        return group->isRaidGroup() ? "lfg_raid" : "lfg_party";
    return group->isRaidGroup() ? "raid" : "party";
}

char const* RoleName(Player* player)
{
    if (!player)
        return "unavailable";
    if (PlayerbotAI::IsTank(player))
        return "tank";
    if (PlayerbotAI::IsHeal(player))
        return "healer";
    if (PlayerbotAI::IsRangedDps(player))
        return "ranged_dps";
    if (PlayerbotAI::IsDps(player))
        return "melee_dps";
    return "unknown";
}

bool ResolveCreateContext(uint32 leaderGuid, std::vector<uint32> const& memberGuids,
                          CreateContext& context, std::string& error, uint32& errorGuid)
{
    context.roster = OrderedRoster(leaderGuid, memberGuids);
    context.rosterSet = std::set<uint32>(context.roster.begin(), context.roster.end());

    std::set<uint32> enrolled;
    if (!LoadLeagueMembers(context.rosterSet, enrolled))
    {
        error = "raid_league_roster_not_found";
        return false;
    }
    for (uint32 guid : context.roster)
    {
        if (!enrolled.contains(guid))
        {
            error = "raid_bot_not_enrolled_in_league";
            errorGuid = guid;
            return false;
        }
    }

    TeamId team = TEAM_NEUTRAL;
    context.members.reserve(memberGuids.size());
    for (std::size_t index = 0; index < context.roster.size(); ++index)
    {
        uint32 const guid = context.roster[index];
        // Headless Playerbots can be loaded in-world without a client-backed connected session.
        // FindPlayer is the authoritative loaded-object lookup; IsOnlinePlayerbot below still
        // rejects real players, offline objects, and non-Playerbot actors.
        Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        if (!player || !player->IsInWorld())
        {
            error = "raid_bot_not_online";
            errorGuid = guid;
            return false;
        }
        if (!IsOnlinePlayerbot(player))
        {
            error = "raid_roster_contains_non_playerbot";
            errorGuid = guid;
            return false;
        }

        if (index == 0)
        {
            context.leader = player;
            team = player->GetTeamId();
            if (team != TEAM_ALLIANCE && team != TEAM_HORDE)
            {
                error = "raid_neutral_faction_not_allowed";
                errorGuid = guid;
                return false;
            }
        }
        else
        {
            if (player->GetTeamId() != team)
            {
                error = "raid_cross_faction_roster_not_allowed";
                errorGuid = guid;
                return false;
            }
            context.members.push_back(player);
        }

        if (Group* existing = player->GetGroup())
        {
            uint32 foreignGuid = 0;
            if (FindForeignGroupMember(existing, context.rosterSet, foreignGuid))
            {
                error = "raid_foreign_group_member";
                errorGuid = foreignGuid;
                return false;
            }

            // Even an exact pre-existing group is not silently claimed: ownership starts only for
            // a group created by this operation, making later leave/disband safely scoped.
            error = "raid_roster_member_already_grouped";
            errorGuid = guid;
            return false;
        }
    }

    return true;
}

void RollBackCreatedGroup(Group* group)
{
    if (!group)
        return;
    uint32 const groupId = group->GetGUID().GetCounter();
    if (sGroupMgr->GetGroupByGUID(groupId) == group)
        group->Disband();
}

bool VerifyExactCreatedGroup(Group* group, CreateContext const& context, uint32 difficulty)
{
    if (!group)
    {
        LOG_ERROR("playerbots", "AutoWow raid verification failed: group_missing");
        return false;
    }

    uint32 const observedLeader = group->GetLeaderGUID().GetCounter();
    uint32 const observedCount = group->GetMembersCount();
    uint32 const observedDifficulty = static_cast<uint32>(group->GetRaidDifficulty());
    std::set<uint32> const observedRoster = GroupRosterSet(group);
    if (observedLeader != context.roster.front())
    {
        LOG_ERROR("playerbots", "AutoWow raid verification failed: leader expected={} observed={}",
                  context.roster.front(), observedLeader);
        return false;
    }
    if (!group->isRaidGroup() || group->isBGGroup() || group->isBFGroup() || group->isLFGGroup())
    {
        LOG_ERROR("playerbots", "AutoWow raid verification failed: group_type raid={} bg={} bf={} lfg={}",
                  group->isRaidGroup(), group->isBGGroup(), group->isBFGroup(), group->isLFGGroup());
        return false;
    }
    if (observedCount != context.roster.size())
    {
        LOG_ERROR("playerbots", "AutoWow raid verification failed: member_count expected={} observed={}",
                  context.roster.size(), observedCount);
        return false;
    }
    if (observedRoster != context.rosterSet)
    {
        LOG_ERROR("playerbots", "AutoWow raid verification failed: roster_set expected_count={} observed_count={}",
                  context.rosterSet.size(), observedRoster.size());
        return false;
    }
    if (observedDifficulty != difficulty)
    {
        LOG_ERROR("playerbots", "AutoWow raid verification failed: difficulty expected={} observed={}",
                  difficulty, observedDifficulty);
        return false;
    }

    // ResolveCreateContext proved every GUID was an in-world Playerbot immediately before this
    // synchronous world-thread mutation. Group::AddMember transiently rebinds the follower's
    // manager/world state, so repeating per-player availability here can reject a valid headless
    // raid. Exact leader, type, size, difficulty, and the complete GUID roster are the durable
    // postconditions; together they exclude missing, duplicate, or foreign members.
    return true;
}

Group* OwnedGroup()
{
    return fixture.active && fixture.groupId ? sGroupMgr->GetGroupByGUID(fixture.groupId) : nullptr;
}

std::string BuildStatus(std::string const& operation)
{
    Group* group = OwnedGroup();
    if (!group)
        return Error(operation, "raid_owned_group_missing");

    std::set<uint32> const expected(fixture.roster.begin(), fixture.roster.end());
    std::set<uint32> const observed = GroupRosterSet(group);
    Player* leader = ObjectAccessor::FindPlayer(
        ObjectGuid::Create<HighGuid::Player>(fixture.leaderGuid));
    uint32 const leaderMap = leader ? leader->GetMapId() : 0;
    uint32 const leaderInstance = leader ? leader->GetInstanceId() : 0;
    bool allOnline = true;
    bool sameMap = leader != nullptr;
    bool sameInstance = leader != nullptr;
    uint32 rolesAvailable = 0;

    for (uint32 guid : fixture.roster)
    {
        Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        PlayerbotAI* botAI = player ? PlayerbotsMgr::instance().GetPlayerbotAI(player) : nullptr;
        bool const available = player && player->IsInWorld() && botAI && !botAI->IsRealPlayer();
        allOnline = allOnline && available;
        sameMap = sameMap && available && player->GetMapId() == leaderMap;
        sameInstance = sameInstance && available && player->GetInstanceId() == leaderInstance;
        if (available)
            ++rolesAvailable;
    }

    bool const exactRoster = observed == expected && observed.size() == fixture.roster.size();
    bool const exactLeader = group->GetLeaderGUID().GetCounter() == fixture.leaderGuid;
    bool const exactGroupType = group->isRaidGroup() && !group->isBGGroup() && !group->isBFGroup() &&
                                !group->isLFGGroup();
    bool const exactDifficulty = static_cast<uint32>(group->GetRaidDifficulty()) == fixture.difficulty;
    uint32 foreignGuid = 0;
    bool const noForeignMembers = !FindForeignGroupMember(group, expected, foreignGuid);

    std::ostringstream out;
    out << "{\"ok\":true,\"operation\":" << JsonString(operation)
        << ",\"leader_guid\":" << fixture.leaderGuid
        << ",\"group_id\":" << fixture.groupId
        << ",\"group_type\":" << JsonString(GroupTypeName(group))
        << ",\"group_type_mask\":" << static_cast<uint32>(group->GetGroupType())
        << ",\"raid_size\":" << fixture.roster.size()
        << ",\"member_count\":" << group->GetMembersCount()
        << ",\"difficulty\":" << static_cast<uint32>(group->GetRaidDifficulty())
        << ",\"faction\":" << JsonString(TeamName(leader ? leader->GetTeamId() : TEAM_NEUTRAL))
        << ",\"map\":" << leaderMap
        << ",\"instance_id\":" << leaderInstance
        << ",\"roles_available\":" << rolesAvailable
        << ",\"expected_roster\":[";

    for (std::size_t index = 0; index < fixture.roster.size(); ++index)
    {
        if (index)
            out << ',';
        out << fixture.roster[index];
    }

    out << "],\"roster\":[";
    for (std::size_t index = 0; index < fixture.roster.size(); ++index)
    {
        if (index)
            out << ',';
        uint32 const guid = fixture.roster[index];
        Player* player = ObjectAccessor::FindPlayer(ObjectGuid::Create<HighGuid::Player>(guid));
        PlayerbotAI* botAI = player ? PlayerbotsMgr::instance().GetPlayerbotAI(player) : nullptr;
        bool const available = player && player->IsInWorld() && botAI && !botAI->IsRealPlayer();
        bool const tank = available && PlayerbotAI::IsTank(player);
        bool const healer = available && PlayerbotAI::IsHeal(player);
        bool const dps = available && PlayerbotAI::IsDps(player);
        bool const ranged = available && PlayerbotAI::IsRanged(player);

        out << "{\"guid\":" << guid
            << ",\"name\":" << JsonString(player ? player->GetName() : "")
            << ",\"online\":" << (available ? "true" : "false")
            << ",\"in_owned_group\":" << (group->IsMember(ObjectGuid::Create<HighGuid::Player>(guid)) ? "true" : "false")
            << ",\"map\":" << (player ? player->GetMapId() : 0)
            << ",\"instance_id\":" << (player ? player->GetInstanceId() : 0)
            << ",\"role\":" << JsonString(available ? RoleName(player) : "unavailable")
            << ",\"roles\":{\"available\":" << (available ? "true" : "false")
            << ",\"tank\":" << (tank ? "true" : "false")
            << ",\"healer\":" << (healer ? "true" : "false")
            << ",\"dps\":" << (dps ? "true" : "false")
            << ",\"ranged\":" << (ranged ? "true" : "false") << "}}";
    }

    out << "],\"proof\":{\"exact_roster\":" << (exactRoster ? "true" : "false")
        << ",\"exact_leader\":" << (exactLeader ? "true" : "false")
        << ",\"exact_group_type\":" << (exactGroupType ? "true" : "false")
        << ",\"exact_difficulty\":" << (exactDifficulty ? "true" : "false")
        << ",\"no_foreign_members\":" << (noForeignMembers ? "true" : "false")
        << ",\"foreign_guid\":" << foreignGuid
        << ",\"all_online_playerbots\":" << (allOnline ? "true" : "false")
        << ",\"same_map\":" << (sameMap ? "true" : "false")
        << ",\"same_instance\":" << (sameInstance ? "true" : "false") << "}}";
    return out.str();
}
}  // namespace

namespace AutoWowRaid
{
bool IsSupportedRaidSize(std::size_t size)
{
    return std::find(kSupportedRaidSizes.begin(), kSupportedRaidSizes.end(), size) !=
           kSupportedRaidSizes.end();
}

bool ValidateCreateShape(uint32 leaderGuid, std::vector<uint32> const& memberGuids,
                         uint32 difficulty, std::string& error)
{
    if (!IsSupportedRaidSize(memberGuids.size() + 1))
    {
        error = "raid_requires_exactly_10_25_or_40_total_guids";
        return false;
    }
    if (!leaderGuid)
    {
        error = "raid_guids_must_be_positive";
        return false;
    }
    if (difficulty > 1)
    {
        error = "raid_difficulty_must_be_0_or_1";
        return false;
    }

    std::set<uint32> unique = {leaderGuid};
    for (uint32 guid : memberGuids)
    {
        if (!guid)
        {
            error = "raid_guids_must_be_positive";
            return false;
        }
        if (!unique.insert(guid).second)
        {
            error = "raid_guids_must_be_unique";
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

bool ParseWireRequest(std::string const& requestText, WireRequest& request, std::string& error)
{
    std::istringstream input(requestText);
    std::string command;
    std::string subcommand;
    input >> command;
    command = Lowercase(command);

    if (command == "raid")
    {
        input >> subcommand;
        subcommand = Lowercase(subcommand);
    }
    else if (command == "raid-create")
        subcommand = "create";
    else if (command == "raid-status")
        subcommand = "status";
    else if (command == "raid-leave")
        subcommand = "leave";

    if (subcommand != "create" && subcommand != "status" && subcommand != "leave")
    {
        error = "raid requires create, status, or leave";
        return false;
    }

    request = WireRequest{};
    if (subcommand == "status" || subcommand == "leave")
    {
        std::string guidToken;
        std::string trailing;
        if (!(input >> guidToken) || !ParseGuid(guidToken, request.subjectGuid))
        {
            error = "raid " + subcommand + " requires one positive numeric GUID";
            return false;
        }
        if (input >> trailing)
        {
            error = "raid " + subcommand + " accepts exactly one GUID";
            return false;
        }

        request.operation = subcommand == "status" ? WireOperation::Status : WireOperation::Leave;
        error.clear();
        return true;
    }

    std::string leaderToken;
    if (!(input >> leaderToken) || !ParseGuid(leaderToken, request.subjectGuid))
    {
        error = "raid create requires a positive numeric leader GUID";
        return false;
    }

    bool difficultySeen = false;
    std::string token;
    while (input >> token)
    {
        std::string const lowered = Lowercase(token);
        constexpr std::string_view prefix = "difficulty=";
        if (lowered.starts_with(prefix))
        {
            if (difficultySeen || lowered.size() != prefix.size() + 1 ||
                (lowered.back() != '0' && lowered.back() != '1'))
            {
                error = "raid create difficulty must appear once as difficulty=0 or difficulty=1";
                return false;
            }
            request.difficulty = static_cast<uint32>(lowered.back() - '0');
            difficultySeen = true;
            continue;
        }

        if (difficultySeen)
        {
            error = "raid create difficulty must be the final argument";
            return false;
        }

        uint32 guid = 0;
        if (!ParseGuid(token, guid))
        {
            error = "raid create member GUIDs must be positive numeric values";
            return false;
        }
        request.memberGuids.push_back(guid);
    }

    if (!difficultySeen)
    {
        error = "raid create requires difficulty=0 or difficulty=1";
        return false;
    }
    if (!ValidateCreateShape(request.subjectGuid, request.memberGuids, request.difficulty, error))
        return false;

    request.operation = WireOperation::Create;
    error.clear();
    return true;
}

std::string Create(uint32 leaderGuid, std::vector<uint32> const& memberGuids, uint32 difficulty)
{
    std::string shapeError;
    if (!ValidateCreateShape(leaderGuid, memberGuids, difficulty, shapeError))
        return Error("raid_create", shapeError);
    if (fixture.active)
        return Error("raid_create", "raid_fixture_already_owned", fixture.leaderGuid);

    CreateContext context;
    std::string error;
    uint32 errorGuid = 0;
    if (!ResolveCreateContext(leaderGuid, memberGuids, context, error, errorGuid))
        return Error("raid_create", error, errorGuid);

    ObjectGuid const leaderObjectGuid = context.leader->GetGUID();
    GroupInviteOperation firstInvite(leaderObjectGuid, context.members.front()->GetGUID());
    if (!firstInvite.Execute())
    {
        // GroupInviteOperation can create the leader's group before AddMember reports failure.
        // Roll back only that new expected-roster group; never disband a group containing a foreign GUID.
        Group* partial = context.leader->GetGroup();
        uint32 foreignGuid = 0;
        if (partial && partial->GetLeaderGUID() == leaderObjectGuid &&
            !FindForeignGroupMember(partial, context.rosterSet, foreignGuid))
            RollBackCreatedGroup(partial);
        return Error("raid_create", "raid_first_invite_failed", context.members.front()->GetGUID().GetCounter());
    }

    Group* group = context.leader->GetGroup();
    uint32 foreignGuid = 0;
    if (!group || group->GetLeaderGUID() != leaderObjectGuid || group->GetMembersCount() != 2 ||
        FindForeignGroupMember(group, context.rosterSet, foreignGuid))
    {
        RollBackCreatedGroup(group);
        return Error("raid_create", foreignGuid ? "raid_foreign_group_member" : "raid_group_create_verification_failed",
                     foreignGuid);
    }

    GroupConvertToRaidOperation convertToRaid(leaderObjectGuid);
    if (!convertToRaid.Execute() || !group->isRaidGroup())
    {
        RollBackCreatedGroup(group);
        return Error("raid_create", "raid_convert_failed");
    }

    for (std::size_t index = 1; index < context.members.size(); ++index)
    {
        Player* member = context.members[index];
        GroupInviteOperation invite(leaderObjectGuid, member->GetGUID());
        if (!invite.Execute() || member->GetGroup() != group)
        {
            uint32 const failedGuid = member->GetGUID().GetCounter();
            RollBackCreatedGroup(group);
            return Error("raid_create", "raid_member_invite_failed", failedGuid);
        }
    }

    group->SetRaidDifficulty(static_cast<Difficulty>(difficulty));
    if (!VerifyExactCreatedGroup(group, context, difficulty))
    {
        RollBackCreatedGroup(group);
        return Error("raid_create", "raid_exact_roster_verification_failed");
    }

    fixture.active = true;
    fixture.groupId = group->GetGUID().GetCounter();
    fixture.leaderGuid = leaderGuid;
    fixture.difficulty = difficulty;
    fixture.roster = context.roster;
    return BuildStatus("raid_create");
}

std::string Status(uint32 leaderGuid)
{
    if (!leaderGuid)
        return Error("raid_status", "raid_guids_must_be_positive");
    if (!fixture.active)
        return Error("raid_status", "raid_fixture_not_owned");
    if (fixture.leaderGuid != leaderGuid)
        return Error("raid_status", "raid_leader_mismatch", leaderGuid);
    return BuildStatus("raid_status");
}

std::string Leave(uint32 subjectGuid)
{
    if (!subjectGuid)
        return Error("raid_leave", "raid_guids_must_be_positive");
    if (!fixture.active)
        return Error("raid_leave", "raid_fixture_not_owned");

    std::set<uint32> const expected(fixture.roster.begin(), fixture.roster.end());
    if (!expected.contains(subjectGuid))
        return Error("raid_leave", "raid_leave_subject_not_owned", subjectGuid);

    Group* group = OwnedGroup();
    if (!group)
    {
        fixture = FixtureOwnership{};
        return "{\"ok\":true,\"operation\":\"raid_leave\",\"action\":\"group_already_gone\",\"ownership_released\":true}";
    }

    uint32 foreignGuid = 0;
    if (group->GetLeaderGUID().GetCounter() != fixture.leaderGuid || !group->isRaidGroup() ||
        group->isBGGroup() || group->isBFGroup() || group->isLFGGroup())
        return Error("raid_leave", "raid_owned_group_identity_drift");
    if (FindForeignGroupMember(group, expected, foreignGuid))
        return Error("raid_leave", "raid_foreign_group_member", foreignGuid);

    if (subjectGuid == fixture.leaderGuid)
    {
        uint32 const groupId = fixture.groupId;
        group->Disband();
        fixture = FixtureOwnership{};

        std::ostringstream out;
        out << "{\"ok\":true,\"operation\":\"raid_leave\",\"action\":\"disband\""
            << ",\"subject_guid\":" << subjectGuid << ",\"group_id\":" << groupId
            << ",\"ownership_released\":true}";
        return out.str();
    }

    if (!group->IsMember(ObjectGuid::Create<HighGuid::Player>(subjectGuid)))
    {
        std::ostringstream out;
        out << "{\"ok\":true,\"operation\":\"raid_leave\",\"action\":\"already_left\""
            << ",\"subject_guid\":" << subjectGuid << ",\"ownership_released\":false"
            << ",\"status\":" << BuildStatus("raid_status") << '}';
        return out.str();
    }

    uint32 const groupId = fixture.groupId;
    group->RemoveMember(ObjectGuid::Create<HighGuid::Player>(subjectGuid));
    Group* remaining = sGroupMgr->GetGroupByGUID(groupId);
    if (!remaining)
    {
        fixture = FixtureOwnership{};
        std::ostringstream out;
        out << "{\"ok\":true,\"operation\":\"raid_leave\",\"action\":\"member_leave_group_disbanded\""
            << ",\"subject_guid\":" << subjectGuid << ",\"group_id\":" << groupId
            << ",\"ownership_released\":true}";
        return out.str();
    }

    std::ostringstream out;
    out << "{\"ok\":true,\"operation\":\"raid_leave\",\"action\":\"member_leave\""
        << ",\"subject_guid\":" << subjectGuid << ",\"group_id\":" << groupId
        << ",\"ownership_released\":false,\"status\":" << BuildStatus("raid_status") << '}';
    return out.str();
}
}  // namespace AutoWowRaid
