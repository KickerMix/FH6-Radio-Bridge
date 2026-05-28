#pragma once

#include <atomic>
#include <string>
#include <thread>

namespace fh6rb
{
std::wstring InProcessAudioFlagPath(const std::wstring& baseDirectory);
std::wstring RadioActiveFlagPath(const std::wstring& baseDirectory);
bool IsInProcessAudioEnabled(const std::wstring& baseDirectory);
bool IsRadioAudioActive(const std::wstring& baseDirectory);
bool SetRadioAudioActive(const std::wstring& baseDirectory, bool active);

class XAudioSharedPlayer
{
public:
    XAudioSharedPlayer() = default;
    ~XAudioSharedPlayer();

    XAudioSharedPlayer(const XAudioSharedPlayer&) = delete;
    XAudioSharedPlayer& operator=(const XAudioSharedPlayer&) = delete;

    bool Start(const std::wstring& baseDirectory);
    void Stop();

private:
    void Worker(std::wstring baseDirectory);

    std::atomic_bool stopRequested_{false};
    std::thread worker_;
};
}
