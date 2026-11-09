// Included in the lifetime recorder's private namespace.
// Constraint construction identifies embedded constraints without a shot whitelist.
std::unordered_map<void*, bool> g_constructedConstraints;
void* (__fastcall* oConstructConstraint)(void*) = nullptr;
void (__fastcall* oDestroyConstraint)(void*) = nullptr;

void* __fastcall ConstructConstraint(void* object)
{
    void* result = oConstructConstraint(object);
    if (g_lifetimeEnabled.load(std::memory_order_relaxed)) {
        AcquireSRWLockExclusive(&g_lock);
        if (result) g_constructedConstraints[result] = true;
        ReleaseSRWLockExclusive(&g_lock);
    }
    return result;
}

void __fastcall DestroyConstraint(void* object)
{
    AcquireSRWLockExclusive(&g_lock);
    g_constructedConstraints.erase(object);
    ReleaseSRWLockExclusive(&g_lock);
    oDestroyConstraint(object);
}

template<class Visit> void VisitCollisionLists(void* collider, Visit visit)
{
    const uint32_t count = Read<uint32_t>(collider, 0x10);
    if (count > 64) std::abort();
    void* categories = Read<void*>(collider, 0x20);
    if (count && !categories) std::abort();
    for (uint32_t i = 0; i < count; ++i) {
        void* category = Read<void*>(categories, size_t(i) * 8);
        if (!category) continue;
        const uint32_t passes = Read<uint32_t>(category, 8);
        void* lists = Read<void*>(category, 0x18);
        if (passes > 1024 || (passes && !lists)) std::abort();
        for (uint32_t pass = 0; pass < passes; ++pass)
            if (void* list = Read<void*>(lists, size_t(pass) * 8)) visit(list);
    }
}

void DetachTransientShotCollisions(std::vector<void*> roots)
{
    if (roots.empty()) return;
    void* collision = Read<void*>((void*)_addr(0x140E18250), 0);
    if (!collision) return;
    void* collider = static_cast<uint8_t*>(collision) + 0x7368;
    std::sort(roots.begin(), roots.end(), std::less<void*>{});
    roots.erase(std::unique(roots.begin(), roots.end()), roots.end());
    // The transient rebuild contract must not destroy or silently ignore a
    // continuous registration. Validate every live/pending bank before mutation.
    VisitCollisionLists(collider, [&](void* list) {
        auto check = [&](size_t array, size_t rootField) {
            const uint32_t count = Read<uint32_t>(list, array + 8);
            void* data = Read<void*>(list, array + 0x18);
            if (count > 1048576 || (count && !data)) std::abort();
            for (uint32_t i = 0; i < count; ++i) {
                void* record = Read<void*>(data, size_t(i) * 8);
                if (!record) std::abort();
                void* root = Read<void*>(record, rootField);
                if (std::binary_search(roots.begin(), roots.end(), root, std::less<void*>{})) {
                    HookLog("LifetimeTrace: cannot withdraw shot root=%p with continuous collision registration list=%p array=0x%zX\n",
                            root, list, array);
                    std::abort();
                }
            }
        };
        check(0x24A8, 8); check(0x24C8, 0x10);
        for (size_t bank = 0; bank < 41; ++bank) {
            check(0x1028 + bank * 0x20, 8); check(0x1548 + bank * 0x20, 0x10);
            check(0x1A68 + bank * 0x20, 8); check(0x1F88 + bank * 0x20, 0x10);
        }
    });
    using Native = void (__fastcall*)(void*);
    ((Native)_addr(0x1405B7F20))(collider);
    VisitCollisionLists(collider, [&](void* list) {
        ((Native)_addr(0x1405B5B60))(list);
        const uint32_t count = Read<uint32_t>(list, 0xFA0);
        void* groups = Read<void*>(list, 0xFB0);
        if (count > 1048576 || (count && !groups)) std::abort();
        for (uint32_t i = 0; i < count; ++i)
            ((Native)_addr(0x1405A45C0))(Read<void*>(groups, size_t(i) * 8));
    });
    // Leave the completed-pass cursor at zero. Native postMove publishes each
    // pass only after rebuilding bounds/groups, before testing or callbacks.
}

void AppendShotRoots(const Identity& shot, std::vector<void*>& roots)
{
    for (size_t field = 0x2058; field < 0x2078; field += 8)
        if (void* root = Read<void*>(shot.object, field)) roots.push_back(root);
    for (const auto& generation : g_shotGenerations)
        if (generation.id && generation.ownerId == shot.id && generation.attack &&
            generation.attack->root->object) roots.push_back(generation.attack->root->object);
}

void FenceRetiredShotCollisions()
{
    std::vector<void*> roots;
    AcquireSRWLockExclusive(&g_lock);
    for (const auto& generation : g_shotGenerations)
        if (generation.id && !generation.attached && !generation.snapshots && generation.attack &&
            generation.attack->root->object) roots.push_back(generation.attack->root->object);
    ReleaseSRWLockExclusive(&g_lock);
    DetachTransientShotCollisions(std::move(roots));
}

// Caller holds the registry lock. Unsupported effect graphs are not snapshot
// payloads, but their live typed unit references still prohibit physical free.
bool HasLiveUnitReference(void* object)
{
    for (const auto& unit : g_registry) {
        if (unit.object == object || (unit.occupancy != Occupancy::Live &&
            unit.occupancy != Occupancy::Quarantined)) continue;
        if (unit.kind == Kind::Effect)
            for (size_t field : {size_t(0x40), size_t(0x138), size_t(0x1F0),
                                 size_t(0x278), size_t(0x280), size_t(0x288)})
                if (Read<void*>(unit.object, field) == object) return true;
        if (unit.kind == Kind::Shot && unit.extent >= 0x2140) {
            for (size_t field : {size_t(0x40), size_t(0x1308), size_t(0x1E38), size_t(0x1E40)})
                if (Read<void*>(unit.object, field) == object) return true;
            for (size_t field = 0x2000; field < 0x2038; field += 8)
                if (Read<void*>(unit.object, field) == object) return true;
        }
    }
    return false;
}

bool HasCharacterShotReference(void* units, void* object)
{
    if (!units || Read<uint32_t>(units, 0xC38) <= 8) return false;
    for (void* unit = Read<void*>(units, 0x50 + 8 * 0x30); unit; unit = Read<void*>(unit, 0x18)) {
        // Category 8 also contains small helpers. Establish the native type
        // before reading character-only reference fields; never scan raw words.
        const uint64_t dti = GetObjectDti(uintptr_t(unit));
        bool character = false;
        uint64_t parent = dti;
        for (unsigned depth = 0; parent && depth < 128; ++depth) {
            if (parent == _addr(0x140D44A30)) { character = true; break; }
            parent = Read<uint64_t>((void*)parent, 0x20);
        }
        if (!character) continue;
        if (Read<void*>(unit, 0x6428) == object) return true;
        const uint64_t preferredDti = dti - _addr(0x140000000) + 0x140000000;
        for (uint32_t field : Rollback_CharacterShotTailOffsets(preferredDti))
            if (Read<void*>(unit, field) == object) return true;
    }
    return false;
}

void FenceFutureShotCollection()
{
    std::vector<Identity> candidates;
    std::vector<void*> roots;
    AcquireSRWLockExclusive(&g_lock);
    for (const auto& shot : g_registry)
        if (shot.kind == Kind::Shot && shot.futureShot && shot.occupancy == Occupancy::Quarantined &&
            !shot.snapshots && !HasRetiredShotGenerations(shot.id) && !HasLiveUnitReference(shot.object))
            candidates.push_back(shot);
    ReleaseSRWLockExclusive(&g_lock);
    void* units = Read<void*>((void*)_addr(0x140E17698), 0);
    for (const auto& shot : candidates)
        if (!HasCharacterShotReference(units, shot.object)) AppendShotRoots(shot, roots);
    DetachTransientShotCollisions(std::move(roots));
}

void WithdrawFutureShot(const Identity& shot, void* manager, void* units)
{
    // Do not run native kill: it can dispatch children, stop restored sound, or
    // mutate the restored character. Detach registration while storage is intact.
    std::vector<void*> constraints;
    AcquireSRWLockExclusive(&g_lock);
    for (const auto& entry : g_constructedConstraints) {
        const uintptr_t relative = uintptr_t(entry.first) - uintptr_t(shot.object);
        if (relative < shot.extent) constraints.push_back(entry.first);
    }
    ReleaseSRWLockExclusive(&g_lock);
    for (void* constraint : constraints) {
        ((bool (__fastcall*)(void*))_addr(0x1405EC190))(constraint);
        if (Read<uint64_t>(constraint, 0x30)) oRemoveGroups(units, constraint);
        if (((Read<uint32_t>(constraint, 0x10) >> 3) & 0x7F) != 0x7F) oRemoveUnit(units, constraint);
        Write(constraint, 0x10, (Read<uint32_t>(constraint, 0x10) & ~0x3FFu) | 0x3F8u);
    }
    if (Read<uint64_t>(shot.object, 0x30)) oRemoveGroups(units, shot.object);
    if (((Read<uint32_t>(shot.object, 0x10) >> 3) & 0x7F) != 0x7F) oRemoveUnit(units, shot.object);
    if (shot.shotLinked) oUnlinkShot(manager, Read<uint32_t>(shot.object, 0x1F54), shot.object);
    oReleaseShotId(manager, Read<uint32_t>(shot.object, 0x1F50));
    Write(shot.object, 0x1458, uintptr_t(0)); Write(shot.object, 0x1460, uintptr_t(0));
    Write(shot.object, 0x10, (Read<uint32_t>(shot.object, 0x10) & ~0x3FFu) | 0x3F8u);
    for (size_t field = 0x2058; field < 0x2078; field += 8)
        if (void* root = Read<void*>(shot.object, field)) Write(root, 8, uint8_t(0));
    AcquireSRWLockExclusive(&g_lock);
    Identity* current = Find(shot.object, false);
    if (!current || current->id != shot.id || current->occupancy != Occupancy::Live) std::abort();
    current->occupancy = Occupancy::Quarantined;
    current->shotLinked = false; current->shotRegistrationReleased = true; current->futureShot = true;
    ++g_quarantined;
    Push("quarantine_hold", current, shot.object, current->snapshots, g_quarantined, 0, 1, false);
    Push("future_shot_withdrawn", current, shot.object, Read<uint32_t>(shot.object, 0x1F50), 0, 0, 0, false);
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace

void LifetimeTrace_RetireFutureShots(int32_t frame)
{
    if constexpr (!LIFETIME_TRACE_ENABLED || !EMPTY_EFFECT_QUARANTINE_ENABLED || !EFFECT_RESTORE_ENABLED) return;
    if (!g_lifetimeEnabled.load(std::memory_order_relaxed)) return;
    UnitGuard fence;
    const unsigned slot = unsigned((frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    std::vector<Identity> future;
    std::vector<void*> roots;
    AcquireSRWLockExclusive(&g_lock);
    const auto* saved = g_liveSnapshots[slot];
    if (!saved || !saved->valid || saved->frame != frame || !fence.manager ||
        saved->unitManager != fence.manager) { ReleaseSRWLockExclusive(&g_lock); return; }
    void* manager = Read<void*>((void*)_addr(0x140D47F40), 0);
    if (saved->shotManager != manager) std::abort();
    for (auto& shot : g_registry)
        if (shot.kind == Kind::Shot && (shot.occupancy == Occupancy::Live ||
            shot.occupancy == Occupancy::Quarantined) && shot.id > saved->birthWatermark) {
            if (!manager || shot.extent < 0x2140 || !shot.deleting) std::abort();
            future.push_back(shot);
            if (shot.occupancy == Occupancy::Live) AppendShotRoots(shot, roots);
            else shot.futureShot = true;
        }
    ReleaseSRWLockExclusive(&g_lock);
    if (!manager) return;
    void* config = Read<void*>((void*)_addr(0x140D50F88), 0);
    if (config != saved->shotConfig) std::abort();
    DetachTransientShotCollisions(std::move(roots));
    for (const auto& shot : future)
        if (shot.occupancy == Occupancy::Live) WithdrawFutureShot(shot, manager, fence.manager);
    // The pending queue stores {unit*, line} pairs, not just pointers. Preserve
    // foreign entries and ordering; abandoned entries must not be published later.
    const uint32_t count = Read<uint32_t>(manager, 0x2448);
    if (count > 512) std::abort();
    uint32_t kept = 0;
    for (uint32_t i = 0; i < count; ++i) {
        auto* entry = static_cast<uint8_t*>(manager) + 0x448 + size_t(i) * 0x10;
        const void* unit = Read<void*>(entry, 0);
        if (std::any_of(future.begin(), future.end(), [&](const auto& shot) { return shot.object == unit; })) continue;
        std::memmove(static_cast<uint8_t*>(manager) + 0x448 + size_t(kept++) * 0x10, entry, 0x10);
    }
    Write(manager, 0x2448, kept);
    // Occupancy is released per exact abandoned allocation. Do not overwrite the
    // live ID table with a historical table until saved-shot resurrection exists.
    Write(manager, 0x438, saved->shotCursor);
    if (config) Write(config, 0x4A89C, saved->shotSlotFlags);
}

namespace {
