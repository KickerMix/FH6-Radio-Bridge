#include "shared_audio_reader.h"

#include <Windows.h>

#include <array>
#include <cstring>
#include <sstream>

namespace
{
constexpr uint32_t kMagic = 0x52484659;
constexpr uint32_t kVersion = 1;
constexpr uint32_t kHeaderSize = 84;
constexpr uint32_t kSampleRate = 48000;
constexpr uint32_t kChannels = 2;
constexpr uint32_t kFormatFloat32Interleaved = 1;
constexpr uint32_t kBufferFrames = 48000 * 5;
constexpr uint32_t kFrameBytes = kChannels * sizeof(float);
constexpr uint32_t kPcmDataOffset = kHeaderSize;
constexpr uint64_t kTargetLatencyFrames = kSampleRate / 5;
constexpr int64_t kWriterStaleMs = 2000;

#pragma pack(push, 1)
struct AudioSharedHeader
{
    uint32_t magic;
    uint32_t version;
    uint32_t headerSize;
    uint32_t sampleRate;

    uint32_t channels;
    uint32_t format;
    uint32_t bufferFrames;
    uint32_t flags;

    uint64_t writeFrame;
    uint64_t readFrame;
    uint64_t lastWriteQpc;
    uint64_t underrunCount;

    float volume;
    float peakL;
    float peakR;
    float rmsL;
    float rmsR;
};
#pragma pack(pop)

static_assert(sizeof(AudioSharedHeader) == kHeaderSize);

bool IsHeaderValid(const AudioSharedHeader& header)
{
    return header.magic == kMagic &&
        header.version == kVersion &&
        header.headerSize == kHeaderSize &&
        header.sampleRate == kSampleRate &&
        header.channels == kChannels &&
        header.format == kFormatFloat32Interleaved &&
        header.bufferFrames == kBufferFrames;
}

int64_t WriterAgeMs(uint64_t lastWriteQpc)
{
    if (lastWriteQpc == 0)
    {
        return -1;
    }

    LARGE_INTEGER now{};
    LARGE_INTEGER frequency{};
    if (!QueryPerformanceCounter(&now) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
    {
        return -1;
    }

    const auto delta = now.QuadPart - static_cast<int64_t>(lastWriteQpc);
    if (delta < 0)
    {
        return 0;
    }

    return (delta * 1000) / frequency.QuadPart;
}

void Silence(float* outputInterleavedStereo, uint32_t frameCount)
{
    if (!outputInterleavedStereo || frameCount == 0)
    {
        return;
    }

    std::memset(outputInterleavedStereo, 0, static_cast<size_t>(frameCount) * kFrameBytes);
}

bool ReadHeader(const void* view, AudioSharedHeader& header)
{
    if (!view)
    {
        return false;
    }

    std::memcpy(&header, view, sizeof(header));
    return true;
}
}

namespace fh6rb
{
SharedAudioRingReader::~SharedAudioRingReader()
{
    Disconnect();
}

bool SharedAudioRingReader::TryConnect()
{
    if (view_)
    {
        return true;
    }

    const std::array<const wchar_t*, 2> names{
        L"Local\\FH6RadioBridgeAudio",
        L"Global\\FH6RadioBridgeAudio"
    };

    for (const auto* name : names)
    {
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!mapping)
        {
            continue;
        }

        const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, SharedAudioTotalBytes);
        if (!view)
        {
            CloseHandle(mapping);
            continue;
        }

        AudioSharedHeader header{};
        if (!ReadHeader(view, header) || !IsHeaderValid(header))
        {
            UnmapViewOfFile(view);
            CloseHandle(mapping);
            continue;
        }

        mapping_ = mapping;
        view_ = view;
        mappingName_ = name;
        readFrame_ = 0;
        return true;
    }

    return false;
}

void SharedAudioRingReader::Disconnect()
{
    if (view_)
    {
        UnmapViewOfFile(view_);
        view_ = nullptr;
    }

    if (mapping_)
    {
        CloseHandle(mapping_);
        mapping_ = nullptr;
    }

    mappingName_.clear();
    readFrame_ = 0;
}

bool SharedAudioRingReader::ReadFrames(float* outputInterleavedStereo, uint32_t frameCount)
{
    if (!outputInterleavedStereo || frameCount == 0)
    {
        return false;
    }

    AudioSharedHeader header{};
    if (!ReadHeader(view_, header) || !IsHeaderValid(header))
    {
        Silence(outputInterleavedStereo, frameCount);
        ++localUnderrunCount_;
        return false;
    }

    const auto writerAgeMs = WriterAgeMs(header.lastWriteQpc);
    if (writerAgeMs < 0 || writerAgeMs > kWriterStaleMs)
    {
        Silence(outputInterleavedStereo, frameCount);
        ++localUnderrunCount_;
        return false;
    }

    const uint64_t writeFrame = header.writeFrame;
    if (readFrame_ == 0 || readFrame_ > writeFrame)
    {
        readFrame_ = writeFrame > kTargetLatencyFrames ? writeFrame - kTargetLatencyFrames : writeFrame;
    }

    if (writeFrame >= readFrame_ &&
        writeFrame - readFrame_ > kBufferFrames - kTargetLatencyFrames)
    {
        readFrame_ = writeFrame > kTargetLatencyFrames ? writeFrame - kTargetLatencyFrames : writeFrame;
    }

    const uint64_t availableFrames = writeFrame >= readFrame_ ? writeFrame - readFrame_ : 0;
    if (availableFrames == 0)
    {
        Silence(outputInterleavedStereo, frameCount);
        ++localUnderrunCount_;
        return false;
    }

    const uint32_t framesToRead = static_cast<uint32_t>(availableFrames < frameCount ? availableFrames : frameCount);
    auto* destination = outputInterleavedStereo;
    const auto* pcmBase = static_cast<const char*>(view_) + kPcmDataOffset;
    uint32_t remainingFrames = framesToRead;
    uint64_t currentReadFrame = readFrame_;

    while (remainingFrames > 0)
    {
        const uint64_t ringFrame = currentReadFrame % kBufferFrames;
        const uint32_t framesUntilWrap = static_cast<uint32_t>(kBufferFrames - ringFrame);
        const uint32_t framesThisRead = remainingFrames < framesUntilWrap ? remainingFrames : framesUntilWrap;
        const auto* source = pcmBase + (ringFrame * kFrameBytes);
        const size_t bytesThisRead = static_cast<size_t>(framesThisRead) * kFrameBytes;

        std::memcpy(destination, source, bytesThisRead);

        destination += static_cast<size_t>(framesThisRead) * kChannels;
        currentReadFrame += framesThisRead;
        remainingFrames -= framesThisRead;
    }

    if (framesToRead < frameCount)
    {
        Silence(destination, frameCount - framesToRead);
        ++localUnderrunCount_;
    }

    readFrame_ += framesToRead;
    return framesToRead == frameCount;
}

SharedAudioStatus SharedAudioRingReader::GetStatus() const
{
    SharedAudioStatus status{};
    status.connected = view_ != nullptr;
    status.mappingName = mappingName_;
    status.localReadFrame = readFrame_;
    status.localUnderrunCount = localUnderrunCount_;

    AudioSharedHeader header{};
    if (ReadHeader(view_, header))
    {
        status.headerValid = IsHeaderValid(header);
        status.flags = header.flags;
        status.writeFrame = header.writeFrame;
        status.lastWriteQpc = header.lastWriteQpc;
        status.underrunCount = header.underrunCount;
        status.peakL = header.peakL;
        status.peakR = header.peakR;
        status.rmsL = header.rmsL;
        status.rmsR = header.rmsR;
        status.writerAgeMs = WriterAgeMs(header.lastWriteQpc);
    }

    return status;
}

SharedAudioStatus ProbeSharedAudio()
{
    SharedAudioStatus status{};
    const std::array<const wchar_t*, 2> names{
        L"Local\\FH6RadioBridgeAudio",
        L"Global\\FH6RadioBridgeAudio"
    };

    for (const auto* name : names)
    {
        HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name);
        if (!mapping)
        {
            continue;
        }

        status.connected = true;
        status.mappingName = name;

        auto* view = static_cast<const AudioSharedHeader*>(MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(AudioSharedHeader)));
        if (view)
        {
            const AudioSharedHeader header = *view;
            status.headerValid = IsHeaderValid(header);
            status.flags = header.flags;
            status.writeFrame = header.writeFrame;
            status.lastWriteQpc = header.lastWriteQpc;
            status.underrunCount = header.underrunCount;
            status.peakL = header.peakL;
            status.peakR = header.peakR;
            status.rmsL = header.rmsL;
            status.rmsR = header.rmsR;
            status.writerAgeMs = WriterAgeMs(header.lastWriteQpc);
            UnmapViewOfFile(view);
        }

        CloseHandle(mapping);
        return status;
    }

    return status;
}

std::string FormatSharedAudioStatus(const SharedAudioStatus& status)
{
    std::ostringstream stream;
    stream << "connected=" << (status.connected ? "true" : "false")
           << " headerValid=" << (status.headerValid ? "true" : "false")
           << " writeFrame=" << status.writeFrame
           << " localReadFrame=" << status.localReadFrame
           << " flags=" << status.flags
           << " writerAgeMs=" << status.writerAgeMs
           << " underrunCount=" << status.underrunCount
           << " localUnderruns=" << status.localUnderrunCount
           << " peak=(" << status.peakL << "," << status.peakR << ")"
           << " rms=(" << status.rmsL << "," << status.rmsR << ")";
    return stream.str();
}
}
