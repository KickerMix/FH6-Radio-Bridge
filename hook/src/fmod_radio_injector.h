#pragma once

#include <string>

namespace fh6rb
{
std::wstring FmodInjectFlagPath(const std::wstring& baseDirectory);
bool IsFmodInjectEnabled(const std::wstring& baseDirectory);
bool StartFmodRadioInjector(const std::wstring& baseDirectory);
}
