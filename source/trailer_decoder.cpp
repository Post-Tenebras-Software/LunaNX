#include "trailer_decoder.hpp"
extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
#include <libswresample/swresample.h>
}
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <stdexcept>

namespace luna::launcher
{
namespace
{
constexpr std::int64_t MaxDuration = 10 * 60 * AV_TIME_BASE;
void check(bool condition, const char* message)
{ if (!condition) throw std::runtime_error(message); }
struct Memory
{
    const std::vector<unsigned char>& bytes;
    std::int64_t position = 0;
    static int read(void* opaque, unsigned char* destination, int size)
    {
        auto& self = *static_cast<Memory*>(opaque);
        const auto count = std::min<std::int64_t>(size, self.bytes.size() - self.position);
        if (count <= 0) return AVERROR_EOF;
        std::memcpy(destination, self.bytes.data() + self.position, count);
        self.position += count;
        return int(count);
    }
    static std::int64_t seek(void* opaque, std::int64_t offset, int mode)
    {
        auto& self = *static_cast<Memory*>(opaque);
        if (mode == AVSEEK_SIZE) return self.bytes.size();
        mode &= ~AVSEEK_FORCE;
        std::int64_t base = mode == SEEK_SET ? 0 : mode == SEEK_CUR ? self.position :
            mode == SEEK_END ? std::int64_t(self.bytes.size()) : -1;
        if (base < 0 || offset < -base || offset > std::int64_t(self.bytes.size()) - base) return AVERROR(EINVAL);
        self.position = base + offset;
        return self.position;
    }
};
struct Resources
{
    AVFormatContext* format = nullptr;
    AVIOContext* io = nullptr;
    AVCodecContext *video = nullptr, *audio = nullptr;
    AVFrame *frame = nullptr, *download = nullptr;
    AVPacket* packet = nullptr;
    SwsContext* scale = nullptr;
    SwrContext* resample = nullptr;
    ~Resources()
    {
        avcodec_free_context(&video); avcodec_free_context(&audio);
        av_frame_free(&frame); av_frame_free(&download); av_packet_free(&packet);
        sws_freeContext(scale); swr_free(&resample);
        avformat_close_input(&format);
        if (io) av_freep(&io->buffer);
        avio_context_free(&io);
    }
};
std::int64_t timestamp(const AVFrame* frame, AVRational base, std::int64_t origin, std::int64_t fallback)
{
    const auto value = frame->best_effort_timestamp == AV_NOPTS_VALUE ? fallback :
        av_rescale_q(frame->best_effort_timestamp, base, AVRational{1, AV_TIME_BASE}) - origin;
    check(value >= -5 * AV_TIME_BASE && value <= MaxDuration + 5 * AV_TIME_BASE, "Unsupported trailer timestamps");
    return std::max<std::int64_t>(0, value);
}
}
void decodeTrailer(const std::vector<unsigned char>& bytes, const TrailerCallbacks& callbacks,
    std::atomic<bool>& cancelled, bool hardware)
{
    if (cancelled) return;
    check(bytes.size() >= 16 && bytes.size() <= MaxTrailerBytes, "Invalid trailer size");
    // MP4 only, not HLS, playlists, DRM manifests or external references.
    check(!std::memcmp(bytes.data() + 4, "ftyp", 4), "This trailer is not a supported MP4");
    Resources r;
    Memory memory{bytes};
    r.format = avformat_alloc_context();
    check(r.format != nullptr, "Trailer allocation failed");
    auto* buffer = static_cast<unsigned char*>(av_malloc(32768));
    check(buffer != nullptr, "Trailer allocation failed");
    r.io = avio_alloc_context(buffer, 32768, 0, &memory, Memory::read, nullptr, Memory::seek);
    if (!r.io) { av_free(buffer); throw std::runtime_error("Trailer allocation failed"); }
    r.format->pb = r.io; r.format->flags |= AVFMT_FLAG_CUSTOM_IO;
    r.format->probesize = 1024 * 1024;
    r.format->max_analyze_duration = 5 * AV_TIME_BASE;
    r.format->max_streams = 8;
    r.format->interrupt_callback = {[](void* p) { return static_cast<std::atomic<bool>*>(p)->load() ? 1 : 0; }, &cancelled};
    AVDictionary* options = nullptr;
    av_dict_set(&options, "enable_drefs", "0", 0);
    av_dict_set(&options, "use_absolute_path", "0", 0);
    av_dict_set(&options, "protocol_whitelist", "", 0);
    const int opened = avformat_open_input(&r.format, nullptr, av_find_input_format("mov"), &options);
    av_dict_free(&options);
    check(opened >= 0, "Couldn't read the trailer container");
    check(avformat_find_stream_info(r.format, nullptr) >= 0 && r.format->nb_streams <= 8, "Couldn't read trailer streams");
    check(r.format->duration == AV_NOPTS_VALUE || r.format->duration <= MaxDuration, "Trailer is longer than ten minutes");
    const int video = av_find_best_stream(r.format, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    const int audio = av_find_best_stream(r.format, AVMEDIA_TYPE_AUDIO, -1, -1, nullptr, 0);
    check(video >= 0, "This trailer has no video");
    const auto* videoStream = r.format->streams[video];
    check(videoStream->codecpar->codec_id == AV_CODEC_ID_H264 && videoStream->codecpar->width > 0 &&
        videoStream->codecpar->width <= 1920 && videoStream->codecpar->height > 0 && videoStream->codecpar->height <= 1088,
        "Trailer requires H.264 video up to 1080p");
    const auto* codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    check(codec != nullptr, "Trailer video decoder unavailable");
    r.video = avcodec_alloc_context3(codec);
    check(r.video && avcodec_parameters_to_context(r.video, videoStream->codecpar) >= 0, "Trailer video setup failed");
    r.video->max_pixels = 1920 * 1088; r.video->thread_count = 1;
    r.video->pkt_timebase = videoStream->time_base;
    if (hardware)
    {
        const auto device = av_hwdevice_find_type_by_name("nvtegra");
        if (device != AV_HWDEVICE_TYPE_NONE &&
            av_hwdevice_ctx_create(&r.video->hw_device_ctx, device, nullptr, nullptr, 0) >= 0)
        {
            r.video->extra_hw_frames = 2;
            r.video->get_format = [](AVCodecContext* c, const AVPixelFormat* formats)
            {
                const auto deviceType = static_cast<AVHWDeviceContext*>(static_cast<void*>(c->hw_device_ctx->data))->type;
                for (int i = 0; const auto* config = avcodec_get_hw_config(c->codec, i); ++i)
                    if (config->device_type == deviceType)
                        for (const auto* f = formats; *f != AV_PIX_FMT_NONE; ++f)
                            if (*f == config->pix_fmt) return *f;
                for (const auto* f = formats; *f != AV_PIX_FMT_NONE; ++f)
                { const auto* desc = av_pix_fmt_desc_get(*f); if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *f; }
                return AV_PIX_FMT_NONE;
            };
        }
    }
    check(avcodec_open2(r.video, codec, nullptr) >= 0, "Trailer video decoder failed");
    if (audio >= 0)
    {
        const auto* stream = r.format->streams[audio];
        const auto id = stream->codecpar->codec_id;
        check(id == AV_CODEC_ID_AAC || id == AV_CODEC_ID_MP3 || id == AV_CODEC_ID_OPUS, "Trailer audio format is unsupported");
        check(stream->codecpar->sample_rate >= 8000 && stream->codecpar->sample_rate <= 192000 &&
            stream->codecpar->ch_layout.nb_channels > 0 && stream->codecpar->ch_layout.nb_channels <= 8, "Invalid trailer audio format");
        codec = avcodec_find_decoder(id);
        check(codec != nullptr, "Trailer audio decoder unavailable");
        r.audio = avcodec_alloc_context3(codec);
        check(r.audio && avcodec_parameters_to_context(r.audio, stream->codecpar) >= 0, "Trailer audio setup failed");
        r.audio->thread_count = 1; r.audio->pkt_timebase = stream->time_base;
        check(avcodec_open2(r.audio, codec, nullptr) >= 0, "Trailer audio decoder failed");
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        check(swr_alloc_set_opts2(&r.resample, &stereo, AV_SAMPLE_FMT_S16, 48000, &r.audio->ch_layout,
            r.audio->sample_fmt, r.audio->sample_rate, 0, nullptr) >= 0 && swr_init(r.resample) >= 0, "Trailer audio conversion failed");
    }
    if (callbacks.info) callbacks.info({std::max<std::int64_t>(0, r.format->duration), audio >= 0});
    r.packet = av_packet_alloc(); r.frame = av_frame_alloc(); r.download = av_frame_alloc();
    check(r.packet && r.frame && r.download, "Trailer frame allocation failed");
    const auto origin = r.format->start_time == AV_NOPTS_VALUE ? 0 : r.format->start_time;
    std::int64_t videoTime = 0, audioTime = 0;
    const auto rate = av_guess_frame_rate(r.format, r.format->streams[video], nullptr);
    const auto frameDuration = rate.num > 0 && rate.den > 0 ?
        std::clamp<std::int64_t>(av_rescale(rate.den, AV_TIME_BASE, rate.num), 16666, 200000) : 33333;
    const auto receive = [&](AVCodecContext* context, bool isVideo)
    {
        while (!cancelled)
        {
            const int received = avcodec_receive_frame(context, r.frame);
            if (received == AVERROR(EAGAIN) || received == AVERROR_EOF) return true;
            check(received >= 0, "Trailer frame decode failed");
            bool keepGoing = true;
            if (isVideo)
            {
                const AVFrame* frame = r.frame;
                const auto* description = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame->format));
                if (description && (description->flags & AV_PIX_FMT_FLAG_HWACCEL))
                {
                    av_frame_unref(r.download);
                    check(av_hwframe_transfer_data(r.download, frame, 0) >= 0, "Trailer hardware transfer failed");
                    frame = r.download;
                }
                check(frame->width > 0 && frame->height > 0 && frame->width <= 1920 && frame->height <= 1088, "Trailer dimensions changed beyond bounds");
                const double scale = std::min({1.0, 1280.0 / frame->width, 720.0 / frame->height});
                auto image = std::make_unique<RgbaImage>();
                image->width = std::max(1, int(frame->width * scale)); image->height = std::max(1, int(frame->height * scale));
                image->stride = image->width * 4; image->pixels.resize(image->stride * image->height);
                r.scale = sws_getCachedContext(r.scale, frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                    image->width, image->height, AV_PIX_FMT_RGBA, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);
                unsigned char* output[] = {image->pixels.data(), nullptr, nullptr, nullptr};
                int strides[] = {image->stride, 0, 0, 0};
                check(r.scale && sws_scale(r.scale, frame->data, frame->linesize, 0, frame->height, output, strides) == image->height, "Trailer video conversion failed");
                const auto time = timestamp(r.frame, videoStream->time_base, origin, videoTime);
                videoTime = time + frameDuration;
                if (callbacks.video) keepGoing = callbacks.video({std::move(image), time});
            }
            else
            {
                check(r.frame->nb_samples > 0 && r.frame->nb_samples <= MaxTrailerAudioSamples &&
                    r.frame->format == r.audio->sample_fmt && r.frame->sample_rate == r.audio->sample_rate &&
                    av_channel_layout_compare(&r.frame->ch_layout, &r.audio->ch_layout) == 0, "Trailer audio frame exceeds bounds");
                TrailerAudio output;
                output.timeUs = timestamp(r.frame, r.format->streams[audio]->time_base, origin, audioTime);
                const int capacity = av_rescale_rnd(swr_get_delay(r.resample, r.audio->sample_rate) + r.frame->nb_samples,
                    48000, r.audio->sample_rate, AV_ROUND_UP);
                check(capacity > 0 && capacity <= MaxTrailerAudioSamples, "Trailer resampled audio exceeds bounds");
                output.samples.resize(capacity * 2);
                auto* data = reinterpret_cast<unsigned char*>(output.samples.data());
                const int count = swr_convert(r.resample, &data, capacity, const_cast<const unsigned char**>(r.frame->extended_data), r.frame->nb_samples);
                check(count >= 0, "Trailer audio conversion failed");
                output.samples.resize(count * 2); audioTime = output.timeUs + std::int64_t(count) * AV_TIME_BASE / 48000;
                if (count && callbacks.audio) keepGoing = callbacks.audio(std::move(output));
            }
            av_frame_unref(r.frame);
            if (!keepGoing) return false;
        }
        return false;
    };
    const auto send = [&](AVCodecContext* context, const AVPacket* packet, bool videoPacket)
    {
        int result = avcodec_send_packet(context, packet);
        if (result == AVERROR(EAGAIN)) { if (!receive(context, videoPacket)) return false; result = avcodec_send_packet(context, packet); }
        check(result >= 0 || result == AVERROR_EOF, "Trailer packet decode failed");
        return receive(context, videoPacket);
    };
    unsigned packets = 0;
    while (!cancelled)
    {
        const int result = av_read_frame(r.format, r.packet);
        if (result == AVERROR_EOF) break;
        check(result >= 0 && ++packets <= 200000 && r.packet->size <= 4 * 1024 * 1024, "Invalid trailer packets");
        bool keepGoing = true;
        if (r.packet->stream_index == video) keepGoing = send(r.video, r.packet, true);
        else if (r.packet->stream_index == audio) keepGoing = send(r.audio, r.packet, false);
        av_packet_unref(r.packet);
        if (!keepGoing) return;
    }
    if (!cancelled && send(r.video, nullptr, true) && r.audio) send(r.audio, nullptr, false);
}
}
