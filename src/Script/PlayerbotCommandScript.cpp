/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "AccountMgr.h"
#include "AutoWowCohortPolicy.h"
#include "BattleGroundTactics.h"
#include "CharacterCache.h"
#include "Chat.h"
#include "GuildTaskMgr.h"
#include "PerfMonitor.h"
#include "PlayerbotMgr.h"
#include "RandomPlayerbotFactory.h"
#include "RandomPlayerbotMgr.h"
#include "ScriptMgr.h"
#include "WorldSessionMgr.h"

using namespace Acore::ChatCommands;

class playerbots_commandscript : public CommandScript
{
public:
    playerbots_commandscript() : CommandScript("playerbots_commandscript") {}

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable playerbotsDebugCommandTable = {
            {"bg", HandleDebugBGCommand, SEC_GAMEMASTER, Console::Yes},
        };

        static ChatCommandTable playerbotsAccountCommandTable = {
            {"setKey", HandleSetSecurityKeyCommand, SEC_PLAYER, Console::No},
            {"link", HandleLinkAccountCommand, SEC_PLAYER, Console::No},
            {"linkedAccounts", HandleViewLinkedAccountsCommand, SEC_PLAYER, Console::No},
            {"unlink", HandleUnlinkAccountCommand, SEC_PLAYER, Console::No},
        };

        static ChatCommandTable playerbotsCommandTable = {
            {"bot", HandlePlayerbotCommand, SEC_PLAYER, Console::No},
            {"gtask", HandleGuildTaskCommand, SEC_GAMEMASTER, Console::Yes},
            {"pmon", HandlePerfMonCommand, SEC_GAMEMASTER, Console::Yes},
            {"rndbot", HandleRandomPlayerbotCommand, SEC_GAMEMASTER, Console::Yes},
            {"debug", playerbotsDebugCommandTable},
            {"account", playerbotsAccountCommandTable},
        };

        // Console-only (SEC_CONSOLE + explicit session refusal); never exposed to players or the bridge.
        static ChatCommandTable autowowCohortCommandTable = {
            {"create", HandleAutoWowCohortCreateCommand, SEC_CONSOLE, Console::Yes},
        };

        static ChatCommandTable autowowCommandTable = {
            {"cohort", autowowCohortCommandTable},
        };

        static ChatCommandTable commandTable = {
            {"playerbots", playerbotsCommandTable},
            {"autowow", autowowCommandTable},
        };

        return commandTable;
    }

    static bool HandlePlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return PlayerbotMgr::HandlePlayerbotMgrCommand(handler, args);
    }

    static bool HandleRandomPlayerbotCommand(ChatHandler* handler, char const* args)
    {
        return RandomPlayerbotMgr::HandlePlayerbotConsoleCommand(handler, args);
    }

    static bool HandleGuildTaskCommand(ChatHandler* handler, char const* args)
    {
        return GuildTaskMgr::HandleConsoleCommand(handler, args);
    }

    static bool HandlePerfMonCommand(ChatHandler* /*handler*/, char const* args)
    {
        if (!strcmp(args, "reset"))
        {
            sPerfMonitor.Reset();
            return true;
        }

        if (!strcmp(args, "tick"))
        {
            sPerfMonitor.PrintStats(true, false);
            sPerfMonitor.DumpJson(true);
            return true;
        }

        if (!strcmp(args, "stack"))
        {
            sPerfMonitor.PrintStats(false, true);
            sPerfMonitor.DumpJson(false);
            return true;
        }

        if (!strcmp(args, "toggle"))
        {
            sPlayerbotAIConfig.perfMonEnabled = !sPlayerbotAIConfig.perfMonEnabled;
            if (sPlayerbotAIConfig.perfMonEnabled)
                LOG_INFO("playerbots", "Performance monitor enabled");
            else
                LOG_INFO("playerbots", "Performance monitor disabled");
            return true;
        }

        sPerfMonitor.PrintStats();
        sPerfMonitor.DumpJson(false);
        return true;
    }

    // .autowow cohort create <account> <raceId> <classId> <gender 0|1> <name>
    // Creates one level-1 character on an EXISTING account through RandomPlayerbotFactory's
    // Player::Create path (stock start position, gear and spells). Never creates accounts.
    // Output (one line): "cohort create: created|exists|refused name=<n> ... [reason=<r>]".
    static bool HandleAutoWowCohortCreateCommand(ChatHandler* handler, char const* args)
    {
        using namespace AutoWowCohortPolicy;

        auto refuse = [handler](std::string const& name, char const* reason)
        {
            handler->PSendSysMessage("cohort create: refused name={} reason={}", name, reason);
            LOG_INFO("playerbots", "AutoWoW cohort create: refused name={} reason={}", name, reason);
            return true;
        };

        if (handler->GetSession())
            return refuse("-", "console_only");

        CreateArgs req;
        if (!ParseCreateArgs(args ? std::string_view(args) : std::string_view(), req))
        {
            handler->SendSysMessage("Usage: .autowow cohort create <account> <raceId> <classId> <gender 0|1> <name>");
            return refuse("-", "usage");
        }

        CreateRefusal const argRefusal = CheckCreateArgs(req, sPlayerbotAIConfig.randomBotAccountPrefix);
        if (argRefusal != CreateRefusal::None)
            return refuse(req.name, CreateRefusalName(argRefusal));

        std::string accountName = req.account;
        Utf8ToUpperOnlyLatin(accountName);
        uint32 const accountId = AccountMgr::GetId(accountName);
        if (!accountId)
            return refuse(req.name, "account_missing");

        std::string name = req.name;
        if (!normalizePlayerName(name) || ObjectMgr::CheckPlayerName(name, true) != CHAR_NAME_SUCCESS)
            return refuse(req.name, "name_invalid");

        uint8 const race = static_cast<uint8>(req.race);
        uint8 const cls = static_cast<uint8>(req.cls);
        uint8 const gender = static_cast<uint8>(req.gender);

        // Idempotency: an existing character with this name is "exists" only when it is the same
        // account/race/class/gender; anything else is a name collision.
        ObjectGuid const existing = sCharacterCache->GetCharacterGuidByName(name);
        if (existing)
        {
            CharacterCacheEntry const* entry = sCharacterCache->GetCharacterCacheByGuid(existing);
            if (entry && entry->AccountId == accountId && entry->Race == race && entry->Class == cls &&
                entry->Sex == gender)
            {
                handler->PSendSysMessage("cohort create: exists name={} guid={} account={}", name,
                                         existing.GetCounter(), accountName);
                LOG_INFO("playerbots", "AutoWoW cohort create: exists name={} guid={} account={}", name,
                         existing.GetCounter(), accountName);
                return true;
            }
            return refuse(name, "name_taken");
        }
        CharacterDatabasePreparedStatement* nameStmt = CharacterDatabase.GetPreparedStatement(CHAR_SEL_CHECK_NAME);
        nameStmt->SetData(0, name);
        if (CharacterDatabase.Query(nameStmt))
            return refuse(name, "name_taken");

        if (!RandomPlayerbotFactory::IsValidRaceClassCombination(race, cls, sWorld->getIntConfig(CONFIG_EXPANSION)))
            return refuse(name, "race_class_invalid");
        if (((1 << (race - 1)) & sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_RACEMASK)) ||
            ((1 << (cls - 1)) & sWorld->getIntConfig(CONFIG_CHARACTER_CREATING_DISABLED_CLASSMASK)))
            return refuse(name, "race_class_disabled");

        // The temporary session's destructor writes account.online = 0 and totaltime, so never run
        // beside a live session of the same account (provisioning runs in a maintenance window).
        if (sWorldSessionMgr->FindSession(accountId))
            return refuse(name, "account_online");
        if (AccountMgr::GetCharactersCount(accountId) >= sWorld->getIntConfig(CONFIG_CHARACTERS_PER_REALM))
            return refuse(name, "realm_slots_full");

        uint32 totalTime = 0;
        if (QueryResult result = LoginDatabase.Query("SELECT totaltime FROM account WHERE id = {}", accountId))
            totalTime = (*result)[0].Get<uint32>();

        WorldSession* session = new WorldSession(accountId, "", 0x0, nullptr, SEC_PLAYER, EXPANSION_WRATH_OF_THE_LICH_KING,
                                                 time_t(0), LOCALE_enUS, 0, false, false, totalTime, true);
        Player* player = RandomPlayerbotFactory::CreateNamedCharacter(session, race, cls, gender, name);
        if (!player)
        {
            delete session;
            return refuse(name, "create_failed");
        }

        player->SaveToDB(true, false);
        sCharacterCache->AddCharacterCacheEntry(player->GetGUID(), accountId, player->GetName(), player->getGender(),
                                                player->getRace(), player->getClass(), player->GetLevel());
        uint32 const guid = player->GetGUID().GetCounter();
        uint32 const level = player->GetLevel();
        player->CleanupsBeforeDelete();
        delete player;
        delete session;

        handler->PSendSysMessage("cohort create: created name={} guid={} account={} race={} class={} gender={} level={}",
                                 name, guid, accountName, race, cls, gender, level);
        LOG_INFO("playerbots", "AutoWoW cohort create: created name={} guid={} account={} race={} class={} gender={} level={}",
                 name, guid, accountName, race, cls, gender, level);
        return true;
    }

    static bool HandleDebugBGCommand(ChatHandler* handler, char const* args)
    {
        return BGTactics::HandleConsoleCommand(handler, args);
    }

    static bool HandleSetSecurityKeyCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
        {
            handler->PSendSysMessage("Usage: .playerbots account setKey <securityKey>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();
        std::string key = args;

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleSetSecurityKeyCommand(player, key);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleLinkAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        char* accountName = strtok((char*)args, " ");
        char* key = strtok(nullptr, " ");

        if (!accountName || !key)
        {
            handler->PSendSysMessage("Usage: .playerbots account link <accountName> <securityKey>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleLinkAccountCommand(player, accountName, key);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleViewLinkedAccountsCommand(ChatHandler* handler, char const* /*args*/)
    {
        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleViewLinkedAccountsCommand(player);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }

    static bool HandleUnlinkAccountCommand(ChatHandler* handler, char const* args)
    {
        if (!args || !*args)
            return false;

        char* accountName = strtok((char*)args, " ");
        if (!accountName)
        {
            handler->PSendSysMessage("Usage: .playerbots account unlink <accountName>");
            return false;
        }

        Player* player = handler->GetSession()->GetPlayer();

        PlayerbotMgr* mgr = PlayerbotsMgr::instance().GetPlayerbotMgr(player);
        if (mgr)
        {
            mgr->HandleUnlinkAccountCommand(player, accountName);
            return true;
        }
        else
        {
            handler->PSendSysMessage("PlayerbotMgr instance not found.");
            return false;
        }
    }
};

void AddPlayerbotsCommandscripts() { new playerbots_commandscript(); }
