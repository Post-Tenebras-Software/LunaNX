#pragma once
#include "launcher_ui.hpp"
#include <switch.h>

namespace luna::launcher
{
class View
{
public:
    explicit View(PrintConsole* console) : console_(console) {}
    ~View();
    void show(const Image& image);
    void hide();
    bool visible() const { return visible_; }
private:
    PrintConsole* console_;
    Framebuffer framebuffer_{};
    bool visible_ = false;
    u32 previousWidth_ = 0, previousHeight_ = 0;
};
}
