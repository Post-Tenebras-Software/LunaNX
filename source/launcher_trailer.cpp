#include "launcher_trailer.hpp"
#include "launcher_media.hpp"
#include <switch.h>
#include <array>
#include <condition_variable>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <malloc.h>
#include <mutex>
#include <stdexcept>
#include <thread>

namespace luna::launcher
{
namespace
{
void audioCheck(Result result)
{ if (R_FAILED(result)) throw std::runtime_error("Trailer audio output unavailable"); }
class TrailerOutput
{
    static constexpr std::size_t BufferBytes = MaxTrailerAudioSamples * 2 * sizeof(std::int16_t);
    struct Slot { AudioOutBuffer buffer{}; bool queued = false; };
    std::array<Slot, 4> slots_{};
    bool initialized_ = false, playing_ = false;
public:
    ~TrailerOutput() { stop(); }
    void release()
    {
        if (!initialized_) return;
        for (unsigned attempts = 0; attempts < slots_.size(); ++attempts)
        {
            AudioOutBuffer* released = nullptr; u32 count = 0;
            audioCheck(audoutGetReleasedAudioOutBuffer(&released, &count));
            if (!count) break;
            bool found = false;
            for (auto& slot : slots_) if (&slot.buffer == released) { slot.queued = false; found = true; break; }
            if (!found) throw std::runtime_error("Trailer audio returned an invalid buffer");
        }
    }
    bool available() const
    { for (const auto& slot : slots_) if (!slot.queued) return true; return false; }
    bool pending() const
    { for (const auto& slot : slots_) if (slot.queued) return true; return false; }
    void submit(const TrailerAudio& pcm)
    {
        if (pcm.samples.empty() || pcm.samples.size() > MaxTrailerAudioSamples * 2 || pcm.samples.size() % 2)
            throw std::runtime_error("Invalid trailer audio samples");
        if (!initialized_)
        {
            audioCheck(audoutInitialize()); initialized_ = true;
            if (audoutGetSampleRate() != 48000 || audoutGetChannelCount() != 2 || audoutGetPcmFormat() != PcmFormat_Int16)
                throw std::runtime_error("Trailer audio output format unavailable");
            for (auto& slot : slots_)
            {
                slot.buffer.buffer = memalign(0x1000, BufferBytes);
                if (!slot.buffer.buffer) throw std::bad_alloc();
                slot.buffer.buffer_size = BufferBytes;
            }
        }
        for (auto& slot : slots_) if (!slot.queued)
        {
            slot.buffer.data_size = pcm.samples.size() * sizeof(std::int16_t);
            std::memcpy(slot.buffer.buffer, pcm.samples.data(), slot.buffer.data_size);
            armDCacheFlush(slot.buffer.buffer, slot.buffer.data_size);
            audioCheck(audoutAppendAudioOutBuffer(&slot.buffer)); slot.queued = true;
            return;
        }
        throw std::runtime_error("Trailer audio queue full");
    }
    void play()
    { if (initialized_ && !playing_ && pending()) { audioCheck(audoutStartAudioOut()); playing_ = true; } }
    void stop()
    {
        if (initialized_) { audoutStopAudioOut(); audoutExit(); initialized_ = playing_ = false; }
        for (auto& slot : slots_) { std::free(slot.buffer.buffer); slot = {}; }
    }
};
}
struct TrailerPlayer::Impl
{
    TlsSetup tls;
    CURLM* multi = nullptr;
    CURL* easy = nullptr;
    State phase = State::Idle;
    std::string error;
    std::vector<unsigned char> bytes;
    std::atomic<bool> cancelled{false};
    std::thread worker;
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<TrailerVideo> video;
    std::deque<TrailerAudio> audio;
    bool done = false, ready = false, started = false, videoProduced = false;
    std::string decodeError;
    TrailerInfo info;
    std::unique_ptr<RgbaImage> pending;
    TrailerOutput output;
    std::chrono::steady_clock::time_point startTime;
    std::int64_t lastVideoTime = 0;
    explicit Impl(TlsSetup setup) : tls(setup) {}
    ~Impl() { stop(); }
    void networkStop()
    {
        if (easy) { if (multi) curl_multi_remove_handle(multi, easy); curl_easy_cleanup(easy); easy = nullptr; }
        if (multi) { curl_multi_cleanup(multi); multi = nullptr; }
    }
    void stop()
    {
        { std::lock_guard<std::mutex> lock(mutex); cancelled = true; }
        wake.notify_all(); networkStop();
        if (worker.joinable()) worker.join();
        output.stop();
        std::lock_guard<std::mutex> lock(mutex);
        video.clear(); audio.clear(); bytes.clear(); bytes.shrink_to_fit(); pending.reset();
        done = ready = started = videoProduced = false; decodeError.clear(); error.clear(); phase = State::Idle;
    }
    static std::size_t receive(char* data, std::size_t size, std::size_t count, void* context)
    {
        auto& self = *static_cast<Impl*>(context);
        if (self.cancelled || (size && count > (MaxTrailerBytes - self.bytes.size()) / size)) return 0;
        try { self.bytes.insert(self.bytes.end(), data, data + size * count); return size * count; }
        catch (...) { return 0; }
    }
    template<typename T> void set(CURLoption option, T value)
    { if (curl_easy_setopt(easy, option, value) != CURLE_OK) throw std::runtime_error("Trailer network setup failed"); }
    void start(const std::string& url)
    {
        stop(); cancelled = false;
        if (!trailerUrl(url)) { phase = State::Failed; error = "Luna didn't supply a supported trailer URL."; return; }
        try
        {
            multi = curl_multi_init(); easy = curl_easy_init();
            if (!multi || !easy) throw std::runtime_error("Trailer network initialization failed");
            set(CURLOPT_URL, url.c_str()); set(CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
            set(CURLOPT_FOLLOWLOCATION, 0L); set(CURLOPT_SSL_VERIFYPEER, 1L); set(CURLOPT_SSL_VERIFYHOST, 2L);
            set(CURLOPT_SSL_CTX_FUNCTION, tls); set(CURLOPT_NOSIGNAL, 1L);
            set(CURLOPT_CONNECTTIMEOUT_MS, 7000L); set(CURLOPT_TIMEOUT_MS, 120000L);
            set(CURLOPT_MAXFILESIZE_LARGE, static_cast<curl_off_t>(MaxTrailerBytes));
            set(CURLOPT_LOW_SPEED_LIMIT, 1024L); set(CURLOPT_LOW_SPEED_TIME, 15L);
            set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
            set(CURLOPT_VERBOSE, 0L); set(CURLOPT_WRITEFUNCTION, receive); set(CURLOPT_WRITEDATA, this);
            if (curl_multi_add_handle(multi, easy) != CURLM_OK) throw std::runtime_error("Trailer download couldn't start");
            phase = State::Loading;
        }
        catch (const std::exception& exception) { networkStop(); phase = State::Failed; error = exception.what(); }
    }
    void decode()
    {
        phase = State::Buffering;
        worker = std::thread([this]
        {
            try
            {
                decodeTrailer(bytes, {
                    [this](TrailerInfo value) { std::lock_guard<std::mutex> lock(mutex); info = value; ready = true; },
                    [this](TrailerVideo frame)
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait(lock, [this] { return cancelled || video.size() < 3; });
                        if (cancelled) return false;
                        videoProduced = true; video.push_back(std::move(frame)); return true;
                    },
                    [this](TrailerAudio pcm)
                    {
                        std::unique_lock<std::mutex> lock(mutex);
                        wake.wait(lock, [this] { return cancelled || audio.size() < 24; });
                        if (cancelled) return false;
                        audio.push_back(std::move(pcm)); return true;
                    }}, cancelled);
            }
            catch (const std::exception& exception)
            { std::lock_guard<std::mutex> lock(mutex); decodeError = exception.what(); }
            catch (...) { std::lock_guard<std::mutex> lock(mutex); decodeError = "Trailer decoder stopped unexpectedly."; }
            std::lock_guard<std::mutex> lock(mutex); done = true;
        });
    }
    void update()
    {
        if (phase == State::Idle || phase == State::Failed || phase == State::Finished) return;
        try
        {
            if (phase == State::Loading)
            {
                int running = 0, remaining = 0;
                if (curl_multi_perform(multi, &running) != CURLM_OK) throw std::runtime_error("Trailer download failed");
                while (auto* message = curl_multi_info_read(multi, &remaining))
                {
                    if (message->msg != CURLMSG_DONE) continue;
                    long status = 0; curl_easy_getinfo(easy, CURLINFO_RESPONSE_CODE, &status);
                    const auto result = message->data.result;
                    printf("Launcher trailer: HTTP=%ld transport=%d bytes=%zu\n", status, int(result), bytes.size());
                    networkStop();
                    if (result == CURLE_FILESIZE_EXCEEDED || bytes.size() >= MaxTrailerBytes)
                        throw std::runtime_error("This trailer exceeds the 96 MiB playback limit.");
                    if (status != 200 || result != CURLE_OK)
                        throw std::runtime_error("Couldn't download the trailer. Check your connection or try another game.");
                    decode(); break;
                }
            }
            if (phase != State::Buffering && phase != State::Playing) return;
            output.release();
            std::unique_lock<std::mutex> lock(mutex);
            if (!decodeError.empty()) throw std::runtime_error(decodeError);
            if (done && !videoProduced) throw std::runtime_error("The trailer didn't contain playable video.");
            if (!started && ready && (!video.empty() || !audio.empty()))
            { started = true; startTime = std::chrono::steady_clock::now(); phase = State::Playing; }
            if (!started)
            { if (done) throw std::runtime_error("The trailer didn't contain playable video."); return; }
            const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - startTime).count();
            while (!video.empty() && video.front().timeUs <= elapsed)
            { lastVideoTime = video.front().timeUs; pending = std::move(video.front().image); video.pop_front(); }
            while (!audio.empty() && audio.front().timeUs <= elapsed + 80000 && output.available())
            {
                auto pcm = std::move(audio.front()); audio.pop_front();
                lock.unlock(); output.submit(pcm); lock.lock();
            }
            wake.notify_all();
            const bool finished = done && video.empty() && audio.empty();
            lock.unlock(); output.play();
            if (finished && !pending && !output.pending() && elapsed > lastVideoTime + 100000) phase = State::Finished;
        }
        catch (const std::exception& exception)
        {
            phase = State::Failed; error = exception.what();
            { std::lock_guard<std::mutex> lock(mutex); cancelled = true; }
            wake.notify_all(); output.stop();
        }
    }
};
TrailerPlayer::TrailerPlayer(TlsSetup setup) : impl_(std::make_unique<Impl>(setup)) {}
TrailerPlayer::~TrailerPlayer() = default;
void TrailerPlayer::start(const std::string& url) { impl_->start(url); }
void TrailerPlayer::update() { impl_->update(); }
void TrailerPlayer::stop() { impl_->stop(); }
TrailerPlayer::State TrailerPlayer::state() const { return impl_->phase; }
std::string TrailerPlayer::message() const
{
    if (impl_->phase == State::Failed) return impl_->error;
    if (impl_->phase == State::Loading) return "Downloading trailer: " + std::to_string(impl_->bytes.size() / 1024) + " KiB. B returns to game details.";
    return "Preparing the trailer's video and audio. B returns to game details.";
}
std::unique_ptr<RgbaImage> TrailerPlayer::takeFrame() { return std::move(impl_->pending); }
}
