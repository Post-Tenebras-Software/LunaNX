#pragma once

#include <chrono>
#include <optional>
#include <stdexcept>

namespace luna::video
{
// Keep frame deadlines on one timeline. Starting a fresh period after each
// wake-up accumulates scheduler/IPC delays and lowers the presentation rate.
class FramePacer
{
public:
    using Clock = std::chrono::steady_clock;
    using Duration = std::chrono::nanoseconds;
    using Time = Clock::time_point;

    void reset() { deadline_.reset(); previous_.reset(); }

    Duration delay(Time started, Time now, Duration period)
    {
        if (period <= Duration::zero() || now < started)
            throw std::invalid_argument("Invalid frame timing");
        if (!deadline_ || period != period_ || (previous_ && now < *previous_))
        {
            deadline_ = started + period;
            period_ = period;
        }
        previous_ = now;
        if (now >= *deadline_)
        {
            // Drop expired deadlines after a stall. Never replay a backlog of
            // frame ticks or add a fresh whole-period sleep to a late frame.
            *deadline_ += period * ((now - *deadline_) / period + 1);
            return Duration::zero();
        }
        const auto remaining = std::chrono::duration_cast<Duration>(*deadline_ - now);
        *deadline_ += period;
        return remaining;
    }

private:
    std::optional<Time> deadline_, previous_;
    Duration period_{};
};
}
