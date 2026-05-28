#include "fmod_radio_stub.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

namespace
{
struct Levels
{
    float peakL = 0.0f;
    float peakR = 0.0f;
    float rmsL = 0.0f;
    float rmsR = 0.0f;
};

Levels CalculateLevels(const std::vector<float>& samples, uint32_t frames)
{
    Levels levels{};
    if (frames == 0)
    {
        return levels;
    }

    double sumL = 0.0;
    double sumR = 0.0;
    for (uint32_t i = 0; i < frames; ++i)
    {
        const float left = samples[(i * 2) + 0];
        const float right = samples[(i * 2) + 1];
        levels.peakL = std::max(levels.peakL, std::abs(left));
        levels.peakR = std::max(levels.peakR, std::abs(right));
        sumL += static_cast<double>(left) * left;
        sumR += static_cast<double>(right) * right;
    }

    levels.rmsL = static_cast<float>(std::sqrt(sumL / frames));
    levels.rmsR = static_cast<float>(std::sqrt(sumR / frames));
    return levels;
}

int ParseIntArg(char** argv, int index, int fallback)
{
    if (!argv[index])
    {
        return fallback;
    }

    try
    {
        return std::stoi(argv[index]);
    }
    catch (...)
    {
        return fallback;
    }
}
}

int main(int argc, char** argv)
{
    const int seconds = argc > 1 ? ParseIntArg(argv, 1, 10) : 10;
    const int framesPerBlock = argc > 2 ? ParseIntArg(argv, 2, 960) : 960;
    const auto safeFramesPerBlock = static_cast<uint32_t>(std::clamp(framesPerBlock, 64, 48000));
    const auto iterations = std::max(1, seconds * static_cast<int>(fh6rb::SharedAudioSampleRate / safeFramesPerBlock));

    fh6rb::FmodRadioPcmProvider provider;
    std::vector<float> buffer(static_cast<size_t>(safeFramesPerBlock) * fh6rb::SharedAudioChannels);

    std::cout << "SharedAudioReadTest starting. seconds=" << seconds
              << " framesPerBlock=" << safeFramesPerBlock << "\n";

    bool lastConnected = false;
    for (int i = 0; i < iterations; ++i)
    {
        if (!provider.GetStatus().connected)
        {
            provider.TryConnect();
        }

        const bool fullBlock = provider.FillFloat32Stereo(buffer.data(), safeFramesPerBlock);
        const auto levels = CalculateLevels(buffer, safeFramesPerBlock);
        const auto status = provider.GetStatus();

        const bool connectedChanged = status.connected != lastConnected;
        lastConnected = status.connected;

        if (i == 0 || i % 10 == 0 || connectedChanged)
        {
            std::cout << "connected=" << (status.connected ? "true" : "false")
                      << " headerValid=" << (status.headerValid ? "true" : "false")
                      << " fullBlock=" << (fullBlock ? "true" : "false")
                      << " writeFrame=" << status.writeFrame
                      << " readFrame=" << status.localReadFrame
                      << " writerAgeMs=" << status.writerAgeMs
                      << " localUnderruns=" << status.localUnderrunCount
                      << " peak=(" << levels.peakL << "," << levels.peakR << ")"
                      << " rms=(" << levels.rmsL << "," << levels.rmsR << ")\n";
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(
            static_cast<int64_t>(safeFramesPerBlock) * 1000 / fh6rb::SharedAudioSampleRate));
    }

    return 0;
}
