#pragma once
#include <cstdint>
#include <string>

namespace luna::launcher
{
inline constexpr std::uint64_t CaptureButton = 1ull << 63;
inline constexpr std::uint64_t PadButtons = 0xffff;
inline constexpr std::uint64_t PlusButton = 1ull << 10, MinusButton = 1ull << 11;
inline constexpr std::uint64_t DefaultMenuButtons = CaptureButton | PlusButton;
inline constexpr std::uint64_t FallbackMenuButtons = MinusButton | PlusButton;
inline bool overlayShortcutAvailable(std::uint64_t menuButtons)
{
    constexpr auto overlay = MinusButton | (1ull << 7); // Minus + R
    const auto menu = menuButtons & PadButtons;
    const auto overlap = menu & overlay;
    return overlap != menu && overlap != overlay; // A configured menu prefix wins.
}
inline bool validShortcut(std::uint64_t buttons)
{
    if (buttons & ~(PadButtons | CaptureButton)) return false;
    unsigned count = 0;
    for (auto remaining = buttons; remaining; remaining &= remaining - 1) ++count;
    return count >= 2 && count <= 4;
}
std::string shortcutLabel(std::uint64_t buttons);
std::uint64_t loadShortcut(const std::string& path);
bool saveShortcut(const std::string& path, std::uint64_t buttons);

class MenuShortcut
{
public:
    struct Action { bool menu = false, suppress = false; };
    void configure(std::uint64_t buttons) { buttons_ = validShortcut(buttons) ? buttons : DefaultMenuButtons; reset(); }
    Action update(std::uint64_t held, bool capturePressed, bool connected, bool inGame, std::int64_t now)
    {
        held &= PadButtons;
        if (!connected) { reset(); return {}; }
        if (blocked_ && !(held & blocked_)) reset();
        if (fired_) return {false, true};
        const auto required = buttons_ & PadButtons;
        const bool complete = (held & required) == required;
        if (!inGame) return {false, blocked_ != 0};
        if (buttons_ & CaptureButton)
        {
            if (capturePressed && complete)
            { blocked_ = required; fired_ = true; return {true, true}; }
            return {false, blocked_ != 0};
        }
        if (!complete) { holding_ = false; return {false, blocked_ != 0}; }
        blocked_ = required;
        if (!holding_ || now < started_) { holding_ = true; started_ = now; }
        if (now - started_ < 2000) return {false, true};
        fired_ = true; return {true, true};
    }
private:
    void reset() { blocked_ = 0; holding_ = fired_ = false; started_ = 0; }
    std::uint64_t buttons_ = DefaultMenuButtons, blocked_ = 0;
    std::int64_t started_ = 0;
    bool holding_ = false, fired_ = false;
};

class ShortcutRecorder
{
public:
    struct Result { std::uint64_t binding = 0; bool cancelled = false; };
    void begin() { stage_ = Stage::Release; candidate_ = 0; cancelOnRelease_ = false; }
    bool active() const { return stage_ != Stage::Idle; }
    bool recording() const { return stage_ == Stage::Release || stage_ == Stage::Listen; }
    Result update(std::uint64_t held, bool capturePressed, bool connected, std::int64_t now)
    {
        held &= PadButtons;
        if (!connected) { stage_ = Stage::Idle; return {0, true}; }
        if (stage_ == Stage::Idle) return {};
        if (stage_ == Stage::Release || stage_ == Stage::Cooling)
        {
            if (!held && !capturePressed) stage_ = stage_ == Stage::Release ? Stage::Listen : Stage::Idle;
            return {};
        }
        if (capturePressed && validShortcut(held | CaptureButton))
        { stage_ = Stage::Cooling; return {held | CaptureButton, false}; }
        // Cancel on B's release so B can also be a modifier for Capture.
        if (!held && cancelOnRelease_) { stage_ = Stage::Idle; return {0, true}; }
        cancelOnRelease_ = held == (1ull << 1);
        if (!validShortcut(held)) { candidate_ = 0; return {}; }
        if (candidate_ != held || now < started_) { candidate_ = held; started_ = now; }
        if (now - started_ < 600) return {};
        stage_ = Stage::Cooling; return {held, false};
    }
private:
    enum class Stage { Idle, Release, Listen, Cooling };
    Stage stage_ = Stage::Idle;
    std::uint64_t candidate_ = 0;
    std::int64_t started_ = 0;
    bool cancelOnRelease_ = false;
};
}
