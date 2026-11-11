#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>
#include <string_view>

namespace luna::launcher
{
// Render Luna's limited rich-text descriptions as text, never as markup or links.
inline std::string descriptionText(std::string_view input, std::size_t inputLimit = 32768, std::size_t outputLimit = 8192)
{
    if (input.size() > inputLimit) return {};
    std::string result;
    bool space = false;
    const auto append = [&](std::uint32_t c)
    {
        if (c <= 32 || c == 127 || c == 160) { space = !result.empty(); return; }
        if (c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return;
        if (space) { result += ' '; space = false; }
        if (c < 128) result += char(c);
        else if (c < 0x800) { result += char(0xc0 | (c >> 6)); result += char(0x80 | (c & 63)); }
        else if (c < 0x10000) { result += char(0xe0 | (c >> 12)); result += char(0x80 | ((c >> 6) & 63)); result += char(0x80 | (c & 63)); }
        else { result += char(0xf0 | (c >> 18)); result += char(0x80 | ((c >> 12) & 63)); result += char(0x80 | ((c >> 6) & 63)); result += char(0x80 | (c & 63)); }
    };
    for (std::size_t at = 0; at < input.size();)
    {
        if (result.size() > outputLimit) return {};
        if (input[at] == '<')
        {
            const auto end = input.find('>', at + 1);
            if (end == std::string_view::npos) { append('<'); ++at; continue; }
            auto tag = std::string(input.substr(at + 1, end - at - 1));
            std::transform(tag.begin(), tag.end(), tag.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            const auto nameEnd = tag.find_first_of(" /\t\r\n");
            const auto name = tag.substr(0, nameEnd);
            if (name == "script" || name == "style")
            {
                // Do not expose executable/style contents as game prose.
                auto rest = std::string(input.substr(end + 1));
                std::transform(rest.begin(), rest.end(), rest.begin(), [](unsigned char c) { return char(std::tolower(c)); });
                const auto close = rest.find("</" + name);
                if (close == std::string::npos) break;
                const auto closeEnd = rest.find('>', close);
                if (closeEnd == std::string::npos) break;
                at = end + 2 + closeEnd; space = !result.empty(); continue;
            }
            if (name == "p" || name == "br" || name == "li" || (!tag.empty() && tag[0] == '/')) space = !result.empty();
            at = end + 1; continue;
        }
        if (input[at] == '&')
        {
            const auto end = input.find(';', at + 1);
            if (end != std::string_view::npos && end - at <= 12)
            {
                const auto entity = input.substr(at + 1, end - at - 1);
                std::uint32_t code = 0;
                if (entity == "amp") code = '&'; else if (entity == "lt") code = '<';
                else if (entity == "gt") code = '>'; else if (entity == "quot") code = '"';
                else if (entity == "apos" || entity == "#39") code = '\''; else if (entity == "nbsp") code = 160;
                else if (!entity.empty() && entity[0] == '#')
                {
                    const bool hex = entity.size() > 1 && (entity[1] == 'x' || entity[1] == 'X');
                    bool valid = entity.size() > (hex ? 2u : 1u);
                    for (std::size_t i = hex ? 2 : 1; i < entity.size() && valid; ++i)
                    {
                        const char c = entity[i];
                        const unsigned digit = c >= '0' && c <= '9' ? c - '0' : hex && c >= 'a' && c <= 'f' ? c - 'a' + 10 : hex && c >= 'A' && c <= 'F' ? c - 'A' + 10 : 99;
                        if (digit >= (hex ? 16u : 10u) || code > 0x10ffff / (hex ? 16u : 10u)) valid = false;
                        else code = code * (hex ? 16 : 10) + digit;
                    }
                    if (!valid) code = 0;
                }
                if (code) { append(code); at = end + 1; continue; }
            }
        }
        const auto c = static_cast<unsigned char>(input[at++]);
        if (c < 128) append(c);
        else { if (space) { result += ' '; space = false; } result += char(c); }
    }
    return result.size() <= outputLimit ? result : std::string();
}

// Consent text must retain link destinations as well as their labels. These
// are displayed for review on another device; no URL is executed or fetched.
inline std::string consentText(std::string_view input)
{
    constexpr std::size_t limit = 131072;
    if (input.size() > limit) return {};
    std::string expanded;
    for (std::size_t at = 0; at < input.size();)
    {
        if (input[at] != '<') { expanded += input[at++]; continue; }
        const auto end = input.find('>', at + 1);
        if (end == std::string_view::npos) return {};
        const auto raw = std::string(input.substr(at, end - at + 1));
        auto lower = raw;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        expanded += raw;
        if (lower.size() > 2 && lower[1] == 'a' && std::isspace(static_cast<unsigned char>(lower[2])))
        {
            auto href = lower.find("href");
            while (href != std::string::npos && (href == 0 ||
                !std::isspace(static_cast<unsigned char>(lower[href - 1])))) href = lower.find("href", href + 4);
            if (href == std::string::npos) return {};
            auto value = lower.find_first_not_of(" \t\r\n", href + 4);
            if (value == std::string::npos || lower[value++] != '=') return {};
            value = lower.find_first_not_of(" \t\r\n", value);
            if (value == std::string::npos || (raw[value] != '\'' && raw[value] != '"')) return {};
            const auto close = raw.find(raw[value], value + 1);
            if (close == std::string::npos) return {};
            const auto url = raw.substr(value + 1, close - value - 1);
            if (url.size() > 4096 || (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0) ||
                url.find_first_of("<>\r\n\t ") != std::string::npos) return {};
            expanded += " [" + url + "] ";
        }
        at = end + 1;
        if (expanded.size() > limit * 2) return {};
    }
    return descriptionText(expanded, limit * 2, limit);
}
}
