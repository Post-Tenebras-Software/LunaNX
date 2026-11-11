#include "video_view.hpp"
#include "h264_decoder.hpp"

#include <cstdio>
#include <cstring>
#include <algorithm>
#include <stdexcept>

namespace
{
// Console writes can continue while its framebuffer belongs to the video.
ConsoleRenderer silentRenderer = {
    [](PrintConsole*) { return true; },
    [](PrintConsole*) {},
    [](PrintConsole*, int, int, int) {},
    [](PrintConsole*) {},
    [](PrintConsole*) {}
};

class StdioLock
{
public:
    StdioLock() { flockfile(stdout); flockfile(stderr); }
    ~StdioLock() { funlockfile(stderr); funlockfile(stdout); }
};
}

VideoView::~VideoView()
{
    hide();
}
void VideoView::setSize(int width, int height)
{
    if (visible_ || width <= 0 || width > RgbaImage::MaxWidth || height <= 0 || height > RgbaImage::MaxHeight)
        throw std::invalid_argument("Video view size unavailable");
    width_ = width; height_ = height;
}
void VideoView::restoreWindowSize()
{
    if (previousWidth_ && previousHeight_)
    {
        if (R_FAILED(nwindowSetDimensions(nwindowGetDefault(), previousWidth_, previousHeight_)))
            printf("Restoring video window dimensions failed.\n");
        previousWidth_ = previousHeight_ = 0;
    }
}

bool VideoView::show(const RgbaImage& image, const RgbaImage* overlay, bool newVideoFrame)
{
    if (image.width <= 0 || image.width > width_ ||
        image.height <= 0 || image.height > height_ ||
        image.stride < image.width * 4 ||
        image.pixels.size() < static_cast<std::size_t>(image.stride) * image.height)
    {
        printf("Video frame has invalid dimensions or pixel storage.\n");
        return false;
    }

    if (!visible_)
    {
        StdioLock lock;
        if (R_FAILED(nwindowGetDimensions(nwindowGetDefault(), &previousWidth_, &previousHeight_)))
        {
            previousWidth_ = previousHeight_ = 0;
            printf("Reading video window dimensions failed.\n");
            return false;
        }
        consoleExit(console_);
        console_->renderer = &silentRenderer;

        Result result = nwindowSetDimensions(nwindowGetDefault(), width_, height_);
        if (R_SUCCEEDED(result))
            result = framebufferCreate(&framebuffer_, nwindowGetDefault(), width_, height_, PIXEL_FORMAT_RGBA_8888, 2);
        if (R_SUCCEEDED(result))
            result = framebufferMakeLinear(&framebuffer_);
        if (R_FAILED(result))
        {
            framebufferClose(&framebuffer_);
            restoreWindowSize();
            consoleInit(console_);
            printf("Video framebuffer failed: 0x%08X\n", static_cast<unsigned int>(result));
            return false;
        }

        visible_ = true;
#ifdef LUNANX_LAUNCHER
        printf("Video view active. Hold Minus+L 1s: console/video. Return shortcut: launcher Settings.\n");
#else
        printf("Video view active. Hold Minus+L 1s: console/video; Minus+Plus 2s: exit.\n");
#endif
    }

    const auto presentStarted = std::chrono::steady_clock::now();
    u32 stride = 0;
    auto* pixels = static_cast<std::uint8_t*>(framebufferBegin(&framebuffer_, &stride));
    const int left = (width_ - image.width) / 2;
    const int top = (height_ - image.height) / 2;
    if (image.width != width_ || image.height != height_)
    {
        for (int y = 0; y < height_; ++y)
        {
            auto* row = reinterpret_cast<u32*>(pixels + static_cast<std::size_t>(y) * stride);
            std::fill_n(row, width_, RGBA8_MAXALPHA(0, 0, 0));
        }
    }
    for (int y = 0; y < image.height; ++y)
    {
        std::memcpy(pixels + static_cast<std::size_t>(y + top) * stride + left * 4,
            image.pixels.data() + static_cast<std::size_t>(y) * image.stride,
            static_cast<std::size_t>(image.width) * 4);
    }
    // The panel is opaque and pre-rendered. Copy it onto the destination,
    // leaving the decoded image untouched and avoiding per-frame alpha work.
    if (overlay && overlay->width > 0 && overlay->height > 0 && overlay->width <= width_ - 32 &&
        overlay->height <= height_ - 32 && overlay->stride >= overlay->width * 4 &&
        overlay->pixels.size() >= static_cast<std::size_t>(overlay->stride) * overlay->height)
        for (int y = 0; y < overlay->height; ++y)
            std::memcpy(pixels + static_cast<std::size_t>(y + 16) * stride + 16 * 4,
                overlay->pixels.data() + static_cast<std::size_t>(y) * overlay->stride,
                static_cast<std::size_t>(overlay->width) * 4);
    framebufferEnd(&framebuffer_);
    if (newVideoFrame)
    {
        stats_.presentTime += std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - presentStarted);
        if (++stats_.framesPresented == 1) printf("First video frame presented.\n");
    }
    return true;
}

void VideoView::hide()
{
    if (!visible_)
        return;

    StdioLock lock;
    framebufferClose(&framebuffer_);
    restoreWindowSize();
    consoleInit(console_);
    visible_ = false;
#ifdef LUNANX_LAUNCHER
    printf("Console restored. Hold Minus+L 1s: show video. Return shortcut: launcher Settings.\n");
#else
    printf("Console restored. Hold Minus+L 1s: show video; Minus+Plus 2s: exit.\n");
#endif
}
