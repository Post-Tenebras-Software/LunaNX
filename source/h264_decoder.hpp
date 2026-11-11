#pragma once

#include <cstddef>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <vector>
#include "video_decoder.hpp"
#include "h264_recovery.hpp"

struct AVCodecContext;
struct AVPacket;
struct AVFrame;
struct SwsContext;

struct RgbaImage
{
    static constexpr int MaxWidth = 1920;
    static constexpr int MaxHeight = 1080;

    int width = 0;
    int height = 0;
    int stride = 0;
    std::vector<std::uint8_t> pixels;
};

class H264Decoder
{
public:
    struct Stats
    {
        std::size_t framesDecoded = 0;
        std::size_t framesConverted = 0;
        std::size_t conversionAttempts = 0;
        std::size_t pendingReplaced = 0;
        std::size_t hardwareFrames = 0, transferAttempts = 0, fallbacks = 0;
        luna::video::Backend backend = luna::video::Backend::Software;
        bool waitingForKeyframe = false;
        std::chrono::nanoseconds decodeTime{};
        std::chrono::nanoseconds conversionTime{};
        std::chrono::nanoseconds transferTime{};
    };

    explicit H264Decoder(int outputWidth = 1280, int outputHeight = 720,
        luna::video::Decoder decoder = luna::video::Decoder::Software, int frameRate = 30);
    ~H264Decoder();

    H264Decoder(const H264Decoder&) = delete;
    H264Decoder& operator=(const H264Decoder&) = delete;

    void decode(const std::byte* data, std::size_t size);
    std::unique_ptr<RgbaImage> takeImage();
    Stats getStats();
    // Initialization capability, not proof of decoded hardware frames.
    bool hardwarePrepared();

private:
    int sendPacket();
    int openDecoder(bool hardware);
    void fallbackToSoftware();
    void hardwareError();
    void receiveFrames();
    AVFrame* downloadFrame();
    std::unique_ptr<RgbaImage> convertFrame(const AVFrame* frame);
    void reportError(const char* operation, int result);
    void release();

    std::mutex decoderMutex_;
    std::mutex imageMutex_;
    AVCodecContext* context_ = nullptr;
    AVPacket* packet_ = nullptr;
    AVFrame* frame_ = nullptr;
    AVFrame* download_ = nullptr;
    SwsContext* converter_ = nullptr;
    // One pending RGBA frame; publishing a newer frame replaces it.
    std::unique_ptr<RgbaImage> image_;
    Stats stats_;
    std::size_t errors_ = 0;
    int loggedWidth_ = 0;
    int loggedHeight_ = 0;
    int loggedPixelFormat_ = -1;
    int transferWidth_ = 0, transferHeight_ = 0, transferFormat_ = -1, hardwareFormat_ = -1;
    bool hardwareConfigured_ = false, hardwareRejected_ = false, fallbackPending_ = false;
    unsigned hardwareErrors_ = 0;
    H264Recovery recovery_;
    const int frameRate_;
    const int outputWidth_, outputHeight_;
};
