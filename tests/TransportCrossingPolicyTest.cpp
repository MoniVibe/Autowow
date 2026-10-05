/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TransportCrossingPolicy.h"

#include <cmath>
#include <limits>

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

TEST(Transports, NorthrendPassagesKeepExactNativeIdentityAndFractionalAnchors)
{
    EXPECT_EQ(kStormwindValiance.passage, NorthrendPassageId::StormwindValiance);
    EXPECT_EQ(kStormwindValiance.entry, 190536U);
    EXPECT_EQ(kStormwindValiance.taxiPath, 965U);
    EXPECT_EQ(kStormwindValiance.sourceStop, 4U);
    EXPECT_EQ(kStormwindValiance.destinationStop, 16U);
    EXPECT_FLOAT_EQ(kStormwindValiance.sourceX, -8302.65f);
    EXPECT_FLOAT_EQ(kStormwindValiance.destinationY, 5129.31f);

    EXPECT_EQ(kOrgrimmarWarsong.passage, NorthrendPassageId::OrgrimmarWarsong);
    EXPECT_EQ(kOrgrimmarWarsong.entry, 186238U);
    EXPECT_EQ(kOrgrimmarWarsong.taxiPath, 712U);
    EXPECT_EQ(kOrgrimmarWarsong.sourceStop, 19U);
    EXPECT_EQ(kOrgrimmarWarsong.destinationStop, 4U);
    EXPECT_FLOAT_EQ(kOrgrimmarWarsong.sourceX, 1174.13f);
    EXPECT_FLOAT_EQ(kOrgrimmarWarsong.destinationZ, 122.207f);

    EXPECT_EQ(kGromgolOrgrimmar.passage, NorthrendPassageId::None);
    EXPECT_EQ(kGromgolOrgrimmar.entry, 175080U);
    EXPECT_EQ(kGromgolOrgrimmar.taxiPath, 285U);
    EXPECT_EQ(kGromgolOrgrimmar.sourceStop, 3U);
    EXPECT_EQ(kGromgolOrgrimmar.destinationStop, 14U);
    EXPECT_EQ(kGromgolOrgrimmar.sourceMap, 0U);
    EXPECT_EQ(kGromgolOrgrimmar.destinationMap, 1U);
    EXPECT_FLOAT_EQ(kGromgolOrgrimmar.sourceX, -12441.0f);
    EXPECT_FLOAT_EQ(kGromgolOrgrimmar.destinationZ, 54.0f);
}

TEST(Transports, NorthrendChainsOwnEveryNativePrefixInOrder)
{
    std::vector<Crossing> const table = NorthrendCrossings();
    std::vector<Crossing> alliance = ChainFor(table, 1, 3523, AutoWowZoneProgression::kBoreanZone);
    ASSERT_EQ(alliance.size(), 2U);
    EXPECT_EQ(alliance[0].via, Via::AreaTrigger);
    EXPECT_EQ(alliance[0].object, 4352U);
    EXPECT_EQ(alliance[1].object, 190536U);

    std::vector<Crossing> horde = ChainFor(table, 2, 3523, AutoWowZoneProgression::kBoreanZone);
    ASSERT_EQ(horde.size(), 3U);
    EXPECT_EQ(horde[0].object, 4352U);
    EXPECT_EQ(horde[1].object, 175080U);
    EXPECT_EQ(horde[1].map, 0U);
    EXPECT_EQ(horde[1].exitMap, 1U);
    EXPECT_EQ(horde[2].object, 186238U);

    EXPECT_EQ(ChainFor(table, 1, AutoWowZoneProgression::kStormwindZone,
                       AutoWowZoneProgression::kBoreanZone).size(), 1U);
    EXPECT_EQ(ChainFor(table, 2, AutoWowZoneProgression::kOrgrimmarZone,
                       AutoWowZoneProgression::kBoreanZone).size(), 1U);

    for (std::uint32_t team : {1U, 2U})
        for (AutoWowZoneProgression::HubSource const& source :
             AutoWowZoneProgression::NorthrendEntryZones(team))
        {
            std::vector<Crossing> const chain =
                ChainFor(table, team, source.zone, AutoWowZoneProgression::kBoreanZone);
            ASSERT_FALSE(chain.empty()) << team << " " << source.zone;
            EXPECT_EQ(chain.size(), source.map == AutoWowZoneProgression::kOutlandMap ? (team == 1 ? 2U : 3U) : 1U);
        }
}

TEST(Transports, NorthrendOutlandAreaTriggerApproachIsInsideCanonicalNativeTrigger)
{
    // Live acore_world.areatrigger entry 4352, loaded by ObjectMgr and checked by
    // Player::IsInAreaTriggerRadius. Orientation is zero, so the core compares
    // length on X and width on Y. The old Y=922 point was outside the narrow width.
    constexpr float centerX = -247.677f;
    constexpr float centerY = 895.675f;
    constexpr float centerZ = 84.3622f;
    constexpr float halfLength = 72.83f / 2.0f;
    constexpr float halfWidth = 6.611f / 2.0f;
    constexpr float halfHeight = 53.81f / 2.0f;
    auto inside = [=](float x, float y, float z)
    {
        return std::fabs(x - centerX) <= halfLength && std::fabs(y - centerY) <= halfWidth &&
               std::fabs(z - centerZ) <= halfHeight;
    };

    EXPECT_FALSE(inside(-248.0f, 922.0f, 84.0f));
    EXPECT_TRUE(inside(-248.0f, 896.0f, 84.0f));

    std::vector<Crossing> const table = NorthrendCrossings();
    for (std::uint32_t team : {1U, 2U})
        for (AutoWowZoneProgression::HubSource const& source :
             AutoWowZoneProgression::NorthrendEntryZones(team))
        {
            if (source.map != AutoWowZoneProgression::kOutlandMap)
                continue;
            std::vector<Crossing> const chain =
                ChainFor(table, team, source.zone, AutoWowZoneProgression::kBoreanZone);
            ASSERT_FALSE(chain.empty());
            Crossing const& areaTrigger = chain.front();
            EXPECT_EQ(areaTrigger.via, Via::AreaTrigger);  // adapter keeps native CMSG_AREATRIGGER
            EXPECT_EQ(areaTrigger.object, 4352U);
            EXPECT_EQ(areaTrigger.map, 530U);
            EXPECT_EQ(areaTrigger.x, -248);
            EXPECT_EQ(areaTrigger.y, 896);
            EXPECT_EQ(areaTrigger.z, 84);
            EXPECT_TRUE(inside(static_cast<float>(areaTrigger.x), static_cast<float>(areaTrigger.y),
                               static_cast<float>(areaTrigger.z)));
            bool const genericPortal =
                areaTrigger.via == Via::Transport &&
                UsePortal(EffectiveMode(true, areaTrigger.via, {}, TransportMode::Auto, areaTrigger.object), 999, 0);
            EXPECT_FALSE(genericPortal);
        }
}

TEST(Transports, EveryNorthrendTransportLegIsForcedRealOnlyForOwnedTrip)
{
    std::vector<ModeOverride> overrides = {{190536, TransportMode::Portal},
                                           {175080, TransportMode::Portal},
                                           {186238, TransportMode::Portal}};
    for (std::uint32_t entry : {190536U, 175080U, 186238U})
    {
        EXPECT_EQ(EffectiveMode(true, Via::Transport, overrides, TransportMode::Portal, entry),
                  TransportMode::Real);
        EXPECT_FALSE(UsePortal(EffectiveMode(true, Via::Transport, overrides, TransportMode::Auto, entry), 999, 0));
    }
    EXPECT_EQ(EffectiveMode(false, Via::Transport, overrides, TransportMode::Auto, 175080),
              TransportMode::Portal);
    EXPECT_EQ(EffectiveMode(true, Via::AreaTrigger, overrides, TransportMode::Auto, 4352),
              TransportMode::Auto);
}

TEST(Transports, NorthrendLoadedIdentityFailsClosedOnEveryMaterialMismatch)
{
    auto facts = [](NorthrendTransportSpec const& s)
    {
        return LoadedTransportFacts{true, s.entry, s.taxiPath,
                                    true, s.sourceStop, s.sourceMap, s.sourceDelay,
                                    s.sourceStopX, s.sourceStopY, s.sourceStopZ,
                                    true, s.destinationStop, s.destinationMap, s.destinationDelay,
                                    s.destinationStopX, s.destinationStopY, s.destinationStopZ};
    };
    LoadedTransportFacts good = facts(kStormwindValiance);
    EXPECT_TRUE(LoadedTransportMatches(kStormwindValiance, good));
    auto bad = good; bad.motionTransport = false; EXPECT_FALSE(LoadedTransportMatches(kStormwindValiance, bad));
    bad = good; ++bad.entry; EXPECT_FALSE(LoadedTransportMatches(kStormwindValiance, bad));
    bad = good; ++bad.taxiPath; EXPECT_FALSE(LoadedTransportMatches(kStormwindValiance, bad));
    bad = good; ++bad.sourceIndex; EXPECT_FALSE(LoadedTransportMatches(kStormwindValiance, bad));
    bad = good; ++bad.destinationMap; EXPECT_FALSE(LoadedTransportMatches(kStormwindValiance, bad));
    bad = good; bad.sourceDelay = 0; EXPECT_FALSE(LoadedTransportMatches(kStormwindValiance, bad));
    bad = good; bad.destinationX += 2.0f; EXPECT_FALSE(LoadedTransportMatches(kStormwindValiance, bad));
    bad = good; bad.sourceZ = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(LoadedTransportMatches(kStormwindValiance, bad));
}

TEST(Transports, NorthrendNativeStopRequiresExactFrameFinitePositionAndNarrowVerticalMatch)
{
    CurrentTransportFacts source{true, kStormwindValiance.sourceStop, kStormwindValiance.sourceMap,
                                 kStormwindValiance.sourceStopX, kStormwindValiance.sourceStopY,
                                 kStormwindValiance.sourceStopZ};
    EXPECT_TRUE(AtNativeStop(kStormwindValiance, false, source));
    auto bad = source; ++bad.nodeIndex; EXPECT_FALSE(AtNativeStop(kStormwindValiance, false, bad));
    bad = source; bad.x = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(AtNativeStop(kStormwindValiance, false, bad));
    bad = source; bad.z += 3.01f; EXPECT_FALSE(AtNativeStop(kStormwindValiance, false, bad));

    CurrentTransportFacts destination{true, kStormwindValiance.destinationStop,
                                      kStormwindValiance.destinationMap,
                                      kStormwindValiance.destinationStopX,
                                      kStormwindValiance.destinationStopY,
                                      kStormwindValiance.destinationStopZ};
    EXPECT_TRUE(AtNativeStop(kStormwindValiance, true, destination));
    destination.map = kStormwindValiance.sourceMap;
    EXPECT_FALSE(AtNativeStop(kStormwindValiance, true, destination));
}

TEST(Transports, NorthrendPhysicalDisembarkRejectsTransportFlightAndInvalidThreeDimensionalArrival)
{
    DisembarkFacts good{false, false, false, kStormwindValiance.destinationMap,
                        kStormwindValiance.destinationX, kStormwindValiance.destinationY,
                        kStormwindValiance.destinationZ};
    EXPECT_TRUE(PhysicalDisembark(kStormwindValiance, good, 60.0f));
    auto bad = good; bad.anyTransport = true; EXPECT_FALSE(PhysicalDisembark(kStormwindValiance, bad, 60.0f));
    bad = good; bad.inFlight = true; EXPECT_FALSE(PhysicalDisembark(kStormwindValiance, bad, 60.0f));
    bad = good; bad.teleporting = true; EXPECT_FALSE(PhysicalDisembark(kStormwindValiance, bad, 60.0f));
    bad = good; ++bad.map; EXPECT_FALSE(PhysicalDisembark(kStormwindValiance, bad, 60.0f));
    bad = good; bad.y = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(PhysicalDisembark(kStormwindValiance, bad, 60.0f));
    bad = good; bad.z = std::numeric_limits<float>::infinity();
    EXPECT_FALSE(PhysicalDisembark(kStormwindValiance, bad, 60.0f));
    bad = good; bad.z += 3.01f; EXPECT_FALSE(PhysicalDisembark(kStormwindValiance, bad, 60.0f));
}

TEST(Transports, NorthrendReceiptNeedsOrderedSameInstanceBoardRideStopAndDisembark)
{
    constexpr std::uint64_t ship = 0xA11CE;
    constexpr std::uint64_t replacement = 0xB0A7;
    ChainState s;
    s.passage = NorthrendPassageId::StormwindValiance;
    ObservePassage(s, {s.passage, 0, false, false, false, true, true});
    EXPECT_FALSE(PassageComplete(s));
    ObservePassage(s, {NorthrendPassageId::OrgrimmarWarsong, ship, true, true, false, false, false});
    EXPECT_FALSE(s.passageBoarded);
    ObservePassage(s, {s.passage, ship, true, true, false, false, false});
    EXPECT_TRUE(s.passageBoarded);
    EXPECT_EQ(s.passageTransportGuid, ship);

    // A replacement same-entry/path ship cannot continue the receipt.
    ObservePassage(s, {s.passage, replacement, true, false, false, false, false});
    EXPECT_FALSE(s.passageDepartedSource);
    ObservePassage(s, {s.passage, ship, true, false, false, false, false});
    EXPECT_TRUE(s.passageDepartedSource);
    ObservePassage(s, {s.passage, replacement, true, false, false, true, false});
    EXPECT_FALSE(s.passageOnDestinationMap);
    ObservePassage(s, {s.passage, ship, true, false, false, true, false});
    EXPECT_TRUE(s.passageOnDestinationMap);
    ObservePassage(s, {s.passage, ship, true, false, true, true, false});
    EXPECT_TRUE(s.passageAtDestinationStop);
    EXPECT_FALSE(PassageComplete(s));
    ObservePassage(s, {s.passage, 0, false, false, false, true, true});
    EXPECT_TRUE(PassageComplete(s));
    std::string const field = PassageField(s, &kStormwindValiance);
    EXPECT_NE(field.find("\"passage_id\":1"), std::string::npos);
    EXPECT_NE(field.find("\"transport_guid\":" + std::to_string(ship)), std::string::npos);
    EXPECT_NE(field.find("\"path\":965"), std::string::npos);
}

TEST(Transports, ChainVersionResetPreservesActiveForeignAndNewerOwnership)
{
    EXPECT_TRUE(CanResetChainVersion(2, false, false));
    EXPECT_FALSE(CanResetChainVersion(2, true, false));
    EXPECT_FALSE(CanResetChainVersion(2, false, true));
    EXPECT_FALSE(CanResetChainVersion(kStateVersion, false, false));
    EXPECT_FALSE(CanResetChainVersion(kStateVersion + 1, false, false));
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

// AutoWow.ZoneProgression.Northrend2: Outland starts take the faction city portal (world DB gameobject spawns,
// spell_target_position landings) then the native terminal passage; no Dark Portal, no Grom'gol prefix.
TEST(Transports, Northrend2OutlandStartsTakeTheCityPortal)
{
    using AutoWowZoneProgression::kBoreanZone;
    std::vector<Crossing> const table = Northrend2Crossings();
    std::vector<Crossing> a = ChainFor(table, 1, 3523, kBoreanZone);  // Netherstorm
    ASSERT_EQ(a.size(), 2U);
    EXPECT_EQ(a[0].via, Via::GameObject);
    EXPECT_EQ(a[0].object, 183325U);  // Shattrath Portal to Stormwind
    EXPECT_EQ(a[0].exitMap, 0U);
    EXPECT_EQ(a[0].exitX, -9003);
    EXPECT_EQ(a[1].object, 190536U);
    std::vector<Crossing> h = ChainFor(table, 2, 3703, kBoreanZone);  // Shattrath
    ASSERT_EQ(h.size(), 2U);
    EXPECT_EQ(h[0].object, 183323U);  // Shattrath Portal to Orgrimmar
    EXPECT_EQ(h[0].exitMap, 1U);
    EXPECT_EQ(h[1].object, 186238U);
    // Hellfire: the Stair of Destiny portals beside the Dark Portal exit.
    EXPECT_EQ(ChainFor(table, 1, AutoWowZoneProgression::kHellfireZone, kBoreanZone)[0].object, 195139U);
    EXPECT_EQ(ChainFor(table, 2, AutoWowZoneProgression::kHellfireZone, kBoreanZone)[0].object, 195140U);
    // Horde Grom'gol: zeppelin to Orgrimmar, then Warsong. Durotar / capitals: one passage.
    std::vector<Crossing> stv = ChainFor(table, 2, AutoWowZoneProgression::kStranglethornZone, kBoreanZone);
    ASSERT_EQ(stv.size(), 2U);
    EXPECT_EQ(stv[0].object, 175080U);
    EXPECT_EQ(stv[1].object, 186238U);
    EXPECT_EQ(ChainFor(table, 2, AutoWowZoneProgression::kDurotarZone, kBoreanZone).size(), 1U);
    EXPECT_EQ(ChainFor(table, 1, AutoWowZoneProgression::kStormwindZone, kBoreanZone).size(), 1U);
    for (Crossing const& c : table)
        EXPECT_NE(c.object, 4352U);
    // Every chain start owns a chain ending at its team's native passage.
    for (std::uint32_t team : {1U, 2U})
        for (AutoWowZoneProgression::HubSource const& s : AutoWowZoneProgression::Northrend2EntryZones(team))
        {
            std::vector<Crossing> const chain = ChainFor(table, team, s.zone, kBoreanZone);
            ASSERT_FALSE(chain.empty()) << team << " " << s.zone;
            EXPECT_EQ(chain.back().object, NorthrendPassageFor(team)->entry);
        }
}
}  // namespace
