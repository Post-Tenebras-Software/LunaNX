#pragma once

// Only the libnx surface used by controller_input.cpp. Production uses libnx.
#include <cstdint>
using u64 = std::uint64_t;
using s32 = std::int32_t;
constexpr s32 JOYSTICK_MAX = 32767;
enum : u64
{
    HidNpadButton_A = 1ULL << 0, HidNpadButton_B = 1ULL << 1,
    HidNpadButton_X = 1ULL << 2, HidNpadButton_Y = 1ULL << 3,
    HidNpadButton_StickL = 1ULL << 4, HidNpadButton_StickR = 1ULL << 5,
    HidNpadButton_L = 1ULL << 6, HidNpadButton_R = 1ULL << 7,
    HidNpadButton_ZL = 1ULL << 8, HidNpadButton_ZR = 1ULL << 9,
    HidNpadButton_Plus = 1ULL << 10, HidNpadButton_Minus = 1ULL << 11,
    HidNpadButton_Left = 1ULL << 12, HidNpadButton_Up = 1ULL << 13,
    HidNpadButton_Right = 1ULL << 14, HidNpadButton_Down = 1ULL << 15,
};
struct HidAnalogStickState { s32 x = 0, y = 0; };
struct PadState
{
    bool connected = true;
    u64 buttons = 0;
    HidAnalogStickState sticks[2]{};
};
inline bool padIsConnected(const PadState* pad) { return pad->connected; }
inline u64 padGetButtons(const PadState* pad) { return pad->buttons; }
inline HidAnalogStickState padGetStickPos(const PadState* pad, unsigned index)
{
    return pad->sticks[index];
}
