#include "logger.h"

#include <Windows.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <sstream>

namespace
{
std::mutex g_logMutex;
std::filesystem::path g_logPath;
std::filesystem::path g_baseDirectory;
bool g_initialized = false;

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

std::filesystem::path FallbackLogPath()
{
    wchar_t localAppData[MAX_PATH]{};
    const DWORD count = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, MAX_PATH);
    if (count == 0 || count >= MAX_PATH)
    {
        return std::filesystem::current_path() / L"hook.log";
    }

    return std::filesystem::path(localAppData) / L"FH6RadioBridge" / L"hook.log";
}

bool TryOpenForAppend(const std::filesystem::path& path)
{
    try
    {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::app);
        return stream.good();
    }
    catch (...)
    {
        return false;
    }
}

std::string Timestamp()
{
    const auto now = std::chrono::system_clock::now();
    const auto nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm localTime{};
    localtime_s(&localTime, &nowTime);

    std::ostringstream stream;
    stream << std::put_time(&localTime, "%Y-%m-%d %H:%M:%S");
    return stream.str();
}

void WriteLine(const char* level, const std::string& message)
{
    if (!g_initialized)
    {
        fh6rb::InitializeLogger();
    }

    std::ofstream stream(g_logPath, std::ios::app);
    if (!stream.good())
    {
        return;
    }

    stream << Timestamp() << " [" << level << "] " << message << "\n";
}
}

namespace fh6rb
{
void SetLoggerBaseDirectory(const std::wstring& baseDirectory)
{
    std::scoped_lock lock(g_logMutex);
    if (g_initialized || baseDirectory.empty())
    {
        return;
    }

    g_baseDirectory = std::filesystem::path(baseDirectory);
}

void InitializeLogger()
{
    std::scoped_lock lock(g_logMutex);
    if (g_initialized)
    {
        return;
    }

    const auto baseDirectory = g_baseDirectory.empty() ? std::filesystem::current_path() : g_baseDirectory;
    auto preferred = baseDirectory / L"fh6-radio-bridge" / L"logs" / L"hook.log";
    if (TryOpenForAppend(preferred))
    {
        g_logPath = std::move(preferred);
    }
    else
    {
        g_logPath = FallbackLogPath();
        TryOpenForAppend(g_logPath);
    }

    g_initialized = true;
    WriteLine("INFO", "Logger initialized at " + WideToUtf8(g_logPath.wstring()));
}

void LogInfo(const std::string& message)
{
    std::scoped_lock lock(g_logMutex);
    WriteLine("INFO", message);
}

void LogWarn(const std::string& message)
{
    std::scoped_lock lock(g_logMutex);
    WriteLine("WARN", message);
}

void LogError(const std::string& message)
{
    std::scoped_lock lock(g_logMutex);
    WriteLine("ERROR", message);
}
}
