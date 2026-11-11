#include "controller_input.hpp"

#include <iomanip>
#include <iostream>
#include <thread>

int main()
{
    ControllerInputSender sender;
    PadState pad;
    const auto channel = std::make_shared<rtc::DataChannel>();
    const auto capture = [&](const char* label)
    {
        std::cout << "TEST " << label;
        for (const auto& packet : channel->packets)
        {
            std::cout << ' ';
            for (auto byte : packet)
                std::cout << std::hex << std::setfill('0') << std::setw(2) <<
                    std::to_integer<unsigned>(byte);
        }
        std::cout << std::dec << '\n';
        channel->packets.clear();
    };
    const auto tick = [&](bool suppressInput = false)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(18));
        sender.update(pad, channel, std::nullopt, suppressInput);
    };
    sender.update(pad, channel);
    capture("initial");
    constexpr struct { const char* label; u64 button; } buttons[] = {
        {"A", HidNpadButton_A}, {"B", HidNpadButton_B},
        {"X", HidNpadButton_X}, {"Y", HidNpadButton_Y},
        {"LS", HidNpadButton_StickL}, {"RS", HidNpadButton_StickR},
        {"L", HidNpadButton_L}, {"R", HidNpadButton_R},
        {"ZL", HidNpadButton_ZL}, {"ZR", HidNpadButton_ZR},
        {"Plus", HidNpadButton_Plus}, {"Minus", HidNpadButton_Minus},
        {"Left", HidNpadButton_Left}, {"Up", HidNpadButton_Up},
        {"Right", HidNpadButton_Right}, {"Down", HidNpadButton_Down},
    };
    for (const auto& button : buttons)
    {
        pad.buttons = button.button;
        tick();
        capture(button.label);
    }
    pad.buttons = 0xffff;
    pad.sticks[0] = {-32768, 32767};
    pad.sticks[1] = {32767, -32768};
    tick();
    capture("all-held");
    pad.connected = false; // Raw held controls must not leak into disconnect.
    tick();
    capture("disconnect");
    tick();
    capture("still-disconnected");
    pad.connected = true;
    pad.buttons = HidNpadButton_A;
    pad.sticks[0] = {};
    pad.sticks[1] = {};
    tick();
    capture("reconnect");
    channel->buffered = 1;
    pad.buttons = HidNpadButton_B;
    tick();
    capture("backpressure");
    channel->buffered = 0;
    tick();
    capture("latest-after-backpressure");
    sender.stop(channel);
    capture("stop");
    sender.stop(channel);
    capture("stop-again");
    sender.update(pad, channel);
    capture("restart");
    channel->open = false;
    sender.update(pad, channel);
    capture("closed");
    channel->open = true;
    sender.update(pad, channel);
    capture("reopen");
    channel->failNext = true;
    tick();
    capture("send-failure");
    tick();
    capture("send-recovery");
    sender.stop(channel);
    capture("final-stop");
    pad.connected = false;
    sender.update(pad, channel);
    capture("initially-disconnected");
    pad.connected = true;
    std::this_thread::sleep_for(std::chrono::milliseconds(18));
    sender.update(pad, channel, 1794000000123LL);
    capture("server-clock");
    pad.buttons = HidNpadButton_Minus | HidNpadButton_L | HidNpadButton_ZL;
    pad.sticks[0] = {32767, -32768};
    tick();
    capture("before-shortcut");
    tick(true);
    capture("shortcut-neutral");
    pad.buttons = HidNpadButton_L;
    tick(true);
    capture("shortcut-release-wait");
    channel->buffered = 1;
    tick(true);
    capture("shortcut-backpressure");
    channel->buffered = 0;
    tick(true);
    capture("shortcut-neutral-after-backpressure");
    pad.sticks[0] = {};
    pad.buttons = HidNpadButton_Plus;
    tick();
    capture("shortcut-resumed");
}
