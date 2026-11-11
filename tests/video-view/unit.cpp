#include "video_view.hpp"
#include "h264_decoder.hpp"
#include <iostream>
#include <stdexcept>

int main()
{
    const auto check = [](bool value, const char* message) { if (!value) throw std::runtime_error(message); };
    PrintConsole console;
    VideoView view(&console);
    RgbaImage frame;
    frame.width = 1280; frame.height = 720; frame.stride = frame.width * 4;
    frame.pixels.assign(frame.stride * frame.height, 0x44);
    RgbaImage overlay;
    overlay.width = overlay.height = 2; overlay.stride = 8; overlay.pixels.assign(16, 0xab);
    check(view.show(frame, &overlay), "Video accepts a valid overlay");
    check(activeFramebuffer && activeFramebuffer->pixels[(16 * 1280 + 16) * 4] == 0xab &&
        activeFramebuffer->pixels[0] == 0x44 && frame.pixels[(16 * 1280 + 16) * 4] == 0x44,
        "Overlay paints the destination corner without modifying decoded video or unrelated pixels");
    check(view.getStats().framesPresented == 1, "First fresh video frame is counted");
    const auto presentTime = view.getStats().presentTime;
    check(view.show(frame, nullptr, false) && view.getStats().framesPresented == 1 &&
        view.getStats().presentTime == presentTime && activeFramebuffer->pixels[(16 * 1280 + 16) * 4] == 0x44,
        "Toggling off restores video without counting a duplicate as another video frame");
    overlay.width = 1280;
    check(view.show(frame, &overlay) && activeFramebuffer->pixels[(16 * 1280 + 16) * 4] == 0x44,
        "Oversized or truncated panels cannot write outside framebuffer bounds");
    check(view.getStats().framesPresented == 2, "Fresh video continues to increment presentation FPS");
    overlay.width = 2; overlay.pixels.clear();
    check(view.show(frame, &overlay, false) && activeFramebuffer->pixels[(16 * 1280 + 16) * 4] == 0x44,
        "Truncated small panels are ignored without an out-of-bounds read");
    frame.pixels.clear();
    check(!view.show(frame) && view.getStats().framesPresented == 2, "Invalid video storage is rejected without a frame count");
    view.hide();
    check(!view.isVisible() && !activeFramebuffer && window.width == 854 && window.height == 480,
        "Video cleanup returns the original window dimensions");
    std::cout << "PASS: video/overlay composition, duplicate-frame statistics, storage bounds and window cleanup (simulated libnx).\n";
}
