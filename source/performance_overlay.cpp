#include "performance_overlay.hpp"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace luna::video
{
struct PerformanceOverlay::Impl
{
    struct Glyph { int width = 0, height = 0, left = 0, top = 0, advance = 0; std::vector<unsigned char> pixels; };
    std::array<Glyph, 95> glyphs;
    RgbaImage image;
    explicit Impl(const std::string& font)
    {
        FT_Library library = nullptr;
        if (FT_Init_FreeType(&library)) throw std::runtime_error("Overlay font unavailable");
        FT_Face face = nullptr;
        if (FT_New_Face(library, font.c_str(), 0, &face))
        { FT_Done_FreeType(library); throw std::runtime_error("Overlay font unavailable"); }
        try
        {
            if (FT_Set_Pixel_Sizes(face, 0, 17)) throw std::runtime_error("Overlay font size unavailable");
            for (unsigned i = 0; i < glyphs.size(); ++i)
            {
                if (FT_Load_Char(face, i + 32, FT_LOAD_RENDER)) throw std::runtime_error("Overlay glyph unavailable");
                const auto g = face->glyph;
                auto& saved = glyphs[i];
                saved.width = g->bitmap.width; saved.height = g->bitmap.rows;
                saved.left = g->bitmap_left; saved.top = g->bitmap_top; saved.advance = g->advance.x >> 6;
                saved.pixels.resize(saved.width * saved.height);
                for (int y = 0; y < saved.height; ++y)
                    std::memcpy(saved.pixels.data() + y * saved.width, g->bitmap.buffer + y * g->bitmap.pitch, saved.width);
            }
        }
        catch (...) { FT_Done_Face(face); FT_Done_FreeType(library); throw; }
        FT_Done_Face(face); FT_Done_FreeType(library);
        image.width = 540; image.height = 182; image.stride = image.width * 4;
        image.pixels.resize(image.stride * image.height);
    }
    void text(int y, const std::string& value, std::array<unsigned char, 3> color)
    {
        int x = 14;
        for (const unsigned char c : value)
        {
            const auto& g = glyphs[c >= 32 && c <= 126 ? c - 32 : '?' - 32];
            if (x + g.advance > image.width - 14) break;
            for (int yy = 0; yy < g.height; ++yy)
                for (int xx = 0; xx < g.width; ++xx)
                {
                    const int px = x + g.left + xx, py = y + 17 - g.top + yy;
                    if (px < 0 || px >= image.width || py < 0 || py >= image.height) continue;
                    const unsigned alpha = g.pixels[yy * g.width + xx];
                    auto* pixel = image.pixels.data() + py * image.stride + px * 4;
                    for (unsigned channel = 0; channel < 3; ++channel)
                        pixel[channel] = (color[channel] * alpha + pixel[channel] * (255 - alpha)) / 255;
                }
            x += g.advance;
        }
    }
    void render(const PerformanceSample& s)
    {
        for (std::size_t at = 0; at < image.pixels.size(); at += 4)
        { image.pixels[at] = 12; image.pixels[at + 1] = 15; image.pixels[at + 2] = 25; image.pixels[at + 3] = 255; }
        text(10, "LunaNX | LIVE STATS", {184,155,255});
        const auto cfg = configuration(s.mode);
        char line[192];
        std::snprintf(line, sizeof(line), "%dx%d @ %d FPS | %s | cap %u Mbps%s", cfg.width, cfg.height, cfg.fps,
            backendLabel(s.backend), bitrateRange(s.mode, s.bitrateMbps).maximum / 1'000'000, s.bitrateMbps ? "" : " (Auto)");
        text(35, line, {247,246,255});
        if (!s.ready) text(62, "Collecting first one-second sample...", {157,164,185});
        else
        {
            std::snprintf(line, sizeof(line), "FPS decoded %.1f / presented %.1f | video %.2f Mbps", s.decodedFps, s.presentedFps, s.encodedMbps);
            text(62, line, {247,246,255});
            std::snprintf(line, sizeof(line), "Replaced %zu | fallbacks %zu", s.replaced, s.fallbacks);
            text(87, line, {247,246,255});
            std::snprintf(line, sizeof(line), "ms decode %.2f | transfer %.2f | RGBA %.2f", s.decodeMs, s.transferMs, s.rgbaMs);
            text(112, line, {157,164,185});
            std::snprintf(line, sizeof(line), "ms present %.2f | loop %.2f", s.presentMs, s.loopMs);
            text(137, line, {157,164,185});
        }
        text(161, "Requested cap; received video payload rate. Samples: 1s.", {157,164,185});
    }
};
PerformanceOverlay::PerformanceOverlay(const std::string& font) : impl_(std::make_unique<Impl>(font)) {}
PerformanceOverlay::~PerformanceOverlay() = default;
const RgbaImage& PerformanceOverlay::draw(const PerformanceSample& sample)
{ impl_->render(sample); return impl_->image; }
}
