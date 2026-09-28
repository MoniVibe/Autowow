/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

// Gathering rule 2026-09-28 runtime (policy: AutoWow/GatherScalePolicy.h). Every hook returns at once with its
// flag off:
//   AutoWow.Gather.YieldScale   MiscScript OnAfterLootTemplateProcess (herb / ore node, skinning corpse loot)
//   AutoWow.Gather.MultiGain    PlayerScript OnPlayerUpdateGatheringSkill
//   AutoWow.Craft.MultiGain     PlayerScript OnPlayerUpdateCraftingSkill
//   AutoWow.Professions.TrainRuns  PlayerScript OnPlayerLearnSpell: "[Professions] learn|rank_up" log
// (AutoWow.Gather.AnySkill is the core patch plus the bot-side gates.)

#include "AutoWowTrainPolicy.h"
#include "Creature.h"
#include "DBCStores.h"
#include "GameObject.h"
#include "GatherScalePolicy.h"
#include "Log.h"
#include "LootMgr.h"
#include "MiscScript.h"
#include "ObjectAccessor.h"
#include "Player.h"
#include "PlayerScript.h"
#include "PlayerbotAIConfig.h"
#include "SpellMgr.h"

namespace AutoWowGatherScale
{
namespace
{
// Herbalism / mining lock of a node: its skill line and lock skill. False for any other lock.
bool NodeLock(GameObject const* go, std::uint32_t& skill, std::uint32_t& req)
{
    LockEntry const* lock = go ? sLockStore.LookupEntry(go->GetGOInfo()->GetLockId()) : nullptr;
    if (!lock)
        return false;
    for (std::size_t j = 0; j < MAX_LOCK_CASE; ++j)
        if (lock->Type[j] == LOCK_KEY_SKILL &&
            (lock->Index[j] == LOCKTYPE_HERBALISM || lock->Index[j] == LOCKTYPE_MINING))
        {
            skill = lock->Index[j] == LOCKTYPE_HERBALISM ? kSkillHerbalism : kSkillMining;
            req = lock->Skill[j];
            return true;
        }
    return false;
}

class GatherScaleMiscScript : public MiscScript
{
public:
    GatherScaleMiscScript() : MiscScript("AutoWowGatherScaleMisc", {MISCHOOK_ON_AFTER_LOOT_TEMPLATE_PROCESS}) {}

    // Runs once per fill (Loot::FillLoot), after the core rolls and before the loot is sent: the skill is the
    // looter's pre-skill-up value, the same one the core gate compared.
    void OnAfterLootTemplateProcess(Loot* loot, LootTemplate const* /*tab*/, LootStore const& store, Player* owner,
                                    bool /*personal*/, bool /*noEmptyError*/, uint16 /*lootMode*/) override
    {
        if (!sPlayerbotAIConfig.autoWowGatherYieldScale || !loot || !owner)
            return;
        std::uint32_t skill = 0, req = 0;
        if (&store == &LootTemplates_Gameobject)
        {
            if (!NodeLock(loot->sourceGameObject, skill, req))
                return;
        }
        else if (&store == &LootTemplates_Skinning)
        {
            Creature* corpse = ObjectAccessor::GetCreature(*owner, loot->sourceWorldObjectGUID);
            if (!corpse)
                return;
            skill = corpse->GetCreatureTemplate()->GetRequiredLootSkill();
            req = SkinningRequired(corpse->GetLevel());
        }
        else
            return;
        if (!IsGatherSkill(skill))
            return;
        std::uint32_t const have = owner->GetSkillValue(skill);
        std::int32_t const d = Deficit(req, have);
        if (d <= 0)
            return;
        bool const scraps = Scraps(d);
        for (LootItem& item : loot->items)
        {
            if (scraps)
                if (std::uint32_t const scrap = ScrapItem(item.itemid))
                    item.itemid = scrap;
            item.count = static_cast<uint8>(ScaledCount(item.count, d));
        }
        LOG_DEBUG("playerbots", "[Gather] scale player={} skill={} req={} have={} d={} pct={} scraps={} stacks={}",
                  owner->GetName(), skill, req, have, d, YieldPct(d), scraps, loot->items.size());
    }
};

class GatherScalePlayerScript : public PlayerScript
{
public:
    GatherScalePlayerScript()
        : PlayerScript("AutoWowGatherScalePlayer", {PLAYERHOOK_ON_UPDATE_GATHERING_SKILL,
                                                    PLAYERHOOK_ON_UPDATE_CRAFTING_SKILL, PLAYERHOOK_ON_LEARN_SPELL})
    {
    }

    // Player::UpdateGatherSkill passes gray = red level + 100 (red = the node / corpse required skill).
    void OnPlayerUpdateGatheringSkill(Player* player, uint32 skillId, uint32 current, uint32 gray, uint32 /*green*/,
                                      uint32 /*yellow*/, uint32& gain) override
    {
        if (!sPlayerbotAIConfig.autoWowGatherMultiGain || !IsGatherSkill(skillId) || gray < 100)
            return;
        gain = CapGain(GatherGain(gain, Deficit(gray - 100, current)), current, player->GetPureMaxSkillValue(skillId));
    }

    // Orange = the skill below the recipe's yellow level (TrivialSkillLineRankLow, Player::UpdateCraftSkill).
    void OnPlayerUpdateCraftingSkill(Player* player, SkillLineAbilityEntry const* skill, uint32 current,
                                     uint32& gain) override
    {
        if (!sPlayerbotAIConfig.autoWowCraftMultiGain || !skill)
            return;
        gain = CapGain(CraftGain(gain, current, skill->TrivialSkillLineRankLow), current,
                       player->GetPureMaxSkillValue(skill->SkillLine));
    }

    // A skill-granting spell (profession rank): the core sets the skill before this hook fires.
    void OnPlayerLearnSpell(Player* player, uint32 spellId) override
    {
        if (!sPlayerbotAIConfig.autoWowProfessionsTrainRuns || !player)
            return;
        SpellLearnSkillNode const* node = sSpellMgr->GetSpellLearnSkill(spellId);
        if (!node || !AutoWowTrainPolicy::IsProfessionSkillLine(node->skill))
            return;
        bool const first = sSpellMgr->GetPrevSpellInChain(spellId) == 0;
        LOG_INFO("playerbots", "[Professions] {} bot={} skill={} value={} max={} spell={}", first ? "learn" : "rank_up",
                 player->GetName(), node->skill, player->GetPureSkillValue(node->skill),
                 player->GetPureMaxSkillValue(node->skill), spellId);
    }
};
}  // namespace

void AddScripts()
{
    new GatherScaleMiscScript();
    new GatherScalePlayerScript();
}
}  // namespace AutoWowGatherScale
