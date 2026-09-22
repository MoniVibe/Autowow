#include "../src/AutoWow/AutoWowGuildTradeControl.h"

#include "gtest/gtest.h"

using namespace AutoWowGuildTradeControl;

namespace
{
GuardFacts ValidFacts()
{
    GuardFacts facts;
    facts.sellerOnline = facts.buyerOnline = true;
    facts.sellerPlayerbot = facts.buyerPlayerbot = true;
    facts.sellerAlive = facts.buyerAlive = true;
    facts.sameFaction = facts.nearby = true;
    facts.sellerGuildId = 2;
    facts.buyerGuildId = 3;
    facts.itemExists = facts.itemEntryMatches = facts.itemTradable = true;
    facts.stackQuantity = 1;
    facts.buyerCanStore = true;
    facts.sellerCanReceiveMoney = true;
    facts.buyerMoney = 1000;
    return facts;
}
}

TEST(AutoWowGuildTradeControlTest, ExactDifferentGuildTradeIsAdmitted)
{
    EXPECT_TRUE(GuardError(ValidFacts(), 2, 26, 603762, 33470, 1, 100).empty());
}

TEST(AutoWowGuildTradeControlTest, RejectsSameGuildProxyAndNonExactStack)
{
    GuardFacts sameGuild = ValidFacts();
    sameGuild.buyerGuildId = sameGuild.sellerGuildId;
    EXPECT_EQ(GuardError(sameGuild, 2, 8, 603762, 33470, 1, 100),
              "different_actual_guilds_required");

    GuardFacts splitStack = ValidFacts();
    splitStack.stackQuantity = 2;
    EXPECT_EQ(GuardError(splitStack, 2, 26, 603762, 33470, 1, 100),
              "exact_whole_stack_quantity_required");
}

TEST(AutoWowGuildTradeControlTest, RejectsUnsafePriceAndRuntimeState)
{
    EXPECT_EQ(GuardError(ValidFacts(), 2, 26, 603762, 33470, 1, 501),
              "price_out_of_bounds");
    GuardFacts combat = ValidFacts();
    combat.buyerInCombat = true;
    EXPECT_EQ(GuardError(combat, 2, 26, 603762, 33470, 1, 100), "in_combat");
    GuardFacts remote = ValidFacts();
    remote.nearby = false;
    EXPECT_EQ(GuardError(remote, 2, 26, 603762, 33470, 1, 100), "not_nearby");
}

TEST(AutoWowGuildTradeControlTest, ReceiptRequiresItemIdentityAndBothExactMoneyDeltas)
{
    ReceiptFacts exact{true, true, 1000, 1100, 800, 700, 100};
    EXPECT_TRUE(ReceiptComplete(exact));
    ReceiptFacts wrongItem = exact;
    wrongItem.exactItemOwnedByBuyer = false;
    EXPECT_FALSE(ReceiptComplete(wrongItem));
    ReceiptFacts wrongSellerMoney = exact;
    wrongSellerMoney.sellerMoneyAfter = 1099;
    EXPECT_FALSE(ReceiptComplete(wrongSellerMoney));
    ReceiptFacts wrongBuyerMoney = exact;
    wrongBuyerMoney.buyerMoneyAfter = 699;
    EXPECT_FALSE(ReceiptComplete(wrongBuyerMoney));
}
