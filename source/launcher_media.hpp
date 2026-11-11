#pragma once
#include <algorithm>
#include <string>

namespace luna::launcher
{
inline bool trailerUrl(const std::string& url)
{
    if (url.size() > 4096 || url.find_first_of("@\\#") != std::string::npos ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c >= 127; })) return false;
    bool trusted = false;
    for (const auto* prefix : {"https://m.media-amazon.com/images/", "https://images-na.ssl-images-amazon.com/images/"})
        if (url.rfind(prefix, 0) == 0) trusted = true;
    const auto path = url.substr(0, url.find('?'));
    return trusted && path.size() > 4 && path.compare(path.size() - 4, 4, ".mp4") == 0;
}
// Artwork is fetched without account headers. Do not follow arbitrary URLs
// supplied by a page or send credentials to an image host.
inline bool artworkUrl(const std::string& url)
{
    if (url.size() > 2048 || url.find_first_of("?#@\\") != std::string::npos ||
        std::any_of(url.begin(), url.end(), [](unsigned char c) { return c <= 32 || c >= 127; })) return false;
    for (const auto* prefix : {"https://m.media-amazon.com/images/", "https://images-na.ssl-images-amazon.com/images/"})
        if (url.rfind(prefix, 0) == 0 && url.size() > std::char_traits<char>::length(prefix)) return true;
    return false;
}
inline std::string artworkRequestUrl(std::string url)
{
    if (!artworkUrl(url)) return {};
    // Luna's ImageStyleCodeBuilder (51706) replaces existing media style codes
    // before adding width/quality/format. Request JPEG explicitly: the browser
    // normally requests WebP, which this small artwork decoder does not handle.
    auto dot = url.find_last_of('.');
    if (dot == std::string::npos) return url;
    auto extension = url.substr(dot + 1);
    std::transform(extension.begin(), extension.end(), extension.begin(),
        [](unsigned char c) { return c >= 'A' && c <= 'Z' ? c + ('a' - 'A') : c; });
    if (extension != "jpg" && extension != "jpeg" && extension != "png" && extension != "webp" && extension != "eprt") return url;
    for (unsigned i = 0; i < 2; ++i)
    {
        const auto begin = url.find("._", url.find_last_of('/'));
        if (begin == std::string::npos) break;
        const auto end = url.find("_.", begin);
        if (end == std::string::npos) break;
        url.replace(begin, end + 2 - begin, ".");
    }
    dot = url.find_last_of('.');
    url.insert(dot, "._SX960_QL85_FMjpg_");
    return url;
}
}
