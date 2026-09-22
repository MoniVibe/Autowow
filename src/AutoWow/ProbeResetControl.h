/*
 * World-thread-only, idempotent Probe Lab roster reset.
 */
#ifndef MOD_PLAYERBOTS_PROBE_RESET_CONTROL_H
#define MOD_PLAYERBOTS_PROBE_RESET_CONTROL_H

#include "Define.h"

#include <string>
#include <vector>

namespace AutoWowProbeReset
{
std::string Reset(std::string const& exteriorRoute, uint32 targetMap, uint32 targetDifficulty,
                  std::vector<uint32> const& roster);
}

#endif
