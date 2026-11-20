#pragma once

// Backend-neutral drawing vocabulary for the native lobby UI.
//
// The lobby screens only ever emit three primitives, which are exactly the
// three unit classes the game's own lobby is built from (see
// doc/native_lobby_ui.md): a textured quad (uUiAnmSprite), a 9-slice frame
// (uUiAnmSpriteFrame) and a text run (uUiAnmSpriteFont). Coordinates are the
// game's 1280x720 UI space; the engine scales them to the viewport itself.
//
// Keeping this interface free of game pointers lets the same screen code run
// in the host-side preview harness (tools/lobby_preview).

#include <cstddef>
#include <cstdint>

namespace lobby {

// Identical to uBaseSprite::mCenterAttr: row = value / 3, column = value % 3.
enum Anchor : uint8_t {
    A_TL = 0, A_TC = 1, A_TR = 2,
    A_ML = 3, A_MC = 4, A_MR = 5,
    A_BL = 6, A_BC = 7, A_BR = 8,
};

struct Color {
    float r, g, b, a;
};

constexpr Color kWhite{1.0f, 1.0f, 1.0f, 1.0f};

constexpr Color WithAlpha(Color c, float a) { return Color{c.r, c.g, c.b, c.a * a}; }
constexpr Color Rgb(uint32_t hex, float a = 1.0f)
{
    return Color{((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f,
                 (hex & 0xFF) / 255.0f, a};
}

struct Uv {
    float u0, v0, u1, v1;
};

constexpr Uv kFullUv{0.0f, 0.0f, 1.0f, 1.0f};

// Native texture resources. Every entry is an existing game asset; nothing is
// shipped by the plugin. The archive column says which .arc must be resident.
enum Tex : uint16_t {
    TEX_WHITE,           // 1x1-style white for flat fills
    TEX_TITLE_BAR,       // ga_net_waku_a   1024x64 blue gradient bar
    TEX_PANEL_DARK,      // ga_net_waku_f   dark translucent panel (9-slice)
    TEX_PANEL_DOT,       // ga_net_waku_d   dark dotted inset (9-slice)
    TEX_PANEL_BLUE,      // ga_net_waku_e   blue dotted inset (9-slice)
    TEX_ROW,             // ga_net_waku_b   tan row frame (9-slice)
    TEX_ROW_SEL,         // ga_net_sel_a    yellow selection frame (9-slice)
    TEX_TAGS,            // ga_net_icon_c2  BATTLE/WATCH/HOST/GUEST tags, 8 rows
    TEX_WORDS_A,         // ga_net_word_a   PLAYER 1/2, CHAMPION, CHALLENGER, READY
    TEX_WORDS_B,         // ga_net_word_b   MATCH IN PROGRESS, FIGHT!, FINISH!
    TEX_CROWN,           // ga_net_icon_a   crown
    TEX_FRIENDS,         // ga_net_icon_d   two-heads icon
    TEX_CHAT_BUBBLE,     // ga_net_chat_a   speech bubble (9-slice)
    TEX_LINE,            // ga_net_line_a   thin accent line
    TEX_USER_ICON,       // lic_user_i001   default player icon
    TEX_NUM_GREEN,       // ga_net_num_b    16px digits 0-9
    TEX_SUBTITLES,       // ga_room_subtit  SEARCH LOBBY / CREATE LOBBY ... art
    TEX_SUBTITLES2,      // ga_room_subtit2 LOBBY art
    TEX_SEARCH_RESULTS,  // ga_room_word_a  SEARCH RESULTS art
    TEX_PING,            // ga_room_icon_c  six connection-strength bars
    TEX_MEMBER_ICONS,    // ga_room_icon_d  heads: host, member, empty, friend seat taken/free
    TEX_SLANT_ROW,       // ga_room_waku_b  tan parallelogram (9-slice)
    TEX_SLANT_SEL,       // ga_room_sel_a   yellow parallelogram (9-slice)
    TEX_INSET,           // ga_room_waku_d  dark rounded inset (9-slice)
    TEX_MENU_SEL,        // mai35_sel01b    white main-menu selection bar
    TEX_MENU_BAR,        // mai35_sel02     grey main-menu bar
    TEX_CURSOR,          // mai_cur01       triangle cursor
    TEX_OPT_ARROW,       // ga_opt_sel_1p   option menu left/right arrow (points left)
    TEX_HELP_BAR,        // ga_window_a     dark footer gradient
    TEX_YELLOW_BAR,      // ga_cmn_bar      yellow accent bar
    TEX_PAD_GLYPHS,      // mai_hlp03       controller button glyphs
    TEX_KEY_GLYPHS,      // mai_hlp04       keyboard key glyphs
    TEX_HP_BAR,          // ga_net_hp_a     life bar: black track above, yellow fill below
    TEX_ROOM_BACKDROP,   // meku_menu_co00  the native lobby's comic plate, 1280x720 (DXT1)
    // The license card's icon set, ui\lic\lic_user_i000..239: the icons a
    // player can pick, among them one portrait per character. See
    // LicenseIcon and CharacterIcon.
    TEX_ICON_FIRST,
    TEX_ICON_LAST = TEX_ICON_FIRST + 239,
    // Steam profile pictures. Each slot is a country flag texture (same size
    // and format as the icons, never drawn by the lobby) whose pixels the
    // plugin replaces with a room member's picture while the lobby is up.
    // Slots are handed out by the backend; the screens get one through
    // RoomService::AvatarTex.
    TEX_AVATAR_FIRST,
    TEX_AVATAR_LAST = TEX_AVATAR_FIRST + 15,
    // Character select faces, 128x128: one per character (id 1..50) and
    // color (0..7), loose files under ui\chs\chs_face_a\chs_cs_f. See
    // CharacterFace.
    TEX_FACE_FIRST,
    TEX_FACE_LAST = TEX_FACE_FIRST + 50 * 8 - 1,
    // The "next battle" screen's player 1 cards, 512x256 with the character's
    // name in the art: one per character (id 1..50) and color (0..7), each in
    // a small archive of its own. See CharacterBanner.
    TEX_BANNER_FIRST,
    TEX_BANNER_LAST = TEX_BANNER_FIRST + 50 * 8 - 1,
    // The stage select screen's icons, 128x128: RANDOM first, then one per
    // stage. See StageIcon.
    TEX_STAGE_FIRST,
    TEX_STAGE_LAST = TEX_STAGE_FIRST + 19,
    TEX_COUNT,
};

enum Archive : uint8_t {
    ARC_COMMON,   // ui\cmn      - resident from boot
    ARC_NETROOM,  // ui\netroom  - loaded by the lobby itself
    ARC_MAINMENU, // ui\mnmain   - resident while the title area is alive
    ARC_LICENSE,  // ui\lic      - requested by the lobby when a portrait is first drawn
    ARC_LOOSE,    // a file of its own on disk, read when first drawn
    ARC_BANNER,   // ui\res\arc\chr\l<id>_<color>, requested when first drawn
    ARC_STAGE,    // ui\mnchsstg, the stage select screen's, requested when first drawn
};

struct TexInfo {
    // Resource path without extension. Localized ("typeD") art is stored
    // without the type/language suffix; the native name composer adds it.
    const char* path;
    uint16_t width;
    uint16_t height;
    Archive archive;
    bool localized;
};

const TexInfo& GetTexInfo(Tex tex);
constexpr int kLicenseIcons = TEX_ICON_LAST - TEX_ICON_FIRST + 1;
constexpr int kAvatarSlots = TEX_AVATAR_LAST - TEX_AVATAR_FIRST + 1;
// License icon by number (0..239); the default player icon otherwise.
Tex LicenseIcon(int number);
// Portrait for a character id (1..51); the default player icon otherwise.
Tex CharacterIcon(int32_t characterId);

// Character select face for a character id (1..50) in one of its colors;
// colors past the eight that have art show the first. The default player
// icon for anything else.
Tex CharacterFace(int32_t characterId, int color);

// The same character and color as the wide card the game shows before a
// battle. The art is the 448x176 window kBannerUv of the texture. It is not
// drawn until its archive has loaded.
Tex CharacterBanner(int32_t characterId, int color);
constexpr Uv kBannerUv{32.0f / 512.0f, 40.0f / 256.0f, 480.0f / 512.0f, 216.0f / 256.0f};
constexpr float kBannerAspect = 176.0f / 448.0f; // height over width
// Character id and color of a banner texture.
constexpr int32_t BannerCharacter(Tex tex) { return (tex - TEX_BANNER_FIRST) / 8 + 1; }
constexpr int BannerColor(Tex tex) { return (tex - TEX_BANNER_FIRST) % 8; }

// The stage select screen's icon for a game stage id; its RANDOM icon for
// anything else. The art is the 114x70 window kStageIconUv of the texture.
Tex StageIcon(int stageId);
constexpr Uv kStageIconUv{7.0f / 128.0f, 1.0f / 128.0f, 121.0f / 128.0f, 71.0f / 128.0f};
constexpr float kStageIconAspect = 70.0f / 114.0f; // height over width

// A player's lobby icon as published to the room: kIconSteam for their Steam
// profile picture, otherwise a license icon's number + 1.
constexpr int kIconSteam = 0;
constexpr int kIconMax = kLicenseIcons;

// The game ships five fonts; these two are the ones menus use.
enum Font : uint8_t {
    FONT_BODY = 0,    // nrodinpro, 32px cell: every list and body string
    FONT_HEADING = 4, // bada_b, 64px cell: bold outlined menu headings
};

// uBaseSprite::mBlendState values the menus use for text.
enum Blend : uint8_t {
    BLEND_NORMAL = 1,
    // What the native main menu puts on its selected label: drawn in white,
    // the glyph fill comes out black on the white bar and the font's dark
    // outline disappears, so the letters are thinner than a black tint gives.
    BLEND_MENU_INK = 5,
};

class Canvas {
public:
    virtual ~Canvas() = default;

    // (x, y) is the anchor point; (w, h) the on-screen size.
    virtual void Sprite(Tex tex, float x, float y, float w, float h, Uv uv,
                        Anchor anchor, Color color) = 0;

    // (w, h) is the size of the stretched centre, as in the native
    // uUiAnmSpriteFrame: the texture's outer thirds are added around it.
    virtual void Frame(Tex tex, float x, float y, float w, float h, Uv uv,
                       Anchor anchor, Color color) = 0;

    // `scale` is the horizontal scale; the vertical one is scale * stretch
    // (the native main menu draws its labels about 1.3 times taller than wide).
    virtual void Text(const char* utf8, float x, float y, float scale,
                      Anchor anchor, Color color, Font font = FONT_BODY,
                      float stretch = 1.0f, Blend blend = BLEND_NORMAL) = 0;
};

// Advance-width sum at scale 1.0 using the fonts' own metric tables.
float TextWidth(const char* utf8, Font font = FONT_BODY);

// Copies `text` into `out`, replacing the tail with "..." when it would exceed
// maxWidth at the given scale. Never splits a UTF-8 sequence.
void FitText(char* out, size_t outSize, const char* text, float maxWidth,
             float scale, Font font = FONT_BODY);

// How a texture is used as a 9-slice frame: the UV window holding the frame
// art, and how far the visible art is inset from that window's edges (most
// frame textures carry a transparent margin or a drop shadow).
struct FrameStyle {
    Uv uv;
    float insetLeft, insetTop, insetRight, insetBottom;
    // Repeat the centre cell instead of stretching it (mFrameAttr bit 0), for
    // patterned plates whose texture would smear when stretched.
    bool tileCenter;
};
const FrameStyle& GetFrameStyle(Tex tex);

// Border thickness contributed by a 9-slice frame on each side (unscaled):
// one third of the UV window, as in uUiAnmSpriteFrame's default centre UV.
float FrameBorderX(Tex tex, Uv uv);
float FrameBorderY(Tex tex, Uv uv);

} // namespace lobby
