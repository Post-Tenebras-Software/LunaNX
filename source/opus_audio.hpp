#pragma once

#include <switch.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <vector>

class OpusAudioPlayer
{
public:
    OpusAudioPlayer() = default;
    ~OpusAudioPlayer();
    OpusAudioPlayer(const OpusAudioPlayer&) = delete;
    OpusAudioPlayer& operator=(const OpusAudioPlayer&) = delete;

    // The receive callback only enqueues; update and stop run on the main thread.
    void enqueue(const std::byte* data, std::size_t size, std::uint32_t timestamp);
    void update();
    void stop();

private:
    static constexpr int SampleRate = 48000;
    static constexpr int Channels = 2;
    static constexpr int MaxSamples = 5760; // Opus packets can contain up to 120 ms.
    static constexpr std::size_t MaxPacketBytes = 65535;
    static constexpr std::size_t MaxPendingPackets = 4;
    static constexpr std::size_t BufferCount = 4;
    static constexpr std::size_t BufferBytes =
        (MaxSamples * Channels * sizeof(s16) + 0xfff) & ~std::size_t(0xfff);

    struct Packet
    {
        std::vector<std::byte> bytes;
        std::uint32_t timestamp;
    };
    struct Slot
    {
        AudioOutBuffer buffer{};
        bool queued = false;
    };

    void initialize();
    void releaseBuffers();
    void logStats();

    std::mutex mutex_;
    std::deque<Packet> pending_;
    bool stopped_ = false;
    std::size_t dropped_ = 0;
    HwopusDecoder decoder_{};
    bool decoderReady_ = false;
    bool outputReady_ = false;
    bool playing_ = false;
    std::array<Slot, BufferCount> slots_{};
    std::array<std::byte, sizeof(HwopusHeader) + MaxPacketBytes> encoded_{};
    std::size_t queued_ = 0;
    std::size_t queuedSamples_ = 0;
    std::size_t decoded_ = 0;
    std::size_t decodeErrors_ = 0;
    std::size_t underruns_ = 0;
    bool haveTimestamp_ = false;
    std::uint32_t lastTimestamp_ = 0;
    std::chrono::steady_clock::time_point lastReport_{};
};
