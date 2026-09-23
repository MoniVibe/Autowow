/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef AUTOWOW_ENGAGEMENT_TRACKER_H
#define AUTOWOW_ENGAGEMENT_TRACKER_H

#include <cstddef>
#include <cstdint>
#include <string>

#include "TacticalPolicy.h"

// Per-bot engagement lifecycle + tactic path (docs/TACTICAL_COMBAT_PLAN.md 4.1). Pure value type fed by
// the runtime adapter once per ReevalMs (and on every combat-state change) with game-time ms; unit-tested.
// An engagement starts on combat false->true and ends kEndDebounceMs after combat true->false (a re-entry
// inside the debounce continues it) or immediately on death. One ledger `engage` line per engagement.
namespace AutoWowTactics
{
inline constexpr std::uint32_t kEngageSchemaVersion = 1;  // `ecv`; fields are append-only
inline constexpr std::size_t kMaxSegs = 16;               // tactic path entries per engagement; rest -> tacs_drop
inline constexpr std::uint64_t kEndDebounceMs = 1000;
inline constexpr std::size_t kHpRing = 8;                 // HP samples (~4 s at ReevalMs 500)
inline constexpr std::uint64_t kSlopeWindowMs = 4000;
inline constexpr std::uint64_t kSlopeMinMs = 1500;

// Wire-stable; append only.
enum class Outcome : std::uint8_t
{
    Win = 0,              // survived with >= 1 kill
    Died = 1,
    Escaped = 2,          // survived after an escape attempt
    DiedAfterEscape = 3,
    NoKill = 4            // survived without a kill (evade / leash / target stolen / abandoned)
};

enum class CastKind : std::uint8_t
{
    Other = 0,
    Shield = 1,   // PW:Shield
    Control = 2   // Psychic Scream
};

struct TickInput
{
    std::uint64_t nowMs = 0;
    bool inCombat = false;
    bool dead = false;
    bool resting = false;      // sitting (eat / drink) - out-of-combat rest accounting
    bool autoRepeat = false;   // wand / auto-shot running
    std::uint32_t hpPct = 100;
    std::uint32_t manaPct = 100;
    std::uint64_t hp = 0;      // effective hp (health + remaining absorb)
    std::uint64_t mana = 0;
    std::int32_t pullRisk = -1;  // PackRisk band of the proactive pull that started this, -1 = none
    EngagementSnapshot snap;     // valid when inCombat
};

struct TacticSeg
{
    TacticId id = TacticId::None;
    std::uint32_t ms = 0;
};

struct EngageRecord
{
    std::uint32_t engId = 0;
    TacticId tac0 = TacticId::None;
    TacticSeg segs[kMaxSegs] = {};
    std::uint32_t segN = 0;
    std::uint32_t segDrop = 0;
    std::uint32_t mobsMax = 0;
    std::uint32_t addsMax = 0;
    std::uint32_t eliteMax = 0;
    std::int32_t lvlDmax = 0;
    std::uint32_t loadMax = 0;
    bool pvp = false;
    std::uint64_t durMs = 0;
    std::uint32_t kills = 0;
    Outcome outcome = Outcome::NoKill;
    std::uint32_t hp0 = 0, hp1 = 0, mp0 = 0, mp1 = 0;
    std::uint64_t manaSpent = 0;
    std::uint64_t hpLost = 0;
    std::uint32_t casts = 0;
    std::uint64_t wandMs = 0;
    std::uint32_t ccN = 0;
    std::uint32_t shieldN = 0;
    std::int64_t gapMs = -1;       // since the previous engagement end; -1 = first since login
    std::int64_t gapRestMs = -1;   // ... of which resting
    std::int32_t pullRisk = -1;
};

class EngagementTracker
{
public:
    // Returns true when an engagement finished; `out` then holds it. After the call, CreditId()/CreditMs()
    // name the tactic and the ms of this tick to credit to the lifetime tac_ms counters (None/0 = none).
    bool Tick(TickInput const& in, PriestParams const& p, EngageRecord& out)
    {
        std::uint64_t const dt = hasLast && in.nowMs >= lastMs ? in.nowMs - lastMs : 0;
        hasLast = true;
        lastMs = in.nowMs;
        creditId = TacticId::None;
        creditMs = 0;

        if (active && !pendingEnd)
        {
            if (rec.tac0 != TacticId::None)
            {
                creditId = tactic;
                creditMs = static_cast<std::uint32_t>(dt);
                if (curSeg < kMaxSegs)
                    rec.segs[curSeg].ms += static_cast<std::uint32_t>(dt);
            }
            if (in.autoRepeat)
                rec.wandMs += dt;
            rec.manaSpent += prevMana > in.mana ? prevMana - in.mana : 0;
            rec.hpLost += prevHp > in.hp ? prevHp - in.hp : 0;
        }
        else if (!active && everEnded && in.resting)
            restMs += dt;
        prevMana = in.mana;
        prevHp = in.hp;

        bool const combat = in.inCombat && !in.dead;
        if (!active)
        {
            if (!combat)
                return false;
            Start(in);
        }
        else if (in.dead)
        {
            rec.hp1 = 0;
            rec.mp1 = in.manaPct;
            Finish(in.nowMs, true, out);
            return true;
        }
        else if (!combat)
        {
            if (!pendingEnd)
            {
                pendingEnd = true;
                endAtMs = in.nowMs;
                rec.hp1 = in.hpPct;
                rec.mp1 = in.manaPct;
            }
            else if (in.nowMs - endAtMs >= kEndDebounceMs)
            {
                Finish(endAtMs, false, out);
                return true;
            }
            return false;
        }
        pendingEnd = false;

        ring[ringHead] = HpSample{in.nowMs, in.hp};
        ringHead = (ringHead + 1) % kHpRing;
        if (ringN < kHpRing)
            ++ringN;

        EngagementSnapshot s = in.snap;
        s.deathEtaMs = DeathEtaMs(in.nowMs, in.hp);
        rec.mobsMax = Max(rec.mobsMax, s.attackers);
        rec.addsMax = Max(rec.addsMax, s.addsNear);
        rec.eliteMax = Max(rec.eliteMax, s.eliteN);
        rec.loadMax = Max(rec.loadMax, s.load);
        rec.lvlDmax = firstSnap || s.lvlDmax > rec.lvlDmax ? s.lvlDmax : rec.lvlDmax;
        rec.pvp = rec.pvp || s.pvp;
        firstSnap = false;
        last = s;

        std::uint64_t const inCurrent = in.nowMs - tacticSinceMs;
        TacticId const next = ChoosePriest(s, p, tactic, static_cast<std::uint32_t>(inCurrent), escapeUsed);
        if (rec.tac0 == TacticId::None)
            rec.tac0 = next;
        if (next != tactic)
        {
            if (next == TacticId::PriestEscape)
                escapeUsed = true;
            tactic = next;
            tacticSinceMs = in.nowMs;
            if (rec.segN < kMaxSegs)
            {
                curSeg = rec.segN++;
                rec.segs[curSeg] = TacticSeg{next, 0};
            }
            else
            {
                curSeg = kMaxSegs;
                ++rec.segDrop;
            }
        }
        return false;
    }

    void NoteKill()
    {
        if (active)
            ++rec.kills;
    }

    void NoteCast(CastKind kind)
    {
        if (!active)
            return;
        ++rec.casts;
        if (kind == CastKind::Shield)
            ++rec.shieldN;
        else if (kind == CastKind::Control)
            ++rec.ccN;
    }

    [[nodiscard]] bool Active() const { return active; }
    // The tactic the bot is (or, in observe mode, would be) running; None outside an engagement.
    [[nodiscard]] TacticId Current() const { return active && !pendingEnd ? tactic : TacticId::None; }
    [[nodiscard]] TacticId CreditId() const { return creditId; }
    [[nodiscard]] std::uint32_t CreditMs() const { return creditMs; }
    // Latest in-combat snapshot (with deathEtaMs); what the strategy triggers read while Current() != None.
    [[nodiscard]] EngagementSnapshot const& Last() const { return last; }

private:
    struct HpSample
    {
        std::uint64_t ms = 0;
        std::uint64_t hp = 0;
    };

    static std::uint32_t Max(std::uint32_t a, std::uint32_t b) { return a > b ? a : b; }

    void Start(TickInput const& in)
    {
        std::uint32_t const nextId = rec.engId + 1;
        rec = EngageRecord{};
        rec.engId = nextId;
        rec.hp0 = in.hpPct;
        rec.mp0 = in.manaPct;
        rec.pullRisk = in.pullRisk;
        if (everEnded)
        {
            rec.gapMs = static_cast<std::int64_t>(in.nowMs >= lastEndMs ? in.nowMs - lastEndMs : 0);
            rec.gapRestMs = static_cast<std::int64_t>(restMs);
        }
        restMs = 0;
        active = true;
        pendingEnd = false;
        firstSnap = true;
        startMs = in.nowMs;
        tactic = TacticId::None;
        tacticSinceMs = in.nowMs;
        escapeUsed = false;
        curSeg = kMaxSegs;
        ringN = 0;
        ringHead = 0;
    }

    void Finish(std::uint64_t endMs, bool died, EngageRecord& out)
    {
        rec.durMs = endMs >= startMs ? endMs - startMs : 0;
        if (died)
            rec.outcome = escapeUsed ? Outcome::DiedAfterEscape : Outcome::Died;
        else if (escapeUsed)
            rec.outcome = Outcome::Escaped;
        else
            rec.outcome = rec.kills ? Outcome::Win : Outcome::NoKill;
        out = rec;
        active = false;
        pendingEnd = false;
        everEnded = true;
        lastEndMs = endMs;
        tactic = TacticId::None;
    }

    // HP slope over the oldest sample within kSlopeWindowMs: ms until 0 at that rate; 0 = not dropping.
    [[nodiscard]] std::uint32_t DeathEtaMs(std::uint64_t nowMs, std::uint64_t hp) const
    {
        HpSample const* oldest = nullptr;
        for (std::size_t k = 0; k < ringN; ++k)
        {
            HpSample const& s = ring[k];
            if (nowMs < s.ms || nowMs - s.ms > kSlopeWindowMs)
                continue;
            if (!oldest || s.ms < oldest->ms)
                oldest = &s;
        }
        if (!oldest || nowMs - oldest->ms < kSlopeMinMs || oldest->hp <= hp)
            return 0;
        std::uint64_t const eta = hp * (nowMs - oldest->ms) / (oldest->hp - hp);
        return eta > 0xffffffffull ? 0xffffffffu : (eta ? static_cast<std::uint32_t>(eta) : 1);
    }

    EngageRecord rec;
    EngagementSnapshot last;
    bool active = false;
    bool pendingEnd = false;
    bool everEnded = false;
    bool hasLast = false;
    bool firstSnap = true;
    bool escapeUsed = false;
    std::uint64_t lastMs = 0;
    std::uint64_t startMs = 0;
    std::uint64_t endAtMs = 0;
    std::uint64_t lastEndMs = 0;
    std::uint64_t restMs = 0;
    std::uint64_t prevMana = 0;
    std::uint64_t prevHp = 0;
    TacticId tactic = TacticId::None;
    std::uint64_t tacticSinceMs = 0;
    std::size_t curSeg = kMaxSegs;
    TacticId creditId = TacticId::None;
    std::uint32_t creditMs = 0;
    HpSample ring[kHpRing] = {};
    std::size_t ringHead = 0;
    std::size_t ringN = 0;
};

// Trailing fields of the ledger `engage` event (",\"ecv\":1,..."). Field order is fixed; append only.
inline std::string FormatEngageFields(EngageRecord const& r, std::uint32_t cls, std::uint32_t tab, std::uint32_t arm)
{
    std::string out;
    out.reserve(512);
    auto u = [&out](char const* name, std::uint64_t v)
    {
        out += ",\"";
        out += name;
        out += "\":";
        out += std::to_string(v);
    };
    auto s = [&out](char const* name, std::int64_t v)
    {
        out += ",\"";
        out += name;
        out += "\":";
        out += std::to_string(v);
    };
    u("ecv", kEngageSchemaVersion);
    u("cls", cls);
    u("tab", tab);
    u("arm", arm);
    u("eng", r.engId);
    u("tac0", static_cast<std::uint32_t>(r.tac0));
    out += ",\"tacs\":[";
    for (std::uint32_t k = 0; k < r.segN && k < kMaxSegs; ++k)
    {
        if (k)
            out.push_back(',');
        out.push_back('[');
        out += std::to_string(static_cast<std::uint32_t>(r.segs[k].id));
        out.push_back(',');
        out += std::to_string(r.segs[k].ms);
        out.push_back(']');
    }
    out.push_back(']');
    u("tacs_drop", r.segDrop);
    u("mobs_max", r.mobsMax);
    u("adds", r.addsMax);
    u("elite_n", r.eliteMax);
    s("lvl_dmax", r.lvlDmax);
    u("load_max", r.loadMax);
    u("pvp", r.pvp ? 1 : 0);
    u("dur_ms", r.durMs);
    u("kills", r.kills);
    u("outcome", static_cast<std::uint32_t>(r.outcome));
    u("hp0", r.hp0);
    u("hp1", r.hp1);
    u("mp0", r.mp0);
    u("mp1", r.mp1);
    u("mana_spent", r.manaSpent);
    u("hp_lost", r.hpLost);
    u("casts", r.casts);
    u("wand_ms", r.wandMs);
    u("cc_n", r.ccN);
    u("shield_n", r.shieldN);
    s("gap_ms", r.gapMs);
    s("gap_rest_ms", r.gapRestMs);
    s("pull_risk", r.pullRisk);
    return out;
}
}  // namespace AutoWowTactics

#endif  // AUTOWOW_ENGAGEMENT_TRACKER_H
