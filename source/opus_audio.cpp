#include "opus_audio.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <malloc.h>
#include <stdexcept>
#include <utility>

namespace
{
void checkAudioResult(Result result, const char* operation)
{
    if (R_SUCCEEDED(result))
        return;
    char message[128];
    std::snprintf(message, sizeof(message), "%s failed: 0x%08X",
        operation, static_cast<unsigned int>(result));
    throw std::runtime_error(message);
}
}

OpusAudioPlayer::~OpusAudioPlayer()
{
    stop();
}

void OpusAudioPlayer::enqueue(const std::byte* data, std::size_t size,
    std::uint32_t timestamp)
{
    if (!data || size == 0 || size > MaxPacketBytes)
        return;
    std::lock_guard<std::mutex> lock(mutex_);
    if (stopped_)
        return;
    Packet packet{{data, data + size}, timestamp};
    if (pending_.size() == MaxPendingPackets)
    {
        pending_.pop_front();
        ++dropped_;
    }
    pending_.push_back(std::move(packet));
}

void OpusAudioPlayer::initialize()
{
    checkAudioResult(hwopusDecoderInitialize(&decoder_, SampleRate, Channels), "hwopus initialization");
    decoderReady_ = true;
    checkAudioResult(audoutInitialize(), "Audio output initialization");
    outputReady_ = true;
    if (audoutGetSampleRate() != SampleRate || audoutGetChannelCount() != Channels ||
        audoutGetPcmFormat() != PcmFormat_Int16)
        throw std::runtime_error("Audio output requires 48000 Hz stereo signed 16-bit PCM.");

    for (auto& slot : slots_)
    {
        slot.buffer.buffer = memalign(0x1000, BufferBytes);
        if (!slot.buffer.buffer)
            throw std::runtime_error("Could not allocate an audio output buffer.");
        std::memset(slot.buffer.buffer, 0, BufferBytes);
        slot.buffer.buffer_size = BufferBytes;
    }
    lastReport_ = std::chrono::steady_clock::now();
    printf("Opus audio initialized: 48000 Hz, stereo, signed 16-bit PCM.\n");
}

void OpusAudioPlayer::releaseBuffers()
{
    while (true)
    {
        AudioOutBuffer* released = nullptr;
        u32 count = 0;
        checkAudioResult(audoutGetReleasedAudioOutBuffer(&released, &count), "Audio buffer release");
        if (count == 0)
            return;
        bool found = false;
        for (auto& slot : slots_)
        {
            if (&slot.buffer == released && slot.queued)
            {
                queuedSamples_ -= slot.buffer.data_size / (Channels * sizeof(s16));
                slot.queued = false;
                --queued_;
                found = true;
                break;
            }
        }
        if (!found)
            throw std::runtime_error("Audio output returned an unknown buffer.");
    }
}

void OpusAudioPlayer::update()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_ || (!outputReady_ && pending_.empty()))
            return;
    }
    if (!outputReady_)
        initialize();

    releaseBuffers();
    if (playing_ && queued_ == 0)
    {
        checkAudioResult(audoutStopAudioOut(), "Audio rebuffering");
        playing_ = false;
        ++underruns_;
    }

    for (auto& slot : slots_)
    {
        if (slot.queued)
            continue;

        Packet packet;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (pending_.empty())
                break;
            packet = std::move(pending_.front());
            pending_.pop_front();
        }

        const std::uint32_t delta = packet.timestamp - lastTimestamp_;
        if (haveTimestamp_ && (delta == 0 || delta >= 0x80000000u))
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++dropped_;
            continue;
        }

        HwopusHeader header{};
        header.size = __builtin_bswap32(static_cast<u32>(packet.bytes.size()));
        std::memcpy(encoded_.data(), &header, sizeof(header));
        std::memcpy(encoded_.data() + sizeof(header), packet.bytes.data(), packet.bytes.size());
        const auto inputSize = sizeof(header) + packet.bytes.size();
        s32 consumed = 0;
        s32 samples = 0;
        const Result result = hwopusDecodeInterleaved(&decoder_, &consumed, &samples,
            encoded_.data(), inputSize, static_cast<s16*>(slot.buffer.buffer), BufferBytes);
        if (R_FAILED(result) || consumed != static_cast<s32>(inputSize) ||
            samples <= 0 || samples > MaxSamples)
        {
            ++decodeErrors_;
            if (decodeErrors_ == 1 || decodeErrors_ % 50 == 0)
                printf("Opus decode failed: 0x%08X, consumed %d/%zu bytes, samples %d\n",
                    static_cast<unsigned int>(result), static_cast<int>(consumed), inputSize,
                    static_cast<int>(samples));
            continue;
        }
        haveTimestamp_ = true;
        lastTimestamp_ = packet.timestamp;
        slot.buffer.data_size = samples * Channels * sizeof(s16);
        armDCacheFlush(slot.buffer.buffer, slot.buffer.data_size);
        checkAudioResult(audoutAppendAudioOutBuffer(&slot.buffer), "Audio buffer submission");
        slot.queued = true;
        ++queued_;
        queuedSamples_ += samples;
        if (++decoded_ == 1)
            printf("First Opus packet decoded: %d samples/channel.\n", static_cast<int>(samples));
    }

    // Build about 60 ms of audio before starting, including after an underrun.
    if (!playing_ && (queuedSamples_ >= SampleRate * 60 / 1000 || queued_ == BufferCount))
    {
        checkAudioResult(audoutStartAudioOut(), "Audio playback start");
        playing_ = true;
        printf("Opus audio playback started.\n");
    }
    logStats();
}

void OpusAudioPlayer::logStats()
{
    const auto now = std::chrono::steady_clock::now();
    if (now - lastReport_ < std::chrono::seconds(3))
        return;
    lastReport_ = now;
    std::size_t pending = 0;
    std::size_t dropped = 0;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        pending = pending_.size();
        dropped = dropped_;
    }
    printf("Audio: decoded %zu packets, queued %zu buffers (~%.0f ms), pending %zu, "
        "dropped %zu, decode errors %zu, underruns %zu\n",
        decoded_, queued_, queuedSamples_ * 1000.0 / SampleRate, pending,
        dropped, decodeErrors_, underruns_);
}

void OpusAudioPlayer::stop()
{
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
            return;
        stopped_ = true;
        pending_.clear();
    }
    if (outputReady_)
    {
        const Result result = audoutStopAudioOut();
        if (R_FAILED(result))
            printf("Audio stop failed: 0x%08X\n", static_cast<unsigned int>(result));
        audoutExit();
        outputReady_ = false;
    }
    for (auto& slot : slots_)
    {
        std::free(slot.buffer.buffer);
        slot.buffer = {};
        slot.queued = false;
    }
    if (decoderReady_)
    {
        hwopusDecoderExit(&decoder_);
        decoderReady_ = false;
        printf("Opus audio cleanup complete.\n");
    }
    playing_ = false;
}
