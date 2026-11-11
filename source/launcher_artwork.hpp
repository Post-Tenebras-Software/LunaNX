#pragma once
#include "launcher_ui.hpp"
#include <curl/curl.h>

namespace luna::launcher
{
class ArtworkCache
{
public:
    using TlsSetup = CURLcode (*)(CURL*, void*, void*);
    explicit ArtworkCache(TlsSetup tls);
    ~ArtworkCache();
    const Image* find(const std::string& url);
    void update(const std::vector<std::string>& wanted, const std::vector<std::string>& prefetch, bool mayStart);
    void cancel();
    void clear();
    std::uint64_t revision() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
