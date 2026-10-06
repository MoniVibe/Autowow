/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "EquipAction.h"
#include "Event.h"
#include "GearUpgradePolicy.h"
#include "ItemCountValue.h"
#include "ItemPackets.h"
#include "ItemUsageValue.h"
#include "ItemVisitors.h"
#include "Log.h"
#include "Playerbots.h"
#include "RandomItemMgr.h"
#include "StatsWeightCalculator.h"
#include "Timer.h"
#include <mutex>
#include <unordered_map>
#include <utility>

namespace
{
std::string RaidLootSourceName(std::string source)
{
    if (source.empty())
        return "equip_upgrade";

    for (char& character : source)
    {
        bool const safe = (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                          (character >= '0' && character <= '9') || character == '_' || character == '-';
        if (!safe)
            character = '_';
    }
    return source;
}

void LogRaidLootEquip(Player* bot, Item* expectedItem, uint8 slot, uint32 previousItemId,
    uint32 previousItemGuidCounter)
{
    Item* equipped = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, slot);
    if (!equipped || !expectedItem || equipped->GetGUID() != expectedItem->GetGUID())
        return;

    LOG_INFO("playerbots",
        "[RaidLoot] event=equip bot={} bot_guid={} bot_guid_counter={} item={} entry={} item_guid={} "
        "item_guid_counter={} slot={} previous_item={} previous_item_guid_counter={} result=equipped",
        bot->GetName(), bot->GetGUID().GetRawValue(), bot->GetGUID().GetCounter(), equipped->GetTemplate()->ItemId,
        equipped->GetTemplate()->ItemId, equipped->GetGUID().GetRawValue(), equipped->GetGUID().GetCounter(),
        static_cast<uint32>(slot), previousItemId, previousItemGuidCounter);
}

// AutoWow.Gear.EquipBagUpgrades: per-bot period gate for the force-equip pass (also rate-limits its [GearEquip] log).
// ponytail: global mutex over a tiny guid->ms map; fine at bot cadence, shard it only if it ever shows on a profile.
bool BagOverrideDue(Player* bot)
{
    static std::mutex mutex;
    static std::unordered_map<uint32, uint32> lastMs;  // bot guid counter -> last pass getMSTime()
    uint32 const now = getMSTime();
    uint32 const period = AutoWowGear::GetEquipBag().tickMs;
    uint32 const guid = bot->GetGUID().GetCounter();
    std::lock_guard<std::mutex> lock(mutex);
    auto it = lastMs.find(guid);
    if (it != lastMs.end() && getMSTimeDiff(it->second, now) < period)
        return false;
    lastMs[guid] = now;
    return true;
}

// AutoWow.Gear.EquipBagUpgrades: the single equip slot a bag piece targets and its kind, but only for the slots the
// stock EquipItem places unconditionally (no score re-gate) -- body armor, cloak, neck, ranged. Returns false for
// weapons, rings, trinkets and shirt / tabard (those keep deferring to the stock scorer). `wornSlot` is where to read
// the currently worn piece.
bool ClassifyForcibleSlot(PlayerbotAI* botAI, ItemTemplate const* proto, AutoWowGear::EquipSlotKind& kind,
                          uint8& wornSlot)
{
    using K = AutoWowGear::EquipSlotKind;
    uint32 const inv = proto->InventoryType;
    if (inv == INVTYPE_RANGED || inv == INVTYPE_THROWN || inv == INVTYPE_RANGEDRIGHT)
    {
        kind = K::Ranged;
        wornSlot = EQUIPMENT_SLOT_RANGED;
        return true;
    }

    uint8 const dstSlot = botAI->FindEquipSlot(proto, NULL_SLOT, true);
    switch (dstSlot)
    {
        case EQUIPMENT_SLOT_HEAD:
        case EQUIPMENT_SLOT_SHOULDERS:
        case EQUIPMENT_SLOT_CHEST:
        case EQUIPMENT_SLOT_WAIST:
        case EQUIPMENT_SLOT_LEGS:
        case EQUIPMENT_SLOT_FEET:
        case EQUIPMENT_SLOT_WRISTS:
        case EQUIPMENT_SLOT_HANDS:
            if (proto->Class != ITEM_CLASS_ARMOR)
                return false;  // a weapon that somehow maps here: leave it to the stock scorer
            kind = K::Armor;
            wornSlot = dstSlot;
            return true;
        case EQUIPMENT_SLOT_BACK:
            kind = K::Cloak;
            wornSlot = dstSlot;
            return true;
        case EQUIPMENT_SLOT_NECK:
            kind = K::Accessory;
            wornSlot = dstSlot;
            return true;
        default:
            return false;  // main / off hand, fingers, trinkets, shirt, tabard: defer (EquipItem re-scores those)
    }
}

// AutoWow.Gear.EquipBagUpgrades: a bag piece the stock scan did not pick -- should the force pass equip it anyway?
// Mirrors QueryItemUsageForEquip's front gates, then applies the ilvl rule (DecideEquipBag) in place of the score rule.
AutoWowGear::EquipWhy ForcibleBagUpgrade(PlayerbotAI* botAI, Player* bot, ItemTemplate const* proto, uint8& slot,
                                         uint32& wornId, uint32& wornIlvl)
{
    using namespace AutoWowGear;
    slot = 0;
    wornId = 0;
    wornIlvl = 0;
    if (proto->InventoryType == INVTYPE_NON_EQUIP || proto->Class == ITEM_CLASS_CONTAINER ||
        proto->Class == ITEM_CLASS_QUIVER ||
        (proto->Class == ITEM_CLASS_WEAPON && proto->SubClass == ITEM_SUBCLASS_WEAPON_MISC))
        return EquipWhy::None;
    if (bot->BotCanUseItem(proto) != EQUIP_ERR_OK)
        return EquipWhy::None;

    // unique-equippable already worn: skip (CanEquipItem would reject it).
    if (proto->HasFlag(ITEM_FLAG_UNIQUE_EQUIPPABLE) &&
        bot->GetItemCount(proto->ItemId, true) > bot->GetItemCount(proto->ItemId, false))
        return EquipWhy::None;

    EquipSlotKind kind;
    uint8 wornSlot;
    if (!ClassifyForcibleSlot(botAI, proto, kind, wornSlot))
        return EquipWhy::None;

    // Ranged slot: the class must actually be proficient with this ranged weapon subclass (bow / gun / crossbow /
    // wand / thrown). BotCanUseItem can admit a ranged piece the stock equip then fails to place (S122: Saewash /
    // Lythalis retried an empty ranged slot 30+ times); CanEquipWeapon is the same proficiency gate the scorer uses.
    if (kind == EquipSlotKind::Ranged &&
        (proto->Class != ITEM_CLASS_WEAPON || !sRandomItemMgr.CanEquipWeapon(proto, bot->getClass())))
        return EquipWhy::None;
    slot = wornSlot;

    Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, wornSlot);
    EquipBagFacts f;
    f.bagIlvl = proto->ItemLevel;
    f.wornIlvl = worn ? worn->GetTemplate()->ItemLevel : 0;
    f.usable = true;
    f.kind = kind;
    f.bestArmorType = kind == EquipSlotKind::Armor &&
                      sRandomItemMgr.CanEquipArmor(proto, bot->getClass(), bot->GetLevel());
    f.stockUpgrade = false;  // caller only reaches here when the stock scan rejected the piece
    if (worn)
    {
        wornId = worn->GetTemplate()->ItemId;
        wornIlvl = f.wornIlvl;
    }
    return DecideEquipBag(GetEquipBag(), f);
}

// AutoWow.Gear.EquipBagUpgrades fail backoff (S122: an equip that never sticks was re-forced every pass, 30+ times).
// A force attempt is recorded; if the same (bot, item entry) is up for forcing again on the next pass it means the
// last attempt did not take (a successful equip leaves the bags, so it would not be evaluated), and the piece is
// parked for failBackoffMs. Returns false to skip this piece. ponytail: global mutex over a small map, pruned lazily.
bool BagForceBackedOff(Player* bot, uint32 entry)
{
    struct Fail { uint32 attempts; uint32 untilMs; };
    static std::mutex mutex;
    static std::unordered_map<uint64, Fail> fails;  // (guid<<32)|entry -> state
    uint32 const now = getMSTime();
    uint32 const backoff = AutoWowGear::GetEquipBag().failBackoffMs;
    uint64 const key = (static_cast<uint64>(bot->GetGUID().GetCounter()) << 32) | entry;
    std::lock_guard<std::mutex> lock(mutex);
    auto it = fails.find(key);
    if (it != fails.end())
    {
        if (it->second.untilMs && static_cast<int32>(it->second.untilMs - now) > 0)
            return true;  // still parked (signed diff: wrap-safe for a < 24-day backoff)
        if (it->second.untilMs)  // backoff elapsed: one more attempt
        {
            it->second = {1, 0};
            return false;
        }
        // seen last pass and still here -> the previous attempt did not stick: park it.
        it->second = {it->second.attempts + 1, static_cast<uint32>(now + backoff)};
        return true;
    }
    fails.emplace(key, Fail{1, 0});  // first attempt
    return false;
}

// AutoWow.Gear.EquipBagUpgrades veto (S122): refuse a stock EQUIP / REPLACE that would swap a worn piece back out for
// a lower / off-type bag candidate the force pass protects. Runs on every scan when the flag is on (the swap-back
// fires on the frequent stock triggers, not just the 90s force pass). Only the armor / cloak / neck / ranged slots the
// force pass owns are guarded; weapons / rings / trinkets keep the stock behaviour.
bool VetoStockReplace(PlayerbotAI* botAI, Player* bot, ItemTemplate const* candProto)
{
    using namespace AutoWowGear;
    EquipSlotKind kind;
    uint8 wornSlot;
    if (!ClassifyForcibleSlot(botAI, candProto, kind, wornSlot))
        return false;
    Item* worn = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, wornSlot);
    if (!worn)
        return false;  // empty slot: let the stock equip fill it
    ItemTemplate const* wornProto = worn->GetTemplate();
    bool const wornUsable = bot->BotCanUseItem(wornProto) == EQUIP_ERR_OK;
    bool const wornBestType = kind == EquipSlotKind::Armor &&
                              sRandomItemMgr.CanEquipArmor(wornProto, bot->getClass(), bot->GetLevel());
    return KeepsWornOverCandidate(GetEquipBag(), wornProto->ItemLevel, wornUsable, wornBestType, candProto->ItemLevel,
                                  kind);
}
}

bool EquipAction::Execute(Event event)
{
    std::string const text = event.getParam();
    ItemIds ids = chat->parseItems(text);
    EquipItems(ids);
    return true;
}

void EquipAction::EquipItems(ItemIds ids)
{
    for (ItemIds::iterator i = ids.begin(); i != ids.end(); i++)
    {
        FindItemByIdVisitor visitor(*i);
        EquipItem(&visitor);
    }
}

// Return bagslot with smalest bag.
uint8 EquipAction::GetSmallestBagSlot()
{
    int8 curBag = 0;
    uint32 curSlots = 0;
    for (uint8 bag = INVENTORY_SLOT_BAG_START; bag < INVENTORY_SLOT_BAG_END; ++bag)
    {
        Bag const* const pBag = (Bag*)bot->GetItemByPos(INVENTORY_SLOT_BAG_0, bag);
        if (pBag)
        {
            if (curBag > 0 && curSlots < pBag->GetBagSize())
                continue;

            curBag = bag;
            curSlots = pBag->GetBagSize();
        }
        else
            return bag;
    }

    return curBag;
}

void EquipAction::EquipItem(FindItemVisitor* visitor)
{
    IterateItems(visitor);
    std::vector<Item*> items = visitor->GetResult();
    if (!items.empty())
        EquipItem(*items.begin());
}

void EquipAction::EquipItem(Item* item)
{
    uint8 bagIndex = item->GetBagSlot();
    uint8 slot = item->GetSlot();
    ItemTemplate const* itemProto = item->GetTemplate();
    uint32 itemId = itemProto->ItemId;
    uint8 invType = itemProto->InventoryType;

    // Handle ammunition separately
    if (invType == INVTYPE_AMMO)
    {
        bot->SetAmmo(itemId);
        std::ostringstream out;
        out << "equipping " << chat->FormatItem(itemProto);
        botAI->TellMaster(out);
        return;
    }

    // Handle bags first
    bool equippedBag = false;
    if (itemProto->Class == ITEM_CLASS_CONTAINER)
    {
        // Attempt to equip as a bag
        uint8 newBagSlot = GetSmallestBagSlot();

        if (newBagSlot > 0)
        {
            uint16 src = ((bagIndex << 8) | slot);
            uint16 dst = ((INVENTORY_SLOT_BAG_0 << 8) | newBagSlot);
            bot->SwapItem(src, dst);
            equippedBag = true;
        }
    }

    // If we didn't equip as a bag, try to equip as gear
    if (!equippedBag)
    {
        // Ranged weapons aren't handled by the rest of the weapon equip logic
        // Handle them early here to avoid issues.
        if (invType == INVTYPE_RANGED || invType == INVTYPE_THROWN || invType == INVTYPE_RANGEDRIGHT)
        {
            Item* previous = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_RANGED);
            uint32 const previousItemId = previous ? previous->GetTemplate()->ItemId : 0;
            uint32 const previousItemGuidCounter = previous ? previous->GetGUID().GetCounter() : 0;
            WorldPacket packet(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
            ObjectGuid itemguid = item->GetGUID();
            packet << itemguid << uint8(EQUIPMENT_SLOT_RANGED);

            WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(packet));
            nicePacket.Read();
            bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);
            LogRaidLootEquip(bot, item, EQUIPMENT_SLOT_RANGED, previousItemId, previousItemGuidCounter);

            std::ostringstream out;
            out << "Equipping " << chat->FormatItem(itemProto) << " in ranged slot";
            botAI->TellMaster(out);
            return;
        }

        uint8 dstSlot = botAI->FindEquipSlot(itemProto, NULL_SLOT, true);

        // Check if the item is a weapon and whether the bot can dual wield or use Titan Grip
        bool isWeapon = (itemProto->Class == ITEM_CLASS_WEAPON);
        bool canTitanGrip = bot->CanTitanGrip();
        bool canDualWield = bot->CanDualWield();

        bool isTwoHander = (invType == INVTYPE_2HWEAPON);
        bool isValidTGWeapon = false;
        if (canTitanGrip && isTwoHander)
        {
            // Titan Grip-valid 2H weapon subclasses: Axe2, Mace2, Sword2
            isValidTGWeapon = (itemProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                               itemProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                               itemProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2);
        }

        // Check if the main hand currently has a 2H weapon equipped
        Item* currentMHItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
        bool have2HWeaponEquipped = (currentMHItem && currentMHItem->GetTemplate()->InventoryType == INVTYPE_2HWEAPON);

        // bool canDualWieldOrTG = (canDualWield || (canTitanGrip && isTwoHander));
        bool canDualWieldOrTG = (canDualWield || isTwoHander);

        // If this is a weapon and we can dual wield or Titan Grip, check if we can improve main/off-hand setup
        if (isWeapon && canDualWieldOrTG)
        {
            // Fetch current main hand and offhand items
            Item* mainHandItem = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_MAINHAND);
            Item* offHandItem  = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, EQUIPMENT_SLOT_OFFHAND);

            // Set up the stats calculator once and reuse results for performance
            StatsWeightCalculator calculator(bot);
            calculator.SetItemSetBonus(false);
            calculator.SetOverflowPenalty(false);

            // Calculate item scores once and store them
            float newItemScore = calculator.CalculateItem(itemId, item->GetItemRandomPropertyId());
            float mainHandScore = mainHandItem
                ? calculator.CalculateItem(mainHandItem->GetTemplate()->ItemId, mainHandItem->GetItemRandomPropertyId()) : 0.0f;
            float offHandScore = offHandItem
                ? calculator.CalculateItem(offHandItem->GetTemplate()->ItemId, offHandItem->GetItemRandomPropertyId()) : 0.0f;

            // Determine where this weapon can go
            bool canGoMain = (invType == INVTYPE_WEAPON ||
                              invType == INVTYPE_WEAPONMAINHAND ||
                              isTwoHander);

            bool canTGOff = false;
            if (canTitanGrip && isTwoHander && isValidTGWeapon)
                canTGOff = true;

            bool canGoOff = (invType == INVTYPE_WEAPON ||
                             invType == INVTYPE_WEAPONOFFHAND ||
                             canTGOff);

            // Check if the main hand item can go to offhand if needed
            bool mainHandCanGoOff = false;
            if (mainHandItem)
            {
                ItemTemplate const* mhProto = mainHandItem->GetTemplate();
                bool mhIsValidTG = false;
                if (canTitanGrip && mhProto->InventoryType == INVTYPE_2HWEAPON)
                {
                    mhIsValidTG = (mhProto->SubClass == ITEM_SUBCLASS_WEAPON_AXE2 ||
                                   mhProto->SubClass == ITEM_SUBCLASS_WEAPON_MACE2 ||
                                   mhProto->SubClass == ITEM_SUBCLASS_WEAPON_SWORD2);
                }

                mainHandCanGoOff = (mhProto->InventoryType == INVTYPE_WEAPON ||
                                    mhProto->InventoryType == INVTYPE_WEAPONOFFHAND ||
                                    (mhProto->InventoryType == INVTYPE_2HWEAPON && mhIsValidTG));
            }

            // Priority 1: Replace main hand if the new weapon is strictly better
            // and if conditions allow (e.g. no conflicting 2H logic)
            bool betterThanMH = (newItemScore > mainHandScore);
            // If a one-handed weapon is better, we can still use it instead of a two-handed weapon
            bool mhConditionOK = (invType != INVTYPE_2HWEAPON ||
                      (isTwoHander && !canTitanGrip) ||
                      (canTitanGrip && isValidTGWeapon));

            if (canGoMain && betterThanMH && mhConditionOK)
            {
                uint32 const previousItemId = mainHandItem ? mainHandItem->GetTemplate()->ItemId : 0;
                uint32 const previousItemGuidCounter = mainHandItem ? mainHandItem->GetGUID().GetCounter() : 0;
                // Equip new weapon in main hand
                {
                    WorldPacket eqPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                    ObjectGuid newItemGuid = item->GetGUID();
                    eqPacket << newItemGuid << uint8(EQUIPMENT_SLOT_MAINHAND);
                    WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(eqPacket));
                    nicePacket.Read();
                    bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);
                }
                LogRaidLootEquip(bot, item, EQUIPMENT_SLOT_MAINHAND, previousItemId, previousItemGuidCounter);

                // Try moving old main hand weapon to offhand if beneficial
                if (mainHandItem && mainHandCanGoOff && (!offHandItem || mainHandScore > offHandScore))
                {
                    ItemTemplate const* oldMHProto = mainHandItem->GetTemplate();

                    WorldPacket offhandPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                    ObjectGuid oldMHGuid = mainHandItem->GetGUID();
                    offhandPacket << oldMHGuid << uint8(EQUIPMENT_SLOT_OFFHAND);
                    WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(offhandPacket));
                    nicePacket.Read();
                    bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);

                    std::ostringstream moveMsg;
                    moveMsg << "Main hand upgrade found. Moving " << chat->FormatItem(oldMHProto) << " to offhand";
                    botAI->TellMaster(moveMsg);
                }

                std::ostringstream out;
                out << "Equipping " << chat->FormatItem(itemProto) << " in main hand";
                botAI->TellMaster(out);
                return;
            }

            // Priority 2: If not better than main hand, check if better than offhand
            else if (canGoOff && newItemScore > offHandScore)
            {
                uint32 const previousItemId = offHandItem ? offHandItem->GetTemplate()->ItemId : 0;
                uint32 const previousItemGuidCounter = offHandItem ? offHandItem->GetGUID().GetCounter() : 0;
                // Equip in offhand
                WorldPacket eqPacket(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
                ObjectGuid newItemGuid = item->GetGUID();
                eqPacket << newItemGuid << uint8(EQUIPMENT_SLOT_OFFHAND);
                WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(eqPacket));
                nicePacket.Read();
                bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);
                LogRaidLootEquip(bot, item, EQUIPMENT_SLOT_OFFHAND, previousItemId, previousItemGuidCounter);

                std::ostringstream out;
                out << "Equipping " << chat->FormatItem(itemProto) << " in offhand";
                botAI->TellMaster(out);
                return;
            }
            else
            {
                // No improvement, do nothing
                return;
            }
        }

        // If not a special dual-wield/TG scenario or no improvement found, fall back to original logic
        if (dstSlot == EQUIPMENT_SLOT_FINGER1 ||
            dstSlot == EQUIPMENT_SLOT_TRINKET1 ||
            (dstSlot == EQUIPMENT_SLOT_MAINHAND && canDualWield &&
                ((invType != INVTYPE_2HWEAPON && !have2HWeaponEquipped) || (canTitanGrip && isValidTGWeapon))))
        {
            // Handle ring/trinket dual-slot logic
            Item* const equippedItems[2] = {
                bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot),
                bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot + 1)
            };

            if (equippedItems[0])
            {
                if (equippedItems[1])
                {
                    // Both slots are full - pick the worst item to replace, but only if new item is better
                    StatsWeightCalculator calc(bot);
                    calc.SetItemSetBonus(false);
                    calc.SetOverflowPenalty(false);

                    // Calculate new item score with random properties
                    int32 newItemRandomProp = item->GetItemRandomPropertyId();
                    float newItemScore = calc.CalculateItem(itemId, newItemRandomProp);

                    // Calculate equipped items scores with random properties
                    int32 firstRandomProp = equippedItems[0]->GetItemRandomPropertyId();
                    int32 secondRandomProp = equippedItems[1]->GetItemRandomPropertyId();
                    float firstItemScore = calc.CalculateItem(equippedItems[0]->GetTemplate()->ItemId, firstRandomProp);
                    float secondItemScore = calc.CalculateItem(equippedItems[1]->GetTemplate()->ItemId, secondRandomProp);

                    // Determine which slot (if any) should be replaced
                    bool betterThanFirst = newItemScore > firstItemScore;
                    bool betterThanSecond = newItemScore > secondItemScore;

                    // Early return if new item is not better than either equipped item
                    if (!betterThanFirst && !betterThanSecond)
                        return;

                    if (betterThanFirst && betterThanSecond)
                    {
                        // New item is better than both - replace the worse of the two equipped items
                        if (firstItemScore > secondItemScore)
                            dstSlot++; // Replace second slot (worse)
                        // else: keep dstSlot as-is (replace first slot)
                    }
                    else if (betterThanSecond)
                        dstSlot++; // Only better than second slot - replace it
                }
                else
                {
                    // Second slot empty, use it
                    dstSlot++;
                }
            }
        }

        // Equip the item in the chosen slot
        Item* previous = bot->GetItemByPos(INVENTORY_SLOT_BAG_0, dstSlot);
        uint32 const previousItemId = previous ? previous->GetTemplate()->ItemId : 0;
        uint32 const previousItemGuidCounter = previous ? previous->GetGUID().GetCounter() : 0;
        {
            WorldPacket packet(CMSG_AUTOEQUIP_ITEM_SLOT, 2);
            ObjectGuid itemguid = item->GetGUID();
            packet << itemguid << dstSlot;
            WorldPackets::Item::AutoEquipItemSlot nicePacket(std::move(packet));
            nicePacket.Read();
            bot->GetSession()->HandleAutoEquipItemSlotOpcode(nicePacket);
        }
        LogRaidLootEquip(bot, item, dstSlot, previousItemId, previousItemGuidCounter);
    }

    std::ostringstream out;
    out << "Equipping " << chat->FormatItem(itemProto);
    botAI->TellMaster(out);
}

ItemIds EquipAction::SelectInventoryItemsToEquip(std::string const& source)
{
    CollectItemsVisitor visitor;
    IterateItems(&visitor, ITERATE_ITEMS_IN_BAGS);

    std::string const telemetrySource = RaidLootSourceName(source);
    // AutoWow.Gear.EquipBagUpgrades: run the far-below-ilvl force pass at most once per EquipBagTickMs, out of combat.
    bool const runBagOverride =
        AutoWowGear::EquipBagEnabled() && !bot->IsInCombat() && BagOverrideDue(bot);
    // The swap-back veto runs on every scan while the flag is on (the stock swap-back fires on the frequent triggers,
    // not just the throttled force pass).
    bool const vetoSwapBack = AutoWowGear::EquipBagEnabled();
    ItemIds items;
    for (auto i = visitor.items.begin(); i != visitor.items.end(); ++i)
    {
        Item* item = *i;
        if (!item)
            continue;

        ItemTemplate const* itemTemplate = item->GetTemplate();
        if (!itemTemplate)
            continue;

        //TODO Expand to Glyphs and Gems, that can be placed in equipment
        //Pre-filter non-equipable items
        if (itemTemplate->InventoryType == INVTYPE_NON_EQUIP)
            continue;

        int32 randomProperty = item->GetItemRandomPropertyId();
        uint32 itemId = item->GetTemplate()->ItemId;
        std::string itemUsageParam;
        if (randomProperty != 0)
            itemUsageParam = std::to_string(itemId) + "," + std::to_string(randomProperty);
        else
            itemUsageParam = std::to_string(itemId);

        ItemUsage usage = AI_VALUE2(ItemUsage, "item upgrade", itemUsageParam);
        if (usage == ITEM_USAGE_EQUIP || usage == ITEM_USAGE_REPLACE || usage == ITEM_USAGE_BAD_EQUIP)
        {
            // AutoWow.Gear.EquipBagUpgrades: refuse a stock swap that would downgrade a worn piece the force pass
            // protects (otherwise the two scans ping-pong the same slot every tick).
            if (vetoSwapBack && VetoStockReplace(botAI, bot, itemTemplate))
                continue;
            LOG_INFO("playerbots",
                "[RaidLoot] event=evaluate trigger=inventory_upgrade_scan source={} bot={} bot_guid={} "
                "bot_guid_counter={} item={} entry={} item_guid={} item_guid_counter={} usage={} selected=true",
                telemetrySource, bot->GetName(), bot->GetGUID().GetRawValue(), bot->GetGUID().GetCounter(), itemId,
                itemId, item->GetGUID().GetRawValue(), item->GetGUID().GetCounter(), static_cast<uint32>(usage));
            items.insert(itemId);
        }
        else if (runBagOverride)
        {
            // AutoWow.Gear.EquipBagUpgrades: the stock scan rejected it (score not above the worn piece), but it is a
            // usable far-below-ilvl upgrade for a slot EquipItem places unconditionally -- force it on.
            uint8 slot = 0;
            uint32 wornId = 0;
            uint32 wornIlvl = 0;
            AutoWowGear::EquipWhy const why = ForcibleBagUpgrade(botAI, bot, itemTemplate, slot, wornId, wornIlvl);
            if ((why == AutoWowGear::EquipWhy::Empty || why == AutoWowGear::EquipWhy::Ilvl) &&
                !BagForceBackedOff(bot, itemId))
            {
                items.insert(itemId);
                LOG_INFO("playerbots", "[GearEquip] bot={} slot={} old={}({}) new={}({}) why={}", bot->GetName(),
                    static_cast<uint32>(slot), wornId, wornIlvl, itemId, itemTemplate->ItemLevel,
                    AutoWowGear::EquipWhyName(why));
            }
        }
    }
    return items;
}

bool EquipUpgradesPacketAction::Execute(Event event)
{
    if (!sPlayerbotAIConfig.autoEquipUpgradeLoot && !sRandomPlayerbotMgr.IsRandomBot(bot))
        return false;
    std::string const source = event.GetSource();
    if (source == "trade status")
    {
        WorldPacket p(event.getPacket());
        p.rpos(0);
        uint32 status;
        p >> status;

        if (status != TRADE_STATUS_TRADE_ACCEPT)
            return false;
    }

    else if (source == "item push result")
    {
        WorldPacket p(event.getPacket());
        p.rpos(0);
        ObjectGuid playerGuid;
        uint32 received, created, sendChatMessage, itemSlot, itemId;
        uint8 bagSlot;

        p >> playerGuid;
        p >> received;
        p >> created;
        p >> sendChatMessage;
        p >> bagSlot;
        p >> itemSlot;
        p >> itemId;

        ItemTemplate const* item = sObjectMgr->GetItemTemplate(itemId);
        if (item->InventoryType == INVTYPE_NON_EQUIP)
            return false;
    }

    ItemIds items = SelectInventoryItemsToEquip(source);
    EquipItems(items);
    return true;
}

bool EquipUpgradeAction::Execute(Event event)
{
    ItemIds items = SelectInventoryItemsToEquip(event.GetSource());
    EquipItems(items);
    return true;
}
