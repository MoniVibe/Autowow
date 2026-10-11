/* AutoWoW original native guild access and contribution eligibility. GPL v2 or later. */
#ifndef AUTOWOW_GUILD_POLICY_H
#define AUTOWOW_GUILD_POLICY_H

#include <algorithm>
#include <cstdint>

namespace AutoWowStrategy
{
enum class GuildBankOperation
{
    View,
    Deposit,
    Withdraw,
    MoveStock,
    SpendMoney
};

struct GuildAccessFacts
{
    bool societyManaged = false;
    std::uint32_t bankGuildId = 0;
    std::uint32_t memberGuildId = 0;
    bool representative = false;
    bool nativeAllowed = false;
};

// Additional restriction for configured society banks only; never replaces native authority.
// Guild masters retain native power and must be enrolled as representatives in managed guild governance.
// Callers route unmanaged banks through the unchanged native path, not this scoped predicate.
inline bool CanUseSocietyGuildBank(GuildAccessFacts const& facts, GuildBankOperation operation)
{
    if (!facts.societyManaged || !facts.bankGuildId || facts.bankGuildId != facts.memberGuildId ||
        !facts.nativeAllowed)
        return false;

    switch (operation)
    {
        case GuildBankOperation::View:
        case GuildBankOperation::Deposit:
            return true;
        case GuildBankOperation::Withdraw:
        case GuildBankOperation::MoveStock:
        case GuildBankOperation::SpendMoney:
            return facts.representative;
        default:
            return false;
    }
}

struct ContributionFacts
{
    std::uint64_t receiptId = 0;
    std::uint64_t contributorGuid = 0;
    std::uint32_t guildIdAtDeposit = 0;
    std::uint32_t recipientGuildId = 0;
    bool confirmed = false;
    bool previouslyRecognized = false;
    bool originalPersonalStock = false;
    bool paidDelivery = false;
    bool returnedOrForwarded = false;
    std::uint64_t deliveredQuantity = 0;
    std::uint64_t demandRemaining = 0;
};

// Eligible units only, not awarded standing, native reputation, rank, XP or payment.
// The repository must validate provenance, receipt uniqueness and demand version atomically.
inline std::uint64_t UsefulContributionUnits(ContributionFacts const& facts)
{
    if (!facts.receiptId || !facts.contributorGuid || !facts.guildIdAtDeposit ||
        facts.guildIdAtDeposit != facts.recipientGuildId ||
        !facts.confirmed || facts.previouslyRecognized || !facts.originalPersonalStock ||
        facts.paidDelivery || facts.returnedOrForwarded)
        return 0;
    return std::min(facts.deliveredQuantity, facts.demandRemaining);
}
} // namespace AutoWowStrategy

#endif
