#include "launcher_shortcut.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

namespace luna::launcher
{
std::string shortcutLabel(std::uint64_t buttons)
{
    std::string label;
    for (const auto& item : {std::pair<std::uint64_t, const char*>{CaptureButton, "Capture"},
        {MinusButton, "Minus"}, {PlusButton, "Plus"}, {1ull << 6, "L"}, {1ull << 7, "R"},
        {1ull << 8, "ZL"}, {1ull << 9, "ZR"}, {1ull << 4, "L Stick"}, {1ull << 5, "R Stick"},
        {1ull << 0, "A"}, {1ull << 1, "B"}, {1ull << 2, "X"}, {1ull << 3, "Y"},
        {1ull << 12, "Left"}, {1ull << 13, "Up"}, {1ull << 14, "Right"}, {1ull << 15, "Down"}})
        if (buttons & item.first) { if (!label.empty()) label += " + "; label += item.second; }
    return label;
}
std::uint64_t loadShortcut(const std::string& path)
{
    try
    {
        if (std::filesystem::file_size(path) > 64) return DefaultMenuButtons;
        std::ifstream input(path);
        std::string header, value, extra;
        std::getline(input, header); std::getline(input, value);
        if (!input || header != "LUNANX_SHORTCUT_V1" || value.size() != 16 ||
            value.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos || std::getline(input, extra)) return DefaultMenuButtons;
        const auto buttons = std::stoull(value, nullptr, 16);
        return validShortcut(buttons) ? buttons : DefaultMenuButtons;
    }
    catch (...) { return DefaultMenuButtons; }
}
bool saveShortcut(const std::string& path, std::uint64_t buttons)
{
    if (!validShortcut(buttons)) return false;
    try
    {
        std::filesystem::create_directories(std::filesystem::path(path).parent_path());
        const auto temporary = path + ".tmp";
        std::ofstream output(temporary, std::ios::trunc);
        output << "LUNANX_SHORTCUT_V1\n" << std::hex << std::setw(16) << std::setfill('0') << buttons << '\n';
        output.close();
        if (!output) { std::filesystem::remove(temporary); return false; }
        std::error_code error;
        std::filesystem::rename(temporary, path, error);
        if (error) { std::filesystem::remove(temporary); return false; }
        return true;
    }
    catch (...) { return false; }
}
}
