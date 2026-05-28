#include "version_proxy.h"

#include "logger.h"

#include <mutex>
#include <string>

namespace
{
std::once_flag g_loadOnce;
HMODULE g_realVersionDll = nullptr;

template <typename T>
T Resolve(const char* name)
{
    auto proc = fh6rb::ResolveVersionProc(name);
    if (!proc)
    {
        SetLastError(ERROR_PROC_NOT_FOUND);
        return nullptr;
    }

    return reinterpret_cast<T>(proc);
}
}

namespace fh6rb
{
HMODULE EnsureRealVersionDllLoaded()
{
    std::call_once(g_loadOnce, []()
    {
        const wchar_t* systemVersionPath = L"C:\\Windows\\System32\\version.dll";
        g_realVersionDll = LoadLibraryW(systemVersionPath);
        if (g_realVersionDll)
        {
            LogInfo("Loaded real version.dll from C:\\Windows\\System32\\version.dll");
        }
        else
        {
            LogError("Failed to load C:\\Windows\\System32\\version.dll, error=" + std::to_string(GetLastError()));
        }
    });

    return g_realVersionDll;
}

FARPROC ResolveVersionProc(const char* name)
{
    const auto module = EnsureRealVersionDllLoaded();
    if (!module)
    {
        return nullptr;
    }

    const auto proc = GetProcAddress(module, name);
    if (!proc)
    {
        LogError(std::string("Missing export in real version.dll: ") + name + ", error=" + std::to_string(GetLastError()));
    }

    return proc;
}
}

extern "C"
BOOL WINAPI GetFileVersionInfoA(LPCSTR filename, DWORD handle, DWORD length, LPVOID data)
{
    using Fn = BOOL(WINAPI*)(LPCSTR, DWORD, DWORD, LPVOID);
    const auto fn = Resolve<Fn>("GetFileVersionInfoA");
    return fn ? fn(filename, handle, length, data) : FALSE;
}

extern "C"
BOOL WINAPI GetFileVersionInfoW(LPCWSTR filename, DWORD handle, DWORD length, LPVOID data)
{
    using Fn = BOOL(WINAPI*)(LPCWSTR, DWORD, DWORD, LPVOID);
    const auto fn = Resolve<Fn>("GetFileVersionInfoW");
    return fn ? fn(filename, handle, length, data) : FALSE;
}

extern "C"
BOOL WINAPI GetFileVersionInfoByHandle(DWORD flags, HANDLE file, LPVOID* data, PDWORD length)
{
    using Fn = BOOL(WINAPI*)(DWORD, HANDLE, LPVOID*, PDWORD);
    const auto fn = Resolve<Fn>("GetFileVersionInfoByHandle");
    return fn ? fn(flags, file, data, length) : FALSE;
}

extern "C"
BOOL WINAPI GetFileVersionInfoExA(DWORD flags, LPCSTR filename, DWORD handle, DWORD length, LPVOID data)
{
    using Fn = BOOL(WINAPI*)(DWORD, LPCSTR, DWORD, DWORD, LPVOID);
    const auto fn = Resolve<Fn>("GetFileVersionInfoExA");
    return fn ? fn(flags, filename, handle, length, data) : FALSE;
}

extern "C"
BOOL WINAPI GetFileVersionInfoExW(DWORD flags, LPCWSTR filename, DWORD handle, DWORD length, LPVOID data)
{
    using Fn = BOOL(WINAPI*)(DWORD, LPCWSTR, DWORD, DWORD, LPVOID);
    const auto fn = Resolve<Fn>("GetFileVersionInfoExW");
    return fn ? fn(flags, filename, handle, length, data) : FALSE;
}

extern "C"
DWORD WINAPI GetFileVersionInfoSizeA(LPCSTR filename, LPDWORD handle)
{
    using Fn = DWORD(WINAPI*)(LPCSTR, LPDWORD);
    const auto fn = Resolve<Fn>("GetFileVersionInfoSizeA");
    return fn ? fn(filename, handle) : 0;
}

extern "C"
DWORD WINAPI GetFileVersionInfoSizeW(LPCWSTR filename, LPDWORD handle)
{
    using Fn = DWORD(WINAPI*)(LPCWSTR, LPDWORD);
    const auto fn = Resolve<Fn>("GetFileVersionInfoSizeW");
    return fn ? fn(filename, handle) : 0;
}

extern "C"
DWORD WINAPI GetFileVersionInfoSizeExA(DWORD flags, LPCSTR filename, LPDWORD handle)
{
    using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPDWORD);
    const auto fn = Resolve<Fn>("GetFileVersionInfoSizeExA");
    return fn ? fn(flags, filename, handle) : 0;
}

extern "C"
DWORD WINAPI GetFileVersionInfoSizeExW(DWORD flags, LPCWSTR filename, LPDWORD handle)
{
    using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPDWORD);
    const auto fn = Resolve<Fn>("GetFileVersionInfoSizeExW");
    return fn ? fn(flags, filename, handle) : 0;
}

extern "C"
DWORD WINAPI VerFindFileA(DWORD flags, LPCSTR filename, LPCSTR winDir, LPCSTR appDir, LPSTR curDir, PUINT curDirLen, LPSTR destDir, PUINT destDirLen)
{
    using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT, LPSTR, PUINT);
    const auto fn = Resolve<Fn>("VerFindFileA");
    return fn ? fn(flags, filename, winDir, appDir, curDir, curDirLen, destDir, destDirLen) : 0;
}

extern "C"
DWORD WINAPI VerFindFileW(DWORD flags, LPCWSTR filename, LPCWSTR winDir, LPCWSTR appDir, LPWSTR curDir, PUINT curDirLen, LPWSTR destDir, PUINT destDirLen)
{
    using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT, LPWSTR, PUINT);
    const auto fn = Resolve<Fn>("VerFindFileW");
    return fn ? fn(flags, filename, winDir, appDir, curDir, curDirLen, destDir, destDirLen) : 0;
}

extern "C"
DWORD WINAPI VerInstallFileA(DWORD flags, LPCSTR srcFile, LPCSTR destFile, LPCSTR srcDir, LPCSTR destDir, LPCSTR curDir, LPSTR tmpFile, PUINT tmpFileLen)
{
    using Fn = DWORD(WINAPI*)(DWORD, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPCSTR, LPSTR, PUINT);
    const auto fn = Resolve<Fn>("VerInstallFileA");
    return fn ? fn(flags, srcFile, destFile, srcDir, destDir, curDir, tmpFile, tmpFileLen) : 0;
}

extern "C"
DWORD WINAPI VerInstallFileW(DWORD flags, LPCWSTR srcFile, LPCWSTR destFile, LPCWSTR srcDir, LPCWSTR destDir, LPCWSTR curDir, LPWSTR tmpFile, PUINT tmpFileLen)
{
    using Fn = DWORD(WINAPI*)(DWORD, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPWSTR, PUINT);
    const auto fn = Resolve<Fn>("VerInstallFileW");
    return fn ? fn(flags, srcFile, destFile, srcDir, destDir, curDir, tmpFile, tmpFileLen) : 0;
}

extern "C"
DWORD WINAPI VerLanguageNameA(DWORD language, LPSTR languageName, DWORD languageNameLength)
{
    using Fn = DWORD(WINAPI*)(DWORD, LPSTR, DWORD);
    const auto fn = Resolve<Fn>("VerLanguageNameA");
    return fn ? fn(language, languageName, languageNameLength) : 0;
}

extern "C"
DWORD WINAPI VerLanguageNameW(DWORD language, LPWSTR languageName, DWORD languageNameLength)
{
    using Fn = DWORD(WINAPI*)(DWORD, LPWSTR, DWORD);
    const auto fn = Resolve<Fn>("VerLanguageNameW");
    return fn ? fn(language, languageName, languageNameLength) : 0;
}

extern "C"
BOOL WINAPI VerQueryValueA(LPCVOID block, LPCSTR subBlock, LPVOID* buffer, PUINT length)
{
    using Fn = BOOL(WINAPI*)(LPCVOID, LPCSTR, LPVOID*, PUINT);
    const auto fn = Resolve<Fn>("VerQueryValueA");
    return fn ? fn(block, subBlock, buffer, length) : FALSE;
}

extern "C"
BOOL WINAPI VerQueryValueW(LPCVOID block, LPCWSTR subBlock, LPVOID* buffer, PUINT length)
{
    using Fn = BOOL(WINAPI*)(LPCVOID, LPCWSTR, LPVOID*, PUINT);
    const auto fn = Resolve<Fn>("VerQueryValueW");
    return fn ? fn(block, subBlock, buffer, length) : FALSE;
}
