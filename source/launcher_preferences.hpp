#pragma once
#include "video_bitrate.hpp"
#include "video_decoder.hpp"

namespace luna::launcher
{
struct VideoPreferences
{
    video::Mode mode = video::Mode::P720_30;
    video::Decoder decoder = video::Decoder::Automatic;
    unsigned bitrateMbps = 0;
    bool statsOverlay = false;
};
VideoPreferences loadVideoPreferences(const std::string& path);
bool saveVideoPreferences(const std::string& path, const VideoPreferences& settings);
}
