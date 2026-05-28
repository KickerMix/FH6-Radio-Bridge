#pragma once

#include <string>

namespace fh6rb
{
bool StartGameEventMonitor(const std::wstring& baseDirectory);
std::wstring GameEventsFlagPath(const std::wstring& baseDirectory);
}
