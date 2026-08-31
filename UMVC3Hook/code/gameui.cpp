#include "../pch.h"
#include "gameui.h"
#include "mk10utils.h"
#include "../imgui/imgui.h"

#include <Windows.h>
#include <cstdint>

extern void HookLog(const char* fmt, ...);

namespace {

constexpr uintptr_t SGAMEUI_GET_INSTANCE             = 0x140001ac0;
constexpr uintptr_t SGAMEUI_REQUEST_PAUSE            = 0x1402bf730;
constexpr uintptr_t SGAMEUI_CONSUME_PAUSE_REQUEST    = 0x1402c1600;
constexpr uintptr_t SGAMEUI_CREATE_POPUP             = 0x1402bb470;
constexpr uintptr_t SGAMEUI_GET_POPUP                = 0x1402bc890;
constexpr uintptr_t SGAMEUI_QUERY_POPUP              = 0x1402bc850;
constexpr uintptr_t SGAMEUI_CLOSE_POPUP              = 0x1402bdb20;
constexpr uintptr_t UUI_POPUP_CONFIGURE              = 0x14032abe0;
constexpr uintptr_t UUI_POPUP_SET_MESSAGE_RESOURCE   = 0x1402c0120;
constexpr uintptr_t UUI_CHOICE_POPUP_CONFIGURE       = 0x140323170;
constexpr uintptr_t UUI_POPUP_SET_MODAL_ATTR         = 0x1402833c0;
constexpr uintptr_t SRESOURCE_GET_INSTANCE           = 0x140001b10;
constexpr uintptr_t SRESOURCE_HASH_KEY               = 0x140519430;
constexpr uintptr_t SRESOURCE_FIND_BUCKET            = 0x140518520;
constexpr uintptr_t RSCHEDULER_ALLOC_DTI             = 0x140e17830;
constexpr int NETWORK_WAIT_MESSAGE_ID                = 0x75b;
constexpr int OPPONENT_DISCONNECTED_MESSAGE_ID       = 1742;
constexpr ULONGLONG RESOURCE_READY_CHECK_INTERVAL_MS = 250;
constexpr char CHOICE_WINDOW_RESOURCE[] =
    "ui\\cmn\\cmn_sprite\\cmn_window_menu";

bool g_waitingForOpponentPopupActive = false;
uint32_t g_waitingForOpponentPopupHandle = 0;
bool g_opponentDisconnectedPopupCreated = false;
uint32_t g_opponentDisconnectedPopupHandle = 0;
ULONGLONG g_choiceWindowResourceCheckTick = 0;

bool IsChoiceWindowResourceLoaded()
{
    using GetResource_t = uint64_t(__fastcall*)();
    using HashKey_t = uint64_t(__fastcall*)(
        uint64_t, uint64_t, const char*);
    using FindBucket_t = uint64_t(__fastcall*)(uint64_t, uint64_t, uint32_t);

    auto getResource = reinterpret_cast<GetResource_t>(
        _addr(SRESOURCE_GET_INSTANCE));
    const uint64_t resourceManager = getResource();
    if (!resourceManager)
        return false;

    const ULONGLONG now = GetTickCount64();
    if (g_choiceWindowResourceCheckTick != 0 &&
        now - g_choiceWindowResourceCheckTick < RESOURCE_READY_CHECK_INTERVAL_MS)
        return false;
    g_choiceWindowResourceCheckTick = now;

    auto hashKey = reinterpret_cast<HashKey_t>(_addr(SRESOURCE_HASH_KEY));
    auto findBucket = reinterpret_cast<FindBucket_t>(
        _addr(SRESOURCE_FIND_BUCKET));
    static uint64_t key = 0;
    if (key == 0) {
        key = hashKey(resourceManager, _addr(RSCHEDULER_ALLOC_DTI),
                      CHOICE_WINDOW_RESOURCE);
    }

    auto resourceLock = reinterpret_cast<CRITICAL_SECTION*>(resourceManager + 8);
    EnterCriticalSection(resourceLock);
    const uint64_t resource = findBucket(resourceManager, key, 0);
    const uint32_t flags = resource ? *(uint32_t*)(resource + 0x54) : 0;
    LeaveCriticalSection(resourceLock);

    return resource && (flags & 0x1) != 0 && (flags & 0x50) == 0;
}

} // namespace

void GameUI_SetWaitingForOpponent(bool active)
{
    if (active == g_waitingForOpponentPopupActive)
        return;

    using GetGameUi_t = uint64_t(__fastcall*)();
    using RequestPause_t = void(__fastcall*)(uint64_t, char, uint8_t);
    using ConsumePauseRequest_t = void(__fastcall*)(uint64_t);
    using CreatePopup_t = uint32_t(__fastcall*)(uint64_t, int, uint32_t, int);
    using GetPopup_t = uint64_t(__fastcall*)(uint64_t, uint32_t*);
    using ClosePopup_t = void(__fastcall*)(uint64_t, uint32_t*);
    using ConfigurePopup_t = void(__fastcall*)(uint64_t, int, int, uint32_t);
    using SetMessageResource_t = void(__fastcall*)(uint64_t, int);

    auto getGameUi = reinterpret_cast<GetGameUi_t>(_addr(SGAMEUI_GET_INSTANCE));
    const uint64_t gameUi = getGameUi();
    if (!gameUi)
        return;

    auto requestPause = reinterpret_cast<RequestPause_t>(
        _addr(SGAMEUI_REQUEST_PAUSE));
    auto consumePauseRequest = reinterpret_cast<ConsumePauseRequest_t>(
        _addr(SGAMEUI_CONSUME_PAUSE_REQUEST));
    if (active) {
        requestPause(gameUi, 1, 1);
        consumePauseRequest(gameUi);

        auto createPopup = reinterpret_cast<CreatePopup_t>(
            _addr(SGAMEUI_CREATE_POPUP));
        g_waitingForOpponentPopupHandle = createPopup(gameUi, 1, 0, 10000);

        auto getPopup = reinterpret_cast<GetPopup_t>(_addr(SGAMEUI_GET_POPUP));
        const uint64_t popup = getPopup(
            gameUi, &g_waitingForOpponentPopupHandle);
        if (!popup) {
            auto closePopup = reinterpret_cast<ClosePopup_t>(
                _addr(SGAMEUI_CLOSE_POPUP));
            closePopup(gameUi, &g_waitingForOpponentPopupHandle);
            g_waitingForOpponentPopupHandle = 0;
            requestPause(gameUi, 0, 1);
            consumePauseRequest(gameUi);
            return;
        }

        auto configurePopup = reinterpret_cast<ConfigurePopup_t>(
            _addr(UUI_POPUP_CONFIGURE));
        configurePopup(popup, 1, NETWORK_WAIT_MESSAGE_ID, 0);
        auto setMessageResource = reinterpret_cast<SetMessageResource_t>(
            _addr(UUI_POPUP_SET_MESSAGE_RESOURCE));
        setMessageResource(popup, 0);
        g_waitingForOpponentPopupActive = true;
        HookLog("Netplay: native waiting popup opened handle=%u\n",
                g_waitingForOpponentPopupHandle);
        return;
    }

    if (g_waitingForOpponentPopupHandle) {
        auto closePopup = reinterpret_cast<ClosePopup_t>(
            _addr(SGAMEUI_CLOSE_POPUP));
        closePopup(gameUi, &g_waitingForOpponentPopupHandle);
        g_waitingForOpponentPopupHandle = 0;
    }
    requestPause(gameUi, 0, 1);
    consumePauseRequest(gameUi);
    g_waitingForOpponentPopupActive = false;
    HookLog("Netplay: native waiting popup closed\n");
}

void GameUI_TryShowOpponentDisconnected()
{
    if (g_opponentDisconnectedPopupCreated ||
        !IsChoiceWindowResourceLoaded())
        return;

    using GetGameUi_t = uint64_t(__fastcall*)();
    using CreatePopup_t = uint32_t(__fastcall*)(uint64_t, int, uint32_t, int);
    using QueryPopup_t = uint64_t(__fastcall*)(uint64_t, uint32_t*);
    using ClosePopup_t = void(__fastcall*)(uint64_t, uint32_t*);
    using ConfigureChoicePopup_t = void(__fastcall*)(
        uint64_t, int, int, uint8_t, int, int, int);
    using SetModalAttr_t = void(__fastcall*)(uint64_t);

    auto getGameUi = reinterpret_cast<GetGameUi_t>(_addr(SGAMEUI_GET_INSTANCE));
    const uint64_t gameUi = getGameUi();
    if (!gameUi)
        return;

    auto createPopup = reinterpret_cast<CreatePopup_t>(
        _addr(SGAMEUI_CREATE_POPUP));
    g_opponentDisconnectedPopupHandle = createPopup(gameUi, 2, 0, 10000);

    auto queryPopup = reinterpret_cast<QueryPopup_t>(_addr(SGAMEUI_QUERY_POPUP));
    const uint64_t popup = queryPopup(
        gameUi, &g_opponentDisconnectedPopupHandle);
    if (!popup) {
        auto closePopup = reinterpret_cast<ClosePopup_t>(
            _addr(SGAMEUI_CLOSE_POPUP));
        closePopup(gameUi, &g_opponentDisconnectedPopupHandle);
        g_opponentDisconnectedPopupHandle = 0;
        return;
    }

    auto configurePopup = reinterpret_cast<ConfigureChoicePopup_t>(
        _addr(UUI_CHOICE_POPUP_CONFIGURE));
    configurePopup(popup, 2, OPPONENT_DISCONNECTED_MESSAGE_ID, 0, 0, 0, 2);
    auto setModalAttr = reinterpret_cast<SetModalAttr_t>(
        _addr(UUI_POPUP_SET_MODAL_ATTR));
    setModalAttr(popup);
    g_opponentDisconnectedPopupCreated = true;
    HookLog("Netplay: native opponent-disconnected popup opened handle=%u message=0x%X\n",
            g_opponentDisconnectedPopupHandle,
            OPPONENT_DISCONNECTED_MESSAGE_ID);
}

void GameUI_ResetOpponentDisconnectedPopup()
{
    g_opponentDisconnectedPopupCreated = false;
    g_opponentDisconnectedPopupHandle = 0;
    g_choiceWindowResourceCheckTick = 0;
}

void GameUI_DrawOpponentDisconnectedFallback(bool visible)
{
    if (!visible || g_opponentDisconnectedPopupCreated)
        return;

    ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x * 0.5f,
                                   ImGui::GetIO().DisplaySize.y * 0.18f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 1.0f, 1.0f));
    ImGui::Begin("##OpponentDisconnectedOverlay", nullptr,
        ImGuiWindowFlags_NoTitleBar |
        ImGuiWindowFlags_NoResize |
        ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoScrollbar |
        ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize |
        ImGuiWindowFlags_NoInputs);
    ImGui::Text("Connection to opponent has been lost.");
    ImGui::End();
    ImGui::PopStyleColor(2);
}
