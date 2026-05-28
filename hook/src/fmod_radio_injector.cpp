#include "fmod_radio_injector.h"

#include "logger.h"
#include "shared_audio_reader.h"

#include <Windows.h>
#include <Psapi.h>
#include <winhttp.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace
{
constexpr std::array<size_t, 2> kSupportedForzaExeSizes{
    187621376, // FH6 build used during initial injector rollout
    187625472  // FH6 3.364.933.0
};
constexpr const char* kTargetSoundName = "HZ6_R9_PeterBroderick_EyesClosedandTraveling";
constexpr size_t kMaxRadioInstances = 64;

std::atomic<fh6rb::SharedAudioRingReader*> g_reader = nullptr;
std::atomic<float> g_gain = 1.0f;
std::atomic<uint64_t> g_dspCalls = 0;
std::atomic<uint64_t> g_dspUnderruns = 0;

std::string Hex(uintptr_t value)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << std::uppercase << value;
    return stream.str();
}

bool LooksLikePointer(uintptr_t value)
{
    return value > 0x10000ull && value < 0x0000800000000000ull;
}

bool IsReadable(const void* address, size_t size)
{
    if (!address || size == 0)
    {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(address, &mbi, sizeof(mbi)) != sizeof(mbi) || mbi.State != MEM_COMMIT ||
        (mbi.Protect & PAGE_GUARD) != 0 || (mbi.Protect & PAGE_NOACCESS) != 0)
    {
        return false;
    }

    const DWORD protection = mbi.Protect & 0xff;
    const bool readable = protection == PAGE_READONLY ||
        protection == PAGE_READWRITE ||
        protection == PAGE_WRITECOPY ||
        protection == PAGE_EXECUTE_READ ||
        protection == PAGE_EXECUTE_READWRITE ||
        protection == PAGE_EXECUTE_WRITECOPY;
    if (!readable)
    {
        return false;
    }

    const auto* begin = static_cast<const uint8_t*>(address);
    const auto* regionBegin = static_cast<const uint8_t*>(mbi.BaseAddress);
    const auto* regionEnd = regionBegin + mbi.RegionSize;
    return begin >= regionBegin && begin + size <= regionEnd;
}

template <typename T>
bool SafeRead(const void* address, T& output)
{
    if (!IsReadable(address, sizeof(T)))
    {
        return false;
    }

    std::memcpy(&output, address, sizeof(T));
    return true;
}

std::optional<std::string> SafeReadMsvcString(const void* address, size_t maxSize = 4096)
{
    struct StringHeader
    {
        uint8_t sbo[16];
        uint64_t size;
        uint64_t capacity;
    } header{};

    if (!SafeRead(address, header) || header.size > maxSize)
    {
        return std::nullopt;
    }

    if (header.capacity >= 16)
    {
        void* data = nullptr;
        std::memcpy(&data, header.sbo, sizeof(data));
        if (!data || !IsReadable(data, static_cast<size_t>(header.size)))
        {
            return std::nullopt;
        }

        std::string value(static_cast<size_t>(header.size), '\0');
        std::memcpy(value.data(), data, value.size());
        return value;
    }

    if (header.size > 16)
    {
        return std::nullopt;
    }

    return std::string(reinterpret_cast<const char*>(header.sbo), static_cast<size_t>(header.size));
}

struct MsvcStringHeader
{
    uint8_t sbo[16];
    uint64_t size;
    uint64_t capacity;
};

static_assert(sizeof(MsvcStringHeader) == 32);

template <typename Fn>
bool SehCall(Fn&& fn)
{
    __try
    {
        fn();
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

bool WriteMsvcString(uint8_t* target, std::string_view source)
{
    if (!target)
    {
        return false;
    }

    MsvcStringHeader header{};
    if (!SafeRead(target, header) || header.capacity < 15 || header.size > header.capacity)
    {
        return false;
    }

    const uint64_t newSize = static_cast<uint64_t>(source.size());
    const bool isHeapString = header.capacity > 15;

    if (isHeapString && newSize <= header.capacity)
    {
        uint8_t* heap = nullptr;
        std::memcpy(&heap, header.sbo, sizeof(heap));
        if (!heap || !IsReadable(heap, static_cast<size_t>(header.capacity)))
        {
            return false;
        }

        return SehCall([&] {
            std::memcpy(heap, source.data(), source.size());
            heap[source.size()] = 0;
            std::memcpy(target + 16, &newSize, sizeof(newSize));
        });
    }

    if (!isHeapString && newSize < 16)
    {
        return SehCall([&] {
            std::memset(target, 0, 16);
            std::memcpy(target, source.data(), source.size());
            std::memcpy(target + 16, &newSize, sizeof(newSize));
        });
    }

    const uint64_t newCapacity = (newSize + 32) & ~uint64_t{15};
    auto* fresh = static_cast<uint8_t*>(VirtualAlloc(nullptr, static_cast<SIZE_T>(newCapacity + 1), MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!fresh)
    {
        return false;
    }

    std::memcpy(fresh, source.data(), source.size());
    fresh[source.size()] = 0;

    const bool wrote = SehCall([&] {
        std::memset(target, 0, 16);
        std::memcpy(target, &fresh, sizeof(fresh));
        std::memcpy(target + 16, &newSize, sizeof(newSize));
        std::memcpy(target + 24, &newCapacity, sizeof(newCapacity));
    });
    if (!wrote)
    {
        VirtualFree(fresh, 0, MEM_RELEASE);
    }
    return wrote;
}

struct PeSection
{
    std::array<char, 9> name{};
    uint8_t* start = nullptr;
    uint8_t* end = nullptr;
    uint32_t characteristics = 0;

    bool Readable() const
    {
        return (characteristics & IMAGE_SCN_MEM_READ) != 0;
    }
};

struct PeImage
{
    uint8_t* base = nullptr;
    size_t size = 0;
    uint8_t* text = nullptr;
    uint8_t* textEnd = nullptr;
    uint8_t* rdata = nullptr;
    uint8_t* rdataEnd = nullptr;
    std::vector<PeSection> sections;
    std::vector<uint32_t> functionRvas;

    bool Valid() const
    {
        return base && text && textEnd && rdata && rdataEnd && !functionRvas.empty();
    }
};

PeImage ParseImage(uint8_t* base)
{
    PeImage image;
    if (!base)
    {
        return image;
    }

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
    {
        return image;
    }

    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE)
    {
        return image;
    }

    image.base = base;
    image.size = nt->OptionalHeader.SizeOfImage;

    auto* section = IMAGE_FIRST_SECTION(nt);
    image.sections.reserve(nt->FileHeader.NumberOfSections);
    for (WORD index = 0; index < nt->FileHeader.NumberOfSections; ++index, ++section)
    {
        PeSection parsed;
        std::memcpy(parsed.name.data(), section->Name, 8);
        parsed.start = base + section->VirtualAddress;
        parsed.end = parsed.start + section->Misc.VirtualSize;
        parsed.characteristics = section->Characteristics;
        image.sections.push_back(parsed);

        if (std::strncmp(parsed.name.data(), ".text", 8) == 0)
        {
            image.text = parsed.start;
            image.textEnd = parsed.end;
        }
        else if (std::strncmp(parsed.name.data(), ".rdata", 8) == 0)
        {
            image.rdata = parsed.start;
            image.rdataEnd = parsed.end;
        }
    }

    const auto& directory = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    if (directory.VirtualAddress != 0 && directory.Size >= sizeof(RUNTIME_FUNCTION))
    {
        auto* functions = reinterpret_cast<RUNTIME_FUNCTION*>(base + directory.VirtualAddress);
        const size_t count = directory.Size / sizeof(RUNTIME_FUNCTION);
        image.functionRvas.reserve(count);
        for (size_t index = 0; index < count; ++index)
        {
            auto function = functions[index];
            for (int hop = 0; hop < 16; ++hop)
            {
                if (function.UnwindData == 0 || function.UnwindData + 4 > image.size)
                {
                    break;
                }

                auto* unwind = base + function.UnwindData;
                if ((unwind[0] & 0x20) == 0)
                {
                    break;
                }

                const size_t codes = (2ull * unwind[2] + 3ull) & ~3ull;
                if (function.UnwindData + codes + 4 + sizeof(RUNTIME_FUNCTION) > image.size)
                {
                    break;
                }

                std::memcpy(&function, unwind + 4 + codes, sizeof(function));
            }

            if (function.BeginAddress != 0)
            {
                image.functionRvas.push_back(function.BeginAddress);
            }
        }
        std::sort(image.functionRvas.begin(), image.functionRvas.end());
        image.functionRvas.erase(std::unique(image.functionRvas.begin(), image.functionRvas.end()), image.functionRvas.end());
    }

    return image;
}

struct ByteOrWildcard
{
    uint8_t value = 0;
    bool wildcard = false;
};

std::vector<ByteOrWildcard> ParseOnePattern(std::string_view text)
{
    std::vector<ByteOrWildcard> pattern;
    auto hex = [](char value) -> int {
        if (value >= '0' && value <= '9')
        {
            return value - '0';
        }
        if (value >= 'a' && value <= 'f')
        {
            return value - 'a' + 10;
        }
        if (value >= 'A' && value <= 'F')
        {
            return value - 'A' + 10;
        }
        return -1;
    };

    for (size_t index = 0; index < text.size();)
    {
        if (text[index] == ' ' || text[index] == '\t')
        {
            ++index;
            continue;
        }
        if (text[index] == '?' && index + 1 < text.size() && text[index + 1] == '?')
        {
            pattern.push_back({0, true});
            index += 2;
            continue;
        }
        if (index + 1 < text.size())
        {
            const int hi = hex(text[index]);
            const int lo = hex(text[index + 1]);
            if (hi >= 0 && lo >= 0)
            {
                pattern.push_back({static_cast<uint8_t>((hi << 4) | lo), false});
                index += 2;
                continue;
            }
        }
        ++index;
    }
    return pattern;
}

std::vector<std::vector<ByteOrWildcard>> ParsePatterns(std::string_view text)
{
    std::vector<std::vector<ByteOrWildcard>> patterns;
    size_t start = 0;
    for (size_t index = 0; index <= text.size(); ++index)
    {
        if (index == text.size() || text[index] == '|')
        {
            patterns.push_back(ParseOnePattern(text.substr(start, index - start)));
            start = index + 1;
        }
    }
    return patterns;
}

bool MatchesPattern(uint8_t* position, uint8_t* end, const std::vector<ByteOrWildcard>& pattern)
{
    if (pattern.empty() || static_cast<size_t>(end - position) < pattern.size())
    {
        return false;
    }

    for (size_t index = 0; index < pattern.size(); ++index)
    {
        if (!pattern[index].wildcard && position[index] != pattern[index].value)
        {
            return false;
        }
    }
    return true;
}

bool MatchesAnyPattern(uint8_t* position, uint8_t* end, const std::vector<std::vector<ByteOrWildcard>>& patterns)
{
    return std::any_of(patterns.begin(), patterns.end(), [&](const auto& pattern) {
        return MatchesPattern(position, end, pattern);
    });
}

std::vector<uint8_t*> FindAnchorStrings(const PeImage& image, std::string_view anchor)
{
    std::vector<uint8_t*> hits;
    if (anchor.empty() || image.rdataEnd <= image.rdata || static_cast<size_t>(image.rdataEnd - image.rdata) < anchor.size() + 1)
    {
        return hits;
    }

    for (uint8_t* position = image.rdata; position + anchor.size() + 1 < image.rdataEnd; ++position)
    {
        if (std::memcmp(position, anchor.data(), anchor.size()) == 0 &&
            position[anchor.size()] == 0 &&
            (position == image.rdata || position[-1] == 0))
        {
            hits.push_back(position);
        }
    }
    return hits;
}

std::vector<uint8_t*> FindLeaToTargets(const PeImage& image, const std::vector<uint8_t*>& targets)
{
    std::vector<uint8_t*> hits;
    if (targets.empty() || image.textEnd <= image.text + 7)
    {
        return hits;
    }

    auto sortedTargets = targets;
    std::sort(sortedTargets.begin(), sortedTargets.end());

    for (uint8_t* position = image.text; position + 7 < image.textEnd; ++position)
    {
        const auto prefix = position[0];
        if (prefix != 0x48 && prefix != 0x4C)
        {
            continue;
        }
        if (position[1] != 0x8D || (position[2] & 0xC7) != 0x05)
        {
            continue;
        }

        int32_t displacement = 0;
        std::memcpy(&displacement, position + 3, sizeof(displacement));
        auto* target = position + 7 + displacement;
        if (std::binary_search(sortedTargets.begin(), sortedTargets.end(), target))
        {
            hits.push_back(position);
        }
    }

    return hits;
}

uint8_t* FindByAnchor(const PeImage& image, std::string_view anchor, std::string_view pattern)
{
    const auto anchors = FindAnchorStrings(image, anchor);
    if (anchors.empty())
    {
        fh6rb::LogWarn("FMOD DSP sigscan: anchor not found: " + std::string(anchor));
        return nullptr;
    }

    const auto leas = FindLeaToTargets(image, anchors);
    if (leas.empty())
    {
        fh6rb::LogWarn("FMOD DSP sigscan: anchor has no LEA targets: " + std::string(anchor));
        return nullptr;
    }

    const auto patterns = ParsePatterns(pattern);
    std::vector<uint8_t*> hits;
    for (auto* lea : leas)
    {
        const auto rva = static_cast<uint32_t>(lea - image.base);
        auto it = std::upper_bound(image.functionRvas.begin(), image.functionRvas.end(), rva);
        if (it == image.functionRvas.begin())
        {
            continue;
        }
        --it;

        auto* function = image.base + *it;
        if (MatchesAnyPattern(function, image.textEnd, patterns) &&
            std::find(hits.begin(), hits.end(), function) == hits.end())
        {
            hits.push_back(function);
        }
    }

    if (hits.size() != 1)
    {
        fh6rb::LogWarn("FMOD DSP sigscan: anchor " + std::string(anchor) +
            " candidates=" + std::to_string(hits.size()));
        return nullptr;
    }
    return hits.front();
}

uint8_t* FindByPattern(const PeImage& image, std::string_view pattern)
{
    const auto patterns = ParsePatterns(pattern);
    uint8_t* hit = nullptr;
    int count = 0;

    for (const auto rva : image.functionRvas)
    {
        auto* function = image.base + rva;
        if (function >= image.text && function < image.textEnd && MatchesAnyPattern(function, image.textEnd, patterns))
        {
            if (++count > 1)
            {
                fh6rb::LogWarn("FMOD DSP sigscan: direct pattern ambiguous in pdata.");
                return nullptr;
            }
            hit = function;
        }
    }
    if (hit)
    {
        return hit;
    }

    count = 0;
    for (auto* position = image.text; position < image.textEnd; ++position)
    {
        if (MatchesAnyPattern(position, image.textEnd, patterns))
        {
            if (++count > 1)
            {
                fh6rb::LogWarn("FMOD DSP sigscan: direct pattern ambiguous in text.");
                return nullptr;
            }
            hit = position;
        }
    }

    if (!hit)
    {
        fh6rb::LogWarn("FMOD DSP sigscan: direct pattern not found.");
    }
    return hit;
}

struct FmodFunctions
{
    using SystemCreateDsp = uint32_t (*)(void*, const void*, void**);
    using DspRelease = uint32_t (*)(void*);
    using ChannelControlAddDsp = uint32_t (*)(uint64_t, int32_t, void*);
    using ChannelControlRemoveDsp = uint32_t (*)(uint64_t, void*);
    using HandleResolver = uint32_t (*)(uint32_t, void**, uint64_t*);
    using HandleUnlock = uint32_t (*)(uint64_t);

    SystemCreateDsp createDsp = nullptr;
    DspRelease releaseDsp = nullptr;
    ChannelControlAddDsp addDsp = nullptr;
    ChannelControlRemoveDsp removeDsp = nullptr;
    HandleResolver resolveHandle = nullptr;
    HandleUnlock unlockHandle = nullptr;

    bool Ready() const
    {
        return createDsp && releaseDsp && addDsp && removeDsp && resolveHandle;
    }
};

bool ResolveFmodFunctions(const PeImage& image, FmodFunctions& functions)
{
    functions.createDsp = reinterpret_cast<FmodFunctions::SystemCreateDsp>(FindByAnchor(
        image,
        "System::createDSP",
        "4C 8B DC 56 48 81 EC 70 01 00 00|40 53 55 56 57 41 56 48 81 EC 50 01 00 00"));
    functions.releaseDsp = reinterpret_cast<FmodFunctions::DspRelease>(FindByAnchor(
        image,
        "DSP::release",
        "48 89 5C 24 10 57 48 81 EC 50 01 00 00"));
    functions.addDsp = reinterpret_cast<FmodFunctions::ChannelControlAddDsp>(FindByAnchor(
        image,
        "ChannelControl::addDSP",
        "4C 8B DC 56 48 81 EC 70 01 00 00|40 53 55 56 57 41 56 48 81 EC 50 01 00 00"));
    functions.removeDsp = reinterpret_cast<FmodFunctions::ChannelControlRemoveDsp>(FindByAnchor(
        image,
        "ChannelControl::removeDSP",
        "48 89 5C 24 18 48 89 74 24 20 57 48 81 EC 50 01 00 00"));
    functions.resolveHandle = reinterpret_cast<FmodFunctions::HandleResolver>(FindByPattern(
        image,
        "48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 20 8B F9 "
        "8B C1 C1 EF 11 49 8B F0 D1 E8 81 E7 FF 0F 00 00 0F B7 E8 4C 8B "
        "F2 4C 8B F9"));
    functions.unlockHandle = reinterpret_cast<FmodFunctions::HandleUnlock>(FindByPattern(
        image,
        "48 8B 89 F0 09 01 00 48 85 C9 0F 85 ?? ?? ?? ?? 33 C0 C3"));

    fh6rb::LogInfo("FMOD DSP sigscan: createDSP=" + Hex(reinterpret_cast<uintptr_t>(functions.createDsp)) +
        " releaseDSP=" + Hex(reinterpret_cast<uintptr_t>(functions.releaseDsp)) +
        " addDSP=" + Hex(reinterpret_cast<uintptr_t>(functions.addDsp)) +
        " removeDSP=" + Hex(reinterpret_cast<uintptr_t>(functions.removeDsp)) +
        " resolver=" + Hex(reinterpret_cast<uintptr_t>(functions.resolveHandle)) +
        " unlock=" + Hex(reinterpret_cast<uintptr_t>(functions.unlockHandle)));

    if (!functions.unlockHandle)
    {
        fh6rb::LogWarn("FMOD DSP sigscan: Handle::unlock was not resolved; refusing to install DSP to avoid FMOD handle leaks.");
        return false;
    }
    return functions.Ready();
}

struct RadioInstance
{
    uint8_t* refCount = nullptr;
    uint8_t* radioStream = nullptr;
    uint8_t* sampleProperties = nullptr;
    std::string soundName;
};

const uint8_t* FindRadioRefCountTypeDescriptorInRange(const uint8_t* begin, const uint8_t* end)
{
    constexpr std::string_view leaf = "RadioStreamFmod";
    constexpr std::string_view outer = "_Ref_count_obj2";
    if (!begin || !end || static_cast<size_t>(end - begin) < leaf.size() + 16)
    {
        return nullptr;
    }

    for (const uint8_t* position = begin; position + leaf.size() < end; ++position)
    {
        if (std::memcmp(position, leaf.data(), leaf.size()) != 0)
        {
            continue;
        }

        const uint8_t* low = position - begin > 128 ? position - 128 : begin;
        bool hasOuter = false;
        for (const uint8_t* scan = low; scan + outer.size() <= position; ++scan)
        {
            if (std::memcmp(scan, outer.data(), outer.size()) == 0)
            {
                hasOuter = true;
                break;
            }
        }
        if (!hasOuter)
        {
            continue;
        }

        for (const uint8_t* scan = position; scan - begin >= 4; --scan)
        {
            if (scan[-2] == '.' && scan[-1] == '?' && scan[0] == 'A' && scan[1] == 'V')
            {
                return scan - 2 - begin >= 16 ? scan - 2 - 16 : nullptr;
            }
        }
    }
    return nullptr;
}

const uint8_t* FindRadioRefCountTypeDescriptor(const PeImage& image)
{
    for (const auto& section : image.sections)
    {
        if (!section.Readable())
        {
            continue;
        }

        if (auto* descriptor = FindRadioRefCountTypeDescriptorInRange(section.start, section.end))
        {
            return descriptor;
        }
    }
    return nullptr;
}

const uint8_t* FindCompleteObjectLocator(const PeImage& image, uint32_t typeDescriptorRva)
{
    for (const auto& section : image.sections)
    {
        if (!section.Readable() || section.end <= section.start + 24)
        {
            continue;
        }

        for (auto* position = section.start; position + 16 <= section.end; position += 4)
        {
            uint32_t signature = 0;
            uint32_t descriptor = 0;
            std::memcpy(&signature, position, sizeof(signature));
            std::memcpy(&descriptor, position + 12, sizeof(descriptor));
            if (signature == 1 && descriptor == typeDescriptorRva)
            {
                return position;
            }
        }
    }
    return nullptr;
}

std::vector<uint8_t*> FindVtableCandidates(uint8_t* begin, uint8_t* end, const uint8_t* completeObjectLocator)
{
    std::vector<uint8_t*> candidates;
    if (end <= begin + sizeof(uintptr_t))
    {
        return candidates;
    }

    for (auto* position = begin; position + sizeof(uintptr_t) <= end; position += sizeof(uintptr_t))
    {
        const uint8_t* value = nullptr;
        std::memcpy(&value, position, sizeof(value));
        if (value == completeObjectLocator)
        {
            candidates.push_back(position + sizeof(uintptr_t));
        }
    }
    return candidates;
}

void ScanHeapForRefCount(const PeImage& image, uint8_t* refCountVtable, std::vector<uint8_t*>& output)
{
    MEMORY_BASIC_INFORMATION mbi{};
    uint8_t* address = nullptr;
    const uint8_t* previousEnd = nullptr;

    while (VirtualQuery(address, &mbi, sizeof(mbi)) == sizeof(mbi) && output.size() < kMaxRadioInstances)
    {
        auto* region = static_cast<uint8_t*>(mbi.BaseAddress);
        auto* regionEnd = region + mbi.RegionSize;
        const DWORD protection = mbi.Protect & 0xff;
        const bool readable = mbi.State == MEM_COMMIT &&
            mbi.Type == MEM_PRIVATE &&
            (protection == PAGE_READWRITE || protection == PAGE_WRITECOPY || protection == PAGE_READONLY) &&
            (mbi.Protect & PAGE_GUARD) == 0;
        const bool overlapsModule = region >= image.base && region < image.base + image.size;

        if (readable && mbi.RegionSize <= 0x4000000 && !overlapsModule)
        {
            auto* aligned = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(region) + 15) & ~uintptr_t{15});
            auto* alignedEnd = reinterpret_cast<uint8_t*>(reinterpret_cast<uintptr_t>(regionEnd) & ~uintptr_t{7});
            SehCall([&] {
                for (auto* position = aligned; position + 24 <= alignedEnd && output.size() < kMaxRadioInstances; position += 16)
                {
                    uint8_t* vtable = nullptr;
                    std::memcpy(&vtable, position, sizeof(vtable));
                    if (vtable != refCountVtable)
                    {
                        continue;
                    }

                    uint32_t useCount = 0;
                    uint32_t weakCount = 0;
                    std::memcpy(&useCount, position + 8, sizeof(useCount));
                    std::memcpy(&weakCount, position + 12, sizeof(weakCount));
                    if (useCount == 0 || weakCount == 0 || useCount > 0x80 || weakCount > 0x80)
                    {
                        continue;
                    }

                    uint8_t* innerVtable = nullptr;
                    std::memcpy(&innerVtable, position + 16, sizeof(innerVtable));
                    if (innerVtable < image.base || innerVtable >= image.base + image.size)
                    {
                        continue;
                    }

                    output.push_back(position);
                }
            });
        }

        if (regionEnd <= previousEnd)
        {
            break;
        }
        previousEnd = regionEnd;
        address = regionEnd;
    }
}

std::string ReadRadioSoundName(uint8_t* radioStream, uint8_t** sampleProperties)
{
    if (sampleProperties)
    {
        *sampleProperties = nullptr;
    }

    uint8_t* wrapper = nullptr;
    if (!SafeRead(radioStream + 0x48, wrapper) || !wrapper)
    {
        return {};
    }

    uint8_t* body = nullptr;
    if (!SafeRead(wrapper + 0x18, body) || !body)
    {
        return {};
    }

    if (sampleProperties)
    {
        *sampleProperties = body;
    }

    std::string value;
    SehCall([&] {
        if (auto text = SafeReadMsvcString(body + 0x10))
        {
            value = *text;
        }
    });
    return value;
}

struct TrackMetadata
{
    std::string title;
    std::string artist;
    std::string playbackStatus;
};

int HexNibble(char value) noexcept
{
    if (value >= '0' && value <= '9')
    {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f')
    {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F')
    {
        return value - 'A' + 10;
    }
    return -1;
}

bool TryReadJsonHex4(std::string_view value, size_t offset, uint32_t& codePoint) noexcept
{
    if (offset + 4 > value.size())
    {
        return false;
    }

    uint32_t parsed = 0;
    for (size_t i = 0; i < 4; ++i)
    {
        const int nibble = HexNibble(value[offset + i]);
        if (nibble < 0)
        {
            return false;
        }
        parsed = (parsed << 4) | static_cast<uint32_t>(nibble);
    }

    codePoint = parsed;
    return true;
}

void AppendUtf8(std::string& output, uint32_t codePoint)
{
    if (codePoint <= 0x7F)
    {
        output.push_back(static_cast<char>(codePoint));
    }
    else if (codePoint <= 0x7FF)
    {
        output.push_back(static_cast<char>(0xC0 | (codePoint >> 6)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else if (codePoint <= 0xFFFF)
    {
        output.push_back(static_cast<char>(0xE0 | (codePoint >> 12)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else if (codePoint <= 0x10FFFF)
    {
        output.push_back(static_cast<char>(0xF0 | (codePoint >> 18)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 12) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | ((codePoint >> 6) & 0x3F)));
        output.push_back(static_cast<char>(0x80 | (codePoint & 0x3F)));
    }
    else
    {
        AppendUtf8(output, 0xFFFD);
    }
}

std::string JsonUnescape(std::string_view value)
{
    std::string output;
    output.reserve(value.size());
    for (size_t index = 0; index < value.size(); ++index)
    {
        if (value[index] != '\\' || index + 1 >= value.size())
        {
            output.push_back(value[index]);
            continue;
        }

        const char escaped = value[++index];
        switch (escaped)
        {
        case '"': output.push_back('"'); break;
        case '\\': output.push_back('\\'); break;
        case '/': output.push_back('/'); break;
        case 'b': output.push_back('\b'); break;
        case 'f': output.push_back('\f'); break;
        case 'n': output.push_back('\n'); break;
        case 'r': output.push_back('\r'); break;
        case 't': output.push_back('\t'); break;
        case 'u':
        {
            uint32_t codePoint = 0;
            if (!TryReadJsonHex4(value, index + 1, codePoint))
            {
                output += "\\u";
                break;
            }

            index += 4;

            if (codePoint >= 0xD800 && codePoint <= 0xDBFF)
            {
                const size_t lowEscape = index + 1;
                uint32_t low = 0;
                if (lowEscape + 5 < value.size() &&
                    value[lowEscape] == '\\' &&
                    value[lowEscape + 1] == 'u' &&
                    TryReadJsonHex4(value, lowEscape + 2, low) &&
                    low >= 0xDC00 && low <= 0xDFFF)
                {
                    codePoint = 0x10000 + ((codePoint - 0xD800) << 10) + (low - 0xDC00);
                    index += 6;
                }
                else
                {
                    codePoint = 0xFFFD;
                }
            }
            else if (codePoint >= 0xDC00 && codePoint <= 0xDFFF)
            {
                codePoint = 0xFFFD;
            }

            AppendUtf8(output, codePoint);
            break;
        }
        default:
            output.push_back(escaped);
            break;
        }
    }
    return output;
}

std::string ExtractJsonString(const std::string& json, std::string_view key)
{
    const std::string needle = "\"" + std::string(key) + "\"";
    auto position = json.find(needle);
    if (position == std::string::npos)
    {
        return {};
    }

    position = json.find(':', position + needle.size());
    if (position == std::string::npos)
    {
        return {};
    }

    position = json.find('"', position + 1);
    if (position == std::string::npos)
    {
        return {};
    }

    ++position;
    std::string raw;
    bool escaped = false;
    for (; position < json.size(); ++position)
    {
        const char value = json[position];
        if (escaped)
        {
            raw.push_back('\\');
            raw.push_back(value);
            escaped = false;
            continue;
        }
        if (value == '\\')
        {
            escaped = true;
            continue;
        }
        if (value == '"')
        {
            break;
        }
        raw.push_back(value);
    }

    return JsonUnescape(raw);
}

std::optional<std::string> HttpGetLocalMetadataJson()
{
    HINTERNET session = WinHttpOpen(L"FH6RadioBridge/1.0", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session)
    {
        return std::nullopt;
    }

    WinHttpSetTimeouts(session, 500, 500, 500, 500);
    HINTERNET connect = WinHttpConnect(session, L"127.0.0.1", 8420, 0);
    if (!connect)
    {
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    HINTERNET request = WinHttpOpenRequest(connect, L"GET", L"/api/metadata", nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, 0);
    if (!request)
    {
        WinHttpCloseHandle(connect);
        WinHttpCloseHandle(session);
        return std::nullopt;
    }

    std::string body;
    const BOOL sent = WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0);
    const BOOL received = sent ? WinHttpReceiveResponse(request, nullptr) : FALSE;
    if (received)
    {
        DWORD available = 0;
        while (WinHttpQueryDataAvailable(request, &available) && available > 0 && body.size() < 64 * 1024)
        {
            std::string chunk(available, '\0');
            DWORD read = 0;
            if (!WinHttpReadData(request, chunk.data(), available, &read) || read == 0)
            {
                break;
            }
            chunk.resize(read);
            body += chunk;
        }
    }

    WinHttpCloseHandle(request);
    WinHttpCloseHandle(connect);
    WinHttpCloseHandle(session);

    if (body.empty())
    {
        return std::nullopt;
    }
    return body;
}

std::optional<TrackMetadata> ReadBridgeMetadata()
{
    const auto json = HttpGetLocalMetadataJson();
    if (!json)
    {
        return std::nullopt;
    }

    TrackMetadata metadata{
        ExtractJsonString(*json, "title"),
        ExtractJsonString(*json, "artist"),
        ExtractJsonString(*json, "playbackStatus")
    };
    if (metadata.title.empty())
    {
        metadata.title = "FH6 Radio Bridge";
    }
    if (metadata.artist.empty())
    {
        metadata.artist = "External Audio";
    }
    return metadata;
}

struct MetadataRuntime
{
    uint8_t* sampleProperties = nullptr;
    std::string title;
    std::string artist;

    void SetTarget(uint8_t* target)
    {
        if (sampleProperties == target)
        {
            return;
        }
        sampleProperties = target;
        title.clear();
        artist.clear();
        fh6rb::LogInfo("FMOD metadata: target SampleProperties=" + Hex(reinterpret_cast<uintptr_t>(sampleProperties)));
    }

    bool Update(const TrackMetadata& metadata)
    {
        if (!sampleProperties)
        {
            return false;
        }

        bool ok = true;
        if (metadata.title != title)
        {
            ok = WriteMsvcString(sampleProperties + 0x30, metadata.title) && ok;
            if (ok)
            {
                title = metadata.title;
            }
        }
        if (metadata.artist != artist)
        {
            ok = WriteMsvcString(sampleProperties + 0x50, metadata.artist) && ok;
            if (ok)
            {
                artist = metadata.artist;
            }
        }
        if (ok)
        {
            fh6rb::LogInfo("FMOD metadata: wrote \"" + title + "\" / \"" + artist + "\" status=" + metadata.playbackStatus);
        }
        else
        {
            fh6rb::LogWarn("FMOD metadata: write failed.");
        }
        return ok;
    }
};

struct DiscoveryCache
{
    bool located = false;
    uint8_t* refCountVtable = nullptr;
    std::vector<uint8_t*> refCounts;
};

std::mutex g_discoveryMutex;
DiscoveryCache g_discoveryCache;

std::vector<RadioInstance> DiscoverRadioInstances(const PeImage& image)
{
    DiscoveryCache cache;
    {
        std::scoped_lock lock(g_discoveryMutex);
        cache = g_discoveryCache;
    }

    if (!cache.located)
    {
        const auto* typeDescriptor = FindRadioRefCountTypeDescriptor(image);
        if (!typeDescriptor)
        {
            fh6rb::LogWarn("FMOD DSP discovery: _Ref_count_obj2<RadioStreamFmod> typedesc not found.");
            return {};
        }

        const auto typeDescriptorRva = static_cast<uint32_t>(typeDescriptor - image.base);
        const auto* completeObjectLocator = FindCompleteObjectLocator(image, typeDescriptorRva);
        if (!completeObjectLocator)
        {
            fh6rb::LogWarn("FMOD DSP discovery: typedesc found but COL not found.");
            return {};
        }

        fh6rb::LogInfo("FMOD DSP discovery: typedescRva=" + Hex(typeDescriptorRva) +
            " colRva=" + Hex(static_cast<uintptr_t>(completeObjectLocator - image.base)));

        std::vector<uint8_t*> refCounts;
        uint8_t* refCountVtable = nullptr;
        for (int pass = 0; pass < 2 && refCounts.empty(); ++pass)
        {
            uint8_t* begin = pass == 0 ? image.rdata : image.base;
            uint8_t* end = pass == 0 ? image.rdataEnd : image.base + image.size;
            for (auto* candidate : FindVtableCandidates(begin, end, completeObjectLocator))
            {
                uint8_t* firstMethod = nullptr;
                std::memcpy(&firstMethod, candidate, sizeof(firstMethod));
                if (firstMethod < image.base || firstMethod >= image.base + image.size)
                {
                    continue;
                }

                ScanHeapForRefCount(image, candidate, refCounts);
                if (!refCounts.empty())
                {
                    refCountVtable = candidate;
                    break;
                }
            }
        }

        if (refCounts.empty())
        {
            fh6rb::LogInfo("FMOD DSP discovery: no RadioStreamFmod refcount heap candidates yet.");
            return {};
        }

        cache.located = true;
        cache.refCountVtable = refCountVtable;
        cache.refCounts = std::move(refCounts);
        {
            std::scoped_lock lock(g_discoveryMutex);
            g_discoveryCache = cache;
        }

        fh6rb::LogInfo("FMOD DSP discovery: cached refCountVtableRva=" +
            Hex(static_cast<uintptr_t>(cache.refCountVtable - image.base)) +
            " candidates=" + std::to_string(cache.refCounts.size()));
    }

    std::vector<RadioInstance> instances;
    for (auto* refCount : cache.refCounts)
    {
        auto* radioStream = refCount + 16;
        uint8_t* sampleProperties = nullptr;
        auto soundName = ReadRadioSoundName(radioStream, &sampleProperties);
        if (soundName.empty())
        {
            continue;
        }

        instances.push_back({refCount, radioStream, sampleProperties, std::move(soundName)});
    }

    if (instances.empty())
    {
        fh6rb::LogInfo("FMOD DSP discovery: cached candidates are not chain-valid yet.");
    }
    else
    {
        fh6rb::LogInfo("FMOD DSP discovery: chain-valid instances=" + std::to_string(instances.size()));
        for (const auto& instance : instances)
        {
            fh6rb::LogInfo("FMOD DSP discovery: RadioStreamFmod=" +
                Hex(reinterpret_cast<uintptr_t>(instance.radioStream)) +
                " SoundName=\"" + instance.soundName + "\"");
        }
    }

    return instances;
}

void* ResolveFmodSystem(const PeImage& image, uint8_t* radioStream)
{
    void* result = nullptr;
    SehCall([&] {
        uint8_t* soundWrapper = nullptr;
        if (!SafeRead(radioStream + 0x08, soundWrapper) || !soundWrapper)
        {
            return;
        }

        uint8_t* system = nullptr;
        if (!SafeRead(soundWrapper + 0xC0, system) || !system)
        {
            return;
        }

        uint8_t* vtable = nullptr;
        if (!SafeRead(system, vtable) || vtable < image.base || vtable >= image.base + image.size)
        {
            return;
        }

        result = system;
    });
    return result;
}

#pragma pack(push, 1)
struct FmodDspDescription
{
    uint32_t pluginSdkVersion;
    char name[32];
    uint32_t version;
    int32_t numInputBuffers;
    int32_t numOutputBuffers;
    void* create;
    void* release;
    void* reset;
    void* read;
    void* process;
    void* setPosition;
    int32_t numParameters;
    uint32_t padding;
    void* parameterDescription;
    void* setParameterFloat;
    void* setParameterInt;
    void* setParameterBool;
    void* setParameterData;
    void* getParameterFloat;
    void* getParameterInt;
    void* getParameterBool;
    void* getParameterData;
    void* shouldIProcess;
    void* userData;
    void* systemRegister;
    void* systemDeregister;
    void* systemMix;
};
#pragma pack(pop)

static_assert(sizeof(FmodDspDescription) == 216);

uint32_t __stdcall RadioBridgeDspReadCallback(void*, float* input, float* output, uint32_t length, int32_t inputChannels, int32_t* outputChannels)
{
    auto* reader = g_reader.load();
    if (!output || length == 0)
    {
        return 0;
    }

    int32_t channels = inputChannels > 0 ? inputChannels : 2;
    if (outputChannels && *outputChannels > 0)
    {
        channels = *outputChannels;
    }
    if (outputChannels)
    {
        *outputChannels = channels;
    }
    if (channels <= 0)
    {
        channels = 2;
    }

    const size_t total = static_cast<size_t>(length) * static_cast<size_t>(channels);
    const float gain = g_gain.load(std::memory_order_acquire);
    bool ok = false;

    if (reader && gain > 0.0f)
    {
        if (channels == 2)
        {
            ok = reader->ReadFrames(output, length);
            if (gain != 1.0f)
            {
                for (size_t index = 0; index < total; ++index)
                {
                    output[index] *= gain;
                }
            }
        }
        else
        {
            thread_local std::vector<float> stereo;
            stereo.resize(static_cast<size_t>(length) * 2);
            ok = reader->ReadFrames(stereo.data(), length);
            for (uint32_t frame = 0; frame < length; ++frame)
            {
                const float left = stereo[static_cast<size_t>(frame) * 2] * gain;
                const float right = stereo[static_cast<size_t>(frame) * 2 + 1] * gain;
                auto* destination = output + static_cast<size_t>(frame) * static_cast<size_t>(channels);
                if (channels == 1)
                {
                    destination[0] = (left + right) * 0.5f;
                }
                else
                {
                    destination[0] = left;
                    destination[1] = right;
                    const float downmix = (left + right) * 0.5f;
                    for (int32_t channel = 2; channel < channels; ++channel)
                    {
                        destination[channel] = downmix;
                    }
                }
            }
        }
    }

    if (!ok)
    {
        std::memset(output, 0, total * sizeof(float));
        g_dspUnderruns.fetch_add(1, std::memory_order_relaxed);
    }

    (void)input;
    g_dspCalls.fetch_add(1, std::memory_order_relaxed);
    return 0;
}

struct DspRuntime
{
    FmodFunctions functions;
    void* fmodSystem = nullptr;
    void* dsp = nullptr;
    uint32_t currentHandle = 0;

    bool ValidateHandle(uint32_t handle)
    {
        if (handle == 0 || !functions.resolveHandle || !functions.unlockHandle)
        {
            return false;
        }

        void* instance = nullptr;
        uint64_t lockState = 0;
        uint32_t result = ~0u;
        if (!SehCall([&] { result = functions.resolveHandle(handle, &instance, &lockState); }))
        {
            fh6rb::LogWarn("FMOD DSP: handle resolver raised SEH exception.");
            return false;
        }

        if (lockState != 0)
        {
            SehCall([&] { functions.unlockHandle(lockState); });
        }

        if (result != 0 || !instance)
        {
            fh6rb::LogWarn("FMOD DSP: handle resolver failed result=" + std::to_string(result) +
                " handle=" + Hex(handle));
            return false;
        }

        return true;
    }

    void ReleaseCurrent()
    {
        if (!dsp)
        {
            return;
        }

        if (currentHandle != 0)
        {
            SehCall([&] { functions.removeDsp(static_cast<uint64_t>(currentHandle), dsp); });
        }
        SehCall([&] { functions.releaseDsp(dsp); });
        fh6rb::LogInfo("FMOD DSP: released previous dsp handle=" + Hex(currentHandle));
        dsp = nullptr;
        currentHandle = 0;
    }

    bool Install(uint32_t handle)
    {
        if (!fmodSystem || handle == 0)
        {
            return false;
        }

        constexpr std::array<uint32_t, 3> sdkVersions{0x00011000u, 0x00011003u, 0x00010000u};
        FmodDspDescription description{};
        std::memcpy(description.name, "FH6 Radio Bridge", 16);
        description.version = 1;
        description.numInputBuffers = 1;
        description.numOutputBuffers = 1;
        description.read = reinterpret_cast<void*>(&RadioBridgeDspReadCallback);
        description.userData = this;

        void* createdDsp = nullptr;
        uint32_t result = ~0u;
        for (const auto sdkVersion : sdkVersions)
        {
            description.pluginSdkVersion = sdkVersion;
            createdDsp = nullptr;
            if (!SehCall([&] { result = functions.createDsp(fmodSystem, &description, &createdDsp); }))
            {
                fh6rb::LogWarn("FMOD DSP: createDSP raised SEH sdk=" + Hex(sdkVersion));
                continue;
            }
            if (result == 0 && createdDsp)
            {
                break;
            }
        }

        if (!createdDsp)
        {
            fh6rb::LogWarn("FMOD DSP: createDSP failed result=" + std::to_string(result));
            return false;
        }

        if (!SehCall([&] { result = functions.addDsp(static_cast<uint64_t>(handle), 0, createdDsp); }) || result != 0)
        {
            fh6rb::LogWarn("FMOD DSP: addDSP failed result=" + std::to_string(result) +
                " handle=" + Hex(handle));
            SehCall([&] { functions.releaseDsp(createdDsp); });
            return false;
        }

        dsp = createdDsp;
        currentHandle = handle;
        fh6rb::LogInfo("FMOD DSP: installed dsp=" + Hex(reinterpret_cast<uintptr_t>(dsp)) +
            " on radio handle=" + Hex(handle));
        return true;
    }
};

bool ReadActiveHandle(uint8_t* radioStream, uint32_t& handle)
{
    handle = 0;
    return radioStream && SafeRead(radioStream + 0x20, handle) && handle != 0;
}

DWORD WINAPI InjectorThread(LPVOID parameter)
{
    auto* baseDirectory = static_cast<std::wstring*>(parameter);
    std::unique_ptr<std::wstring> baseDirectoryOwner(baseDirectory);

    fh6rb::LogInfo("FMOD DSP injector: worker started; using DSP replacement path.");
    std::this_thread::sleep_for(std::chrono::seconds(5));

    const auto image = ParseImage(reinterpret_cast<uint8_t*>(GetModuleHandleW(nullptr)));
    if (!image.Valid())
    {
        fh6rb::LogWarn("FMOD DSP injector: failed to parse Forza PE image.");
        return 0;
    }
    if (std::find(kSupportedForzaExeSizes.begin(), kSupportedForzaExeSizes.end(), image.size) ==
        kSupportedForzaExeSizes.end())
    {
        std::ostringstream sizes;
        for (size_t i = 0; i < kSupportedForzaExeSizes.size(); ++i)
        {
            if (i) sizes << ",";
            sizes << kSupportedForzaExeSizes[i];
        }
        fh6rb::LogWarn("FMOD DSP injector: unsupported Forza image size=" + std::to_string(image.size) +
            " supportedSizes=[" + sizes.str() + "]");
        return 0;
    }

    DspRuntime runtime;
    if (!ResolveFmodFunctions(image, runtime.functions))
    {
        fh6rb::LogWarn("FMOD DSP injector: FMOD signatures unresolved; injector is dormant.");
        return 0;
    }

    fh6rb::SharedAudioRingReader reader;
    if (!reader.TryConnect())
    {
        fh6rb::LogWarn("FMOD DSP injector: shared audio is unavailable now; callback will output silence until bridge connects.");
    }
    g_reader.store(&reader, std::memory_order_release);

    RadioInstance target{};
    for (int attempt = 1; attempt <= 120 && fh6rb::IsFmodInjectEnabled(*baseDirectoryOwner); ++attempt)
    {
        const auto instances = DiscoverRadioInstances(image);
        if (instances.empty())
        {
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        auto selected = std::find_if(instances.begin(), instances.end(), [](const RadioInstance& instance) {
            return instance.soundName == kTargetSoundName;
        });
        if (selected == instances.end())
        {
            selected = instances.begin();
            fh6rb::LogWarn("FMOD DSP injector: target SoundName was not found; falling back to \"" + selected->soundName + "\".");
        }

        target = *selected;
        runtime.fmodSystem = ResolveFmodSystem(image, target.radioStream);
        if (!runtime.fmodSystem)
        {
            fh6rb::LogWarn("FMOD DSP injector: FMOD SystemI resolution failed; retrying.");
            std::this_thread::sleep_for(std::chrono::seconds(5));
            continue;
        }

        fh6rb::LogInfo("FMOD DSP injector: targeting RadioStreamFmod=" +
            Hex(reinterpret_cast<uintptr_t>(target.radioStream)) +
            " SoundName=\"" + target.soundName + "\" SystemI=" +
            Hex(reinterpret_cast<uintptr_t>(runtime.fmodSystem)));
        break;
    }

    if (!target.radioStream || !runtime.fmodSystem)
    {
        fh6rb::LogWarn("FMOD DSP injector: discovery timed out.");
        return 0;
    }

    MetadataRuntime metadataRuntime;
    metadataRuntime.SetTarget(target.sampleProperties);

    auto nextStatusAt = std::chrono::steady_clock::now();
    auto nextMetadataAt = std::chrono::steady_clock::now();
    while (fh6rb::IsFmodInjectEnabled(*baseDirectoryOwner))
    {
        reader.TryConnect();

        uint32_t handle = 0;
        if (ReadActiveHandle(target.radioStream, handle) && handle != runtime.currentHandle)
        {
            if (runtime.ValidateHandle(handle))
            {
                runtime.ReleaseCurrent();
                runtime.Install(handle);
            }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= nextMetadataAt)
        {
            if (const auto metadata = ReadBridgeMetadata())
            {
                metadataRuntime.Update(*metadata);
            }
            nextMetadataAt = now + std::chrono::milliseconds(750);
        }

        if (now >= nextStatusAt)
        {
            const auto status = reader.GetStatus();
            fh6rb::LogInfo("FMOD DSP status: handle=" + Hex(runtime.currentHandle) +
                " dsp=" + Hex(reinterpret_cast<uintptr_t>(runtime.dsp)) +
                " dspCalls=" + std::to_string(g_dspCalls.load(std::memory_order_relaxed)) +
                " dspUnderruns=" + std::to_string(g_dspUnderruns.load(std::memory_order_relaxed)) +
                " audio=" + fh6rb::FormatSharedAudioStatus(status));
            nextStatusAt = now + std::chrono::seconds(5);
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }

    runtime.ReleaseCurrent();
    g_reader.store(nullptr, std::memory_order_release);
    fh6rb::LogInfo("FMOD DSP injector: worker exiting.");
    return 0;
}
}

namespace fh6rb
{
std::wstring FmodInjectFlagPath(const std::wstring& baseDirectory)
{
    return (std::filesystem::path(baseDirectory) / L"fh6-radio-bridge" / L"enable_fmod_inject.flag").wstring();
}

bool IsFmodInjectEnabled(const std::wstring& baseDirectory)
{
    try
    {
        return std::filesystem::exists(FmodInjectFlagPath(baseDirectory));
    }
    catch (...)
    {
        return false;
    }
}

bool StartFmodRadioInjector(const std::wstring& baseDirectory)
{
    auto* copy = new std::wstring(baseDirectory);
    HANDLE thread = CreateThread(nullptr, 0, InjectorThread, copy, 0, nullptr);
    if (!thread)
    {
        delete copy;
        return false;
    }

    CloseHandle(thread);
    return true;
}
}
