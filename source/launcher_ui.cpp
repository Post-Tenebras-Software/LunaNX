#include "launcher_ui.hpp"
#include "build_info.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <cmath>
#include <map>
#include <stdexcept>
#include <tuple>

namespace luna::launcher
{
namespace
{
constexpr std::uint32_t rgb(unsigned r, unsigned g, unsigned b, unsigned a = 255)
{ return r | (g << 8) | (b << 16) | (a << 24); }
constexpr auto Background = rgb(12, 15, 25), Panel = rgb(24, 28, 43), Text = rgb(247, 246, 255),
    Muted = rgb(157, 164, 185), Purple = rgb(184, 155, 255), Line = rgb(47, 51, 71);
std::uint32_t blend(std::uint32_t base, std::uint32_t foreground)
{
    const unsigned a = foreground >> 24, b = 255 - a;
    return rgb(((base & 255) * b + (foreground & 255) * a) / 255,
        (((base >> 8) & 255) * b + ((foreground >> 8) & 255) * a) / 255,
        (((base >> 16) & 255) * b + ((foreground >> 16) & 255) * a) / 255);
}
std::uint32_t nextCodepoint(const std::string& text, std::size_t& at)
{
    const auto first = static_cast<unsigned char>(text[at++]);
    if (first < 128) return first;
    unsigned extra = first >= 0xf0 ? 3 : first >= 0xe0 ? 2 : first >= 0xc2 ? 1 : 0;
    if (!extra || at + extra > text.size()) return 0xfffd;
    std::uint32_t code = first & ((1u << (6 - extra)) - 1);
    for (unsigned i = 0; i < extra; ++i)
    {
        const auto c = static_cast<unsigned char>(text[at]);
        if ((c & 0xc0) != 0x80) return 0xfffd;
        ++at; code = (code << 6) | (c & 63);
    }
    if (code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff) ||
        (extra == 1 && code < 128) || (extra == 2 && code < 2048) || (extra == 3 && code < 65536)) return 0xfffd;
    return code;
}
}
struct Renderer::Impl
{
    FT_Library library = nullptr;
    FT_Face regular = nullptr, bold = nullptr;
    Image canvas{1280, 720, std::vector<std::uint32_t>(1280 * 720)};
    struct Glyph { int width, height, left, top, advance; std::vector<unsigned char> pixels; };
    std::map<std::tuple<std::uint32_t, int, bool>, Glyph> glyphs;
    Impl(const std::string& normal, const std::string& strong)
    {
        if (FT_Init_FreeType(&library)) throw std::runtime_error("Font engine unavailable");
        if (FT_New_Face(library, normal.c_str(), 0, &regular) || FT_New_Face(library, strong.c_str(), 0, &bold))
        {
            if (regular) FT_Done_Face(regular);
            if (bold) FT_Done_Face(bold);
            FT_Done_FreeType(library);
            throw std::runtime_error("Launcher fonts unavailable");
        }
    }
    ~Impl() { FT_Done_Face(regular); FT_Done_Face(bold); FT_Done_FreeType(library); }
    void pixel(int x, int y, std::uint32_t color)
    {
        if (x >= 0 && x < 1280 && y >= 0 && y < 720)
        { auto& p = canvas.pixels[y * 1280 + x]; p = color >> 24 == 255 ? color : blend(p, color); }
    }
    void box(int x, int y, int w, int h, std::uint32_t color, int radius = 0, int border = 0)
    {
        radius = std::max(0, std::min({radius, w / 2, h / 2}));
        for (int yy = std::max(0, y); yy < std::min(720, y + h); ++yy)
            for (int xx = std::max(0, x); xx < std::min(1280, x + w); ++xx)
            {
                const auto distance = [&](int inset)
                {
                    const int r = std::max(0, radius - inset);
                    const int dx = std::max({x + inset + r - xx, 0, xx - (x + w - inset - r - 1)});
                    const int dy = std::max({y + inset + r - yy, 0, yy - (y + h - inset - r - 1)});
                    return dx * dx + dy * dy <= r * r;
                };
                if (distance(0) && (!border || xx < x + border || xx >= x + w - border ||
                    yy < y + border || yy >= y + h - border || !distance(border))) pixel(xx, yy, color);
            }
    }
    void circle(int x, int y, int r, std::uint32_t color)
    { box(x - r, y - r, r * 2, r * 2, color, r); }
    Glyph& glyph(std::uint32_t code, int size, bool strong)
    {
        const auto key = std::make_tuple(code, size, strong);
        auto found = glyphs.find(key);
        if (found != glyphs.end()) return found->second;
        // Bound the glyph cache across long browsing sessions.
        if (glyphs.size() >= 2048) glyphs.clear();
        auto face = strong ? bold : regular;
        FT_Set_Pixel_Sizes(face, 0, size);
        if (FT_Load_Char(face, code, FT_LOAD_RENDER)) FT_Load_Char(face, '?', FT_LOAD_RENDER);
        const auto g = face->glyph;
        Glyph value{static_cast<int>(g->bitmap.width), static_cast<int>(g->bitmap.rows),
            g->bitmap_left, g->bitmap_top, static_cast<int>(g->advance.x >> 6), {}};
        value.pixels.resize(value.width * value.height);
        for (int row = 0; row < value.height; ++row)
            for (int col = 0; col < value.width; ++col)
                value.pixels[row * value.width + col] = g->bitmap.buffer[row * g->bitmap.pitch + col];
        return glyphs.emplace(key, std::move(value)).first->second;
    }
    int measure(const std::string& text, int size, bool strong = false)
    {
        int width = 0;
        for (std::size_t at = 0; at < text.size();) width += glyph(nextCodepoint(text, at), size, strong).advance;
        return width;
    }
    void text(int x, int y, const std::string& value, int size, std::uint32_t color = Text,
        bool strong = false, int maxWidth = 1200)
    {
        const int end = x + maxWidth;
        for (std::size_t at = 0; at < value.size();)
        {
            const auto code = nextCodepoint(value, at);
            if (code == '\n') break;
            const auto& g = glyph(code, size, strong);
            if (x + g.advance > end) break;
            for (int yy = 0; yy < g.height; ++yy)
                for (int xx = 0; xx < g.width; ++xx)
                {
                    const auto alpha = g.pixels[yy * g.width + xx];
                    if (alpha) pixel(x + g.left + xx, y + size - g.top + yy,
                        (color & 0xffffff) | (((color >> 24) * alpha / 255) << 24));
                }
            x += g.advance;
        }
    }
    int paragraph(int x, int y, const std::string& value, int size, int width, int maxLines,
        std::uint32_t color = Text, bool strong = false)
    {
        std::string line, word;
        int lines = 0;
        const auto flush = [&]
        { if (lines < maxLines) text(x, y + lines++ * (size + 7), line, size, color, strong, width); line.clear(); };
        const auto appendWord = [&]
        {
            if (!line.empty() && measure(line + " " + word, size, strong) > width) flush();
            if (!line.empty()) line += ' ';
            for (std::size_t at = 0; at < word.size() && lines < maxLines;)
            {
                const auto before = at;
                nextCodepoint(word, at);
                const auto character = word.substr(before, at - before);
                if (measure(line + character, size, strong) > width) flush();
                line += character;
            }
            word.clear();
        };
        for (std::size_t at = 0; at < value.size() && lines < maxLines;)
        {
            const auto before = at;
            const auto code = nextCodepoint(value, at);
            if (code == '\n' || code == ' ') { appendWord(); if (code == '\n') flush(); }
            else word += value.substr(before, at - before);
        }
        if (!word.empty() && lines < maxLines) appendWord();
        if (!line.empty() && lines < maxLines) flush();
        return lines * (size + 7);
    }
    std::vector<std::string> descriptionLines(const std::string& value)
    {
        std::vector<std::string> lines;
        std::string line, word;
        const auto flush = [&] { if (!line.empty()) lines.push_back(std::move(line)); line.clear(); };
        const auto append = [&]
        {
            if (word.empty()) return;
            if (!line.empty() && measure(line + " " + word, 20) > 1100) flush();
            if (!line.empty()) line += ' ';
            for (std::size_t at = 0; at < word.size();)
            {
                const auto begin = at; nextCodepoint(word, at);
                const auto character = word.substr(begin, at - begin);
                if (measure(line + character, 20) > 1100) flush();
                line += character;
            }
            word.clear();
        };
        for (std::size_t at = 0; at < value.size();)
        {
            const auto begin = at; const auto c = nextCodepoint(value, at);
            if (c == ' ' || c == '\n') { append(); if (c == '\n') flush(); }
            else word += value.substr(begin, at - begin);
        }
        append(); flush();
        return lines;
    }
    void shade(int x, int y, int w, int h, bool horizontal, unsigned from, unsigned to, std::uint32_t color = Background)
    {
        for (int yy = std::max(0, y); yy < std::min(720, y + h); ++yy)
            for (int xx = std::max(0, x); xx < std::min(1280, x + w); ++xx)
            {
                const float t = horizontal ? float(xx - x) / std::max(1, w - 1) : float(yy - y) / std::max(1, h - 1);
                const unsigned alpha = static_cast<unsigned>(from * (1 - t) + to * t);
                pixel(xx, yy, (color & 0xffffff) | (alpha << 24));
            }
    }
    bool image(const Image* source, int x, int y, int w, int h, int radius = 0)
    {
        if (!source || source->width <= 0 || source->height <= 0 ||
            source->pixels.size() != static_cast<std::size_t>(source->width) * source->height) return false;
        const double scale = std::max(double(w) / source->width, double(h) / source->height);
        for (int yy = 0; yy < h; ++yy)
            for (int xx = 0; xx < w; ++xx)
            {
                const int dx = std::max({radius - xx, 0, xx - (w - radius - 1)});
                const int dy = std::max({radius - yy, 0, yy - (h - radius - 1)});
                if (dx * dx + dy * dy > radius * radius) continue;
                const int sx = std::clamp(int((xx - w / 2) / scale + source->width / 2), 0, source->width - 1);
                const int sy = std::clamp(int((yy - h / 2) / scale + source->height / 2), 0, source->height - 1);
                pixel(x + xx, y + yy, source->pixels[sy * source->width + sx]);
            }
        return true;
    }
    void placeholder(int x, int y, int w, int h)
    {
        box(x, y, w, h, rgb(35, 34, 58), 12);
        shade(x, y, w, h, true, 0, 170);
        circle(x + w / 2, y + h / 2, 22, rgb(123, 103, 169, 80));
        circle(x + w / 2 + 10, y + h / 2 - 8, 21, rgb(35, 34, 58));
    }
    struct Prompt { std::string keys, label; bool enabled = true; };
    std::vector<std::string> promptKeys(const std::string& keys)
    {
        std::vector<std::string> result;
        for (std::size_t at = 0; at < keys.size();)
        {
            const auto separator = keys.find('/', at);
            auto key = keys.substr(at, separator == std::string::npos ? separator : separator - at);
            const auto first = key.find_first_not_of(' '), last = key.find_last_not_of(' ');
            if (first != std::string::npos) result.push_back(key.substr(first, last - first + 1));
            if (separator == std::string::npos) break;
            at = separator + 1;
        }
        return result;
    }
    int keyWidth(const std::string& key, int height)
    { return std::max(height, measure(key, height - 10, true) + 16); }
    int keysWidth(const std::string& keys, int height)
    {
        int width = 0;
        for (const auto& key : promptKeys(keys))
        { if (width) width += 15; width += keyWidth(key, height); }
        return width;
    }
    int keycaps(int x, int y, const std::string& keys, int height, bool inverted = false, bool enabled = true)
    {
        const int start = x;
        for (const auto& key : promptKeys(keys))
        {
            if (x != start) { text(x + 4, y + 3, "/", height - 10, enabled ? Text : Muted, true); x += 15; }
            const int width = keyWidth(key, height);
            const bool round = key == "A" || key == "B" || key == "X" || key == "Y" || key == "+" || key == "Minus";
            const auto fill = !enabled ? rgb(65, 69, 87) : inverted ? Background : Text;
            const auto ink = !enabled ? Muted : inverted ? Text : Background;
            box(x, y + 2, width, height, rgb(0, 0, 0, 70), round ? height / 2 : 6);
            box(x, y, width, height, fill, round ? height / 2 : 6);
            text(x + (width - measure(key, height - 10, true)) / 2, y + 3, key, height - 10, ink, true, width);
            x += width;
        }
        return x - start;
    }
    int prompt(int x, int y, const Prompt& hint, int size = 17, int maxWidth = 1168)
    {
        const auto width = keycaps(x, y, hint.keys, size + 11, false, hint.enabled);
        text(x + width + 10, y + 3, hint.label, size, hint.enabled ? Text : Muted, true, std::max(0, maxWidth - width - 10));
        return width + 10 + measure(hint.label, size, true);
    }
    void button(int x, int y, int w, const std::string& caption, bool primary = true, bool enabled = true,
        const std::string& key = "A")
    {
        box(x, y, w, 52, enabled ? (primary ? Purple : rgb(35, 39, 57)) : rgb(35, 38, 52), 26);
        if (enabled && !primary) box(x, y, w, 52, Line, 26, 1);
        const auto inset = key.empty() ? 24 : 20 + keycaps(x + 20, y + 12, key, 28, primary, enabled) + 12;
        text(x + inset, y + 13, caption, 20, enabled ? (primary ? Background : Text) : Muted, true, w - inset - 16);
    }
    void footer(const std::vector<Prompt>& hints)
    {
        box(0, 662, 1280, 58, rgb(20, 24, 38));
        box(48, 662, 1184, 1, Line);
        int size = 17, gap = 26;
        const auto total = [&]
        {
            int width = 0;
            for (const auto& hint : hints)
                width += keysWidth(hint.keys, size + 11) + 10 + measure(hint.label, size, true) + gap;
            return width - gap;
        };
        while (size > 15 && total() > 1168) { --size; gap = 20; }
        int x = 56;
        for (const auto& hint : hints)
            x += prompt(x, 677, hint, size, 1224 - x) + gap;
    }
    void header(const Model& m, const Artwork& artwork)
    {
        circle(67, 42, 17, Purple);
        circle(75, 35, 16, Background);
        text(94, 21, "Luna", 32, Text, true);
        text(171, 25, "NX", 25, Purple, true);
        box(215, 29, 49, 22, rgb(40, 33, 60), 6);
        text(226, 33, LUNANX_BUILD_CHANNEL, 10, Purple, true);
        text(330, 29, "Home", 19, Text, true);
        if (m.page == Page::Home || m.page == Page::Library || m.page == Page::Detail || m.page == Page::Gallery)
            box(m.browsing ? 423 : 330, 68, m.browsing ? 103 : 53, 3, Purple, 1);
        const bool catalog = m.page == Page::Home || m.page == Page::Library;
        if (catalog)
        {
            prompt(423, 27, {"R", "Catalog"}, 18, 165);
            prompt(600, 27, {"Y", "Search"}, 18, 145);
            prompt(764, 27, {"X", "Settings"}, 18, 170);
        }
        if (m.page == Page::Welcome || m.page == Page::Link || m.page == Page::Profiles ||
            m.page == Page::Detail || m.page == Page::Settings)
            prompt(423, 27, {"X", "Settings"}, 18, 180);
        if (!catalog) text(633, 35, "UNOFFICIAL CLIENT", 11, Muted, true);
        if (!m.profileName.empty())
        {
            const int width = std::min(185, measure(m.profileName, 17));
            text(1195 - width, 30, m.profileName, 17, Text, false, 185);
            if (!image(artwork(m.profileArtwork), 1206, 26, 32, 32, 16))
            {
                circle(1222, 42, 16, rgb(69, 51, 100));
                std::size_t first = 0; nextCodepoint(m.profileName, first);
                text(1217, 30, m.profileName.substr(0, first), 17, Text, true, 20);
            }
        }
        box(48, 84, 1184, 1, Line);
    }
    void busy(const Model& m, const std::string& heading, const std::string& message)
    {
        text(96, 195, heading, 43, Text, true, 1088);
        paragraph(98, 271, message, 21, 960, 4, Muted);
        for (unsigned i = 0; i < 9; ++i)
            box(99 + i * 38, 408, 27, 5, i == m.animation % 9 ? Purple : rgb(55, 46, 77), 2);
    }
    void render(const Model& m, const Artwork& artwork)
    {
        std::fill(canvas.pixels.begin(), canvas.pixels.end(), Background);
        header(m, artwork);
        if (m.page == Page::Home && (m.hasRemoteGame || (m.recentGames && !m.recentGames->empty())))
        {
            const auto recentCount = m.recentGames ? m.recentGames->size() : 0;
            const auto recent = recentCount ? std::min(m.selectedRecent, recentCount - 1) : 0;
            const auto count = m.games ? m.games->size() : 0;
            const auto selected = count ? std::min(m.selectedGame, count - 1) : 0;
            const bool focusRecent = !m.remoteFocused && recentCount && (m.recentFocused || !count);
            const auth::Game empty;
            const auto& game = focusRecent ? (*m.recentGames)[recent].game : count ? (*m.games)[selected] : empty;
            if (!image(artwork(m.hasRemoteGame ? m.remoteGameArtwork : game.artwork), 490, 85, 790, 132)) placeholder(490, 85, 790, 132);
            shade(480, 85, 800, 132, true, 255, 0);
            shade(490, 174, 790, 44, false, 0, 255);
            text(56, 102, m.hasRemoteGame ? "YOUR ACTIVE LUNA GAME" : focusRecent ? "JUMP BACK IN" : "PICK UP YOUR NEXT ADVENTURE", 12, Purple, true);
            paragraph(54, 128, m.hasRemoteGame ? m.remoteGameTitle : game.title, m.hasRemoteGame ? 27 : 34, 1100, m.hasRemoteGame ? 1 : 2, Text, true);
            if (m.hasRemoteGame)
            {
                button(56, 171, 282, "Return to game", m.remoteFocused && m.remoteAction == 0, true,
                    m.remoteFocused && m.remoteAction == 0 ? "A" : "");
                button(354, 171, 206, "End Game", m.remoteFocused && m.remoteAction == 1, true,
                    m.remoteFocused && m.remoteAction == 1 ? "A" : "");
                text(584, 189, m.remoteMessage.empty() ? "Stream disconnected. Your Luna game may still be active." : m.remoteMessage,
                    15, Muted, false, 636);
            }
            text(56, m.hasRemoteGame ? 240 : 233, "Recently Played", 23, focusRecent ? Text : Muted, true);
            if (recentCount) text(1070, 240, std::to_string(recent + 1) + " / " + std::to_string(recentCount), 15, Muted);
            const auto start = recent / 4 * 4;
            for (std::size_t i = start; i < recentCount && i < start + 4; ++i)
            {
                const int x = 56 + static_cast<int>(i - start) * 298;
                if (focusRecent && i == recent) box(x - 5, 273, 286, 136, Purple, 13, 3);
                const auto& tile = (*m.recentGames)[i].game;
                if (!image(artwork(tile.artwork), x, 278, 276, 126, 9)) placeholder(x, 278, 276, 126);
                text(x, 415, tile.title, 16, focusRecent && i == recent ? Text : Muted, focusRecent && i == recent, 275);
            }
            if (!recentCount) text(57, 308, "Your played games will appear here.", 20, Muted);
            text(56, 459, "Your Luna games", 23, m.remoteFocused || focusRecent ? Muted : Text, true);
            if (count)
            {
                text(1070, 466, std::to_string(selected + 1) + " / " + std::to_string(count), 15, Muted);
                const auto first = selected / 4 * 4;
                for (std::size_t i = first; i < count && i < first + 4; ++i)
                {
                    const int x = 56 + static_cast<int>(i - first) * 298;
                    if (!m.remoteFocused && !focusRecent && i == selected) box(x - 5, 498, 286, 136, Purple, 13, 3);
                    const auto& tile = (*m.games)[i];
                    if (!image(artwork(tile.artwork), x, 503, 276, 126, 9)) placeholder(x, 503, 276, 126);
                    text(x, 640, tile.title, 15, !m.remoteFocused && !focusRecent && i == selected ? Text : Muted,
                        !m.remoteFocused && !focusRecent && i == selected, 275);
                }
            }
            else text(57, 510, "Open Catalog to find your next game.", 20, Muted);
            if (!m.hasRemoteGame && !m.browseMessage.empty()) text(56, 211, m.browseMessage, 14, Purple, false, 1168);
            footer({{"D-pad / Stick", "Browse / rows"}, {"A", m.remoteFocused ? (m.remoteAction ? "End Game" : "Return to game") : "Game details"}, {"R", "Catalog"}, {"Y", "Search"}, {"B", "Profiles"}, {"+", "Exit"}});
        }
        else if (m.page == Page::Home && m.games && !m.games->empty())
        {
            const auto selected = std::min(m.selectedGame, m.games->size() - 1);
            const auto& game = (*m.games)[selected];
            if (!image(artwork(game.artwork), 490, 85, 790, 328)) placeholder(490, 85, 790, 328);
            shade(480, 85, 800, 328, true, 255, 0);
            shade(490, 310, 790, 104, false, 0, 255);
            text(56, 125, "PICK UP YOUR NEXT ADVENTURE", 13, Purple, true);
            paragraph(54, 158, game.title, 44, 650, 2, Text, true);
            text(57, 276, m.browseMessage.empty() ? "Your games. Ready when you are." : m.browseMessage,
                m.browseMessage.empty() ? 20 : 15, Muted, false, 630);
            button(56, 323, 204, "View game");
            if (!m.loginMessage.empty() && !m.rememberedLogin)
                text(280, 339, "Sign-in not saved - see Settings", 15, Muted);
            text(56, 405, "Your Luna games", 25, Text, true);
            const auto start = selected / 4 * 4;
            text(1070, 413, std::to_string(selected + 1) + " / " + std::to_string(m.games->size()), 16, Muted);
            for (std::size_t i = start; i < m.games->size() && i < start + 4; ++i)
            {
                const int x = 56 + static_cast<int>(i - start) * 298;
                if (i == selected) box(x - 5, 451, 286, 167, Purple, 15, 3);
                const auto& tile = (*m.games)[i];
                if (!image(artwork(tile.artwork), x, 456, 276, 155, 10)) placeholder(x, 456, 276, 155);
                text(x + 1, 624, tile.title, 17, i == selected ? Text : Muted, i == selected, 275);
            }
            footer({{"D-pad / Stick", "Browse"}, {"A", "Game details"}, {"R", "Catalog"}, {"Y", "Search"}, {"B", "Profiles"}, {"+", "Exit"}});
        }
        else if (m.page == Page::EndGame)
        {
            text(56, 131, m.replacementGameTitle.empty() ? "End this Luna game?" : "Switch to another game?", 39, Text, true);
            paragraph(58, 221, m.remoteGameTitle, 29, 1100, 2, Text, true);
            paragraph(58, 328, m.replacementGameTitle.empty() ?
                "This ends the remote game session." :
                "End the current session and start " + m.replacementGameTitle + "?",
                23, 1080, 3, Muted);
            text(58, 441, "Save your progress first. Unsaved progress in the current game may be lost.", 19, Text);
            button(56, 498, 280, m.replacementGameTitle.empty() ? "End Game" : "End & start");
            button(360, 498, 242, "Keep game", false, true, "B");
            footer({{"A", m.replacementGameTitle.empty() ? "End Game" : "End & start"}, {"B", "Keep game"}});
        }
        else if (m.page == Page::Library)
        {
            text(56, 108, m.searchQuery.empty() ? (m.includedOnly?"Included with your access":"Explore the Luna catalog") :
                (m.includedOnly?"Search your included games":"Search results"), 33, Text, true, 950);
            const auto count = m.games ? m.games->size() : 0;
            const auto selected = count ? std::min(m.selectedGame, count - 1) : 0;
            const auto start = selected / 8 * 8;
            const auto caption = !m.browseMessage.empty() ? m.browseMessage : !m.searchQuery.empty() ?
                "Results for \"" + m.searchQuery + "\"" : m.includedOnly ? "Games Luna reports as included with your current access." :
                "All games supplied by Luna, including titles requiring another subscription or purchase.";
            text(57, 153, caption, 17, m.browseMessage.empty() ? Muted : Purple, false, 1166);
            text(1045, 119, count ? std::to_string(m.catalogOffset + selected + 1) + " / " +
                std::to_string(m.catalogOffset + count) + (m.hasNextPage ? "+" : "") : "0 results", 16, Muted, false, 180);
            if (!count)
            {
                text(56, 291, m.browseMessage.empty() ? (m.includedOnly?"No included games on this page.":"No games found.") : "Results unavailable.", 32, Text, true);
                paragraph(57, 346, m.includedOnly?"Press L to show all games, Y to search, or B to return.":
                    "Press Y to try another search, or B to return. Nothing here changes your Luna subscriptions.", 20, 850, 3, Muted);
            }
            for (std::size_t i = start; i < count && i < start + 8; ++i)
            {
                const int x = 56 + static_cast<int>((i - start) % 4) * 298;
                const int y = 194 + static_cast<int>((i - start) / 4) * 222;
                if (i == selected) box(x - 5, y - 5, 286, 165, Purple, 15, 3);
                const auto& tile = (*m.games)[i];
                if (!image(artwork(tile.artwork), x, y, 276, 155, 10)) placeholder(x, y, 276, 155);
                paragraph(x, y + 167, tile.title, 17, 275, 2, i == selected ? Text : Muted, i == selected);
            }
            if (count) text(57, 636, "Showing " + std::to_string(m.catalogOffset + start + 1) + "-" +
                std::to_string(m.catalogOffset + std::min(start + 8, count)) +
                (m.hasNextPage ? "  /  More results available" : ""), 14, Muted);
            std::vector<Prompt> hints = {{"D-pad / Stick", "Browse"}, {"A", "Details"}, {"Y", "Search"},
                {"L",m.includedOnly?"Show all":"Included only"}};
            if (m.catalogOffset || start) hints.push_back({"ZL", "Previous"});
            if (start + 8 < count || m.hasNextPage) hints.push_back({"ZR", "Next"});
            hints.push_back({"B", m.browsing ? "Home" : "Profiles"});
            footer(hints);
        }
        else if (m.page == Page::Detail && m.detail)
        {
            const auto& d = *m.detail;
            if (!image(artwork(d.artwork), 495, 85, 785, 490)) placeholder(495, 85, 785, 490);
            shade(480, 85, 800, 490, true, 255, 5);
            shade(495, 385, 785, 191, false, 0, 255);
            text(56, 119, m.browsing ? "CATALOG  /  GAME DETAILS" : "HOME  /  GAME DETAILS", 13, Purple, true);
            paragraph(54, 161, d.title, 43, 710, 2, Text, true);
            paragraph(57, 282, d.description.empty() ? "Luna hasn't supplied a description for this game." : d.description, 19, 555, 5, Muted);
            button(56, 456, 256, d.buttonText.empty() ? std::string("Play") : d.buttonText, true, m.canPlay);
            if (!d.trailers.empty()) button(328, 456, 256, "Watch trailer", false, true, "ZL");
            text(57, 526, std::string(video::configuration(m.video).label) + "  /  Xbox button positions", 15, Muted);
            if (!m.canPlay) text(57, 558, "This Luna action needs a flow not supported by this version.", 15, Muted);
            if (!d.screenshots.empty())
            {
                text(820, 540, "SCREENSHOTS", 12, Purple, true);
                for (std::size_t i = 0; i < d.screenshots.size() && i < 3; ++i)
                {
                    const int x = 820 + static_cast<int>(i) * 129;
                    if (!image(artwork(d.screenshots[i]), x, 567, 117, 66, 7)) placeholder(x, 567, 117, 66);
                }
            }
            std::vector<Prompt> hints = {{"A", "Play / notices"}, {"B", m.browsing ? "Results" : "Home"}, {"X", "Settings"}};
            if (!d.description.empty()) hints.push_back({"Y", "About game"});
            if (!d.trailers.empty()) hints.push_back({"ZL", "Trailer"});
            if (!d.screenshots.empty()) hints.push_back({"ZR", "Screenshots"});
            hints.push_back({"+", "Exit"});
            footer(hints);
        }
        else if (m.page == Page::About && m.detail)
        {
            text(57, 119, "ABOUT THIS GAME", 13, Purple, true);
            paragraph(54, 158, m.detail->title, 34, 1120, 2, Text, true);
            const auto lines = descriptionLines(m.detail->description);
            const auto pages = std::max<std::size_t>(1, (lines.size() + 12) / 13);
            const auto page = std::min(m.descriptionPage, pages - 1);
            for (std::size_t i = page * 13; i < lines.size() && i < page * 13 + 13; ++i)
                text(58, 247 + int(i - page * 13) * 27, lines[i], 20, Text, false, 1100);
            text(58, 628, "Page " + std::to_string(page + 1) + " / " + std::to_string(pages), 14, Muted);
            footer({{"L / R", "Pages"}, {"B", "Game details"}, {"+", "Exit"}});
        }
        else if (m.page == Page::Trailer)
        {
            if (m.trailerFailed)
            {
                text(57, 154, "TRAILER", 13, Purple, true);
                paragraph(54, 205, "Trailer unavailable.", 39, 1120, 2, Text, true);
                paragraph(58, 300, m.message, 21, 1080, 5, Muted);
            }
            else busy(m, "Getting your trailer ready.", m.message);
            footer({{"B", "Game details"}, {"+", "Exit"}});
        }
        else if (m.page == Page::Gallery && m.detail && !m.detail->screenshots.empty())
        {
            const auto index = std::min(m.screenshot, m.detail->screenshots.size() - 1);
            if (!image(artwork(m.detail->screenshots[index]), 160, 109, 960, 540, 12)) placeholder(160, 109, 960, 540);
            box(1000, 122, 103, 31, rgb(12, 15, 25, 220), 15);
            text(1020, 128, std::to_string(index + 1) + " / " + std::to_string(m.detail->screenshots.size()), 15, Text, true);
            footer({{"Left / Right", "Screenshots"}, {"B", "Game details"}, {"+", "Exit"}});
        }
        else if (m.page == Page::ProfileResult)
        {
            text(56,153,"LUNA PROFILES",13,Purple,true);
            paragraph(54,205,m.title,39,1120,2,Text,true);
            paragraph(58,325,m.message,22,1080,5,Muted);
            footer({{"B","Manage Profiles"},{"+","Exit"}});
        }
        else if (m.page == Page::ManageProfiles && m.managedProfiles)
        {
            text(56, 131, "Manage Profiles", 39, Text, true);
            text(58, 192, "Names and avatars saved to your Luna account.", 20, Muted);
            const auto start=m.selected/4*4;
            for (std::size_t i=start;i<m.managedProfiles->size()&&i<start+4;++i)
            {
                const int x=56+static_cast<int>(i-start)*298; const auto& p=(*m.managedProfiles)[i];
                box(x,268,276,270,Panel,20);
                if (i==m.selected) box(x,268,276,270,Purple,20,3);
                if (!image(artwork(p.artwork),x+74,295,128,128,24))
                { circle(x+138,359,48,rgb(69,51,100)); text(x+125,339,"NX",21,Purple,true); }
                text(x+22,449,p.name,23,Text,true,232);
                text(x+22,493,p.editable?"Edit name and avatar":
                    (p.blockedBy & auth::ProfileManager::MissingRoute)?"Edit route unavailable":"Requires setup in Luna",
                    15,p.editable?Purple:Muted,false,232);
            }
            if (m.managedProfiles->empty()) text(58,302,"No profiles available to edit.",24,Muted);
            text(58,580,m.message.empty()?(m.canCreateProfile?"Press Y to create a standard adult profile.":"New-profile creation requires setup in Luna."):m.message,
                18,m.message.empty()?Muted:Purple,false,1160);
            footer({{"D-pad / Stick","Choose"},{"A","Edit"},{"Y","New profile"},{"ZR","Refresh"},{"B","Profiles"},{"+","Exit"}});
        }
        else if (m.page == Page::EditProfile && m.profileForm)
        {
            const auto& f=*m.profileForm;
            text(56,116,f.creating?"Create a profile":"Edit profile",38,Text,true);
            text(58,174,f.creating?"Standard adult profile":"Your changes apply to your Amazon account.",18,Muted);
            box(56,223,1168,69,Panel,12);
            if (m.profileRow==0) box(56,223,1168,69,Purple,12,2);
            text(76,235,"PROFILE NAME",12,Purple,true);
            text(76,256,f.name.empty()?"Press A to enter a name":f.name,22,Text,true,1080);
            text(58,313,"LUNA AVATAR",13,Purple,true);
            const auto start=f.selectedAvatar/8*8;
            for (std::size_t i=start;i<f.avatars.size()&&i<start+8;++i)
            {
                const int x=56+static_cast<int>(i-start)*146;
                box(x,345,130,130,Panel,16);
                if (i==f.selectedAvatar) box(x,345,130,130,m.profileRow==1?Purple:Line,16,m.profileRow==1?3:2);
                if (!image(artwork(f.avatars[i].artwork),x+8,353,114,114,12)) circle(x+65,410,35,rgb(69,51,100));
            }
            text(58,491,"Gamer tag: "+f.gamertag,17,Muted,false,900);
            if (!f.avatars.empty()) text(1070,491,std::to_string(f.selectedAvatar+1)+" / "+std::to_string(f.avatars.size()),15,Muted);
            button(56,535,250,f.creating?"Create profile":"Save changes",m.profileRow==2,m.canSaveProfile);
            if (!m.message.empty()) text(58,612,m.message,16,Purple,false,1160);
            if (m.confirmProfileSave)
            {
                box(0,85,1280,577,rgb(5,7,15,235)); box(190,220,900,260,Panel,20);
                text(225,252,f.creating?"Create this Luna profile?":"Save these profile changes?",29,Text,true,830);
                text(226,311,"Name: "+f.name,22,Text,false,670);
                text(226,359,"This will update your Amazon account.",19,Muted);
                if (f.selectedAvatar<f.avatars.size())
                {
                    if (!image(artwork(f.avatars[f.selectedAvatar].artwork),944,294,96,96,16)) circle(992,342,43,rgb(69,51,100));
                    text(944,400,"Avatar "+std::to_string(f.selectedAvatar+1),13,Muted);
                }
                prompt(226,416,{"A","Confirm"},20,300); prompt(565,416,{"B","Keep editing"},20,300);
            }
            if (m.confirmProfileSave) footer({{"A","Confirm save"},{"B","Keep editing"}});
            else footer({{"Up / Down","Field"},{"Left / Right","Avatar"},{"A","Edit / Save"},{"B","Cancel"},{"+","Exit"}});
        }
        else if (m.page == Page::Profiles && m.profiles)
        {
            text(56, 143, "Who's playing?", 43, Text, true);
            text(58, 207, "Choose your Luna profile to find your next game.", 20, Muted);
            const auto start = m.selected / 4 * 4;
            for (std::size_t i = start; i < m.profiles->size() && i < start + 4; ++i)
            {
                const int x = 56 + static_cast<int>(i - start) * 298;
                const auto& profile = (*m.profiles)[i];
                box(x, 288, 276, 265, Panel, 20);
                if (i == m.selected) box(x, 288, 276, 265, Purple, 20, 3);
                if (!image(artwork(profile.artwork),x+78,318,120,120,24))
                {
                    circle(x + 138, 378, 48, rgb(69, 51, 100));
                    circle(x + 138, 366, 13, Purple);
                    box(x + 112, 386, 52, 23, Purple, 11);
                }
                text(x + 22, 459, profile.name, 24, Text, true, 232);
                if (profile.block == auth::ProfileBlock::None) prompt(x + 22, 500, {"A", "Choose profile"}, 15, 232);
                else text(x + 22, 503, "Requires setup in Luna", 15, Muted);
            }
            if (!m.message.empty()) text(58, 587, m.message, 18, Purple, false, 1120);
            footer({{"D-pad / Stick", "Choose"}, {"A", "Confirm"}, {"Y", "Manage Profiles"}, {"B", "Cancel"}, {"+", "Exit"}});
        }
        else if (m.page == Page::Welcome || m.page == Page::Link)
        {
            circle(988, 341, 168, rgb(34, 27, 57));
            circle(988, 341, 119, rgb(53, 39, 82));
            circle(988, 341, 75, Purple);
            circle(1021, 314, 73, rgb(53, 39, 82));
            text(57, 150, "PLAY BEYOND YOUR CONSOLE", 13, Purple, true);
            paragraph(54, 193, m.page == Page::Link ? "Your next game\nis one sign-in away." : "A little console.\nA world of games.", 47, 720, 3, Text, true);
            paragraph(58, 340, m.page == Page::Link ? "On your phone or computer, open" :
                "Connect your Amazon account to browse your Luna games and stream directly to Nintendo Switch.", 21, 620, 3, Muted);
            if (m.page == Page::Link)
            {
                text(58, 379, "amazon.com/code", 28, Purple, true);
                box(56, 437, 600, 88, Panel, 16);
                text(81, 453, m.publicCode.empty() ? "Getting your code..." : m.publicCode, 40, Text, true, 550);
                text(59, 548, "Waiting for Amazon approval...", 18, Muted);
                footer({{"B", "Cancel sign-in"}, {"+", "Exit"}});
            }
            else
            {
                if (!m.message.empty()) paragraph(58, 408, m.message, 16, 620, 2, Muted);
                button(56, 461, 310, "Sign in with Amazon");
                text(58, 547, "An unofficial client by PostTenebrasSoftware", 16, Muted);
                text(58, 578, "Amazon Luna access and a network connection are required.", 14, Muted);
                footer({{"A", "Get started"}, {"X", "Settings"}, {"+", "Exit"}});
            }
        }
        else if (m.page == Page::Notice && m.notice)
        {
            box(48, 104, 1184, 548, Panel, 20);
            text(77, 122, m.title, 25, Text, true, 960);
            text(1100, 130, std::to_string(m.noticePage + 1) + " / " + std::to_string(m.noticePages), 15, Purple);
            for (std::size_t i = 0; i < m.noticeLines.size() && i < 16; ++i)
            {
                int size = 18;
                while (size > 12 && measure(m.noticeLines[i], size) > 1118) --size;
                text(78, 174 + static_cast<int>(i) * 21, m.noticeLines[i], size, Text, false, 1118);
            }
            const auto start = m.selected / 4 * 4;
            for (std::size_t i = start; i < m.notice->choices.size() && i < start + 4; ++i)
            {
                const int x = 78 + static_cast<int>((i - start) % 2) * 558;
                const int y = 526 + static_cast<int>((i - start) / 2) * 58;
                const auto action = m.notice->choices[i].action;
                const bool supported = action == auth::LaunchAction::GameSession || action == auth::LaunchAction::DirectLaunch ||
                    action == auth::LaunchAction::AcceptEula ||
                    m.notice->choices[i].actionTarget == "DIALOG_DISMISS";
                const bool allowed = supported && m.noticeComplete && m.noticePage + 1 == m.noticePages;
                button(x, y, 531, m.notice->choices[i].label, i == m.selected, allowed, i == m.selected ? "A" : "");
            }
            if (m.noticePage + 1 < m.noticePages)
                footer({{"L / R", "Read all pages"}, {"Left / Right", "Choose"}, {"B", "Back"}, {"+", "Exit"}});
            else footer({{"Left / Right", "Choose"}, {"A", "Confirm", m.noticeComplete}, {"L / R", "Pages"}, {"B", "Back"}, {"+", "Exit"}});
        }
        else if (m.page == Page::Settings)
        {
            text(56, 113, "Make yourself comfortable.", 34, Text, true);
            text(1000, 127, "LunaNX v" LUNANX_BUILD_VERSION, 14, Muted, false, 220);
            text(59, 165, "PREFERENCES", 13, Purple, true);
            box(56, 183, 1168, 56, Panel, 13);
            if (m.settingsRow == 0) box(56, 183, 1168, 56, Purple, 13, 2);
            text(82, 200, "Video quality", 20, Text, true);
            prompt(405, 198, {"A / Y", "Change preset"}, 14, 260);
            text(690, 197, video::configuration(m.video).label, 22, Purple, true, 500);
            box(56, 247, 1168, 56, Panel, 13);
            if (m.settingsRow == 1) box(56, 247, 1168, 56, Purple, 13, 2);
            text(82, 264, "Video decoder", 20, Text, true);
            prompt(405, 262, {"A / Y", "Change decoder"}, 14, 330);
            text(810, 261, video::decoderLabel(m.decoder), 22, Purple, true, 370);
            box(56, 311, 1168, 56, Panel, 13);
            if (m.settingsRow == 2) box(56, 311, 1168, 56, Purple, 13, 2);
            text(82, 328, "Bitrate limit", 20, Text, true);
            prompt(405, 326, {"A", "Change limit"}, 14, 330);
            text(810, 325, video::bitrateLabel(m.video, m.bitrateMbps), 22, Purple, true, 370);
            box(56, 375, 1168, 56, Panel, 13);
            if (m.settingsRow == 3) box(56, 375, 1168, 56, Purple, 13, 2);
            text(82, 392, "Performance overlay", 20, Text, true);
            prompt(405, 390, {"A / Y", "Toggle stats"}, 14, 330);
            text(810, 389, m.statsOverlay ? "On" : "Off", 22, Purple, true, 370);
            box(56, 439, 1168, 72, Panel, 13);
            if (m.settingsRow == 4) box(56, 439, 1168, 72, Purple, 13, 2);
            text(82, 445, m.recordingShortcut ? "Record your shortcut" : "Return to LunaNX", 20, Text, true);
            if (m.recordingShortcut) text(520, 449, "Waiting for buttons...", 20, Purple, true, 675);
            else keycaps(520, 449, m.menuShortcut, 26);
            text(82, 482, m.recordingShortcut ? "Release buttons, then hold 2-4 buttons; or hold modifiers and tap Capture." :
                m.shortcutInstructions + " Disconnects the stream and opens Home.", 15, Muted, false, 1100);
            box(56, 519, 1168, 64, Panel, 13);
            if (m.settingsRow == 5) box(56, 519, 1168, 64, Purple, 13, 2);
            text(82, 525, "Amazon sign-in", 20, Text, true);
            text(810, 535, m.rememberedLogin ? "Remembered" : m.signedIn ? "This session only" : "Not signed in", 21, Purple, true, 370);
            prompt(82, 554, {"A", m.forgetLoginConfirmation ? "Confirm forget and sign out" : "Forget saved sign-in and sign out"}, 14, 700);
            std::string help = m.recordingShortcut ? "Hold 2-4 buttons briefly to save; or hold modifiers and tap Capture. B alone cancels." :
                m.settingsRow == 5 ? m.loginMessage : m.settingsRow == 4 ? m.shortcutStatus :
                m.settingsRow == 3 ? (m.overlayShortcutAvailable ? "Hold Minus + R for 1 second during play to show or hide live stats. Your preference is saved." : "The return shortcut overlaps Minus + R. Change the overlay here or choose a different return shortcut.") :
                m.settingsRow == 2 ? "A higher limit can improve clarity on a strong connection. Luna adapts within the requested range; actual bitrate may be lower. Y restores Auto." :
                m.settingsRow == 1 ? "Auto uses NVDEC with software fallback. Frame conversion and display still use the CPU." :
                (m.docked ? "TV mode: 1080p presets are available. Changes apply to the next game." :
                "Handheld mode: up to 720p. Changes apply to the next game.");
            if (m.settingsRow < 4 && !m.videoPreferencesMessage.empty()) help = m.videoPreferencesMessage + " " + help;
            paragraph(60, 592, help, 16, 1110, 2, Muted);
            if (m.recordingShortcut) footer({{"B", "Cancel recording"}});
            else if (m.forgetLoginConfirmation) footer({{"A", "Forget and sign out"}, {"B", "Cancel"}});
            else
            {
                std::vector<Prompt> hints = {{"Up / Down", "Select"}, {"A", "Change / select"}};
                if (m.settingsRow < 5) hints.push_back({"Y", m.settingsRow == 4 ? "Reset shortcut" : m.settingsRow == 3 ? "Toggle overlay" : m.settingsRow == 2 ? "Auto bitrate" : m.settingsRow == 1 ? "Cycle decoder" : "Cycle video"});
                hints.push_back({"B / X", "Return"});
                footer(hints);
            }
        }
        else if (m.page == Page::Connecting)
        {
            busy(m, m.title.empty() ? "Getting your game ready." : m.title, m.message);
            if (m.queuePosition)
            {
                box(794, 377, 430, 200, Panel, 20);
                text(821, 403, "YOUR PLACE IN LINE", 13, Purple, true);
                text(819, 433, std::to_string(*m.queuePosition), 65, Text, true);
                if (m.waitSeconds) text(824, 526, "Estimated wait: " + std::to_string((*m.waitSeconds + 59) / 60) + " min", 17, Muted);
            }
            if (m.profileSaving) footer({});
            else if (m.waitingForVideo) footer({{m.menuShortcut.c_str(), "Return to LunaNX"}});
            else footer({{"B", "Cancel"}, {"+", "Cancel and exit"}});
        }
        else if (m.page == Page::Error || m.page == Page::Ended)
        {
            text(57, 153, m.page == Page::Ended ? "SESSION ENDED" : "LET'S TAKE A LOOK", 13, Purple, true);
            text(54, 201, m.title.empty() ? "We couldn't complete that." : m.title, 42, Text, true, 1150);
            paragraph(58, 283, m.message, 21, 1090, 6, Muted);
            if (m.remoteMayRemain) paragraph(58, 475, "Your Luna session may still be active. Closing LunaNX does not end a remote game.", 18, 1080, 2, Muted);
            footer({{"B", "Back"}, {"Minus + L", "Hold: debug console"}, {"+", "Exit"}});
        }
        else
        {
            busy(m, m.title.empty() ? "Connecting to Luna." : m.title, m.message);
            footer({{"B", "Cancel"}, {"+", "Exit"}});
        }
    }
};
Renderer::Renderer(const std::string& normal, const std::string& strong) : impl_(std::make_unique<Impl>(normal, strong)) {}
Renderer::~Renderer() = default;
const Image& Renderer::draw(const Model& m, const Artwork& artwork)
{ impl_->render(m, artwork); return impl_->canvas; }
std::size_t Renderer::descriptionPages(const std::string& text)
{ return std::max<std::size_t>(1, (impl_->descriptionLines(text).size() + 12) / 13); }
}
