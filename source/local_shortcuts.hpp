#pragma once

#include <switch.h>
#include <chrono>

class LocalShortcuts
{
public:
    using Clock = std::chrono::steady_clock;
    struct Actions
    {
        bool toggleVideo = false;
        bool exit = false;
        bool suppressInput = false;
        bool toggleOverlay = false;
    };

    Actions update(u64 buttons, bool connected, Clock::time_point now = Clock::now(), bool overlayEnabled = false)
    {
        if (!connected)
        {
            *this = {};
            return {};
        }
        if (blockedButtons_ && !(buttons & blockedButtons_))
            *this = {};
        if (fired_)
            return {false, false, true};

        // Exit, then console view, take priority over the overlay chord.
        // Changing chords restarts the timer.
        const auto chord = (buttons & ExitButtons) == ExitButtons ? Chord::Exit :
            (buttons & VideoButtons) == VideoButtons ? Chord::Video :
            overlayEnabled && (buttons & OverlayButtons) == OverlayButtons ? Chord::Overlay : Chord::None;
        if (chord == Chord::None)
        {
            holding_ = Chord::None;
            return {false, false, blockedButtons_ != 0};
        }
        if (chord != holding_ || now < started_)
        {
            holding_ = chord;
            started_ = now;
        }
        // Keep input neutral through partial release, including a cancelled hold.
        blockedButtons_ |= chord == Chord::Exit ? ExitButtons : chord == Chord::Video ? VideoButtons : OverlayButtons;
        const auto duration = chord == Chord::Exit ? std::chrono::seconds(2) : std::chrono::seconds(1);
        if (now - started_ < duration)
            return {false, false, true};
        fired_ = true;
        return {chord == Chord::Video, chord == Chord::Exit, true, chord == Chord::Overlay};
    }

private:
    enum class Chord { None, Video, Exit, Overlay };
    static constexpr u64 VideoButtons = HidNpadButton_Minus | HidNpadButton_L;
    static constexpr u64 ExitButtons = HidNpadButton_Minus | HidNpadButton_Plus;
    static constexpr u64 OverlayButtons = HidNpadButton_Minus | HidNpadButton_R;
    Chord holding_ = Chord::None;
    Clock::time_point started_{};
    u64 blockedButtons_ = 0;
    bool fired_ = false;
};
