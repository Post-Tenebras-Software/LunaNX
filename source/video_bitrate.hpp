#pragma once
#include "video_mode.hpp"
#include <array>
#include <string>

namespace luna::video
{
// Zero preserves the existing per-mode adaptive range. Manual values are
// ceilings, not a command for the encoder to hold a constant bitrate.
inline constexpr std::array<unsigned, 9> BitrateChoices{0, 5, 8, 12, 14, 15, 20, 25, 30};
inline bool validBitrate(unsigned mbps)
{ return std::find(BitrateChoices.begin(), BitrateChoices.end(), mbps) != BitrateChoices.end(); }
inline unsigned bitrateLimit(Mode mode)
{
    // Conservative baseline H.264 table A-1 MaxBR limits for our SDP levels.
    switch (configuration(mode).h264Level)
    {
    case 31: return 14;
    case 32: case 40: return 20;
    case 42: return 30; // UI ceiling below this level's 50 Mbps MaxBR.
    default: throw std::invalid_argument("Unsupported bitrate level");
    }
}
inline unsigned effectiveBitrate(Mode mode, unsigned mbps)
{
    if (!validBitrate(mbps)) throw std::invalid_argument("Invalid bitrate setting");
    return std::min(mbps, bitrateLimit(mode));
}
inline unsigned nextBitrate(Mode mode, unsigned current)
{
    current = effectiveBitrate(mode, current);
    for (const auto choice : BitrateChoices)
        if (choice > current && choice <= bitrateLimit(mode)) return choice;
    return 0;
}
struct BitrateRange { std::uint32_t minimum, maximum, start; };
inline BitrateRange bitrateRange(Mode mode, unsigned mbps = 0)
{
    const auto settings = configuration(mode);
    const auto selected = effectiveBitrate(mode, mbps);
    if (!selected) return {settings.minBits, settings.maxBits, settings.startBits};
    const auto maximum = selected * 1'000'000u;
    // Leave adaptation headroom rather than forcing the minimum up with the cap.
    return {std::min(settings.minBits, 2'000'000u), maximum, maximum * 3 / 4};
}
inline std::string bitrateLabel(Mode mode, unsigned mbps)
{
    const auto range = bitrateRange(mode, mbps);
    return std::string(mbps ? "Up to " : "Auto / up to ") +
        std::to_string(range.maximum / 1'000'000) + " Mbps";
}
}
