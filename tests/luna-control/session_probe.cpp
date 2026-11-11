#include "local_control_session.hpp"

#include <iomanip>
#include <iostream>
#include <stdexcept>

namespace control = luna::control;
control::Bytes fromHex(const std::string& value)
{
    if (value == "-") return {};
    if (value.size() % 2) throw std::runtime_error("Odd hex");
    control::Bytes bytes;
    for (std::size_t i = 0; i < value.size(); i += 2)
    {
        const auto part = value.substr(i, 2);
        if (part.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("Invalid hex");
        bytes.push_back(static_cast<std::uint8_t>(std::stoul(part, nullptr, 16)));
    }
    return bytes;
}
int main(int argc, char**)
{
    try
    {
        std::int64_t elapsed = 0;
        control::LocalControlSession session([&] { return control::ClockReading{1000000 + elapsed, elapsed}; }, argc > 1);
        bool failNext = false;
        const auto send = [&](const control::Bytes& bytes)
        {
            if (failNext) { failNext = false; throw std::runtime_error("Injected send failure"); }
            std::cout << "OUT ";
            for (auto byte : bytes)
                std::cout << std::hex << std::setfill('0') << std::setw(2) << static_cast<unsigned>(byte);
            std::cout << std::dec << '\n';
        };
        std::string command;
        while (std::cin >> command)
        {
            if (command == "tick")
            {
                bool open = false, buffered = false;
                if (!(std::cin >> elapsed >> open >> buffered)) throw std::runtime_error("Bad tick");
                session.update(open, buffered, send);
                std::cout << "STATUS " << session.statusJson() << '\n';
            }
            else if (command == "frame" || command == "flood")
            {
                unsigned count = 1;
                if (command == "flood" && !(std::cin >> count)) throw std::runtime_error("Bad flood");
                if (count > 1000) throw std::runtime_error("Flood exceeds probe limit");
                std::string hex;
                if (!(std::cin >> hex)) throw std::runtime_error("Bad frame");
                const auto bytes = fromHex(hex);
                unsigned accepted = 0;
                for (unsigned i = 0; i < count; ++i)
                    if (session.enqueue(bytes.data(), bytes.size())) ++accepted;
                std::cout << "QUEUED " << accepted << '\n';
            }
            else if (command == "fail") failNext = true;
            else if (command == "stop")
            {
                session.stop();
                std::cout << "STATUS " << session.statusJson() << '\n';
            }
            else throw std::runtime_error("Unknown session probe command");
        }
    }
    catch (const std::exception& error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
