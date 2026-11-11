#include "launcher_view.hpp"
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace luna::launcher
{
namespace
{
ConsoleRenderer silentRenderer = {
    [](PrintConsole*) { return true; }, [](PrintConsole*) {},
    [](PrintConsole*, int, int, int) {}, [](PrintConsole*) {}, [](PrintConsole*) {}};
struct StdioLock
{
    StdioLock() { flockfile(stdout); flockfile(stderr); }
    ~StdioLock() { funlockfile(stderr); funlockfile(stdout); }
};
}
View::~View() { hide(); }
void View::show(const Image& image)
{
    if (image.width != 1280 || image.height != 720 || image.pixels.size() != 1280 * 720)
        throw std::invalid_argument("Launcher dimensions");
    if (!visible_)
    {
        StdioLock lock;
        if (R_FAILED(nwindowGetDimensions(nwindowGetDefault(), &previousWidth_, &previousHeight_)))
            throw std::runtime_error("Launcher window unavailable");
        consoleExit(console_); console_->renderer = &silentRenderer;
        auto result = nwindowSetDimensions(nwindowGetDefault(), 1280, 720);
        if (R_SUCCEEDED(result)) result = framebufferCreate(&framebuffer_, nwindowGetDefault(), 1280, 720, PIXEL_FORMAT_RGBA_8888, 2);
        if (R_SUCCEEDED(result)) result = framebufferMakeLinear(&framebuffer_);
        if (R_FAILED(result))
        {
            framebufferClose(&framebuffer_);
            nwindowSetDimensions(nwindowGetDefault(), previousWidth_, previousHeight_);
            consoleInit(console_);
            throw std::runtime_error("Launcher framebuffer unavailable");
        }
        visible_ = true;
    }
    u32 stride = 0;
    auto* pixels = static_cast<unsigned char*>(framebufferBegin(&framebuffer_, &stride));
    for (unsigned y = 0; y < 720; ++y)
        std::memcpy(pixels + y * stride, image.pixels.data() + y * 1280, 1280 * 4);
    framebufferEnd(&framebuffer_);
}
void View::hide()
{
    if (!visible_) return;
    StdioLock lock;
    framebufferClose(&framebuffer_);
    nwindowSetDimensions(nwindowGetDefault(), previousWidth_, previousHeight_);
    consoleInit(console_);
    visible_ = false;
}
}
