#include "lobby_canvas.h"

#include <cstdio>
#include <cstring>

namespace lobby {

namespace {

// Paths and pixel sizes read from the retail archives (ui\cmn, ui\netroom,
// ui\mnmain). Order must match enum Tex.
const TexInfo kTextures[TEX_ICON_FIRST] = {
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
    {"ui\\net\\net_sprite\\ga_net_hp_a_BM_HQ_NOMIP", 256, 32, ARC_NETROOM, false},
};

// The license icon (ui\lic\lic_user_i, 000..239) showing each character, by
// character id 1..51 (read off the icon sheet).
const uint8_t kCharacterIcon[51] = {
    1, 6, 17, 3, 11, 9, 4, 7, 181, 13, 12, 8, 14,
    10, 2, 18, 5, 15, 16, 171, 179, 177, 178, 180, 182, 28,
    22, 19, 32, 20, 27, 35, 21, 26, 24, 31, 174, 30, 34,
    23, 33, 25, 36, 29, 183, 188, 184, 185, 186, 187, 37
};

// Country flags (ui\lic\lic_flag_i): 64x64 and in the icons' format, and
// never drawn by the lobby, so they host the Steam profile pictures.
const char* const kAvatarHosts[kAvatarSlots] = {
    "japan", "united_states", "uk", "iceland", "ireland", "argentina", "israel", "italy", "india",
    "indonesia", "australia", "austria", "oman", "netherlands", "qatar", "canada",
};

// File name stem of each character's select-screen faces, by id 1..50.
const char* const kFaceNames[50] = {
    "Ryu", "Chunli", "Gouki", "Chris", "Wesker", "VJoe", "Dante", "Trish", "Frank", "Spencer",
    "Arthur", "Amaterasu", "Zero", "Tron", "Morrigan", "Leilei", "Felicia", "CViper", "Haggar",
    "Jill", "Hiryu", "Vergil", "Naruhodo", "RedArremer", "Nemesis", "SpiderMan", "CapAmerica",
    "Wolverine", "Magneto", "Hulk", "SheHulk", "TaskMaster", "IronMan", "Thor", "DrDoom",
    "Phoenix", "Shuma", "Modok", "Dormammu", "DeadPool", "Storm", "SuperSkrull", "Sentinel",
    "X23", "Nova", "RRaccoon", "GhostRider", "IronFist", "DrStrange", "HawkEye",
};

// Paths for the icon, avatar and face ranges, built on first use.
TexInfo g_generated[TEX_COUNT - TEX_ICON_FIRST];
char g_generatedPaths[TEX_COUNT - TEX_ICON_FIRST][64];
bool g_generatedReady = false;

void BuildGenerated()
{
    for (int i = 0; i < TEX_COUNT - TEX_ICON_FIRST; ++i) {
        if (i < kLicenseIcons)
            snprintf(g_generatedPaths[i], sizeof(g_generatedPaths[i]),
                     "ui\\lic\\lic_user_i\\lic_user_i%03d_BM_HQ_NOMIP", i);
        else if (i < kLicenseIcons + kAvatarSlots)
            snprintf(g_generatedPaths[i], sizeof(g_generatedPaths[i]),
                     "ui\\lic\\lic_flag_i\\lic_flag_%s_BM_HQ_NOMIP",
                     kAvatarHosts[i - kLicenseIcons]);
        else if (i >= TEX_BANNER_FIRST - TEX_ICON_FIRST) {
            // The game adds "_typeB" or "_typeB_other" for its language.
            const int banner = i - (TEX_BANNER_FIRST - TEX_ICON_FIRST);
            snprintf(g_generatedPaths[i], sizeof(g_generatedPaths[i]),
                     "ui\\res\\res_stgm\\stgm_p\\p_%s%02d_BM_HQ_NOMIP",
                     kFaceNames[banner / 8], banner % 8);
            g_generated[i] = TexInfo{g_generatedPaths[i], 512, 256, ARC_BANNER, false};
            continue;
        } else {
            const int face = i - kLicenseIcons - kAvatarSlots;
            snprintf(g_generatedPaths[i], sizeof(g_generatedPaths[i]),
                     "ui\\chs\\chs_face_a\\chs_cs_f\\f_%s%02d_BM_HQ_NOMIP",
                     kFaceNames[face / 8], face % 8);
            g_generated[i] = TexInfo{g_generatedPaths[i], 128, 128, ARC_LOOSE, false};
            continue;
        }
        g_generated[i] = TexInfo{g_generatedPaths[i], 64, 64, ARC_LICENSE, false};
    }
    g_generatedReady = true;
}

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

Tex LicenseIcon(int number)
{
    if (number < 0 || number >= kLicenseIcons) return TEX_USER_ICON;
    return static_cast<Tex>(TEX_ICON_FIRST + number);
}

Tex CharacterIcon(int32_t characterId)
{
    if (characterId < 1 || characterId > 51) return TEX_USER_ICON;
    return LicenseIcon(kCharacterIcon[characterId - 1]);
}

Tex CharacterFace(int32_t characterId, int color)
{
    if (characterId < 1 || characterId > 50) return TEX_USER_ICON;
    if (color < 0 || color > 7) color = 0;
    return static_cast<Tex>(TEX_FACE_FIRST + (characterId - 1) * 8 + color);
}

Tex CharacterBanner(int32_t characterId, int color)
{
    if (characterId < 1 || characterId > 50) return TEX_USER_ICON;
    if (color < 0 || color > 7) color = 0;
    return static_cast<Tex>(TEX_BANNER_FIRST + (characterId - 1) * 8 + color);
}

const TexInfo& GetTexInfo(Tex tex)
{
    if (tex < TEX_ICON_FIRST) return kTextures[tex];
    if (tex >= TEX_COUNT) return kTextures[TEX_WHITE];
    if (!g_generatedReady) BuildGenerated();
    return g_generated[tex - TEX_ICON_FIRST];
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
