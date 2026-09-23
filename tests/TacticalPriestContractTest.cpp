/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License,
 * or (at your option) any later version.
 */

#include "TacticalPolicy.h"

#include "gtest/gtest.h"

#include <filesystem>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <string>

// Source contract of the T2 priest tactical wiring: every action a tactic trigger pushes or a factor table
// names is a registered action, every "tactic ..." trigger is registered, the conf dist defaults equal the
// code defaults, and the engine wiring stays behind AutoWow.Tactics.Enable + the treatment arm.
namespace
{
std::string Read(char const* rel)
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
    std::regex const re("\n" + std::regex_replace(key, std::regex("\\."), "\\.") + " = \"([^\"]*)\"");
    return std::regex_search(conf, m, re) ? m[1].str() : std::string("<missing>");
}
}  // namespace

TEST(TacticalPriestContract, ActionsAndTriggersResolve)
{
    std::string const strategy = Read("src/Ai/Class/Priest/Strategy/TacticalPriestStrategy.cpp");
    std::string const priest = Read("src/Ai/Class/Priest/PriestAiObjectContext.cpp");
    std::string const base = Read("src/Ai/Base/ActionContext.h");
    std::string const conf = Read("conf/playerbots.conf.dist");

    std::set<std::string> registered = Captures(priest, "creators\\[\"([^\"]+)\"\\] = &PriestAiObjectContextInternal::");
    for (std::string const& name : Captures(base, "creators\\[\"([^\"]+)\"\\]"))
        registered.insert(name);

    std::set<std::string> used = Captures(strategy, "NextAction\\(\"([^\"]+)\"");
    ASSERT_GE(used.size(), 10u);
    for (char const* tactic : {"Wand", "Burst", "Multi", "Emergency", "Escape"})
        for (auto const& [name, permille] :
             AutoWowTactics::ParseFactors(ConfValue(conf, std::string("AutoWow.Tactics.Priest.Factors.") + tactic)))
            used.insert(name);
    for (std::string const& name : used)
        EXPECT_TRUE(registered.count(name)) << "unregistered action: " << name;

    std::set<std::string> const triggers = Captures(strategy, "TriggerNode\\(\"(tactic [^\"]+)\"");
    std::set<std::string> const registeredTriggers =
        Captures(priest, "creators\\[\"(tactic [^\"]+)\"\\] = &PriestTriggerFactoryInternal::");
    EXPECT_EQ(triggers.size(), 10u);
    EXPECT_EQ(triggers, registeredTriggers);
}

TEST(TacticalPriestContract, ConfDefaultsMatchCodeAndWiringIsGated)
{
    std::string const conf = Read("conf/playerbots.conf.dist");
    std::string const runtime = Read("src/AutoWow/TacticalRuntime.cpp");
    auto codeDefault = [&runtime](char const* constant)
    {
        std::size_t const at = runtime.find(std::string("constexpr char ") + constant);
        std::size_t const end = runtime.find(';', at);
        std::string joined;  // adjacent string literals concatenate
        std::regex const re("\"([^\"]*)\"");
        std::string const decl = runtime.substr(at, end - at);
        for (auto it = std::sregex_iterator(decl.begin(), decl.end(), re); it != std::sregex_iterator(); ++it)
            joined += (*it)[1].str();
        return joined;
    };
    EXPECT_EQ(ConfValue(conf, "AutoWow.Tactics.Priest.Factors.Wand"), codeDefault("kDefaultWandFactors"));
    EXPECT_EQ(ConfValue(conf, "AutoWow.Tactics.Priest.Factors.Multi"), codeDefault("kDefaultMultiFactors"));
    EXPECT_EQ(ConfValue(conf, "AutoWow.Tactics.Priest.Factors.Emergency"), codeDefault("kDefaultEmergencyFactors"));
    EXPECT_EQ(ConfValue(conf, "AutoWow.Tactics.Priest.Factors.Escape"), codeDefault("kDefaultEscapeFactors"));
    EXPECT_NE(conf.find("\nAutoWow.Tactics.Enable = 0\n"), std::string::npos);
    EXPECT_NE(conf.find("\nAutoWow.Tactics.Observe = 0\n"), std::string::npos);
    EXPECT_NE(conf.find("\nAutoWow.Tactics.PriestShadowLevelingSpec = 0\n"), std::string::npos);
    EXPECT_EQ(conf.find("\nAiPlayerbot.RandomClassSpecProb.5.6"), std::string::npos);  // random draw untouched

    std::string const factory = Read("src/Bot/Factory/AiFactory.cpp");
    EXPECT_NE(factory.find("if (AutoWowTactics::Enabled() && AutoWowTactics::IsTreatment(player))\n"
                           "        engine->addStrategy(\"tactical\", false);"),
              std::string::npos);
    EXPECT_NE(factory.find("if (AutoWowTactics::Enabled() && AutoWowTactics::IsTreatment(player))\n"
                           "        nonCombatEngine->addStrategy(\"tactical nc\", false);"),
              std::string::npos);
}
