/*
 * This file is part of the mod-playerbots module for AzerothCore. See AUTHORS file for Copyright
 * information; released under GNU GPL v2 license, redistribute/modify under version 2 of the License.
 */

#ifndef MOD_PLAYERBOTS_PROBE_PLACE_CONTROL_H
#define MOD_PLAYERBOTS_PROBE_PLACE_CONTROL_H

#include <string>

#include "ProbePlacePolicy.h"

namespace AutoWowProbePlace
{
// World thread only (AutoWowBridge dispatches through PlayerbotWorldThreadProcessor). Reads
// AutoWow.Probe.Enable, AutoWow.FixtureGuids and AutoWow.OracleRuntime.BotGuids per request and
// refuses through GateGuid before resolving or mutating anything.
std::string Execute(WireRequest const& request);
}

#endif  // MOD_PLAYERBOTS_PROBE_PLACE_CONTROL_H
