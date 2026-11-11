#pragma once

#include "luna_time_sync.hpp"
#include "video_bitrate.hpp"

#include <deque>
#include <mutex>

namespace luna::control
{
// Main thread owns protocol/clock state;
// network callbacks may call enqueue(), which only writes the bounded mailbox.
class LocalControlSession
{
public:
    struct Status
    {
        std::uint64_t revision = 0;
        bool opened = false, displayReply = false, closing = false, closeAckSent = false;
        bool shouldClosePeer = false;
        unsigned startupMessages = 0, pingsSent = 0, pingsAccepted = 0;
        std::size_t invalidFrames = 0, droppedFrames = 0, sendErrors = 0;
        std::optional<TimeSample> clock;
    };
    using Send = std::function<void(const Bytes&)>;
    explicit LocalControlSession(ClockSynchronizer::Clock clock, bool live = false,
        video::Mode mode = video::Mode::P720_30, unsigned bitrateMbps = 0);
    bool enqueue(const std::uint8_t* data, std::size_t size);
    void update(bool channelOpen, bool buffered, const Send& send);
    void stop();
    const Status& status() const { return status_; }
    std::string statusJson() const;

    static constexpr std::size_t MaxQueuedFrames = 16;
    static constexpr std::size_t MaxQueuedBytes = 2 * (MaxPayloadBytes + 16);

private:
    void receive(const Bytes& frame);
    Bytes startupMessage(unsigned index) const;
    ClockSynchronizer::Clock clock_;
    ClockSynchronizer sync_;
    Status status_;
    bool stopped_ = false;
    bool live_ = false;
    video::Mode mode_;
    unsigned bitrateMbps_ = 0;
    std::int64_t closeDeadlineMs_ = 0;
    std::mutex mutex_;
    bool accepting_ = true;
    std::deque<Bytes> incoming_;
    std::size_t queuedBytes_ = 0, dropped_ = 0;
};
}
