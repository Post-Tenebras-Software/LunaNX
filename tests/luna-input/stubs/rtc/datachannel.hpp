#pragma once

#include <cstddef>
#include <stdexcept>
#include <vector>

namespace rtc
{
using byte = std::byte;
class DataChannel
{
public:
    bool open = true;
    bool failNext = false;
    std::size_t buffered = 0;
    std::vector<std::vector<byte>> packets;
    bool isOpen() const { return open; }
    std::size_t bufferedAmount() const { return buffered; }
    bool send(const byte* data, std::size_t size)
    {
        if (failNext)
        {
            failNext = false;
            throw std::runtime_error("Injected send failure");
        }
        packets.emplace_back(data, data + size);
        return false; // Accepted into the buffer, matching libdatachannel semantics.
    }
};
}
