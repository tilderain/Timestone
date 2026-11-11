// Included in the lifetime recorder's private namespace, after every stage.
// Saved-shot rewind: one transaction over retained storage. Every shot in the
// target snapshot still owns its original allocation (deferred destruction) and
// future shots were already withdrawn, so restoring is copying bytes back in
// place and republishing registration. Nothing is reconstructed or relinked.

// Class-specific fields past the common prefix that stay live: owned resource
// references (released only by the destructor) and sound cRefs (live audio
// policy). Derived from every uShot destructor in the executable: these are the
// only ownership a concrete tail has besides inline constraints. Keyed by
// preferred-base DTI address.
struct ShotTailLiveField { uintptr_t dti; uint32_t offset, size; };
constexpr ShotTailLiveField ShotTailLiveFields[] = {
    {0x140D4D7E0, 0x2300, 0x10}, // uShotLaserRekkouha: two resources
    {0x140D4CA20, 0x2150, 0x08}, // uShotOptionAttackWire
    {0x140D48A68, 0x2150, 0x08}, // uShotOptionShieldSlash
    {0x140D4CA60, 0x2140, 0x08}, // uShotOptionSwingWire
    {0x140D487C0, 0x2148, 0x08}, // uShotShieldKnightShield
    {0x140D48808, 0x2218, 0x08}, // uShotVulcanAxe
    {0x140D48EE8, 0x2210, 0x08}, // uShotVulcanSatelliteLaserLock
    {0x140D4D6A0, 0x21A8, 0x08}, // uShotVulcanShockingPinkBomb resource
    {0x140D4CB00, 0x2150, 0x08}, // uShotVulcanSwingWire
    {0x140D4B398, 0x2140, 0x10}, // uShotMissileJammingBomb sound
    {0x140D4BD40, 0x2150, 0x10}, // uShotShieldHealingField sound
    {0x140D4AF00, 0x2250, 0x10}, // uShotVulcanAnkihouBomb sound
    {0x140D4D178, 0x2148, 0x10}, // uShotVulcanBarrier sound
    {0x140D49750, 0x2140, 0x10}, // uShotVulcanFireCarpet sound
    {0x140D4B428, 0x2140, 0x10}, // uShotVulcanJammingBomb sound
    {0x140D4B4A8, 0x2228, 0x10}, // uShotVulcanLightWall sound
    {0x140D497D8, 0x2140, 0x10}, // uShotVulcanMagicCreate sound
    {0x140D4D6A0, 0x21C0, 0x10}, // uShotVulcanShockingPinkBomb sound
    {0x140D4BE38, 0x2148, 0x10}, // uShotVulcanTKBind sound
};

using ShotTailHoles = std::vector<std::pair<size_t, size_t>>; // {offset, size}

// Copies [ShotCpuCommonSize, extent) except the holes. No native calls.
void RestoreShotTail(uint8_t* live, const uint8_t* saved, size_t extent, ShotTailHoles& holes)
{
    std::sort(holes.begin(), holes.end());
    size_t at = ShotCpuCommonSize;
    for (const auto& [offset, size] : holes) {
        if (offset > at) std::memcpy(live + at, saved + at, std::min(offset, extent) - at);
        at = std::max(at, offset + size);
        if (at >= extent) return;
    }
    std::memcpy(live + at, saved + at, extent - at);
}

// Caller holds g_lock. Tail words that are exactly a retained unit's address:
// registry membership, not a guess about pointer-shaped values. A scalar that
// collides only extends that unit's lease.
template<class Visit> void VisitShotTailUnits(const Identity& owner, Visit visit)
{
    for (size_t field = ShotCpuCommonSize; field + 8 <= owner.extent; field += 8) {
        void* object = Read<void*>(owner.object, field);
        if (uintptr_t(object) < 0x10000 || object == owner.object) continue;
        const Identity* identity = Find(object, false);
        if (identity && (identity->occupancy == Occupancy::Live || identity->occupancy == Occupancy::Quarantined))
            visit(field, object);
    }
}

} // namespace

void RollbackUnit_RestoreShots(int32_t frame)
{
    RollbackUnitFailureScope failureScope("shots.restore", nullptr, 0, frame);
    if constexpr (!ROLLBACK_UNIT_ENABLED || !EMPTY_EFFECT_QUARANTINE_ENABLED || !EFFECT_RESTORE_ENABLED ||
                  !SAVED_SHOT_RESTORE_ENABLED) return;
    if (!g_rollbackUnitEnabled.load(std::memory_order_relaxed)) return;
    UnitGuard unitFence;
    ShotWorldGuard shotFence;
    const unsigned slot = unsigned((frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    const uint32_t bit = uint32_t(1) << slot;
    struct Work { Identity* owner; const ShotState* shot; const uint8_t* image; };
    // Restore-fence scratch, reused across restores.
    static std::vector<Work> work;
    static ScratchSet savedConstraints;
    work.clear(); savedConstraints.clear();
    AcquireSRWLockExclusive(&g_lock);
    const auto* saved = g_liveSnapshots[slot];
    if (!saved || !saved->valid || saved->frame != frame || !unitFence.manager ||
        saved->unitManager != unitFence.manager || saved->shots.empty()) {
        // No shot existed at the target frame: withdrawal already removed every
        // later one, and the saved registration is the empty one it left behind.
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    LIFE_REQUIRE(saved->shotPayload == RollbackUnitShotPayload::Capture && saved->thread == GetCurrentThreadId() &&
        shotFence.manager && saved->shotManager == shotFence.manager);
    for (const auto& shot : saved->shots) {
        const auto member = std::find_if(saved->members.begin(), saved->members.end(),
            [&](const Identity& member) { return member.kind == Kind::Shot && member.id == shot.id; });
        LIFE_REQUIRE(member != saved->members.end());
        Identity* owner = Find(member->object, false);
        LIFE_REQUIRE(owner && owner->id == shot.id && owner->kind == Kind::Shot && (owner->snapshots & bit) &&
            !(owner->occupancy != Occupancy::Live && owner->occupancy != Occupancy::Quarantined));
        const ShotSpan& body = ShotBodySpan(shot, owner->object);
        LIFE_REQUIRE(body.memory.size == owner->extent);
        work.push_back({owner, &shot, shot.payload.data() + body.memory.offset});
    }
    // Ascending: each shot finds its inline ones by range. Used under this lock only.
    const std::vector<void*>& constraints = SortedConstraints();
    for (const auto& node : saved->constraints) {
        // The same provenance the execution stage requires, checked before any write.
        const auto current = g_constructedConstraints.find(node.object);
        LIFE_REQUIRE(current != g_constructedConstraints.end() && current->second == node.construction &&
            Read<uintptr_t>(node.object, 0) == Read<uintptr_t>(node.image, 0) &&
            Read<uintptr_t>(node.object, 0x40) == Read<uintptr_t>(node.image, 0x40) &&
            savedConstraints.insert(node.object));
    }
    // Per shot: unit-header holes for saved inline constraints, whole-object holes
    // for ones initialized after the snapshot (the execution stage withdraws them).
    // Inner hole lists keep their storage too: clear, never shrink the outer.
    static std::vector<ShotTailHoles> holes;
    static std::vector<void*> future;
    if (holes.size() < work.size()) holes.resize(work.size());
    for (size_t i = 0; i < work.size(); ++i) holes[i].clear();
    future.clear();
    for (size_t i = 0; i < work.size(); ++i) {
        const Identity& owner = *work[i].owner;
        if (owner.extent >= 0x90)
            for (auto at = std::lower_bound(constraints.begin(), constraints.end(), owner.object, std::less<void*>{});
                 at != constraints.end() && uintptr_t(*at) - uintptr_t(owner.object) <= owner.extent - 0x90; ++at) {
                const size_t relative = uintptr_t(*at) - uintptr_t(owner.object);
                if (relative < ShotCpuCommonSize) continue; // Not tail bytes; the kernel leaves it live.
                if (savedConstraints.contains(*at)) {
                    // Lifecycle/category flags, scheduler links and group mask.
                    holes[i].push_back({relative + 0x10, 0x18});
                    holes[i].push_back({relative + 0x30, 0x08});
                } else {
                    LIFE_REQUIRE(g_constructedConstraints.at(*at) > saved->constraintWatermark);
                    holes[i].push_back({relative, 0x90});
                    future.push_back(*at);
                }
            }
        // None of these classes has a subclass, so the exact DTI identifies them.
        for (const auto& field : ShotTailLiveFields)
            if (owner.dti == uint64_t(_addr(field.dti)) && field.offset + field.size <= owner.extent)
                holes[i].push_back({field.offset, field.size});
    }
    ReleaseSRWLockExclusive(&g_lock);

    for (size_t i = 0; i < work.size(); ++i) {
        const Identity& owner = *work[i].owner;
        RollbackUnitFailureScope shotScope("shot.restore", owner.object, owner.id, frame);
        StageShotGenerationStorage(owner, *work[i].shot, bit);
        LIFE_REQUIRE(StageShotCpuFields(owner, *work[i].shot, bit) == ShotCpuRestoreStatus::ReviewedSubset);
        RestoreShotTail(static_cast<uint8_t*>(owner.object), work[i].image, owner.extent, holes[i]);
        // Joint-dependency rebuild request; the joints, depth order and model
        // flags it describes came back with the pose spans above.
        Write(owner.object, 0x123D, Read<uint8_t>(work[i].image, 0x123D));
    }
    // Constraint links are plain bytes among retained objects: the saved nodes,
    // the shots' joints (pose spans) and character joints (rollback regions) all
    // return to the same frame, so the chains agree without relinking or sorting.
    for (const auto& node : saved->constraints) {
        std::memcpy(static_cast<uint8_t*>(node.object) + 8, node.image + 8, 8);
        Write(node.object, 0x28, Read<float>(node.image, 0x28));
        std::memcpy(static_cast<uint8_t*>(node.object) + 0x48, node.image + 0x48, 0x42);
    }
    // No saved chain references a later inline constraint any more; clear its own
    // links so a native unlink cannot write through them into restored neighbours.
    for (void* constraint : future) {
        Write(constraint, 0x50, (void*)nullptr); Write(constraint, 0x58, (void*)nullptr);
        Write(constraint, 0x60, (void*)nullptr); Write(constraint, 0x70, int32_t(-1));
    }
    StageShotManagerState(*saved, bit);
    StageShotExecutionState(*saved, bit);
    AcquireSRWLockExclusive(&g_lock);
    Push("shots_restored", nullptr, nullptr, uint32_t(frame), work.size(), saved->constraints.size(), future.size(), false);
    ReleaseSRWLockExclusive(&g_lock);
}

namespace {
