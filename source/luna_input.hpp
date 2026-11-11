#pragma once

#include <cstdint>
#include <optional>
#include <vector>

namespace luna::input
{
using Bytes = std::vector<std::uint8_t>;

enum class GamepadType : std::uint32_t { X360 = 0, DS4 = 1 };
enum class HardwareClass : std::uint32_t
{
    Unknown = 0,
    GenericController = 4,
    Xbox360 = 5,
    XboxOne = 6,
    DualShock4 = 7,
    SwitchPro = 11,
};
enum class ResiliencyCategory : std::uint32_t { Lossy = 0, Keyframe = 1, Terminal = 2 };

struct ResiliencyAttributes
{
    ResiliencyCategory category = ResiliencyCategory::Lossy;
    std::uint32_t eventId = 0;
    std::uint32_t keyframeId = 0;
    // The browser supplies its chainId here, in field 40 (not field 41).
    std::uint32_t terminalIdentifier = 0;
    std::uint32_t retransmitCount = 0;
};

struct GamepadState
{
    std::uint32_t deviceId = 0;
    std::uint32_t packetNumber = 0;
    std::uint32_t buttons = 0;
    std::uint32_t leftTrigger = 0;
    std::uint32_t rightTrigger = 0;
    // Offset unsigned axes: center 32767, range 0..65534; positive Y is up.
    std::uint32_t leftX = 32767;
    std::uint32_t leftY = 32767;
    std::uint32_t rightX = 32767;
    std::uint32_t rightY = 32767;
    GamepadType type = GamepadType::X360;
    std::int32_t leftSensitivity = 0;
    std::int32_t rightSensitivity = 0;
};

struct InputTiming
{
    // Caller supplies server-adjusted epoch milliseconds, not app uptime.
    std::int64_t timestampMs = 0;
    HardwareClass hardwareClass = HardwareClass::SwitchPro;
};

// Match browser rounding for finite normalized values; throw std::invalid_argument
// outside the axis [-1, 1] and trigger [0, 1] ranges. No deadzone is applied.
std::uint32_t convertAxis(double value);
std::uint32_t convertTrigger(double value);

// Complete ClientMessage bytes, with no incoming-server framing header added.
// Empty optional metadata still has message presence when explicitly supplied.
Bytes encodeGamepad(const GamepadState& state, const InputTiming& timing,
    const std::optional<ResiliencyAttributes>& resiliency = std::nullopt);
Bytes encodeConnection(std::uint32_t deviceId, bool connected, GamepadType type,
    const std::optional<ResiliencyAttributes>& resiliency = std::nullopt);
Bytes encodePointerModeAck();
}
