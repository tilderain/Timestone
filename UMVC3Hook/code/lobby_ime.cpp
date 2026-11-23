#include "lobby_ime.h"

#include "lobby_native.h"
#include "../kiero/minhook/include/MinHook.h"

#include <imm.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>

extern void HookLog(const char* fmt, ...);

namespace lobby {

namespace {

std::atomic<HWND> g_window{nullptr};
std::atomic<bool> g_open{false}, g_composing{false};
std::atomic<float> g_caretX{0.5f}, g_caretY{0.5f};
SRWLOCK g_compositionLock = SRWLOCK_INIT;
wchar_t g_composition[64] = {};

// Window-thread state.
int g_applied = -1;          // 1 context attached, 0 detached, -1 not decided yet
unsigned char g_leadByte = 0; // first half of a double-byte WM_CHAR
bool g_caret = false;         // the window has a system caret at the text's end

// Wine's version string ("10.0"), or null on Windows.
const char* WineVersion()
{
    using GetVersion = const char*(__cdecl*)();
    static const GetVersion get = reinterpret_cast<GetVersion>(
        GetProcAddress(GetModuleHandleW(L"ntdll"), "wine_get_version"));
    return get ? get() : nullptr;
}

bool UnderWine() { return WineVersion() != nullptr; }

// Wine's X11 driver (winex11.drv/xim.c) passes the place of the text on to
// the Linux input method only in its "over the spot" input style
// (X11DRV_SetIMECompositionRect, Wine 10 and later), and in that style the
// input method draws the unfinished text itself and the lobby never sees it.
// The default style is kept: the unfinished text shows in the field, and the
// candidate list sits at the window's bottom left corner.
//
// An unreleased build set InputStyle=overthespot for this program. That is
// taken back once; a value someone sets afterwards is theirs.
void RestoreWineInputStyle()
{
    const char* version = WineVersion();
    if (!version) return;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(nullptr, path, MAX_PATH)) return;
    const wchar_t* name = path;
    for (const wchar_t* c = path; *c; ++c)
        if (*c == L'\\' || *c == L'/') name = c + 1;
    wchar_t key[MAX_PATH + 64];
    swprintf(key, sizeof(key) / sizeof(key[0]), L"Software\\Wine\\AppDefaults\\%ls\\X11 Driver", name);
    HKEY handle = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, key, 0, KEY_READ | KEY_WRITE, &handle) != ERROR_SUCCESS)
        return; // never written
    wchar_t current[32] = {};
    DWORD size = sizeof(current) - sizeof(wchar_t);
    const bool present = RegQueryValueExW(handle, L"InputStyle", nullptr, nullptr,
                                          reinterpret_cast<BYTE*>(current), &size) == ERROR_SUCCESS;
    const bool done = RegQueryValueExW(handle, L"TimestoneInputStyleRestored", nullptr, nullptr,
                                       nullptr, nullptr) == ERROR_SUCCESS;
    if (present && !done && !_wcsicmp(current, L"overthespot")) {
        const LONG removed = RegDeleteValueW(handle, L"InputStyle");
        HookLog("LobbyIme: Wine %s, input style back to the default for the next launch (%ld)\n",
                version, removed);
    } else {
        HookLog("LobbyIme: Wine %s, input style \"%ls\"\n", version, present ? current : L"default");
    }
    if (present && !done) {
        const DWORD one = 1;
        RegSetValueExW(handle, L"TimestoneInputStyleRestored", 0, REG_DWORD,
                       reinterpret_cast<const BYTE*>(&one), sizeof(one));
    }
    RegCloseKey(handle);
}

BOOL(WINAPI* g_originalImmDisableIME)(DWORD) = nullptr;

BOOL WINAPI Hook_ImmDisableIME(DWORD thread)
{
    HookLog("LobbyIme: kept input methods available (ImmDisableIME(%lu) ignored)\n", thread);
    return TRUE;
}

void SetComposition(const wchar_t* text, size_t units)
{
    AcquireSRWLockExclusive(&g_compositionLock);
    const size_t capacity = sizeof(g_composition) / sizeof(g_composition[0]) - 1;
    if (units > capacity) {
        // Keep the end, which is where typing happens, on a whole character.
        text += units - capacity;
        units = capacity;
        if (*text >= 0xDC00 && *text <= 0xDFFF) { ++text; --units; }
    }
    memcpy(g_composition, text, units * sizeof(wchar_t));
    g_composition[units] = 0;
    ReleaseSRWLockExclusive(&g_compositionLock);
}

void ReadOpenStatus(HWND window)
{
    bool open = false;
    if (HIMC context = ImmGetContext(window)) {
        open = ImmGetOpenStatus(context) != FALSE;
        ImmReleaseContext(window, context);
    }
    // Under Wine the state is the one Apply sets, not the Linux input
    // method's own, and keys that one takes never show as held anyway.
    g_open.store(open && !UnderWine(), std::memory_order_release);
}

// Puts the candidate list (and, for input methods that follow it, their own
// composition window) under the end of the text being typed. Three hints,
// because which one is followed depends on the input method: the composition
// window's position, the candidate window's, and the system caret (Windows
// input methods that ask where the caret is). The caret covers the text
// line, `at` is the line's bottom. Wine follows none of them in its default
// input style (see RestoreWineInputStyle).
void PlaceCandidates(HWND window, HIMC context)
{
    RECT client = {};
    if (!GetClientRect(window, &client)) return;
    POINT at = {static_cast<LONG>(g_caretX.load(std::memory_order_relaxed) * client.right),
                static_cast<LONG>(g_caretY.load(std::memory_order_relaxed) * client.bottom)};
    const int line = client.bottom * 30 / 720 > 8 ? client.bottom * 30 / 720 : 8;
    // Never shown: only its place matters.
    if (!g_caret) g_caret = CreateCaret(window, nullptr, 1, line) != FALSE;
    if (g_caret) SetCaretPos(at.x, at.y - line);
    COMPOSITIONFORM composition = {};
    composition.dwStyle = CFS_POINT;
    composition.ptCurrentPos = at;
    ImmSetCompositionWindow(context, &composition);
    CANDIDATEFORM candidates = {};
    candidates.dwStyle = CFS_CANDIDATEPOS;
    candidates.ptCurrentPos = at;
    ImmSetCandidateWindow(context, &candidates);
}

// Attaches or detaches the window's input context to match `wanted`.
void Apply(HWND window, bool wanted)
{
    if (g_applied == (wanted ? 1 : 0)) return;
    const bool first = g_applied < 0;
    g_applied = wanted ? 1 : 0; // before the calls below: they send messages back here
    if (!wanted) {
        if (HIMC context = ImmGetContext(window)) {
            ImmNotifyIME(context, NI_COMPOSITIONSTR, CPS_CANCEL, 0);
            // Detaching the context below is what keeps a Windows input
            // method out of the game, and all that is done there: its on/off
            // state is the player's, and is as they left it at the next
            // field. Wine's X11 input methods do not look at the context,
            // only at the open status (it becomes the X input context's
            // preedit state), so under Wine the input method is turned off
            // here and back on with the next field.
            if (UnderWine()) ImmSetOpenStatus(context, FALSE);
            ImmReleaseContext(window, context);
        }
        SetComposition(L"", 0);
        g_composing.store(false, std::memory_order_release);
        g_open.store(false, std::memory_order_release);
        g_leadByte = 0;
        if (g_caret) DestroyCaret();
        g_caret = false;
    }
    const BOOL done = ImmAssociateContextEx(window, nullptr, wanted ? IACE_DEFAULT : 0);
    if (wanted) {
        if (UnderWine()) {
            if (HIMC context = ImmGetContext(window)) {
                ImmSetOpenStatus(context, TRUE);
                ImmReleaseContext(window, context);
            }
        }
        ReadOpenStatus(window);
    }
    if (first || (wanted && !done))
        HookLog("LobbyIme: input context %s (%s, default IME window %s)\n",
                wanted ? "attached" : "detached", done ? "ok" : "failed",
                ImmGetDefaultIMEWnd(window) ? "present" : "absent");
}

void PushUtf16(const wchar_t* text, size_t units)
{
    for (size_t i = 0; i < units; ++i) Native_OnChar(text[i]);
}

// WM_CHAR as this window procedure receives it: UTF-16 for a Unicode window,
// otherwise bytes in the system code page, double-byte characters in halves.
void OnChar(HWND window, WPARAM wParam)
{
    if (IsWindowUnicode(window)) {
        Native_OnChar(static_cast<uint32_t>(wParam & 0xFFFF));
        return;
    }
    char bytes[2];
    int count = 0;
    const unsigned char byte = static_cast<unsigned char>(wParam);
    if (g_leadByte) {
        bytes[count++] = static_cast<char>(g_leadByte);
        g_leadByte = 0;
    } else if (IsDBCSLeadByteEx(CP_ACP, byte)) {
        g_leadByte = byte;
        return;
    }
    bytes[count++] = static_cast<char>(byte);
    wchar_t wide[4];
    const int units = MultiByteToWideChar(CP_ACP, 0, bytes, count, wide, 4);
    if (units > 0) PushUtf16(wide, static_cast<size_t>(units));
}

void OnComposition(HWND window, LPARAM flags)
{
    HIMC context = ImmGetContext(window);
    if (!context) return;
    wchar_t text[256];
    if (flags & GCS_RESULTSTR) {
        const LONG bytes = ImmGetCompositionStringW(context, GCS_RESULTSTR, text, sizeof(text));
        if (bytes > 0) PushUtf16(text, static_cast<size_t>(bytes) / sizeof(wchar_t));
    }
    // Without a composition string the composition is over: taken as the
    // result above, or cancelled (no flags at all).
    LONG bytes = 0;
    if (flags & GCS_COMPSTR)
        bytes = ImmGetCompositionStringW(context, GCS_COMPSTR, text, sizeof(text));
    SetComposition(text, bytes > 0 ? static_cast<size_t>(bytes) / sizeof(wchar_t) : 0);
    PlaceCandidates(window, context);
    ImmReleaseContext(window, context);
}

} // namespace

void Ime_InstallHook()
{
    RestoreWineInputStyle();
    if (!LoadLibraryW(L"imm32.dll")) return;
    const MH_STATUS created = MH_CreateHookApi(L"imm32", "ImmDisableIME", (LPVOID)Hook_ImmDisableIME,
                                               (LPVOID*)&g_originalImmDisableIME);
    MH_STATUS enabled = MH_UNKNOWN;
    if (created == MH_OK) {
        if (HMODULE imm = GetModuleHandleW(L"imm32"))
            enabled = MH_EnableHook((LPVOID)GetProcAddress(imm, "ImmDisableIME"));
    }
    if (enabled != MH_OK)
        HookLog("LobbyIme: could not hook ImmDisableIME (%d/%d); input methods stay off\n",
                static_cast<int>(created), static_cast<int>(enabled));
}

bool Ime_WindowMessage(HWND window, UINT message, WPARAM wParam, LPARAM* lParam, bool wanted,
                       LRESULT* result)
{
    g_window.store(window, std::memory_order_release);
    Apply(window, wanted);
    if (!wanted) return false;

    switch (message) {
    case WM_NULL:
        // The game thread's nudge: the text's end moved.
        if (HIMC context = ImmGetContext(window)) {
            PlaceCandidates(window, context);
            ImmReleaseContext(window, context);
        }
        return false;
    case WM_CHAR:
        OnChar(window, wParam);
        return false;
    case WM_IME_SETCONTEXT:
        // The lobby draws the composition itself; the candidate list is left
        // to the system.
        *lParam &= ~static_cast<LPARAM>(ISC_SHOWUICOMPOSITIONWINDOW);
        return false;
    case WM_IME_STARTCOMPOSITION:
        g_composing.store(true, std::memory_order_release);
        if (HIMC context = ImmGetContext(window)) {
            PlaceCandidates(window, context);
            ImmReleaseContext(window, context);
        }
        *result = 0;
        return true;
    case WM_IME_COMPOSITION:
        OnComposition(window, *lParam);
        *result = 0;
        return true;
    case WM_IME_ENDCOMPOSITION:
        SetComposition(L"", 0);
        g_composing.store(false, std::memory_order_release);
        *result = 0;
        return true;
    case WM_IME_NOTIFY:
        if (wParam == IMN_SETOPENSTATUS || wParam == IMN_SETCONVERSIONMODE) ReadOpenStatus(window);
        return false;
    case WM_INPUTLANGCHANGE:
        ReadOpenStatus(window);
        return false;
    default:
        return false;
    }
}

void Ime_Wake()
{
    if (HWND window = g_window.load(std::memory_order_acquire)) PostMessageW(window, WM_NULL, 0, 0);
}

bool Ime_Open() { return g_open.load(std::memory_order_acquire); }
bool Ime_Composing() { return g_composing.load(std::memory_order_acquire); }

void Ime_GetComposition(char* utf8, size_t size)
{
    if (!utf8 || !size) return;
    utf8[0] = '\0';
    AcquireSRWLockShared(&g_compositionLock);
    const int written = g_composition[0]
        ? WideCharToMultiByte(CP_UTF8, 0, g_composition, -1, utf8, static_cast<int>(size),
                              nullptr, nullptr)
        : 1;
    ReleaseSRWLockShared(&g_compositionLock);
    if (written <= 0) utf8[0] = '\0'; // did not fit: show nothing rather than a cut sequence
}

void Ime_SetCaret(float x, float y)
{
    x = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
    y = y < 0.0f ? 0.0f : y > 1.0f ? 1.0f : y;
    const bool moved = x != g_caretX.load(std::memory_order_relaxed) ||
                       y != g_caretY.load(std::memory_order_relaxed);
    g_caretX.store(x, std::memory_order_relaxed);
    g_caretY.store(y, std::memory_order_relaxed);
    // So the list is in place before the first key of a composition.
    if (moved) Ime_Wake();
}

} // namespace lobby
