#include "local_shortcuts.hpp"

#include <initializer_list>
#include <iostream>
#include <stdexcept>

namespace
{
constexpr u64 Video = HidNpadButton_Minus | HidNpadButton_L;
constexpr u64 Exit = HidNpadButton_Minus | HidNpadButton_Plus;
struct Step
{
    int milliseconds;
    u64 buttons;
    bool suppressed = false;
    bool toggle = false;
    bool exit = false;
    bool connected = true;
};
void check(const char* name, std::initializer_list<Step> steps)
{
    LocalShortcuts shortcuts;
    for (const auto& step : steps)
    {
        const auto now = LocalShortcuts::Clock::time_point{std::chrono::milliseconds(step.milliseconds)};
        const auto result = shortcuts.update(step.buttons, step.connected, now);
        if (result.toggleVideo != step.toggle || result.exit != step.exit || result.suppressInput != step.suppressed)
            throw std::runtime_error(name);
    }
}
}

int main()
{
    check("Ordinary controls stay available", {
        {0, HidNpadButton_L}, {2000, HidNpadButton_L}, {2100, 0},
        {2200, HidNpadButton_Plus}, {5000, HidNpadButton_Plus}, {5100, 0},
        {5200, HidNpadButton_Minus}, {8000, HidNpadButton_Minus},
        {8100, HidNpadButton_L | HidNpadButton_Plus}, {12000, HidNpadButton_L | HidNpadButton_Plus}
    });
    check("Video threshold, one action and partial release", {
        {0, Video, true}, {999, Video, true}, {1000, Video, true, true},
        {5000, Video, true}, {5100, HidNpadButton_L, true},
        {5200, Exit | HidNpadButton_L, true}, // Fired chords cannot switch actions until release.
        {5300, 0}, {5400, Video, true}, {6400, Video, true, true}
    });
    check("Exit threshold and one action", {
        {0, Exit, true}, {1999, Exit, true}, {2000, Exit, true, false, true},
        {5000, Exit, true}, {5100, HidNpadButton_Plus, true}, {5200, 0},
        {5300, HidNpadButton_Plus}
    });
    check("Cancelled holds restart their timer without leaking a remaining button", {
        {0, Video, true}, {900, HidNpadButton_Minus, true}, {1000, Video, true},
        {1900, Video, true}, {1999, Video, true}, {2000, Video, true, true}, {2100, 0}
    });
    check("Exit has priority when both combinations are held", {
        {0, Video | Exit, true}, {1000, Video | Exit, true},
        {2000, Video | Exit, true, false, true}, {3000, Video | Exit, true}
    });
    check("Changing combinations restarts the timer", {
        {0, Video, true}, {900, Video | Exit, true}, {2000, Video | Exit, true},
        {2899, Exit, true}, {2900, Exit, true, false, true},
        {3000, HidNpadButton_L, true}, {3100, 0}
    });
    check("Disconnect cancels a pending or fired shortcut", {
        {0, Exit, true}, {1900, Exit, false, false, false, false},
        {2000, Exit, true}, {3999, Exit, true}, {4000, Exit, true, false, true},
        {4100, Exit, false, false, false, false}, {4200, HidNpadButton_Plus}
    });
    {
        LocalShortcuts shortcuts;
        const auto update = [&](u64 buttons, int milliseconds, bool enabled = true, bool connected = true)
        { return shortcuts.update(buttons, connected, LocalShortcuts::Clock::time_point{std::chrono::milliseconds(milliseconds)}, enabled); };
        constexpr auto overlay = HidNpadButton_Minus | HidNpadButton_R;
        if (update(overlay, 0, false).suppressInput || update(HidNpadButton_R, 1).suppressInput ||
            !update(overlay, 10).suppressInput || update(overlay, 1009).toggleOverlay ||
            !update(overlay, 1010).toggleOverlay || update(overlay, 4000).toggleOverlay ||
            !update(HidNpadButton_R, 4100).suppressInput || update(0, 4200).suppressInput)
            throw std::runtime_error("Overlay threshold, one-shot action, opt-in and partial release");
        update(overlay, 5000); update(overlay, 5200, true, false);
        if (update(overlay, 6000).toggleOverlay || !update(overlay, 7000).toggleOverlay)
            throw std::runtime_error("Overlay reconnect requires a fresh hold");
        update(0, 7100); update(overlay, 8000); update(overlay, 7500);
        if (update(overlay, 8499).toggleOverlay || !update(overlay, 8500).toggleOverlay)
            throw std::runtime_error("Overlay clock rollback restarts its hold");
    }
    std::cout << "PASS: shortcut deadlines, cancellation, one-shot actions, exit priority, release gating and reconnect.\n";
}
