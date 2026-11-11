#include "h264_decoder.hpp"
#include "video_mode.hpp"

extern "C"
{
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/pixdesc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

#include <cerrno>
#include <algorithm>
#include <climits>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

H264Decoder::H264Decoder(int outputWidth, int outputHeight, luna::video::Decoder decoder, int frameRate) :
    frameRate_(frameRate), outputWidth_(outputWidth), outputHeight_(outputHeight)
{
    luna::video::fitSize(1, 1, outputWidth_, outputHeight_);
    if (frameRate != 30 && frameRate != 60) throw std::invalid_argument("Invalid decoder frame rate");
    packet_ = av_packet_alloc();
    frame_ = av_frame_alloc();
    download_ = av_frame_alloc();
    if (!packet_ || !frame_ || !download_)
    {
        release();
        throw std::runtime_error("Could not allocate H.264 decoder resources.");
    }

    int result = openDecoder(decoder == luna::video::Decoder::Automatic);
    if (result < 0 && hardwareConfigured_)
    {
        reportError("hardware initialization", result);
        ++stats_.fallbacks;
        result = openDecoder(false);
    }
    if (result < 0)
    {
        char message[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(result, message, sizeof(message));
        release();
        throw std::runtime_error(std::string("Could not open H.264 decoder: ") + message);
    }

    printf("H.264 decoder initialized (%s).\n", hardwareConfigured_ ?
        "NVDEC device ready; waiting for hardware frames" : "software");
}

int H264Decoder::openDecoder(bool hardware)
{
    avcodec_free_context(&context_);
    hardwareConfigured_ = hardwareRejected_ = false;
    hardwareFormat_ = -1;
    const auto* codec = avcodec_find_decoder_by_name("h264");
    if (!codec) return AVERROR_DECODER_NOT_FOUND;
    context_ = avcodec_alloc_context3(codec);
    if (!context_) return AVERROR(ENOMEM);
    context_->thread_count = 1;
    context_->thread_type = 0;
    context_->max_pixels = 1920 * 1088; // Allow H.264's coded-height padding for 1080p.
    context_->framerate = {frameRate_, 1};
    context_->pkt_timebase = {1, 90000};
    context_->opaque = this;
    if (hardware)
    {
        const auto type = av_hwdevice_find_type_by_name("nvtegra");
        if (type != AV_HWDEVICE_TYPE_NONE)
            for (int i = 0; const auto* config = avcodec_get_hw_config(codec, i); ++i)
                if (config->device_type == type && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
                { hardwareFormat_ = config->pix_fmt; break; }
        int result = AVERROR(ENOSYS);
        if (hardwareFormat_ >= 0)
            result = av_hwdevice_ctx_create(&context_->hw_device_ctx, type, nullptr, nullptr, 0);
        hardwareConfigured_ = result >= 0;
        if (!hardwareConfigured_)
        {
            av_buffer_unref(&context_->hw_device_ctx);
            ++stats_.fallbacks;
            printf("H.264 NVDEC unavailable: result=%d; using software.\n", result);
        }
        else context_->extra_hw_frames = 2;
    }
    // A device handle alone does not prove hardware output. Negotiate the
    // actual format, and let FFmpeg retry a software format if HW init fails.
    context_->get_format = [](AVCodecContext* context, const AVPixelFormat* formats)
    {
        auto& self = *static_cast<H264Decoder*>(context->opaque);
        if (self.hardwareConfigured_ && !self.hardwareRejected_)
        {
            for (const auto* format = formats; *format != AV_PIX_FMT_NONE; ++format)
                if (*format == self.hardwareFormat_) return *format;
            self.hardwareRejected_ = true;
            { std::lock_guard<std::mutex> lock(self.imageMutex_); ++self.stats_.fallbacks; }
            printf("H.264 NVDEC format rejected; using software.\n");
        }
        for (const auto* format = formats; *format != AV_PIX_FMT_NONE; ++format)
        {
            const auto* desc = av_pix_fmt_desc_get(*format);
            if (desc && !(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *format;
        }
        return AV_PIX_FMT_NONE;
    };
    return avcodec_open2(context_, codec, nullptr);
}

H264Decoder::~H264Decoder()
{
    release();
}

void H264Decoder::release()
{
    sws_freeContext(converter_);
    converter_ = nullptr;
    av_frame_free(&frame_);
    av_frame_free(&download_);
    av_packet_free(&packet_);
    avcodec_free_context(&context_);
}

void H264Decoder::reportError(const char* operation, int result)
{
    ++errors_;
    if (errors_ != 1 && errors_ % 60 != 0)
        return;

    char message[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(result, message, sizeof(message));
    printf("H.264 decoder %s failed: %s (errors: %zu)\n",
        operation, message, errors_);
}

void H264Decoder::decode(const std::byte* data, std::size_t size)
{
    std::lock_guard<std::mutex> lock(decoderMutex_);
    if (size == 0)
        return;
    if (!context_ || !data || size > H264Recovery::MaxAccessUnit)
    {
        reportError("input", AVERROR(EINVAL));
        return;
    }

    const bool idr = recovery_.observe(data, size);
    std::vector<std::byte> parameters;
    {
        std::lock_guard<std::mutex> imageLock(imageMutex_);
        if (stats_.waitingForKeyframe)
        {
            if (!idr || !recovery_.ready()) return;
            parameters = recovery_.parameters();
        }
    }
    // FFmpeg requires zero padding beyond encoded bytes; av_new_packet supplies it.
    av_packet_unref(packet_);
    int result = av_new_packet(packet_, static_cast<int>(parameters.size() + size));
    if (result < 0)
    {
        reportError("packet allocation", result);
        return;
    }
    if (!parameters.empty()) std::memcpy(packet_->data, parameters.data(), parameters.size());
    std::memcpy(packet_->data + parameters.size(), data, size);

    result = sendPacket();
    if (result == AVERROR(EAGAIN))
    {
        receiveFrames();
        result = sendPacket();
    }
    av_packet_unref(packet_);

    if (result < 0)
    {
        reportError("send", result);
        hardwareError();
        if (fallbackPending_) fallbackToSoftware();
        return;
    }

    receiveFrames();
    if (fallbackPending_) fallbackToSoftware();
}

void H264Decoder::hardwareError()
{
    if (hardwareConfigured_ && !hardwareRejected_ && ++hardwareErrors_ >= 3) fallbackPending_ = true;
}
void H264Decoder::fallbackToSoftware()
{
    fallbackPending_ = false;
    av_frame_unref(frame_); av_frame_unref(download_);
    transferWidth_ = transferHeight_ = 0; transferFormat_ = -1;
    const int result = openDecoder(false);
    {
        std::lock_guard<std::mutex> lock(imageMutex_);
        ++stats_.fallbacks;
        stats_.backend = luna::video::Backend::Software;
        stats_.waitingForKeyframe = true;
        image_.reset();
    }
    if (result < 0)
    {
        avcodec_free_context(&context_);
        reportError("software recovery initialization", result);
    }
    printf("H.264 hardware recovery: software decoder; waiting for a fresh IDR.\n");
}

int H264Decoder::sendPacket()
{
    const auto started = std::chrono::steady_clock::now();
    const int result = avcodec_send_packet(context_, packet_);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    std::lock_guard<std::mutex> lock(imageMutex_);
    stats_.decodeTime += std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
    return result;
}

std::unique_ptr<RgbaImage> H264Decoder::takeImage()
{
    std::lock_guard<std::mutex> lock(imageMutex_);
    return std::move(image_);
}

H264Decoder::Stats H264Decoder::getStats()
{
    std::lock_guard<std::mutex> lock(imageMutex_);
    return stats_;
}

bool H264Decoder::hardwarePrepared()
{
    std::lock_guard<std::mutex> lock(decoderMutex_);
    return hardwareConfigured_ && !hardwareRejected_;
}

AVFrame* H264Decoder::downloadFrame()
{
    const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame_->format));
    if (!desc) return nullptr;
    if (!(desc->flags & AV_PIX_FMT_FLAG_HWACCEL)) return frame_;
    if (!frame_->hw_frames_ctx) return nullptr;
    const auto* frames = reinterpret_cast<const AVHWFramesContext*>(frame_->hw_frames_ctx->data);
    const auto* sw = av_pix_fmt_desc_get(frames->sw_format);
    if (!sw || (sw->flags & AV_PIX_FMT_FLAG_HWACCEL) || frames->width < frame_->width ||
        frames->height < frame_->height || frames->width > 1920 || frames->height > 1088) return nullptr;
    int result = 0;
    if (transferWidth_ != frames->width || transferHeight_ != frames->height || transferFormat_ != frames->sw_format)
    {
        av_frame_unref(download_);
        transferWidth_ = transferHeight_ = 0; transferFormat_ = -1;
        download_->format = frames->sw_format;
        download_->width = frames->width; download_->height = frames->height;
        // Align both allocation and pitches for VIC. av_frame_get_buffer's
        // alignment argument only guarantees pitches, not the base address.
        // Extra rows and zero padding also accommodate swscale's SIMD reads.
        const int paddedHeight = (frames->height + 31) & ~31;
        result = av_image_get_buffer_size(frames->sw_format, frames->width, paddedHeight, 256);
        if (result < 0) { reportError("hardware transfer layout", result); return nullptr; }
        const auto bytes = static_cast<std::size_t>(result) + AV_INPUT_BUFFER_PADDING_SIZE;
        download_->buf[0] = av_buffer_aligned_alloc(bytes, 256);
        if (!download_->buf[0]) { reportError("hardware transfer allocation", AVERROR(ENOMEM)); return nullptr; }
        std::memset(download_->buf[0]->data, 0, bytes);
        result = av_image_fill_arrays(download_->data, download_->linesize, download_->buf[0]->data,
            frames->sw_format, frames->width, paddedHeight, 256);
        if (result < 0) { reportError("hardware transfer allocation", result); return nullptr; }
        download_->extended_data = download_->data;
        transferWidth_ = frames->width; transferHeight_ = frames->height; transferFormat_ = frames->sw_format;
        printf("H.264 transfer buffer: %dx%d, alignment=256.\n", transferWidth_, transferHeight_);
    }
    // This private buffer is never shared; conversion completes before reuse.
    download_->width = frame_->width; download_->height = frame_->height;
    const auto started = std::chrono::steady_clock::now();
    result = av_hwframe_transfer_data(download_, frame_, 0);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    {
        std::lock_guard<std::mutex> lock(imageMutex_);
        ++stats_.transferAttempts;
        stats_.transferTime += std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed);
    }
    if (result < 0) { reportError("hardware frame transfer", result); return nullptr; }
    download_->colorspace = frame_->colorspace;
    download_->color_range = frame_->color_range;
    return download_;
}

std::unique_ptr<RgbaImage> H264Decoder::convertFrame(const AVFrame* frame)
{
    auto image = std::make_unique<RgbaImage>();
    const auto fitted = luna::video::fitSize(frame->width, frame->height, outputWidth_, outputHeight_);
    image->width = fitted.width;
    image->height = fitted.height;

    converter_ = sws_getCachedContext(converter_, frame->width, frame->height,
        static_cast<AVPixelFormat>(frame->format),
        image->width, image->height, AV_PIX_FMT_RGBA, SWS_BILINEAR,
        nullptr, nullptr, nullptr);
    if (!converter_)
    {
        reportError("RGBA conversion setup", AVERROR(EINVAL));
        return nullptr;
    }

    const int* coefficients = sws_getCoefficients(frame->colorspace);
    const int result = sws_setColorspaceDetails(converter_, coefficients,
        frame->color_range == AVCOL_RANGE_JPEG, coefficients, 1, 0, 1 << 16, 1 << 16);
    if (result < 0)
    {
        reportError("color space setup", result);
        return nullptr;
    }

    image->stride = (image->width * 4 + 31) & ~31;
    image->pixels.resize(static_cast<std::size_t>(image->stride) * image->height +
        AV_INPUT_BUFFER_PADDING_SIZE);
    std::uint8_t* output[] = {image->pixels.data(), nullptr, nullptr, nullptr};
    const int strides[] = {image->stride, 0, 0, 0};
    const int rows = sws_scale(converter_, frame->data, frame->linesize,
        0, frame->height, output, strides);
    if (rows != image->height)
    {
        reportError("RGBA conversion", rows < 0 ? rows : AVERROR_INVALIDDATA);
        return nullptr;
    }

    return image;
}

void H264Decoder::receiveFrames()
{
    while (true)
    {
        const auto decodeStarted = std::chrono::steady_clock::now();
        const int result = avcodec_receive_frame(context_, frame_);
        const auto decodeElapsed = std::chrono::steady_clock::now() - decodeStarted;
        {
            std::lock_guard<std::mutex> lock(imageMutex_);
            stats_.decodeTime +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(decodeElapsed);
            if (result >= 0)
                ++stats_.framesDecoded;
        }
        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF)
            return;
        if (result < 0)
        {
            reportError("receive", result);
            hardwareError();
            return;
        }

        if (frame_->width <= 0 || frame_->height <= 0 || frame_->width > RgbaImage::MaxWidth ||
            frame_->height > RgbaImage::MaxHeight || !frame_->data[0] ||
            (frame_->flags & AV_FRAME_FLAG_CORRUPT) || frame_->decode_error_flags != 0)
        {
            reportError("frame validation", AVERROR_INVALIDDATA);
            hardwareError();
            av_frame_unref(frame_);
            if (fallbackPending_) return;
            continue;
        }

        const auto* desc = av_pix_fmt_desc_get(static_cast<AVPixelFormat>(frame_->format));
        const bool hardware = desc && (desc->flags & AV_PIX_FMT_FLAG_HWACCEL);
        auto* linear = downloadFrame();
        if (!linear)
        {
            hardwareError(); av_frame_unref(frame_);
            if (fallbackPending_) return;
            continue;
        }
        {
            std::lock_guard<std::mutex> lock(imageMutex_);
            const auto backend = hardware ? luna::video::Backend::Nvdec : luna::video::Backend::Software;
            if (stats_.backend != backend) printf("H.264 decoder backend active: %s.\n", luna::video::backendLabel(backend));
            stats_.backend = backend;
            if (hardware) ++stats_.hardwareFrames;
        }
        const char* format = av_get_pix_fmt_name(static_cast<AVPixelFormat>(linear->format));
        const int width = frame_->width;
        const int height = frame_->height;
        if (width != loggedWidth_ || height != loggedHeight_ ||
            linear->format != loggedPixelFormat_)
        {
            printf("H.264 decoded format: %dx%d, %s\n",
                width, height, format ? format : "unknown");
            loggedWidth_ = width;
            loggedHeight_ = height;
            loggedPixelFormat_ = linear->format;
        }

        const auto conversionStarted = std::chrono::steady_clock::now();
        auto image = convertFrame(linear);
        const auto conversionElapsed = std::chrono::steady_clock::now() - conversionStarted;
        av_frame_unref(frame_);

        std::size_t converted = 0;
        {
            std::lock_guard<std::mutex> lock(imageMutex_);
            ++stats_.conversionAttempts;
            stats_.conversionTime +=
                std::chrono::duration_cast<std::chrono::nanoseconds>(conversionElapsed);
            if (!image)
            {
                hardwareError();
                if (fallbackPending_) return;
                continue;
            }

            hardwareErrors_ = 0;
            stats_.waitingForKeyframe = false;
            if (image_)
                ++stats_.pendingReplaced;
            image_ = std::move(image);
            converted = ++stats_.framesConverted;
        }

        if (converted == 1)
        {
            printf("First H.264 frame converted: %dx%d, format: %s\n",
                width, height, format ? format : "unknown");
        }
    }
}
