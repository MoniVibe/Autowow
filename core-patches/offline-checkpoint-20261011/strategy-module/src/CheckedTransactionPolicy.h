/* AutoWoW checked transaction sequencing. GPL v2 or later. */
#ifndef AUTOWOW_CHECKED_TRANSACTION_POLICY_H
#define AUTOWOW_CHECKED_TRANSACTION_POLICY_H

#include <cstddef>

namespace AutoWowStrategy
{
// Backend operations must use one exclusively owned session without statement-level reconnect/replay.
// A lost COMMIT acknowledgement is unknown, never success. Quarantine closes that session; callers retry
// immutable receipt terms only, so a possibly committed operation can be reconciled without new rewards.
template <typename Backend>
[[nodiscard]] bool ExecuteCheckedTransaction(std::size_t statements, Backend& backend)
{
    if (!statements)
        return false;

    if (!backend.Begin())
    {
        backend.Quarantine();
        return false;
    }

    for (std::size_t index = 0; index < statements; ++index)
    {
        if (!backend.Step(index))
        {
            if (!backend.Rollback())
                backend.Quarantine();
            return false;
        }
    }

    if (!backend.Commit())
    {
        backend.Quarantine();
        return false;
    }

    return true;
}
} // namespace AutoWowStrategy

#endif
