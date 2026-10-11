/* AutoWoW original custom policy tests. GPL v2 or later. Not part of runtime module source collection. */
#ifdef NDEBUG
#error "Strategy policy tests require assertions enabled; do not accept a vacuous optimized test run"
#endif

#include "StrategyPolicy.h"
#include "TreasuryPolicy.h"

#include <cassert>
#include <limits>

int main()
{
    using namespace AutoWowStrategy;
    auto const empty = PlanIncome(2, 3600, 1800, 4);
    assert(empty.valid && empty.firstPeriod == 0);
    auto const next = PlanIncome(2, 5400, 1800, 4);
    assert(next.valid && next.firstPeriod == 3 && next.lastPeriod == 3 && next.skippedPeriods == 0);
    auto const outage = PlanIncome(2, 36000, 1800, 4);
    assert(outage.valid && outage.firstPeriod == 17 && outage.lastPeriod == 20 && outage.skippedPeriods == 14);
    assert(!PlanIncome(3, 3600, 1800, 4).valid);
    assert(!PlanIncome(0, 1, 0, 4).valid);
    assert(!PlanIncome(0, 1, 1, 0).valid);
    auto const maximum = PlanIncome(std::numeric_limits<std::uint64_t>::max(),
                                     std::numeric_limits<std::uint64_t>::max(), 1, 4);
    assert(maximum.valid && maximum.firstPeriod == 0);

    GloryFacts facts;
    facts.enabled = true;
    facts.hostile = true;
    facts.sameContext = true;
    facts.victimLevel = 70;
    facts.greyLevel = 60;
    auto const decide = [&facts](std::uint32_t pair, std::uint32_t daily,
                                std::uint64_t now = 1000, std::uint64_t last = 0)
    {
        return EvaluateGlory(facts, {}, pair, daily, now, last);
    };
    assert(decide(0, 0).candidatePoints == 10);
    assert(decide(1, 10).candidatePoints == 5);
    assert(decide(2, 15).candidatePoints == 2);
    assert(decide(3, 17).reason == GloryReason::RepeatedOpponent);
    assert(decide(0, 49).candidatePoints == 1);
    assert(decide(0, 50).reason == GloryReason::DailyCap);
    assert(decide(1, 10, 1000, 500).reason == GloryReason::PairCooldown);
    assert(decide(1, 10, 1000, 400).candidatePoints == 5);
    assert(decide(1, 10, 100, 500).reason == GloryReason::ClockBackwards);

    facts.sameAccount = true;
    assert(decide(0, 0).reason == GloryReason::SameAccount);
    facts.bothHeadless = true;
    assert(decide(0, 0).candidatePoints == 10);
    facts.sameAccount = false;
    facts.self = true;
    assert(decide(0, 0).reason == GloryReason::Self);
    facts.self = false;
    facts.hostile = false;
    assert(decide(0, 0).reason == GloryReason::Friendly);
    facts.hostile = true;
    facts.sameContext = false;
    assert(decide(0, 0).reason == GloryReason::Unqualified);
    facts.sameContext = true;
    facts.duel = true;
    assert(decide(0, 0).reason == GloryReason::Duel);
    facts.duel = false;
    facts.arena = true;
    assert(decide(0, 0).reason == GloryReason::Arena);
    facts.arena = false;
    facts.victimLevel = 60;
    assert(decide(0, 0).reason == GloryReason::GreyTarget);
    facts.victimLevel = 70;
    facts.noCredit = true;
    assert(decide(0, 0).reason == GloryReason::NoCredit);
    facts.noCredit = false;
    facts.enabled = false;
    assert(decide(0, 0).reason == GloryReason::Disabled);

    ResourceStock stock;
    auto const produced = PlanProduction(stock, 100, 80);
    assert(produced.accepted && produced.credited == 80 && produced.overflow == 20);
    assert(Conserved(produced.next));
    auto const reserved = PlanTransfer(produced.next, StockMove::ReserveForGuild, 60);
    assert(reserved.accepted && reserved.next.treasury == 20 && reserved.next.reserved == 60);
    auto const partial = PlanTransfer(reserved.next, StockMove::DeliverToGuild, 25);
    assert(partial.accepted && partial.next.reserved == 35 && partial.next.guild == 25);
    assert(!PlanTransfer(partial.next, StockMove::DeliverToGuild, 36).accepted);
    assert(!PlanTransfer(partial.next, StockMove::ReserveForGuild, 21).accepted);
    auto const consumed = PlanTransfer(partial.next, StockMove::ConsumeByWorkshop, 25);
    assert(consumed.accepted && consumed.next.consumed == 25 && Conserved(consumed.next));
    auto const invested = PlanTransfer(consumed.next, StockMove::InvestInTerritory, 20);
    assert(invested.accepted && invested.next.treasury == 0 && invested.next.invested == 20);
    assert(!PlanTransfer(invested.next, StockMove::InvestInTerritory, 1).accepted);
    assert(!PlanTransfer(invested.next, StockMove::ReserveForGuild, 0).accepted);
    assert(!PlanTransfer(invested.next, static_cast<StockMove>(99), 1).accepted);
    stock.treasury = 1;
    assert(!Conserved(stock) && !PlanProduction(stock, 1, 10).accepted);
    stock = {};
    stock.produced = std::numeric_limits<std::uint64_t>::max();
    stock.treasury = stock.produced;
    assert(!PlanProduction(stock, 1, stock.produced).accepted);
    stock.reserved = 1;
    assert(!Conserved(stock));
}
