#include "luna_input.hpp"

#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace input = luna::input;

template <typename F>
void requireRejection(F operation)
{
    try { operation(); }
    catch (const std::invalid_argument&) { return; }
    throw std::runtime_error("Invalid input was accepted");
}

void checkInvalidInputs()
{
    for (double value : {-1.01, 1.01, std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        requireRejection([&] { input::convertAxis(value); });
    for (double value : {-0.01, 1.01, std::numeric_limits<double>::infinity(),
        -std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()})
        requireRejection([&] { input::convertTrigger(value); });
    const auto invalidType = static_cast<input::GamepadType>(2);
    requireRejection([&] { input::encodeConnection(0, true, invalidType); });
    input::GamepadState state;
    for (auto member : {&input::GamepadState::leftX, &input::GamepadState::leftY,
        &input::GamepadState::rightX, &input::GamepadState::rightY})
    {
        state.*member = 65535;
        requireRejection([&] { input::encodeGamepad(state, {}); });
        state.*member = 32767;
    }
    for (auto member : {&input::GamepadState::leftTrigger, &input::GamepadState::rightTrigger})
    {
        state.*member = 256;
        requireRejection([&] { input::encodeGamepad(state, {}); });
        state.*member = 0;
    }
    state.type = invalidType;
    requireRejection([&] { input::encodeGamepad(state, {}); });
    state.type = input::GamepadType::X360;
    input::ResiliencyAttributes invalid;
    invalid.category = static_cast<input::ResiliencyCategory>(3);
    requireRejection([&] { input::encodeGamepad(state, {}, invalid); });
    requireRejection([&] { input::encodeConnection(0, true, state.type, invalid); });
}

std::optional<input::ResiliencyAttributes> readResiliency()
{
    bool present;
    std::cin >> present;
    if (!present)
        return std::nullopt;
    input::ResiliencyAttributes result;
    std::uint32_t category;
    std::cin >> category >> result.eventId >> result.keyframeId >>
        result.terminalIdentifier >> result.retransmitCount;
    result.category = static_cast<input::ResiliencyCategory>(category);
    return result;
}

int main()
{
    try
    {
        checkInvalidInputs();
        std::string kind;
        while (std::cin >> kind)
        {
            input::Bytes bytes;
            if (kind == "axis" || kind == "trigger")
            {
                double value;
                if (!(std::cin >> value))
                    throw std::runtime_error("Malformed conversion test");
                std::cout << (kind == "axis" ? input::convertAxis(value) :
                    input::convertTrigger(value)) << '\n';
                continue;
            }
            if (kind == "ack")
                bytes = input::encodePointerModeAck();
            else if (kind == "connection")
            {
                std::uint32_t deviceId, type;
                bool connected;
                std::cin >> deviceId >> connected >> type;
                const auto resiliency = readResiliency();
                if (!std::cin) throw std::runtime_error("Malformed connection test");
                bytes = input::encodeConnection(deviceId, connected,
                    static_cast<input::GamepadType>(type), resiliency);
            }
            else if (kind == "gamepad")
            {
                input::GamepadState s;
                input::InputTiming timing;
                std::uint32_t type, hardware;
                std::cin >> s.deviceId >> s.packetNumber >> s.buttons >> s.leftTrigger >>
                    s.rightTrigger >> s.leftX >> s.leftY >> s.rightX >> s.rightY >> type >>
                    s.leftSensitivity >> s.rightSensitivity >> timing.timestampMs >> hardware;
                s.type = static_cast<input::GamepadType>(type);
                timing.hardwareClass = static_cast<input::HardwareClass>(hardware);
                const auto resiliency = readResiliency();
                if (!std::cin) throw std::runtime_error("Malformed gamepad test");
                bytes = input::encodeGamepad(s, timing, resiliency);
            }
            else
                throw std::runtime_error("Unknown test command");

            for (auto byte : bytes)
                std::cout << std::hex << std::setfill('0') << std::setw(2) <<
                    static_cast<unsigned>(byte);
            std::cout << std::dec << '\n';
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
