#include "video_mode.hpp"
#include "local_control_session.hpp"
#include "launcher_preferences.hpp"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <vector>

namespace video = luna::video;
namespace control = luna::control;
namespace
{
unsigned checks = 0;
void check(bool condition, const char* message)
{
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
control::DisplayConfiguration declaration(video::Mode mode, bool live, unsigned bitrate = 0)
{
    control::LocalControlSession session([] { return control::ClockReading{1000000, 0}; }, live, mode, bitrate);
    std::vector<control::Bytes> messages;
    for (int i = 0; i < 2; ++i) session.update(true, false, [&](const auto& bytes) { messages.push_back(bytes); });
    check(messages.size() == 2, "Startup sends feature and display declarations");
    return std::get<control::DisplayConfiguration>(control::decodePayload(messages[1]).body);
}
}
int main(int argc, char** argv)
{
    using Mode = video::Mode;
    check(argc == 2, "Settings tests require an isolated temporary directory");
    const auto baseline = declaration(Mode::P720_30, false);
    check(baseline.display && baseline.display->resolution && baseline.display->resolution->width == 1280 &&
        baseline.display->resolution->height == 720 && baseline.display->refreshRate == 30 &&
        baseline.codecs[0].profile == 66 && baseline.codecs[0].level == 31 && baseline.startBitsPerSecond == 600000 &&
        baseline.minBitsPerSecond == 400000 && baseline.maxBitsPerSecond == 800000, "Local alpha startup defaults stay at 720p30");
    const auto liveBaseline = declaration(Mode::P720_30, true);
    check(liveBaseline.startBitsPerSecond == 6000000 && liveBaseline.minBitsPerSecond == 4000000 &&
        liveBaseline.maxBitsPerSecond == 8000000, "Live alpha default bitrates stay unchanged");
    struct Expected { Mode mode; unsigned width, height, fps, level, maxFs, maxMbps; const char* sdpLevel; };
    for (const auto& expected : std::vector<Expected>{
        {Mode::P720_30, 1280, 720, 30, 31, 3600, 108000, "42e01f"},
        {Mode::P720_60, 1280, 720, 60, 32, 5120, 216000, "42e020"},
        {Mode::P1080_30, 1920, 1080, 30, 40, 8192, 245760, "42e028"},
        {Mode::P1080_60, 1920, 1080, 60, 42, 8704, 522240, "42e02a"}})
    {
        const auto message = declaration(expected.mode, true);
        const auto& display = *message.display;
        check(display.resolution->width == expected.width && display.resolution->height == expected.height &&
            display.refreshRate == expected.fps, "Chosen dimensions and FPS reach encoded control message");
        check(message.codecs.size() == 1 && message.codecs[0].level == expected.level &&
            std::string(video::configuration(expected.mode).fmtp).find(expected.sdpLevel) != std::string::npos,
            "SDP and control channel agree on the H.264 level");
        const auto macroblocks = ((expected.width + 15) / 16) * ((expected.height + 15) / 16);
        check(macroblocks <= expected.maxFs && macroblocks * expected.fps <= expected.maxMbps,
            "Independent H.264 table A-1 limits admit the requested frame size/rate");
        check(message.minBitsPerSecond <= message.startBitsPerSecond && message.startBitsPerSecond <= message.maxBitsPerSecond,
            "Requested bitrate range is ordered");
        check(video::effectiveMode(expected.mode, true) == expected.mode, "All four requests are available docked");
    }
    auto cycle = Mode::P720_30;
    for (const auto expected : {Mode::P720_60, Mode::P1080_30, Mode::P1080_60, Mode::P720_30})
    {
        cycle = video::nextMode(cycle, true); check(cycle == expected, "Docked selector cycles through four modes");
    }
    check(video::nextMode(Mode::P720_60, false) == Mode::P720_30 &&
        video::effectiveMode(Mode::P1080_60, false) == Mode::P720_60 &&
        video::effectiveMode(Mode::P1080_30, false) == Mode::P720_30, "Handheld cannot start a 1080p request, including after undocking during setup");
    for (const auto target : std::vector<video::Size>{{1280,720},{1920,1080}})
    {
        for (const auto source : std::vector<video::Size>{{640,360},{1280,720},{1920,1080},{3840,2160},
            {640,480},{480,640},{std::numeric_limits<int>::max(),1},{1,std::numeric_limits<int>::max()}})
        {
            const auto fitted = video::fitSize(source.width, source.height, target.width, target.height);
            check(fitted.width > 0 && fitted.width <= target.width && fitted.height > 0 && fitted.height <= target.height,
                "Letterboxed output always fits configured buffers, including extreme input dimensions");
        }
        const auto sixteenNine = video::fitSize(1920,1080,target.width,target.height);
        check(sixteenNine.width == target.width && sixteenNine.height == target.height, "16:9 output fills configured resolution");
    }
    const auto fourThree = video::fitSize(640,480,1920,1080);
    check(fourThree.width == 1440 && fourThree.height == 1080, "1080p conversion preserves 4:3 aspect ratio");
    for (const auto bad : std::vector<video::Size>{{0,720},{1921,1080},{1280,1081},{-1,720}})
    {
        bool rejected = false;
        try { video::fitSize(640,360,bad.width,bad.height); } catch (const std::invalid_argument&) { rejected = true; }
        check(rejected, "Invalid framebuffer sizes are rejected before decoder/framebuffer allocation");
    }
    bool rejected = false;
    try { video::configuration(static_cast<Mode>(99)); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Unknown video mode is rejected");
    const auto raised = declaration(Mode::P720_60, true, 20);
    check(raised.maxBitsPerSecond == 20'000'000 && raised.startBitsPerSecond == 15'000'000 &&
        raised.minBitsPerSecond == 2'000'000 && raised.display->refreshRate == 60,
        "Handheld 20 Mbps reaches the encoded startup declaration without changing resolution/FPS");
    const auto local = declaration(Mode::P720_60, false, 20);
    check(local.maxBitsPerSecond == 800'000 && local.startBitsPerSecond == 600'000,
        "Manual streaming caps leave local test defaults unchanged");
    for (const auto& limit : std::vector<std::pair<Mode, unsigned>>{
        {Mode::P720_30, 14}, {Mode::P720_60, 20}, {Mode::P1080_30, 20}, {Mode::P1080_60, 30}})
    {
        for (const auto choice : video::BitrateChoices)
        {
            const auto config = declaration(limit.first, true, choice);
            check(config.minBitsPerSecond <= config.startBitsPerSecond && config.startBitsPerSecond <= config.maxBitsPerSecond &&
                config.maxBitsPerSecond <= limit.second * 1'000'000,
                "Every bitrate choice is ordered and bounded by the independently listed mode cap");
        }
        unsigned choice = 0, count = 0;
        do
        {
            choice = video::nextBitrate(limit.first, choice);
            check(video::validBitrate(choice) && choice <= limit.second, "Settings cycle stays within the mode's supported choices");
            ++count;
        } while (choice && count <= video::BitrateChoices.size());
        check(!choice && count > 1, "Bitrate cycle returns to Auto");
    }
    check(video::bitrateRange(Mode::P720_30, 30).maximum == 14'000'000 &&
        video::bitrateRange(video::effectiveMode(Mode::P1080_60, false), 30).maximum == 20'000'000,
        "Saved bitrate is clamped after mode/handheld changes");
    rejected = false;
    try { declaration(Mode::P720_60, true, 999); } catch (const std::invalid_argument&) { rejected = true; }
    check(rejected, "Invalid bitrate settings cannot reach a control message");

    namespace launcher = luna::launcher;
    const auto path = (std::filesystem::path(argv[1]) / "video.cfg").string();
    const auto defaults = [&](const launcher::VideoPreferences& value)
    { return value.mode == Mode::P720_30 && value.decoder == video::Decoder::Automatic && value.bitrateMbps == 0 && !value.statsOverlay; };
    check(defaults(launcher::loadVideoPreferences(path)), "Missing settings preserve existing defaults");
    for (const auto mode : {Mode::P720_30, Mode::P720_60, Mode::P1080_30, Mode::P1080_60})
        for (const auto decoder : {video::Decoder::Automatic, video::Decoder::Software})
            for (const auto bitrate : video::BitrateChoices)
            for (const bool overlay : {false, true})
            {
                check(launcher::saveVideoPreferences(path, {mode, decoder, bitrate, overlay}), "Valid video settings save");
                const auto restored = launcher::loadVideoPreferences(path);
                check(restored.mode == mode && restored.decoder == decoder && restored.bitrateMbps == bitrate && restored.statsOverlay == overlay,
                    "Mode, decoder, bitrate and overlay survive a settings-file round trip");
            }
    check(!launcher::saveVideoPreferences(path, {Mode::P720_30, video::Decoder::Automatic, 999}) &&
        launcher::loadVideoPreferences(path).bitrateMbps == 30, "An invalid save preserves the previous file");
    for (const auto& body : std::vector<std::string>{"", "LUNANX_VIDEO_V2\nmode=0\ndecoder=1\nbitrate=0\n",
        "LUNANX_VIDEO_V1\nmode=99\ndecoder=1\nbitrate=0\n", "LUNANX_VIDEO_V1\nmode=0\ndecoder=8\nbitrate=0\n",
        "LUNANX_VIDEO_V1\nmode=0\ndecoder=1\nbitrate=-1\n", "LUNANX_VIDEO_V1\nmode=0\ndecoder=1\nbitrate=999\n",
        "LUNANX_VIDEO_V1\nmode=0\ndecoder=1\nbitrate=20\nextra=1\n", "LUNANX_VIDEO_V1\nmode=1\n",
        "LUNANX_VIDEO_V2\nmode=0\ndecoder=1\nbitrate=0\noverlay=8\n",
        "LUNANX_VIDEO_V1\nmode=0junk\ndecoder=1\nbitrate=0\n", std::string(129, 'x')})
    {
        { std::ofstream output(path, std::ios::binary | std::ios::trunc); output << body; }
        check(defaults(launcher::loadVideoPreferences(path)), "Corrupt, unknown or oversized settings restore defaults");
    }
    { std::ofstream output(path); output << "LUNANX_VIDEO_V1\nmode=1\ndecoder=1\nbitrate=20\n"; }
    const auto migrated = launcher::loadVideoPreferences(path);
    check(migrated.mode == Mode::P720_60 && migrated.bitrateMbps == 20 && !migrated.statsOverlay,
        "Older three-field settings load with the overlay disabled");
    const auto unavailable = (std::filesystem::path(path) / "child.cfg").string();
    check(!launcher::saveVideoPreferences(unavailable, {}) && defaults(launcher::loadVideoPreferences(unavailable)),
        "An unavailable SD path fails without altering the in-memory selection");
    std::cout << "PASS: " << checks << " video mode, encoded bitrate, settings persistence, level and output-bound checks (no hardware claim).\n";
}
