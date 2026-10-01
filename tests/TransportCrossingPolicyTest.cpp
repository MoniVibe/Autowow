/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TransportCrossingPolicy.h"

#include "gtest/gtest.h"

namespace
{
using namespace AutoWowTransports;
using AutoWowZoneProgression::Mode;

TEST(Transports, ChainLookupBySeqAndTeam)
{
    std::vector<Crossing> const table = DefaultCrossings();
    std::vector<Crossing> const tel = ChainFor(table, 1, 141, 148);
    ASSERT_EQ(tel.size(), 2u);
    EXPECT_EQ(tel[0].via, Via::AreaTrigger);
    EXPECT_EQ(tel[0].object, 527u);
    EXPECT_EQ(tel[1].via, Via::Transport);
    EXPECT_EQ(tel[1].object, 176244u);
    EXPECT_TRUE(ChainFor(table, 2, 141, 148).empty());  // alliance-only rows
    EXPECT_EQ(ChainFor(table, 2, 3433, 267).at(0).object, 184502u);
    EXPECT_TRUE(ChainFor(table, 1, 12, 40).empty());     // Elwynn -> Westfall walks

    // seq order wins over table order; team 0 serves both.
    std::vector<Crossing> t2 = {{0, 1, 2, 1, Via::GameObject, 20}, {0, 1, 2, 0, Via::AreaTrigger, 10}};
    std::vector<Crossing> const c2 = ChainFor(t2, 2, 1, 2);
    ASSERT_EQ(c2.size(), 2u);
    EXPECT_EQ(c2[0].object, 10u);
    EXPECT_EQ(c2[1].object, 20u);
}

TEST(Transports, EveryExtraRouteHasAChain)
{
    std::vector<Crossing> const table = DefaultCrossings();
    for (AutoWowZoneProgression::Route const& r : ExtraRoutes())
    {
        EXPECT_TRUE(r.crossing);
        EXPECT_FALSE(ChainFor(table, r.team, r.from, r.to).empty()) << r.from << "->" << r.to;
    }
    for (Crossing const& c : table)
        if (c.via == Via::Transport)
            EXPECT_TRUE(c.stopX || c.stopY) << c.object;
}

TEST(Transports, StartLegSkipsCrossingsAlreadyBehind)
{
    std::vector<Crossing> const tel = ChainFor(DefaultCrossings(), 1, 141, 148);
    EXPECT_EQ(StartLeg(tel, 1, 9800, 1000, 300), 0u);  // Dolanaar, up the tree
    EXPECT_EQ(StartLeg(tel, 1, 8700, 980, 300), 1u);   // Rut'theran: straight to the boat
    EXPECT_EQ(StartLeg(tel, 0, 8558, 1014, 300), 0u);  // other map: from the start
}

TEST(Transports, ModeFlightThenWalkThenChain)
{
    EXPECT_EQ(SelectMode(true, false, true, true), Mode::Flight);
    EXPECT_EQ(SelectMode(false, true, false, true), Mode::Walk);
    EXPECT_EQ(SelectMode(false, true, true, true), Mode::Chain);
    EXPECT_EQ(SelectMode(false, false, false, true), Mode::Chain);
    EXPECT_EQ(SelectMode(false, false, true, false), Mode::Unreachable);
}

TEST(Transports, ObjectStep)
{
    Obs o;
    EXPECT_EQ(NextObjectStep(o), Step::Approach);
    o.atApproach = true;
    EXPECT_EQ(NextObjectStep(o), Step::Use);
    o.atExit = true;
    EXPECT_EQ(NextObjectStep(o), Step::Done);
}

TEST(Transports, BoardingHappyPath)
{
    Obs o;
    Step s = NextTransportStep(Step::Approach, o);
    EXPECT_EQ(s, Step::Approach);
    o.atApproach = true;
    s = NextTransportStep(s, o);
    EXPECT_EQ(s, Step::Wait);
    o.dockedHere = true;
    s = NextTransportStep(s, o);
    EXPECT_EQ(s, Step::Board);
    o.atApproach = false;  // walking onto the deck leaves the dock point: still boarding
    s = NextTransportStep(s, o);
    EXPECT_EQ(s, Step::Board);
    o.onTransport = true;
    o.dockedHere = false;
    s = NextTransportStep(s, o);
    EXPECT_EQ(s, Step::Ride);
    s = NextTransportStep(s, o);
    EXPECT_EQ(s, Step::Ride);
    o.dockedExit = true;
    s = NextTransportStep(s, o);
    EXPECT_EQ(s, Step::Disembark);
    o.onTransport = false;
    o.atExit = true;
    s = NextTransportStep(s, o);
    EXPECT_EQ(s, Step::Done);
    EXPECT_EQ(NextTransportStep(Step::Done, Obs{}), Step::Done);
}

TEST(Transports, BoardingFailures)
{
    Obs o;
    o.atApproach = false;
    // Missed the ship while boarding: back to waiting, then (off the dock point) re-approach.
    EXPECT_EQ(NextTransportStep(Step::Board, o), Step::Wait);
    EXPECT_EQ(NextTransportStep(Step::Wait, o), Step::Approach);
    // Fell off mid-sea: restart the crossing; stepped off on the exit side: done.
    EXPECT_EQ(NextTransportStep(Step::Ride, o), Step::Approach);
    o.atExit = true;
    EXPECT_EQ(NextTransportStep(Step::Ride, o), Step::Done);
    // Ship left before we stepped off: keep riding (it comes back).
    Obs aboard;
    aboard.onTransport = true;
    EXPECT_EQ(NextTransportStep(Step::Disembark, aboard), Step::Ride);
    EXPECT_TRUE(FailedRide(Step::Ride, Step::Approach, false));
    EXPECT_TRUE(FailedRide(Step::Wait, Step::Approach, true));
    EXPECT_FALSE(FailedRide(Step::Wait, Step::Approach, false));
    EXPECT_FALSE(FailedRide(Step::Ride, Step::Done, false));
}

TEST(Transports, StuckStepExceptRide)
{
    Params p;
    p.stepTimeoutMs = 1000;
    EXPECT_FALSE(StepStuck(p, Step::Wait, 100, 1100));
    EXPECT_TRUE(StepStuck(p, Step::Wait, 100, 1101));
    EXPECT_FALSE(StepStuck(p, Step::Ride, 100, 999999));
    EXPECT_FALSE(StepStuck(p, Step::Wait, 5000, 100));  // clock behind: never stuck
}

TEST(Transports, PortalModeSelection)
{
    TransportMode m = TransportMode::Real;
    EXPECT_TRUE(ParseMode("portal", m));
    EXPECT_EQ(m, TransportMode::Portal);
    EXPECT_FALSE(ParseMode("teleport", m));
    EXPECT_EQ(m, TransportMode::Portal);  // untouched

    std::vector<ModeOverride> ov;
    ASSERT_TRUE(ParseModeOverrides("176244=portal; 175080=real", ov));
    ASSERT_EQ(ov.size(), 2u);
    EXPECT_EQ(ModeFor(ov, TransportMode::Auto, 176244), TransportMode::Portal);
    EXPECT_EQ(ModeFor(ov, TransportMode::Auto, 175080), TransportMode::Real);
    EXPECT_EQ(ModeFor(ov, TransportMode::Auto, 181646), TransportMode::Auto);
    EXPECT_FALSE(ParseModeOverrides("176244", ov));
    EXPECT_FALSE(ParseModeOverrides("abc=real", ov));
    EXPECT_FALSE(ParseModeOverrides("0=real", ov));
    EXPECT_EQ(ov.size(), 2u);  // untouched

    EXPECT_FALSE(UsePortal(TransportMode::Real, 99, 2));
    EXPECT_TRUE(UsePortal(TransportMode::Portal, 0, 2));
    EXPECT_FALSE(UsePortal(TransportMode::Auto, 1, 2));
    EXPECT_TRUE(UsePortal(TransportMode::Auto, 2, 2));
}

TEST(Transports, ParseCrossings)
{
    std::vector<Crossing> out = DefaultCrossings();
    std::size_t const n = out.size();
    EXPECT_FALSE(ParseCrossings("1,141,148,0,0,527", out));                                   // short
    EXPECT_FALSE(ParseCrossings("1,141,148,0,3,527,1,0,0,0,0,0,1,0,0,0,0,0", out));           // bad via
    EXPECT_FALSE(ParseCrossings("1,141,148,0,0,0,1,0,0,0,0,0,1,0,0,0,0,0", out));             // object 0
    EXPECT_EQ(out.size(), n);
    ASSERT_TRUE(ParseCrossings(" 2,17,33,0,2,175080,1,1354,-4643,54,1361,-4631,0,-12441,215,31,-12464,232;", out));
    ASSERT_EQ(out.size(), 1u);
    EXPECT_EQ(out[0].via, Via::Transport);
    EXPECT_EQ(out[0].x, 1354);
    EXPECT_EQ(out[0].y, -4643);
    EXPECT_EQ(out[0].exitMap, 0u);
    EXPECT_EQ(out[0].exitStopY, 232);
}

TEST(Transports, LegLogAndLedgerField)
{
    LegLog log;
    log.Open(Leg::Walk, 1000);
    log.Open(Leg::Walk, 1500);  // reissued walk stays one leg
    log.Open(Leg::TravelObject, 4000);
    log.Open(Leg::Walk, 4100);
    log.Open(Leg::Transport, 9000);
    log.Open(Leg::Portal, 9500);
    log.Open(Leg::Walk, 9600);
    log.Close(20000);
    EXPECT_EQ(LegsField(log), ",\"legs\":[[\"walk\",3000],[\"travel_object\",100],[\"walk\",4900],"
                              "[\"transport\",500],[\"portal\",100],[\"walk\",10400]]");
    EXPECT_EQ(LegsField(LegLog{}), ",\"legs\":[]");

    LegLog full;
    for (std::uint32_t k = 0; k < kMaxLegs + 3; ++k)
        full.Open(k % 2 ? Leg::Walk : Leg::Transport, k * 10);
    EXPECT_EQ(full.count, kMaxLegs);
}

// AutoWow.ZoneProgression.Outland: every Outland entry route has a chain that starts on the source continent,
// runs in seq order and ends through the Dark Portal into map 530.
TEST(Transports, EveryOutlandEntryRouteEndsAtTheDarkPortal)
{
    std::vector<Crossing> const table = OutlandCrossings();
    std::size_t entries = 0;
    for (AutoWowZoneProgression::Route const& r : AutoWowZoneProgression::OutlandRoutes())
    {
        if (!AutoWowZoneProgression::IsOutlandEntry(r))
            continue;
        ++entries;
        std::vector<Crossing> const chain = ChainFor(table, r.team, r.from, r.to);
        ASSERT_FALSE(chain.empty()) << r.team << " " << r.from;
        for (std::uint32_t k = 0; k < chain.size(); ++k)
        {
            EXPECT_EQ(chain[k].seq, k);
            if (k)
            {
                EXPECT_EQ(chain[k].map, chain[k - 1].exitMap);  // each leg starts where the last one landed
            }
        }
        EXPECT_EQ(chain.back().via, Via::AreaTrigger);
        EXPECT_EQ(chain.back().object, 4354u);
        EXPECT_EQ(chain.back().exitMap, 530u);
        for (Crossing const& x : chain)
            EXPECT_EQ(x.team, r.team);
    }
    EXPECT_GT(entries, 30u);

    // Horde Silithus: Orgrimmar portal, Dark Portal. Alliance Silithus: Moonspray, Rut'theran trigger,
    // Darnassus portal, Dark Portal. Alliance EPL: Ironforge portal first. Horde EPL / Alliance Duskwood: walk.
    std::vector<Crossing> c = ChainFor(table, 2, 1377, 3483);
    ASSERT_EQ(c.size(), 2u);
    EXPECT_EQ(c[0].object, 195142u);
    EXPECT_EQ(c[0].map, 1u);
    c = ChainFor(table, 1, 1377, 3483);
    ASSERT_EQ(c.size(), 4u);
    EXPECT_EQ(c[0].via, Via::Transport);
    EXPECT_EQ(c[0].object, 176244u);
    EXPECT_EQ(c[1].object, 542u);
    EXPECT_EQ(c[2].object, 195141u);
    EXPECT_EQ(ChainFor(table, 1, 1657, 3483).size(), 2u);  // Darnassus: portal, Dark Portal
    c = ChainFor(table, 1, 139, 3483);
    ASSERT_EQ(c.size(), 2u);
    EXPECT_EQ(c[0].object, 195141u);
    EXPECT_EQ(c[0].map, 0u);
    EXPECT_EQ(ChainFor(table, 2, 139, 3483).size(), 1u);
    EXPECT_EQ(ChainFor(table, 1, 10, 3483).size(), 1u);
    // A Darnassus bot standing at the portal joins the chain there.
    EXPECT_EQ(StartLeg(ChainFor(table, 1, 1377, 3483), 1, 9660, 2505, 300), 2u);
    // The Outland chain uses the crossing machine unchanged: flight still wins, else chain.
    EXPECT_EQ(SelectMode(false, false, true, true), Mode::Chain);
}

TEST(Transports, DarkPortalArrivalSpecsAreFactionExact)
{
    ArrivalSpec const* horde = ArrivalForTeam(2);
    ArrivalSpec const* alliance = ArrivalForTeam(1);
    ASSERT_NE(horde, nullptr);
    ASSERT_NE(alliance, nullptr);
    EXPECT_EQ(horde->npc, 18930u);
    EXPECT_EQ(horde->menu, 7938u);
    EXPECT_EQ(horde->spell, 34924u);
    EXPECT_TRUE(NativeArrivalPathMatches(*horde, 565, 130, 99));
    EXPECT_FALSE(NativeArrivalPathMatches(*horde, 564, 129, 100));
    EXPECT_EQ(alliance->npc, 18931u);
    EXPECT_EQ(alliance->menu, 7939u);
    EXPECT_EQ(alliance->spell, 34907u);
    EXPECT_TRUE(NativeArrivalPathMatches(*alliance, 564, 129, 100));
    EXPECT_FALSE(NativeArrivalPathMatches(*alliance, 565, 130, 99));
    EXPECT_EQ(ArrivalForTeam(0), nullptr);
    EXPECT_EQ(ArrivalForTeam(3), nullptr);
}

TEST(Transports, DarkPortalArrivalOwnershipIsNarrow)
{
    EXPECT_TRUE(RequiresArrivalService(true, false, true, 4354));
    EXPECT_FALSE(RequiresArrivalService(false, false, true, 4354));
    EXPECT_FALSE(RequiresArrivalService(true, true, true, 4354));
    EXPECT_FALSE(RequiresArrivalService(true, false, false, 4354));
    EXPECT_FALSE(RequiresArrivalService(true, false, true, 527));

    ArrivalSpec const& horde = kHordeArrival;
    EXPECT_TRUE(AtArrivalSource(horde, 530, -178, 1027, 54.19f));
    EXPECT_FALSE(AtArrivalSource(horde, 0, -178, 1027, 54.19f));
    EXPECT_FALSE(AtArrivalSource(horde, 530, -178, 1027, 60.0f));
    EXPECT_FALSE(AtArrivalSource(horde, 530, -248, 922, 84.0f));
    EXPECT_TRUE(AtDarkPortalExit(horde, 530, -248, 922, 84));
    EXPECT_TRUE(AtDarkPortalExit(horde, 530, -248, 970, 84));
    EXPECT_FALSE(AtDarkPortalExit(horde, 530, -248, 922, 90));
    EXPECT_FALSE(AtDarkPortalExit(horde, 530, -248, 922, 89.1f));
    EXPECT_FALSE(AtDarkPortalExit(horde, 530, -248, 922, 145));
    EXPECT_FALSE(AtDarkPortalExit(horde, 0, -248, 922, 84));
    EXPECT_TRUE(OwnsArrivalPosition(horde, 530, -248, 922, 84.0f));
    EXPECT_TRUE(OwnsArrivalPosition(horde, 530, -178, 1027, 54.19f));
    EXPECT_FALSE(OwnsArrivalPosition(horde, 530, -248, 922, 90.0f));
}

TEST(Transports, DarkPortalPreparedRideOrNativeBypass)
{
    ArrivalSpec const& horde = kHordeArrival;
    EXPECT_EQ(SelectArrivalRide(horde, true, 7938, 1, 1, false), ArrivalRide::PreparedMenu);
    EXPECT_EQ(SelectArrivalRide(horde, false, 7938, 1, 1, false), ArrivalRide::NativePath);  // stale sender
    EXPECT_EQ(SelectArrivalRide(horde, true, 7939, 1, 1, false), ArrivalRide::NativePath);  // wrong faction menu
    EXPECT_EQ(SelectArrivalRide(horde, true, 7938, 0, 4, false), ArrivalRide::NativePath);  // rewarded/taxi menu
    EXPECT_EQ(SelectArrivalRide(horde, true, 7938, 1, 1, true), ArrivalRide::NativePath);   // coded/invalid
    EXPECT_EQ(SelectArrivalRide(horde, true, 7938, 99, 0, false), ArrivalRide::NativePath); // option absent
}

TEST(Transports, DarkPortalFlightNeedsStartAndPhysicalLanding)
{
    ArrivalSpec const& horde = kHordeArrival;
    EXPECT_TRUE(MatchingArrivalFlight(horde, true, 130, 99));
    EXPECT_FALSE(MatchingArrivalFlight(horde, false, 130, 99));
    EXPECT_FALSE(MatchingArrivalFlight(horde, true, 129, 99));
    EXPECT_FALSE(MatchingArrivalFlight(horde, true, 130, 100));

    EXPECT_TRUE(AtArrivalLanding(horde, 530, 229, 2634, 88));
    EXPECT_TRUE(AtArrivalLanding(horde, 530, 275, 2634, 88));
    // Exact final TaxiPathNode.dbc points for the scripted intro paths remain inside the
    // TaxiNodes.dbc destination receipt (565 -> 99 and 564 -> 100 respectively).
    EXPECT_TRUE(AtArrivalLanding(horde, 530, 227.18196f, 2634.1853f, 89.713875f));
    EXPECT_TRUE(AtArrivalLanding(kAllianceArrival, 530, -676.94794f, 2716.6277f, 95.62141f));
    EXPECT_FALSE(AtArrivalLanding(horde, 530, 229, 2634, 140));  // same XY, 52 yd above the node
    EXPECT_FALSE(AtArrivalLanding(horde, 0, 229, 2634, 88));
    EXPECT_FALSE(AtArrivalLanding(kAllianceArrival, 530, 229, 2634, 88));
}

TEST(Transports, DarkPortalArrivalReceiptFlowIsFactionSymmetric)
{
    for (ArrivalSpec const* spec : {&kHordeArrival, &kAllianceArrival})
    {
        EXPECT_EQ(EvaluateArrivalReceipt(*spec, ArrivalPhase::AwaitFlightStart, false, false,
                                         spec->sourceNode, spec->destinationNode, spec->map,
                                         spec->sourceX, spec->sourceY, spec->serviceZ), ArrivalReceipt::None);
        EXPECT_EQ(EvaluateArrivalReceipt(*spec, ArrivalPhase::AwaitFlightStart, false, true,
                                         spec->sourceNode + 1, spec->destinationNode, spec->map,
                                         spec->sourceX, spec->sourceY, spec->serviceZ), ArrivalReceipt::None);
        EXPECT_EQ(EvaluateArrivalReceipt(*spec, ArrivalPhase::AwaitFlightStart, false, true,
                                         spec->sourceNode, spec->destinationNode, spec->map,
                                         spec->sourceX, spec->sourceY, spec->serviceZ), ArrivalReceipt::FlightStarted);
        EXPECT_EQ(EvaluateArrivalReceipt(*spec, ArrivalPhase::AwaitFlightEnd, true, true,
                                         spec->sourceNode, spec->destinationNode, spec->map,
                                         spec->sourceX, spec->sourceY, spec->serviceZ), ArrivalReceipt::FlightActive);
        EXPECT_EQ(EvaluateArrivalReceipt(*spec, ArrivalPhase::AwaitFlightEnd, true, false, 0, 0, spec->map,
                                         spec->destinationX, spec->destinationY, spec->destinationZ),
                  ArrivalReceipt::Landed);
        EXPECT_EQ(EvaluateArrivalReceipt(*spec, ArrivalPhase::AwaitFlightEnd, true, false, 0, 0,
                                         spec->map + 1, spec->destinationX, spec->destinationY,
                                         spec->destinationZ), ArrivalReceipt::None);
        EXPECT_EQ(EvaluateArrivalReceipt(*spec, ArrivalPhase::AwaitFlightEnd, true, false, 0, 0, spec->map,
                                         spec->destinationX, spec->destinationY, spec->destinationZ + 52.0f),
                  ArrivalReceipt::None);
    }
}

TEST(Transports, DarkPortalArrivalExcludedTimeAndTerminalWalk)
{
    ChainState state;
    state.arrivalPhase = ArrivalPhase::AwaitFlightStart;
    state.arrivalStepAt = 1000;
    ASSERT_TRUE(HoldArrivalTimer(state, 1500));
    EXPECT_FALSE(HoldArrivalTimer(state, 2000));
    EXPECT_EQ(ResumeArrivalTimer(state, 11500), 10000u);
    EXPECT_EQ(state.arrivalStepAt, 11000u);
    EXPECT_EQ(state.arrivalHeldAt, 0u);

    EXPECT_FALSE(TerminalWalkAllowed(true, ArrivalPhase::None));
    EXPECT_FALSE(TerminalWalkAllowed(true, ArrivalPhase::AwaitFlightStart));
    EXPECT_FALSE(TerminalWalkAllowed(true, ArrivalPhase::Failed));
    EXPECT_TRUE(TerminalWalkAllowed(true, ArrivalPhase::Complete));
    EXPECT_TRUE(TerminalWalkAllowed(false, ArrivalPhase::Failed));  // unrelated chains remain unchanged
}
}  // namespace
