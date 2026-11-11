#pragma once
#include "h264_decoder.hpp"
#include <atomic>
#include <functional>
#include <string>

namespace luna::launcher
{
struct TrailerVideo { std::unique_ptr<RgbaImage> image; std::int64_t timeUs = 0; };
struct TrailerAudio { std::vector<std::int16_t> samples; std::int64_t timeUs = 0; };
struct TrailerInfo { std::int64_t durationUs = 0; bool audio = false; };
struct TrailerCallbacks
{
    std::function<void(TrailerInfo)> info;
    std::function<bool(TrailerVideo)> video;
    std::function<bool(TrailerAudio)> audio;
};
inline constexpr std::size_t MaxTrailerBytes = 96 * 1024 * 1024;
inline constexpr int MaxTrailerAudioSamples = 8192;
// Only an already-downloaded MP4 buffer; demuxing cannot open network resources.
void decodeTrailer(const std::vector<unsigned char>& bytes, const TrailerCallbacks& callbacks,
    std::atomic<bool>& cancelled, bool hardware = true);
}
