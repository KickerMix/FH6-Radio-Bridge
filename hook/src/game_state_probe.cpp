#include "game_state_probe.h"

#include "logger.h"

#include <Psapi.h>

#include <algorithm>
#include <cstring>
#include <sstream>
#include <cstdlib>

namespace
{
constexpr const char* kRadioStateSingletonPattern =
    "48 89 5C 24 08 48 89 54 24 10 57 48 83 EC 40 48 8B FA "
    "48 8B 1D ?? ?? ?? ?? "
    "48 85 DB 74 16 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B D0 48 8B CB";

constexpr std::size_t kMovRipOffset = 18;
constexpr std::size_t kDispOffset = kMovRipOffset + 3;
constexpr std::size_t kInsnEndOffset = kMovRipOffset + 7;

constexpr std::ptrdiff_t kRaceRunningA = 0x68;
constexpr std::ptrdiff_t kRaceRunningB = 0x69;
constexpr std::ptrdiff_t kRaceRestartDw = 0x80;
constexpr std::ptrdiff_t kStationChain1Off = 0x40;
constexpr std::ptrdiff_t kStationChain2Off = 0x50;
constexpr std::ptrdiff_t kStationNameOff = 0x200;

bool IsReadable(const void* address, std::size_t size) noexcept
{
    if (!address || size == 0)
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi))
    {
        return false;
    }

    if (mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0)
    {
        return false;
    }

    const auto start = reinterpret_cast<std::uintptr_t>(address);
    const auto end = start + size;
    const auto regionEnd = reinterpret_cast<std::uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
    return end >= start && end <= regionEnd;
}

bool SafeCopy(const void* address, void* output, std::size_t size) noexcept
{
    if (!IsReadable(address, size))
    {
        return false;
    }

    __try
    {
        std::memcpy(output, address, size);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

template <typename T>
bool SafeRead(const void* address, T& output) noexcept
{
    return SafeCopy(address, &output, sizeof(T));
}

std::optional<std::string> SafeReadMsvcString(const void* address) noexcept
{
    // MSVC std::string layout used by FH6: small buffer at +0, size at +0x10,
    // capacity at +0x18; if capacity >= 16, +0 contains a heap pointer.
    std::uint64_t size = 0;
    std::uint64_t capacity = 0;
    if (!SafeRead(static_cast<const std::uint8_t*>(address) + 0x10, size) ||
        !SafeRead(static_cast<const std::uint8_t*>(address) + 0x18, capacity) ||
        size == 0 ||
        size > 128)
    {
        return std::nullopt;
    }

    const char* chars = static_cast<const char*>(address);
    if (capacity >= 16)
    {
        if (!SafeRead(address, chars) || !chars)
        {
            return std::nullopt;
        }
    }

    if (!IsReadable(chars, static_cast<std::size_t>(size)))
    {
        return std::nullopt;
    }

    std::string result(static_cast<std::size_t>(size), '\0');
    if (!SafeCopy(chars, result.data(), result.size()))
    {
        return std::nullopt;
    }

    return result;
}

std::string Hex(std::uintptr_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

struct PatternByte
{
    std::uint8_t value = 0;
    bool wildcard = false;
};

std::vector<PatternByte> ParsePattern(const char* pattern)
{
    std::vector<PatternByte> bytes;
    const char* p = pattern;
    while (*p)
    {
        while (*p == ' ')
        {
            ++p;
        }

        if (!*p)
        {
            break;
        }

        if (p[0] == '?' && p[1] == '?')
        {
            bytes.push_back({0, true});
            p += 2;
            continue;
        }

        char token[3] = {p[0], p[1], 0};
        bytes.push_back({static_cast<std::uint8_t>(std::strtoul(token, nullptr, 16)), false});
        p += 2;
    }

    return bytes;
}
}

namespace fh6rb
{
GameStateProbe::GameStateProbe()
{
    const auto image = CurrentProcessImage();
    if (!image.base || image.size == 0)
    {
        LogWarn("[gstate] could not resolve process image; game event detection disabled.");
        return;
    }

    auto* match = FindPattern(image, kRadioStateSingletonPattern);
    if (!match)
    {
        LogWarn("[gstate] radio_state_singleton pattern not found; race/station events disabled.");
        return;
    }

    std::int32_t disp = 0;
    std::memcpy(&disp, match + kDispOffset, sizeof(disp));
    auto* slot = reinterpret_cast<const std::uint8_t* const*>(match + kInsnEndOffset + disp);
    auto* imageEnd = image.base + image.size;
    if (reinterpret_cast<const std::uint8_t*>(slot) < image.base || reinterpret_cast<const std::uint8_t*>(slot) >= imageEnd)
    {
        LogWarn("[gstate] decoded singleton slot outside image; game event detection disabled.");
        return;
    }

    singletonSlot_ = slot;
    LogInfo("[gstate] radio_state singleton slot resolved at " + Hex(reinterpret_cast<std::uintptr_t>(slot)));
}

GameStateProbe::Snapshot GameStateProbe::Read() const noexcept
{
    Snapshot out{};
    if (!singletonSlot_)
    {
        return out;
    }

    const std::uint8_t* radioState = nullptr;
    if (!SafeRead(singletonSlot_, radioState) || !radioState)
    {
        return out;
    }

    out.valid = true;

    std::uint8_t a = 0;
    std::uint8_t b = 0;
    std::int32_t restart = 0;
    if (SafeRead(radioState + kRaceRunningA, a) && SafeRead(radioState + kRaceRunningB, b))
    {
        out.raceActive = a != 0 && b != 0;
    }

    if (SafeRead(radioState + kRaceRestartDw, restart))
    {
        out.raceRestart = restart == -1;
    }

    const std::uint8_t* chain1 = nullptr;
    const std::uint8_t* chain2 = nullptr;
    if (SafeRead(radioState + kStationChain1Off, chain1) && chain1 &&
        SafeRead(chain1 + kStationChain2Off, chain2) && chain2)
    {
        if (auto name = SafeReadMsvcString(chain2 + kStationNameOff))
        {
            out.stationName = *name;
            out.onTargetStation = *name == "Streamer Mode" || *name == "Universal Radio";
        }
    }

    return out;
}

GameStateProbe::ModuleImage GameStateProbe::CurrentProcessImage() noexcept
{
    MODULEINFO info{};
    const auto module = GetModuleHandleW(nullptr);
    if (!module || !GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info)))
    {
        return {};
    }

    return {static_cast<std::uint8_t*>(info.lpBaseOfDll), static_cast<std::size_t>(info.SizeOfImage)};
}

std::uint8_t* GameStateProbe::FindPattern(const ModuleImage& image, const char* pattern) noexcept
{
    const auto bytes = ParsePattern(pattern);
    if (bytes.empty() || image.size < bytes.size())
    {
        return nullptr;
    }

    const auto last = image.size - bytes.size();
    for (std::size_t i = 0; i <= last; i++)
    {
        bool match = true;
        for (std::size_t j = 0; j < bytes.size(); j++)
        {
            if (!bytes[j].wildcard && image.base[i + j] != bytes[j].value)
            {
                match = false;
                break;
            }
        }

        if (match)
        {
            return image.base + i;
        }
    }

    return nullptr;
}
}
