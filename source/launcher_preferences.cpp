#include "launcher_preferences.hpp"
#include <charconv>
#include <filesystem>
#include <fstream>

namespace luna::launcher
{
namespace
{
bool valid(const VideoPreferences& value)
{
    try { video::configuration(value.mode); } catch (...) { return false; }
    return (value.decoder == video::Decoder::Software || value.decoder == video::Decoder::Automatic) &&
        video::validBitrate(value.bitrateMbps);
}
unsigned field(std::istream& input, const std::string& key)
{
    std::string line;
    if (!std::getline(input, line) || line.rfind(key, 0) != 0) throw std::invalid_argument("Preference field");
    unsigned value = 0;
    const auto result = std::from_chars(line.data() + key.size(), line.data() + line.size(), value);
    if (result.ec != std::errc{} || result.ptr != line.data() + line.size())
        throw std::invalid_argument("Preference value");
    return value;
}
}
VideoPreferences loadVideoPreferences(const std::string& path)
{
    try
    {
        if (std::filesystem::file_size(path) > 128) return {};
        std::ifstream input(path);
        std::string header, extra;
        if (!std::getline(input, header) || (header != "LUNANX_VIDEO_V1" && header != "LUNANX_VIDEO_V2")) return {};
        const auto mode = field(input, "mode=");
        const auto decoder = field(input, "decoder=");
        const auto bitrate = field(input, "bitrate=");
        const auto overlay = header == "LUNANX_VIDEO_V2" ? field(input, "overlay=") : 0;
        if (mode > 3 || decoder > 1 || overlay > 1 || std::getline(input, extra)) return {};
        const VideoPreferences value{static_cast<video::Mode>(mode), static_cast<video::Decoder>(decoder), bitrate, overlay != 0};
        return valid(value) ? value : VideoPreferences{};
    }
    catch (...) { return {}; }
}
bool saveVideoPreferences(const std::string& path, const VideoPreferences& settings)
{
    if (!valid(settings)) return false;
    try
    {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        const auto temporary = path + ".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        output << "LUNANX_VIDEO_V2\nmode=" << static_cast<unsigned>(settings.mode) <<
            "\ndecoder=" << static_cast<unsigned>(settings.decoder) << "\nbitrate=" << settings.bitrateMbps <<
            "\noverlay=" << settings.statsOverlay << '\n';
        output.close();
        std::error_code error;
        if (output) std::filesystem::rename(temporary, path, error);
        if (!output || error) { std::filesystem::remove(temporary, error); return false; }
        return true;
    }
    catch (...) { return false; }
}
}
