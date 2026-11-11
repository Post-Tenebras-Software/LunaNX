#pragma once

#include <switch.h>
#include <cstddef>
#include <chrono>

struct RgbaImage;

class VideoView
{
public:
    struct Stats
    {
        std::size_t framesPresented = 0;
        std::chrono::nanoseconds presentTime{};
    };

    explicit VideoView(PrintConsole* console) : console_(console) {}
    ~VideoView();

    VideoView(const VideoView&) = delete;
    VideoView& operator=(const VideoView&) = delete;

    bool show(const RgbaImage& image, const RgbaImage* overlay = nullptr, bool newVideoFrame = true);
    void setSize(int width, int height);
    void hide();
    bool isVisible() const { return visible_; }
    Stats getStats() const { return stats_; }

private:
    PrintConsole* console_;
    Framebuffer framebuffer_ = {};
    bool visible_ = false;
    Stats stats_;
    int width_ = 1280, height_ = 720;
    u32 previousWidth_ = 0, previousHeight_ = 0;
    void restoreWindowSize();
};
