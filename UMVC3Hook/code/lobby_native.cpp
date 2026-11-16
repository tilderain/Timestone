// Native-engine backend for the lobby UI. Addresses are preferred-image VAs
// from umvc3.exe (unpacked); doc/native_lobby_ui.md records how each one was
// identified.
#include "../pch.h"
#include "lobby_native.h"
#include "lobby_ui.h"
#include "mk10utils.h"

#include <Windows.h>
#include <atomic>
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
constexpr uintptr_t CRESOURCE_RELEASE_REF = 0x14050D5A0;
constexpr uintptr_t SGAMEUI_GET_INSTANCE = 0x140001AC0;
constexpr uintptr_t SGAMEUI_LOAD_NETROOM_ARCHIVE = 0x1402BDE30;
constexpr uintptr_t SGAMEUI_NETROOM_ARCHIVE_STATUS = 0x1402BCEF0; // 0 ready, 1 loading, -1 none
// (mode, base, out[0x104], appendTypeTag): mode 3 yields "<base>_typeD[_<lang>]".
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
constexpr uintptr_t SPAD_CANCEL_MASK = 0x140001A50;
constexpr uintptr_t MENU_SE_DECIDE = 0x14032A7F0;  // system voice 9
constexpr uintptr_t MENU_SE_CANCEL = 0x1403F78C0;  // system voice 10
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
constexpr size_t FRAME_ATTR = 0x540;         // bit 0: tile the centre cell
constexpr size_t FONT_MSG_INDEX = 0x594;     // -1 while showing a raw string
constexpr size_t FONT_CACHED_MSG_ID = 0x6F0;
constexpr size_t FONT_TYPE = 0x700;
constexpr size_t FONT_FIX_SIZE = 0x75B;
constexpr size_t USCHEDULER_UNIT_TRACK_NUM = 0x68;
constexpr size_t SPAD_SLOT_BASE = 0x58;
constexpr size_t SPAD_SLOT_STRIDE = 0x2E0;
constexpr size_t SPAD_SLOT_ACTIVE = 0x08;
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

void* GetTexture(Tex tex)
{
    if (tex >= TEX_COUNT) return nullptr;
    if (g_textures[tex]) return g_textures[tex];
    if (g_frame < g_textureRetryFrame[tex]) return nullptr;

    const TexInfo& info = GetTexInfo(tex);
    char path[0x108] = {};
    if (info.localized) {
        using Compose = void(__fastcall*)(int, const char*, char*, char);
        Native<Compose>(COMPOSE_LOCALIZED_NAME)(3, info.path, path, 1);
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
              Font font) override
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
        ApplyPlacement(unit, x, y, scale, scale, anchor, color);
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

// Menu keys follow the game's own fixed keyboard mapping
// (sPad__accumulateUnitFlagsFromQuery): arrows, Enter/A, Backspace/B, X, Y,
// Esc as Start.
struct KeyState {
    bool up, down, left, right, confirm, cancel, x, y, menu;
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

    using Release = void(__fastcall*)(void*);
    for (void*& texture : g_textures) {
        if (texture) Native<Release>(CRESOURCE_RELEASE_REF)(texture);
        texture = nullptr;
    }
    HookLog("LobbyNative: shut down (%u clones killed)\n", static_cast<unsigned>(killed));
    ResetState();
}

void Native_Invalidate()
{
    // Texture references are still ours to release; unit memory is not.
    using Release = void(__fastcall*)(void*);
    for (void*& texture : g_textures) {
        if (texture) Native<Release>(CRESOURCE_RELEASE_REF)(texture);
        texture = nullptr;
    }
    ResetState();
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
    uint32_t decideMask = 0x4000, cancelMask = 0x2000;
    if (pad) {
        for (int slot = 0; slot < 4; ++slot) {
            uint8_t* slotObject = static_cast<uint8_t*>(pad) + SPAD_SLOT_BASE + slot * SPAD_SLOT_STRIDE;
            if (slotObject[SPAD_SLOT_ACTIVE])
                padHeld |= *reinterpret_cast<uint32_t*>(slotObject + SPAD_SLOT_BUTTONS);
        }
        decideMask = Native<GetMask>(SPAD_DECIDE_MASK)(pad);
        cancelMask = Native<GetMask>(SPAD_CANCEL_MASK)(pad);
    }
    if (!foreground) padHeld = 0;

    const KeyState keys = foreground ? ReadKeyboard(textActive) : KeyState{};

    // Logical held bits.
    enum : uint32_t {
        H_UP = 1, H_DOWN = 2, H_LEFT = 4, H_RIGHT = 8, H_CONFIRM = 16, H_CANCEL = 32,
        H_X = 64, H_Y = 128, H_MENU = 256, H_LB = 512, H_RB = 1024,
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
    if (padHeld & PAD_RB) fromPad |= H_RB;
    if (keys.up) fromKeys |= H_UP;
    if (keys.down) fromKeys |= H_DOWN;
    if (keys.left) fromKeys |= H_LEFT;
    if (keys.right) fromKeys |= H_RIGHT;
    if (keys.confirm) fromKeys |= H_CONFIRM;
    if (keys.cancel) fromKeys |= H_CANCEL;
    if (keys.x) fromKeys |= H_X;
    if (keys.y) fromKeys |= H_Y;
    if (keys.menu) fromKeys |= H_MENU;

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
    input.usingKeyboard = g_usingKeyboard;

    if (textActive && foreground && IsKeyHeld(VK_BACK)) {
        const int frames = g_backspaceFrames++;
        input.backspace = frames == 0 || (frames >= kRepeatDelay && (frames - kRepeatDelay) % 3 == 0);
    } else {
        g_backspaceFrames = 0;
    }
}

void Native_PlaySfx(uint32_t sfxMask)
{
    using PlayVoid = void(__fastcall*)();
    if (sfxMask & SFX_CONFIRM) {
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
