#include "logger.h"
#include "fmod_memory_probe.h"
#include "fmod_radio_injector.h"
#include "game_event_monitor.h"
#include "shared_audio_reader.h"
#include "version_proxy.h"
#include "xaudio_shared_player.h"

#include <Windows.h>

#include <string>

namespace
{
std::string WideToUtf8(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    const int size = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (size <= 0)
    {
        return {};
    }

    std::string result(static_cast<size_t>(size - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring ModulePath(HMODULE module)
{
    std::wstring buffer(MAX_PATH, L'\0');
    DWORD size = 0;

    for (;;)
    {
        size = GetModuleFileNameW(module, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (size == 0)
        {
            return {};
        }

        if (size < buffer.size() - 1)
        {
            buffer.resize(size);
            return buffer;
        }

        buffer.resize(buffer.size() * 2);
    }
}

std::wstring DirectoryName(const std::wstring& path)
{
    const auto separator = path.find_last_of(L"\\/");
    if (separator == std::wstring::npos)
    {
        return {};
    }

    return path.substr(0, separator);
}

std::wstring CurrentDirectory()
{
    const DWORD required = GetCurrentDirectoryW(0, nullptr);
    if (required == 0)
    {
        return {};
    }

    std::wstring buffer(required, L'\0');
    const DWORD size = GetCurrentDirectoryW(required, buffer.data());
    if (size == 0 || size >= required)
    {
        return {};
    }

    buffer.resize(size);
    return buffer;
}

DWORD WINAPI StartupThread(LPVOID parameter)
{
    const auto module = static_cast<HMODULE>(parameter);
    const auto proxyPath = ModulePath(module);
    const auto baseDirectory = DirectoryName(proxyPath);
    fh6rb::SetLoggerBaseDirectory(baseDirectory);
    fh6rb::InitializeLogger();
    fh6rb::LogInfo("FH6 Radio Bridge version.dll proxy loaded.");
    fh6rb::LogInfo("Process id: " + std::to_string(GetCurrentProcessId()));
    fh6rb::LogInfo("Process image: " + WideToUtf8(ModulePath(nullptr)));
    fh6rb::LogInfo("Proxy DLL path: " + WideToUtf8(proxyPath));
    fh6rb::LogInfo("Current directory: " + WideToUtf8(CurrentDirectory()));

    fh6rb::EnsureRealVersionDllLoaded();

    const auto status = fh6rb::ProbeSharedAudio();
    fh6rb::LogInfo("Shared audio status: " + fh6rb::FormatSharedAudioStatus(status));
    if (!status.connected || !status.headerValid)
    {
        fh6rb::LogWarn("Shared audio is unavailable or invalid; future audio callbacks must return silence.");
    }

    if (fh6rb::IsInProcessAudioEnabled(baseDirectory))
    {
        auto* player = new fh6rb::XAudioSharedPlayer();
        if (player->Start(baseDirectory))
        {
            fh6rb::LogInfo("In-process XAudio2 shared audio player enabled.");
        }
        else
        {
            fh6rb::LogWarn("In-process XAudio2 shared audio player did not start.");
            delete player;
        }
    }
    else
    {
        fh6rb::LogInfo("In-process XAudio2 shared audio player disabled. Create " +
            WideToUtf8(fh6rb::InProcessAudioFlagPath(baseDirectory)) + " to enable it.");
    }

    if (fh6rb::IsFmodProbeEnabled(baseDirectory))
    {
        if (fh6rb::StartFmodMemoryProbe(baseDirectory))
        {
            fh6rb::LogInfo("FMOD/radio memory probe enabled.");
        }
        else
        {
            fh6rb::LogWarn("FMOD/radio memory probe failed to start.");
        }
    }
    else
    {
        fh6rb::LogInfo("FMOD/radio memory probe disabled. Create " +
            WideToUtf8(fh6rb::FmodProbeFlagPath(baseDirectory)) + " to enable it.");
    }

    if (fh6rb::IsFmodInjectEnabled(baseDirectory))
    {
        if (fh6rb::StartFmodRadioInjector(baseDirectory))
        {
            fh6rb::LogInfo("FMOD radio injector enabled.");
        }
        else
        {
            fh6rb::LogWarn("FMOD radio injector failed to start.");
        }
    }
    else
    {
        fh6rb::LogInfo("FMOD radio injector disabled. Create " +
            WideToUtf8(fh6rb::FmodInjectFlagPath(baseDirectory)) + " to enable it.");
    }

    if (fh6rb::StartGameEventMonitor(baseDirectory))
    {
        fh6rb::LogInfo("Game event monitor started for race start and next song hook events.");
    }
    else
    {
        fh6rb::LogWarn("Game event monitor did not start.");
    }

    return 0;
}
}

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(module);

        HANDLE thread = CreateThread(nullptr, 0, StartupThread, module, 0, nullptr);
        if (thread)
        {
            CloseHandle(thread);
        }
    }

    return TRUE;
}
