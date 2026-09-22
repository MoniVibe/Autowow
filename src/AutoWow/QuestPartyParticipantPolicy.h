/*
 * Pure participant-set policy for an explicit quest directive.
 *
 * A party may escort a questing leader without every member owning the quest. Only online,
 * alive Playerbots with the exact active quest are participants: they may receive the durable
 * quest directive and contribute to quest postconditions. Other valid Playerbots remain
 * nonparticipants and may follow, but never provide quest proof. This policy does not grant,
 * share, complete, abandon, or otherwise mutate a quest.
 */
#ifndef PLAYERBOTS_AUTOWOW_QUEST_PARTY_PARTICIPANT_POLICY_H
#define PLAYERBOTS_AUTOWOW_QUEST_PARTY_PARTICIPANT_POLICY_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace AutoWowQuestParty
{
enum class MemberDisposition : std::uint8_t
{
    InvalidRoster,
    Participant,
    NonParticipant
};

enum class Decision : std::uint8_t
{
    RejectRoster,
    RejectLeader,
    ExecuteParticipants,
    AlreadyRewarded
};

struct MemberFacts
{
    bool leader = false;
    bool present = false;
    bool online = false;
    bool playerbot = false;
    bool alive = false;
    bool exactQuestInLog = false;
    bool rewarded = false;
    bool statusAllowed = false;
};

struct ParticipantPlan
{
    Decision decision = Decision::RejectRoster;
    std::size_t participantCount = 0;
    std::size_t nonParticipantCount = 0;
    std::size_t invalidRosterCount = 0;
    bool leaderParticipates = false;
    bool leaderRewarded = false;
};

[[nodiscard]] inline constexpr bool IsParticipant(MemberFacts const& facts) noexcept
{
    return facts.present && facts.online && facts.playerbot && facts.alive &&
        facts.exactQuestInLog && !facts.rewarded && facts.statusAllowed;
}

[[nodiscard]] inline constexpr MemberDisposition Classify(MemberFacts const& facts) noexcept
{
    if (!facts.present || !facts.playerbot)
        return MemberDisposition::InvalidRoster;
    return IsParticipant(facts) ? MemberDisposition::Participant : MemberDisposition::NonParticipant;
}

[[nodiscard]] inline ParticipantPlan BuildPlan(std::vector<MemberFacts> const& members) noexcept
{
    ParticipantPlan plan;
    bool leaderFound = false;

    for (MemberFacts const& member : members)
    {
        if (member.leader)
        {
            leaderFound = true;
            plan.leaderRewarded = member.rewarded;
        }

        switch (Classify(member))
        {
            case MemberDisposition::InvalidRoster:
                ++plan.invalidRosterCount;
                break;
            case MemberDisposition::Participant:
                ++plan.participantCount;
                plan.leaderParticipates = plan.leaderParticipates || member.leader;
                break;
            case MemberDisposition::NonParticipant:
                ++plan.nonParticipantCount;
                break;
        }
    }

    if (members.empty() || !leaderFound || plan.invalidRosterCount != 0)
    {
        plan.decision = Decision::RejectRoster;
        return plan;
    }

    // A terminal leader request is an idempotent no-op. It must not prime followers or use them as
    // evidence for a quest the leader has already rewarded.
    if (plan.leaderRewarded && !plan.leaderParticipates)
    {
        plan.decision = Decision::AlreadyRewarded;
        return plan;
    }

    if (!plan.leaderParticipates)
    {
        plan.decision = Decision::RejectLeader;
        return plan;
    }

    plan.decision = Decision::ExecuteParticipants;
    return plan;
}

[[nodiscard]] inline constexpr std::string_view DecisionName(Decision decision) noexcept
{
    switch (decision)
    {
        case Decision::RejectRoster: return "reject_roster";
        case Decision::RejectLeader: return "reject_leader";
        case Decision::ExecuteParticipants: return "execute_participants";
        case Decision::AlreadyRewarded: return "already_rewarded";
        default: return "unknown";
    }
}
} // namespace AutoWowQuestParty

#endif // PLAYERBOTS_AUTOWOW_QUEST_PARTY_PARTICIPANT_POLICY_H
