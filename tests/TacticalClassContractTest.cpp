/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TacticalClassTables.h"
#include "TacticalPolicy.h"

#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>

// Source contract of the non-priest tactical classes (TacticalClassStrategy.cpp + TacticalClassTables.h):
// every "tac ..." trigger a class table uses is registered in the shared TriggerContext, every action a class
// pushes or factors is registered in that class's action context (or the shared ActionContext), the class
// context registers "tactical" for its own family, the conf dist defaults equal the code defaults, and the
// strategy dispatch covers exactly the families that have a table row.
namespace
{
using namespace AutoWowTactics;

std::string Read(std::string const& rel)
{
    std::filesystem::path const path = std::filesystem::path(__FILE__).parent_path().parent_path() / rel;
    std::ifstream in(path, std::ios::in | std::ios::binary);
    EXPECT_TRUE(in.is_open()) << "Could not open " << path.string();
    return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

std::set<std::string> Captures(std::string const& text, std::string const& pattern)
{
    std::set<std::string> out;
    std::regex const re(pattern);
    for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it)
        out.insert((*it)[1].str());
    return out;
}

std::string ConfValue(std::string const& conf, std::string const& key)
{
    std::smatch m;
    std::regex const re("\n" + std::regex_replace(key, std::regex("\\."), "\\.") + " = \"?([^\"\n]*)\"?\n");
    return std::regex_search(conf, m, re) ? m[1].str() : std::string("<missing>");
}

// Text of `void <name>(...) { ... }` up to its closing brace at column 0; empty when absent.
std::string FunctionBody(std::string const& text, std::string const& signature)
{
    std::size_t const at = text.find(signature);
    if (at == std::string::npos)
        return {};
    std::size_t const end = text.find("\n}\n", at);
    return text.substr(at, end == std::string::npos ? std::string::npos : end - at);
}

struct FamilySource
{
    Family family;
    char const* context;
    char const* internal;
};

constexpr FamilySource kSources[] = {
    {Family::Warlock, "src/Ai/Class/Warlock/WarlockAiObjectContext.cpp", "WarlockAiObjectContextInternal"},
    {Family::Mage, "src/Ai/Class/Mage/MageAiObjectContext.cpp", "MageAiObjectContextInternal"},
    {Family::Hunter, "src/Ai/Class/Hunter/HunterAiObjectContext.cpp", "HunterAiObjectContextInternal"},
    {Family::Rogue, "src/Ai/Class/Rogue/RogueAiObjectContext.cpp", "RogueAiObjectContextInternal"},
    {Family::Warrior, "src/Ai/Class/Warrior/WarriorAiObjectContext.cpp", "WarriorAiObjectContextInternal"},
    {Family::Paladin, "src/Ai/Class/Paladin/PaladinAiObjectContext.cpp", "PaladinAiObjectContextInternal"},
    {Family::Druid, "src/Ai/Class/Druid/DruidAiObjectContext.cpp", "DruidAiObjectContextInternal"},
    {Family::Shaman, "src/Ai/Class/Shaman/ShamanAiObjectContext.cpp", "ShamanAiObjectContextInternal"},
    {Family::DeathKnight, "src/Ai/Class/Dk/DKAiObjectContext.cpp", "DeathKnightAiObjectContextInternal"},
};
}  // namespace

TEST(TacticalClassContract, TriggersActionsFactorsAndRegistrationResolve)
{
    std::string const strategy = Read("src/Ai/Base/Strategy/TacticalClassStrategy.cpp");
    std::string const conf = Read("conf/playerbots.conf.dist");
    std::set<std::string> const baseActions = Captures(Read("src/Ai/Base/ActionContext.h"), "creators\\[\"([^\"]+)\"\\]");
    std::set<std::string> const tacTriggers =
        Captures(Read("src/Ai/Base/TriggerContext.h"), "creators\\[\"(tac [^\"]+)\"\\] = &TriggerContext::");
    EXPECT_EQ(tacTriggers.size(), 14u);
    for (std::string const& name : Captures(strategy, "TriggerNode\\(\"(tac [^\"]+)\""))
        EXPECT_TRUE(tacTriggers.count(name)) << "unregistered trigger: " << name;
    EXPECT_NE(conf.find("\nAutoWow.Tactics.Classes = \"priest\"\n"), std::string::npos);  // default = priest only

    std::uint32_t rows = 0;
    for (FamilySource const& src : kSources)
    {
        ClassTable const& t = kClassTables[static_cast<std::uint32_t>(src.family)];
        std::string const key(t.key);
        if (key.empty())
            continue;
        ++rows;
        SCOPED_TRACE(key);
        std::string const ctx = Read(src.context);
        std::set<std::string> actions =
            Captures(ctx, std::string("creators\\[\"([^\"]+)\"\\]\\s*=\\s*&") + src.internal + "::");
        actions.insert(baseActions.begin(), baseActions.end());

        std::string const body = FunctionBody(strategy, "void " + key + "Nodes(");
        ASSERT_FALSE(body.empty()) << "no " << key << "Nodes() in TacticalClassStrategy.cpp";
        std::set<std::string> const pushed = Captures(body, "NextAction\\(\"([^\"]+)\"");
        EXPECT_GE(pushed.size(), 4u);
        for (std::string const& name : pushed)
            EXPECT_TRUE(actions.count(name)) << "unregistered action: " << name;
        EXPECT_NE(strategy.find("case Family::" + key + ":\n            " + key + "Nodes(triggers);"), std::string::npos);

        for (std::uint32_t i = 0; i < 4; ++i)
        {
            std::string const confKey = "AutoWow.Tactics." + key + ".Factors." + std::string(kClassSlotKeys[i]);
            EXPECT_EQ(ConfValue(conf, confKey), std::string(t.factors[i])) << confKey;
            for (auto const& [name, permille] : ParseFactors(t.factors[i]))
                EXPECT_TRUE(actions.count(name)) << "unregistered factored action: " << name;
        }
        EXPECT_EQ(ConfValue(conf, "AutoWow.Tactics." + key + ".HealHpPct"), std::to_string(t.healHpPct));
        EXPECT_FALSE(t.control[0].empty());

        EXPECT_NE(ctx.find("creators[\"tactical\"]"), std::string::npos);
        EXPECT_NE(ctx.find("new TacticalClassStrategy(botAI, AutoWowTactics::Family::" + key + ")"), std::string::npos);
    }
    EXPECT_EQ(rows, 9u);  // every non-priest family has tactics
    EXPECT_TRUE(kClassTables[static_cast<std::uint32_t>(Family::Priest)].key.empty());  // priest: own tables
}
