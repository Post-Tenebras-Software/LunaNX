#include "launcher_image_cache.hpp"
#include "launcher_media.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace luna::launcher
{
namespace fs = std::filesystem;
namespace
{
constexpr std::uint32_t Magic = 0x31414e4c; // LNA1, versioned cache format.
bool cacheFile(const fs::directory_entry& file)
{
    const auto name = file.path().filename().string();
    return file.is_regular_file() && !file.is_symlink() && name.size() == 20 &&
        name.substr(16) == ".bin" && name.substr(0, 16).find_first_not_of("0123456789abcdef") == std::string::npos;
}
}
std::string ImageDiskCache::path(const std::string& url) const
{
    // Filename hash is not an integrity/authentication mechanism. Every cache
    // read compares the entire URL too, so collisions only cause cache misses.
    std::uint64_t hash = 14695981039346656037ull;
    for (const unsigned char c : url) { hash ^= c; hash *= 1099511628211ull; }
    std::ostringstream name; name << std::hex << std::setfill('0') << std::setw(16) << hash << ".bin";
    return (fs::path(directory_) / name.str()).string();
}
std::vector<unsigned char> ImageDiskCache::load(const std::string& url) const
{
    if (!artworkUrl(url)) return {};
    try
    {
        const auto file = path(url);
        if (!fs::is_regular_file(file) || fs::is_symlink(file)) return {};
        const auto length = fs::file_size(file);
        if (length < 12 || length > MaxEntry + 2048 + 12) return {};
        std::ifstream input(file, std::ios::binary);
        std::uint32_t header[3]{};
        input.read(reinterpret_cast<char*>(header), sizeof(header));
        if (!input || header[0] != Magic || header[1] != url.size() || !header[2] || header[2] > MaxEntry ||
            length != sizeof(header) + header[1] + header[2]) return {};
        std::string stored(header[1], '\0'); input.read(stored.data(), stored.size());
        if (!input || stored != url) return {};
        std::vector<unsigned char> bytes(header[2]); input.read(reinterpret_cast<char*>(bytes.data()), bytes.size());
        if (!input) return {};
        std::error_code error;
        fs::last_write_time(file, fs::file_time_type::clock::now(), error);
        return bytes;
    }
    catch (...) { return {}; }
}
void ImageDiskCache::save(const std::string& url, const std::vector<unsigned char>& bytes) const
{
    if (!artworkUrl(url) || bytes.empty() || bytes.size() > MaxEntry || bytes.size() + url.size() + 12 > limit_) return;
    try
    {
        fs::create_directories(directory_);
        const auto destination = path(url), temporary = destination + ".tmp";
        const std::uint32_t header[] = {Magic, static_cast<std::uint32_t>(url.size()), static_cast<std::uint32_t>(bytes.size())};
        {
            std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
            output.write(reinterpret_cast<const char*>(header), sizeof(header)); output.write(url.data(), url.size());
            output.write(reinterpret_cast<const char*>(bytes.data()), bytes.size()); output.close();
            if (!output) { fs::remove(temporary); return; }
        }
        std::error_code error;
        fs::rename(temporary, destination, error);
        if (error) fs::remove(temporary);
        prune();
    }
    catch (...) {} // A missing/read-only/full SD card must not stop browsing.
}
void ImageDiskCache::prune() const
{
    struct File { fs::path path; std::uint64_t size; fs::file_time_type used; };
    std::vector<File> files;
    std::uint64_t total = 0;
    for (const auto& file : fs::directory_iterator(directory_))
        if (cacheFile(file))
        { const auto size = file.file_size(); files.push_back({file.path(), size, file.last_write_time()}); total += size; }
    std::sort(files.begin(), files.end(), [](const auto& a, const auto& b) { return a.used < b.used; });
    auto remaining = files.size();
    for (const auto& file : files)
    {
        if (total <= limit_ && remaining <= 256) break;
        fs::remove(file.path); total -= file.size; --remaining;
    }
}
}
