#include "../pch.h"
#include "rollback_profile.h"
#include "rollback.h"
#include "netplay.h"
#include "mk10utils.h"

#include <algorithm>

RollbackProfile* g_activeRollbackProfile = nullptr;

static FILE* Profile_OpenJson()
{
    static char path[MAX_PATH] = {};
    static bool initialized = false;
    char desiredPath[MAX_PATH] = {};

    if (Netplay_IsActive()) {
        char netplayDirectory[MAX_PATH];
        if (!Rollback_GetNetplayLiveDirectory(netplayDirectory, sizeof(netplayDirectory)))
            return nullptr;
        snprintf(desiredPath, sizeof(desiredPath), "%s/rollback_profile.json",
                 netplayDirectory);
    } else {
        HMODULE module = nullptr;
        if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)&Profile_OpenJson, &module))
            return nullptr;

        if (!GetModuleFileNameA(module, desiredPath, sizeof(desiredPath))) return nullptr;
        char* separator = strrchr(desiredPath, '\\');
        if (separator)
            snprintf(separator + 1, desiredPath + sizeof(desiredPath) - separator - 1,
                     "rollback_profile.json");
        else
            snprintf(desiredPath, sizeof(desiredPath), "rollback_profile.json");
    }

    if (strcmp(path, desiredPath) != 0) {
        snprintf(path, sizeof(path), "%s", desiredPath);
        initialized = false;
    }

    if (!initialized) {
        FILE* file = fopen(path, "wb");
        if (!file) return nullptr;
        fputs("[\n]\n", file);
        fclose(file);
        initialized = true;
    }

    FILE* file = fopen(path, "r+b");
    if (!file) return nullptr;
    if (fseek(file, -2, SEEK_END) != 0) {
        fclose(file);
        return nullptr;
    }
    return file;
}

static void Profile_CloseJson(FILE* file)
{
    fputs("\n]\n", file);
    fclose(file);
}

uint64_t Profile_NowEnabled()
{
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return (uint64_t)value.QuadPart;
}

double Profile_Ms(uint64_t ticks)
{
    static uint64_t frequency = []() {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return (uint64_t)value.QuadPart;
    }();
    return frequency ? (double)ticks * 1000.0 / (double)frequency : 0.0;
}

void Profile_BeginRollbackEnabled(RollbackProfile& profile, int32_t startFrame,
                                  int32_t endFrame)
{
    profile = {};
    profile.startFrame = startFrame;
    profile.endFrame = endFrame;
    g_activeRollbackProfile = &profile;
}

void Profile_EndRollbackEnabled(RollbackProfile& profile, uint64_t profileStart)
{
    profile.total = Profile_NowEnabled() - profileStart;
    g_activeRollbackProfile = nullptr;
}

void Profile_AddTicksEnabled(uint64_t RollbackProfile::*field, uint64_t ticks)
{
    if (g_activeRollbackProfile)
        g_activeRollbackProfile->*field += ticks;
}

void Profile_AddElapsedEnabled(uint64_t RollbackProfile::*field, uint64_t start)
{
    Profile_AddTicksEnabled(field, Profile_NowEnabled() - start);
}

void Profile_AddFramePhaseEnabled(uint64_t RollbackProfile::*field,
                                  uint64_t& checkpoint)
{
    const uint64_t now = Profile_NowEnabled();
    Profile_AddTicksEnabled(field, now - checkpoint);
    checkpoint = now;
}

void Profile_AddFramePhaseMaxEnabled(uint64_t RollbackProfile::*field,
                                     uint64_t RollbackProfile::*maximum,
                                     uint64_t& checkpoint)
{
    const uint64_t now = Profile_NowEnabled();
    const uint64_t elapsed = now - checkpoint;
    if (g_activeRollbackProfile) {
        g_activeRollbackProfile->*field += elapsed;
        g_activeRollbackProfile->*maximum =
            std::max(g_activeRollbackProfile->*maximum, elapsed);
    }
    checkpoint = now;
}

void Profile_FinishFrameEnabled(uint64_t start)
{
    if (!g_activeRollbackProfile) return;
    const uint64_t elapsed = Profile_NowEnabled() - start;
    g_activeRollbackProfile->frameTotal += elapsed;
    g_activeRollbackProfile->frameTotalMax =
        std::max(g_activeRollbackProfile->frameTotalMax, elapsed);
}

void Profile_AddHeadlessPhaseEnabled(uint64_t RollbackProfile::*field,
                                     uint64_t RollbackProfile::*maximum,
                                     uint64_t& checkpoint)
{
    Profile_AddFramePhaseMaxEnabled(field, maximum, checkpoint);
}

void Profile_FinishHeadlessEnabled(uint64_t start)
{
    if (!g_activeRollbackProfile) return;
    const uint64_t elapsed = Profile_NowEnabled() - start;
    g_activeRollbackProfile->headlessTotal += elapsed;
    g_activeRollbackProfile->headlessTotalMax =
        std::max(g_activeRollbackProfile->headlessTotalMax, elapsed);
}

uint64_t Profile_HookBegin()
{
    return g_activeRollbackProfile ? Profile_NowEnabled() : 0;
}

void Profile_HookEnd(RollbackHookProfileId id, uint64_t start)
{
    if (!start || !g_activeRollbackProfile) return;
    const size_t index = (size_t)id;
    if (index >= (size_t)RollbackHookProfileId::Count) return;

    const uint64_t elapsed = Profile_NowEnabled() - start;
    g_activeRollbackProfile->hookTicks[index] += elapsed;
    g_activeRollbackProfile->hookMaxTicks[index] =
        std::max(g_activeRollbackProfile->hookMaxTicks[index], elapsed);
    ++g_activeRollbackProfile->hookCalls[index];
}

void Profile_RecordUnitCategory(RollbackUnitProfilePhase phase, uint32_t category,
                                 uint64_t ticks)
{
    if (!g_activeRollbackProfile || category >= ROLLBACK_UNIT_CATEGORY_COUNT) return;
    const size_t phaseIndex = (size_t)phase;
    if (phaseIndex >= (size_t)RollbackUnitProfilePhase::Count) return;

    g_activeRollbackProfile->unitCategoryTicks[phaseIndex][category] += ticks;
    g_activeRollbackProfile->unitCategoryMaxTicks[phaseIndex][category] =
        std::max(g_activeRollbackProfile->unitCategoryMaxTicks[phaseIndex][category], ticks);
    ++g_activeRollbackProfile->unitCategoryCalls[phaseIndex][category];
}

void Profile_RegisterUnitObjectType(RollbackUnitProfilePhase phase, uint32_t category,
                                    uint64_t dti)
{
    if (!g_activeRollbackProfile || !dti) return;
    for (uint32_t i = 0; i < g_activeRollbackProfile->unitObjectTypeCount; ++i) {
        RollbackUnitObjectProfile& entry = g_activeRollbackProfile->unitObjectTypes[i];
        if (entry.phase == phase && entry.category == category && entry.dti == dti) {
            ++entry.visits;
            return;
        }
    }
    if (g_activeRollbackProfile->unitObjectTypeCount >= ROLLBACK_UNIT_OBJECT_TYPE_COUNT)
        return;
    RollbackUnitObjectProfile& entry = g_activeRollbackProfile->unitObjectTypes[
        g_activeRollbackProfile->unitObjectTypeCount++];
    entry.phase = phase;
    entry.category = category;
    entry.dti = dti;
    entry.visits = 1;
}

void Profile_RecordUnitObject(RollbackUnitProfilePhase phase, uint32_t category,
                              uint64_t dti, uint64_t ticks)
{
    RollbackProfile* profile = g_activeRollbackProfile;
    if (!profile || !dti) return;
    for (uint32_t i = 0; i < profile->unitObjectTypeCount; ++i) {
        RollbackUnitObjectProfile& entry = profile->unitObjectTypes[i];
        if (entry.phase != phase || entry.category != category || entry.dti != dti) continue;
        InterlockedAdd64((volatile LONG64*)&entry.ticks, (LONG64)ticks);
        InterlockedIncrement((volatile LONG*)&entry.calls);
        uint64_t previous = entry.maxTicks;
        while (ticks > previous) {
            const uint64_t observed = (uint64_t)InterlockedCompareExchange64(
                (volatile LONG64*)&entry.maxTicks, (LONG64)ticks, (LONG64)previous);
            if (observed == previous) break;
            previous = observed;
        }
        return;
    }
}

void Profile_RecordSnapshotEnabled(uint64_t profileStart, uint64_t buildRegionsEnd,
                                    uint64_t allocationEnd, uint64_t copyRegionsEnd,
                                    uint64_t snapshotMetadataEnd, uint64_t collisionClonesEnd,
                                    uint64_t shotRecipesEnd, size_t bytes, uint32_t regions)
{
    if (!g_activeRollbackProfile) return;

    g_activeRollbackProfile->saveTotal += shotRecipesEnd - profileStart;
    g_activeRollbackProfile->saveBuildRegions += buildRegionsEnd - profileStart;
    g_activeRollbackProfile->saveRecycleAndAllocate += allocationEnd - buildRegionsEnd;
    g_activeRollbackProfile->saveCopyRegions += copyRegionsEnd - allocationEnd;
    g_activeRollbackProfile->saveMetadata += snapshotMetadataEnd - copyRegionsEnd;
    g_activeRollbackProfile->saveCollisionClones += collisionClonesEnd - snapshotMetadataEnd;
    g_activeRollbackProfile->saveShotRecipes += shotRecipesEnd - collisionClonesEnd;
    g_activeRollbackProfile->snapshotBytes += bytes;
    g_activeRollbackProfile->snapshotRegions += regions;
    ++g_activeRollbackProfile->snapshotCount;
}

void Profile_DumpEnabled(const RollbackProfile& profile, bool ndjsonEnabled,
                         bool multiResimEnabled)
{
    const int frameCount = profile.endFrame - profile.startFrame;
    const double frameDivisor = frameCount > 0 ? (double)frameCount : 1.0;
    const double snapshotDivisor = profile.snapshotCount > 0 ?
        (double)profile.snapshotCount : 1.0;
    static const char* const hookNames[] = {
        "button_remap", "get_current_input", "step_intro", "step_fighting",
        "process_frame_input", "pad_slot_read", "pad_poll_all", "xinput_to_mask",
        "pad_post_read", "input_auto_repeat", "shot_spawn", "shot_spawn_ex",
        "effect_create", "build_attach_matrix", "shot_update_hitboxes", "king_kobun_move",
        "unit_remove", "unit_remove_recipe_lookup", "unit_remove_shot_detach",
        "unit_remove_effect_check", "unit_remove_effect_sanitize",
        "unit_remove_effect_resource", "unit_remove_effect_generator",
        "unit_remove_effect_sub_buffers", "unit_remove_native", "unit_remove_recipe_cleanup",
        "kirigakure_effect_update", "render_end_frame", "sound_move",
        "game_ui_move", "chr_cpu_class_func", "main_move"
    };
    static_assert(sizeof(hookNames) / sizeof(hookNames[0]) ==
                  (size_t)RollbackHookProfileId::Count);

    FILE* json = Profile_OpenJson();
    if (json) {
        const bool firstEntry = ftell(json) == 2;
        if (!firstEntry) fputs(",\n", json);
        fprintf(json,
                "  {\"type\":\"rollback\",\"start_frame\":%d,\"end_frame\":%d,"
                "\"frame_count\":%d,\"total_ms\":%.6f,\"avg_frame_ms\":%.6f,"
                "\"max_frame_ms\":%.6f,\"ndjson_enabled\":%s,\"multi_resim_enabled\":%s,"
                "\"phases_ms\":{\"patch_enable\":%.6f,\"input_history_save\":%.6f,"
                "\"diag_setup\":%.6f,\"pre_restore_json\":%.6f,\"restore\":%.6f,"
                "\"input_availability\":%.6f,\"frame_input\":%.6f,\"frame_diag_pre\":%.6f,"
                "\"frame_json\":%.6f,\"frame_snapshot\":%.6f,\"native_tick\":%.6f,"
                 "\"native_tick_max\":%.6f,\"frame_input_end\":%.6f,"
                 "\"frame_diag_post\":%.6f,\"input_history_restore\":%.6f,"
                 "\"timing_resync\":%.6f,\"cutin_repair\":%.6f,\"patch_disable\":%.6f},"
                 "\"headless_tick\":{\"total_ms\":%.6f,\"max_ms\":%.6f,"
                 "\"timing_input_ms\":%.6f,\"timing_input_max_ms\":%.6f,"
                 "\"early_draw_collision_ms\":%.6f,\"early_draw_collision_max_ms\":%.6f,"
                 "\"viewport_setup_ms\":%.6f,\"viewport_setup_max_ms\":%.6f,"
                 "\"render_begin_area_ms\":%.6f,\"render_begin_area_max_ms\":%.6f,"
                 "\"action_ms\":%.6f,\"action_max_ms\":%.6f,"
                 "\"input_devices_ms\":%.6f,\"input_devices_max_ms\":%.6f,"
                 "\"gameplay_setup_ms\":%.6f,\"gameplay_setup_max_ms\":%.6f,"
                 "\"unit_move_ms\":%.6f,\"unit_move_max_ms\":%.6f,"
                 "\"collision_hit_ms\":%.6f,\"collision_hit_max_ms\":%.6f,"
                 "\"unit_sync_ms\":%.6f,\"unit_sync_max_ms\":%.6f,"
                 "\"simulation_tail_ms\":%.6f,\"simulation_tail_max_ms\":%.6f,"
                 "\"manager_core_ms\":%.6f,\"manager_core_max_ms\":%.6f,"
                 "\"network_services_ms\":%.6f,\"network_services_max_ms\":%.6f,"
                 "\"render_ms\":%.6f,\"render_max_ms\":%.6f,"
                 "\"post_frame_ms\":%.6f,\"post_frame_max_ms\":%.6f,"
                 "\"alt_f4_ms\":%.6f,\"alt_f4_max_ms\":%.6f},"
                 "\"snapshot\":{\"count\":%u,\"total_ms\":%.6f,\"avg_ms\":%.6f,"
                "\"avg_kib\":%.6f,\"avg_regions\":%.6f,\"build_regions_ms\":%.6f,"
                "\"recycle_allocate_ms\":%.6f,\"copy_regions_ms\":%.6f,"
                "\"metadata_ms\":%.6f,\"collision_clones_ms\":%.6f,"
                "\"shot_recipes_ms\":%.6f},"
                "\"restore\":{\"total_ms\":%.6f,\"setup_ms\":%.6f,\"regions_ms\":%.6f,"
                "\"collision_clones_ms\":%.6f,\"owner_pointers_ms\":%.6f,"
                 "\"hit_solver_ms\":%.6f,\"stale_pointers_ms\":%.6f,"
                 "\"effect_slots_ms\":%.6f,\"shots_ms\":%.6f,\"ui_ms\":%.6f},"
                 "\"shot_restore\":{\"pin_resources_ms\":%.6f,\"pre_sanitize_ms\":%.6f,"
                 "\"retire_effects_ms\":%.6f,\"collect_shots_ms\":%.6f,"
                 "\"destroy_shots_ms\":%.6f,\"post_sanitize_ms\":%.6f,"
                 "\"sanitize_effects_ms\":%.6f,\"respawn_setup_ms\":%.6f,"
                 "\"respawn_ms\":%.6f,\"respawn_prepare_ms\":%.6f,"
                 "\"respawn_factory_ms\":%.6f,\"respawn_register_ms\":%.6f,"
                 "\"respawn_release_effects_ms\":%.6f,\"respawn_concrete_ms\":%.6f,"
                 "\"concrete_validate_ms\":%.6f,\"concrete_copy_body_ms\":%.6f,"
                 "\"concrete_pointer_scan_ms\":%.6f,\"concrete_embedded_live_ms\":%.6f,"
                 "\"concrete_embedded_saved_ms\":%.6f,\"concrete_apply_body_ms\":%.6f,"
                 "\"concrete_targets_ms\":%.6f,\"concrete_collision_ms\":%.6f,"
                 "\"respawn_overlay_ms\":%.6f,\"finalize_ms\":%.6f,"
                 "\"effect_visits\":%u,\"effect_sanitize_calls\":%u,"
                 "\"generator_nodes\":%u,\"current_shots\":%u,\"saved_shots\":%u},"
                 "\"unit_categories\":[",
                profile.startFrame, profile.endFrame, frameCount, Profile_Ms(profile.total),
                Profile_Ms(profile.frameTotal) / frameDivisor, Profile_Ms(profile.frameTotalMax),
                ndjsonEnabled ? "true" : "false", multiResimEnabled ? "true" : "false",
                Profile_Ms(profile.patchEnable), Profile_Ms(profile.inputHistorySave),
                Profile_Ms(profile.diagSetup), Profile_Ms(profile.preRestoreJson),
                Profile_Ms(profile.restoreTotal), Profile_Ms(profile.inputAvailability),
                Profile_Ms(profile.frameInput), Profile_Ms(profile.frameDiagPre),
                Profile_Ms(profile.frameJson), Profile_Ms(profile.frameSnapshot),
                Profile_Ms(profile.frameNativeTick), Profile_Ms(profile.frameNativeTickMax),
                 Profile_Ms(profile.frameInputEnd), Profile_Ms(profile.frameDiagPost),
                  Profile_Ms(profile.inputHistoryRestore), Profile_Ms(profile.timingResync),
                  Profile_Ms(profile.cutinRepair), Profile_Ms(profile.patchDisable),
                 Profile_Ms(profile.headlessTotal), Profile_Ms(profile.headlessTotalMax),
                 Profile_Ms(profile.headlessTimingInput), Profile_Ms(profile.headlessTimingInputMax),
                 Profile_Ms(profile.headlessEarlyDrawCollision), Profile_Ms(profile.headlessEarlyDrawCollisionMax),
                 Profile_Ms(profile.headlessViewportSetup), Profile_Ms(profile.headlessViewportSetupMax),
                 Profile_Ms(profile.headlessRenderBeginArea), Profile_Ms(profile.headlessRenderBeginAreaMax),
                 Profile_Ms(profile.headlessAction), Profile_Ms(profile.headlessActionMax),
                 Profile_Ms(profile.headlessInputDevices), Profile_Ms(profile.headlessInputDevicesMax),
                 Profile_Ms(profile.headlessGameplaySetup), Profile_Ms(profile.headlessGameplaySetupMax),
                 Profile_Ms(profile.headlessUnitMove), Profile_Ms(profile.headlessUnitMoveMax),
                 Profile_Ms(profile.headlessCollisionHit), Profile_Ms(profile.headlessCollisionHitMax),
                 Profile_Ms(profile.headlessUnitSync), Profile_Ms(profile.headlessUnitSyncMax),
                 Profile_Ms(profile.headlessSimulationTail), Profile_Ms(profile.headlessSimulationTailMax),
                 Profile_Ms(profile.headlessManagerCore), Profile_Ms(profile.headlessManagerCoreMax),
                 Profile_Ms(profile.headlessNetworkServices), Profile_Ms(profile.headlessNetworkServicesMax),
                 Profile_Ms(profile.headlessRender), Profile_Ms(profile.headlessRenderMax),
                 Profile_Ms(profile.headlessPostFrame), Profile_Ms(profile.headlessPostFrameMax),
                 Profile_Ms(profile.headlessAltF4), Profile_Ms(profile.headlessAltF4Max),
                 profile.snapshotCount,
                Profile_Ms(profile.saveTotal), Profile_Ms(profile.saveTotal) / snapshotDivisor,
                (double)profile.snapshotBytes / snapshotDivisor / 1024.0,
                (double)profile.snapshotRegions / snapshotDivisor,
                Profile_Ms(profile.saveBuildRegions), Profile_Ms(profile.saveRecycleAndAllocate),
                Profile_Ms(profile.saveCopyRegions), Profile_Ms(profile.saveMetadata),
                Profile_Ms(profile.saveCollisionClones), Profile_Ms(profile.saveShotRecipes),
                Profile_Ms(profile.restoreTotal), Profile_Ms(profile.restoreSetup),
                Profile_Ms(profile.restoreRegions), Profile_Ms(profile.restoreCollisionClones),
                 Profile_Ms(profile.restoreOwnerPointers), Profile_Ms(profile.restoreHitSolver),
                 Profile_Ms(profile.restoreStalePointers), Profile_Ms(profile.restoreEffectSlots),
                 Profile_Ms(profile.restoreShots), Profile_Ms(profile.restoreUi),
                 Profile_Ms(profile.restoreShotPinResources),
                 Profile_Ms(profile.restoreShotPreSanitize),
                 Profile_Ms(profile.restoreShotRetireEffects),
                 Profile_Ms(profile.restoreShotCollect),
                 Profile_Ms(profile.restoreShotDestroy),
                 Profile_Ms(profile.restoreShotPostSanitize),
                 Profile_Ms(profile.restoreShotSanitizeEffects),
                 Profile_Ms(profile.restoreShotRespawnSetup),
                 Profile_Ms(profile.restoreShotRespawn),
                 Profile_Ms(profile.restoreShotRespawnPrepare),
                 Profile_Ms(profile.restoreShotRespawnFactory),
                 Profile_Ms(profile.restoreShotRespawnRegister),
                 Profile_Ms(profile.restoreShotRespawnReleaseEffects),
                 Profile_Ms(profile.restoreShotRespawnConcrete),
                 Profile_Ms(profile.restoreConcreteValidate),
                 Profile_Ms(profile.restoreConcreteCopyBody),
                 Profile_Ms(profile.restoreConcretePointerScan),
                 Profile_Ms(profile.restoreConcreteEmbeddedLive),
                 Profile_Ms(profile.restoreConcreteEmbeddedSaved),
                 Profile_Ms(profile.restoreConcreteApplyBody),
                 Profile_Ms(profile.restoreConcreteTargets),
                 Profile_Ms(profile.restoreConcreteCollision),
                 Profile_Ms(profile.restoreShotRespawnOverlay),
                 Profile_Ms(profile.restoreShotFinalize),
                 profile.restoreEffectVisits, profile.restoreEffectSanitizeCalls,
                 profile.restoreEffectGeneratorNodes, profile.restoreCurrentShots,
                  profile.restoreSavedShots);

        using GetUnitCategoryNameFn = const char*(__fastcall*)(uint64_t, uint32_t);
        auto getCategoryName = (GetUnitCategoryNameFn)_addr(0x14051b1c0);
        uint64_t sUnit = *(uint64_t*)_addr(0x140e17698);
        static const char* const unitPhaseNames[] = {"move", "sync_job", "sync_save"};
        bool firstCategory = true;
        for (size_t phase = 0; phase < (size_t)RollbackUnitProfilePhase::Count; ++phase) {
            for (size_t category = 0; category < ROLLBACK_UNIT_CATEGORY_COUNT; ++category) {
                const uint32_t calls = profile.unitCategoryCalls[phase][category];
                if (!calls) continue;
                const char* name = sUnit ? getCategoryName(sUnit, (uint32_t)category) : "unknown";
                fprintf(json,
                        "%s{\"phase\":\"%s\",\"category\":%u,\"name\":\"%s\","
                        "\"calls\":%u,\"total_ms\":%.6f,\"avg_us\":%.6f,\"max_us\":%.6f}",
                        firstCategory ? "" : ",", unitPhaseNames[phase],
                        (unsigned)category, name ? name : "unknown", calls,
                        Profile_Ms(profile.unitCategoryTicks[phase][category]),
                        Profile_Ms(profile.unitCategoryTicks[phase][category]) * 1000.0 / calls,
                        Profile_Ms(profile.unitCategoryMaxTicks[phase][category]) * 1000.0);
                firstCategory = false;
            }
        }
        fputs("],\"unit_objects\":[", json);

        bool firstObject = true;
        for (uint32_t i = 0; i < profile.unitObjectTypeCount; ++i) {
            const RollbackUnitObjectProfile& entry = profile.unitObjectTypes[i];
            const char* name = *(const char**)(entry.dti + 0x08);
            fprintf(json,
                    "%s{\"category\":%u,\"phase\":\"%s\",\"dti\":\"0x%llx\","
                    "\"name\":\"%s\",\"visits\":%u,\"calls\":%u,\"total_ms\":%.6f,"
                    "\"avg_us\":%.6f,\"max_us\":%.6f}",
                    firstObject ? "" : ",", entry.category,
                    unitPhaseNames[(size_t)entry.phase],
                    (unsigned long long)entry.dti, name ? name : "unknown",
                    entry.visits, entry.calls, Profile_Ms(entry.ticks),
                    entry.calls ? Profile_Ms(entry.ticks) * 1000.0 / entry.calls : 0.0,
                    Profile_Ms(entry.maxTicks) * 1000.0);
            firstObject = false;
        }
        fputs("],\"hooks\":[", json);

        bool firstHook = true;
        for (size_t hook = 0; hook < (size_t)RollbackHookProfileId::Count; ++hook) {
            const uint32_t calls = profile.hookCalls[hook];
            if (calls == 0) continue;
            fprintf(json,
                    "%s{\"name\":\"%s\",\"calls\":%u,\"total_ms\":%.6f,"
                    "\"avg_us\":%.6f,\"max_us\":%.6f}",
                    firstHook ? "" : ",", hookNames[hook], calls,
                    Profile_Ms(profile.hookTicks[hook]),
                    Profile_Ms(profile.hookTicks[hook]) * 1000.0 / calls,
                    Profile_Ms(profile.hookMaxTicks[hook]) * 1000.0);
            firstHook = false;
        }
        fputs("]}", json);
        Profile_CloseJson(json);
    }

}

void Profile_DumpPostResimEnabled(int32_t frame, uint64_t notifyTicks,
                                 uint64_t resolveTicks, uint64_t saveSnapshotTicks,
                                 uint64_t totalTicks)
{
    FILE* json = Profile_OpenJson();
    if (!json) return;

    const bool firstEntry = ftell(json) == 2;
    if (!firstEntry) fputs(",\n", json);
    fprintf(json,
            "  {\"type\":\"post_resim\",\"frame\":%d,\"notify_ms\":%.6f,"
            "\"resolve_ms\":%.6f,\"save_snapshot_ms\":%.6f,\"total_ms\":%.6f}",
            frame, Profile_Ms(notifyTicks), Profile_Ms(resolveTicks),
            Profile_Ms(saveSnapshotTicks), Profile_Ms(totalTicks));
    Profile_CloseJson(json);
}
