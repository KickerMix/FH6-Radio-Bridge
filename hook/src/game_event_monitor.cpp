#include "game_event_monitor.h"

#include "game_state_probe.h"
#include "local_bridge_client.h"
#include "logger.h"

#include <Windows.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>
#include <exception>

namespace
{
using clock_type = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::atomic_bool g_started = false;
std::unique_ptr<std::thread> g_thread;

bool IsEnabled(const std::wstring& baseDirectory)
{
    try
    {
        const auto flag = fh6rb::GameEventsFlagPath(baseDirectory);
        // Enabled by default. Create disable_game_events.flag when you need a
        // completely inert hook-side event monitor for troubleshooting.
        const auto disableFlag = std::filesystem::path(baseDirectory) / L"fh6-radio-bridge" / L"disable_game_events.flag";
        return !std::filesystem::exists(disableFlag) || std::filesystem::exists(flag);
    }
    catch (...)
    {
        return true;
    }
}

void SendEvent(const fh6rb::LocalBridgeClient& client, const std::string& type)
{
    if (client.PostEvent(type))
    {
        fh6rb::LogInfo("[events] sent hook event: " + type);
    }
    else
    {
        fh6rb::LogWarn("[events] could not send hook event to Bridge: " + type);
    }
}

void Worker(std::wstring baseDirectory)
{
    fh6rb::LogInfo("[events] game event monitor starting.");

    if (!IsEnabled(baseDirectory))
    {
        fh6rb::LogInfo("[events] game event monitor disabled by flag.");
        return;
    }

    fh6rb::GameStateProbe probe;
    if (!probe.IsAvailable())
    {
        fh6rb::LogWarn("[events] game event monitor exiting because GameStateProbe is unavailable.");
        return;
    }

    fh6rb::LocalBridgeClient client;

    bool initialized = false;
    bool prevR10 = false;
    bool prevRace = false;
    bool quickSkipArmed = false;
    bool raceStartArmed = true;
    clock_type::time_point lastR10Off{};
    clock_type::time_point raceInactiveSince = clock_type::now();
    clock_type::time_point lastSkipCommand = clock_type::now() - 10s;
    clock_type::time_point lastRaceEvent = clock_type::now() - 60s;
    std::string lastStation;

    while (IsEnabled(baseDirectory))
    {
        const auto now = clock_type::now();
        const auto state = probe.Read();
        const bool r10 = state.onTargetStation;

        if (state.valid && state.stationName != lastStation)
        {
            lastStation = state.stationName;
            if (!lastStation.empty())
            {
                fh6rb::LogInfo("[events] current radio station: " + lastStation);
            }
        }

        if (!state.valid)
        {
            std::this_thread::sleep_for(20ms);
            continue;
        }

        if (!initialized)
        {
            initialized = true;
            prevRace = state.raceActive;
            prevR10 = r10;
            raceStartArmed = !state.raceActive;
            raceInactiveSince = now;
            std::this_thread::sleep_for(20ms);
            continue;
        }

        if (!state.raceActive)
        {
            if (prevRace)
            {
                raceInactiveSince = now;
                raceStartArmed = false;
            }
            else if (!raceStartArmed && now - raceInactiveSince >= 15s)
            {
                raceStartArmed = true;
                fh6rb::LogInfo("[events] race start detector re-armed after stable non-race state.");
            }
        }

        const bool raceEdge = state.raceActive && !prevRace;
        if (raceEdge && raceStartArmed && r10 && !state.raceRestart)
        {
            if (now - lastRaceEvent >= 45s && now - lastSkipCommand >= 1500ms)
            {
                SendEvent(client, "raceStart");
                lastRaceEvent = now;
                lastSkipCommand = now;
                raceStartArmed = false;
            }
        }

        prevRace = state.raceActive;

        if (prevR10 && !r10)
        {
            lastR10Off = now;
            quickSkipArmed = true;
        }
        else if (!prevR10 && r10)
        {
            if (quickSkipArmed && now - lastR10Off <= 1000ms && now - lastSkipCommand >= 1500ms)
            {
                SendEvent(client, "quickStationSkip");
                lastSkipCommand = now;
            }
            quickSkipArmed = false;
        }

        if (quickSkipArmed && now - lastR10Off > 1000ms)
        {
            quickSkipArmed = false;
        }

        prevR10 = r10;
        std::this_thread::sleep_for(20ms);
    }

    fh6rb::LogInfo("[events] game event monitor stopped.");
}
}

namespace fh6rb
{
std::wstring GameEventsFlagPath(const std::wstring& baseDirectory)
{
    return (std::filesystem::path(baseDirectory) / L"fh6-radio-bridge" / L"enable_game_events.flag").wstring();
}

bool StartGameEventMonitor(const std::wstring& baseDirectory)
{
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true))
    {
        return true;
    }

    try
    {
        g_thread = std::make_unique<std::thread>(Worker, baseDirectory);
        g_thread->detach();
        return true;
    }
    catch (const std::exception& ex)
    {
        LogWarn(std::string("[events] could not start game event monitor: ") + ex.what());
        g_started = false;
        return false;
    }
}
}
