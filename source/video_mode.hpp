#pragma once

#include <algorithm>
#include <cstdint>
#include <stdexcept>

namespace luna::video
{
enum class Mode { P720_30, P720_60, P1080_30, P1080_60 };
struct Configuration
{
    int width, height, fps;
    std::uint32_t h264Level, minBits, maxBits, startBits;
    const char* fmtp;
    const char* label;
};
inline Configuration configuration(Mode mode)
{
    // H.264 level limits: FFmpeg libavcodec/h264_levels.c (table A-1).
    // A requested level/resolution is experimental capacity, not measured FPS.
    switch (mode)
    {
    case Mode::P720_30: return {1280, 720, 30, 31, 4'000'000, 8'000'000, 6'000'000,
        "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e01f", "720p30"};
    case Mode::P720_60: return {1280, 720, 60, 32, 6'000'000, 12'000'000, 8'000'000,
        "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e020", "720p60 (experimental)"};
    case Mode::P1080_30: return {1920, 1080, 30, 40, 8'000'000, 16'000'000, 10'000'000,
        "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e028", "1080p30 (TV, experimental)"};
    case Mode::P1080_60: return {1920, 1080, 60, 42, 10'000'000, 20'000'000, 12'000'000,
        "level-asymmetry-allowed=0;packetization-mode=1;profile-level-id=42e02a", "1080p60 (TV, experimental)"};
    }
    throw std::invalid_argument("Unknown video mode");
}
inline Mode effectiveMode(Mode selected, bool docked)
{
    configuration(selected); // Reject invalid enums, including in docked mode.
    if (!docked && selected == Mode::P1080_30) return Mode::P720_30;
    if (!docked && selected == Mode::P1080_60) return Mode::P720_60;
    return selected;
}
inline Mode nextMode(Mode current, bool docked)
{
    current = effectiveMode(current, docked);
    switch (current)
    {
    case Mode::P720_30: return Mode::P720_60;
    case Mode::P720_60: return docked ? Mode::P1080_30 : Mode::P720_30;
    case Mode::P1080_30: return Mode::P1080_60;
    case Mode::P1080_60: return Mode::P720_30;
    }
    throw std::invalid_argument("Unknown video mode");
}
struct Size { int width, height; };
inline Size fitSize(int sourceWidth, int sourceHeight, int outputWidth, int outputHeight)
{
    if (sourceWidth <= 0 || sourceHeight <= 0 || outputWidth <= 0 || outputWidth > 1920 ||
        outputHeight <= 0 || outputHeight > 1080) throw std::invalid_argument("Invalid video dimensions");
    if (static_cast<std::int64_t>(sourceWidth) * outputHeight >= static_cast<std::int64_t>(sourceHeight) * outputWidth)
        return {outputWidth, std::max(1, static_cast<int>(static_cast<std::int64_t>(sourceHeight) * outputWidth / sourceWidth))};
    return {std::max(1, static_cast<int>(static_cast<std::int64_t>(sourceWidth) * outputHeight / sourceHeight)), outputHeight};
}
}
