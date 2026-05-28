#pragma once

#include <cstdint>
#include <string>

namespace fh6rb
{
constexpr uint32_t SharedAudioSampleRate = 48000;
constexpr uint32_t SharedAudioChannels = 2;
constexpr uint32_t SharedAudioBufferFrames = SharedAudioSampleRate * 5;
constexpr uint32_t SharedAudioHeaderSize = 84;
constexpr uint32_t SharedAudioFrameBytes = SharedAudioChannels * sizeof(float);
constexpr uint32_t SharedAudioBufferBytes = SharedAudioBufferFrames * SharedAudioFrameBytes;
constexpr uint32_t SharedAudioTotalBytes = SharedAudioHeaderSize + SharedAudioBufferBytes;

struct SharedAudioStatus
{
    bool connected = false;
    bool headerValid = false;
    std::wstring mappingName;
    uint32_t flags = 0;
    uint64_t writeFrame = 0;
    uint64_t lastWriteQpc = 0;
    uint64_t underrunCount = 0;
    float peakL = 0.0f;
    float peakR = 0.0f;
    float rmsL = 0.0f;
    float rmsR = 0.0f;
    int64_t writerAgeMs = -1;
    uint64_t localReadFrame = 0;
    uint64_t localUnderrunCount = 0;
};

class SharedAudioRingReader
{
public:
    SharedAudioRingReader() = default;
    ~SharedAudioRingReader();

    SharedAudioRingReader(const SharedAudioRingReader&) = delete;
    SharedAudioRingReader& operator=(const SharedAudioRingReader&) = delete;

    bool TryConnect();
    void Disconnect();

    // Realtime-safe after TryConnect succeeds: no allocation, no file IO, no waits.
    bool ReadFrames(float* outputInterleavedStereo, uint32_t frameCount);

    SharedAudioStatus GetStatus() const;

private:
    void* mapping_ = nullptr;
    const void* view_ = nullptr;
    std::wstring mappingName_;
    uint64_t readFrame_ = 0;
    uint64_t localUnderrunCount_ = 0;
};

SharedAudioStatus ProbeSharedAudio();
std::string FormatSharedAudioStatus(const SharedAudioStatus& status);
}
