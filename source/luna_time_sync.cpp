#include "luna_time_sync.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace luna::control
{
ClockSynchronizer::ClockSynchronizer(Clock clock) : clock_(std::move(clock))
{
    if (!clock_) throw std::invalid_argument("Time synchronization needs a clock");
}
ClockReading ClockSynchronizer::readClock()
{
    const auto now = clock_();
    // Match the browser's exact integer range and keep arithmetic well inside int64.
    if (now.epochMs < 0 || now.epochMs > MaxExactTimestamp ||
        now.steadyMs < 0 || now.steadyMs > MaxExactTimestamp)
        throw std::invalid_argument("Clock outside supported millisecond range");
    return now;
}
void ClockSynchronizer::restartBurst(const ClockReading& now)
{
    best_.reset();
    pending_.clear();
    burstRemaining_ = 8;
    nextBurstMs_ = now.steadyMs;
    nextMaintenanceMs_ = now.steadyMs + MaintenanceIntervalMs;
}
void ClockSynchronizer::start()
{
    stop();
    const auto now = readClock();
    restartBurst(now);
    lastClock_ = now;
    running_ = true;
}
void ClockSynchronizer::stop()
{
    running_ = false;
    burstRemaining_ = 0;
    pending_.clear();
    best_.reset();
    lastClock_.reset();
}
ClockReading ClockSynchronizer::refresh()
{
    const auto now = readClock();
    if (running_)
    {
        if (lastClock_)
        {
            const auto steadyDelta = now.steadyMs - lastClock_->steadyMs;
            const auto wallDelta = now.epochMs - lastClock_->epochMs;
            const auto divergence = wallDelta - steadyDelta;
            if (steadyDelta < 0 || wallDelta < 0 || divergence > ClockStepToleranceMs ||
                divergence < -ClockStepToleranceMs)
                restartBurst(now);
        }
        if (best_ && now.steadyMs - best_->receivedSteadyMs > StaleAfterMs)
            restartBurst(now);
        pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& ping)
        {
            return now.steadyMs - ping.steadyMs > PendingLifetimeMs;
        }), pending_.end());
        lastClock_ = now;
    }
    return now;
}
std::optional<Ping> ClockSynchronizer::nextPing()
{
    if (!running_) return std::nullopt;
    const auto now = refresh();
    if (now.steadyMs >= nextMaintenanceMs_)
    {
        if (!best_) restartBurst(now);
        else nextMaintenanceMs_ = now.steadyMs + MaintenanceIntervalMs;
        if (!burstRemaining_) nextBurstMs_ = now.steadyMs;
    }
    const bool burst = burstRemaining_ != 0;
    if (now.steadyMs < nextBurstMs_ || pending_.size() >= 8) return std::nullopt;
    if (std::any_of(pending_.begin(), pending_.end(), [&](const Pending& ping)
        { return ping.epochMs == now.epochMs; })) return std::nullopt;
    pending_.push_back({now.epochMs, now.steadyMs});
    if (burst) --burstRemaining_;
    nextBurstMs_ = burstRemaining_ ? now.steadyMs + 1000 : nextMaintenanceMs_;
    return Ping{now.epochMs, 0, 0};
}
void ClockSynchronizer::cancelPing(std::int64_t timestamp)
{
    pending_.erase(std::remove_if(pending_.begin(), pending_.end(), [&](const Pending& ping)
        { return ping.epochMs == timestamp; }), pending_.end());
}
bool ClockSynchronizer::handlePing(const Ping& response)
{
    if (!running_) return false;
    const auto now = refresh();
    const auto found = std::find_if(pending_.begin(), pending_.end(), [&](const Pending& ping)
        { return ping.epochMs == response.clientSendTimestampMs; });
    if (found == pending_.end()) return false;
    const auto sent = *found;
    pending_.erase(found); // A reply is single-use, including a rejected sample.
    if (response.serverReceiveTimestampMs <= 0 || response.serverReceiveTimestampMs > MaxExactTimestamp ||
        response.serverSendTimestampMs < response.serverReceiveTimestampMs ||
        response.serverSendTimestampMs > MaxExactTimestamp || now.epochMs < sent.epochMs)
        return false;
    const auto delay = (now.epochMs - sent.epochMs) -
        (response.serverSendTimestampMs - response.serverReceiveTimestampMs);
    if (delay <= 0 || delay > (best_ ? best_->delayMs : 999)) return false;
    const auto offsetTwice = (response.serverReceiveTimestampMs - sent.epochMs) +
        (response.serverSendTimestampMs - now.epochMs);
    best_ = TimeSample{offsetTwice, delay, now.steadyMs};
    return true;
}
std::optional<TimeSample> ClockSynchronizer::sample()
{
    refresh();
    return best_;
}
std::int64_t ClockSynchronizer::serverTimeMs()
{
    const auto now = refresh();
    return best_ ? (2 * now.epochMs + best_->offsetTwiceMs) / 2 : now.epochMs;
}
}
