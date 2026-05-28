#include "xaudio_shared_player.h"

#include "logger.h"
#include "shared_audio_reader.h"

#include <Windows.h>
#include <xaudio2.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace
{
constexpr uint32_t kFramesPerBuffer = 960;
constexpr size_t kBufferCount = 8;
constexpr uint32_t kTargetQueuedBuffers = 4;
constexpr auto kControlPollInterval = std::chrono::milliseconds(250);

std::string HResultString(HRESULT hr)
{
    std::ostringstream stream;
    stream << "0x" << std::hex << static_cast<unsigned long>(hr);
    return stream.str();
}

class ComApartment
{
public:
    ComApartment()
        : hr_(CoInitializeEx(nullptr, COINIT_MULTITHREADED)),
          initialized_(SUCCEEDED(hr_))
    {
    }

    ~ComApartment()
    {
        if (initialized_)
        {
            CoUninitialize();
        }
    }

    HRESULT Result() const
    {
        return hr_;
    }

    bool IsInitialized() const
    {
        return initialized_;
    }

private:
    HRESULT hr_ = E_FAIL;
    bool initialized_ = false;
};

void FillSilence(std::vector<float>& buffer)
{
    std::memset(buffer.data(), 0, buffer.size() * sizeof(float));
}

WAVEFORMATEX SharedAudioWaveFormat()
{
    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = fh6rb::SharedAudioChannels;
    format.nSamplesPerSec = fh6rb::SharedAudioSampleRate;
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(format.nChannels * (format.wBitsPerSample / 8));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;
    return format;
}

bool IsCurrentProcessForeground()
{
    HWND foreground = GetForegroundWindow();
    if (!foreground)
    {
        return false;
    }

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    return processId == GetCurrentProcessId();
}
}

namespace fh6rb
{
std::wstring InProcessAudioFlagPath(const std::wstring& baseDirectory)
{
    return (std::filesystem::path(baseDirectory) / L"fh6-radio-bridge" / L"enable_inprocess_audio.flag").wstring();
}

std::wstring RadioActiveFlagPath(const std::wstring& baseDirectory)
{
    return (std::filesystem::path(baseDirectory) / L"fh6-radio-bridge" / L"radio_active.flag").wstring();
}

bool IsInProcessAudioEnabled(const std::wstring& baseDirectory)
{
    try
    {
        return std::filesystem::exists(InProcessAudioFlagPath(baseDirectory));
    }
    catch (...)
    {
        return false;
    }
}

bool IsRadioAudioActive(const std::wstring& baseDirectory)
{
    try
    {
        return std::filesystem::exists(RadioActiveFlagPath(baseDirectory));
    }
    catch (...)
    {
        return false;
    }
}

bool SetRadioAudioActive(const std::wstring& baseDirectory, bool active)
{
    try
    {
        const auto flagPath = std::filesystem::path(RadioActiveFlagPath(baseDirectory));
        if (active)
        {
            std::filesystem::create_directories(flagPath.parent_path());
            std::ofstream stream(flagPath, std::ios::trunc);
            stream << "active\n";
            return stream.good();
        }

        std::error_code error;
        std::filesystem::remove(flagPath, error);
        return !error;
    }
    catch (...)
    {
        return false;
    }
}

XAudioSharedPlayer::~XAudioSharedPlayer()
{
    Stop();
}

bool XAudioSharedPlayer::Start(const std::wstring& baseDirectory)
{
    if (worker_.joinable())
    {
        return true;
    }

    stopRequested_ = false;

    try
    {
        worker_ = std::thread(&XAudioSharedPlayer::Worker, this, baseDirectory);
        return true;
    }
    catch (...)
    {
        return false;
    }
}

void XAudioSharedPlayer::Stop()
{
    stopRequested_ = true;
    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id())
    {
        worker_.join();
    }
}

void XAudioSharedPlayer::Worker(std::wstring baseDirectory)
{
    LogInfo("XAudio2 shared audio worker starting.");

    ComApartment com;
    if (!com.IsInitialized())
    {
        LogError("CoInitializeEx failed before XAudio2 startup: " + HResultString(com.Result()));
        return;
    }

    IXAudio2* xaudio = nullptr;
    HRESULT hr = XAudio2Create(&xaudio, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr) || !xaudio)
    {
        LogError("XAudio2Create failed: " + HResultString(hr));
        return;
    }

    IXAudio2MasteringVoice* masteringVoice = nullptr;
    hr = xaudio->CreateMasteringVoice(&masteringVoice);
    if (FAILED(hr) || !masteringVoice)
    {
        LogError("CreateMasteringVoice failed: " + HResultString(hr));
        xaudio->Release();
        return;
    }

    const auto format = SharedAudioWaveFormat();
    IXAudio2SourceVoice* sourceVoice = nullptr;
    hr = xaudio->CreateSourceVoice(&sourceVoice, &format);
    if (FAILED(hr) || !sourceVoice)
    {
        LogError("CreateSourceVoice failed: " + HResultString(hr));
        masteringVoice->DestroyVoice();
        xaudio->Release();
        return;
    }

    hr = sourceVoice->Start(0);
    if (FAILED(hr))
    {
        LogError("SourceVoice Start failed: " + HResultString(hr));
        sourceVoice->DestroyVoice();
        masteringVoice->DestroyVoice();
        xaudio->Release();
        return;
    }

    SharedAudioRingReader reader;
    std::array<std::vector<float>, kBufferCount> buffers;
    for (auto& buffer : buffers)
    {
        buffer.resize(static_cast<size_t>(kFramesPerBuffer) * SharedAudioChannels);
        FillSilence(buffer);
    }

    size_t bufferIndex = 0;
    auto lastStatusLog = std::chrono::steady_clock::now() - std::chrono::seconds(10);
    auto lastControlPoll = std::chrono::steady_clock::now() - kControlPollInterval;
    bool radioActive = IsRadioAudioActive(baseDirectory);
    bool f8WasDown = false;
    LogInfo(std::string("Radio audio gate initial state: ") + (radioActive ? "active" : "inactive") +
        ". Toggle " + std::filesystem::path(RadioActiveFlagPath(baseDirectory)).string());
    LogInfo("Radio audio gate hotkey: F8 while the game window is focused.");

    while (!stopRequested_)
    {
        XAUDIO2_VOICE_STATE state{};
        sourceVoice->GetState(&state, XAUDIO2_VOICE_NOSAMPLESPLAYED);

        if (state.BuffersQueued >= kTargetQueuedBuffers)
        {
            Sleep(5);
            continue;
        }

        auto& buffer = buffers[bufferIndex];
        const auto now = std::chrono::steady_clock::now();
        if (now - lastControlPoll >= kControlPollInterval)
        {
            const bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
            if (IsCurrentProcessForeground() && f8Down && !f8WasDown)
            {
                const bool requestedActive = !IsRadioAudioActive(baseDirectory);
                if (SetRadioAudioActive(baseDirectory, requestedActive))
                {
                    LogInfo(std::string("Radio audio gate hotkey toggled: ") +
                        (requestedActive ? "active" : "inactive"));
                }
                else
                {
                    LogWarn("Radio audio gate hotkey failed to update flag file.");
                }
            }
            f8WasDown = f8Down;

            const bool nextRadioActive = IsRadioAudioActive(baseDirectory);
            if (nextRadioActive != radioActive)
            {
                radioActive = nextRadioActive;
                LogInfo(std::string("Radio audio gate changed: ") + (radioActive ? "active" : "inactive"));
            }

            lastControlPoll = now;
        }

        if (!radioActive)
        {
            FillSilence(buffer);
        }
        else if (!reader.TryConnect())
        {
            FillSilence(buffer);
        }
        else if (!reader.ReadFrames(buffer.data(), kFramesPerBuffer))
        {
            const auto status = reader.GetStatus();
            if (!status.connected || !status.headerValid)
            {
                reader.Disconnect();
            }
        }

        XAUDIO2_BUFFER xaudioBuffer{};
        xaudioBuffer.AudioBytes = static_cast<UINT32>(buffer.size() * sizeof(float));
        xaudioBuffer.pAudioData = reinterpret_cast<const BYTE*>(buffer.data());

        hr = sourceVoice->SubmitSourceBuffer(&xaudioBuffer);
        if (FAILED(hr))
        {
            LogError("SubmitSourceBuffer failed: " + HResultString(hr));
            break;
        }

        bufferIndex = (bufferIndex + 1) % buffers.size();

        if (now - lastStatusLog >= std::chrono::seconds(5))
        {
            LogInfo(std::string("XAudio2 shared audio status: gate=") +
                (radioActive ? "active " : "inactive ") +
                FormatSharedAudioStatus(reader.GetStatus()));
            lastStatusLog = now;
        }
    }

    sourceVoice->Stop(0);
    sourceVoice->FlushSourceBuffers();
    sourceVoice->DestroyVoice();
    masteringVoice->DestroyVoice();
    xaudio->Release();

    LogInfo("XAudio2 shared audio worker stopped.");
}
}
