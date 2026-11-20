// Native-engine backend for the lobby UI. Addresses are preferred-image VAs
// from umvc3.exe (unpacked); doc/native_lobby_ui.md records how each one was
// identified.
#include "../pch.h"
#include "lobby_native.h"
#include "lobby_avatar.h"
#include "steam_lobby.h"
#include "lobby_ui.h"
#include "mk10utils.h"

#include <Windows.h>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

extern void HookLog(const char* fmt, ...);

namespace lobby {

namespace {

// ---------------------------------------------------------------------------
// Native entry points
// ---------------------------------------------------------------------------
constexpr uintptr_t SRESOURCE_GET_INSTANCE = 0x140001B10;
constexpr uintptr_t SRESOURCE_VT_CREATE = 0x60;           // (this, MtDTI*, const char*, u32)
constexpr uintptr_t RTEXTURE_DTI = 0x140E17570;
// Archive request, as sGameUi__loadNetRoomArchive (0x1402BDE30) makes it:
// compose the language-specific name, then sResource::create with the
// archive DTI and the asynchronous flag.
constexpr uintptr_t RARCHIVE_DTI = 0x140E17BD8;
constexpr uintptr_t COMPOSE_ARCHIVE_NAME = 0x140455260; // (buffer[272], 3, name, 0) -> object
constexpr uintptr_t ARCHIVE_NAME_CSTR = 0x140151080;    // (object) -> const char*
constexpr uintptr_t CRESOURCE_RELEASE_REF = 0x14050D5A0;
constexpr uintptr_t SGAMEUI_GET_INSTANCE = 0x140001AC0;
constexpr uintptr_t SGAMEUI_LOAD_NETROOM_ARCHIVE = 0x1402BDE30;
constexpr uintptr_t SGAMEUI_NETROOM_ARCHIVE_STATUS = 0x1402BCEF0; // 0 ready, 1 loading, -1 none
// (mode, base, out[0x104], appendTypeTag): mode 3 yields "<base>_typeD[_<lang>]",
// mode 2 "<base>_typeB[_other]".
constexpr uintptr_t COMPOSE_LOCALIZED_NAME = 0x1404552A0;
// (unused context, path, uUiScheduler** out, moveLine): loads an rScheduler,
// constructs a uUiScheduler for it and adds it to sUnit.
constexpr uintptr_t UI_LOAD_SCHEDULER = 0x1403E9550;
constexpr uintptr_t USCHEDULER_GET_UNIT_INDEX = 0x14050EE60;     // (this, MtDTI*, name)
constexpr uintptr_t USCHEDULER_GET_UNIT = 0x14050EE40;           // (this, index)
constexpr uintptr_t USCHEDULER_SET_PAUSE = 0x14050F550;
constexpr uintptr_t SUNIT_GET_INSTANCE = 0x140003160;
constexpr uintptr_t SUNIT_ADD_BOTTOM = 0x14051A6B0;              // (this, line, unit, 0)
constexpr uintptr_t CUNIT_GET_MOVE_LINE = 0x14007AB30;
constexpr uintptr_t CUNIT_VT_KILL = 0x78;

constexpr uintptr_t UUIANMSPRITE_DTI = 0x140D53CF8;
constexpr uintptr_t UUIANMSPRITE_NEW = 0x1402C9E30;
constexpr uintptr_t UUIANMSPRITE_COPY = 0x1402C9DC0;             // (this, source)
constexpr uintptr_t UUIANMSPRITEFRAME_DTI = 0x140D543A8;
constexpr uintptr_t UUIANMSPRITEFRAME_NEW = 0x1402CF8C0;
constexpr uintptr_t UUIANMSPRITEFRAME_COPY = 0x1402CF630;
constexpr uintptr_t UUIANMSPRITEFONT_DTI = 0x140D53D88;
constexpr uintptr_t UUIANMSPRITEFONT_NEW = 0x1402CAA50;
constexpr uintptr_t UUIANMSPRITEFONT_COPY = 0x1402CA2D0;         // (this, source, -, -)
constexpr size_t UUIANMSPRITE_SIZE = 0x540;
constexpr size_t UUIANMSPRITEFRAME_SIZE = 0x580;
constexpr size_t UUIANMSPRITEFONT_SIZE = 0x760;
constexpr uint32_t kUnitAlignment = 0x10;
constexpr uintptr_t UBASESPRITE_SET_TEXTURE = 0x1402C6E00;       // (this, rTexture*)
constexpr uintptr_t UUIANMSPRITEFONT_SET_TEXT = 0x1402CEF30;     // reset message index + text
constexpr uintptr_t UUIANMSPRITEFONT_SET_FONT_TYPE = 0x1402CE4E0;

constexpr uintptr_t SPAD_GET_INSTANCE = 0x140001B00;
constexpr uintptr_t SPAD_DECIDE_MASK = 0x140001A70;
constexpr uintptr_t SPAD_REDETECT_SLOT = 0x140514950;  // (this, slot, flags)
constexpr uintptr_t SPAD_CANCEL_MASK = 0x140001A50;
constexpr uintptr_t MENU_SE_DECIDE = 0x14032A7F0;  // system voice 9
constexpr uintptr_t MENU_SE_CANCEL = 0x1403F78C0;  // system voice 10
// The main menu family's cues (vtable +0x140 / +0x148, neither reads `this`):
// an item decided (bank 1 voice 6 with bank 9 voice 8), and a menu closing
// back to its parent (bank 1 voice 7). The mode change that closes a menu
// (0x1403EABE0) plays bank 9 voice 1 just before the latter.
constexpr uintptr_t MAINMENU_SE_DECIDE = 0x1403EA970;
constexpr uintptr_t MAINMENU_SE_EXIT = 0x1403EA8B0;
constexpr uint32_t kMenuExitBank = 9;
constexpr uint32_t kMenuExitVoice = 1;
// The cue aChrSelect__enterStageSelection (0x140366EF0) plays when character
// select moves on to the stage: voice 0xA0 of sound bank 6.
constexpr uintptr_t GSOUND_INSTANCE = 0x140003AA0;
constexpr uintptr_t GSOUND_RESOLVE_REF = 0x1402AA2B0;      // (this, cRef* out, bank)
constexpr uintptr_t GSOUND_PLAY_ORDINARY_VOICE = 0x1402AAC40; // (this, cRef* out, ref, id, 0, 1.0f, 0)
constexpr uint32_t kStageSelectBank = 6;
constexpr uint32_t kStageSelectVoice = 0xA0;
constexpr uintptr_t MENU_SE_CURSOR = 0x1403F7920;  // system voice 8 when +0x88 != +0x8C

// ---------------------------------------------------------------------------
// Native layouts (cUnit / uCoord / uBaseSprite / derived)
// ---------------------------------------------------------------------------
constexpr size_t CUNIT_FLAGS = 0x10;         // bits 0-2 state, bit 11 draw
constexpr uint32_t CUNIT_DRAW = 0x800;
constexpr size_t UCOORD_PARENT = 0x40;
constexpr size_t UCOORD_PARENT_NO = 0x48;
constexpr size_t UCOORD_POS = 0x50;
constexpr size_t UCOORD_SCALE = 0x70;
constexpr size_t SPRITE_GEOMETRY_DIRTY = 0x108;
constexpr size_t SPRITE_COLOR = 0x310;       // packed R,G,B,A bytes
constexpr size_t SPRITE_SIZE = 0x330;
constexpr size_t SPRITE_UV0 = 0x338;
constexpr size_t SPRITE_UV1 = 0x340;
constexpr size_t SPRITE_UV_OFFSET = 0x348;
constexpr size_t SPRITE_TEXTURE = 0x350;
constexpr size_t SPRITE_DEPTH = 0x360;
constexpr size_t SPRITE_ATTR = 0x364;
constexpr uint32_t SPRITE_ATTR_AUTO_SIZE = 0x1; // resize to the texture on assignment
constexpr uint32_t SPRITE_ATTR_SHADOW = 0x2;
constexpr uint32_t SPRITE_ATTR_GRADIENT = 0x10;
constexpr size_t SPRITE_CENTER_ATTR = 0x368;
constexpr size_t SPRITE_BLEND_STATE = 0x398;
constexpr size_t FRAME_ATTR = 0x540;         // bit 0: tile the centre cell
constexpr size_t FONT_MSG_INDEX = 0x594;     // -1 while showing a raw string
constexpr size_t FONT_CACHED_MSG_ID = 0x6F0;
constexpr size_t FONT_TYPE = 0x700;
constexpr size_t FONT_FIX_SIZE = 0x75B;
constexpr size_t USCHEDULER_UNIT_TRACK_NUM = 0x68;
constexpr size_t SPAD_SLOT_BASE = 0x58;
constexpr size_t SPAD_SLOT_STRIDE = 0x2E0;
constexpr size_t SPAD_SLOT_ACTIVE = 0x08;
constexpr size_t SPAD_SLOT_KIND = 0x0B;      // 0 none, 2 DirectInput, others XInput
constexpr size_t SPAD_SLOT_BUTTONS = 0x164;

// Internal pad mask bits (sPad__decodeXInputState_toButtons).
constexpr uint32_t PAD_START = 0x8;
constexpr uint32_t PAD_UP = 0x10010, PAD_RIGHT = 0x20020, PAD_DOWN = 0x40040, PAD_LEFT = 0x80080;
constexpr uint32_t PAD_LB = 0x100, PAD_RB = 0x200, PAD_Y = 0x1000, PAD_X = 0x8000;

// Prototype units cloned for every primitive. All three live in the native
// lobby's own layout, so they carry exactly the render state it uses.
constexpr char kPrototypeScheduler[] = "ui\\net\\net_sprite\\net_room_free2";
constexpr char kSpritePrototype[] = "icon_RoomFriend";
constexpr char kFramePrototype[] = "Frame_RoomTitle";
constexpr char kFontPrototype[] = "Font_Log_a2";

// Draw order: the engine sorts by 0x7FFD - mDepth, so smaller is nearer.
// Commands are issued back-to-front and step toward the viewer.
constexpr int kDepthFar = 31800;
constexpr int kDepthStep = 3;

constexpr size_t kMaxUnitsPerKind = 768;

template <typename T>
T& Field(void* object, size_t offset)
{
    return *reinterpret_cast<T*>(static_cast<uint8_t*>(object) + offset);
}

template <typename Fn>
Fn Native(uintptr_t address)
{
    return reinterpret_cast<Fn>(_addr(address));
}

bool IsUnitAlive(void* unit)
{
    // MtObject__isAliveState: state 1 (queued) or 2 (running).
    return unit && ((Field<uint32_t>(unit, CUNIT_FLAGS) & 7u) - 1u) < 2u;
}

void SetDraw(void* unit, bool draw)
{
    uint32_t& flags = Field<uint32_t>(unit, CUNIT_FLAGS);
    flags = draw ? (flags | CUNIT_DRAW) : (flags & ~CUNIT_DRAW);
}

void KillUnit(void* unit)
{
    if (!IsUnitAlive(unit)) return;
    using Kill = void(__fastcall*)(void*);
    (*reinterpret_cast<Kill*>(*reinterpret_cast<uint8_t**>(unit) + CUNIT_VT_KILL))(unit);
}

uint32_t PackColor(Color color)
{
    const auto channel = [](float value) -> uint32_t {
        if (value <= 0.0f) return 0;
        if (value >= 1.0f) return 255;
        return static_cast<uint32_t>(value * 255.0f + 0.5f);
    };
    return channel(color.r) | (channel(color.g) << 8) | (channel(color.b) << 16) |
           (channel(color.a) << 24);
}

// ---------------------------------------------------------------------------
// Unit pools
// ---------------------------------------------------------------------------
enum Kind { KIND_SPRITE, KIND_FRAME, KIND_FONT, KIND_COUNT };

struct Pool {
    void* prototype = nullptr;
    uintptr_t vtable = 0;      // class vtable of our clones, for sanity checks
    std::vector<void*> units;
    size_t used = 0;
    size_t usedLastFrame = 0;
};

enum class Stage { Idle, WaitArchive, WaitPrototypes, Ready };

Stage g_stage = Stage::Idle;
Pool g_pools[KIND_COUNT];
void* g_scheduler = nullptr;
void* g_textures[TEX_COUNT] = {};
uint32_t g_textureRetryFrame[TEX_COUNT] = {};
bool g_textureFailureLogged[TEX_COUNT] = {};
uint32_t g_frame = 0;
uint32_t g_stageFrames = 0;
int g_nextDepth = kDepthFar;

void* CloneUnit(Kind kind)
{
    Pool& pool = g_pools[kind];
    if (!IsUnitAlive(pool.prototype) || pool.units.size() >= kMaxUnitsPerKind) return nullptr;

    // The class operator new is (size, alignment): it forwards both to the
    // class allocator together with the DTI id. Sizes are the native object
    // sizes of the three classes.
    using Allocate = void*(__fastcall*)(size_t, uint32_t);
    using Copy = void*(__fastcall*)(void*, void*);
    using CopyFont = void*(__fastcall*)(void*, void*, uint64_t, uint64_t);
    void* unit = nullptr;
    switch (kind) {
    case KIND_SPRITE:
        if (void* memory = Native<Allocate>(UUIANMSPRITE_NEW)(UUIANMSPRITE_SIZE, kUnitAlignment))
            unit = Native<Copy>(UUIANMSPRITE_COPY)(memory, pool.prototype);
        break;
    case KIND_FRAME:
        if (void* memory = Native<Allocate>(UUIANMSPRITEFRAME_NEW)(UUIANMSPRITEFRAME_SIZE, kUnitAlignment))
            unit = Native<Copy>(UUIANMSPRITEFRAME_COPY)(memory, pool.prototype);
        break;
    default:
        if (void* memory = Native<Allocate>(UUIANMSPRITEFONT_NEW)(UUIANMSPRITEFONT_SIZE, kUnitAlignment))
            unit = Native<CopyFont>(UUIANMSPRITEFONT_COPY)(memory, pool.prototype, 0, 0);
        break;
    }
    if (!unit) return nullptr;

    // Clones are free-standing: no scheduler parent, no inherited effects.
    Field<void*>(unit, UCOORD_PARENT) = nullptr;
    Field<int32_t>(unit, UCOORD_PARENT_NO) = -1;
    Field<float>(unit, SPRITE_UV_OFFSET) = 0.0f;
    Field<float>(unit, SPRITE_UV_OFFSET + 4) = 0.0f;
    if (kind == KIND_FONT)
        Field<uint8_t>(unit, FONT_FIX_SIZE) = 0;
    else
        // The sprite prototype sizes itself from its texture; ours are sized
        // explicitly per draw command.
        Field<uint32_t>(unit, SPRITE_ATTR) &=
            ~(SPRITE_ATTR_AUTO_SIZE | SPRITE_ATTR_SHADOW | SPRITE_ATTR_GRADIENT);
    SetDraw(unit, false);

    using GetInstance = void*(__fastcall*)();
    using GetMoveLine = uint32_t(__fastcall*)(void*);
    using AddBottom = bool(__fastcall*)(void*, uint32_t, void*, uint32_t);
    void* unitManager = Native<GetInstance>(SUNIT_GET_INSTANCE)();
    const uint32_t line = Native<GetMoveLine>(CUNIT_GET_MOVE_LINE)(pool.prototype);
    if (!unitManager || !Native<AddBottom>(SUNIT_ADD_BOTTOM)(unitManager, line, unit, 0)) {
        HookLog("LobbyNative: sUnit::addBottom rejected a kind-%d clone\n", static_cast<int>(kind));
        return nullptr;
    }
    if (!pool.vtable) pool.vtable = *reinterpret_cast<uintptr_t*>(unit);
    pool.units.push_back(unit);
    return unit;
}

void* AcquireUnit(Kind kind)
{
    Pool& pool = g_pools[kind];
    if (pool.used < pool.units.size()) return pool.units[pool.used++];
    void* unit = CloneUnit(kind);
    if (unit) ++pool.used;
    return unit;
}

// ui\lic holds the character portraits. Nothing in the title area loads it, so
// the lobby asks for it the first time a portrait is wanted and keeps the
// handle for good; the textures then resolve on a later retry.
void* g_licenseArchive = nullptr;
// ui\mnchsstg holds the stage select screen's icons (and its large previews,
// about 7 MB in all). It is asked for the first time a stage icon is wanted
// and given back when the lobby's units are released.
void* g_stageArchive = nullptr;

// Requests a language-specific UI archive by its base name ("ui\\lic").
void RequestArchive(void*& handle, const char* name)
{
    if (handle) return;
    using GetInstance = void*(__fastcall*)();
    using Compose = void*(__fastcall*)(void*, int, const char*, int);
    using CStr = const char*(__fastcall*)(void*);
    using Create = void*(__fastcall*)(void*, void*, const char*, uint32_t);
    void* resources = Native<GetInstance>(SRESOURCE_GET_INSTANCE)();
    if (!resources) return;
    alignas(16) uint8_t buffer[0x120] = {};
    void* composed = Native<Compose>(COMPOSE_ARCHIVE_NAME)(buffer, 3, name, 0);
    const char* path = composed ? Native<CStr>(ARCHIVE_NAME_CSTR)(composed) : nullptr;
    if (!path) return;
    const Create create = *reinterpret_cast<Create*>(
        *reinterpret_cast<uint8_t**>(resources) + SRESOURCE_VT_CREATE);
    handle = create(resources, reinterpret_cast<void*>(_addr(RARCHIVE_DTI)), path, 2);
    HookLog("LobbyNative: requested the %s archive (%p)\n", path, handle);
}

void RequestLicenseArchive()
{
    RequestArchive(g_licenseArchive, "ui\\lic");
}

// The wide character cards (ARC_BANNER) come in an archive per character and
// color, which the game loads for its "next battle" screen as
// sGameUI__loadNextBattlePreviewArchives (0x1402BDEC0) does: sResource::create
// with the archive DTI and the asynchronous flag. A handle is kept for each
// one asked for until the lobby's units are released.
void* g_bannerArchives[TEX_BANNER_LAST - TEX_BANNER_FIRST + 1] = {};
constexpr size_t CRESOURCE_STATE = 0x54;       // bit 0 loaded, bit 4 load failed
constexpr uint32_t kResourceSettled = 0x11;

// True once the card's archive has been read (or has failed to).
bool BannerArchiveReady(Tex tex)
{
    void*& archive = g_bannerArchives[tex - TEX_BANNER_FIRST];
    if (!archive) {
        using GetInstance = void*(__fastcall*)();
        using Create = void*(__fastcall*)(void*, void*, const char*, uint32_t);
        void* resources = Native<GetInstance>(SRESOURCE_GET_INSTANCE)();
        if (!resources) return false;
        char path[64];
        snprintf(path, sizeof(path), "ui\\res\\arc\\chr\\l%04d_%02d",
                 static_cast<int>(BannerCharacter(tex)), BannerColor(tex));
        const Create create = *reinterpret_cast<Create*>(
            *reinterpret_cast<uint8_t**>(resources) + SRESOURCE_VT_CREATE);
        archive = create(resources, reinterpret_cast<void*>(_addr(RARCHIVE_DTI)), path, 2);
        if (!archive) return false;
    }
    return (Field<uint32_t>(archive, CRESOURCE_STATE) & kResourceSettled) != 0;
}

void ReleaseBannerArchives()
{
    using Release = void(__fastcall*)(void*);
    if (g_stageArchive) Native<Release>(CRESOURCE_RELEASE_REF)(g_stageArchive);
    g_stageArchive = nullptr;
    for (void*& archive : g_bannerArchives) {
        if (archive) Native<Release>(CRESOURCE_RELEASE_REF)(archive);
        archive = nullptr;
    }
}

constexpr int kLooseLoadsPerFrame = 4;
uint32_t g_looseLoadFrame = 0;
int g_looseLoads = 0;

void* GetTexture(Tex tex)
{
    if (tex >= TEX_COUNT) return nullptr;
    if (g_textures[tex]) return g_textures[tex];
    if (g_frame < g_textureRetryFrame[tex]) return nullptr;

    const TexInfo& info = GetTexInfo(tex);
    if (info.archive == ARC_LICENSE) RequestLicenseArchive();
    // Asked every frame while it loads: nothing is read from disk here.
    if (info.archive == ARC_BANNER && !BannerArchiveReady(tex)) return nullptr;
    if (info.archive == ARC_STAGE) {
        RequestArchive(g_stageArchive, "ui\\mnchsstg");
        if (!g_stageArchive ||
            !(Field<uint32_t>(g_stageArchive, CRESOURCE_STATE) & kResourceSettled))
            return nullptr;
    }
    if (info.archive == ARC_LOOSE) {
        // These are read from disk on the spot (sResource__createFromDTIPath
        // 0x140516D70, flag bit 0), so only a few are taken per frame; the
        // rest of a freshly opened grid fills in over the next frames.
        if (g_looseLoadFrame != g_frame) {
            g_looseLoadFrame = g_frame;
            g_looseLoads = 0;
        }
        if (g_looseLoads >= kLooseLoadsPerFrame) return nullptr;
        ++g_looseLoads;
    }
    char path[0x108] = {};
    if (info.localized || info.archive == ARC_BANNER) {
        // The cards are "typeB" art: one for Japanese, "_other" for the rest.
        using Compose = void(__fastcall*)(int, const char*, char*, char);
        // The stage select's RANDOM icon is "typeC" art (mode 1).
        const int mode = info.archive == ARC_BANNER ? 2 : info.archive == ARC_STAGE ? 1 : 3;
        Native<Compose>(COMPOSE_LOCALIZED_NAME)(mode, info.path, path, 1);
    } else {
        snprintf(path, sizeof(path), "%s", info.path);
    }

    using GetInstance = void*(__fastcall*)();
    using Create = void*(__fastcall*)(void*, void*, const char*, uint32_t);
    void* resources = Native<GetInstance>(SRESOURCE_GET_INSTANCE)();
    void* texture = nullptr;
    if (resources) {
        const Create create = *reinterpret_cast<Create*>(
            *reinterpret_cast<uint8_t**>(resources) + SRESOURCE_VT_CREATE);
        texture = create(resources, reinterpret_cast<void*>(_addr(RTEXTURE_DTI)), path, 1);
    }
    if (!texture) {
        // The owning archive may simply not be resident yet; retry later.
        g_textureRetryFrame[tex] = g_frame + 45;
        if (!g_textureFailureLogged[tex]) {
            g_textureFailureLogged[tex] = true;
            HookLog("LobbyNative: texture not available yet: %s\n", path);
        }
        return nullptr;
    }
    g_textures[tex] = texture;
    return texture;
}

// ---------------------------------------------------------------------------
// Steam profile pictures
//
// rTexture keeps its GPU texture (nDraw::Texture) at +0xE0 (rTexture__load
// 0x140515240, rTexture__setHandle 0x140515A60). A picture is converted to the
// icon textures' own format and built into a new nDraw::Texture exactly as
// rTexture__load builds one (allocation 0x140781C40, constructor 0x140781720,
// which copies the pixels). That texture is swapped into a country flag
// texture (same size and format as the icons, never drawn by the lobby) for
// as long as the lobby holds it; the original pointer is kept and put
// back, and ours released (vtable +0x28, deferred by the engine while a frame
// still references it), before the icon's resource reference is dropped.
// ---------------------------------------------------------------------------
constexpr uintptr_t NDRAW_TEXTURE_ALLOC = 0x140781C40;  // (size, align) -> memory
constexpr uintptr_t NDRAW_TEXTURE_CTOR = 0x140781720;
constexpr size_t NDRAW_TEXTURE_SIZE = 0x78;
constexpr size_t NDRAW_TEXTURE_VT_RELEASE = 0x28;
constexpr size_t RTEXTURE_GPU_TEXTURE = 0xE0;
constexpr size_t RTEXTURE_WIDTH = 0xF0;
constexpr size_t RTEXTURE_HEIGHT = 0xF4;
constexpr uint32_t kTexFormatIcon = 0x2A;
constexpr uint32_t kAvatarRetryFrames = 90;   // Steam has not cached the picture yet
constexpr uint32_t kAvatarKeepFrames = 180;   // a slot drawn this recently is not reused

struct AvatarSlot {
    uint64_t steamId = 0;
    void* original = nullptr; // the icon's own GPU texture, restored on release
    void* ours = nullptr;
    uint32_t lastUsed = 0;
    uint32_t nextTry = 0;
};
AvatarSlot g_avatars[kAvatarSlots];
bool g_avatarLogged = false;

Tex AvatarSlotTex(int slot) { return static_cast<Tex>(TEX_AVATAR_FIRST + slot); }

// Puts the icon's own texture back and frees ours.
void ReleaseAvatar(int index)
{
    AvatarSlot& slot = g_avatars[index];
    if (slot.ours) {
        if (void* texture = g_textures[AvatarSlotTex(index)])
            Field<void*>(texture, RTEXTURE_GPU_TEXTURE) = slot.original;
        using Release = void(__fastcall*)(void*);
        (*reinterpret_cast<Release*>(*reinterpret_cast<uint8_t**>(slot.ours) +
                                     NDRAW_TEXTURE_VT_RELEASE))(slot.ours);
    }
    slot = AvatarSlot{};
}

void ReleaseAllAvatars()
{
    for (int i = 0; i < kAvatarSlots; ++i) ReleaseAvatar(i);
}

bool UploadAvatar(int index)
{
    AvatarSlot& slot = g_avatars[index];
    void* texture = GetTexture(AvatarSlotTex(index));
    if (!texture) return false;
    // Only ever replace what we expect: a loaded 64x64 icon.
    void* current = Field<void*>(texture, RTEXTURE_GPU_TEXTURE);
    if (!current || Field<uint32_t>(texture, RTEXTURE_WIDTH) != kAvatarSize ||
        Field<uint32_t>(texture, RTEXTURE_HEIGHT) != kAvatarSize)
        return false;

    static uint8_t rgba[kAvatarRgbaBytes];
    static uint8_t encoded[kAvatarTextureBytes];
    if (!SteamLobby_GetAvatar64(slot.steamId, rgba)) return false;
    EncodeAvatarTexture(rgba, encoded);

    using Allocate = void*(__fastcall*)(size_t, uint32_t);
    using Construct = void*(__fastcall*)(void*, uint16_t, uint32_t, uint8_t, uint32_t, uint32_t,
                                         int32_t, uint32_t, void*, uint32_t, void*);
    void* memory = Native<Allocate>(NDRAW_TEXTURE_ALLOC)(NDRAW_TEXTURE_SIZE, 0x10);
    if (!memory) return false;
    // Arguments as rTexture__load passes them for a plain 2D texture.
    void* created = Native<Construct>(NDRAW_TEXTURE_CTOR)(
        memory, kAvatarSize, kAvatarSize, 1, 1, kTexFormatIcon, 0, 0, nullptr, 0, encoded);
    if (!created) return false;
    slot.original = current;
    slot.ours = created;
    Field<void*>(texture, RTEXTURE_GPU_TEXTURE) = created;
    if (!g_avatarLogged) {
        g_avatarLogged = true;
        HookLog("LobbyNative: Steam profile pictures are drawing (first one in slot %d)\n", index);
    }
    return true;
}

void HideSchedulerUnits()
{
    if (!g_scheduler) return;
    using GetUnit = void*(__fastcall*)(void*, uint32_t);
    const uint32_t count = Field<uint32_t>(g_scheduler, USCHEDULER_UNIT_TRACK_NUM);
    for (uint32_t i = 0; i < count && i < 512; ++i) {
        if (void* unit = Native<GetUnit>(USCHEDULER_GET_UNIT)(g_scheduler, i))
            SetDraw(unit, false);
    }
}

void* FindPrototype(uintptr_t dti, const char* name)
{
    using GetUnitIndex = uint32_t(__fastcall*)(void*, void*, const char*);
    using GetUnit = void*(__fastcall*)(void*, uint32_t);
    const uint32_t index = Native<GetUnitIndex>(USCHEDULER_GET_UNIT_INDEX)(
        g_scheduler, reinterpret_cast<void*>(_addr(dti)), name);
    if (index == UINT32_MAX) return nullptr;
    void* unit = Native<GetUnit>(USCHEDULER_GET_UNIT)(g_scheduler, index);
    return IsUnitAlive(unit) ? unit : nullptr;
}

void ResetState()
{
    for (Pool& pool : g_pools) {
        pool.prototype = nullptr;
        pool.vtable = 0;
        pool.units.clear();
        pool.used = 0;
        pool.usedLastFrame = 0;
    }
    g_scheduler = nullptr;
    memset(g_textures, 0, sizeof(g_textures));
    memset(g_textureRetryFrame, 0, sizeof(g_textureRetryFrame));
    g_stage = Stage::Idle;
    g_stageFrames = 0;
}

// The engine can destroy units behind our back (sUnit::killAll on an area
// change). A clone whose vtable or state no longer matches means every cached
// pointer is stale.
bool PoolsLookValid()
{
    if (!IsUnitAlive(g_scheduler)) return false;
    for (const Pool& pool : g_pools) {
        if (!IsUnitAlive(pool.prototype)) return false;
        if (pool.units.empty()) continue;
        void* first = pool.units.front();
        void* last = pool.units.back();
        if (*reinterpret_cast<uintptr_t*>(first) != pool.vtable || !IsUnitAlive(first) ||
            *reinterpret_cast<uintptr_t*>(last) != pool.vtable || !IsUnitAlive(last))
            return false;
    }
    return true;
}

void ApplyPlacement(void* unit, float x, float y, float scaleX, float scaleY, Anchor anchor,
                    Color color)
{
    Field<float>(unit, UCOORD_POS) = x;
    Field<float>(unit, UCOORD_POS + 4) = y;
    Field<float>(unit, UCOORD_POS + 8) = 0.0f;
    Field<float>(unit, UCOORD_SCALE) = scaleX;
    Field<float>(unit, UCOORD_SCALE + 4) = scaleY;
    Field<float>(unit, UCOORD_SCALE + 8) = 1.0f;
    Field<uint32_t>(unit, SPRITE_CENTER_ATTR) = anchor;
    Field<uint32_t>(unit, SPRITE_COLOR) = PackColor(color);
    Field<int32_t>(unit, SPRITE_DEPTH) = g_nextDepth;
    g_nextDepth -= kDepthStep;
    // Size, UV, colour and anchor feed geometry that the unit only rebuilds
    // when flagged (uBaseSprite::move / uUiAnmSpriteFrame::move).
    Field<uint8_t>(unit, SPRITE_GEOMETRY_DIRTY) = 1;
    SetDraw(unit, true);
}

void ApplyTextured(void* unit, Tex tex, void* texture, float w, float h, Uv uv)
{
    if (Field<void*>(unit, SPRITE_TEXTURE) != texture) {
        using SetTexture = void(__fastcall*)(void*, void*);
        Native<SetTexture>(UBASESPRITE_SET_TEXTURE)(unit, texture);
    }
    (void)tex;
    Field<float>(unit, SPRITE_SIZE) = w;
    Field<float>(unit, SPRITE_SIZE + 4) = h;
    Field<float>(unit, SPRITE_UV0) = uv.u0;
    Field<float>(unit, SPRITE_UV0 + 4) = uv.v0;
    Field<float>(unit, SPRITE_UV1) = uv.u1;
    Field<float>(unit, SPRITE_UV1 + 4) = uv.v1;
}

class NativeCanvas final : public Canvas {
public:
    void Sprite(Tex tex, float x, float y, float w, float h, Uv uv, Anchor anchor,
                Color color) override
    {
        if (color.a <= 0.004f || w <= 0.0f || h <= 0.0f) return;
        void* texture = GetTexture(tex);
        if (!texture) return;
        void* unit = AcquireUnit(KIND_SPRITE);
        if (!unit) return;
        ApplyTextured(unit, tex, texture, w, h, uv);
        ApplyPlacement(unit, x, y, 1.0f, 1.0f, anchor, color);
    }

    void Frame(Tex tex, float x, float y, float w, float h, Uv uv, Anchor anchor,
               Color color) override
    {
        if (color.a <= 0.004f) return;
        void* texture = GetTexture(tex);
        if (!texture) return;
        void* unit = AcquireUnit(KIND_FRAME);
        if (!unit) return;
        ApplyTextured(unit, tex, texture, w, h, uv);
        Field<uint32_t>(unit, FRAME_ATTR) = GetFrameStyle(tex).tileCenter ? 1u : 0u;
        ApplyPlacement(unit, x, y, 1.0f, 1.0f, anchor, color);
    }

    void Text(const char* utf8, float x, float y, float scale, Anchor anchor, Color color,
              Font font, float stretch, Blend blend) override
    {
        if (!utf8 || !utf8[0] || color.a <= 0.004f) return;
        void* unit = AcquireUnit(KIND_FONT);
        if (!unit) return;
        if (Field<int32_t>(unit, FONT_TYPE) != static_cast<int32_t>(font)) {
            using SetFontType = void(__fastcall*)(void*, int32_t);
            Native<SetFontType>(UUIANMSPRITEFONT_SET_FONT_TYPE)(unit, static_cast<int32_t>(font));
        }
        // The native setter ignores an unchanged string, so this is cheap.
        using SetText = void(__fastcall*)(void*, const char*);
        Native<SetText>(UUIANMSPRITEFONT_SET_TEXT)(unit, utf8);
        // Alignment is baked into the glyph quads when text is laid out, and a
        // pooled unit can keep its string while its anchor changes. Marking
        // the cached message id stale is how setText itself forces a relayout
        // (uUiAnmSpriteFont__isTextGeometryDirty compares the two).
        if (Field<uint32_t>(unit, SPRITE_CENTER_ATTR) != static_cast<uint32_t>(anchor))
            Field<int32_t>(unit, FONT_CACHED_MSG_ID) = Field<int32_t>(unit, FONT_MSG_INDEX) - 1;
        // Two engine quirks, both read from rebuildTextQuadGeometry
        // (0x1402CB440) and measureResolvedText (0x1402CF3B0):
        //  - glyph quads are laid out at cell * mScale and then transformed
        //    by the world matrix, which carries mScale again, so on-screen
        //    size goes with mScale squared;
        //  - the measured height only counts line breaks, so a single line is
        //    always placed with its top at the anchor whatever the row.
        // The canvas contract is linear scale and a true vertical anchor.
        const float cell = font == FONT_HEADING ? 64.0f : 32.0f;
        const float scaleY = scale * stretch;
        const float unitScaleX = sqrtf(scale > 0.0f ? scale : 0.0f);
        const float unitScaleY = sqrtf(scaleY > 0.0f ? scaleY : 0.0f);
        const float top = y - static_cast<float>(anchor / 3) * 0.5f * cell * scaleY;
        // Pooled units are reused, so the blend state is set on every draw.
        Field<int32_t>(unit, SPRITE_BLEND_STATE) = blend;
        ApplyPlacement(unit, x, top, unitScaleX, unitScaleY, anchor, color);
    }
};

NativeCanvas g_canvas;

// ---------------------------------------------------------------------------
// Input
// ---------------------------------------------------------------------------
constexpr int kRepeatDelay = 18;
constexpr int kRepeatInterval = 5;

uint32_t g_previousHeld = 0;
bool g_inputPrimed = false;
uint32_t g_lastInputFrame = 0;
int g_repeatFrames[4] = {};
bool g_usingKeyboard = false;
bool g_previousKeys[256] = {};
bool g_textKeysPrimed = false; // g_previousKeys holds the keys down when typing began
int g_backspaceFrames = 0;
char g_typed[64] = {};

// Characters posted by the window procedure (single producer, single consumer).
std::atomic<uint32_t> g_charHead{0}, g_charTail{0};
uint16_t g_charQueue[64] = {};

bool IsGameForeground()
{
    HWND window = GetForegroundWindow();
    if (!window) return false;
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    return process == GetCurrentProcessId();
}

bool IsKeyHeld(int vk)
{
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

void AppendUtf8(char* out, size_t capacity, uint32_t codepoint)
{
    size_t used = strlen(out);
    char encoded[5] = {};
    if (codepoint < 0x80) {
        encoded[0] = static_cast<char>(codepoint);
    } else if (codepoint < 0x800) {
        encoded[0] = static_cast<char>(0xC0 | (codepoint >> 6));
        encoded[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else if (codepoint < 0x10000) {
        encoded[0] = static_cast<char>(0xE0 | (codepoint >> 12));
        encoded[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        encoded[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
    } else {
        encoded[0] = static_cast<char>(0xF0 | (codepoint >> 18));
        encoded[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
        encoded[2] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
        encoded[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
    }
    const size_t length = strlen(encoded);
    if (used + length + 1 > capacity) return;
    memcpy(out + used, encoded, length + 1);
}

// ---------------------------------------------------------------------------
// XInput fallback
// ---------------------------------------------------------------------------
// The lobby normally reads the masks sPad has already decoded. If no native
// slot reports anything (in game the controller did nothing in the lobby while
// the keyboard worked, cause not established), controllers are read straight
// from XInput and translated to the same internal mask.
struct XInputPadState {
    DWORD packet;
    WORD buttons;
    BYTE leftTrigger, rightTrigger;
    SHORT lx, ly, rx, ry;
};
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XInputPadState*);
XInputGetStateFn g_xinputGetState = nullptr;
bool g_xinputLoadTried = false;
uint32_t g_xinputNextProbe[4] = {};
bool g_xinputLogged = false;
constexpr uint32_t kXInputReprobeFrames = 120; // absent users are slow to query
constexpr SHORT kStickThreshold = 16000;

uint32_t ReadXInputMask()
{
    if (!g_xinputLoadTried) {
        g_xinputLoadTried = true;
        for (const char* name : {"XINPUT1_3.dll", "XINPUT1_4.dll", "XINPUT9_1_0.dll"}) {
            if (HMODULE module = LoadLibraryA(name)) {
                g_xinputGetState = reinterpret_cast<XInputGetStateFn>(
                    reinterpret_cast<void*>(GetProcAddress(module, "XInputGetState")));
                if (g_xinputGetState) break;
            }
        }
        if (!g_xinputGetState) HookLog("LobbyNative: XInput is not available for the fallback\n");
    }
    if (!g_xinputGetState) return 0;

    uint32_t mask = 0;
    for (DWORD user = 0; user < 4; ++user) {
        if (g_frame < g_xinputNextProbe[user]) continue;
        XInputPadState state = {};
        if (g_xinputGetState(user, &state) != ERROR_SUCCESS) {
            g_xinputNextProbe[user] = g_frame + kXInputReprobeFrames;
            continue;
        }
        const WORD b = state.buttons;
        if (b & 0x0001) mask |= 0x10;       // d-pad up
        if (b & 0x0002) mask |= 0x40;       // d-pad down
        if (b & 0x0004) mask |= 0x80;       // d-pad left
        if (b & 0x0008) mask |= 0x20;       // d-pad right
        if (b & 0x0010) mask |= 0x8;        // start
        if (b & 0x0020) mask |= 0x1;        // back
        if (b & 0x0100) mask |= 0x100;      // left shoulder
        if (b & 0x0200) mask |= 0x200;      // right shoulder
        if (b & 0x1000) mask |= 0x4000;     // A
        if (b & 0x2000) mask |= 0x2000;     // B
        if (b & 0x4000) mask |= 0x8000;     // X
        if (b & 0x8000) mask |= 0x1000;     // Y
        if (state.ly > kStickThreshold) mask |= 0x10000;
        if (state.lx > kStickThreshold) mask |= 0x20000;
        if (state.ly < -kStickThreshold) mask |= 0x40000;
        if (state.lx < -kStickThreshold) mask |= 0x80000;
    }
    return mask;
}

void LogPadState(void* pad, const char* why)
{
    HookLog("LobbyNative: pad state (%s): sPad.mActive=%u keyboardBlock=%u/%u source=%u\n", why,
            Field<uint8_t>(pad, 0x38), Field<uint8_t>(pad, 0xC20), Field<uint8_t>(pad, 0xC21),
            Field<uint8_t>(pad, 0xC30));
    for (int slot = 0; slot < 4; ++slot) {
        uint8_t* slotObject = static_cast<uint8_t*>(pad) + SPAD_SLOT_BASE + slot * SPAD_SLOT_STRIDE;
        HookLog("LobbyNative:   slot %d active=%u ready=%u kind=%u device=%u buttons=%08x\n", slot,
                slotObject[0x08], slotObject[0x09], slotObject[0x0B],
                *reinterpret_cast<uint32_t*>(slotObject + 0x18),
                *reinterpret_cast<uint32_t*>(slotObject + SPAD_SLOT_BUTTONS));
    }
}

} // namespace

// sPad only polls and re-detects a slot whose active byte is set, and that
// byte is only ever set by sPad__redetectSlot (0x140514950). In game every
// slot was found inactive in the title area (the startup detection had not
// taken), so no controller reached the lobby or the main menu. Ask for
// detection the way sPad::Pad::detect does (flags 0x18: XInput, then
// DirectInput) until each slot is active; from then on the engine's own poll
// keeps re-detecting it. The call is rate limited to once a second per slot.
void Native_EnsurePadSlotsActive()
{
    using GetInstance = void*(__fastcall*)();
    void* pad = Native<GetInstance>(SPAD_GET_INSTANCE)();
    if (!pad) return;
    using Redetect = uint32_t(__fastcall*)(void*, uint32_t, uint32_t);
    static bool logged[4] = {};
    for (uint32_t slot = 0; slot < 4; ++slot) {
        uint8_t* slotObject = static_cast<uint8_t*>(pad) + SPAD_SLOT_BASE + slot * SPAD_SLOT_STRIDE;
        if (slotObject[SPAD_SLOT_ACTIVE]) continue;
        Native<Redetect>(SPAD_REDETECT_SLOT)(pad, slot, 0x18);
        if (slotObject[SPAD_SLOT_ACTIVE] && !logged[slot]) {
            logged[slot] = true;
            HookLog("LobbyNative: pad slot %u activated (kind=%u)\n", slot, slotObject[SPAD_SLOT_KIND]);
        }
    }
}

namespace {

// Menu keys follow the game's own fixed keyboard mapping
// (sPad__accumulateUnitFlagsFromQuery): arrows, Enter/A, Backspace/B, X, Y,
// Esc as Start.
struct KeyState {
    bool up, down, left, right, confirm, cancel, x, y, menu, quickChat;
};

KeyState ReadKeyboard(bool textActive)
{
    KeyState keys = {};
    keys.up = IsKeyHeld(VK_UP);
    keys.down = IsKeyHeld(VK_DOWN);
    keys.left = IsKeyHeld(VK_LEFT);
    keys.right = IsKeyHeld(VK_RIGHT);
    keys.confirm = IsKeyHeld(VK_RETURN);
    keys.menu = !textActive && IsKeyHeld(VK_ESCAPE);
    if (textActive) {
        // While typing, letters and Backspace edit the text; Esc cancels.
        keys.cancel = IsKeyHeld(VK_ESCAPE);
    } else {
        keys.confirm |= IsKeyHeld('A');
        keys.cancel = IsKeyHeld(VK_BACK) || IsKeyHeld('B');
        keys.x = IsKeyHeld('X');
        keys.y = IsKeyHeld('Y');
        // X is also the quick chat key; in a room that is all it does.
        keys.quickChat = keys.x;
    }
    return keys;
}

void CollectTypedText(bool textActive, bool foreground)
{
    g_typed[0] = '\0';
    // Drain window-procedure characters first (IME and dead keys arrive here).
    uint32_t tail = g_charTail.load(std::memory_order_relaxed);
    const uint32_t head = g_charHead.load(std::memory_order_acquire);
    uint32_t pendingHigh = 0;
    for (; tail != head; ++tail) {
        const uint32_t unit = g_charQueue[tail % 64];
        if (!textActive) continue;
        if (unit >= 0xD800 && unit <= 0xDBFF) {
            pendingHigh = unit;
        } else if (unit >= 0xDC00 && unit <= 0xDFFF) {
            if (pendingHigh)
                AppendUtf8(g_typed, sizeof(g_typed),
                           0x10000 + ((pendingHigh - 0xD800) << 10) + (unit - 0xDC00));
            pendingHigh = 0;
        } else if (unit >= 0x20 && unit != 0x7F) {
            AppendUtf8(g_typed, sizeof(g_typed), unit);
        }
    }
    g_charTail.store(tail, std::memory_order_release);

    if (!textActive || !foreground) {
        memset(g_previousKeys, 0, sizeof(g_previousKeys));
        g_textKeysPrimed = false;
        return;
    }
    if (!g_textKeysPrimed) {
        // Keys already down when typing starts are not text: Y opens the
        // chat on the keyboard and must not also type a "y".
        g_textKeysPrimed = true;
        for (int vk = 0; vk < 256; ++vk) g_previousKeys[vk] = IsKeyHeld(vk);
        g_typed[0] = '\0';
        return;
    }
    if (g_typed[0]) {
        // The window delivers WM_CHAR itself; do not also synthesise text.
        for (int vk = 0; vk < 256; ++vk) g_previousKeys[vk] = IsKeyHeld(vk);
        return;
    }

    // The game's message loop does not translate keystrokes, so synthesise
    // characters from key edges with the current layout.
    BYTE state[256] = {};
    if (IsKeyHeld(VK_SHIFT)) state[VK_SHIFT] = 0x80;
    if (IsKeyHeld(VK_CONTROL)) state[VK_CONTROL] = 0x80;
    if (IsKeyHeld(VK_MENU)) state[VK_MENU] = 0x80;
    if (GetKeyState(VK_CAPITAL) & 1) state[VK_CAPITAL] = 1;
    for (int vk = 0x20; vk < 0xFF; ++vk) {
        const bool down = IsKeyHeld(vk);
        const bool pressed = down && !g_previousKeys[vk];
        g_previousKeys[vk] = down;
        if (!pressed || vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU ||
            (vk >= VK_LSHIFT && vk <= VK_RMENU) || (vk >= VK_PRIOR && vk <= VK_DOWN) ||
            (vk >= VK_F1 && vk <= VK_F24))
            continue;
        if (state[VK_CONTROL] & 0x80) continue;
        WCHAR buffer[4] = {};
        state[vk] = 0x80;
        const int produced = ToUnicode(static_cast<UINT>(vk), MapVirtualKeyW(vk, MAPVK_VK_TO_VSC),
                                       state, buffer, 4, 0);
        state[vk] = 0;
        for (int i = 0; i < produced && i < 4; ++i)
            if (buffer[i] >= 0x20 && buffer[i] != 0x7F)
                AppendUtf8(g_typed, sizeof(g_typed), buffer[i]);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Frame lifecycle
// ---------------------------------------------------------------------------
bool Native_IsReady()
{
    return g_stage == Stage::Ready;
}

Canvas* Native_BeginFrame()
{
    ++g_frame;
    ++g_stageFrames;

    using GetInstance = void*(__fastcall*)();
    void* gameUi = Native<GetInstance>(SGAMEUI_GET_INSTANCE)();
    if (!gameUi) return nullptr;

    if (g_stage == Stage::Idle) {
        using LoadArchive = void(__fastcall*)(void*);
        Native<LoadArchive>(SGAMEUI_LOAD_NETROOM_ARCHIVE)(gameUi);
        g_stage = Stage::WaitArchive;
        g_stageFrames = 0;
        HookLog("LobbyNative: requested ui\\netroom archive\n");
        return nullptr;
    }

    if (g_stage == Stage::WaitArchive) {
        using ArchiveStatus = int32_t(__fastcall*)(void*);
        const int32_t status = Native<ArchiveStatus>(SGAMEUI_NETROOM_ARCHIVE_STATUS)(gameUi);
        if (status == -1) {
            // The title area released the handle; ask again.
            using LoadArchive = void(__fastcall*)(void*);
            Native<LoadArchive>(SGAMEUI_LOAD_NETROOM_ARCHIVE)(gameUi);
            return nullptr;
        }
        if (status != 0) {
            if (g_stageFrames == 600)
                HookLog("LobbyNative: still waiting for ui\\netroom after 600 frames\n");
            return nullptr;
        }
        using LoadScheduler = bool(__fastcall*)(void*, const char*, void**, uint32_t);
        g_scheduler = nullptr;
        if (!Native<LoadScheduler>(UI_LOAD_SCHEDULER)(nullptr, kPrototypeScheduler, &g_scheduler, 0) ||
            !g_scheduler) {
            if (g_stageFrames % 120 == 1)
                HookLog("LobbyNative: could not create prototype scheduler %s\n", kPrototypeScheduler);
            g_scheduler = nullptr;
            return nullptr;
        }
        g_stage = Stage::WaitPrototypes;
        g_stageFrames = 0;
        HookLog("LobbyNative: prototype scheduler created at %p\n", g_scheduler);
        return nullptr;
    }

    if (g_stage == Stage::WaitPrototypes) {
        if (!IsUnitAlive(g_scheduler)) {
            HookLog("LobbyNative: prototype scheduler died before its units were ready\n");
            ResetState();
            return nullptr;
        }
        // The scheduler spawns its units over the next frames; never show them.
        HideSchedulerUnits();
        g_pools[KIND_SPRITE].prototype = FindPrototype(UUIANMSPRITE_DTI, kSpritePrototype);
        g_pools[KIND_FRAME].prototype = FindPrototype(UUIANMSPRITEFRAME_DTI, kFramePrototype);
        g_pools[KIND_FONT].prototype = FindPrototype(UUIANMSPRITEFONT_DTI, kFontPrototype);
        if (!g_pools[KIND_SPRITE].prototype || !g_pools[KIND_FRAME].prototype ||
            !g_pools[KIND_FONT].prototype) {
            if (g_stageFrames == 300)
                HookLog("LobbyNative: prototypes missing after 300 frames sprite=%p frame=%p font=%p\n",
                        g_pools[KIND_SPRITE].prototype, g_pools[KIND_FRAME].prototype,
                        g_pools[KIND_FONT].prototype);
            return nullptr;
        }
        // Freeze the layout's own timeline; only the clones are ever drawn.
        using SetPause = void(__fastcall*)(void*, char);
        Native<SetPause>(USCHEDULER_SET_PAUSE)(g_scheduler, 1);
        g_stage = Stage::Ready;
        HookLog("LobbyNative: ready sprite=%p frame=%p font=%p\n",
                g_pools[KIND_SPRITE].prototype, g_pools[KIND_FRAME].prototype,
                g_pools[KIND_FONT].prototype);
    }

    if (!PoolsLookValid()) {
        HookLog("LobbyNative: native units were destroyed externally; rebuilding\n");
        ResetState();
        return nullptr;
    }

    HideSchedulerUnits();
    for (Pool& pool : g_pools) pool.used = 0;
    g_nextDepth = kDepthFar;
    return &g_canvas;
}

void Native_EndFrame()
{
    if (g_stage != Stage::Ready) return;
    for (Pool& pool : g_pools) {
        for (size_t i = pool.used; i < pool.usedLastFrame && i < pool.units.size(); ++i)
            SetDraw(pool.units[i], false);
        pool.usedLastFrame = pool.used;
    }
}

void Native_Shutdown()
{
    if (g_stage == Stage::Idle && !g_scheduler) return;
    size_t killed = 0;
    if (g_stage == Stage::Ready && PoolsLookValid()) {
        for (Pool& pool : g_pools) {
            for (void* unit : pool.units) {
                if (*reinterpret_cast<uintptr_t*>(unit) != pool.vtable) continue;
                KillUnit(unit);
                ++killed;
            }
        }
    }
    if (IsUnitAlive(g_scheduler)) KillUnit(g_scheduler);

    ReleaseAllAvatars(); // while the icon textures are still referenced
    using Release = void(__fastcall*)(void*);
    for (void*& texture : g_textures) {
        if (texture) Native<Release>(CRESOURCE_RELEASE_REF)(texture);
        texture = nullptr;
    }
    ReleaseBannerArchives();
    HookLog("LobbyNative: shut down (%u clones killed)\n", static_cast<unsigned>(killed));
    ResetState();
}

void Native_Invalidate()
{
    // Texture references are still ours to release; unit memory is not.
    ReleaseAllAvatars();
    using Release = void(__fastcall*)(void*);
    for (void*& texture : g_textures) {
        if (texture) Native<Release>(CRESOURCE_RELEASE_REF)(texture);
        texture = nullptr;
    }
    ReleaseBannerArchives();
    ResetState();
}

// The texture showing a Steam user's profile picture, or the default player
// icon while there is none (not cached by Steam yet, every slot busy, no
// Steam). Pictures are fetched once and kept while they are being drawn.
Tex Native_AvatarTex(uint64_t steamId)
{
    if (!steamId) return TEX_USER_ICON;
    int found = -1, spare = -1;
    for (int i = 0; i < kAvatarSlots; ++i) {
        if (g_avatars[i].steamId == steamId) found = i;
    }
    if (found < 0) {
        // A free slot, else the one that has gone longest without being drawn.
        for (int i = 0; i < kAvatarSlots; ++i) {
            const AvatarSlot& slot = g_avatars[i];
            if (slot.steamId && g_frame - slot.lastUsed < kAvatarKeepFrames) continue;
            if (spare < 0 || !slot.steamId ||
                (g_avatars[spare].steamId && slot.lastUsed < g_avatars[spare].lastUsed))
                spare = i;
            if (!slot.steamId) break;
        }
        if (spare < 0) return TEX_USER_ICON;
        ReleaseAvatar(spare);
        g_avatars[spare].steamId = steamId;
        found = spare;
    }
    AvatarSlot& slot = g_avatars[found];
    slot.lastUsed = g_frame;
    if (!slot.ours && g_frame >= slot.nextTry && !UploadAvatar(found))
        slot.nextTry = g_frame + kAvatarRetryFrames;
    return slot.ours ? AvatarSlotTex(found) : TEX_USER_ICON;
}

// ---------------------------------------------------------------------------
// Input and sound
// ---------------------------------------------------------------------------
void Native_OnChar(uint32_t utf16)
{
    const uint32_t head = g_charHead.load(std::memory_order_relaxed);
    if (head - g_charTail.load(std::memory_order_acquire) >= 64) return;
    g_charQueue[head % 64] = static_cast<uint16_t>(utf16);
    g_charHead.store(head + 1, std::memory_order_release);
}

void Native_ReadInput(Input& input, bool textActive)
{
    input = {};
    const bool foreground = IsGameForeground();
    CollectTypedText(textActive, foreground);
    input.typed = g_typed;

    // Pads: OR the decoded button masks of every active hardware slot.
    using GetInstance = void*(__fastcall*)();
    using GetMask = uint32_t(__fastcall*)(void*);
    void* pad = Native<GetInstance>(SPAD_GET_INSTANCE)();
    uint32_t padHeld = 0;
    bool nativeDevice = false; // sPad has a controller open in some slot
    uint32_t decideMask = 0x4000, cancelMask = 0x2000;
    if (pad) {
        for (int slot = 0; slot < 4; ++slot) {
            uint8_t* slotObject = static_cast<uint8_t*>(pad) + SPAD_SLOT_BASE + slot * SPAD_SLOT_STRIDE;
            if (slotObject[SPAD_SLOT_ACTIVE])
                padHeld |= *reinterpret_cast<uint32_t*>(slotObject + SPAD_SLOT_BUTTONS);
            if (slotObject[SPAD_SLOT_ACTIVE] && slotObject[SPAD_SLOT_KIND]) nativeDevice = true;
        }
        decideMask = Native<GetMask>(SPAD_DECIDE_MASK)(pad);
        cancelMask = Native<GetMask>(SPAD_CANCEL_MASK)(pad);
    }
    // One snapshot of what sPad sees, a couple of seconds in, for the log.
    static bool statLogged = false;
    if (pad && !statLogged && g_frame > 120) {
        statLogged = true;
        LogPadState(pad, "lobby");
    }
    if (!nativeDevice) {
        const uint32_t direct = ReadXInputMask();
        if (direct && !g_xinputLogged) {
            g_xinputLogged = true;
            HookLog("LobbyNative: controller input is coming from the XInput fallback (%08x)\n",
                    direct);
            if (pad) LogPadState(pad, "fallback in use");
        }
        padHeld = direct;
    }
    if (!foreground) padHeld = 0;

    const KeyState keys = foreground ? ReadKeyboard(textActive) : KeyState{};

    // Logical held bits.
    enum : uint32_t {
        H_UP = 1, H_DOWN = 2, H_LEFT = 4, H_RIGHT = 8, H_CONFIRM = 16, H_CANCEL = 32,
        H_X = 64, H_Y = 128, H_MENU = 256, H_LB = 512, H_RB = 1024, H_QUICK_CHAT = 2048,
    };
    uint32_t fromPad = 0, fromKeys = 0;
    if (padHeld & PAD_UP) fromPad |= H_UP;
    if (padHeld & PAD_DOWN) fromPad |= H_DOWN;
    if (padHeld & PAD_LEFT) fromPad |= H_LEFT;
    if (padHeld & PAD_RIGHT) fromPad |= H_RIGHT;
    if (padHeld & decideMask) fromPad |= H_CONFIRM;
    if (padHeld & cancelMask) fromPad |= H_CANCEL;
    if (padHeld & PAD_X) fromPad |= H_X;
    if (padHeld & PAD_Y) fromPad |= H_Y;
    if (padHeld & PAD_START) fromPad |= H_MENU;
    if (padHeld & PAD_LB) fromPad |= H_LB;
    if (padHeld & PAD_RB) fromPad |= H_RB | H_QUICK_CHAT;
    if (keys.up) fromKeys |= H_UP;
    if (keys.down) fromKeys |= H_DOWN;
    if (keys.left) fromKeys |= H_LEFT;
    if (keys.right) fromKeys |= H_RIGHT;
    if (keys.confirm) fromKeys |= H_CONFIRM;
    if (keys.cancel) fromKeys |= H_CANCEL;
    if (keys.x) fromKeys |= H_X;
    if (keys.y) fromKeys |= H_Y;
    if (keys.menu) fromKeys |= H_MENU;
    if (keys.quickChat) fromKeys |= H_QUICK_CHAT;

    const uint32_t held = fromPad | fromKeys;
    // A button already down when the lobby (re)appears is not a press: the
    // Enter that launched the game must not pick the first menu entry.
    if (!g_inputPrimed || g_frame - g_lastInputFrame > 30) {
        g_inputPrimed = true;
        g_previousHeld = held;
        for (int& frames : g_repeatFrames) frames = 1;
    }
    g_lastInputFrame = g_frame;
    const uint32_t pressed = held & ~g_previousHeld;
    if (pressed & fromKeys) g_usingKeyboard = true;
    else if (pressed & fromPad) g_usingKeyboard = false;
    g_previousHeld = held;

    // Directions repeat after a delay, like uUiCursor's navigation input.
    const uint32_t directions[4] = {H_UP, H_DOWN, H_LEFT, H_RIGHT};
    bool fire[4] = {};
    for (int i = 0; i < 4; ++i) {
        if (!(held & directions[i])) {
            g_repeatFrames[i] = 0;
            continue;
        }
        const int frames = g_repeatFrames[i]++;
        fire[i] = frames == 0 ||
                  (frames >= kRepeatDelay && (frames - kRepeatDelay) % kRepeatInterval == 0);
    }
    input.up = fire[0];
    input.down = fire[1];
    input.left = fire[2];
    input.right = fire[3];
    input.confirm = (pressed & H_CONFIRM) != 0;
    input.cancel = (pressed & H_CANCEL) != 0;
    input.actionX = (pressed & H_X) != 0;
    input.actionY = (pressed & H_Y) != 0;
    input.menu = (pressed & H_MENU) != 0;
    input.pageLeft = (pressed & H_LB) != 0;
    input.pageRight = (pressed & H_RB) != 0;
    input.quickChat = (pressed & H_QUICK_CHAT) != 0;
    input.usingKeyboard = g_usingKeyboard;

    if (textActive && foreground && IsKeyHeld(VK_BACK)) {
        const int frames = g_backspaceFrames++;
        input.backspace = frames == 0 || (frames >= kRepeatDelay && (frames - kRepeatDelay) % 3 == 0);
    } else {
        g_backspaceFrames = 0;
    }
}

void Native_FocusGameWindow()
{
    struct Search {
        DWORD process;
        HWND window;
    } search = {GetCurrentProcessId(), nullptr};
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            Search& found = *reinterpret_cast<Search*>(param);
            DWORD process = 0;
            GetWindowThreadProcessId(window, &process);
            if (process != found.process || !IsWindowVisible(window) || GetWindow(window, GW_OWNER))
                return TRUE;
            found.window = window;
            return FALSE;
        },
        reinterpret_cast<LPARAM>(&search));
    if (!search.window || GetForegroundWindow() == search.window) return;
    if (IsIconic(search.window)) ShowWindow(search.window, SW_RESTORE);
    // Windows only lets the foreground thread hand the foreground over, so
    // borrow its input queue for the call; flash the taskbar button as well
    // in case it still says no.
    const DWORD self = GetCurrentThreadId();
    const DWORD front = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
    if (front && front != self) AttachThreadInput(front, self, TRUE);
    BringWindowToTop(search.window);
    SetForegroundWindow(search.window);
    if (front && front != self) AttachThreadInput(front, self, FALSE);
    if (GetForegroundWindow() != search.window) {
        FLASHWINFO flash = {sizeof(flash), search.window, FLASHW_ALL | FLASHW_TIMERNOFG, 0, 0};
        FlashWindowEx(&flash);
    }
}

void Native_PlaySfx(uint32_t sfxMask)
{
    using PlayVoid = void(__fastcall*)();
    if (sfxMask & SFX_MATCH_FOUND) {
        using GetSound = void*(__fastcall*)();
        using ResolveRef = void*(__fastcall*)(void*, void*, uint32_t);
        using PlayVoice = void*(__fastcall*)(void*, void*, void*, uint32_t, uint32_t, float, uint32_t);
        bool played = false;
        if (void* sound = Native<GetSound>(GSOUND_INSTANCE)()) {
            alignas(16) uint8_t ref[32] = {};
            alignas(16) uint8_t handle[32] = {};
            void* bank = Native<ResolveRef>(GSOUND_RESOLVE_REF)(sound, ref, kStageSelectBank);
            Native<PlayVoice>(GSOUND_PLAY_ORDINARY_VOICE)(sound, handle, bank, kStageSelectVoice, 0,
                                                         1.0f, 0);
            played = *reinterpret_cast<uint64_t*>(handle + 8) != UINT64_MAX;
        }
        // That bank belongs to character select; if it is not loaded here the
        // prompt still makes itself heard.
        if (!played) Native<PlayVoid>(MENU_SE_DECIDE)();
        HookLog("LobbyNative: match-found cue (stage select voice %s)\n", played ? "played" : "unavailable");
        return;
    }
    if (sfxMask & SFX_MENU_ENTER) {
        Native<PlayVoid>(MAINMENU_SE_DECIDE)();
    } else if (sfxMask & SFX_MENU_EXIT) {
        using GetSound = void*(__fastcall*)();
        using ResolveRef = void*(__fastcall*)(void*, void*, uint32_t);
        using PlayVoice = void*(__fastcall*)(void*, void*, void*, uint32_t, uint32_t, float, uint32_t);
        if (void* sound = Native<GetSound>(GSOUND_INSTANCE)()) {
            alignas(16) uint8_t ref[32] = {};
            alignas(16) uint8_t handle[32] = {};
            void* bank = Native<ResolveRef>(GSOUND_RESOLVE_REF)(sound, ref, kMenuExitBank);
            Native<PlayVoice>(GSOUND_PLAY_ORDINARY_VOICE)(sound, handle, bank, kMenuExitVoice, 0,
                                                         1.0f, 0);
        }
        Native<PlayVoid>(MAINMENU_SE_EXIT)();
    } else if (sfxMask & SFX_CONFIRM) {
        Native<PlayVoid>(MENU_SE_DECIDE)();
    } else if (sfxMask & SFX_CANCEL) {
        Native<PlayVoid>(MENU_SE_CANCEL)();
    } else if (sfxMask & SFX_MOVE) {
        // The cursor cue is a method that compares two cursor fields; hand it
        // a scratch object whose fields differ.
        alignas(8) uint8_t scratch[0x90] = {};
        *reinterpret_cast<int32_t*>(scratch + 0x88) = 1;
        using PlayCursor = void(__fastcall*)(void*);
        Native<PlayCursor>(MENU_SE_CURSOR)(scratch);
    }
}

} // namespace lobby
