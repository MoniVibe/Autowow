/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#ifndef PLAYERBOTS_SHAREDVALUECONTEXT_H
#define PLAYERBOTS_SHAREDVALUECONTEXT_H

#include "LootValues.h"
#include "NamedObjectContext.h"
#include "PlayerbotAI.h"
#include "PvpValues.h"
#include "QuestValues.h"

#include <memory>

class SharedValueContext : public NamedObjectContext<UntypedValue>
{
public:
    static SharedValueContext& instance()
    {
        static SharedValueContext instance;

        return instance;
    }

    template <class T>
    Value<T>* getGlobalValue(std::string const name)
    {
        // Global values must be owned by this process-lifetime context. Building a temporary
        // SharedNamedObjectContextList here makes its destructor delete both this singleton and the
        // returned value before the caller can invoke Get(), leaving a dangling pointer.
        UntypedValue* value = NamedObjectContext<UntypedValue>::create(name, sharedBotAI.get());
        return dynamic_cast<Value<T>*>(value);
    }

    template <class T>
    Value<T>* getGlobalValue(std::string const name, std::string const param)
    {
        return getGlobalValue<T>((std::string(name) + "::" + param));
    }

    template <class T>
    Value<T>* getGlobalValue(std::string const name, uint32 param)
    {
        std::ostringstream out;
        out << param;
        return getGlobalValue<T>(name, out.str());
    }

private:
    SharedValueContext() : NamedObjectContext(true), sharedBotAI(std::make_unique<PlayerbotAI>())
    {
        creators["bg masters"] = &SharedValueContext::bg_masters;
        creators["drop map"] = &SharedValueContext::drop_map;
        creators["item drop list"] = &SharedValueContext::item_drop_list;
        creators["entry loot list"] = &SharedValueContext::entry_loot_list;

        creators["entry quest relation"] = &SharedValueContext::entry_quest_relation;
        creators["quest guidp map"] = &SharedValueContext::quest_guidp_map;
        creators["quest givers"] = &SharedValueContext::quest_givers;
    }
    ~SharedValueContext()
    {
        // Destroy the cached values while their shared PlayerbotAI owner is still alive. The base
        // destructor calls Clear() again, which is harmless after this first pass emptied the map.
        Clear();
    }

    SharedValueContext(SharedValueContext const&) = delete;
    SharedValueContext& operator=(SharedValueContext const&) = delete;

    SharedValueContext(SharedValueContext&&) = delete;
    SharedValueContext& operator=(SharedValueContext&&) = delete;

    static UntypedValue* bg_masters(PlayerbotAI* botAI) { return new BgMastersValue(botAI); }
    static UntypedValue* drop_map(PlayerbotAI* botAI) { return new DropMapValue(botAI); }
    static UntypedValue* item_drop_list(PlayerbotAI* botAI) { return new ItemDropListValue(botAI); }
    static UntypedValue* entry_loot_list(PlayerbotAI* botAI) { return new EntryLootListValue(botAI); }

    static UntypedValue* entry_quest_relation(PlayerbotAI* botAI) { return new EntryQuestRelationMapValue(botAI); }
    static UntypedValue* quest_guidp_map(PlayerbotAI* botAI) { return new QuestGuidpMapValue(botAI); }
    static UntypedValue* quest_givers(PlayerbotAI* botAI) { return new QuestGiversValue(botAI); }

    std::unique_ptr<PlayerbotAI> sharedBotAI;

};

#define sSharedValueContext SharedValueContext::instance()

#endif
