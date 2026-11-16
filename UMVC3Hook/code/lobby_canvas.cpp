#include "lobby_canvas.h"

#include <cstdio>
#include <cstring>

namespace lobby {

namespace {

// Paths and pixel sizes read from the retail archives (ui\cmn, ui\netroom,
// ui\mnmain). Order must match enum Tex.
#define LIC_ICON(n) \
    {"ui\\lic\\lic_user_i\\lic_user_i" #n "_BM_HQ_NOMIP", 64, 64, ARC_LICENSE, false}
const TexInfo kTextures[TEX_COUNT] = {
    {"ui\\ui_filter\\white_BM_HQ_NOMIP", 32, 32, ARC_COMMON, false},
    {"ui\\net\\net_sprite\\ga_net_waku_a_BM_HQ_NOMIP", 1024, 64, ARC_NETROOM, false},
    {"ui\\net\\net_sprite\\ga_net_waku_f_BM_HQ_NOMIP", 64, 128, ARC_NETROOM, false},
    {"ui\\net\\net_sprite\\ga_net_waku_d_BM_HQ_NOMIP", 256, 64, ARC_NETROOM, false},
    {"ui\\net\\net_sprite\\ga_net_waku_e_BM_HQ_NOMIP", 256, 64, ARC_NETROOM, false},
    {"ui\\net\\net_sprite\\ga_net_waku_b_BM_HQ_NOMIP", 64, 64, ARC_NETROOM, false},
    {"ui\\net\\net_sprite\\ga_net_sel_a_BM_HQ_NOMIP", 64, 64, ARC_NETROOM, false},
    {"ui\\net\\net_sprite\\ga_net_icon_c2_BM_HQ_NOMIP", 64, 256, ARC_NETROOM, true},
    {"ui\\net\\net_sprite\\ga_net_word_a_BM_HQ_NOMIP", 128, 256, ARC_NETROOM, true},
    {"ui\\net\\net_sprite\\ga_net_word_b_BM_HQ_NOMIP", 256, 256, ARC_NETROOM, true},
    {"ui\\net\\net_sprite\\ga_net_icon_a_BM_HQ_NOMIP", 64, 64, ARC_NETROOM, false},
    {"ui\\net\\net_sprite\\ga_net_icon_d_BM_HQ_NOMIP", 64, 64, ARC_NETROOM, false},
    {"ui\\net\\net_sprite\\ga_net_chat_a_BM_HQ_NOMIP", 256, 64, ARC_NETROOM, false},
    {"ui\\net\\net_sprite\\ga_net_line_a_BM_HQ_NOMIP", 32, 32, ARC_NETROOM, false},
    {"ui\\lic\\lic_user_i\\lic_user_i001_BM_HQ_NOMIP", 64, 64, ARC_NETROOM, false},
    {"ui\\net\\net_sprite\\ga_net_num_b_BM_HQ_NOMIP", 256, 32, ARC_NETROOM, false},
    {"ui\\net\\net_sprite\\ga_room_subtit_BM_HQ_NOMIP", 256, 256, ARC_MAINMENU, true},
    {"ui\\net\\net_sprite\\ga_room_subtit2_BM_HQ_NOMIP", 256, 64, ARC_MAINMENU, true},
    {"ui\\net\\net_sprite\\ga_room_word_a_BM_HQ_NOMIP", 256, 64, ARC_MAINMENU, true},
    {"ui\\net\\net_sprite\\ga_room_icon_c_BM_HQ_NOMIP", 256, 256, ARC_MAINMENU, false},
    {"ui\\net\\net_sprite\\ga_room_icon_d_BM_HQ_NOMIP", 256, 64, ARC_MAINMENU, true},
    {"ui\\net\\net_sprite\\ga_room_waku_b_BM_HQ_NOMIP", 256, 64, ARC_MAINMENU, false},
    {"ui\\net\\net_sprite\\ga_room_sel_a_BM_HQ_NOMIP", 256, 64, ARC_MAINMENU, false},
    {"ui\\net\\net_sprite\\ga_room_waku_d_BM_HQ_NOMIP", 32, 64, ARC_MAINMENU, false},
    {"ui\\mainmenu\\mai_tit\\mai35_sel01b_BM_HQ_NOMIP", 512, 64, ARC_MAINMENU, false},
    {"ui\\mainmenu\\mai_tit\\mai35_sel02_BM_HQ_NOMIP", 512, 64, ARC_MAINMENU, false},
    {"ui\\mainmenu\\mai_cur\\mai_cur01_BM_HQ_NOMIP", 64, 64, ARC_MAINMENU, false},
    {"ui\\opt\\opt_sprite\\ga_opt_sel_1p_BM_HQ_NOMIP", 32, 32, ARC_COMMON, false},
    {"ui\\cmn\\cmn_sprite\\ga_window_a_BM_HQ_NOMIP", 128, 32, ARC_COMMON, false},
    {"ui\\cmn\\cmn_sprite\\ga_cmn_bar_BM_HQ_NOMIP", 256, 16, ARC_COMMON, false},
    {"ui\\mainmenu\\mai_hlp\\mai_hlp03_BM_HQ_NOMIP", 256, 256, ARC_COMMON, false},
    {"ui\\mainmenu\\mai_hlp\\mai_hlp04_BM_HQ_NOMIP", 256, 256, ARC_COMMON, false},
    // Character portraits, by character id 1..51. The numbers are the license
    // icons showing each character (read off the icon sheet).
    LIC_ICON(001),
    LIC_ICON(006),
    LIC_ICON(017),
    LIC_ICON(003),
    LIC_ICON(011),
    LIC_ICON(009),
    LIC_ICON(004),
    LIC_ICON(007),
    LIC_ICON(181),
    LIC_ICON(013),
    LIC_ICON(012),
    LIC_ICON(008),
    LIC_ICON(014),
    LIC_ICON(010),
    LIC_ICON(002),
    LIC_ICON(018),
    LIC_ICON(005),
    LIC_ICON(015),
    LIC_ICON(016),
    LIC_ICON(171),
    LIC_ICON(179),
    LIC_ICON(177),
    LIC_ICON(178),
    LIC_ICON(180),
    LIC_ICON(182),
    LIC_ICON(028),
    LIC_ICON(022),
    LIC_ICON(019),
    LIC_ICON(032),
    LIC_ICON(020),
    LIC_ICON(027),
    LIC_ICON(035),
    LIC_ICON(021),
    LIC_ICON(026),
    LIC_ICON(024),
    LIC_ICON(031),
    LIC_ICON(174),
    LIC_ICON(030),
    LIC_ICON(034),
    LIC_ICON(023),
    LIC_ICON(033),
    LIC_ICON(025),
    LIC_ICON(036),
    LIC_ICON(029),
    LIC_ICON(183),
    LIC_ICON(188),
    LIC_ICON(184),
    LIC_ICON(185),
    LIC_ICON(186),
    LIC_ICON(187),
    LIC_ICON(037),
};
#undef LIC_ICON

// Advance widths for U+0020..U+007E, copied from the fonts' FSD metric tables
// (ui\font\nrodinpro and ui\font\bada_b). Glyph index is codepoint - 0x20.
const uint8_t kBodyWidths[95] = {
    16,11,11,17,17,22,20,11,10,10,15,18,11,13,11,14,18,13,17,18,19,17,18,17,
    18,18,11,11,15,17,15,18,19,20,18,19,19,17,17,19,18,11,17,18,17,21,18,19,
    18,19,19,18,19,18,20,24,20,19,18,11,19,11,14,14,11,16,16,15,15,15,12,17,
    15,11,10,15,11,21,15,16,16,16,13,16,13,15,16,21,16,16,15,11,11,11,12,
};
const uint8_t kHeadingWidths[95] = {
    32,24,24,36,40,46,44,16,24,24,26,36,24,24,24,30,40,26,40,40,40,38,40,38,
    40,40,24,24,32,34,32,40,44,40,36,38,36,32,32,37,38,18,34,36,34,41,38,37,
    38,41,40,38,39,40,42,45,40,41,39,20,24,21,26,28,18,32,34,33,33,33,26,36,
    33,16,21,34,14,43,33,35,34,34,27,32,27,33,33,44,34,34,31,24,24,24,28,
};
// Frame usage measured from the textures' alpha bounds. Entries not listed
// use the whole texture with no inset.
constexpr FrameStyle kDefaultFrame{{0.0f, 0.0f, 1.0f, 1.0f}, 0.0f, 0.0f, 0.0f, 0.0f, false};
constexpr FrameStyle kPanelDarkFrame{{0.0f, 0.0f, 0.75f, 1.0f}, 7.0f, 4.0f, 7.0f, 4.0f, false};
constexpr FrameStyle kPanelInsetFrame{{0.0f, 0.0f, 1.0f, 1.0f}, 2.0f, 11.0f, 2.0f, 11.0f, true};
constexpr FrameStyle kRowFrame{{0.0f, 0.0f, 1.0f, 1.0f}, 7.0f, 8.0f, 3.0f, 2.0f, false};
constexpr FrameStyle kRowSelFrame{{0.0f, 0.0f, 1.0f, 1.0f}, 6.0f, 6.0f, 6.0f, 6.0f, false};
constexpr FrameStyle kSlantRowFrame{{0.0f, 0.0f, 0.75f, 1.0f}, 19.0f, 3.0f, 17.0f, 0.0f, false};
constexpr FrameStyle kSlantSelFrame{{0.0f, 0.0f, 0.75f, 1.0f}, 18.0f, 1.0f, 18.0f, 3.0f, false};
constexpr FrameStyle kInsetFrame{{0.0f, 0.0f, 1.0f, 1.0f}, 1.0f, 8.0f, 1.0f, 8.0f, false};

constexpr float kBodyFallbackWidth = 18.0f;    // typical Latin-1 / kana advance
constexpr float kHeadingFallbackWidth = 36.0f;

// Length of the UTF-8 sequence starting at `lead`; malformed bytes count as 1.
size_t SequenceLength(unsigned char lead)
{
    if (lead < 0x80) return 1;
    if ((lead & 0xE0) == 0xC0) return 2;
    if ((lead & 0xF0) == 0xE0) return 3;
    if ((lead & 0xF8) == 0xF0) return 4;
    return 1;
}

float GlyphWidth(const char* text, size_t length, Font font)
{
    const unsigned char lead = static_cast<unsigned char>(text[0]);
    if (length == 1 && lead >= 0x20 && lead <= 0x7E) {
        return font == FONT_HEADING ? kHeadingWidths[lead - 0x20]
                                    : kBodyWidths[lead - 0x20];
    }
    if (length == 1) return 0.0f; // control characters
    // Full-width CJK occupies the whole 32px cell in the body font.
    if (length >= 3 && font == FONT_BODY) return 30.0f;
    return font == FONT_HEADING ? kHeadingFallbackWidth : kBodyFallbackWidth;
}

} // namespace

Tex CharacterIcon(int32_t characterId)
{
    if (characterId < 1 || characterId > 51) return TEX_USER_ICON;
    return static_cast<Tex>(TEX_CHAR_FIRST + characterId - 1);
}

const TexInfo& GetTexInfo(Tex tex)
{
    return kTextures[tex < TEX_COUNT ? tex : TEX_WHITE];
}

float TextWidth(const char* utf8, Font font)
{
    if (!utf8) return 0.0f;
    float width = 0.0f;
    const size_t total = strlen(utf8);
    for (size_t i = 0; i < total;) {
        size_t length = SequenceLength(static_cast<unsigned char>(utf8[i]));
        if (i + length > total) length = total - i;
        width += GlyphWidth(utf8 + i, length, font);
        i += length;
    }
    return width;
}

void FitText(char* out, size_t outSize, const char* text, float maxWidth,
             float scale, Font font)
{
    if (!out || outSize == 0) return;
    out[0] = '\0';
    if (!text) return;
    if (scale <= 0.0f) scale = 1.0f;

    const float limit = maxWidth / scale;
    if (TextWidth(text, font) <= limit) {
        snprintf(out, outSize, "%s", text);
        return;
    }

    const float ellipsis = TextWidth("...", font);
    const size_t total = strlen(text);
    float width = 0.0f;
    size_t kept = 0;
    for (size_t i = 0; i < total;) {
        size_t length = SequenceLength(static_cast<unsigned char>(text[i]));
        if (i + length > total) length = total - i;
        const float glyph = GlyphWidth(text + i, length, font);
        if (width + glyph + ellipsis > limit) break;
        width += glyph;
        i += length;
        kept = i;
    }
    if (kept + 4 > outSize) {
        // Not enough room for the kept prefix and the ellipsis: back up to a
        // sequence boundary that fits.
        kept = 0;
        for (size_t i = 0; i < total;) {
            size_t length = SequenceLength(static_cast<unsigned char>(text[i]));
            if (i + length + 4 > outSize) break;
            i += length;
            kept = i;
        }
    }
    memcpy(out, text, kept);
    snprintf(out + kept, outSize - kept, "...");
}

const FrameStyle& GetFrameStyle(Tex tex)
{
    switch (tex) {
    case TEX_PANEL_DARK: return kPanelDarkFrame;
    case TEX_PANEL_DOT:
    case TEX_PANEL_BLUE: return kPanelInsetFrame;
    case TEX_ROW: return kRowFrame;
    case TEX_ROW_SEL: return kRowSelFrame;
    case TEX_SLANT_ROW: return kSlantRowFrame;
    case TEX_SLANT_SEL: return kSlantSelFrame;
    case TEX_INSET: return kInsetFrame;
    default: return kDefaultFrame;
    }
}

float FrameBorderX(Tex tex, Uv uv)
{
    return GetTexInfo(tex).width * (uv.u1 - uv.u0) / 3.0f;
}

float FrameBorderY(Tex tex, Uv uv)
{
    return GetTexInfo(tex).height * (uv.v1 - uv.v0) / 3.0f;
}

} // namespace lobby
