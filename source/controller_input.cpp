#include "controller_input.hpp"
#include "luna_input.hpp"

#include <algorithm>
#include <cstdio>
#include <exception>

namespace
{
double normalizeStick(s32 value)
{
    return std::clamp(static_cast<double>(value) / JOYSTICK_MAX, -1.0, 1.0);
}

luna::input::InputTiming localTiming(std::optional<std::int64_t> serverTimeMs = std::nullopt)
{
    // Live input supplies the clock sample from the control channel.
    return {serverTimeMs.value_or(std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()),
        luna::input::HardwareClass::SwitchPro};
}

void sendBytes(rtc::DataChannel& channel, const luna::input::Bytes& bytes)
{
    // false means accepted into the send buffer, not a failed send.
    channel.send(reinterpret_cast<const rtc::byte*>(bytes.data()), bytes.size());
}

luna::input::GamepadState sampleController(const PadState& pad)
{
    const u64 buttons = padGetButtons(&pad);
    // Use Xbox button positions: Switch B/A/Y/X are Xbox A/B/X/Y.
    // ZL/ZR are separate trigger fields; L/Plus are Xbox LB/Start.
    constexpr struct { u64 source; std::uint32_t target; } mapping[] = {
        {HidNpadButton_B, 0x1000}, {HidNpadButton_A, 0x2000},
        {HidNpadButton_Y, 0x4000}, {HidNpadButton_X, 0x8000},
        {HidNpadButton_StickL, 0x0040}, {HidNpadButton_StickR, 0x0080},
        {HidNpadButton_L, 0x0100}, {HidNpadButton_R, 0x0200},
        {HidNpadButton_Plus, 0x0010}, {HidNpadButton_Minus, 0x0020},
        {HidNpadButton_Up, 0x0001}, {HidNpadButton_Down, 0x0002},
        {HidNpadButton_Left, 0x0004}, {HidNpadButton_Right, 0x0008},
    };
    luna::input::GamepadState state;
    for (const auto& entry : mapping)
        if (buttons & entry.source)
            state.buttons |= entry.target;
    state.leftTrigger = buttons & HidNpadButton_ZL ? 255 : 0;
    state.rightTrigger = buttons & HidNpadButton_ZR ? 255 : 0;
    const auto left = padGetStickPos(&pad, 0);
    const auto right = padGetStickPos(&pad, 1);
    state.leftX = luna::input::convertAxis(normalizeStick(left.x));
    state.leftY = luna::input::convertAxis(normalizeStick(left.y));
    state.rightX = luna::input::convertAxis(normalizeStick(right.x));
    state.rightY = luna::input::convertAxis(normalizeStick(right.y));
    return state;
}
}

void ControllerInputSender::reset()
{
    active_ = false;
    connected_.reset();
    sequence_ = 0;
    sendErrors_ = 0;
    nextSend_ = {};
}

void ControllerInputSender::stop(const std::shared_ptr<rtc::DataChannel>& channel)
{
    if (active_ && connected_.value_or(false) && channel && channel->isOpen())
    {
        try
        {
            luna::input::GamepadState neutral;
            neutral.packetNumber = sequence_++;
            sendBytes(*channel, luna::input::encodeGamepad(neutral, localTiming()));
            sendBytes(*channel, luna::input::encodeConnection(0, false,
                luna::input::GamepadType::X360));
        }
        catch (const std::exception& error)
        {
            printf("Controller release send failed: %s\n", error.what());
        }
    }
    // Peer close may discard queued sends; the browser also releases on close/timeout.
    reset();
}

void ControllerInputSender::update(
    const PadState& pad, const std::shared_ptr<rtc::DataChannel>& channel,
    std::optional<std::int64_t> serverTimeMs, bool suppressInput)
{
    if (!channel || !channel->isOpen())
    {
        reset();
        return;
    }

    if (!active_)
    {
        active_ = true;
        printf("Binary Luna controller forwarding active. L: Xbox LB; +: Xbox Start.\n");
#ifdef LUNANX_LAUNCHER
        printf("Hold Minus+L 1s: console/video. Return shortcut: launcher Settings.\n");
#else
        printf("Hold Minus+L 1s: console/video; Minus+Plus 2s: exit.\n");
#endif
    }

    const auto now = std::chrono::steady_clock::now();
    if (now < nextSend_)
        return;
    nextSend_ = now + std::chrono::microseconds(16'667);

    try
    {
        // Keep sampling locally instead of queuing old input behind a slow send.
        if (channel->bufferedAmount() != 0)
            return;

        const bool connected = padIsConnected(&pad);
        if (!connected_ || *connected_ != connected)
        {
            if (!connected)
            {
                luna::input::GamepadState neutral;
                neutral.packetNumber = sequence_++;
                sendBytes(*channel, luna::input::encodeGamepad(neutral, localTiming(serverTimeMs)));
            }
            sendBytes(*channel, luna::input::encodeConnection(0, connected,
                luna::input::GamepadType::X360));
            connected_ = connected;
            printf("Controller 0 %s.\n", connected ? "connected" : "disconnected");
        }
        if (connected)
        {
            auto state = suppressInput ? luna::input::GamepadState{} : sampleController(pad);
            state.packetNumber = sequence_++;
            sendBytes(*channel, luna::input::encodeGamepad(state, localTiming(serverTimeMs)));
        }
    }
    catch (const std::exception& error)
    {
        if (++sendErrors_ == 1 || sendErrors_ % 120 == 0)
            printf("Controller send failed: %s\n", error.what());
    }
}
