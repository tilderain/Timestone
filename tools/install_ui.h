#pragma once

// The installer's portable half: its window drawn in software, and the text
// formats it reads and writes. Nothing here touches Windows, so
// tools/test_install.cpp builds it natively to check the logic and render
// the screens to pictures.
//
// The look follows the lobby's modals (LobbyUi::DrawModal and
// DrawUpdateProgress in UMVC3Hook/code/lobby_ui.cpp) over the lobby's space
// backdrop, which is the plugin's own code (space_bg.cpp) and its one shipped
// picture (UMVC3Hook/assets, CC0). The game's panel and font textures cannot
// be shipped, so the panel, bar and buttons are drawn here as plain shapes
// and the text is in Bai Jamjuree (UMVC3Hook/assets, SIL Open Font License),
// which both installers carry.

#include "space_bg.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace install {

using lobby::Color;

constexpr float kScreenW = 1280.0f, kScreenH = 720.0f;
constexpr Color kText = lobby::kWhite;
constexpr Color kDim = lobby::Rgb(0xBAC4D2);
constexpr Color kFaint = lobby::Rgb(0x7E8A9A);
constexpr Color kYellow = lobby::Rgb(0xFFD63A);

// ---------------------------------------------------------------------------
// A lobby::Canvas over a block of 0x00RRGGBB pixels, top row first. It draws
// what the space backdrop asks for: flat and star-textured rectangles, and
// the grid's lines. Coordinates are the lobby's 1280x720 space.
// ---------------------------------------------------------------------------
class SoftCanvas final : public lobby::Canvas {
public:
    // `stars` is kSpaceStarsRgbaBytes from BuildSpaceStarsRgba, or null.
    SoftCanvas(uint32_t* pixels, int width, int height, const uint8_t* stars)
        : pixels_(pixels), width_(width), height_(height), stars_(stars),
          scale_(width / kScreenW)
    {
    }

    float Scale() const { return scale_; }

    void Fill(float x, float y, float w, float h, Color color)
    {
        Sprite(lobby::TEX_WHITE, x, y, w, h, lobby::kFullUv, lobby::A_TL, color);
    }

    // A border `thickness` wide just inside the rectangle.
    void Outline(float x, float y, float w, float h, float thickness, Color color)
    {
        // On whole pixels, and at least one of them, so no side rounds away.
        const float right = Round((x + w) * scale_) / scale_, bottom = Round((y + h) * scale_) / scale_;
        x = Round(x * scale_) / scale_;
        y = Round(y * scale_) / scale_;
        w = right - x;
        h = bottom - y;
        const int pixels = Round(thickness * scale_);
        thickness = (pixels < 1 ? 1 : pixels) / scale_;
        Fill(x, y, w, thickness, color);
        Fill(x, y + h - thickness, w, thickness, color);
        Fill(x, y + thickness, thickness, h - 2.0f * thickness, color);
        Fill(x + w - thickness, y + thickness, thickness, h - 2.0f * thickness, color);
    }

    void Sprite(lobby::Tex tex, float x, float y, float w, float h, lobby::Uv uv,
                lobby::Anchor anchor, Color color) override
    {
        x -= w * (anchor % 3) * 0.5f;
        y -= h * (anchor / 3) * 0.5f;
        const float left = x * scale_, top = y * scale_;
        const float right = (x + w) * scale_, bottom = (y + h) * scale_;
        int x0 = Round(left), y0 = Round(top), x1 = Round(right), y1 = Round(bottom);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > width_) x1 = width_;
        if (y1 > height_) y1 = height_;
        if (x0 >= x1 || y0 >= y1) return;
        const int alpha = Level(color.a);
        if (alpha <= 0) return;

        if (tex != lobby::TEX_SPACE_STARS) {
            const int r = Channel(color.r), g = Channel(color.g), b = Channel(color.b);
            for (int py = y0; py < y1; ++py) {
                uint32_t* row = pixels_ + static_cast<size_t>(py) * width_;
                for (int px = x0; px < x1; ++px) Blend(row[px], r, g, b, alpha);
            }
            return;
        }
        if (!stars_ || right <= left || bottom <= top) return;

        const int size = lobby::kSpaceStarsSize;
        const int mr = Level(color.r), mg = Level(color.g), mb = Level(color.b);
        const float texelsPerPixelU = (uv.u1 - uv.u0) * size / (right - left);
        const float texelsPerPixelV = (uv.v1 - uv.v0) * size / (bottom - top);
        const int32_t stepU = static_cast<int32_t>(texelsPerPixelU * 65536.0f);
        for (int py = y0; py < y1; ++py) {
            float fv = uv.v0 * size + (py + 0.5f - top) * texelsPerPixelV - 0.5f;
            if (fv < 0.0f) fv = 0.0f;
            if (fv > size - 1.001f) fv = size - 1.001f;
            const int iy = static_cast<int>(fv);
            const int wy = static_cast<int>((fv - iy) * 256.0f);
            const uint8_t* row0 = stars_ + static_cast<size_t>(iy) * size * 4;
            const uint8_t* row1 = row0 + static_cast<size_t>(size) * 4;
            const float fu = uv.u0 * size + (x0 + 0.5f - left) * texelsPerPixelU - 0.5f;
            int32_t u = static_cast<int32_t>(fu * 65536.0f);
            uint32_t* out = pixels_ + static_cast<size_t>(py) * width_;
            for (int px = x0; px < x1; ++px, u += stepU) {
                int32_t cu = u;
                if (cu < 0) cu = 0;
                if (cu > (size - 2) << 16) cu = (size - 2) << 16;
                const int ix = cu >> 16, wx = (cu >> 8) & 255;
                const uint8_t* a = row0 + ix * 4;
                const uint8_t* b = row1 + ix * 4;
                int rgb[3];
                for (int c = 0; c < 3; ++c) {
                    const int upper = a[c] * (256 - wx) + a[c + 4] * wx;
                    const int lower = b[c] * (256 - wx) + b[c + 4] * wx;
                    rgb[c] = (upper * (256 - wy) + lower * wy) >> 16;
                }
                Blend(out[px], (rgb[0] * mr) >> 8, (rgb[1] * mg) >> 8, (rgb[2] * mb) >> 8, alpha);
            }
        }
    }

    void Frame(lobby::Tex, float, float, float, float, lobby::Uv, lobby::Anchor, Color) override {}
    void SpriteTurned(lobby::Tex, float, float, float, float, lobby::Uv, float, Color) override {}
    void Text(const char*, float, float, float, lobby::Anchor, Color, lobby::Font, float,
              lobby::Blend) override
    {
    }

    void Line(float x0, float y0, float x1, float y1, float width, Color color) override
    {
        const float half = width * 0.5f * scale_;
        Stroke(x0, y0, x1, y1, half + 0.5f, color, [half](float distance) {
            const float cover = half - distance + 0.5f;
            return cover < 0.0f ? 0.0f : cover > 1.0f ? 1.0f : cover;
        });
    }

    // The grid line as the game draws it: the profile space_bg.cpp bakes into
    // the star texture, a bright core two units across inside a soft halo.
    bool GlowLine(float x0, float y0, float x1, float y1, Color color) override
    {
        const float scale = scale_;
        Stroke(x0, y0, x1, y1, 4.5f * scale, color, [scale](float distance) {
            static const float kProfile[5] = {230.0f / 255.0f, 60.0f / 255.0f, 36.0f / 255.0f,
                                              14.0f / 255.0f, 0.0f};
            const float units = distance / scale - 0.5f; // texel centres sit at +-0.5, 1.5 ...
            if (units <= 0.0f) return kProfile[0];
            if (units >= 4.0f) return 0.0f;
            const int index = static_cast<int>(units);
            const float t = units - index;
            return kProfile[index] + (kProfile[index + 1] - kProfile[index]) * t;
        });
        return true;
    }

private:
    static int Round(float value) { return static_cast<int>(std::floor(value + 0.5f)); }
    static int Channel(float value)
    {
        return value <= 0.0f ? 0 : value >= 1.0f ? 255 : static_cast<int>(value * 255.0f + 0.5f);
    }
    static int Level(float value)
    {
        return value <= 0.0f ? 0 : value >= 1.0f ? 256 : static_cast<int>(value * 256.0f + 0.5f);
    }
    static void Blend(uint32_t& pixel, int r, int g, int b, int alpha)
    {
        const int keep = 256 - alpha;
        const int dr = (pixel >> 16) & 255, dg = (pixel >> 8) & 255, db = pixel & 255;
        pixel = static_cast<uint32_t>(((r * alpha + dr * keep) >> 8) << 16 |
                                      ((g * alpha + dg * keep) >> 8) << 8 |
                                      ((b * alpha + db * keep) >> 8));
    }

    // Every pixel within `reach` pixels of the segment, at the strength
    // `profile` gives for its distance from it.
    template <typename Profile>
    void Stroke(float ax, float ay, float bx, float by, float reach, Color color, Profile profile)
    {
        ax *= scale_;
        ay *= scale_;
        bx *= scale_;
        by *= scale_;
        int x0 = static_cast<int>(std::floor((ax < bx ? ax : bx) - reach));
        int x1 = static_cast<int>(std::ceil((ax > bx ? ax : bx) + reach));
        int y0 = static_cast<int>(std::floor((ay < by ? ay : by) - reach));
        int y1 = static_cast<int>(std::ceil((ay > by ? ay : by) + reach));
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > width_) x1 = width_;
        if (y1 > height_) y1 = height_;
        if (x0 >= x1 || y0 >= y1 || color.a <= 0.0f) return;
        const float dx = bx - ax, dy = by - ay;
        const float lengthSquared = dx * dx + dy * dy;
        const int r = Channel(color.r), g = Channel(color.g), b = Channel(color.b);
        for (int py = y0; py < y1; ++py) {
            uint32_t* row = pixels_ + static_cast<size_t>(py) * width_;
            for (int px = x0; px < x1; ++px) {
                const float qx = px + 0.5f - ax, qy = py + 0.5f - ay;
                float t = lengthSquared > 0.0f ? (qx * dx + qy * dy) / lengthSquared : 0.0f;
                t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
                const float ox = qx - dx * t, oy = qy - dy * t;
                const float distanceSquared = ox * ox + oy * oy;
                if (distanceSquared >= reach * reach) continue;
                const int alpha = Level(profile(std::sqrt(distanceSquared)) * color.a);
                if (alpha > 0) Blend(row[px], r, g, b, alpha);
            }
        }
    }

    uint32_t* pixels_;
    int width_, height_;
    const uint8_t* stars_;
    float scale_;
};

// ---------------------------------------------------------------------------
// Text is the platform's: the installer draws it with GDI, the test with
// boxes of about the right size.
// ---------------------------------------------------------------------------
enum class FontId { Title, Button, Body, Small };
// Heights in the 1280x720 space.
constexpr float kFontHeight[4] = {34.0f, 25.0f, 25.0f, 20.0f};

struct Rect {
    float x, y, w, h;
};

enum TextFlags : unsigned {
    kTextWrap = 0,       // centred lines broken at the rectangle's width, from its top
    kTextMiddle = 1,     // one line, centred both ways
    kTextPath = 2,       // wrapped like kTextWrap, and broken after a slash or anywhere if it must be
    kTextOutline = 4,    // a dark edge all round, as the game's heading font has
    kTextRight = 8,      // one line against the right edge
};

class TextPainter {
public:
    virtual ~TextPainter() = default;
    // Height `text` takes when wrapped to `width` (kTextWrap or kTextPath).
    virtual float BlockHeight(FontId font, const wchar_t* text, float width,
                              unsigned flags = kTextWrap) = 0;
    virtual void Draw(FontId font, const wchar_t* text, Rect rect, Color color,
                      unsigned flags) = 0;
};

constexpr int kMaxButtons = 4;

// One screen of the installer: a modal over the backdrop.
struct View {
    const wchar_t* title = L"";
    const wchar_t* message = L"";
    const wchar_t* detail = L"";  // a path under the message, dimmer, shown whole
    const wchar_t* footer = L"";  // bottom right corner of the window
    const wchar_t* buttons[kMaxButtons] = {};
    int buttonCount = 0;
    int cursor = 0;
    bool busy = false;            // three travelling dots
    bool progress = false;        // a bar and a status line
    float fraction = -1.0f;       // 0..1, or negative while the size is unknown
    const wchar_t* status = L"";
};

struct Layout {
    Rect panel;
    Rect buttons[kMaxButtons];
};

inline bool Contains(const Rect& rect, float x, float y)
{
    return x >= rect.x && x < rect.x + rect.w && y >= rect.y && y < rect.y + rect.h;
}

// `frame` counts sixtieths of a second. Returns where things landed, for the
// mouse.
inline Layout DrawScreen(SoftCanvas& canvas, TextPainter& text, const View& view, uint32_t frame)
{
    lobby::DrawSpaceBackground(canvas, frame, 1.0f);
    text.Draw(FontId::Small, view.footer, Rect{640.0f, 676.0f, 616.0f, 28.0f}, kFaint, kTextRight);

    const float w = 760.0f, inner = w - 80.0f;
    const bool hasDetail = view.detail[0] != 0;
    const float messageH = view.message[0] ? text.BlockHeight(FontId::Body, view.message, inner)
                                           : 0.0f;
    const float detailH = hasDetail ? text.BlockHeight(FontId::Small, view.detail, inner, kTextPath)
                                    : 0.0f;
    float h = 84.0f + messageH;
    if (hasDetail) h += detailH + 14.0f;
    if (view.progress) h += 92.0f;
    if (view.busy) h += 40.0f;
    h += view.buttonCount > 0 ? 100.0f : 30.0f;

    const float x = (kScreenW - w) * 0.5f, y = (kScreenH - h) * 0.5f - 12.0f;
    canvas.Fill(x + 7.0f, y + 9.0f, w, h, Color{0.0f, 0.0f, 0.0f, 0.38f});
    canvas.Fill(x, y, w, h, lobby::Rgb(0x000000, 0.86f));
    canvas.Outline(x, y, w, h, 2.0f, lobby::Rgb(0x6E7480, 0.95f));
    canvas.Outline(x + 6.0f, y + 6.0f, w - 12.0f, h - 12.0f, 1.0f, lobby::Rgb(0x25282E, 0.95f));

    text.Draw(FontId::Title, view.title, Rect{x + 30.0f, y + 24.0f, w - 60.0f, 44.0f}, kYellow,
              kTextMiddle | kTextOutline);
    float at = y + 80.0f;
    if (messageH > 0.0f) {
        text.Draw(FontId::Body, view.message, Rect{x + 40.0f, at, inner, messageH}, kText,
                  kTextWrap);
        at += messageH;
    }
    if (hasDetail) {
        text.Draw(FontId::Small, view.detail, Rect{x + 40.0f, at + 8.0f, inner, detailH}, kDim,
                  kTextPath);
        at += detailH + 14.0f;
    }
    if (view.progress) {
        const float barX = x + 50.0f, barY = at + 20.0f, barW = w - 100.0f, barH = 22.0f;
        canvas.Fill(barX - 2.0f, barY - 2.0f, barW + 4.0f, barH + 4.0f, lobby::WithAlpha(kFaint, 0.8f));
        canvas.Fill(barX, barY, barW, barH, Color{0.0f, 0.0f, 0.0f, 0.85f});
        if (view.fraction >= 0.0f) {
            const float filled = barW * (view.fraction > 1.0f ? 1.0f : view.fraction);
            if (filled > 0.0f) {
                canvas.Fill(barX, barY, filled, barH, kYellow);
                // A highlight sweeping along the filled part keeps it lively.
                const float sweep = std::fmod(frame * 6.0f, barW + 120.0f) - 60.0f;
                const float from = sweep > 0.0f ? sweep : 0.0f;
                const float to = filled < sweep + 60.0f ? filled : sweep + 60.0f;
                if (to > from)
                    canvas.Fill(barX + from, barY, to - from, barH, lobby::WithAlpha(kText, 0.35f));
            }
        } else {
            // Size not known yet: a block travelling back and forth.
            const float block = barW * 0.2f;
            const float t = 0.5f + 0.5f * std::sin(frame * 0.06f);
            canvas.Fill(barX + (barW - block) * t, barY, block, barH, kYellow);
        }
        text.Draw(FontId::Small, view.status, Rect{x + 40.0f, at + 54.0f, inner, 28.0f}, kDim,
                  kTextMiddle);
        at += 92.0f;
    }
    if (view.busy) {
        for (int i = 0; i < 3; ++i) {
            float phase = std::sin(frame * 0.14f - i * 0.9f);
            if (phase < 0.0f) phase = 0.0f;
            canvas.Fill(kScreenW * 0.5f - 22.0f + i * 18.0f, at + 18.0f, 8.0f, 8.0f,
                        lobby::WithAlpha(kYellow, 0.35f + 0.65f * phase));
        }
    }

    Layout layout{};
    layout.panel = Rect{x, y, w, h};
    // Four across is as many as the panel holds, a little narrower each.
    const bool many = view.buttonCount > 3;
    const float buttonW = many ? 164.0f : 200.0f, pitch = many ? 178.0f : 224.0f, buttonH = 50.0f;
    for (int i = 0; i < view.buttonCount && i < kMaxButtons; ++i) {
        const float centre = kScreenW * 0.5f + (i - (view.buttonCount - 1) * 0.5f) * pitch;
        const Rect rect{centre - buttonW * 0.5f, y + h - 82.0f, buttonW, buttonH};
        layout.buttons[i] = rect;
        const bool selected = i == view.cursor;
        if (selected) {
            const float pulse = 0.72f + 0.28f * std::sin(frame * 0.10f);
            canvas.Fill(rect.x, rect.y, rect.w, rect.h, lobby::WithAlpha(kYellow, 0.16f * pulse));
            canvas.Outline(rect.x, rect.y, rect.w, rect.h, 3.0f, lobby::WithAlpha(kYellow, pulse));
        } else {
            canvas.Outline(rect.x, rect.y, rect.w, rect.h, 1.0f, lobby::WithAlpha(kFaint, 0.55f));
        }
        text.Draw(FontId::Button, view.buttons[i], rect, selected ? kText : kDim,
                  kTextMiddle | kTextOutline);
    }
    return layout;
}

// ---------------------------------------------------------------------------
// Valve's text KeyValues, as far as Steam's library list and app manifests
// need: every value written straight after the quoted key `key`, in file
// order. Keys match without regard to case.
// ---------------------------------------------------------------------------
inline std::vector<std::string> VdfValues(const std::string& text, const char* key)
{
    std::vector<std::string> values;
    const size_t keyLength = strlen(key);
    bool afterKey = false;
    for (size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '{' || c == '}') {
            afterKey = false;
            continue;
        }
        if (c != '"') continue;
        std::string token;
        for (++i; i < text.size() && text[i] != '"'; ++i) {
            if (text[i] == '\\' && i + 1 < text.size()) ++i;
            token.push_back(text[i]);
        }
        if (afterKey) {
            values.push_back(token);
            afterKey = false;
            continue;
        }
        afterKey = token.size() == keyLength;
        for (size_t k = 0; afterKey && k < keyLength; ++k) {
            const char a = token[k], b = key[k];
            const char la = a >= 'A' && a <= 'Z' ? a + 32 : a;
            const char lb = b >= 'A' && b <= 'Z' ? b + 32 : b;
            afterKey = la == lb;
        }
    }
    return values;
}

// ---------------------------------------------------------------------------
// A Wine prefix's user.reg. Makes `executable` load `dll` as native first
// (HKCU\Software\Wine\AppDefaults\<executable>\DllOverrides), which is what
// WINEDLLOVERRIDES="<dll>=n,b" does for one launch.
// ---------------------------------------------------------------------------
enum class OverrideResult {
    Added,
    Present,    // the prefix already says how to load it; left as it is
    NotRegistry // not a registry file this knows how to edit
};

inline OverrideResult AddWineDllOverride(std::string& registry, const char* executable,
                                         const char* dll, unsigned unixTime)
{
    if (registry.compare(0, 23, "WINE REGISTRY Version 2") != 0) return OverrideResult::NotRegistry;
    const auto lower = [](std::string text) {
        for (char& c : text)
            if (c >= 'A' && c <= 'Z') c += 32;
        return text;
    };
    const std::string header = std::string("[Software\\\\Wine\\\\AppDefaults\\\\") + executable +
                               "\\\\DllOverrides]";
    const std::string value = std::string("\"") + dll + "\"=";
    const std::string line = value + "\"native,builtin\"\n";
    const std::string haystack = lower(registry);

    size_t section = haystack.find("\n" + lower(header));
    if (section == std::string::npos) {
        if (registry.back() != '\n') registry += '\n';
        registry += "\n" + header + " " + std::to_string(unixTime) + "\n" + line;
        return OverrideResult::Added;
    }
    ++section;
    size_t body = haystack.find('\n', section);
    body = body == std::string::npos ? haystack.size() : body + 1;
    size_t end = haystack.find("\n[", body > 0 ? body - 1 : 0);
    end = end == std::string::npos ? haystack.size() : end + 1;
    // "name"= or "*name"= at the start of a line of this section.
    for (const std::string& wanted : {lower(value), "\"*" + lower(value).substr(1)}) {
        for (size_t at = haystack.find(wanted, body); at != std::string::npos && at < end;
             at = haystack.find(wanted, at + 1)) {
            if (at == body || haystack[at - 1] == '\n') return OverrideResult::Present;
        }
    }
    // After the section's own "#time=" line when it has one.
    size_t insert = body;
    if (haystack.compare(insert, 6, "#time=") == 0) {
        insert = haystack.find('\n', insert);
        insert = insert == std::string::npos ? registry.size() : insert + 1;
    }
    if (insert == registry.size() && registry.back() != '\n') {
        registry += '\n';
        insert = registry.size();
    }
    registry.insert(insert, line);
    return OverrideResult::Added;
}

// ---------------------------------------------------------------------------
// The release ZIP. It is written without compression (the plugin's own
// updater reads it the same way), so a file is a run of the archive's bytes.
// ---------------------------------------------------------------------------
struct ZipFile {
    std::string name;
    size_t offset;
    size_t size;
};

// Every file of the archive. False for anything but a plain archive of
// stored, unencrypted files in its top folder with ordinary names.
inline bool ListStoredZip(const std::vector<unsigned char>& zip, std::vector<ZipFile>& files)
{
    const auto u16 = [&zip](size_t at) {
        return static_cast<size_t>(zip[at]) | (static_cast<size_t>(zip[at + 1]) << 8);
    };
    const auto u32 = [&zip, &u16](size_t at) { return u16(at) | (u16(at + 2) << 16); };

    files.clear();
    if (zip.size() < 22 || zip.size() > 0xffffffffULL) return false;
    const size_t searchStart = zip.size() > 22 + 0xffff ? zip.size() - (22 + 0xffff) : 0;
    size_t eocd = std::string::npos;
    for (size_t pos = zip.size() - 22;; --pos) {
        if (u32(pos) == 0x06054b50) {
            eocd = pos;
            break;
        }
        if (pos == searchStart) break;
    }
    if (eocd == std::string::npos || u16(eocd + 4) != 0 || u16(eocd + 6) != 0 ||
        u16(eocd + 8) != u16(eocd + 10))
        return false;
    const size_t entries = u16(eocd + 10), centralSize = u32(eocd + 12);
    size_t cursor = u32(eocd + 16);
    if (cursor > zip.size() || centralSize > zip.size() - cursor) return false;

    for (size_t i = 0; i < entries; ++i) {
        if (cursor > zip.size() || zip.size() - cursor < 46 || u32(cursor) != 0x02014b50)
            return false;
        const size_t nameLength = u16(cursor + 28);
        const size_t recordSize = 46 + nameLength + u16(cursor + 30) + u16(cursor + 32);
        if (recordSize > zip.size() - cursor) return false;
        ZipFile file;
        file.name.assign(reinterpret_cast<const char*>(zip.data() + cursor + 46), nameLength);
        if (file.name.empty() || file.name.size() > 96 || file.name[0] == '.' ||
            file.name.back() == '.' || file.name.back() == ' ')
            return false;
        for (const char c : file.name) {
            const bool plain = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                               (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_' ||
                               c == ' ';
            if (!plain) return false;
        }
        for (const ZipFile& other : files)
            if (other.name == file.name) return false;
        const bool encrypted = (u16(cursor + 8) & 1) != 0;
        const size_t method = u16(cursor + 10);
        const size_t packed = u32(cursor + 20), local = u32(cursor + 42);
        file.size = u32(cursor + 24);
        if (encrypted || method != 0 || packed != file.size || local > zip.size() ||
            zip.size() - local < 30 || u32(local) != 0x04034b50)
            return false;
        file.offset = local + 30 + u16(local + 26) + u16(local + 28);
        if (file.offset > zip.size() || file.size > zip.size() - file.offset) return false;
        files.push_back(file);
        cursor += recordSize;
    }
    return !files.empty();
}

// Files of the release that are for whoever unpacks it by hand and are not
// put into the game folder: the notices and the installer itself. An install
// also takes out the copies earlier installers left there.
inline const char* const kPackageOnlyFiles[] = {"Timestone-THIRDPARTYNOTICES",
                                                "TimestoneInstall.exe"};

inline void DropPackageOnlyFiles(std::vector<ZipFile>& files)
{
    for (size_t i = files.size(); i-- > 0;)
        for (const char* name : kPackageOnlyFiles)
            if (files[i].name == name) {
                files.erase(files.begin() + i);
                break;
            }
}

// ---------------------------------------------------------------------------
// GitHub's release description, as far as picking one file out of it needs.
// ---------------------------------------------------------------------------

// The string value of the first `key` at or after `start`.
inline bool ExtractJsonString(const std::string& json, const char* key, size_t start,
                              std::string& value)
{
    const std::string marker = std::string("\"") + key + "\"";
    const size_t keyPos = json.find(marker, start);
    const size_t colon = keyPos == std::string::npos ? keyPos
                                                     : json.find(':', keyPos + marker.size());
    const size_t quote = colon == std::string::npos ? colon : json.find('"', colon + 1);
    if (quote == std::string::npos) return false;
    value.clear();
    for (size_t i = quote + 1; i < json.size(); ++i) {
        const char c = json[i];
        if (c == '"') return true;
        if (c == '\\' && i + 1 < json.size()) {
            const char escaped = json[++i];
            if (escaped != '/' && escaped != '\\' && escaped != '"') return false;
            value.push_back(escaped);
        } else {
            value.push_back(c);
        }
    }
    return false;
}

// Where to download the release file `assetName`, the SHA-256 GitHub lists
// for it (64 hex digits) and its size in bytes (0 if not given).
inline bool ParseReleaseAsset(const std::string& json, const char* assetName, std::string& url,
                              std::string& sha256, unsigned long long* size = nullptr)
{
    size_t asset = json.find("\"assets\"");
    while (asset != std::string::npos) {
        asset = json.find("\"name\"", asset + 1);
        if (asset == std::string::npos) break;
        std::string name;
        if (!ExtractJsonString(json, "name", asset, name) || name != assetName) continue;
        std::string digest;
        if (!ExtractJsonString(json, "digest", asset, digest) || digest.size() != 71 ||
            digest.compare(0, 7, "sha256:") != 0 ||
            !ExtractJsonString(json, "browser_download_url", asset, url))
            return false;
        sha256 = digest.substr(7);
        if (size) {
            *size = 0;
            size_t at = json.find("\"size\"", asset);
            at = at == std::string::npos ? at : json.find(':', at);
            for (++at; at < json.size() && json[at] == ' '; ++at) {}
            for (; at < json.size() && json[at] >= '0' && json[at] <= '9'; ++at)
                *size = *size * 10 + static_cast<unsigned>(json[at] - '0');
        }
        return true;
    }
    return false;
}

// Whether version `latest` is past `current`: dotted numbers, with or without
// a leading "v". Anything that is not a number counts as newer unless the two
// are the same text.
template <typename Char>
bool IsNewerVersion(const Char* latest, const Char* current)
{
    const auto skipPrefix = [](const Char* v) { return *v == 'v' || *v == 'V' ? v + 1 : v; };
    const auto number = [](const Char*& v, unsigned long& value) {
        const Char* start = v;
        for (value = 0; *v >= '0' && *v <= '9'; ++v) value = value * 10 + static_cast<unsigned>(*v - '0');
        return v != start;
    };
    latest = skipPrefix(latest);
    current = skipPrefix(current);
    const Char* a = latest;
    const Char* b = current;
    for (int part = 0; part < 4; ++part) {
        unsigned long left = 0, right = 0;
        if (!number(a, left) || !number(b, right)) {
            for (a = latest, b = current; *a && *a == *b; ++a, ++b) {}
            return *a != *b;
        }
        if (left != right) return left > right;
        if (*a == '.') ++a;
        if (*b == '.') ++b;
        if (!*a && !*b) return false;
    }
    return false;
}

// ---------------------------------------------------------------------------
// For the build that has no Windows to ask.
// ---------------------------------------------------------------------------

// SHA-256 of `data`, as 64 lowercase hex digits.
inline std::string Sha256Hex(const unsigned char* data, size_t size)
{
    static const uint32_t kRound[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                         0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto rotate = [](uint32_t v, int by) { return (v >> by) | (v << (32 - by)); };
    const auto block = [&](const unsigned char* in) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<uint32_t>(in[i * 4]) << 24 | static_cast<uint32_t>(in[i * 4 + 1]) << 16 |
                   static_cast<uint32_t>(in[i * 4 + 2]) << 8 | in[i * 4 + 3];
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t v[8];
        memcpy(v, state, sizeof(v));
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = rotate(v[4], 6) ^ rotate(v[4], 11) ^ rotate(v[4], 25);
            const uint32_t t1 = v[7] + s1 + ((v[4] & v[5]) ^ (~v[4] & v[6])) + kRound[i] + w[i];
            const uint32_t s0 = rotate(v[0], 2) ^ rotate(v[0], 13) ^ rotate(v[0], 22);
            const uint32_t t2 = s0 + ((v[0] & v[1]) ^ (v[0] & v[2]) ^ (v[1] & v[2]));
            memmove(v + 1, v, 7 * sizeof(uint32_t));
            v[4] += t1;
            v[0] = t1 + t2;
        }
        for (int i = 0; i < 8; ++i) state[i] += v[i];
    };
    size_t at = 0;
    for (; size - at >= 64; at += 64) block(data + at);
    unsigned char tail[128] = {};
    const size_t rest = size - at;
    if (rest) memcpy(tail, data + at, rest);
    tail[rest] = 0x80;
    const size_t padded = rest < 56 ? 64 : 128;
    const uint64_t bits = static_cast<uint64_t>(size) * 8;
    for (int i = 0; i < 8; ++i) tail[padded - 1 - i] = static_cast<unsigned char>(bits >> (8 * i));
    block(tail);
    if (padded == 128) block(tail + 64);
    static const char kHex[] = "0123456789abcdef";
    std::string hex;
    for (const uint32_t word : state)
        for (int shift = 28; shift >= 0; shift -= 4) hex.push_back(kHex[(word >> shift) & 15]);
    return hex;
}

// The ProductVersion string of a Windows program's version resource, found
// by its name in the file rather than by walking the resource tree. Empty if
// there is none.
inline std::string PeProductVersion(const std::vector<unsigned char>& file)
{
    static const char kKey[] = "ProductVersion";
    const size_t keyBytes = (sizeof(kKey) - 1) * 2;
    for (size_t at = 0; at + keyBytes + 2 <= file.size(); at += 2) {
        size_t i = 0;
        for (; i < sizeof(kKey) - 1; ++i)
            if (file[at + i * 2] != static_cast<unsigned char>(kKey[i]) || file[at + i * 2 + 1] != 0)
                break;
        if (i != sizeof(kKey) - 1) continue;
        // The key's terminator and padding to a four-byte boundary, then the value.
        size_t value = at + keyBytes;
        while (value + 1 < file.size() && file[value] == 0 && file[value + 1] == 0) value += 2;
        std::string version;
        for (; value + 1 < file.size() && version.size() < 32; value += 2) {
            const unsigned c = file[value] | (file[value + 1] << 8);
            if (c < 0x20 || c > 0x7e) break;
            version.push_back(static_cast<char>(c));
        }
        if (!version.empty()) return version;
    }
    return {};
}

} // namespace install
