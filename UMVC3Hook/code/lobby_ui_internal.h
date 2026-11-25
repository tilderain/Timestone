#pragma once

// Drawing kit shared by the lobby UI's source files (lobby_ui.cpp and
// lobby_ui_settings.cpp): the palette, the selection animation, the screen
// geometry and the Painter. Not part of the interface in lobby_ui.h.

#include "lobby_canvas.h"

#include <cmath>
#include <cstdint>

namespace lobby {

// ---------------------------------------------------------------------------
// Palette and shared geometry (1280x720 UI space)
// ---------------------------------------------------------------------------
constexpr Color kText = kWhite;
// The option menu's selection animation, read from the shared sprite
// animation list (ui\cmn\anm\sprite_base, CAC): animation 8 drives the
// highlight bar and 9/10 the left/right arrows, all on one 76-frame loop, so
// they move together. From frame 1 the highlight dims (colour 0xBF -> 0x3F)
// while the arrows slide 5 px inward, over 50 frames; from frame 51 the
// highlight brightens again and the arrows slide back, over 25 frames.
// Easing as uUiAnmSprite__interpolateChannel (0x1402C7B90): mode 5 is
// 1-(1-u)^2, mode 2 is u^2.
constexpr uint32_t kSelectLoopFrames = 76;
constexpr float kSelectArrowTravel = 5.0f;
inline float EaseOutQuad(float u) { return 1.0f - (1.0f - u) * (1.0f - u); }

// Highlight brightness, 1 = brightest, 0 = dimmest.
inline float SelectionLevel(uint32_t frame)
{
    const uint32_t t = frame % kSelectLoopFrames;
    if (t < 1u) return 1.0f;
    if (t < 51u) return 1.0f - EaseOutQuad((t - 1u) / 50.0f);
    return EaseOutQuad((t - 51u) / 25.0f);
}

// How far each arrow sits in from its rest position, toward the value.
inline float SelectionArrowInset(uint32_t frame)
{
    const uint32_t t = frame % kSelectLoopFrames;
    if (t < 1u) return 0.0f;
    if (t < 51u) return kSelectArrowTravel * EaseOutQuad((t - 1u) / 50.0f);
    const float u = (t - 51u) / 25.0f;
    return kSelectArrowTravel * (1.0f - u * u);
}

// -1..1 form of SelectionLevel for the highlight alpha expressions.
inline float SelectionWave(uint32_t frame) { return SelectionLevel(frame) * 2.0f - 1.0f; }

// Highlight bar opacity, from `dimmest` up to the native peak colour 0xBF.
constexpr float kSelectPeak = 0xBF / 255.0f;
inline float SelectionPulse(uint32_t frame, float dimmest = 0.44f)
{
    return dimmest + (kSelectPeak - dimmest) * SelectionLevel(frame);
}

// Opacity of the top menu's white selection bar: 1.0 down to kMenuBarFadeMin
// and back, once every kMenuBarFadeFrames frames.
constexpr float kMenuBarFadeMin = 0.82f;
constexpr float kMenuBarFadeFrames = 96.0f;
inline float MenuBarFade(uint32_t frame)
{
    const float phase = std::cos(frame * (6.2831853f / kMenuBarFadeFrames));
    return kMenuBarFadeMin + (1.0f - kMenuBarFadeMin) * (0.5f + 0.5f * phase);
}

constexpr Color kDim = Rgb(0xBAC4D2);
constexpr Color kFaint = Rgb(0x7E8A9A);
constexpr Color kYellow = Rgb(0xFFD63A);
constexpr Color kCyan = Rgb(0x5CE4FF);
constexpr Color kRed = Rgb(0xFF5A4C);
constexpr Color kGreen = Rgb(0x86F25E);
constexpr Color kPurple = Rgb(0xC79BFF); // the host changed the lobby's settings

constexpr float kScreenW = 1280.0f;
constexpr float kScreenH = 720.0f;
constexpr float kSafeLeft = 56.0f;
constexpr float kSafeRight = 1224.0f;
constexpr float kFooterY = 664.0f;

constexpr uint32_t kTransitionFrames = 9;

inline float Clamp01(float value) { return value < 0.0f ? 0.0f : value > 1.0f ? 1.0f : value; }
inline float EaseOut(float t) { t = 1.0f - Clamp01(t); return 1.0f - t * t * t; }

// Applies the screen transition (fade + slide) to everything drawn through it.
class Painter {
public:
    Painter(Canvas& canvas, float alpha, float dx) : canvas_(canvas), alpha_(alpha), dx_(dx) {}

    Painter Fixed() const { return Painter(canvas_, 1.0f, 0.0f); }
    Painter Faded(float alpha) const { return Painter(canvas_, alpha_ * alpha, dx_); }

    void Sprite(Tex tex, float x, float y, float w, float h, Uv uv, Anchor anchor,
                Color color = kWhite) const
    {
        canvas_.Sprite(tex, x + dx_, y, w, h, uv, anchor, WithAlpha(color, alpha_));
    }

    void SpriteTurned(Tex tex, float x, float y, float w, float h, Uv uv, float radians,
                      Color color = kWhite) const
    {
        canvas_.SpriteTurned(tex, x + dx_, y, w, h, uv, radians, WithAlpha(color, alpha_));
    }

    void Fill(float x, float y, float w, float h, Color color) const
    {
        canvas_.Sprite(TEX_WHITE, x + dx_, y, w, h, Uv{0.25f, 0.25f, 0.75f, 0.75f}, A_TL,
                       WithAlpha(color, alpha_));
    }

    void Line(float x0, float y0, float x1, float y1, float width, Color color) const
    {
        canvas_.Line(x0 + dx_, y0, x1 + dx_, y1, width, WithAlpha(color, alpha_));
    }

    // 9-slice frame described by the rectangle its visible art should cover
    // (top-left origin). The frame's transparent margin is added around it.
    void Box(Tex tex, float x, float y, float w, float h, Color color = kWhite) const
    {
        const FrameStyle& style = GetFrameStyle(tex);
        const float totalW = w + style.insetLeft + style.insetRight;
        const float totalH = h + style.insetTop + style.insetBottom;
        float innerW = totalW - 2.0f * FrameBorderX(tex, style.uv);
        float innerH = totalH - 2.0f * FrameBorderY(tex, style.uv);
        if (innerW < 0.0f) innerW = 0.0f;
        if (innerH < 0.0f) innerH = 0.0f;
        canvas_.Frame(tex, x - style.insetLeft + dx_, y - style.insetTop, innerW, innerH,
                      style.uv, A_TL, WithAlpha(color, alpha_));
    }

    void Text(const char* text, float x, float y, float scale, Anchor anchor,
              Color color = kText, Font font = FONT_BODY, float stretch = 1.0f,
              Blend blend = BLEND_NORMAL) const
    {
        if (!text || !text[0]) return;
        canvas_.Text(text, x + dx_, y, scale, anchor, WithAlpha(color, alpha_), font, stretch,
                     blend);
    }

    void TextFit(const char* text, float x, float y, float scale, float maxWidth,
                 Anchor anchor, Color color = kText, Font font = FONT_BODY) const
    {
        char fitted[192];
        FitText(fitted, sizeof(fitted), text, maxWidth, scale, font);
        Text(fitted, x, y, scale, anchor, color, font);
    }

    // Like TextFit, but the whole text is always shown: it is drawn smaller
    // until it fits maxWidth, never cut. Returns the scale used.
    float TextShrink(const char* text, float x, float y, float scale, float maxWidth,
                     Anchor anchor, Color color = kText, Font font = FONT_BODY) const
    {
        const float width = TextWidth(text, font) * scale;
        if (width > maxWidth && width > 0.0f) scale *= maxWidth / width;
        Text(text, x, y, scale, anchor, color, font);
        return scale;
    }

    // Small label with a dark drop for legibility over the busy backdrop.
    void Label(const char* text, float x, float y, float scale, Anchor anchor,
               Color color = kText) const
    {
        Text(text, x + 2.0f, y + 2.0f, scale, anchor, Color{0.0f, 0.0f, 0.0f, 0.75f});
        Text(text, x, y, scale, anchor, color);
    }

private:
    Canvas& canvas_;
    float alpha_;
    float dx_;
};

// Distance from a value's centre to each arrow. The option menu uses 130; the
// lobby's values run longer (character and assist names), so a little more,
// and the value text is fitted to stay clear of the arrows.
constexpr float kValueArrowSpan = 150.0f;
constexpr float kValueTextWidth = 2.0f * (kValueArrowSpan - 24.0f);

// Left/right arrows around a selectable value: the option menu's own arrow
// art (drawn pointing left; the right one is the mirror image), 32 px as
// there, sliding inward and back in step with the selection highlight.
inline void DrawValueArrows(const Painter& p, uint32_t frame, float centerX, float y, float halfSpan)
{
    const float inset = SelectionArrowInset(frame);
    p.Sprite(TEX_OPT_ARROW, centerX - halfSpan + inset, y, 32.0f, 32.0f, kFullUv, A_MC);
    p.Sprite(TEX_OPT_ARROW, centerX + halfSpan - inset, y, 32.0f, 32.0f,
             Uv{1.0f, 0.0f, 0.0f, 1.0f}, A_MC);
}

} // namespace lobby
