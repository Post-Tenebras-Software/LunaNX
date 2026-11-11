#pragma once

#include "luna_control.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace luna::control
{
struct ClockReading { std::int64_t epochMs, steadyMs; };
struct TimeSample
{
    // Twice the offset retains half-millisecond precision without float rounding.
    std::int64_t offsetTwiceMs = 0;
    std::int64_t delayMs = 0;
    std::int64_t receivedSteadyMs = 0;
    double offsetMs() const { return static_cast<double>(offsetTwiceMs) / 2; }
};

// Single-threaded, caller-driven scheduling. Owns no timer threads or sockets.
class ClockSynchronizer
{
public:
    using Clock = std::function<ClockReading()>;
    explicit ClockSynchronizer(Clock clock);
    void start();
    void stop();
    // Call while the channel is open. Returned pings are recorded as pending.
    // If transport send throws, cancelPing; a buffered send is still accepted.
    std::optional<Ping> nextPing();
    void cancelPing(std::int64_t clientSendTimestampMs);
    bool handlePing(const Ping& response); // true only if this sample updates the offset
    std::optional<TimeSample> sample();
    std::int64_t serverTimeMs();
    bool running() const { return running_; }
    std::size_t pendingCount() const { return pending_.size(); }

    static constexpr std::int64_t StaleAfterMs = 1'500'000;
    static constexpr std::int64_t MaintenanceIntervalMs = 60'000;
    static constexpr std::int64_t PendingLifetimeMs = 10'000;
    static constexpr std::int64_t ClockStepToleranceMs = 250;
    static constexpr std::int64_t MaxExactTimestamp = 9'007'199'254'740'991;

private:
    struct Pending { std::int64_t epochMs, steadyMs; };
    ClockReading readClock();
    ClockReading refresh();
    void restartBurst(const ClockReading& now);
    Clock clock_;
    bool running_ = false;
    std::optional<ClockReading> lastClock_;
    std::optional<TimeSample> best_;
    std::vector<Pending> pending_;
    unsigned burstRemaining_ = 0;
    std::int64_t nextBurstMs_ = 0, nextMaintenanceMs_ = 0;
};
}
