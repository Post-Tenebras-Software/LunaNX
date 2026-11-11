#include "launcher_artwork.hpp"
#include "launcher_media.hpp"
#include "launcher_image_cache.hpp"
extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}
#include <map>
#include <set>
#include <stdexcept>
#include <cstring>
#include <cstdio>

namespace luna::launcher
{
namespace
{
constexpr std::size_t MaxImageBytes = ImageDiskCache::MaxEntry;
Image decode(const std::vector<unsigned char>& bytes)
{
    // Only PNG and JPEG; no containers, external resources or animated media.
    AVCodecID id = AV_CODEC_ID_NONE;
    if (bytes.size() >= 8 && !std::memcmp(bytes.data(), "\x89PNG\r\n\x1a\n", 8)) id = AV_CODEC_ID_PNG;
    if (bytes.size() >= 3 && bytes[0] == 0xff && bytes[1] == 0xd8 && bytes[2] == 0xff) id = AV_CODEC_ID_MJPEG;
    if (id == AV_CODEC_ID_NONE) throw std::runtime_error("Unsupported artwork");
    auto* codec = avcodec_find_decoder(id);
    std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)> context(avcodec_alloc_context3(codec),
        [](AVCodecContext* p) { avcodec_free_context(&p); });
    std::unique_ptr<AVPacket, void(*)(AVPacket*)> packet(av_packet_alloc(), [](AVPacket* p) { av_packet_free(&p); });
    std::unique_ptr<AVFrame, void(*)(AVFrame*)> frame(av_frame_alloc(), [](AVFrame* p) { av_frame_free(&p); });
    if (!codec || !context || !packet || !frame) throw std::runtime_error("Artwork decoder allocation");
    context->thread_count = 1;
    context->max_pixels = 8 * 1024 * 1024;
    if (avcodec_open2(context.get(), codec, nullptr) < 0 || av_new_packet(packet.get(), bytes.size()) < 0)
        throw std::runtime_error("Artwork decoder setup");
    std::memcpy(packet->data, bytes.data(), bytes.size());
    if (avcodec_send_packet(context.get(), packet.get()) < 0 || avcodec_receive_frame(context.get(), frame.get()) < 0 ||
        frame->width <= 0 || frame->height <= 0 || frame->width > 4096 || frame->height > 4096)
        throw std::runtime_error("Artwork decode failed");
    const double scale = std::min({1.0, 960.0 / frame->width, 540.0 / frame->height});
    Image image{std::max(1, int(frame->width * scale)), std::max(1, int(frame->height * scale)), {}};
    image.pixels.resize(image.width * image.height);
    std::unique_ptr<SwsContext, void(*)(SwsContext*)> converter(sws_getContext(frame->width, frame->height,
        static_cast<AVPixelFormat>(frame->format), image.width, image.height, AV_PIX_FMT_RGBA,
        SWS_FAST_BILINEAR, nullptr, nullptr, nullptr), sws_freeContext);
    unsigned char* output[] = {reinterpret_cast<unsigned char*>(image.pixels.data()), nullptr, nullptr, nullptr};
    int strides[] = {image.width * 4, 0, 0, 0};
    if (!converter || sws_scale(converter.get(), frame->data, frame->linesize, 0, frame->height, output, strides) != image.height)
        throw std::runtime_error("Artwork conversion failed");
    return image;
}
}
struct ArtworkCache::Impl
{
    TlsSetup tls;
    CURLM* multi = nullptr;
    CURL* easy = nullptr;
    struct Entry { Image image; std::uint64_t used; };
    std::map<std::string, Entry> entries;
    std::set<std::string> settled;
    ImageDiskCache disk{"sdmc:/switch/LunaNX/cache/artwork"};
    std::string active;
    std::vector<unsigned char> bytes;
    std::uint64_t clock = 0, revision = 0;
    explicit Impl(TlsSetup setup) : tls(setup) { transport(); }
    ~Impl() { cancel(); if (multi) curl_multi_cleanup(multi); }
    void transport()
    {
        if (multi) return;
        multi = curl_multi_init();
        if (!multi) throw std::runtime_error("Artwork transport unavailable");
        curl_multi_setopt(multi, CURLMOPT_MAXCONNECTS, 1L);
    }
    void cancel()
    {
        if (easy) { curl_multi_remove_handle(multi, easy); curl_easy_cleanup(easy); easy = nullptr; }
        active.clear(); bytes.clear();
    }
    template<typename T> void set(CURLoption option, T value)
    { if (curl_easy_setopt(easy, option, value) != CURLE_OK) throw std::runtime_error("Artwork option unavailable"); }
    static std::size_t receive(char* data, std::size_t size, std::size_t count, void* context)
    {
        auto& self = *static_cast<Impl*>(context);
        if (size && count > (MaxImageBytes - self.bytes.size()) / size) return 0;
        try { self.bytes.insert(self.bytes.end(), data, data + size * count); return size * count; }
        catch (...) { return 0; }
    }
    void store(const std::string& url, Image image)
    {
        // Twelve decoded images (~24 MiB maximum), including negative cache
        // entries. Repeated failures cannot flood requests every UI frame.
        if (entries.size() >= 12)
            entries.erase(std::min_element(entries.begin(), entries.end(),
                [](const auto& a, const auto& b) { return a.second.used < b.second.used; }));
        entries[url] = {std::move(image), ++clock}; ++revision;
    }
    void settle(const std::string& url)
    {
        if (settled.size() >= 512) settled.erase(settled.begin());
        settled.insert(url);
    }
    void start(const std::string& url)
    {
        transport();
        active = url; easy = curl_easy_init();
        if (!easy) throw std::runtime_error("Artwork HTTP allocation");
        const auto requestUrl = artworkRequestUrl(active);
        set(CURLOPT_URL, requestUrl.c_str());
        set(CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS));
        set(CURLOPT_FOLLOWLOCATION, 0L);
        set(CURLOPT_SSL_VERIFYPEER, 1L); set(CURLOPT_SSL_VERIFYHOST, 2L);
        set(CURLOPT_SSL_CTX_FUNCTION, tls);
        set(CURLOPT_NOSIGNAL, 1L); set(CURLOPT_CONNECTTIMEOUT_MS, 4000L); set(CURLOPT_TIMEOUT_MS, 15000L);
        set(CURLOPT_HTTP_VERSION, static_cast<long>(CURL_HTTP_VERSION_1_1));
        // Reuse the single verified image connection across successive covers.
        // The entire connection pool is closed before starting WebRTC.
        set(CURLOPT_VERBOSE, 0L);
        set(CURLOPT_WRITEFUNCTION, receive); set(CURLOPT_WRITEDATA, this);
        if (curl_multi_add_handle(multi, easy) != CURLM_OK) throw std::runtime_error("Artwork dispatch");
    }
};
ArtworkCache::ArtworkCache(TlsSetup setup) : impl_(std::make_unique<Impl>(setup)) {}
ArtworkCache::~ArtworkCache() = default;
const Image* ArtworkCache::find(const std::string& url)
{
    const auto found = impl_->entries.find(url);
    if (found == impl_->entries.end() || found->second.image.pixels.empty()) return nullptr;
    found->second.used = ++impl_->clock;
    return &found->second.image;
}
void ArtworkCache::cancel() { impl_->cancel(); }
void ArtworkCache::clear()
{
    impl_->cancel(); impl_->entries.clear(); impl_->settled.clear(); impl_->bytes.shrink_to_fit();
    if (impl_->multi) { curl_multi_cleanup(impl_->multi); impl_->multi = nullptr; }
    ++impl_->revision;
}
std::uint64_t ArtworkCache::revision() const { return impl_->revision; }
void ArtworkCache::update(const std::vector<std::string>& wanted, const std::vector<std::string>& prefetch, bool mayStart)
{
    auto& state = *impl_;
    try
    {
        const auto visible = [&](const std::string& url) { return std::find(wanted.begin(), wanted.end(), url) != wanted.end(); };
        if (state.easy && !visible(state.active) && std::find(prefetch.begin(), prefetch.end(), state.active) == prefetch.end()) state.cancel();
        if (state.easy)
        {
            int running = 0;
            if (curl_multi_perform(state.multi, &running) != CURLM_OK) throw std::runtime_error("Artwork transport");
            int remaining = 0;
            while (auto* message = curl_multi_info_read(state.multi, &remaining))
            {
                if (message->msg != CURLMSG_DONE) continue;
                long status = 0; curl_easy_getinfo(state.easy, CURLINFO_RESPONSE_CODE, &status);
                Image image;
                if (message->data.result == CURLE_OK && status == 200)
                    try { image = decode(state.bytes); } catch (...) {}
                printf("Launcher artwork: source=network HTTP=%ld decoded=%d bytes=%zu visible=%d\n",
                    status, !image.pixels.empty(), state.bytes.size(), visible(state.active));
                if (!image.pixels.empty()) state.disk.save(artworkRequestUrl(state.active), state.bytes);
                state.settle(state.active);
                if (visible(state.active)) state.store(state.active, std::move(image));
                state.cancel();
            }
        }
        if (!state.easy)
        {
            for (const auto& url : wanted)
                if (artworkUrl(url) && !state.entries.count(url))
                {
                    const auto bytes = state.disk.load(artworkRequestUrl(url));
                    Image image;
                    if (!bytes.empty()) try { image = decode(bytes); } catch (...) {}
                    if (!image.pixels.empty())
                    {
                        printf("Launcher artwork: source=disk decoded=1 bytes=%zu\n", bytes.size());
                        state.store(url, std::move(image)); return;
                    }
                    if (mayStart) state.start(url);
                    return; // Visible images always take priority over prefetch.
                }
            if (mayStart)
                for (const auto& url : prefetch)
                    if (artworkUrl(url) && !state.entries.count(url) && !state.settled.count(url))
                    {
                        if (!state.disk.load(artworkRequestUrl(url)).empty()) state.settle(url);
                        else state.start(url);
                        return;
                    }
        }
    }
    catch (...) { if (!state.active.empty()) { state.settle(state.active); state.store(state.active, {}); } state.cancel(); }
}
}
