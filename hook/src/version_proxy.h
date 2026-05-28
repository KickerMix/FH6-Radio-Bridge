#pragma once

#include <Windows.h>

namespace fh6rb
{
HMODULE EnsureRealVersionDllLoaded();
FARPROC ResolveVersionProc(const char* name);
}
