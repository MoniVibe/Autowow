/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef PLAYERBOTS_DUNGEONGATEPOLICY_H
#define PLAYERBOTS_DUNGEONGATEPOLICY_H

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <vector>

// Scripted dungeon progression steps (AutoWow.DungeonNav.Gates, default 0). One compiled-in row per
// step; ids/positions verified against core scripts and the world DB (docs/DUNGEON_GATES.md).
// While an encounter is unfinished and has rows, the navigator goes to the first row whose doneWhen
// is false and performs it out of combat with the party gathered.
namespace DungeonGate
{
enum class Kind : std::uint8_t
{
    UseGo,      // CMSG_GAMEOBJ_USE on the object (any GO type, rituals included)
    Gossip,     // gossip hello + select gossipOption on the creature
    Escort,     // leader keeps up with the escorted creature
    KillSet,    // attack live creatures of entry within radius of the row position
    EnterArea,  // stand at the row position
    ProxyKill,  // KillSet whose target stands in for a boss with no static spawn
    LootGo,     // open the chest GO and autostore item doneValue into the navigating bot's bags
    UseItemOnGo,  // the party member holding keyItem uses it on the GO (CMSG_USE_ITEM)
    AreaTrigger,  // send CMSG_AREATRIGGER entry from the row position (headless bots never emit it)
    Prerequisite,  // no action: until doneWhen the encounter is set aside (its boss is not there to fight yet)
};

enum class DoneWhen : std::uint8_t
{
    GoUsed,         // GO doneData (0 = the row GO): state not READY, or IN_USE / NOT_SELECTABLE
    CreaturesDead,  // no live creature of the row entry (spawnGuid, or within radius) left
    InstanceData,   // InstanceScript::GetData(doneData) >= doneValue
    EncounterDone,  // the row's encounter is complete
    Escorting,      // the row creature's AI reports an escort in progress
    Unlocked,       // door GO doneData not READY or gone from its loaded grid, or (doneValue != 0) a
                    // party member holds item doneValue
    Hostile,        // the row creature (spawnGuid) is hostile to the navigating bot
    SuccessorActive,  // native encounter DONE, or a live summoned successor entry doneData/doneValue
};

struct Step
{
    std::uint32_t mapId;
    std::uint32_t encounterIdx;
    std::uint32_t stepOrder;
    Kind kind;
    std::uint32_t entry;
    std::uint32_t spawnGuid;  // 0 = any spawn of entry within radius
    float x;
    float y;
    float z;
    float radius;
    std::uint32_t gossipOption;
    std::uint32_t repeat;  // UseGo/Gossip: uses issued before waiting out the timeout
    DoneWhen doneWhen;
    std::uint32_t doneData;
    std::uint32_t doneValue;
    std::uint32_t keyItem;  // != 0: skipped while no party member holds it, unless
                            // AutoWow.DungeonNav.Gates.BypassKeys = 1
    bool optional;          // skipped for the instance once it times out
    std::uint32_t timeoutMs;
};

// Rows sorted by (mapId, encounterIdx, stepOrder); DungeonGatePolicyTest enforces it.
inline constexpr Step Steps[] = {
    // Deadmines: Sneed (idx1) has no static spawn. Sneed's Shredder ejects him on death (SAI 642).
    {36, 1, 0, Kind::ProxyKill, 642, 79223, -289.453f, -513.009f, 49.6785f, 15.0f, 0, 1,
        DoneWhen::CreaturesDead, 0, 0, 0, false, 300000},
    {36, 1, 1, Kind::KillSet, 643, 0, -289.453f, -513.009f, 49.6785f, 40.0f, 0, 1,
        DoneWhen::EncounterDone, 0, 0, 0, false, 120000},
    // Deadmines: the Iron Clad Door (30534) holds Mr. Smite..VanCleef (idx3-6). Chest 17155 (loot 2882)
    // drops Defias Gunpowder 5397; its use casts 6250 (open lock 83, key 5397) on the Defias Cannon, whose
    // SAI 1639800 sets the door's GO state. The lever 101833 is not selectable.
    {36, 3, 0, Kind::LootGo, 17155, 26203, -106.409f, -617.284f, 13.8495f, 10.0f, 0, 1,
        DoneWhen::Unlocked, 30534, 5397, 0, false, 120000},
    {36, 3, 1, Kind::UseItemOnGo, 16398, 26205, -107.562f, -659.674f, 7.21211f, 10.0f, 0, 1,
        DoneWhen::Unlocked, 30534, 0, 5397, false, 120000},
    {36, 4, 0, Kind::LootGo, 17155, 26203, -106.409f, -617.284f, 13.8495f, 10.0f, 0, 1,
        DoneWhen::Unlocked, 30534, 5397, 0, false, 120000},
    {36, 4, 1, Kind::UseItemOnGo, 16398, 26205, -107.562f, -659.674f, 7.21211f, 10.0f, 0, 1,
        DoneWhen::Unlocked, 30534, 0, 5397, false, 120000},
    {36, 5, 0, Kind::LootGo, 17155, 26203, -106.409f, -617.284f, 13.8495f, 10.0f, 0, 1,
        DoneWhen::Unlocked, 30534, 5397, 0, false, 120000},
    {36, 5, 1, Kind::UseItemOnGo, 16398, 26205, -107.562f, -659.674f, 7.21211f, 10.0f, 0, 1,
        DoneWhen::Unlocked, 30534, 0, 5397, false, 120000},
    {36, 6, 0, Kind::LootGo, 17155, 26203, -106.409f, -617.284f, 13.8495f, 10.0f, 0, 1,
        DoneWhen::Unlocked, 30534, 5397, 0, false, 120000},
    {36, 6, 1, Kind::UseItemOnGo, 16398, 26205, -107.562f, -659.674f, 7.21211f, 10.0f, 0, 1,
        DoneWhen::Unlocked, 30534, 0, 5397, false, 120000},
    // Wailing Caverns: Mutanus (idx7) is summoned by the Disciple of Naralex escort (SAI 3678, gossip
    // menu 201 option 0, shown once instance data 0..3 are DONE).
    {43, 7, 0, Kind::Gossip, 3678, 18675, -134.965f, 125.402f, -78.0945f, 10.0f, 0, 1,
        DoneWhen::Escorting, 0, 0, 0, false, 120000},
    {43, 7, 1, Kind::Escort, 3678, 18675, -134.965f, 125.402f, -78.0945f, 10.0f, 0, 1,
        DoneWhen::EncounterDone, 0, 0, 0, false, 1200000},
    // Razorfen Kraul: ward 21099 in front of Agathelos (idx5) opens once both Ward Keepers die.
    {47, 5, 0, Kind::KillSet, 4625, 0, 2066.5f, 2012.0f, 64.1f, 15.0f, 0, 1,
        DoneWhen::CreaturesDead, 0, 0, 0, false, 300000},
    // Blackfathom Deeps: Aku'mai (idx7) portal 21117 opens after the four fires are lit and every
    // summoned wave (one creature group per fire) is dead.
    {48, 7, 0, Kind::UseGo, 21118, 32930, -813.47f, -158.535f, -24.5271f, 10.0f, 0, 1,
        DoneWhen::GoUsed, 0, 0, 0, false, 120000},
    {48, 7, 1, Kind::KillSet, 4825, 0, -818.7f, -164.5f, -24.5f, 60.0f, 0, 1,
        DoneWhen::CreaturesDead, 0, 0, 0, false, 300000},
    {48, 7, 2, Kind::UseGo, 21119, 32932, -813.578f, -170.461f, -24.5276f, 10.0f, 0, 1,
        DoneWhen::GoUsed, 0, 0, 0, false, 120000},
    {48, 7, 3, Kind::KillSet, 4977, 0, -818.7f, -164.5f, -24.5f, 60.0f, 0, 1,
        DoneWhen::CreaturesDead, 0, 0, 0, false, 300000},
    {48, 7, 4, Kind::UseGo, 21120, 32933, -823.955f, -170.407f, -24.5267f, 10.0f, 0, 1,
        DoneWhen::GoUsed, 0, 0, 0, false, 120000},
    {48, 7, 5, Kind::KillSet, 4823, 0, -818.7f, -164.5f, -24.5f, 60.0f, 0, 1,
        DoneWhen::CreaturesDead, 0, 0, 0, false, 300000},
    {48, 7, 6, Kind::UseGo, 21121, 32931, -823.88f, -158.535f, -24.5278f, 10.0f, 0, 1,
        DoneWhen::GoUsed, 0, 0, 0, false, 120000},
    {48, 7, 7, Kind::KillSet, 4978, 0, -818.7f, -164.5f, -24.5f, 60.0f, 0, 1,
        DoneWhen::CreaturesDead, 0, 0, 0, false, 300000},
    {48, 7, 8, Kind::EnterArea, 21117, 32682, -818.361f, -200.647f, -25.7911f, 10.0f, 0, 1,
        DoneWhen::GoUsed, 0, 0, 0, false, 180000},
    // Uldaman: Ironaya (idx2) sleeps behind the Seal of Khaz'Mul (124372), unattackable, until the Keystone
    // (124371, lock 359 = Staff of Prehistoria 7733) is used: its SAI sets DATA_IRONAYA_DOORS (0) DONE (3), opens
    // the seal and frees Ironaya. Key row: without the staff the encounter is set aside (KeyBlocked). S68: the
    // horde probe walked to the sealed room from the north and stalled no_reachable_waypoint at (-214,374).
    {70, 2, 0, Kind::UseGo, 124371, 14393, -234.688f, 239.619f, -50.9083f, 10.0f, 0, 1,
        DoneWhen::InstanceData, 0, 3, 7733, false, 60000},
    // Uldaman: Altar of Archaedas (ritual, 1 participant) casts 10340, which sets DATA_ARCHAEDAS (2)
    // IN_PROGRESS and wakes Archaedas (idx7).
    {70, 7, 0, Kind::UseGo, 133234, 40698, 96.4808f, 269.052f, -52.1487f, 10.0f, 0, 1,
        DoneWhen::InstanceData, 2, 1, 0, false, 60000},
    // Gnomeregan: Grubbis (idx2) arrives in Blastmaster Emi Shortfuse's escort (SAI 7998, menu 1080
    // option 0, summon group 4).
    {90, 2, 0, Kind::Gossip, 7998, 30136, -514.935f, -138.544f, -152.399f, 10.0f, 0, 1,
        DoneWhen::Escorting, 0, 0, 0, false, 120000},
    {90, 2, 1, Kind::Escort, 7998, 30136, -514.935f, -138.544f, -152.399f, 10.0f, 0, 1,
        DoneWhen::EncounterDone, 0, 0, 0, false, 1200000},
    // Sunken Temple: Atal'alarion (idx0) statues 148830..148835 in entry order; DATA_STATUES (10) counts
    // the correct uses and a wrong statue casts poison, so each row waits for its own count.
    {109, 0, 0, Kind::UseGo, 148830, 27898, -515.553f, 95.2582f, -148.74f, 10.0f, 0, 1,
        DoneWhen::InstanceData, 10, 1, 0, false, 60000},
    {109, 0, 1, Kind::UseGo, 148831, 27899, -419.849f, 94.4837f, -148.74f, 10.0f, 0, 1,
        DoneWhen::InstanceData, 10, 2, 0, false, 60000},
    {109, 0, 2, Kind::UseGo, 148832, 28111, -491.4f, 135.97f, -148.74f, 10.0f, 0, 1,
        DoneWhen::InstanceData, 10, 3, 0, false, 60000},
    {109, 0, 3, Kind::UseGo, 148833, 28112, -491.491f, 53.4818f, -148.74f, 10.0f, 0, 1,
        DoneWhen::InstanceData, 10, 4, 0, false, 60000},
    {109, 0, 4, Kind::UseGo, 148834, 28113, -443.855f, 136.101f, -148.74f, 10.0f, 0, 1,
        DoneWhen::InstanceData, 10, 5, 0, false, 60000},
    {109, 0, 5, Kind::UseGo, 148835, 28114, -443.417f, 53.8312f, -148.74f, 10.0f, 0, 1,
        DoneWhen::InstanceData, 10, 6, 0, false, 60000},
    // Sunken Temple: Dreamscythe (idx1) and Weaver (idx2) spawn in phase 2 and their SAI shows them only once
    // TYPE_JAMMAL_AN (1) is DONE (3); Shade of Eranikus (idx8) is NOT_SELECTABLE until Jammal'an dies and his SAI
    // drops the immunity under the same condition. Until then those encounters are set aside, not walked to.
    {109, 1, 0, Kind::Prerequisite, 5721, 239020, -453.45f, 137.17f, -90.75f, 10.0f, 0, 1,
        DoneWhen::InstanceData, 1, 3, 0, false, 60000},
    {109, 2, 0, Kind::Prerequisite, 5720, 239021, -458.84f, 127.7f, -91.57f, 10.0f, 0, 1,
        DoneWhen::InstanceData, 1, 3, 0, false, 60000},
    // Sunken Temple: Jammal'an (idx3) is IMMUNE_TO_PC and behind forcefield 149431 until DATA_DEFENDER_KILLED (11)
    // reaches 6: the six balcony trolls (Mijan, Zul'Lor, Zolo, Gasher, Loro, Hukku; SAI on death set data 11), in the
    // walk order of route leg (109, 3).
    {109, 3, 0, Kind::KillSet, 5717, 39847, -406.189f, 131.068f, -66.9138f, 15.0f, 0, 1,
        DoneWhen::CreaturesDead, 0, 0, 0, false, 300000},
    {109, 3, 1, Kind::KillSet, 5716, 34522, -467.396f, 165.997f, -66.7027f, 15.0f, 0, 1,
        DoneWhen::CreaturesDead, 0, 0, 0, false, 300000},
    {109, 3, 2, Kind::KillSet, 5712, 39843, -528.646f, 130.163f, -66.7533f, 15.0f, 0, 1,
        DoneWhen::CreaturesDead, 0, 0, 0, false, 300000},
    {109, 3, 3, Kind::KillSet, 5713, 39844, -527.969f, 59.4516f, -66.7188f, 15.0f, 0, 1,
        DoneWhen::CreaturesDead, 0, 0, 0, false, 300000},
    {109, 3, 4, Kind::KillSet, 5714, 39845, -466.655f, 24.4261f, -66.7908f, 15.0f, 0, 1,
        DoneWhen::CreaturesDead, 0, 0, 0, false, 300000},
    {109, 3, 5, Kind::KillSet, 5715, 39846, -405.506f, 60.4569f, -67.0678f, 15.0f, 0, 1,
        DoneWhen::CreaturesDead, 0, 0, 0, false, 300000},
    {109, 8, 0, Kind::Prerequisite, 5709, 39842, -658.379f, -35.7623f, -90.8352f, 10.0f, 0, 1,
        DoneWhen::InstanceData, 1, 3, 0, false, 60000},
    // Razorfen Downs: Gong 148917 summons group 1, 2, 3 on GetData(148917) == 0, 1, 2; it is
    // unselectable while a wave lives and the data advances when the wave dies. Use 3 brings
    // Tuten'kash (idx0) and leaves the gong unselectable.
    {129, 0, 0, Kind::UseGo, 148917, 32045, 2552.44f, 856.984f, 51.495f, 10.0f, 0, 2,
        DoneWhen::InstanceData, 148917, 2, 0, false, 600000},
    {129, 0, 1, Kind::UseGo, 148917, 32045, 2552.44f, 856.984f, 51.495f, 10.0f, 0, 1,
        DoneWhen::GoUsed, 0, 0, 0, false, 60000},
    {129, 0, 2, Kind::KillSet, 7355, 0, 2552.44f, 856.984f, 51.495f, 80.0f, 0, 1,
        DoneWhen::EncounterDone, 0, 0, 0, false, 300000},
    // Zul'Farrak: Witch Doctor Zum'rah (idx4, guid 81524) is faction 35 until area trigger 962 (r10 at his
    // grave) runs its SAI (set faction 37). S68: the probe stood on him logging activation_blocked=not_hostile.
    {209, 4, 0, Kind::AreaTrigger, 962, 81524, 1909.27f, 1015.11f, 11.5155f, 5.0f, 0, 1,
        DoneWhen::Hostile, 0, 0, 0, false, 60000},
    // Hellfire Ramparts: Vazruden (idx2) has no static spawn. The passive Herald summons two sentries;
    // their deaths start the encounter and summon Vazruden and Nazan. SuccessorActive is a reversible
    // handoff to ordinary combat: a wipe can reset the sentries until the native boss state is DONE.
    {543, 2, 0, Kind::KillSet, 17517, 0, -1377.98f, 1718.07f, 82.88f, 60.0f, 0, 1,
        DoneWhen::SuccessorActive, 17537, 17536, 0, false, 300000},
};

constexpr std::size_t NoStep = std::numeric_limits<std::size_t>::max();
// Navigator goal ids for rows; creature spawn ids stay below this bit.
constexpr std::uint32_t GoalIdBase = 0x80000000u;

inline std::uint32_t GoalId(std::size_t row)
{
    return GoalIdBase | static_cast<std::uint32_t>(row + 1);
}

// Dungeon wings with no navmesh link between them (offline region query over the p1data mmaps): a party inside one
// wing can never reach another wing's encounters in that instance, so those are set aside (gate_unavailable=
// other_wing) and left out of the probe's clearable mask. Anchors are the wing's entrances and boss spawns; a party
// belongs to the wing of the anchor nearest its leader (the table's first on a tie).
struct WingAnchor
{
    std::uint32_t mapId;
    std::uint32_t encounterMask;  // the wing's encounter indices
    float x;
    float y;
    float z;
};

// Dire Maul (429): East (Zevrim 0, Hydrospawn 1, Lethtendris 2, Alzzin 3; entrances areatrigger_teleport 3183-3185)
// is one region (1614 polys); West and North (4..15, joined by the courtyard; entrances 3186, 3187, 3189) another
// (2849 polys). S75: the probe enters East and was scored against all 16.
constexpr std::uint32_t DireMaulEast = 0x000Fu;
constexpr std::uint32_t DireMaulWestNorth = 0xFFF0u;
inline constexpr WingAnchor WingAnchors[] = {
    {429, DireMaulEast, 44.4f, -154.8f, -2.7f},
    {429, DireMaulEast, -201.1f, -328.7f, -2.7f},
    {429, DireMaulEast, 9.3f, -837.1f, -32.5f},
    {429, DireMaulEast, -34.983f, -448.0f, -37.8785f},
    {429, DireMaulEast, 4.57887f, -438.407f, -59.954f},
    {429, DireMaulEast, -5.45062f, -441.126f, 16.4179f},
    {429, DireMaulEast, 274.844f, -427.251f, -119.962f},
    {429, DireMaulWestNorth, -63.0f, 159.9f, -3.5f},
    {429, DireMaulWestNorth, 31.6f, 159.4f, -3.5f},
    {429, DireMaulWestNorth, 255.2f, -16.1f, -2.6f},
    {429, DireMaulWestNorth, -84.35f, 543.96f, 28.62f},
    {429, DireMaulWestNorth, -38.08f, 812.44f, -29.45f},
    {429, DireMaulWestNorth, 14.39f, 475.85f, -23.3f},
    {429, DireMaulWestNorth, 33.14f, 575.55f, -4.31f},
    {429, DireMaulWestNorth, 132.63f, 625.91f, -48.38f},
    {429, DireMaulWestNorth, 356.8f, 258.31f, 11.65f},
    {429, DireMaulWestNorth, 410.71f, -3.15f, -24.56f},
    {429, DireMaulWestNorth, 491.23f, 97.39f, -2.5f},
    {429, DireMaulWestNorth, 550.38f, 533.72f, -25.32f},
    {429, DireMaulWestNorth, 627.59f, 481.72f, 29.46f},
    {429, DireMaulWestNorth, 828.07f, 480.75f, 37.32f},
    {429, DireMaulWestNorth, 833.99f, 489.54f, 37.4f},
};

// The encounter lies in another wing than the one (x, y, z) is in; false for maps without wings.
inline bool OtherWing(std::uint32_t mapId, std::uint32_t encounterIdx, float x, float y, float z)
{
    WingAnchor const* nearest = nullptr;
    float best = 0.0f;
    for (WingAnchor const& anchor : WingAnchors)
    {
        if (anchor.mapId != mapId)
            continue;
        float const dx = anchor.x - x;
        float const dy = anchor.y - y;
        float const dz = anchor.z - z;
        float const distance = dx * dx + dy * dy + dz * dz;
        if (!nearest || distance < best)
        {
            nearest = &anchor;
            best = distance;
        }
    }
    return nearest && encounterIdx < 32 && !(nearest->encounterMask >> encounterIdx & 1u);
}

inline bool MapHasSteps(std::uint32_t mapId)
{
    for (Step const& step : Steps)
        if (step.mapId == mapId)
            return true;
    return false;
}

inline bool SuccessorActive(bool encounterDone, bool firstSuccessorAlive, bool secondSuccessorAlive)
{
    return encounterDone || firstSuccessorAlive || secondSuccessorAlive;
}

inline bool LatchesDone(DoneWhen doneWhen)
{
    return doneWhen != DoneWhen::SuccessorActive;
}

inline bool RequiresBossStateCompletion(std::uint32_t mapId, std::uint32_t encounterIdx)
{
    for (Step const& step : Steps)
        if (step.mapId == mapId && step.encounterIdx == encounterIdx &&
            step.doneWhen == DoneWhen::SuccessorActive)
            return true;
    return false;
}

inline std::uint32_t AuthorizeGateBackedEncounters(std::uint32_t mapId, std::uint32_t clearableMask)
{
    for (Step const& step : Steps)
        if (step.mapId == mapId && step.encounterIdx < 32 &&
            step.doneWhen == DoneWhen::SuccessorActive)
            clearableMask |= 1u << step.encounterIdx;
    return clearableMask;
}

inline bool CompletionConfirmed(std::uint32_t mapId, std::uint32_t encounterIdx,
    std::uint32_t completedEncounterMask, bool bossStateDone)
{
    if (encounterIdx >= 32 || !(completedEncounterMask & (1u << encounterIdx)))
        return false;
    return !RequiresBossStateCompletion(mapId, encounterIdx) || bossStateDone;
}

// Table indices of the encounter's rows, in stepOrder.
inline std::vector<std::size_t> StepsFor(std::uint32_t mapId, std::uint32_t encounterIdx)
{
    std::vector<std::size_t> rows;
    for (std::size_t index = 0; index < std::size(Steps); ++index)
        if (Steps[index].mapId == mapId && Steps[index].encounterIdx == encounterIdx)
            rows.push_back(index);
    return rows;
}

inline char const* KindName(Kind kind)
{
    switch (kind)
    {
        case Kind::UseGo: return "use_go";
        case Kind::Gossip: return "gossip";
        case Kind::Escort: return "escort";
        case Kind::KillSet: return "kill_set";
        case Kind::EnterArea: return "enter_area";
        case Kind::ProxyKill: return "proxy_kill";
        case Kind::LootGo: return "loot_go";
        case Kind::UseItemOnGo: return "use_item_on_go";
        case Kind::AreaTrigger: return "area_trigger";
        case Kind::Prerequisite: return "prerequisite";
    }
    return "unknown";
}

inline bool CountsUses(Kind kind)
{
    return kind == Kind::UseGo || kind == Kind::Gossip || kind == Kind::LootGo ||
        kind == Kind::UseItemOnGo || kind == Kind::AreaTrigger;
}

// Per-instance runtime of one row.
struct StepRuntime
{
    std::uint32_t acts = 0;
    std::uint32_t firstActMs = 0;  // 0 = not acted yet
    bool done = false;             // doneWhen observed true once; steps only move forward
    bool skipped = false;          // optional row that timed out
};

// First row, in order, that is not skipped, not done and not key-gated. A row is key-gated while its
// keyItem is set, no party member holds it (holdsKey(item) false) and bypassKeys is off. isDone(i) is
// evaluated lazily and never past the returned row, so a later row can never run before an earlier
// unfinished one. doneWhen is read before the key: the cannon consumes the Gunpowder, and soak S56 skipped
// that row silently (no `done` line) once the key was gone, so the log could not tell an opened door from
// a closed one.
template <typename HoldsKey, typename IsDone>
std::size_t SelectStep(std::vector<Step> const& rows, std::vector<StepRuntime> const& runtime,
    bool bypassKeys, HoldsKey&& holdsKey, IsDone&& isDone)
{
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        if (index < runtime.size() && (runtime[index].done || runtime[index].skipped))
            continue;
        if (isDone(index))
            continue;
        if (rows[index].keyItem && !bypassKeys && !holdsKey(rows[index].keyItem))
            continue;
        return index;
    }
    return NoStep;
}

// The encounter cannot start for this party: its first unfinished row (in order, not done, not skipped) is
// key-gated (keyItem set, not held, bypassKeys off). The navigator sets such an encounter aside instead of walking
// to its boss (S68 Uldaman Ironaya). Same lazy isDone contract as SelectStep.
template <typename HoldsKey, typename IsDone>
bool KeyBlocked(std::vector<Step> const& rows, std::vector<StepRuntime> const& runtime, bool bypassKeys,
    HoldsKey&& holdsKey, IsDone&& isDone)
{
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        if (index < runtime.size() && (runtime[index].done || runtime[index].skipped))
            continue;
        if (isDone(index))
            continue;
        return rows[index].keyItem && !bypassKeys && !holdsKey(rows[index].keyItem);
    }
    return false;
}

// The encounter's first unfinished row (same contract as KeyBlocked) is a Prerequisite: its boss is not fightable
// yet, so the navigator sets it aside for now. Unlike a key-blocked encounter it still counts toward a full clear.
template <typename IsDone>
bool PrerequisitePending(std::vector<Step> const& rows, std::vector<StepRuntime> const& runtime, IsDone&& isDone)
{
    for (std::size_t index = 0; index < rows.size(); ++index)
    {
        if (index < runtime.size() && (runtime[index].done || runtime[index].skipped))
            continue;
        if (isDone(index))
            continue;
        return rows[index].kind == Kind::Prerequisite;
    }
    return false;
}

enum class Wait
{
    Act,    // issue the row action
    Hold,   // uses spent; wait for doneWhen
    Retry,  // timed out, required row: start the row over
    Skip,   // timed out, optional row: skip it for this instance
};

inline Wait Evaluate(Step const& step, StepRuntime const& runtime, std::uint32_t elapsedMs)
{
    if (runtime.firstActMs && step.timeoutMs && elapsedMs >= step.timeoutMs)
        return step.optional ? Wait::Skip : Wait::Retry;
    if (CountsUses(step.kind) && runtime.acts >= (step.repeat ? step.repeat : 1u))
        return Wait::Hold;
    return Wait::Act;
}

// How the navigator walks to a selected row's position. Soak S55: both Deadmines probe parties stalled at the
// foundry exit (-153,-590) with the Gunpowder row (enc 3) selected and no gate line, 54 yd short of the chest.
// The slope-checked probe fails the navmesh-walkable hump at (-143.5,-585.9) (FindSmoothPath checks each step
// against a steer point held at the previous height, NavmeshSnap.h), the stored travel nodes end at the same
// hump, so the travel route ran out and was rebuilt forever. Detour connects the foundry, the chest and the
// cannon (offline replay over the 036 mmaps: complete corridors, 18 / 16 smoothed points without the slope
// check, a failed step with it). A row the slope-free path reaches is walked directly.
enum class Approach : std::uint8_t
{
    Direct,       // MoveTo the row position; the core mover paths without the slope check
    TravelNodes,  // stored travel-node route toward the row position
};

inline Approach SelectApproach(bool slopeCheckedReached, bool slopeFreeReached)
{
    return slopeCheckedReached || slopeFreeReached ? Approach::Direct : Approach::TravelNodes;
}

// Escort row, per scan. Soak S62 Gnomeregan: Emi Shortfuse's escort (SAI 7998) summons trogg group 1 with
// attackScriptOwner on her; the party stood within the escort distance logging `wait`, nobody was attacked, she
// died (spawntime 86400) and the row logged npc_missing until the run was declared stuck. Her attackers come first.
enum class EscortAct : std::uint8_t
{
    Defend,  // attack the nearest live attacker of the escorted creature
    Wait,    // near enough, nothing to defend
    Follow,  // move to the escorted creature
};

inline EscortAct SelectEscortAct(bool escortedAttacked, bool withinEscortDistance)
{
    if (escortedAttacked)
        return EscortAct::Defend;
    return withinEscortDistance ? EscortAct::Wait : EscortAct::Follow;
}

// The per-scan gate decision logs (approach, wait, wait_party, no_key, ...) are INFO: a (row, result) pair logs
// the first time and again LogRepeatMs after its last line.
constexpr std::uint32_t LogRepeatMs = 30000;

inline bool ShouldLog(bool logged, std::uint32_t sinceLastMs)
{
    return !logged || sinceLastMs >= LogRepeatMs;
}
}

#endif
