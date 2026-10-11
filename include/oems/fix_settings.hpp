#pragma once
// Loading QuickFIX settings files. Any "${FIX44_XML}" in a file is replaced with
// the FIX 4.4 data dictionary of the pinned QuickFIX build, so config files
// don't hard-code a machine-specific path.

#include <quickfix/SessionSettings.h>

#include <string>

namespace oems {

std::string fix44DictionaryPath();
FIX::SessionSettings loadSessionSettings(const std::string& path); // throws std::runtime_error if unreadable

} // namespace oems
