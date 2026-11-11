#include "launcher_trailer.hpp"
#include <switch.h>
extern "C" {
#include <libavutil/log.h>
}
#include <cstdarg>
#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <thread>
using namespace luna::launcher;
using Clock = std::chrono::steady_clock;
namespace
{
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
using Writer = std::size_t (*)(char*, std::size_t, std::size_t, void*);
struct Easy { Writer writer = nullptr; void* data = nullptr; bool verifiedPeer = false, verifiedHost = false, redirectsOff = false; long protocols = 0; curl_off_t limit = 0; };
struct Multi { CURL* easy = nullptr; bool delivered = false, reported = false; CURLMsg message{}; };
std::map<CURL*, Easy> handles;
std::map<CURLM*, Multi> multis;
std::vector<unsigned char> fixture;
long httpStatus = 200;
bool tooLarge = false;
std::deque<AudioOutBuffer*> submitted;
bool device = false, started = false;
std::int64_t elapsedSamples = 0;
Clock::time_point began;
std::size_t audioBytes = 0;
}
extern "C" {
CURL* curl_easy_init() { auto* p = reinterpret_cast<CURL*>(new char); handles[p] = {}; return p; }
void curl_easy_cleanup(CURL* p) { require(handles.erase(p) == 1, "Easy handle cleanup exactly once"); delete reinterpret_cast<char*>(p); }
CURLM* curl_multi_init() { auto* p = reinterpret_cast<CURLM*>(new char); multis[p] = {}; return p; }
CURLMcode curl_multi_cleanup(CURLM* p) { require(multis.erase(p) == 1, "Multi cleanup exactly once"); delete reinterpret_cast<char*>(p); return CURLM_OK; }
CURLMcode curl_multi_add_handle(CURLM* p, CURL* e) { multis.at(p).easy = e; return CURLM_OK; }
CURLMcode curl_multi_remove_handle(CURLM*, CURL*) { return CURLM_OK; }
CURLcode curl_easy_setopt(CURL* p, CURLoption option, ...)
{
    auto& h = handles.at(p); va_list args; va_start(args, option);
    if (option == CURLOPT_WRITEFUNCTION) h.writer = va_arg(args, Writer);
    else if (option == CURLOPT_WRITEDATA) h.data = va_arg(args, void*);
    else if (option == CURLOPT_SSL_VERIFYPEER) h.verifiedPeer = va_arg(args, long) == 1;
    else if (option == CURLOPT_SSL_VERIFYHOST) h.verifiedHost = va_arg(args, long) == 2;
    else if (option == CURLOPT_FOLLOWLOCATION) h.redirectsOff = va_arg(args, long) == 0;
    else if (option == CURLOPT_PROTOCOLS) h.protocols = va_arg(args, long);
    else if (option == CURLOPT_MAXFILESIZE_LARGE) h.limit = va_arg(args, curl_off_t);
    va_end(args); return CURLE_OK;
}
CURLcode curl_easy_getinfo(CURL*, CURLINFO option, ...)
{ va_list args; va_start(args, option); *va_arg(args, long*) = httpStatus; va_end(args); return CURLE_OK; }
CURLMcode curl_multi_perform(CURLM* p, int* running)
{
    auto& m = multis.at(p); auto& h = handles.at(m.easy);
    require(h.verifiedPeer && h.verifiedHost && h.redirectsOff && h.protocols == CURLPROTO_HTTPS && h.limit == MaxTrailerBytes,
        "Trailer HTTP keeps TLS validation, HTTPS-only, no redirects and a bounded body");
    if (!m.delivered)
    {
        const auto size = tooLarge ? MaxTrailerBytes + 1 : fixture.size();
        const auto wrote = h.writer(reinterpret_cast<char*>(fixture.data()), 1, size, h.data);
        m.message.msg = CURLMSG_DONE; m.message.data.result = wrote == size ? CURLE_OK : CURLE_WRITE_ERROR;
        m.delivered = true;
    }
    *running = 0; return CURLM_OK;
}
CURLMsg* curl_multi_info_read(CURLM* p, int* left)
{ auto& m = multis.at(p); *left = 0; if (m.reported) return nullptr; m.reported = true; return &m.message; }
}
Result audoutInitialize() { require(!device, "Never open two trailer audio devices"); device = true; return 0; }
void audoutExit() { require(device, "Exit an initialized audio device"); device = started = false; submitted.clear(); elapsedSamples = 0; }
u32 audoutGetSampleRate() { return 48000; }
u32 audoutGetChannelCount() { return 2; }
PcmFormat audoutGetPcmFormat() { return PcmFormat_Int16; }
Result audoutAppendAudioOutBuffer(AudioOutBuffer* buffer)
{
    require(device && buffer->buffer && reinterpret_cast<std::uintptr_t>(buffer->buffer) % 4096 == 0 &&
        buffer->data_size <= buffer->buffer_size, "Audio buffer alignment, size and ownership");
    submitted.push_back(buffer); audioBytes += buffer->data_size; return 0;
}
Result audoutGetReleasedAudioOutBuffer(AudioOutBuffer** buffer, u32* count)
{
    *buffer = nullptr; *count = 0;
    if (started && !submitted.empty())
    {
        const auto played = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - began).count() * 48000 / 1000000;
        const auto samples = std::int64_t(submitted.front()->data_size / 4);
        if (played >= elapsedSamples + samples)
        { elapsedSamples += samples; *buffer = submitted.front(); submitted.pop_front(); *count = 1; }
    }
    return 0;
}
Result audoutStartAudioOut() { if (!started) began = Clock::now(); started = true; return 0; }
Result audoutStopAudioOut() { started = false; return 0; }
void armDCacheFlush(void*, std::size_t) {}
int main(int argc, char** argv)
{
    require(argc == 2, "Expected synthetic trailer fixture"); av_log_set_level(AV_LOG_ERROR);
    std::ifstream input(argv[1], std::ios::binary); fixture = {std::istreambuf_iterator<char>(input), {}};
    TrailerPlayer player(nullptr);
    const std::string url = "https://m.media-amazon.com/images/S/synthetic.mp4";
    player.start("http://untrusted.invalid/file.mp4");
    require(player.state() == TrailerPlayer::State::Failed && handles.empty(), "Invalid URL never opens a connection");
    player.start(url); player.stop();
    require(handles.empty() && multis.empty() && !device, "Cancel download closes transport without opening audio");
    httpStatus = 404; player.start(url); player.update();
    require(player.state() == TrailerPlayer::State::Failed && handles.empty() && !device, "HTTP errors do not enter the media pipeline");
    httpStatus = 200; tooLarge = true; player.start(url); player.update();
    require(player.state() == TrailerPlayer::State::Failed && handles.empty(), "Over-limit body aborts before copying bytes");
    tooLarge = false;
    for (unsigned i = 0; i < 12; ++i)
    {
        player.start(url); player.update();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        const auto start = Clock::now(); player.stop();
        require(Clock::now() - start < std::chrono::milliseconds(500), "Stop joins a producer blocked on full media queues");
        require(player.state() == TrailerPlayer::State::Idle && handles.empty() && multis.empty() && !device,
            "Repeat/cancel resets workers, transport and audio");
    }
    player.start(url); unsigned frames = 0; const auto deadline = Clock::now() + std::chrono::seconds(4);
    while (Clock::now() < deadline && player.state() != TrailerPlayer::State::Finished && player.state() != TrailerPlayer::State::Failed)
    {
        player.update(); if (player.takeFrame()) ++frames;
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
    }
    require(player.state() == TrailerPlayer::State::Finished && frames >= 20 && audioBytes > 100000,
        "Timed playback presents video, drains PCM audio and reaches completion");
    player.stop();
    require(!device && handles.empty() && multis.empty(), "Completed playback leaves no audio or HTTP resources");
    std::cout << "PASS: trailer download policies/errors/bounds, queue-full cancellation, repeated open/stop, timed video/audio and resource cleanup (mock HTTP/libnx).\n";
}
