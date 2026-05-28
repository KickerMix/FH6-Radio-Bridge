#pragma once

#include <string>

namespace fh6rb
{
std::wstring FmodProbeFlagPath(const std::wstring& baseDirectory);
std::wstring FmodProbeRequestPath(const std::wstring& baseDirectory);
bool IsFmodProbeEnabled(const std::wstring& baseDirectory);
bool StartFmodMemoryProbe(const std::wstring& baseDirectory);
}
