#pragma once

// The installers' text: the font they carry (Bai Jamjuree, see
// UMVC3Hook/assets/README.md) drawn into the picture by stb_truetype, so the
// Windows and Linux windows show the same letters in the same places. GDI
// would fit each letter to whole pixels, which spreads and thins them.
//
// A character that font lacks (it is Latin and Thai) comes from a font of
// the system's, if the installer handed one over with Spare.
//
// Include from one source file only: it holds stb_truetype's code.

#include "install_ui.h"

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "imstb_truetype.h" // the copy that comes with Dear ImGui

#include <map>
#include <utility>

namespace install {

class FontText final : public TextPainter {
public:
    FontText(uint32_t* pixels, int width, int height)
        : pixels_(pixels), width_(width), height_(height), scale_(width / kScreenW)
    {
    }

    // The font the text is drawn in: one file for the text, one for titles
    // and buttons. False if either cannot be read.
    bool Use(const unsigned char* body, const unsigned char* bodyEnd,
             const unsigned char* title, const unsigned char* titleEnd)
    {
        return regular_.Use(body, bodyEnd) && bold_.Use(title, titleEnd);
    }

    // Fonts of the system's, for characters the ones above lack. Either may
    // be empty.
    void Spare(std::vector<unsigned char> regular, std::vector<unsigned char> bold)
    {
        spareRegular_.Take(std::move(regular));
        spareBold_.Take(std::move(bold));
        if (spareRegular_.loaded && !spareBold_.loaded) spareBold_ = spareRegular_, spareBold_.Rebind();
        if (spareBold_.loaded && !spareRegular_.loaded) spareRegular_ = spareBold_, spareRegular_.Rebind();
        glyphs_.clear();
    }

    // Whether every character of `text` can be drawn.
    bool Covers(FontId font, const wchar_t* text)
    {
        for (; *text; ++text) {
            if (*text < L' ') continue;
            const int character = static_cast<int>(*text);
            if (stbtt_FindGlyphIndex(&FaceOf(font).info, character)) continue;
            if (SpareOf(font).loaded && stbtt_FindGlyphIndex(&SpareOf(font).info, character)) continue;
            return false;
        }
        return true;
    }

    float BlockHeight(FontId font, const wchar_t* text, float width, unsigned flags) override
    {
        return Wrap(font, text, width * scale_, (flags & kTextPath) != 0).size() * LineHeight(font) /
                   scale_ + 2.0f;
    }

    void Draw(FontId font, const wchar_t* text, Rect rect, Color color, unsigned flags) override
    {
        if (!text[0]) return;
        const float left = rect.x * scale_, top = rect.y * scale_;
        const float boxW = rect.w * scale_, boxH = rect.h * scale_;
        const float lineH = LineHeight(font);
        std::vector<std::wstring> lines;
        float y = top;
        if (flags & (kTextMiddle | kTextRight)) {
            lines.push_back(Fit(font, text, boxW));
            y = top + (boxH - lineH) * 0.5f;
        } else {
            lines = Wrap(font, text, boxW, (flags & kTextPath) != 0);
        }
        for (const std::wstring& line : lines) {
            const float lineW = Measure(font, line);
            const float x = flags & kTextRight ? left + boxW - lineW : left + (boxW - lineW) * 0.5f;
            const float baseline = y + Ascent(font);
            if (flags & kTextOutline) {
                const int edge = scale_ >= 1.25f ? 2 : 1;
                for (int dy = -edge; dy <= edge; ++dy)
                    for (int dx = -edge; dx <= edge; ++dx)
                        if (dx || dy) DrawLine(font, line, x + dx, baseline + dy, lobby::Rgb(0x080A12));
            }
            DrawLine(font, line, x, baseline, color);
            y += lineH;
        }
    }

private:
    struct Face {
        std::vector<unsigned char> data;
        stbtt_fontinfo info{};
        bool loaded = false;

        // Plain TrueType and OpenType files; a collection's first face.
        bool Take(std::vector<unsigned char> file)
        {
            data = std::move(file);
            loaded = false;
            return data.size() >= 12 && Rebind();
        }
        bool Use(const unsigned char* begin, const unsigned char* end)
        {
            data.assign(begin, end);
            return data.size() >= 12 && Rebind();
        }
        bool Rebind()
        {
            const int offset = stbtt_GetFontOffsetForIndex(data.data(), 0);
            loaded = offset >= 0 && stbtt_InitFont(&info, data.data(), offset) != 0;
            return loaded;
        }
    };
    struct Glyph {
        int w = 0, h = 0, left = 0, top = 0;
        float advance = 0.0f;
        std::vector<unsigned char> cover;
    };

    static int Index(FontId font) { return static_cast<int>(font); }
    static bool Heading(FontId font) { return font == FontId::Title || font == FontId::Button; }
    Face& FaceOf(FontId font) { return Heading(font) ? bold_ : regular_; }
    Face& SpareOf(FontId font) { return Heading(font) ? spareBold_ : spareRegular_; }
    float Pixels(FontId font) const { return kFontHeight[Index(font)] * 0.80f * scale_; }
    float Scale(FontId font) { return stbtt_ScaleForMappingEmToPixels(&FaceOf(font).info, Pixels(font)); }
    float Ascent(FontId font)
    {
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&FaceOf(font).info, &ascent, &descent, &gap);
        return ascent * Scale(font);
    }
    float LineHeight(FontId font)
    {
        int ascent, descent, gap;
        stbtt_GetFontVMetrics(&FaceOf(font).info, &ascent, &descent, &gap);
        return (ascent - descent + gap) * Scale(font);
    }

    const Glyph& GlyphOf(FontId font, wchar_t character)
    {
        const uint64_t key = (static_cast<uint64_t>(Index(font)) << 32) | static_cast<uint32_t>(character);
        const auto found = glyphs_.find(key);
        if (found != glyphs_.end()) return found->second;
        Glyph glyph;
        const stbtt_fontinfo* info = &FaceOf(font).info;
        float scale = Scale(font);
        int index = stbtt_FindGlyphIndex(info, static_cast<int>(character));
        if (!index && SpareOf(font).loaded) {
            const stbtt_fontinfo* spare = &SpareOf(font).info;
            if (const int found = stbtt_FindGlyphIndex(spare, static_cast<int>(character))) {
                info = spare;
                index = found;
                scale = stbtt_ScaleForMappingEmToPixels(spare, Pixels(font));
            }
        }
        int advance = 0, bearing = 0;
        stbtt_GetGlyphHMetrics(info, index, &advance, &bearing);
        glyph.advance = advance * scale;
        if (unsigned char* bitmap = stbtt_GetGlyphBitmap(info, scale, scale, index, &glyph.w, &glyph.h,
                                                         &glyph.left, &glyph.top)) {
            glyph.cover.assign(bitmap, bitmap + static_cast<size_t>(glyph.w) * glyph.h);
            stbtt_FreeBitmap(bitmap, nullptr);
        }
        return glyphs_.emplace(key, std::move(glyph)).first->second;
    }

    float Measure(FontId font, const std::wstring& text)
    {
        float width = 0.0f;
        for (const wchar_t c : text) width += GlyphOf(font, c).advance;
        return width;
    }

    // One line no wider than `width`: what does not fit is replaced by dots.
    std::wstring Fit(FontId font, const std::wstring& text, float width)
    {
        if (Measure(font, text) <= width) return text;
        std::wstring head = text;
        while (head.size() > 1 && Measure(font, head + L"...") > width) head.pop_back();
        return head + L"...";
    }

    // Lines broken to fit `width`: at spaces, or for a path after a slash
    // (a folder name is kept whole where it can be). A run longer than a
    // line is cut where the line ends.
    std::vector<std::wstring> Wrap(FontId font, const std::wstring& text, float width, bool path = false)
    {
        std::vector<std::wstring> lines;
        std::wstring line;
        size_t keep = 0, resume = 0; // the last place to break: the line's length, and where the next starts
        for (size_t i = 0; i <= text.size(); ++i) {
            if (i == text.size() || text[i] == L'\n') {
                lines.push_back(line);
                line.clear();
                keep = resume = 0;
                continue;
            }
            const size_t before = line.size();
            line.push_back(text[i]);
            if (Measure(font, line) > width && before > 0) {
                const bool broken = keep > 0;
                lines.push_back(line.substr(0, broken ? keep : before));
                line = line.substr(broken ? resume : before);
                keep = resume = 0;
            }
            if (!path && text[i] == L' ') {
                keep = line.size() - 1; // the space itself is dropped
                resume = line.size();
            } else if (path && (text[i] == L'/' || text[i] == L'\\')) {
                keep = resume = line.size(); // the slash stays at the line's end
            }
        }
        return lines;
    }

    void DrawLine(FontId font, const std::wstring& text, float x, float baseline, Color color)
    {
        const int r = static_cast<int>(color.r * 255.0f), g = static_cast<int>(color.g * 255.0f),
                  b = static_cast<int>(color.b * 255.0f);
        const int base = static_cast<int>(std::floor(baseline + 0.5f));
        for (const wchar_t c : text) {
            const Glyph& glyph = GlyphOf(font, c);
            const int originX = static_cast<int>(std::floor(x + 0.5f)) + glyph.left;
            const int originY = base + glyph.top;
            for (int row = 0; row < glyph.h; ++row) {
                const int py = originY + row;
                if (py < 0 || py >= height_) continue;
                for (int column = 0; column < glyph.w; ++column) {
                    const int px = originX + column;
                    const int cover = glyph.cover[static_cast<size_t>(row) * glyph.w + column];
                    if (!cover || px < 0 || px >= width_) continue;
                    uint32_t& pixel = pixels_[static_cast<size_t>(py) * width_ + px];
                    const int alpha = cover + (cover >> 7), keep = 256 - alpha;
                    const int dr = (pixel >> 16) & 255, dg = (pixel >> 8) & 255, db = pixel & 255;
                    pixel = static_cast<uint32_t>(((r * alpha + dr * keep) >> 8) << 16 |
                                                  ((g * alpha + dg * keep) >> 8) << 8 |
                                                  ((b * alpha + db * keep) >> 8));
                }
            }
            x += glyph.advance;
        }
    }

    uint32_t* pixels_;
    int width_, height_;
    float scale_;
    Face regular_, bold_;           // what the text is drawn in
    Face spareRegular_, spareBold_; // the system's, for characters those lack
    std::map<uint64_t, Glyph> glyphs_;
};

} // namespace install
