#pragma once

#include <switch.h>
#include <rtc/datachannel.hpp>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>

// Main-thread sender of Luna input messages; live mode supplies synchronized time.
class ControllerInputSender
{
public:
    void update(const PadState& pad, const std::shared_ptr<rtc::DataChannel>& channel,
        std::optional<std::int64_t> serverTimeMs = std::nullopt, bool suppressInput = false);
    void stop(const std::shared_ptr<rtc::DataChannel>& channel);

private:
    void reset();
    bool active_ = false;
    std::optional<bool> connected_;
    std::uint32_t sequence_ = 0;
    std::size_t sendErrors_ = 0;
    std::chrono::steady_clock::time_point nextSend_{};
};
