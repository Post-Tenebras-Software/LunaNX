#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>

// Only used to restart a decoder at an IDR. FFmpeg still parses/validates the
// bitstream. Cached parameter NALs are bounded and never persisted or logged.
class H264Recovery
{
public:
    static constexpr std::size_t MaxAccessUnit = 4 * 1024 * 1024, MaxParameterSet = 64 * 1024;
    bool observe(const std::byte* data, std::size_t size)
    {
        if (!data || !size || size > MaxAccessUnit) return false;
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
        const auto prefix = [&](std::size_t at) -> std::size_t
        {
            if (at + 3 > size || bytes[at] || bytes[at + 1]) return 0;
            if (bytes[at + 2] == 1) return 3;
            return at + 4 <= size && !bytes[at + 2] && bytes[at + 3] == 1 ? 4 : 0;
        };
        bool idr = false;
        for (std::size_t at = 0; at < size;)
        {
            const auto start = prefix(at);
            if (!start) { ++at; continue; }
            const auto payload = at + start;
            auto end = payload;
            while (end < size && !prefix(end)) ++end;
            if (payload < end)
            {
                const auto type = bytes[payload] & 31;
                idr |= type == 5;
                if (type == 7 || type == 8)
                {
                    auto& saved = type == 7 ? sps_ : pps_;
                    saved.clear();
                    if (end - payload <= MaxParameterSet - 4)
                    {
                        saved = {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{1}};
                        saved.insert(saved.end(), data + payload, data + end);
                    }
                }
            }
            at = end;
        }
        return idr;
    }
    bool ready() const { return !sps_.empty() && !pps_.empty(); }
    std::vector<std::byte> parameters() const
    {
        auto result = sps_;
        result.insert(result.end(), pps_.begin(), pps_.end());
        return result;
    }
private:
    std::vector<std::byte> sps_, pps_;
};
