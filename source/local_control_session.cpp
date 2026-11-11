#include "local_control_session.hpp"

#include <cstdio>
#include <new>
#include <stdexcept>
#include <utility>

namespace luna::control
{
LocalControlSession::LocalControlSession(ClockSynchronizer::Clock clock, bool live, video::Mode mode, unsigned bitrateMbps)
    : clock_(std::move(clock)), sync_(clock_), live_(live), mode_(mode), bitrateMbps_(bitrateMbps)
{
    video::bitrateRange(mode_, bitrateMbps_);
}
bool LocalControlSession::enqueue(const std::uint8_t* data, std::size_t size)
{
    std::lock_guard<std::mutex> lock(mutex_);
    if (!accepting_) return false;
    if ((!data && size) || size > MaxPayloadBytes + 16 || incoming_.size() >= MaxQueuedFrames ||
        size > MaxQueuedBytes - queuedBytes_)
    {
        ++dropped_;
        return false;
    }
    try
    {
        Bytes copy;
        if (size) copy.assign(data, data + size);
        incoming_.push_back(std::move(copy));
    }
    catch (const std::bad_alloc&)
    {
        ++dropped_;
        return false;
    }
    queuedBytes_ += size;
    return true;
}
Bytes LocalControlSession::startupMessage(unsigned index) const
{
    if (index == 0)
        return encodeFeatureConfiguration({{"server-reconfigure", "client-display-config-flag"}});
    if (index == 1)
    {
        const auto mode = video::configuration(mode_);
        const auto bitrate = video::bitrateRange(mode_, bitrateMbps_);
        DisplayConfiguration config;
        config.negotiationId = 1;
        config.display = Display{Resolution{static_cast<std::uint32_t>(mode.width), static_cast<std::uint32_t>(mode.height)},
            static_cast<std::uint32_t>(mode.fps), SafeZones{}};
        config.codecs = {{Codec::Avc, 66, mode.h264Level, Tier::Main, 1}};
        config.minBitsPerSecond = live_ ? bitrate.minimum : 400000;
        config.maxBitsPerSecond = live_ ? bitrate.maximum : 800000;
        config.startBitsPerSecond = live_ ? bitrate.start : 600000;
        return encodeDisplayConfiguration(config);
    }
    return encodeClientContext({"LunaNX", "0.1.0", "Nintendo Switch", "console", "Horizon", live_ ? "homebrew" : "local-test"});
}
void LocalControlSession::receive(const Bytes& frame)
{
    const auto message = decodeServerFrame(frame);
    if (status_.closing) return;
    if (const auto* ping = std::get_if<Ping>(&message.body))
    {
        if (sync_.handlePing(*ping)) ++status_.pingsAccepted;
        ++status_.revision;
    }
    else if (std::holds_alternative<DisplayConfiguration>(message.body))
    {
        status_.displayReply = true; // Synthetic display echo is not gameplay readiness.
        ++status_.revision;
    }
    else if (std::holds_alternative<CloseConnectionRequest>(message.body))
    {
        status_.closing = true;
        sync_.stop();
        status_.clock.reset();
        closeDeadlineMs_ = clock_().steadyMs + 2000;
        ++status_.revision;
    }
}
void LocalControlSession::update(bool channelOpen, bool buffered, const Send& send)
{
    if (stopped_) return;
    if (!channelOpen)
    {
        if (status_.opened)
        {
            status_.shouldClosePeer = true;
            stop();
        }
        return;
    }
    if (!status_.opened)
    {
        sync_.start();
        status_.opened = true;
        ++status_.revision;
    }

    std::deque<Bytes> frames;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        frames.swap(incoming_);
        queuedBytes_ = 0;
        if (status_.droppedFrames != dropped_)
        {
            status_.droppedFrames = dropped_;
            ++status_.revision;
        }
    }
    for (const auto& frame : frames)
    {
        try { receive(frame); }
        catch (const std::invalid_argument&)
        {
            ++status_.invalidFrames;
            ++status_.revision;
        }
    }
    const auto sample = sync_.sample();
    if (status_.clock.has_value() != sample.has_value() || (sample &&
        (status_.clock->offsetTwiceMs != sample->offsetTwiceMs || status_.clock->delayMs != sample->delayMs)))
        ++status_.revision;
    status_.clock = sample;

    if (status_.closing && clock_().steadyMs >= closeDeadlineMs_)
    {
        status_.shouldClosePeer = true;
        stop();
        return;
    }
    if (buffered) return;
    std::optional<Ping> pending;
    try
    {
        if (status_.closing)
        {
            if (!status_.closeAckSent)
            {
                send(encodeCloseAck());
                status_.closeAckSent = true;
                // Browser closes its peer after receiving the ACK. Bound the fallback wait.
                closeDeadlineMs_ = clock_().steadyMs + 1000;
                ++status_.revision;
            }
        }
        else if (status_.startupMessages < 3)
        {
            send(startupMessage(status_.startupMessages));
            ++status_.startupMessages;
            ++status_.revision;
        }
        else if ((pending = sync_.nextPing()))
        {
            send(encodePing(*pending));
            ++status_.pingsSent;
            ++status_.revision;
        }
    }
    catch (const std::exception&)
    {
        if (pending) sync_.cancelPing(pending->clientSendTimestampMs);
        ++status_.sendErrors;
        ++status_.revision;
    }
}
void LocalControlSession::stop()
{
    if (stopped_) return;
    stopped_ = true;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        accepting_ = false;
        incoming_.clear();
        queuedBytes_ = 0;
    }
    sync_.stop();
    status_.clock.reset();
    ++status_.revision;
}
std::string LocalControlSession::statusJson() const
{
    char buffer[640];
    const int count = std::snprintf(buffer, sizeof(buffer),
        "{\"type\":\"lunanx-control-status\",\"startupMessages\":%u,\"displayReply\":%s,"
        "\"synchronized\":%s,\"offsetMs\":%.1f,\"delayMs\":%lld,\"pingsSent\":%u,\"pingsAccepted\":%u,"
        "\"invalidFrames\":%zu,\"droppedFrames\":%zu,\"sendErrors\":%zu,"
        "\"closing\":%s,\"closeAckSent\":%s,\"shouldClosePeer\":%s}",
        status_.startupMessages, status_.displayReply ? "true" : "false", status_.clock ? "true" : "false",
        status_.clock ? status_.clock->offsetMs() : 0,
        static_cast<long long>(status_.clock ? status_.clock->delayMs : 0),
        status_.pingsSent, status_.pingsAccepted, status_.invalidFrames, status_.droppedFrames, status_.sendErrors,
        status_.closing ? "true" : "false", status_.closeAckSent ? "true" : "false",
        status_.shouldClosePeer ? "true" : "false");
    if (count < 0 || static_cast<std::size_t>(count) >= sizeof(buffer))
        throw std::runtime_error("Control status exceeded its buffer");
    return std::string(buffer, static_cast<std::size_t>(count));
}
}
