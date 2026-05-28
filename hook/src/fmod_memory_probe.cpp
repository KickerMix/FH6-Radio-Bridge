#include "fmod_memory_probe.h"

#include "logger.h"

#include <Windows.h>
#include <TlHelp32.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstring>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <iomanip>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr size_t kMaxHitsPerPattern = 8;
constexpr size_t kMaxDeepScanBytes = 512ull * 1024ull * 1024ull;
constexpr size_t kMaxDeepScanRegions = 768;
constexpr size_t kReadChunkBytes = 1024ull * 1024ull;
constexpr size_t kMaxRadioStreamCandidates = 64;
constexpr size_t kMaxObjectProbeBytes = 2300ull * 1024ull * 1024ull;
constexpr size_t kMaxObjectProbeRegions = 8000;
constexpr auto kMaxObjectProbeDuration = std::chrono::seconds(35);
constexpr uintptr_t kMinLikelyGameHeapAddress = 0x10000000000ull;

constexpr size_t kExpectedForzaExeSize = 187621376;
constexpr uintptr_t kRvaRadioStreamFmodTypeName = 0xA032878;
constexpr uintptr_t kRvaRadioStreamFmodRefCountVtable = 0x6979FC0;
constexpr uintptr_t kRvaRadioStreamFmodObjectVtable = 0x6973A28;

struct ModuleInfo
{
    std::wstring name;
    std::wstring path;
    uintptr_t base = 0;
    size_t size = 0;
};

struct ProbePattern
{
    const char* name;
    const char* text;
};

struct BytePattern
{
    const char* name;
    const char* bytes;
};

struct PatternResult
{
    const ProbePattern* pattern = nullptr;
    size_t hits = 0;
    bool truncated = false;
    std::vector<uintptr_t> firstHits;
};

struct RadioStreamCandidate
{
    uintptr_t refCount = 0;
    uintptr_t object = 0;
    uintptr_t objectVtable = 0;
    uintptr_t fmodSound = 0;
    uintptr_t handle = 0;
    uintptr_t eventString = 0;
    uintptr_t eventStringRefCount = 0;
    uintptr_t sampleData = 0;
    uintptr_t systemI = 0;
    uintptr_t bankString = 0;
    uint32_t state = 0;
    std::string eventText;
    std::string sampleText;
    std::string bankText;
};

const std::array<ProbePattern, 16> kPatterns{{
    {"fmod_upper", "FMOD"},
    {"fmod_lower", "fmod"},
    {"fsb5", "FSB5"},
    {"radio_track_event", "/Master/Radio/Track"},
    {"radio_dj_event", "/Master/Radio/DJ"},
    {"streamer_mode", "Streamer Mode"},
    {"anchor_track", "HZ6_R9_PeterBroderick_EyesClosedandTraveling"},
    {"radio_bridge_synthetic_track", "FH6_RadioBridge_Stream"},
    {"r9_bank", "R9_Tracks_CU1"},
    {"track_start_marker", "TrackStart"},
    {"track_loop_marker", "TrackLoopStart"},
    {"dj_drop_marker", "DJDrop"},
    {"radio_station_xml", "RadioStation"},
    {"reference_fmod_inject", "fmod-inject"},
    {"reference_radio_stream_fmod", "RadioStreamFmod"},
    {"reference_r10_active", "r10-active"},
}};

const std::array<BytePattern, 12> kFmodApiPatterns{{
    {"fmod_api_large_stack_150_a", "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 50 01 00 00"},
    {"fmod_api_large_stack_170", "4C 8B DC 56 48 81 EC 70 01 00 00"},
    {"fmod_api_large_stack_180", "4C 8B DC 56 48 81 EC 80 01 00 00"},
    {"fmod_api_large_stack_150_b", "48 89 5C 24 10 57 48 81 EC 50 01 00 00"},
    {"fmod_api_large_stack_150_c", "48 89 5C 24 10 48 89 74 24 18 57 48 81 EC 50 01 00 00"},
    {"fmod_api_large_stack_160_a", "48 89 5C 24 10 48 89 74 24 18 57 48 81 EC 60 01 00 00"},
    {"fmod_api_large_stack_160_b", "48 89 5C 24 18 57 48 81 EC 60 01 00 00 0F 29 B4 24 50 01 00 00"},
    {"fmod_api_large_stack_160_c", "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 60 01 00 00"},
    {"fmod_handle_resolver", "48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 8B F9 8B C1 C1 EF 11 49 8B F0 D1 E8 81 E7 FF 0F 00 00 0F B7 E8 4C 8B F2 4C 8B F9"},
    {"studio_global_candidate", "48 8B 89 F0 09 01 00 48 85 C9 0F 85 ?? ?? ?? ?? 33 C0 C3"},
    {"radio_state_singleton", "48 89 5C 24 08 48 89 54 24 10 57 48 83 EC 40 48 8B FA 48 8B 1D ?? ?? ?? ?? 48 85 DB 74 16 48 8D 4C 24 20 E8 ?? ?? ?? ?? 48 8B D0 48 8B CB"},
    {"channel_command_bool_rva_57c15e0", "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 30 41 8A F0 8A FA 8B D9"},
}};

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

std::string Hex(uintptr_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

std::wstring NormalizePath(const std::wstring& value)
{
    if (value.empty())
    {
        return {};
    }

    try
    {
        return std::filesystem::weakly_canonical(std::filesystem::path(value)).wstring();
    }
    catch (...)
    {
        return value;
    }
}

bool SamePath(const std::wstring& left, const std::wstring& right)
{
    return _wcsicmp(NormalizePath(left).c_str(), NormalizePath(right).c_str()) == 0;
}

std::wstring ProbeDirectory(const std::wstring& baseDirectory)
{
    return (std::filesystem::path(baseDirectory) / L"fh6-radio-bridge").wstring();
}

bool IsReadableProtection(DWORD protect)
{
    if ((protect & PAGE_GUARD) != 0 || (protect & PAGE_NOACCESS) != 0)
    {
        return false;
    }

    const DWORD baseProtect = protect & 0xff;
    return baseProtect == PAGE_READONLY ||
        baseProtect == PAGE_READWRITE ||
        baseProtect == PAGE_WRITECOPY ||
        baseProtect == PAGE_EXECUTE_READ ||
        baseProtect == PAGE_EXECUTE_READWRITE ||
        baseProtect == PAGE_EXECUTE_WRITECOPY;
}

std::string MemoryTypeString(DWORD type)
{
    switch (type)
    {
    case MEM_IMAGE:
        return "MEM_IMAGE";
    case MEM_MAPPED:
        return "MEM_MAPPED";
    case MEM_PRIVATE:
        return "MEM_PRIVATE";
    default:
        return "0x" + std::to_string(type);
    }
}

std::string ProtectString(DWORD protect)
{
    if ((protect & PAGE_GUARD) != 0)
    {
        return "GUARD";
    }

    switch (protect & 0xff)
    {
    case PAGE_EXECUTE:
        return "EXECUTE";
    case PAGE_EXECUTE_READ:
        return "EXECUTE_READ";
    case PAGE_EXECUTE_READWRITE:
        return "EXECUTE_READWRITE";
    case PAGE_EXECUTE_WRITECOPY:
        return "EXECUTE_WRITECOPY";
    case PAGE_NOACCESS:
        return "NOACCESS";
    case PAGE_READONLY:
        return "READONLY";
    case PAGE_READWRITE:
        return "READWRITE";
    case PAGE_WRITECOPY:
        return "WRITECOPY";
    default:
        return "0x" + std::to_string(protect);
    }
}

std::vector<ModuleInfo> EnumerateModules()
{
    std::vector<ModuleInfo> modules;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, GetCurrentProcessId());
    if (snapshot == INVALID_HANDLE_VALUE)
    {
        fh6rb::LogWarn("FMOD probe could not create module snapshot. GetLastError=" + std::to_string(GetLastError()));
        return modules;
    }

    MODULEENTRY32W entry{};
    entry.dwSize = sizeof(entry);

    if (Module32FirstW(snapshot, &entry))
    {
        do
        {
            ModuleInfo module;
            module.name = entry.szModule;
            module.path = entry.szExePath;
            module.base = reinterpret_cast<uintptr_t>(entry.modBaseAddr);
            module.size = static_cast<size_t>(entry.modBaseSize);
            modules.push_back(std::move(module));
        } while (Module32NextW(snapshot, &entry));
    }
    else
    {
        fh6rb::LogWarn("FMOD probe Module32FirstW failed. GetLastError=" + std::to_string(GetLastError()));
    }

    CloseHandle(snapshot);

    std::sort(modules.begin(), modules.end(), [](const ModuleInfo& left, const ModuleInfo& right) {
        return left.base < right.base;
    });

    return modules;
}

bool IsInsideAnyModule(uintptr_t address, const std::vector<ModuleInfo>& modules)
{
    for (const auto& module : modules)
    {
        if (address >= module.base && address < module.base + module.size)
        {
            return true;
        }
    }

    return false;
}

const ModuleInfo* FindForzaModule(const std::vector<ModuleInfo>& modules)
{
    for (const auto& module : modules)
    {
        if (_wcsicmp(module.name.c_str(), L"forzahorizon6.exe") == 0)
        {
            return &module;
        }
    }

    for (const auto& module : modules)
    {
        const auto name = WideToUtf8(module.name);
        if (name.find("forza") != std::string::npos || name.find("horizon") != std::string::npos)
        {
            return &module;
        }
    }

    return nullptr;
}

template <typename T>
bool ReadValue(uintptr_t address, T& value)
{
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), &value, sizeof(T), &bytesRead) &&
        bytesRead == sizeof(T);
}

std::string ReadAsciiString(uintptr_t address, size_t maxLength)
{
    if (address == 0 || maxLength == 0)
    {
        return {};
    }

    std::vector<char> buffer(maxLength + 1, '\0');
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), buffer.data(), maxLength, &bytesRead) ||
        bytesRead == 0)
    {
        return {};
    }

    std::string result;
    result.reserve(static_cast<size_t>(bytesRead));
    for (size_t index = 0; index < static_cast<size_t>(bytesRead); ++index)
    {
        const auto value = static_cast<unsigned char>(buffer[index]);
        if (value == 0)
        {
            break;
        }

        if (value < 0x20 || value > 0x7E)
        {
            if (result.empty())
            {
                return {};
            }
            break;
        }

        result.push_back(static_cast<char>(value));
    }

    return result;
}

bool MemoryWindowContainsAscii(uintptr_t address, size_t length, const char* pattern)
{
    if (address == 0 || length == 0 || pattern == nullptr)
    {
        return false;
    }

    std::vector<char> buffer(length);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(address), buffer.data(), buffer.size(), &bytesRead) ||
        bytesRead == 0)
    {
        return false;
    }

    const auto patternLength = std::strlen(pattern);
    return std::search(buffer.begin(), buffer.begin() + static_cast<std::ptrdiff_t>(bytesRead), pattern, pattern + patternLength) !=
        buffer.begin() + static_cast<std::ptrdiff_t>(bytesRead);
}

bool LooksLikePointer(uintptr_t value)
{
    return value > 0x10000ull && value < 0x0000800000000000ull;
}

std::string ReadCStringOrStringObject(uintptr_t address, size_t maxLength)
{
    auto direct = ReadAsciiString(address, maxLength);
    if (!direct.empty())
    {
        return direct;
    }

    uintptr_t textPointer = 0;
    if (ReadValue(address, textPointer) && LooksLikePointer(textPointer))
    {
        return ReadAsciiString(textPointer, maxLength);
    }

    return {};
}

std::string ReadAsciiFromPointerField(uintptr_t object, uintptr_t offset)
{
    uintptr_t textPointer = 0;
    if (!ReadValue(object + offset, textPointer))
    {
        return {};
    }

    return ReadCStringOrStringObject(textPointer, 160);
}

bool IsLikelyRadioStreamObject(uintptr_t object, uintptr_t expectedObjectVtable)
{
    uintptr_t objectVtable = 0;
    return ReadValue(object, objectVtable) && objectVtable == expectedObjectVtable;
}

bool ContainsText(const std::string& value, const char* text)
{
    return text != nullptr && value.find(text) != std::string::npos;
}

bool IsActiveRadioStreamCandidate(const RadioStreamCandidate& candidate)
{
    return candidate.object != 0 &&
        candidate.objectVtable != 0 &&
        LooksLikePointer(candidate.fmodSound) &&
        candidate.handle != 0 &&
        candidate.state != 0 &&
        ContainsText(candidate.sampleText, "/Master/Radio/Track") &&
        ContainsText(candidate.bankText, ".bank");
}

std::vector<int> ParseBytePattern(const char* pattern)
{
    std::vector<int> bytes;
    std::istringstream stream(pattern ? pattern : "");
    std::string token;
    while (stream >> token)
    {
        if (token == "?" || token == "??")
        {
            bytes.push_back(-1);
            continue;
        }

        try
        {
            bytes.push_back(std::stoi(token, nullptr, 16) & 0xff);
        }
        catch (...)
        {
            bytes.clear();
            return bytes;
        }
    }

    return bytes;
}

std::vector<uintptr_t> ScanBufferForBytePattern(
    const std::vector<uint8_t>& buffer,
    uintptr_t bufferAddress,
    const std::vector<int>& pattern,
    size_t maxHits)
{
    std::vector<uintptr_t> hits;
    if (pattern.empty() || buffer.size() < pattern.size())
    {
        return hits;
    }

    for (size_t offset = 0; offset + pattern.size() <= buffer.size(); ++offset)
    {
        bool matched = true;
        for (size_t index = 0; index < pattern.size(); ++index)
        {
            if (pattern[index] >= 0 && buffer[offset + index] != static_cast<uint8_t>(pattern[index]))
            {
                matched = false;
                break;
            }
        }

        if (matched)
        {
            hits.push_back(bufferAddress + offset);
            if (hits.size() >= maxHits)
            {
                break;
            }
        }
    }

    return hits;
}

void LogFmodApiPatternCandidates(const ModuleInfo& forza)
{
    if (forza.base == 0 || forza.size == 0 || forza.size > 256ull * 1024ull * 1024ull)
    {
        fh6rb::LogWarn("FMOD API sigscan skipped: Forza module size is invalid or unexpectedly large.");
        return;
    }

    std::vector<uint8_t> image(forza.size);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(forza.base),
            image.data(),
            image.size(),
            &bytesRead) ||
        bytesRead == 0)
    {
        fh6rb::LogWarn("FMOD API sigscan skipped: could not read Forza image.");
        return;
    }

    image.resize(static_cast<size_t>(bytesRead));
    for (const auto& patternInfo : kFmodApiPatterns)
    {
        const auto pattern = ParseBytePattern(patternInfo.bytes);
        const auto hits = ScanBufferForBytePattern(image, forza.base, pattern, 8);
        if (hits.empty())
        {
            fh6rb::LogInfo(std::string("FMOD API sigscan: ") + patternInfo.name + " hits=0");
            continue;
        }

        std::ostringstream stream;
        stream << "FMOD API sigscan: " << patternInfo.name << " hits=" << hits.size() << " rvas=";
        for (size_t index = 0; index < hits.size(); ++index)
        {
            if (index != 0)
            {
                stream << ",";
            }
            stream << Hex(hits[index] - forza.base);
        }
        fh6rb::LogInfo(stream.str());
    }
}

bool ReadModuleImage(const ModuleInfo& module, std::vector<uint8_t>& image)
{
    if (module.base == 0 || module.size == 0 || module.size > 256ull * 1024ull * 1024ull)
    {
        return false;
    }

    image.assign(module.size, 0);
    SIZE_T bytesRead = 0;
    if (!ReadProcessMemory(
            GetCurrentProcess(),
            reinterpret_cast<LPCVOID>(module.base),
            image.data(),
            image.size(),
            &bytesRead) ||
        bytesRead == 0)
    {
        image.clear();
        return false;
    }

    image.resize(static_cast<size_t>(bytesRead));
    return true;
}

template <typename T>
bool ReadImageValue(const std::vector<uint8_t>& image, size_t offset, T& value)
{
    if (offset + sizeof(T) > image.size())
    {
        return false;
    }

    std::memcpy(&value, image.data() + offset, sizeof(T));
    return true;
}

std::vector<uintptr_t> FindRttiVtableCandidates(const ModuleInfo& module, uintptr_t typeDescriptorRva)
{
    std::vector<uint8_t> image;
    if (!ReadModuleImage(module, image))
    {
        fh6rb::LogWarn("FMOD RTTI scan skipped: could not read Forza image.");
        return {};
    }

    std::vector<uintptr_t> completeObjectLocators;
    for (size_t offset = 0; offset + 24 <= image.size(); offset += 4)
    {
        uint32_t signature = 0;
        uint32_t objectOffset = 0;
        uint32_t cdOffset = 0;
        int32_t typeRva = 0;
        int32_t classRva = 0;
        int32_t selfRva = 0;
        ReadImageValue(image, offset + 0, signature);
        ReadImageValue(image, offset + 4, objectOffset);
        ReadImageValue(image, offset + 8, cdOffset);
        ReadImageValue(image, offset + 12, typeRva);
        ReadImageValue(image, offset + 16, classRva);
        ReadImageValue(image, offset + 20, selfRva);

        if (signature != 1 ||
            objectOffset > 0x1000 ||
            cdOffset > 0x1000 ||
            typeRva != static_cast<int32_t>(typeDescriptorRva) ||
            classRva <= 0 ||
            selfRva != static_cast<int32_t>(offset))
        {
            continue;
        }

        completeObjectLocators.push_back(module.base + offset);
    }

    std::vector<uintptr_t> vtables;
    for (const auto col : completeObjectLocators)
    {
        const uint64_t colPointer = static_cast<uint64_t>(col);
        for (size_t offset = 0; offset + sizeof(uint64_t) <= image.size(); offset += sizeof(uintptr_t))
        {
            uint64_t value = 0;
            ReadImageValue(image, offset, value);
            if (value != colPointer || offset + sizeof(uint64_t) >= image.size())
            {
                continue;
            }

            uint64_t firstFunction = 0;
            ReadImageValue(image, offset + sizeof(uint64_t), firstFunction);
            if (firstFunction < module.base || firstFunction >= module.base + module.size)
            {
                continue;
            }

            const uintptr_t vtable = module.base + offset + sizeof(uint64_t);
            if (std::find(vtables.begin(), vtables.end(), vtable) == vtables.end())
            {
                vtables.push_back(vtable);
            }
        }
    }

    std::ostringstream stream;
    stream << "FMOD RTTI scan: RadioStreamFmod COLs=" << completeObjectLocators.size() << " vtables=";
    for (size_t index = 0; index < vtables.size(); ++index)
    {
        if (index != 0)
        {
            stream << ",";
        }
        stream << Hex(vtables[index] - module.base);
    }
    fh6rb::LogInfo(stream.str());
    return vtables;
}

bool ScanBufferForQword(
    const std::vector<uint8_t>& buffer,
    uintptr_t bufferAddress,
    uint64_t value,
    std::vector<uintptr_t>& hits,
    const std::function<bool(uintptr_t)>& onHit)
{
    if (buffer.size() < sizeof(uint64_t) || hits.size() >= kMaxRadioStreamCandidates)
    {
        return false;
    }

    for (size_t offset = 0; offset + sizeof(uint64_t) <= buffer.size(); offset += sizeof(uintptr_t))
    {
        uint64_t candidate = 0;
        std::memcpy(&candidate, buffer.data() + offset, sizeof(candidate));
        if (candidate == value)
        {
            const uintptr_t hit = bufferAddress + offset;
            hits.push_back(hit);
            if (onHit && onHit(hit))
            {
                return true;
            }
            if (hits.size() >= kMaxRadioStreamCandidates)
            {
                return false;
            }
        }
    }

    return false;
}

bool ScanRangeForQword(
    uintptr_t begin,
    uintptr_t end,
    uint64_t value,
    std::vector<uintptr_t>& hits,
    const std::function<bool(uintptr_t)>& onHit)
{
    std::vector<uint8_t> buffer(kReadChunkBytes);
    uintptr_t cursor = begin;
    while (cursor < end && hits.size() < kMaxRadioStreamCandidates)
    {
        const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kReadChunkBytes, end - cursor));
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(cursor), buffer.data(), requested, &bytesRead) ||
            bytesRead == 0)
        {
            cursor += requested;
            continue;
        }

        buffer.resize(static_cast<size_t>(bytesRead));
        if (ScanBufferForQword(buffer, cursor, value, hits, onHit))
        {
            return true;
        }
        buffer.resize(kReadChunkBytes);

        uintptr_t next = cursor + bytesRead;
        if (bytesRead > sizeof(uint64_t))
        {
            next -= sizeof(uint64_t) - 1;
        }
        if (next <= cursor)
        {
            break;
        }
        cursor = next;
    }

    return false;
}

std::vector<uintptr_t> FindQwordInPrivateMemory(
    uint64_t value,
    const std::string& label,
    const std::function<bool(uintptr_t)>& onHit = {})
{
    std::vector<uintptr_t> hits;
    const auto startedAt = std::chrono::steady_clock::now();

    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    uintptr_t cursor = std::max(
        reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress),
        kMinLikelyGameHeapAddress);
    const uintptr_t maxAddress = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    size_t scannedBytes = 0;
    size_t scannedRegions = 0;
    bool stoppedByVisitor = false;

    while (cursor < maxAddress &&
        hits.size() < kMaxRadioStreamCandidates &&
        scannedBytes < kMaxObjectProbeBytes &&
        scannedRegions < kMaxObjectProbeRegions &&
        std::chrono::steady_clock::now() - startedAt < kMaxObjectProbeDuration)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0)
        {
            break;
        }

        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBase + mbi.RegionSize;
        const bool shouldScan = mbi.State == MEM_COMMIT &&
            mbi.Type == MEM_PRIVATE &&
            IsReadableProtection(mbi.Protect);

        if (shouldScan)
        {
            const size_t remainingBudget = kMaxObjectProbeBytes - scannedBytes;
            const uintptr_t scanEnd = regionBase + std::min<uintptr_t>(mbi.RegionSize, remainingBudget);
            if (ScanRangeForQword(regionBase, scanEnd, value, hits, onHit))
            {
                stoppedByVisitor = true;
            }
            scannedBytes += static_cast<size_t>(scanEnd - regionBase);
            ++scannedRegions;
            if (stoppedByVisitor)
            {
                break;
            }
        }

        if (regionEnd <= cursor)
        {
            break;
        }
        cursor = regionEnd;
    }

    fh6rb::LogInfo("FMOD object probe qword scan complete: label=" + label +
        " hits=" + std::to_string(hits.size()) +
        " scannedBytes=" + std::to_string(scannedBytes) +
        " scannedRegions=" + std::to_string(scannedRegions) +
        " stoppedByVisitor=" + std::string(stoppedByVisitor ? "true" : "false"));
    return hits;
}

void ReadRadioStreamCandidateFields(RadioStreamCandidate& candidate)
{
    ReadValue(candidate.object, candidate.objectVtable);
    ReadValue(candidate.object + 0x08, candidate.fmodSound);
    ReadValue(candidate.object + 0x10, candidate.handle);
    ReadValue(candidate.object + 0x28, candidate.state);
    ReadValue(candidate.object + 0x40, candidate.eventString);
    ReadValue(candidate.object + 0x48, candidate.eventStringRefCount);
    ReadValue(candidate.object + 0x50, candidate.sampleData);

    if (LooksLikePointer(candidate.eventString))
    {
        candidate.eventText = ReadCStringOrStringObject(candidate.eventString, 160);
    }
    if (LooksLikePointer(candidate.sampleData))
    {
        candidate.sampleText = ReadAsciiString(candidate.sampleData, 160);
    }
    if (LooksLikePointer(candidate.fmodSound))
    {
        ReadValue(candidate.fmodSound + 0xC0, candidate.systemI);

        uintptr_t bankString = 0;
        if (ReadValue(candidate.fmodSound + 0x18, bankString) && LooksLikePointer(bankString))
        {
            candidate.bankText = ReadAsciiString(bankString, 160);
            candidate.bankString = bankString;
        }

        if (candidate.bankText.empty() && ReadValue(candidate.fmodSound + 0x20, bankString) && LooksLikePointer(bankString))
        {
            candidate.bankText = ReadAsciiString(bankString, 160);
            candidate.bankString = bankString;
        }
    }

}

RadioStreamCandidate ReadRadioStreamCandidateFromRefCount(uintptr_t refCount, uintptr_t expectedObjectVtable)
{
    RadioStreamCandidate candidate;
    candidate.refCount = refCount;
    candidate.object = refCount + 0x10;
    ReadRadioStreamCandidateFields(candidate);

    if (candidate.objectVtable != expectedObjectVtable)
    {
        candidate.object = 0;
    }

    return candidate;
}

RadioStreamCandidate ReadRadioStreamCandidateFromObject(uintptr_t object, uintptr_t expectedObjectVtable)
{
    RadioStreamCandidate candidate;
    candidate.refCount = object >= 0x10 ? object - 0x10 : 0;
    candidate.object = object;
    ReadRadioStreamCandidateFields(candidate);

    if (candidate.objectVtable != expectedObjectVtable)
    {
        candidate.object = 0;
    }

    return candidate;
}

void LogRadioStreamCandidate(const RadioStreamCandidate& candidate)
{
    std::ostringstream stream;
    stream << "FMOD object probe: radioStream=" << Hex(candidate.object)
           << " refCount=" << Hex(candidate.refCount)
           << " objectVtable=" << Hex(candidate.objectVtable)
           << " handle=" << Hex(candidate.handle)
           << " state=" << candidate.state
           << " fmodSound=" << Hex(candidate.fmodSound)
           << " systemI=" << Hex(candidate.systemI)
           << " eventPtr=" << Hex(candidate.eventString)
           << " eventRef=" << Hex(candidate.eventStringRefCount)
           << " sampleData=" << Hex(candidate.sampleData)
           << " bankPtr=" << Hex(candidate.bankString);

    if (!candidate.eventText.empty())
    {
        stream << " event=\"" << candidate.eventText << "\"";
    }
    if (!candidate.sampleText.empty())
    {
        stream << " sample=\"" << candidate.sampleText << "\"";
    }
    if (!candidate.bankText.empty())
    {
        stream << " bank=\"" << candidate.bankText << "\"";
    }

    fh6rb::LogInfo(stream.str());
}

void ProbeRadioStreamObjects(const std::vector<ModuleInfo>& modules)
{
    const auto* forza = FindForzaModule(modules);
    if (!forza)
    {
        fh6rb::LogWarn("FMOD object probe skipped: Forza executable module not found.");
        return;
    }

    std::ostringstream header;
    header << "FMOD object probe version guard: module=" << WideToUtf8(forza->name)
           << " base=" << Hex(forza->base)
           << " size=" << forza->size
           << " expectedSize=" << kExpectedForzaExeSize;
    fh6rb::LogInfo(header.str());

    if (forza->size != kExpectedForzaExeSize)
    {
        fh6rb::LogWarn("FMOD object probe skipped: executable size does not match known FH6 build.");
        return;
    }

    if (!MemoryWindowContainsAscii(forza->base + kRvaRadioStreamFmodTypeName, 512, "RadioStreamFmod"))
    {
        fh6rb::LogWarn("FMOD object probe skipped: RadioStreamFmod RTTI guard did not match.");
        return;
    }

    const uintptr_t refCountVtable = forza->base + kRvaRadioStreamFmodRefCountVtable;
    const uintptr_t hardcodedObjectVtable = forza->base + kRvaRadioStreamFmodObjectVtable;
    auto objectVtableCandidates = FindRttiVtableCandidates(*forza, kRvaRadioStreamFmodTypeName - 0x10);
    if (std::find(objectVtableCandidates.begin(), objectVtableCandidates.end(), hardcodedObjectVtable) ==
        objectVtableCandidates.end())
    {
        objectVtableCandidates.push_back(hardcodedObjectVtable);
    }

    LogFmodApiPatternCandidates(*forza);
    fh6rb::LogInfo("FMOD object probe scanning heap for RadioStreamFmod ref_count vtable=" + Hex(refCountVtable) +
        " objectVtableCandidates=" + std::to_string(objectVtableCandidates.size()));

    std::vector<RadioStreamCandidate> discovered;
    auto alreadyDiscovered = [&discovered](uintptr_t object) {
        return std::any_of(discovered.begin(), discovered.end(), [object](const RadioStreamCandidate& candidate) {
            return candidate.object == object;
        });
    };

    auto recordCandidate = [&](const RadioStreamCandidate& candidate, const char* source) {
        if (candidate.object == 0 || alreadyDiscovered(candidate.object))
        {
            return false;
        }

        fh6rb::LogInfo(std::string("FMOD object probe candidate source=") + source);
        LogRadioStreamCandidate(candidate);
        discovered.push_back(candidate);
        return IsActiveRadioStreamCandidate(candidate);
    };

    const auto refCountHits = FindQwordInPrivateMemory(refCountVtable, "RadioStreamFmod ref_count vtable", [&](uintptr_t refCount) {
        for (const auto objectVtable : objectVtableCandidates)
        {
            const auto candidate = ReadRadioStreamCandidateFromRefCount(refCount, objectVtable);
            if (recordCandidate(candidate, "ref_count"))
            {
                return true;
            }
        }
        return false;
    });
    if (refCountHits.empty())
    {
        fh6rb::LogInfo("FMOD object probe: no RadioStreamFmod ref_count objects found.");
    }

    for (const auto refCount : refCountHits)
    {
        const uintptr_t object = refCount + 0x10;
        const auto matchingVtable = std::find_if(
            objectVtableCandidates.begin(),
            objectVtableCandidates.end(),
            [object](uintptr_t objectVtable) {
                return IsLikelyRadioStreamObject(object, objectVtable);
            });
        if (matchingVtable == objectVtableCandidates.end())
        {
            uintptr_t actualVtable = 0;
            ReadValue(object, actualVtable);
            fh6rb::LogInfo("FMOD object probe rejected refCount=" + Hex(refCount) +
                " object=" + Hex(object) + " objectVtable=" + Hex(actualVtable));
            continue;
        }

        const auto candidate = ReadRadioStreamCandidateFromRefCount(refCount, *matchingVtable);
        recordCandidate(candidate, "ref_count-postscan");
    }

    if (std::none_of(discovered.begin(), discovered.end(), IsActiveRadioStreamCandidate))
    {
        for (const auto objectVtable : objectVtableCandidates)
        {
            if (std::any_of(discovered.begin(), discovered.end(), IsActiveRadioStreamCandidate))
            {
                break;
            }

            fh6rb::LogInfo("FMOD object probe scanning heap for RadioStreamFmod object vtable=" + Hex(objectVtable));
            FindQwordInPrivateMemory(objectVtable, "RadioStreamFmod object vtable", [&](uintptr_t object) {
                const auto candidate = ReadRadioStreamCandidateFromObject(object, objectVtable);
                return recordCandidate(candidate, "object_vtable");
            });
        }
    }

    const auto activeCount = static_cast<size_t>(
        std::count_if(discovered.begin(), discovered.end(), IsActiveRadioStreamCandidate));
    fh6rb::LogInfo("FMOD object probe complete: refCountHits=" + std::to_string(refCountHits.size()) +
        " discoveredRadioStreams=" + std::to_string(discovered.size()) +
        " activeRadioStreams=" + std::to_string(activeCount));
}

void ScanBufferForPattern(
    const std::vector<uint8_t>& buffer,
    uintptr_t bufferAddress,
    PatternResult& result)
{
    if (buffer.empty() || !result.pattern || result.truncated)
    {
        return;
    }

    const auto* patternBegin = reinterpret_cast<const uint8_t*>(result.pattern->text);
    const size_t patternLength = std::strlen(result.pattern->text);
    if (patternLength == 0 || buffer.size() < patternLength)
    {
        return;
    }

    auto searchStart = buffer.begin();
    while (searchStart != buffer.end())
    {
        const auto found = std::search(searchStart, buffer.end(), patternBegin, patternBegin + patternLength);
        if (found == buffer.end())
        {
            return;
        }

        const auto offset = static_cast<size_t>(std::distance(buffer.begin(), found));
        const uintptr_t hitAddress = bufferAddress + offset;
        if (result.firstHits.empty() || result.firstHits.back() != hitAddress)
        {
            if (result.firstHits.size() < kMaxHitsPerPattern)
            {
                result.firstHits.push_back(hitAddress);
            }
            ++result.hits;
        }

        if (result.hits >= kMaxHitsPerPattern)
        {
            result.truncated = true;
            return;
        }

        searchStart = found + 1;
    }
}

std::vector<PatternResult> NewResults()
{
    std::vector<PatternResult> results;
    results.reserve(kPatterns.size());
    for (const auto& pattern : kPatterns)
    {
        PatternResult result;
        result.pattern = &pattern;
        results.push_back(std::move(result));
    }
    return results;
}

std::vector<PatternResult> ScanReadableRange(uintptr_t begin, uintptr_t end)
{
    std::vector<PatternResult> results = NewResults();
    std::vector<uint8_t> buffer(kReadChunkBytes);
    size_t maxPatternLength = 0;
    for (const auto& pattern : kPatterns)
    {
        maxPatternLength = std::max(maxPatternLength, std::strlen(pattern.text));
    }

    uintptr_t cursor = begin;
    while (cursor < end)
    {
        const size_t requested = static_cast<size_t>(std::min<uintptr_t>(kReadChunkBytes, end - cursor));
        SIZE_T bytesRead = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<LPCVOID>(cursor), buffer.data(), requested, &bytesRead) || bytesRead == 0)
        {
            cursor += requested;
            continue;
        }

        buffer.resize(static_cast<size_t>(bytesRead));
        for (auto& result : results)
        {
            ScanBufferForPattern(buffer, cursor, result);
        }

        buffer.resize(kReadChunkBytes);

        uintptr_t next = cursor + bytesRead;
        if (bytesRead > maxPatternLength && maxPatternLength > 1)
        {
            next -= maxPatternLength - 1;
        }

        if (next <= cursor)
        {
            break;
        }
        cursor = next;
    }

    return results;
}

void LogResults(const std::string& prefix, const std::vector<PatternResult>& results)
{
    for (const auto& result : results)
    {
        if (result.hits == 0)
        {
            continue;
        }

        std::ostringstream stream;
        stream << prefix << " pattern=" << result.pattern->name << " hits=" << result.hits;
        if (result.truncated)
        {
            stream << "+";
        }
        stream << " first=";
        for (size_t index = 0; index < result.firstHits.size(); ++index)
        {
            if (index != 0)
            {
                stream << ",";
            }
            stream << Hex(result.firstHits[index]);
        }
        fh6rb::LogInfo(stream.str());
    }
}

bool AnyHits(const std::vector<PatternResult>& results)
{
    return std::any_of(results.begin(), results.end(), [](const PatternResult& result) {
        return result.hits > 0;
    });
}

void LogModuleInventory(const std::vector<ModuleInfo>& modules, const std::wstring& proxyPath)
{
    fh6rb::LogInfo("FMOD probe module inventory: count=" + std::to_string(modules.size()));
    for (const auto& module : modules)
    {
        std::ostringstream stream;
        stream << "FMOD probe module: base=" << Hex(module.base)
               << " size=" << module.size
               << " name=" << WideToUtf8(module.name)
               << " path=" << WideToUtf8(module.path);
        if (SamePath(module.path, proxyPath))
        {
            stream << " self=true";
        }
        fh6rb::LogInfo(stream.str());
    }
}

void ScanModules(const std::vector<ModuleInfo>& modules, const std::wstring& proxyPath)
{
    for (const auto& module : modules)
    {
        if (module.base == 0 || module.size == 0)
        {
            continue;
        }

        if (SamePath(module.path, proxyPath))
        {
            continue;
        }

        uintptr_t cursor = module.base;
        const uintptr_t end = module.base + module.size;
        while (cursor < end)
        {
            MEMORY_BASIC_INFORMATION mbi{};
            if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0)
            {
                break;
            }

            const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
            const uintptr_t regionEnd = regionBase + mbi.RegionSize;
            const uintptr_t scanBegin = std::max(cursor, regionBase);
            const uintptr_t scanEnd = std::min(end, regionEnd);

            if (scanBegin < scanEnd && mbi.State == MEM_COMMIT && IsReadableProtection(mbi.Protect))
            {
                const auto results = ScanReadableRange(scanBegin, scanEnd);
                if (AnyHits(results))
                {
                    std::ostringstream prefix;
                    prefix << "FMOD probe module hit: module=" << WideToUtf8(module.name)
                           << " base=" << Hex(module.base)
                           << " region=" << Hex(scanBegin) << "-" << Hex(scanEnd);
                    LogResults(prefix.str(), results);
                }
            }

            if (regionEnd <= cursor)
            {
                break;
            }
            cursor = regionEnd;
        }
    }
}

void ScanDeepReadableMemory(const std::vector<ModuleInfo>& modules)
{
    fh6rb::LogInfo("FMOD probe deep scan starting: maxBytes=" + std::to_string(kMaxDeepScanBytes) +
        " maxRegions=" + std::to_string(kMaxDeepScanRegions));

    SYSTEM_INFO systemInfo{};
    GetNativeSystemInfo(&systemInfo);
    uintptr_t cursor = reinterpret_cast<uintptr_t>(systemInfo.lpMinimumApplicationAddress);
    const uintptr_t maxAddress = reinterpret_cast<uintptr_t>(systemInfo.lpMaximumApplicationAddress);
    size_t scannedBytes = 0;
    size_t scannedRegions = 0;

    while (cursor < maxAddress && scannedBytes < kMaxDeepScanBytes && scannedRegions < kMaxDeepScanRegions)
    {
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(reinterpret_cast<LPCVOID>(cursor), &mbi, sizeof(mbi)) == 0)
        {
            break;
        }

        const uintptr_t regionBase = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
        const uintptr_t regionEnd = regionBase + mbi.RegionSize;
        const bool shouldScan = mbi.State == MEM_COMMIT &&
            mbi.Type != MEM_IMAGE &&
            IsReadableProtection(mbi.Protect) &&
            !IsInsideAnyModule(regionBase, modules);

        if (shouldScan)
        {
            const size_t remainingBudget = kMaxDeepScanBytes - scannedBytes;
            const uintptr_t scanEnd = regionBase + std::min<uintptr_t>(mbi.RegionSize, remainingBudget);
            const auto results = ScanReadableRange(regionBase, scanEnd);
            scannedBytes += static_cast<size_t>(scanEnd - regionBase);
            ++scannedRegions;

            if (AnyHits(results))
            {
                std::ostringstream prefix;
                prefix << "FMOD probe deep hit: type=" << MemoryTypeString(mbi.Type)
                       << " protect=" << ProtectString(mbi.Protect)
                       << " region=" << Hex(regionBase) << "-" << Hex(scanEnd);
                LogResults(prefix.str(), results);
            }
        }

        if (regionEnd <= cursor)
        {
            break;
        }
        cursor = regionEnd;
    }

    fh6rb::LogInfo("FMOD probe deep scan complete: scannedBytes=" + std::to_string(scannedBytes) +
        " scannedRegions=" + std::to_string(scannedRegions));
}

bool ReadRequest(const std::wstring& path, uint64_t& lastWrite, bool& deep)
{
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data))
    {
        return false;
    }

    const uint64_t writeTime =
        (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) |
        static_cast<uint64_t>(data.ftLastWriteTime.dwLowDateTime);

    if (writeTime == 0 || writeTime == lastWrite)
    {
        return false;
    }

    lastWrite = writeTime;
    deep = false;

    try
    {
        std::ifstream stream(std::filesystem::path(path), std::ios::binary);
        std::string content((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
        std::transform(content.begin(), content.end(), content.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        deep = content.find("deep") != std::string::npos;
    }
    catch (...)
    {
    }

    return true;
}

void RunProbeScan(const std::wstring& baseDirectory, const std::string& reason, bool deep)
{
    const auto proxyPath = (std::filesystem::path(baseDirectory) / L"version.dll").wstring();
    fh6rb::LogInfo("FMOD probe scan starting: reason=" + reason + " deep=" + std::string(deep ? "true" : "false"));

    const auto modules = EnumerateModules();
    LogModuleInventory(modules, proxyPath);
    ScanModules(modules, proxyPath);
    ProbeRadioStreamObjects(modules);
    if (deep)
    {
        ScanDeepReadableMemory(modules);
    }

    fh6rb::LogInfo("FMOD probe scan complete: reason=" + reason);
}

void ProbeWorker(std::wstring baseDirectory)
{
    fh6rb::LogInfo("FMOD/radio memory probe worker started.");

    std::this_thread::sleep_for(std::chrono::seconds(5));
    if (!fh6rb::IsFmodProbeEnabled(baseDirectory))
    {
        fh6rb::LogInfo("FMOD/radio memory probe worker exiting; flag disabled before initial scan.");
        return;
    }

    RunProbeScan(baseDirectory, "startup+5s", false);

    uint64_t lastRequestWrite = 0;
    const auto requestPath = fh6rb::FmodProbeRequestPath(baseDirectory);
    for (int tick = 0; tick < 600; ++tick)
    {
        if (!fh6rb::IsFmodProbeEnabled(baseDirectory))
        {
            fh6rb::LogInfo("FMOD/radio memory probe worker exiting; flag disabled.");
            return;
        }

        bool deep = false;
        if (ReadRequest(requestPath, lastRequestWrite, deep))
        {
            RunProbeScan(baseDirectory, deep ? "manual-deep-request" : "manual-module-request", deep);
        }

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    fh6rb::LogInfo("FMOD/radio memory probe worker exiting after watch window.");
}
}

namespace fh6rb
{
std::wstring FmodProbeFlagPath(const std::wstring& baseDirectory)
{
    return (std::filesystem::path(ProbeDirectory(baseDirectory)) / L"enable_fmod_probe.flag").wstring();
}

std::wstring FmodProbeRequestPath(const std::wstring& baseDirectory)
{
    return (std::filesystem::path(ProbeDirectory(baseDirectory)) / L"request_fmod_probe_scan.flag").wstring();
}

bool IsFmodProbeEnabled(const std::wstring& baseDirectory)
{
    try
    {
        return std::filesystem::exists(FmodProbeFlagPath(baseDirectory));
    }
    catch (...)
    {
        return false;
    }
}

bool StartFmodMemoryProbe(const std::wstring& baseDirectory)
{
    try
    {
        std::thread(ProbeWorker, baseDirectory).detach();
        return true;
    }
    catch (...)
    {
        return false;
    }
}
}
