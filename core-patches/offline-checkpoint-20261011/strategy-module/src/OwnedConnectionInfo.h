/* AutoWoW private connection-description ownership. GPL v2 or later. */
#ifndef AUTOWOW_OWNED_CONNECTION_INFO_H
#define AUTOWOW_OWNED_CONNECTION_INFO_H

#include "MySQLConnection.h"

namespace AutoWowStrategy
{
// Declare this base before MySQLConnection: its copied description must exist before the native constructor
// takes a reference, and must outlive native Close/destruction. Socket normalization stays connection-local.
class OwnedConnectionInfo
{
protected:
    explicit OwnedConnectionInfo(MySQLConnectionInfo const& info) : _info(info) { }
    MySQLConnectionInfo& OwnedInfo() { return _info; }

private:
    MySQLConnectionInfo _info;
};
} // namespace AutoWowStrategy

#endif
