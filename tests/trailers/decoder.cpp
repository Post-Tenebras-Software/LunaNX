#include "trailer_decoder.hpp"
#include "launcher_media.hpp"
#include "launcher_text.hpp"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <cmath>
using namespace luna::launcher;
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
std::vector<unsigned char> load(const char* name)
{ std::ifstream input(name, std::ios::binary); return {std::istreambuf_iterator<char>(input), {}}; }
int main(int argc, char** argv)
{
    require(argc == 4, "Expected AV, silent and invalid-codec fixtures");
    require(trailerUrl("https://m.media-amazon.com/images/S/trailer.mp4?signature=synthetic") &&
        !trailerUrl("https://m.media-amazon.com.images.invalid/images/S/trailer.mp4") &&
        !trailerUrl("https://m.media-amazon.com@evil.invalid/images/trailer.mp4") &&
        !trailerUrl("file:///tmp/trailer.mp4") && !trailerUrl("http://m.media-amazon.com/images/S/trailer.mp4") &&
        !trailerUrl("https://m.media-amazon.com/images/S/playlist.m3u8"), "Trailer destinations and media types must be restricted");
    require(descriptionText("<style>hidden</style><p>Fish &amp; chips&nbsp;&#39;yes&#39;</p>") == "Fish & chips 'yes'" &&
        descriptionText("<script>hidden") == "" && descriptionText(std::string(32769, 'a')).empty(), "Rich text filtering and bounds");
    auto bytes = load(argv[1]); std::atomic<bool> cancelled{false};
    unsigned frames = 0, audio = 0; std::int64_t previous = -1; std::size_t samples = 0; double energy = 0;
    decodeTrailer(bytes, {
        [](TrailerInfo info) { require(info.audio && info.durationUs > 0, "Container audio/duration metadata"); },
        [&](TrailerVideo frame)
        {
            require(frame.image && frame.image->width == 160 && frame.image->height == 90 &&
                frame.image->pixels.size() == 160 * 90 * 4 && frame.timeUs >= previous, "Decoded video dimensions and presentation timestamps");
            previous = frame.timeUs; ++frames; return true;
        },
        [&](TrailerAudio frame)
        {
            require(!frame.samples.empty() && frame.samples.size() % 2 == 0 && frame.samples.size() <= MaxTrailerAudioSamples * 2,
                "Bounded interleaved stereo PCM");
            for (std::size_t i = 0; i < frame.samples.size(); i += 2)
            { require(frame.samples[i] == frame.samples[i + 1], "Mono input is resampled to both stereo channels"); energy += std::abs(int(frame.samples[i])); }
            samples += frame.samples.size() / 2; ++audio; return true;
        }}, cancelled, false);
    require(frames == 30 && audio > 30 && samples > 47000 && samples < 51000 && energy > 100000,
        "Real MP4 demux, delayed H.264/B-frame drain, AAC decode and 44.1kHz to 48kHz resampling");
    frames = 0;
    decodeTrailer(bytes, {{}, [&](TrailerVideo) { ++frames; return false; }, [](TrailerAudio) { return true; }}, cancelled, false);
    require(frames == 1, "Consumer cancellation stops the decoder without draining more frames");
    cancelled = true; frames = 0;
    decodeTrailer(bytes, {{}, [&](TrailerVideo) { ++frames; return true; }, {}}, cancelled, false);
    require(frames == 0, "Cancellation interrupts preparation and decoding");
    cancelled = false; frames = 0;
    decodeTrailer(load(argv[2]), {[](TrailerInfo info) { require(!info.audio, "Silent trailers do not require an audio device"); },
        [&](TrailerVideo) { ++frames; return true; }, {}}, cancelled, false);
    require(frames == 30, "Silent trailers still decode and drain completely");
    for (auto invalid : {std::vector<unsigned char>{}, std::vector<unsigned char>(32, 0), load(argv[3])})
    {
        bool rejected = false;
        try { decodeTrailer(invalid, {}, cancelled, false); } catch (const std::exception&) { rejected = true; }
        require(rejected, "Reject empty, malformed and unsupported-codec media");
    }
    std::cout << "PASS: real MP4/H.264/AAC decoding, B-frame drain, timestamp ordering, resampling, silent media, cancellation, URL/text validation and malformed input.\n";
}
