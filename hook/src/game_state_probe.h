#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fh6rb
{
class GameStateProbe
{
public:
    struct Snapshot
    {
        bool raceActive = false;
        bool raceRestart = false;
        bool onTargetStation = false;
        bool valid = false;
        std::string stationName;
    };

    GameStateProbe();

    bool IsAvailable() const noexcept { return singletonSlot_ != nullptr; }

    Snapshot Read() const noexcept;

private:
    struct ModuleImage
    {
        std::uint8_t* base = nullptr;
        std::size_t size = 0;
    };

    static ModuleImage CurrentProcessImage() noexcept;
    static std::uint8_t* FindPattern(const ModuleImage& image, const char* pattern) noexcept;

    const std::uint8_t* const* singletonSlot_ = nullptr;
};
}
