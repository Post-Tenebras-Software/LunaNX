#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace luna::launcher
{
// Stores only validated public image URLs and compressed image bytes.
// Failure is a cache miss; account/launch data never enters this API.
class ImageDiskCache
{
public:
    static constexpr std::size_t MaxEntry = 6 * 1024 * 1024;
    explicit ImageDiskCache(std::string directory, std::uint64_t limit = 64 * 1024 * 1024)
        : directory_(std::move(directory)), limit_(limit) {}
    std::vector<unsigned char> load(const std::string& url) const;
    void save(const std::string& url, const std::vector<unsigned char>& bytes) const;
private:
    std::string path(const std::string& url) const;
    void prune() const;
    std::string directory_;
    std::uint64_t limit_;
};
}
