#pragma once
#include <switch.h>
#include <chrono>
#include <memory>
#include <functional>
#include <string>
#include "video_bitrate.hpp"
#include "video_decoder.hpp"

// Thin adapter over the hardware-tested alpha streaming path.
class NativeRuntime
{
public:
    explicit NativeRuntime(PrintConsole* console);
    ~NativeRuntime();
    void prepareVideo(luna::video::Mode mode, luna::video::Decoder decoder, unsigned bitrateMbps = 0);
    bool hardwareDecoderPrepared() const;
    bool setStatsOverlay(bool enabled);
    bool statsOverlayEnabled() const;
    void setVideoHandoff(std::function<void()> handoff);
    bool hasPresentedFrame() const;
    bool waitingForVideo() const;
    std::string createOffer(luna::video::Mode mode = luna::video::Mode::P720_30,
        luna::video::Decoder decoder = luna::video::Decoder::Software, unsigned bitrateMbps = 0);
    luna::video::Mode videoMode() const;
    std::int64_t frameIntervalNs() const;
    void paceFrame(std::chrono::steady_clock::time_point started);
    void applyAnswer(const std::string& sdp);
    void update(PadState& pad, bool suppressInput, bool toggleVideo);
    void stop();
    bool visible() const;
    bool active() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
