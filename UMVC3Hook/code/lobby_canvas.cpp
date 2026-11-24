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
    {"ui\\mainmenu\\ma_sprite\\meku_menu_co00_BM_NOMIP", 1280, 720, ARC_NETROOM, false},
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

// Game stage id of each stage icon slot; slot 0 is the RANDOM icon.
const int16_t kStageIconIds[TEX_STAGE_LAST - TEX_STAGE_FIRST + 1] = {
    -1, 0, 2, 3, 4, 5, 6, 7, 9, 10, 11, 100, 102, 103, 104, 105, 106, 107, 109, 110,
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
        else if (i == TEX_SPACE_STARS - TEX_ICON_FIRST)
            snprintf(g_generatedPaths[i], sizeof(g_generatedPaths[i]),
                     "ui\\lic\\lic_flag_i\\lic_flag_germany_BM_HQ_NOMIP");
        else if (i == TEX_FACE_RANDOM - TEX_ICON_FIRST || i == TEX_FACE_RANDOM_ALL - TEX_ICON_FIRST) {
            snprintf(g_generatedPaths[i], sizeof(g_generatedPaths[i]),
                     "ui\\chs\\chs_face_a\\chs_cs_f\\f_Random%s_BM_HQ_NOMIP",
                     i == TEX_FACE_RANDOM - TEX_ICON_FIRST ? "" : "_all");
            g_generated[i] = TexInfo{g_generatedPaths[i], 128, 128, ARC_LOOSE, true};
            continue;
        } else if (i >= TEX_STAGE_FIRST - TEX_ICON_FIRST) {
            // Slot 0 is the RANDOM icon ("99", localized as typeC); the
            // second set of stages (ids 100 and up) is the "_b" art.
            const int stage = i - (TEX_STAGE_FIRST - TEX_ICON_FIRST);
            const int id = stage == 0 ? 99 : kStageIconIds[stage];
            snprintf(g_generatedPaths[i], sizeof(g_generatedPaths[i]),
                     "ui\\chs\\chs_stgt\\chs_st_m\\chs_stgm%02d%s_BM_HQ_NOMIP", id % 100,
                     stage != 0 && id >= 100 ? "_b" : "");
            g_generated[i] = TexInfo{g_generatedPaths[i], 128, 128, ARC_STAGE, stage == 0};
            continue;
        } else if (i >= TEX_BANNER_FIRST - TEX_ICON_FIRST) {
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

// kGlyphCodepoints / kGlyphIndices (the game's glyph order beyond ASCII) and
// kBodyWidths / kHeadingWidths (advance widths by glyph index, from the fonts'
// FSD metric tables ui\font\nrodinpro and ui\font\bada_b).
#include "lobby_font_tables.inc"

constexpr int kMissingGlyph = '?' - 0x20; // what the game draws for anything else

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

// The glyph the game draws for a codepoint, as
// sGameConfig__mapUtf8GlyphToFontIndex does it: ASCII is `codepoint - 0x20`,
// everything else is looked up in the glyph order. -1 when there is none.
int GlyphIndex(uint32_t codepoint)
{
    if (codepoint < 0x80) return codepoint >= 0x20 && codepoint != 0x7F ? static_cast<int>(codepoint) - 0x20 : -1;
    if (codepoint > 0xFFFF) return -1;
    size_t low = 0, high = sizeof(kGlyphCodepoints) / sizeof(kGlyphCodepoints[0]);
    while (low < high) {
        const size_t middle = (low + high) / 2;
        if (kGlyphCodepoints[middle] < codepoint) low = middle + 1;
        else high = middle;
    }
    if (low < sizeof(kGlyphCodepoints) / sizeof(kGlyphCodepoints[0]) &&
        kGlyphCodepoints[low] == codepoint)
        return kGlyphIndices[low];
    return -1;
}

float GlyphWidth(uint32_t codepoint, Font font)
{
    if (codepoint < 0x20) return 0.0f; // control characters are skipped
    int glyph = GlyphIndex(codepoint);
    if (font == FONT_HEADING) {
        if (glyph < 0 || glyph >= static_cast<int>(sizeof(kHeadingWidths))) glyph = kMissingGlyph;
        return kHeadingWidths[glyph];
    }
    if (glyph < 0) glyph = kMissingGlyph;
    return kBodyWidths[glyph];
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

Tex StageIcon(int stageId)
{
    for (int i = 1; i <= TEX_STAGE_LAST - TEX_STAGE_FIRST; ++i)
        if (kStageIconIds[i] == stageId) return static_cast<Tex>(TEX_STAGE_FIRST + i);
    return TEX_STAGE_FIRST;
}

const TexInfo& GetTexInfo(Tex tex)
{
    if (tex < TEX_ICON_FIRST) return kTextures[tex];
    if (tex >= TEX_COUNT) return kTextures[TEX_WHITE];
    if (!g_generatedReady) BuildGenerated();
    return g_generated[tex - TEX_ICON_FIRST];
}

size_t DecodeUtf8(const char* text, uint32_t* codepoint)
{
    const unsigned char lead = static_cast<unsigned char>(text[0]);
    size_t length = 1;
    uint32_t value = lead, least = 0;
    if (lead >= 0xC2 && lead < 0xE0) { length = 2; value = lead & 0x1F; least = 0x80; }
    else if (lead >= 0xE0 && lead < 0xF0) { length = 3; value = lead & 0x0F; least = 0x800; }
    else if (lead >= 0xF0 && lead < 0xF5) { length = 4; value = lead & 0x07; least = 0x10000; }
    else if (lead >= 0x80) value = 0xFFFD;
    for (size_t i = 1; i < length; ++i) {
        const unsigned char next = static_cast<unsigned char>(text[i]);
        if ((next & 0xC0) != 0x80) { // also stops at the terminator
            *codepoint = 0xFFFD;
            return 1;
        }
        value = (value << 6) | (next & 0x3F);
    }
    if (length > 1 && (value < least || value > 0x10FFFF || (value >= 0xD800 && value <= 0xDFFF))) {
        *codepoint = 0xFFFD;
        return 1;
    }
    *codepoint = value;
    return length;
}

size_t Utf8Count(const char* utf8)
{
    size_t count = 0;
    uint32_t codepoint;
    for (; utf8 && *utf8; utf8 += DecodeUtf8(utf8, &codepoint)) ++count;
    return count;
}

bool FontHasGlyph(uint32_t codepoint, Font font)
{
    if (font == FONT_HEADING) {
        // The heading atlas has 256 cells, and a few of those the glyph
        // order reaches hold a different character (its 0x64, 0x65, 0x77,
        // 0x78 and 0x7E are a currency sign, a backslash, oe, OE and an
        // inverted question mark), so only these are what they should be.
        return (codepoint >= 0x20 && codepoint <= 0x7E) || (codepoint >= 0xC0 && codepoint <= 0xFF) ||
               codepoint == 0xA1 || codepoint == 0xA2 || codepoint == 0xA3 || codepoint == 0xA9 ||
               codepoint == 0xAE;
    }
    return GlyphIndex(codepoint) >= 0;
}

bool FontHasText(const char* utf8, Font font)
{
    while (utf8 && *utf8) {
        uint32_t codepoint;
        utf8 += DecodeUtf8(utf8, &codepoint);
        if (!FontHasGlyph(codepoint, font)) return false;
    }
    return true;
}

float TextWidth(const char* utf8, Font font)
{
    if (!utf8) return 0.0f;
    float width = 0.0f;
    const size_t total = strlen(utf8);
    for (size_t i = 0; i < total;) {
        uint32_t codepoint;
        const size_t length = DecodeUtf8(utf8 + i, &codepoint);
        width += GlyphWidth(codepoint, font);
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
        uint32_t codepoint;
        const size_t length = DecodeUtf8(text + i, &codepoint);
        const float glyph = GlyphWidth(codepoint, font);
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
            uint32_t codepoint;
            const size_t length = DecodeUtf8(text + i, &codepoint);
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
