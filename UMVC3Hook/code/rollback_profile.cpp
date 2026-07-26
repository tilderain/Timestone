#include "../pch.h"
#include "rollback_profile.h"
#include "rollback.h"
#include "netplay.h"

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
        "game_ui_move", "scheduler_rebuild", "chr_cpu_class_func", "main_move"
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
                "\"timing_resync\":%.6f,\"patch_disable\":%.6f},"
                "\"snapshot\":{\"count\":%u,\"total_ms\":%.6f,\"avg_ms\":%.6f,"
                "\"avg_kib\":%.6f,\"avg_regions\":%.6f,\"build_regions_ms\":%.6f,"
                "\"recycle_allocate_ms\":%.6f,\"copy_regions_ms\":%.6f,"
                "\"metadata_ms\":%.6f,\"collision_clones_ms\":%.6f,"
                "\"shot_recipes_ms\":%.6f},"
                "\"restore\":{\"total_ms\":%.6f,\"setup_ms\":%.6f,\"regions_ms\":%.6f,"
                "\"collision_clones_ms\":%.6f,\"owner_pointers_ms\":%.6f,"
                "\"hit_solver_ms\":%.6f,\"stale_pointers_ms\":%.6f,"
                "\"effect_slots_ms\":%.6f,\"shots_ms\":%.6f,\"ui_ms\":%.6f},"
                "\"hooks\":[",
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
                Profile_Ms(profile.patchDisable), profile.snapshotCount,
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
                Profile_Ms(profile.restoreShots), Profile_Ms(profile.restoreUi));

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
