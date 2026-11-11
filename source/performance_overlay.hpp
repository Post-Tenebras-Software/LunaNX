#pragma once
#include "h264_decoder.hpp"
#include "video_bitrate.hpp"
#include <string>

namespace luna::video
{
struct PerformanceSample
{
    Mode mode = Mode::P720_30;
    unsigned bitrateMbps = 0;
    Backend backend = Backend::Software;
    bool ready = false;
    double decodedFps = 0, presentedFps = 0, encodedMbps = 0;
    double decodeMs = 0, transferMs = 0, rgbaMs = 0, presentMs = 0, loopMs = 0;
    std::size_t replaced = 0, fallbacks = 0;
};
class PerformanceOverlay
{
public:
    explicit PerformanceOverlay(const std::string& font);
    ~PerformanceOverlay();
    PerformanceOverlay(const PerformanceOverlay&) = delete;
    PerformanceOverlay& operator=(const PerformanceOverlay&) = delete;
    const RgbaImage& draw(const PerformanceSample& sample);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
