#pragma once
#include "trailer_decoder.hpp"
#include <curl/curl.h>
#include <memory>

namespace luna::launcher
{
class TrailerPlayer
{
public:
    enum class State { Idle, Loading, Buffering, Playing, Finished, Failed };
    using TlsSetup = CURLcode (*)(CURL*, void*, void*);
    explicit TrailerPlayer(TlsSetup setup);
    ~TrailerPlayer();
    TrailerPlayer(const TrailerPlayer&) = delete;
    TrailerPlayer& operator=(const TrailerPlayer&) = delete;
    void start(const std::string& url);
    void update();
    void stop();
    State state() const;
    std::string message() const;
    std::unique_ptr<RgbaImage> takeFrame();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
