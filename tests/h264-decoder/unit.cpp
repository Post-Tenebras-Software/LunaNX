#include "h264_decoder.hpp"
extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}
#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>

struct SwsContext { int width, height; };
namespace
{
unsigned checks = 0;
void require(bool condition, const char* description)
{ ++checks; if (!condition) throw std::runtime_error(description); }
struct Environment
{
    bool device = true, config = true, rejectFormat = false, failOpen = false, transferError = false;
    bool sendError = false, receiveError = false, softwareSendError = false, allocationError = false;
    int width = 1280, height = 720;
    unsigned creates = 0, transfers = 0, allocations = 0, softwarePackets = 0, sends = 0;
    int allocatedHeight = 0;
    std::vector<unsigned char> lastSoftwarePacket;
} env;
struct Context { bool negotiated = false, pending = false, hardware = false; };
std::map<AVCodecContext*, Context> contexts;
std::map<AVBufferRef*, std::function<void()>> buffers;
unsigned char placeholder = 1;
AVBufferRef* buffer(void* data, std::function<void()> release)
{
    auto* ref = new AVBufferRef{}; ref->data = static_cast<unsigned char*>(data);
    buffers.emplace(ref, std::move(release)); return ref;
}
std::vector<std::byte> bytes(std::initializer_list<unsigned char> input)
{
    std::vector<std::byte> out;
    for (auto value : input) out.push_back(std::byte{value});
    return out;
}
const auto Idr = bytes({0,0,0,1,0x67,0x42,0x01,0,0,1,0x68,0x22,0,0,0,1,0x65,0x99});
const auto Delta = bytes({0,0,1,0x41,0x23});
const auto IdrOnly = bytes({0,0,1,0x65,0x99});
void feed(H264Decoder& decoder, const std::vector<std::byte>& frame) { decoder.decode(frame.data(), frame.size()); }
void reset()
{
    require(buffers.empty() && contexts.empty(), "Decoder teardown releases device, frame and codec references");
    env = {};
}
}
extern "C"
{
const AVCodec* avcodec_find_decoder_by_name(const char*) { static AVCodec codec{}; return &codec; }
AVCodecContext* avcodec_alloc_context3(const AVCodec*)
{
    auto* context = new AVCodecContext{};
    contexts.emplace(context, Context{}); return context;
}
void av_buffer_unref(AVBufferRef** ref)
{
    if (!*ref) return;
    const auto found = buffers.find(*ref);
    require(found != buffers.end(), "Every FFmpeg reference is owned exactly once");
    found->second(); buffers.erase(found);
    delete *ref; *ref = nullptr;
}
void avcodec_free_context(AVCodecContext** context)
{
    if (!*context) return;
    av_buffer_unref(&(*context)->hw_device_ctx);
    av_buffer_unref(&(*context)->hw_frames_ctx);
    contexts.erase(*context); delete *context; *context = nullptr;
}
const AVCodecHWConfig* avcodec_get_hw_config(const AVCodec*, int index)
{
    static const AVCodecHWConfig config = {AV_PIX_FMT_NVTEGRA, AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX, AV_HWDEVICE_TYPE_NVTEGRA};
    return env.config && index == 0 ? &config : nullptr;
}
AVHWDeviceType av_hwdevice_find_type_by_name(const char* name)
{ require(std::string(name) == "nvtegra", "Only Switch Tegra hardware is requested"); return AV_HWDEVICE_TYPE_NVTEGRA; }
int av_hwdevice_ctx_create(AVBufferRef** out, AVHWDeviceType, const char*, AVDictionary*, int)
{
    ++env.creates;
    if (!env.device) return AVERROR(ENOSYS);
    out[0] = buffer(&placeholder, [] {}); return 0;
}
int avcodec_open2(AVCodecContext* context, const AVCodec*, AVDictionary**)
{
    require(context->thread_count == 1 && context->thread_type == 0 &&
        context->framerate.num > 0 && context->max_pixels == 1920 * 1088,
        "Decoder retains low-latency threading, bounded pixels and explicit frame rate");
    return env.failOpen && context->hw_device_ctx ? AVERROR_EXTERNAL : 0;
}
int avcodec_send_packet(AVCodecContext* context, const AVPacket* packet)
{
    ++env.sends;
    auto& state = contexts.at(context);
    if (!state.negotiated)
    {
        const AVPixelFormat both[] = {AV_PIX_FMT_NVTEGRA, AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
        const AVPixelFormat software[] = {AV_PIX_FMT_YUV420P, AV_PIX_FMT_NONE};
        auto format = context->get_format(context, context->hw_device_ctx ? both : software);
        if (format == AV_PIX_FMT_NVTEGRA && env.rejectFormat) format = context->get_format(context, software);
        state.hardware = format == AV_PIX_FMT_NVTEGRA; state.negotiated = true;
    }
    if ((state.hardware && env.sendError) || (!state.hardware && env.softwareSendError)) return AVERROR_INVALIDDATA;
    if (!state.hardware)
    {
        ++env.softwarePackets;
        env.lastSoftwarePacket.assign(packet->data, packet->data + packet->size);
    }
    state.pending = true; return 0;
}
int avcodec_receive_frame(AVCodecContext* context, AVFrame* frame)
{
    auto& state = contexts.at(context);
    if (!state.pending) return AVERROR(EAGAIN);
    state.pending = false;
    if (state.hardware && env.receiveError) return AVERROR_EXTERNAL;
    frame->width = env.width; frame->height = env.height; frame->data[0] = &placeholder;
    frame->colorspace = AVCOL_SPC_BT709; frame->color_range = AVCOL_RANGE_MPEG;
    frame->format = state.hardware ? AV_PIX_FMT_NVTEGRA : AV_PIX_FMT_YUV420P;
    if (state.hardware)
    {
        auto* hardware = new AVHWFramesContext{};
        hardware->width = env.width; hardware->height = (env.height + 15) & ~15;
        hardware->sw_format = AV_PIX_FMT_NV12;
        frame->hw_frames_ctx = buffer(hardware, [hardware] { delete hardware; });
    }
    return 0;
}
AVPacket* av_packet_alloc() { return new AVPacket{}; }
void av_packet_unref(AVPacket* packet) { std::free(packet->data); packet->data = nullptr; packet->size = 0; }
void av_packet_free(AVPacket** packet) { if (*packet) { av_packet_unref(*packet); delete *packet; *packet = nullptr; } }
int av_new_packet(AVPacket* packet, int count)
{
    packet->data = static_cast<unsigned char*>(std::calloc(count + AV_INPUT_BUFFER_PADDING_SIZE, 1));
    packet->size = count;
    return packet->data ? 0 : AVERROR(ENOMEM);
}
AVFrame* av_frame_alloc() { auto* frame = new AVFrame{}; frame->format = AV_PIX_FMT_NONE; return frame; }
void av_frame_unref(AVFrame* frame)
{
    for (auto& ref : frame->buf) av_buffer_unref(&ref);
    av_buffer_unref(&frame->hw_frames_ctx);
    *frame = {}; frame->format = AV_PIX_FMT_NONE;
}
void av_frame_free(AVFrame** frame) { if (*frame) { av_frame_unref(*frame); delete *frame; *frame = nullptr; } }
AVBufferRef* av_buffer_aligned_alloc(std::size_t size, std::size_t alignment)
{
    require(alignment == 256, "Hardware readback buffers request VIC-compatible alignment");
    ++env.allocations;
    if (env.allocationError) return nullptr;
    void* data = nullptr;
    if (posix_memalign(&data, alignment, size)) return nullptr;
    auto* ref = buffer(data, [data] { std::free(data); });
    ref->size = size; return ref;
}
int av_image_get_buffer_size(AVPixelFormat format, int width, int height, int alignment)
{
    require(format == AV_PIX_FMT_NV12 && alignment == 256, "Readback requests aligned linear NV12 layout");
    env.allocatedHeight = height;
    return ((width + alignment - 1) & ~(alignment - 1)) * (height + (height + 1) / 2);
}
int av_image_fill_arrays(unsigned char* data[4], int linesize[4], const unsigned char* src,
    AVPixelFormat format, int width, int height, int alignment)
{
    linesize[0] = linesize[1] = (width + alignment - 1) & ~(alignment - 1);
    data[0] = const_cast<unsigned char*>(src); data[1] = data[0] + linesize[0] * height;
    return av_image_get_buffer_size(format, width, height, alignment);
}
int av_hwframe_transfer_data(AVFrame* dst, const AVFrame* src, int)
{
    ++env.transfers;
    require(dst->width == src->width && dst->height == src->height && dst->format == AV_PIX_FMT_NV12,
        "Transfer preserves display dimensions and uses the device's software pixel format");
    for (int i = 0; i < 2; ++i)
        require(!(reinterpret_cast<std::uintptr_t>(dst->data[i]) & 255) && !(dst->linesize[i] & 255),
            "Every downloaded plane and pitch meets VIC alignment");
    return env.transferError ? AVERROR_EXTERNAL : 0;
}
const AVPixFmtDescriptor* av_pix_fmt_desc_get(AVPixelFormat format)
{
    static const AVPixFmtDescriptor hardware = [] { AVPixFmtDescriptor d{}; d.flags = AV_PIX_FMT_FLAG_HWACCEL; return d; }();
    static const AVPixFmtDescriptor software{};
    if (format == AV_PIX_FMT_NONE) return nullptr;
    return format == AV_PIX_FMT_NVTEGRA ? &hardware : &software;
}
const char* av_get_pix_fmt_name(AVPixelFormat format) { return format == AV_PIX_FMT_NV12 ? "nv12" : "yuv420p"; }
int av_strerror(int, char* buffer, std::size_t size) { std::snprintf(buffer, size, "simulated decoder failure"); return 0; }
SwsContext* sws_getCachedContext(SwsContext* context, int, int, AVPixelFormat format, int width, int height,
    AVPixelFormat output, int, SwsFilter*, SwsFilter*, const double*)
{
    require(format != AV_PIX_FMT_NVTEGRA && output == AV_PIX_FMT_RGBA, "Only linear frames reach swscale");
    if (!context) context = new SwsContext{};
    context->width = width; context->height = height; return context;
}
void sws_freeContext(SwsContext* context) { delete context; }
const int* sws_getCoefficients(int colorspace)
{ require(colorspace == AVCOL_SPC_BT709, "Downloaded frames retain source color metadata"); static const int coefficients[4]{}; return coefficients; }
int sws_setColorspaceDetails(SwsContext*, const int*, int sourceRange, const int*, int destRange, int, int, int)
{ require(!sourceRange && destRange == 1, "Limited-range video is converted to full-range display RGBA"); return 0; }
int sws_scale(SwsContext* context, const unsigned char* const[], const int[], int, int,
    unsigned char* const dst[], const int strides[])
{
    for (int y = 0; y < context->height; ++y) std::memset(dst[0] + y * strides[0], 0x7f, context->width * 4);
    return context->height;
}
}

int main()
{
    H264Recovery sets;
    require(!sets.observe(Delta.data(), Delta.size()) && !sets.ready(), "Delta frames cannot initialize recovery");
    require(sets.observe(Idr.data(), Idr.size()) && sets.ready(), "Both Annex-B prefix sizes and mixed parameter/IDR NALs are recognized");
    require(sets.parameters().size() == 13 && sets.parameters()[3] == std::byte{1},
        "Cached SPS/PPS are normalized to four-byte start codes");
    auto oversized = std::vector<std::byte>(H264Recovery::MaxParameterSet + 1, std::byte{0x22});
    oversized[0] = oversized[1] = oversized[2] = std::byte{0}; oversized[3] = std::byte{1}; oversized[4] = std::byte{0x67};
    sets.observe(oversized.data(), oversized.size());
    require(!sets.ready(), "Oversized replacement SPS invalidates the stale cache");
    require(!sets.observe(nullptr, 10) && !sets.observe(Idr.data(), H264Recovery::MaxAccessUnit + 1),
        "Invalid/oversized recovery inputs are bounded before scanning");
    {
        H264Decoder decoder(1280, 720, luna::video::Decoder::Software);
        feed(decoder, Idr); const auto image = decoder.takeImage();
        require(image && image->width == 1280 && env.creates == 0 && env.transfers == 0,
            "Forced software never creates a hardware device or downloads a hardware frame");
        require(!decoder.hardwarePrepared() && decoder.getStats().backend == luna::video::Backend::Software,
            "Software preflight cannot suppress its warning");
    }
    reset();
    {
        H264Decoder decoder(1920, 1080, luna::video::Decoder::Automatic, 60);
        require(decoder.hardwarePrepared() && decoder.getStats().backend == luna::video::Backend::Software,
            "Creating a hardware device is not reported as successful hardware decoding");
        feed(decoder, Idr); feed(decoder, Delta);
        require(decoder.getStats().backend == luna::video::Backend::Nvdec &&
            decoder.getStats().hardwareFrames == 2 && env.allocations == 1 && env.transfers == 2,
            "Hardware frames are downloaded and the aligned buffer is reused");
        env.width = 1920; env.height = 1080; feed(decoder, Delta);
        const auto image = decoder.takeImage();
        require(env.allocations == 2 && env.allocatedHeight == 1088 && image && image->height == 1080,
            "Resolution changes reallocate coded-height padding while preserving 1080 display rows");
        require(decoder.getStats().pendingReplaced == 2, "Hardware path preserves latest-frame publication");
    }
    reset();
    {
        H264Decoder decoder(1280, 720, luna::video::Decoder::Automatic);
        feed(decoder, Idr); decoder.takeImage();
        env.width = 960; env.height = 540; env.allocationError = true; feed(decoder, Delta);
        require(!decoder.takeImage(), "Failed resize allocation publishes no incomplete frame");
        env.width = 1280; env.height = 720; env.allocationError = false; feed(decoder, Delta);
        require(decoder.takeImage() && env.allocations == 3 && env.transfers == 2,
            "Returning to the previous resolution after allocation failure recreates its buffer");
    }
    reset(); env.device = false;
    {
        H264Decoder decoder(1280, 720, luna::video::Decoder::Automatic);
        feed(decoder, Idr);
        require(!decoder.hardwarePrepared() && decoder.takeImage() && decoder.getStats().fallbacks == 1 && env.transfers == 0,
            "Unavailable hardware initializes a working software decoder");
    }
    reset(); env.config = false;
    {
        H264Decoder decoder(1280, 720, luna::video::Decoder::Automatic);
        feed(decoder, Idr);
        require(!decoder.hardwarePrepared() && decoder.takeImage() && !env.creates,
            "A missing NVDEC codec configuration never opens a device");
    }
    reset(); env.failOpen = true;
    {
        H264Decoder decoder(1280, 720, luna::video::Decoder::Automatic);
        feed(decoder, Idr);
        require(!decoder.hardwarePrepared() && decoder.takeImage() && decoder.getStats().fallbacks == 1,
            "Hardware codec-open failure reopens in software");
    }
    reset(); env.rejectFormat = true;
    {
        H264Decoder decoder(1280, 720, luna::video::Decoder::Automatic);
        feed(decoder, Idr);
        require(!decoder.hardwarePrepared() && decoder.takeImage() && decoder.getStats().backend == luna::video::Backend::Software &&
            decoder.getStats().fallbacks == 1, "FFmpeg's failed hardware format negotiation selects a software format");
    }
    for (int error = 0; error < 3; ++error)
    {
        reset();
        env.transferError = error == 0; env.sendError = error == 1; env.receiveError = error == 2;
        {
            H264Decoder decoder(1280, 720, luna::video::Decoder::Automatic);
            feed(decoder, Idr); feed(decoder, Delta); feed(decoder, Delta);
            require(!decoder.hardwarePrepared() && decoder.getStats().waitingForKeyframe && decoder.getStats().fallbacks == 1,
                "Three hardware failures reopen software and request a fresh keyframe");
            const auto sent = env.sends; feed(decoder, Delta);
            require(env.sends == sent, "Recovery skips reference-dependent frames until an IDR");
            env.softwareSendError = true; feed(decoder, IdrOnly);
            require(decoder.getStats().waitingForKeyframe, "A failed recovery IDR keeps requesting a valid keyframe");
            env.softwareSendError = false; feed(decoder, IdrOnly);
            require(!decoder.getStats().waitingForKeyframe && decoder.takeImage() && env.softwarePackets == 1,
                "A valid IDR resumes software output without restarting the game session");
            require(env.lastSoftwarePacket.size() > IdrOnly.size() && env.lastSoftwarePacket[4] == 0x67 &&
                env.lastSoftwarePacket[11] == 0x68, "Recovery prepends cached SPS/PPS even when the new IDR omits them");
        }
    }
    reset();
    {
        H264Decoder decoder(1280, 720, luna::video::Decoder::Automatic);
        require(decoder.hardwarePrepared() && decoder.getStats().framesDecoded == 0,
            "Decoder initialization may precede remote allocation and produce no frames");
    }
    reset();
    std::cout << "PASS: " << checks << " decoder-wrapper, hardware-transfer, fallback and bounded-IDR recovery checks (simulated FFmpeg).\n";
}
