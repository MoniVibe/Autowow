/* AutoWoW native guild policy tests. GPL v2 or later. Not collected as runtime source. */
#ifdef NDEBUG
#error "Guild policy tests require assertions enabled"
#endif

#include "GuildPolicy.h"

#include <cassert>
#include <limits>

int main()
{
    using namespace AutoWowStrategy;
    GuildAccessFacts member{true, 11, 11, false, true};
    assert(CanUseSocietyGuildBank(member, GuildBankOperation::View));
    assert(CanUseSocietyGuildBank(member, GuildBankOperation::Deposit));
    assert(!CanUseSocietyGuildBank(member, GuildBankOperation::Withdraw));
    assert(!CanUseSocietyGuildBank(member, GuildBankOperation::MoveStock));
    assert(!CanUseSocietyGuildBank(member, GuildBankOperation::SpendMoney));
    member.representative = true;
    assert(CanUseSocietyGuildBank(member, GuildBankOperation::Withdraw));
    assert(CanUseSocietyGuildBank(member, GuildBankOperation::MoveStock));
    assert(CanUseSocietyGuildBank(member, GuildBankOperation::SpendMoney));
    member.nativeAllowed = false;
    assert(!CanUseSocietyGuildBank(member, GuildBankOperation::Withdraw));
    assert(!CanUseSocietyGuildBank(member, GuildBankOperation::Deposit));
    member.nativeAllowed = true;
    member.memberGuildId = 12;
    assert(!CanUseSocietyGuildBank(member, GuildBankOperation::Deposit));
    assert(!CanUseSocietyGuildBank(member, GuildBankOperation::Withdraw));
    member.memberGuildId = 0;
    assert(!CanUseSocietyGuildBank(member, GuildBankOperation::View));
    member.memberGuildId = 11;
    member.societyManaged = false;
    assert(!CanUseSocietyGuildBank(member, GuildBankOperation::Deposit));
    member.societyManaged = true;
    member.bankGuildId = 0;
    assert(!CanUseSocietyGuildBank(member, GuildBankOperation::Deposit));
    member.bankGuildId = 11;
    assert(!CanUseSocietyGuildBank(member, static_cast<GuildBankOperation>(99)));

    ContributionFacts contribution{1, 1001, 11, 11, true, false, true, false, false, 50, 20};
    assert(UsefulContributionUnits(contribution) == 20);
    contribution.demandRemaining = 100;
    assert(UsefulContributionUnits(contribution) == 50);
    contribution.demandRemaining = 0;
    assert(UsefulContributionUnits(contribution) == 0);
    contribution.demandRemaining = 20;
    contribution.previouslyRecognized = true;
    assert(UsefulContributionUnits(contribution) == 0);
    contribution.previouslyRecognized = false;
    contribution.confirmed = false;
    assert(UsefulContributionUnits(contribution) == 0);
    contribution.confirmed = true;
    contribution.originalPersonalStock = false;
    assert(UsefulContributionUnits(contribution) == 0);
    contribution.originalPersonalStock = true;
    contribution.paidDelivery = true;
    assert(UsefulContributionUnits(contribution) == 0);
    contribution.paidDelivery = false;
    contribution.returnedOrForwarded = true;
    assert(UsefulContributionUnits(contribution) == 0);
    contribution.returnedOrForwarded = false;
    contribution.recipientGuildId = 12;
    assert(UsefulContributionUnits(contribution) == 0);
    contribution.recipientGuildId = 11;
    contribution.guildIdAtDeposit = 0;
    assert(UsefulContributionUnits(contribution) == 0);
    contribution.guildIdAtDeposit = 11;
    contribution.contributorGuid = 0;
    assert(UsefulContributionUnits(contribution) == 0);
    contribution.contributorGuid = 1001;
    contribution.receiptId = 0;
    assert(UsefulContributionUnits(contribution) == 0);
    contribution.receiptId = 1;
    contribution.deliveredQuantity = 0;
    assert(UsefulContributionUnits(contribution) == 0);
    contribution.deliveredQuantity = std::numeric_limits<std::uint64_t>::max();
    contribution.demandRemaining = contribution.deliveredQuantity;
    assert(UsefulContributionUnits(contribution) == contribution.deliveredQuantity);
}
