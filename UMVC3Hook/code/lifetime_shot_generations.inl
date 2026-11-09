// Included in lifetime_trace's private namespace. A snapshot leases ownership
// domains, not individual allocations. Unchanged children inherit their native
// owner's lifetime; shared_ptr below only joins cohorts across COW transitions.
enum class ShotDomain : uint8_t {
    Pose, Work, Keys, ModelResource, ShotResource,
    Motion0, Motion1, Motion2, Motion3, Motion4, Motion5, Motion6, Motion7,
    Motion8, Motion9, Motion10, Motion11, Motion12, Motion13, Motion14, Motion15,
    Command0, Command1, Command2, Command3, Attack0, Attack1, Attack2, Attack3, Count
};
constexpr size_t ShotDomainCount = size_t(ShotDomain::Count);
bool BorrowedShotResourceDomain(ShotDomain domain)
{
    return domain >= ShotDomain::Motion0 && domain <= ShotDomain::Command3;
}
bool ShotResourceDomain(ShotDomain domain)
{
    return domain == ShotDomain::ModelResource || domain == ShotDomain::ShotResource ||
        BorrowedShotResourceDomain(domain);
}
size_t ShotResourceField(ShotDomain domain)
{
    if (domain == ShotDomain::ModelResource) return 0x100;
    if (domain == ShotDomain::ShotResource) return 0x1F68;
    if (domain >= ShotDomain::Motion0 && domain <= ShotDomain::Motion15)
        return 0x11B8 + (size_t(domain) - size_t(ShotDomain::Motion0)) * 8;
    if (domain >= ShotDomain::Command0 && domain <= ShotDomain::Command3)
        return 0x13E8 + (size_t(domain) - size_t(ShotDomain::Command0)) * 8;
    std::abort();
}
struct NativeAttackObject {
    void* object = nullptr;
    Deleting deleting = nullptr;
    bool nativeOwner = true;
};
struct NativeAttackArray {
    void* data = nullptr;
    bool nativeOwner = true;
};
struct AttackEntry {
    std::shared_ptr<NativeAttackObject> wrapper, primitive;
};
struct AttackCohort {
    std::shared_ptr<NativeAttackObject> root;
    std::shared_ptr<NativeAttackArray> array;
    std::vector<AttackEntry> entries;
};
struct ShotGeneration {
    uint64_t id = 0, ownerId = 0;
    void* owner = nullptr;
    ShotDomain domain = ShotDomain::Pose;
    uint32_t snapshots = 0;
    bool attached = false;
    uint8_t image[0x1450]{};
    uintptr_t resource = 0;
    size_t keyframeBytes = 0;
    uintptr_t jointVtable = 0;
    std::shared_ptr<AttackCohort> attack;
};
struct ShotDomains {
    void* owner = nullptr;
    std::array<int, ShotDomainCount> current;
    std::shared_ptr<NativeAttackObject> roots[4];
    ShotDomains() { current.fill(-1); }
};
std::vector<ShotGeneration> g_shotGenerations;
std::unordered_map<uint64_t, ShotDomains> g_shotDomains;
std::unordered_map<void*, std::pair<uint64_t, unsigned>> g_attackOwners;
struct KeyframeExtent { void* data; size_t bytes; };
std::unordered_map<void*, KeyframeExtent> g_keyframeExtents;
uint64_t g_nextShotGeneration = 0;
thread_local bool g_collectingShotGeneration = false;
thread_local void* g_transferredShotResource = nullptr;
void (__fastcall* oGenerationModel)(void*, void*) = nullptr;
void (__fastcall* oGenerationBlend)(void*, uint32_t) = nullptr;
void (__fastcall* oSetShotMotionList)(void*, void*, uint32_t) = nullptr;
void (__fastcall* oSetShotCommandResource)(void*, void*, uint32_t) = nullptr;
void (__fastcall* oGenerationBaseModel)(void*, void*) = nullptr;
void (__fastcall* oGenerationResourceRelease)(void*) = nullptr;
void (__fastcall* oGenerationExtendArray)(void*, uint32_t) = nullptr;
uint32_t (__fastcall* oGenerationAddChild)(void*, void*, uint8_t) = nullptr;
void (__fastcall* collectModelGeneration)(void*) = nullptr;
Deleting logicallyRetireJoints = nullptr;

size_t KeyframeCountBytes(void* owner)
{
    const size_t keys = Read<uint32_t>(owner, 0x1238), blends = Read<uint32_t>(owner, 0x554);
    return keys <= 65536 && blends <= 8 ? keys * blends * 0xC : 0;
}
void RememberKeyframeExtent(void* owner)
{
    void* data = Read<void*>(owner, 0x6D8);
    AcquireSRWLockExclusive(&g_lock);
    auto at = g_keyframeExtents.find(owner);
    if (data && (at == g_keyframeExtents.end() || at->second.data != data))
        g_keyframeExtents[owner] = {data, KeyframeCountBytes(owner)};
    ReleaseSRWLockExclusive(&g_lock);
}
size_t ShotKeyframeBytes(const Identity& shot)
{
    void* data = Read<void*>(shot.object, 0x6D8);
    if (!data) return 0;
    AcquireSRWLockExclusive(&g_lock);
    size_t result = KeyframeCountBytes(shot.object);
    const auto at = g_keyframeExtents.find(shot.object);
    if (at != g_keyframeExtents.end() && at->second.data == data) result = at->second.bytes;
    else {
        const auto owner = g_shotDomains.find(shot.id);
        if (owner != g_shotDomains.end()) {
            const int index = owner->second.current[size_t(ShotDomain::Keys)];
            if (index >= 0 && Read<void*>(g_shotGenerations[index].image, 0x6D8) == data)
                result = g_shotGenerations[index].keyframeBytes;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

template<class Visit> void VisitDomainFields(ShotDomain domain, Visit visit)
{
    if (domain == ShotDomain::Pose) {
        for (size_t field : {size_t(0x538), size_t(0x548), size_t(0x11B0)}) visit(field, size_t(8));
        visit(0x116C, 4);
    } else if (domain == ShotDomain::Work) {
        for (size_t i = 0; i < 8; ++i) visit(0x6C0 + i * 0x180, size_t(8));
    } else if (domain == ShotDomain::Keys) {
        // Only slot zero owns the slab; the other seven pointers alias it.
        for (size_t i = 0; i < 8; ++i) visit(0x6D8 + i * 0x180, size_t(8));
    }
}

int NewShotGeneration(const Identity& owner, ShotDomain domain)
{
    const size_t keyframeBytes = domain == ShotDomain::Keys ? ShotKeyframeBytes(owner) : 0;
    size_t index = 0;
    while (index < g_shotGenerations.size() && g_shotGenerations[index].id) ++index;
    if (index == g_shotGenerations.size()) g_shotGenerations.emplace_back();
    auto& generation = g_shotGenerations[index];
    generation.id = ++g_nextShotGeneration;
    generation.ownerId = owner.id; generation.owner = owner.object;
    generation.domain = domain; generation.snapshots = 0; generation.attached = true;
    if (domain <= ShotDomain::Keys) std::memcpy(generation.image, owner.object, sizeof(generation.image));
    else if (ShotResourceDomain(domain))
        generation.resource = Read<uintptr_t>(owner.object, ShotResourceField(domain));
    auto& domains = g_shotDomains[owner.id];
    domains.owner = owner.object;
    domains.current[size_t(domain)] = int(index);
    if (domain == ShotDomain::Keys) generation.keyframeBytes = keyframeBytes;
    if (domain == ShotDomain::Pose) {
        const auto* joints = Read<uint8_t*>(owner.object, 0x538);
        generation.jointVtable = joints && Read<uint32_t>(joints - 0x10, 0) ? Read<uintptr_t>(joints, 0) : 0;
    }
    return int(index);
}

std::shared_ptr<NativeAttackObject> AttackObject(void* object)
{
    if (!object) return {};
    auto result = std::make_shared<NativeAttackObject>();
    result->object = object;
    result->deleting = (Deleting)Read<void*>(Read<void*>(object, 0), 0);
    return result;
}

std::shared_ptr<AttackCohort> ReadAttackCohort(const Identity& owner, unsigned slot,
                                            const std::shared_ptr<AttackCohort>& previous = {})
{
    void* root = Read<void*>(owner.object, 0x2058 + slot * 8);
    if (!root) return {};
    if (Read<uint8_t>(root, 0xC0)) std::abort(); // Normal shot nodes borrow their callback owner.
    auto& domains = g_shotDomains[owner.id];
    if (!domains.roots[slot] || domains.roots[slot]->object != root) domains.roots[slot] = AttackObject(root);
    g_attackOwners[root] = {owner.id, slot};
    auto cohort = std::make_shared<AttackCohort>();
    cohort->root = domains.roots[slot];
    const size_t count = Read<uint32_t>(root, 0x20), capacity = Read<uint32_t>(root, 0x24);
    void* array = Read<void*>(root, 0x30);
    if (count > capacity || capacity > 65536 || (capacity && !array)) std::abort();
    cohort->array = previous && previous->array->data == array ? previous->array : std::make_shared<NativeAttackArray>();
    cohort->array->data = array;
    for (size_t i = 0; i < count; ++i) {
        void* wrapper = Read<void*>(array, i * 8);
        if (!wrapper) { cohort->entries.emplace_back(); continue; }
        // These are the native shot graph's ownership rules, not a type whitelist.
        if (Read<uint8_t>(wrapper, 0x18) || Read<uint8_t>(wrapper, 0x50)) std::abort();
        void* primitive = Read<void*>(wrapper, 0x10);
        AttackEntry entry;
        if (previous && i < previous->entries.size()) {
            const auto& old = previous->entries[i];
            if (old.wrapper && old.wrapper->object == wrapper) entry.wrapper = old.wrapper;
            if (primitive && old.primitive && old.primitive->object == primitive) entry.primitive = old.primitive;
        }
        if (previous && (!entry.wrapper || (primitive && !entry.primitive)))
            for (const auto& old : previous->entries) {
                if (!entry.wrapper && old.wrapper && old.wrapper->object == wrapper) entry.wrapper = old.wrapper;
                if (!entry.primitive && primitive && old.primitive && old.primitive->object == primitive) entry.primitive = old.primitive;
            }
        if (!entry.wrapper) entry.wrapper = AttackObject(wrapper);
        if (primitive && !entry.primitive) entry.primitive = AttackObject(primitive);
        cohort->entries.push_back(std::move(entry));
    }
    return cohort;
}

void ValidateAttackCohort(const AttackCohort& cohort)
{
    void* root = cohort.root->object;
    if (Read<void*>(root, 0x30) != cohort.array->data || Read<uint32_t>(root, 0x20) != cohort.entries.size())
        std::abort();
    for (size_t i = 0; i < cohort.entries.size(); ++i) {
        const auto& entry = cohort.entries[i];
        void* wrapper = Read<void*>(cohort.array->data, i * 8);
        if (wrapper != (entry.wrapper ? entry.wrapper->object : nullptr)) std::abort();
        if (wrapper && (Read<uint8_t>(wrapper, 0x18) || Read<uint8_t>(wrapper, 0x50))) std::abort();
        if (wrapper && Read<void*>(wrapper, 0x10) != (entry.primitive ? entry.primitive->object : nullptr))
            std::abort(); // An unsupported ownership-changing entry point bypassed COW.
    }
}

// Called at the same main-thread/unit fence as capture and native mutation.
int EnsureShotGeneration(const Identity& owner, ShotDomain domain)
{
    auto& domains = g_shotDomains[owner.id];
    int index = domains.current[size_t(domain)];
    if (index >= 0 && domain >= ShotDomain::Attack0) {
        void* root = Read<void*>(owner.object, 0x2058 + (unsigned(domain) - unsigned(ShotDomain::Attack0)) * 8);
        auto& old = g_shotGenerations[index];
        if (old.attack && old.attack->root->object != root) std::abort();
        if (!old.attack && root) {
            old.attached = false;
            if (!old.snapshots) old.id = 0;
            domains.current[size_t(domain)] = index = -1;
        }
    }
    if (index >= 0) return index;
    index = NewShotGeneration(owner, domain);
    if (domain >= ShotDomain::Attack0) {
        auto cohort = ReadAttackCohort(owner, unsigned(domain) - unsigned(ShotDomain::Attack0));
        g_shotGenerations[index].attack = std::move(cohort);
    }
    return index;
}

// Retire only the fields the native operation is about to destroy. The other
// domains, including same-count joints, remain on the live owner unchanged.
void RetireModelDomain(const Identity& owner, ShotDomain domain, bool force = false)
{
    auto at = g_shotDomains.find(owner.id);
    if (at == g_shotDomains.end()) return;
    int& current = at->second.current[size_t(domain)];
    if (current < 0) {
        if (!force) return;
        EnsureShotGeneration(owner, domain);
    }
    auto& generation = g_shotGenerations[current];
    current = -1;
    if (!generation.snapshots && !force) { generation.id = 0; return; }
    VisitDomainFields(domain, [&](size_t field, size_t size) {
        const uint64_t expected = size == 4 ? Read<uint32_t>(generation.image, field) : Read<uint64_t>(generation.image, field);
        const uint64_t actual = size == 4 ? Read<uint32_t>(owner.object, field) : Read<uint64_t>(owner.object, field);
        if (actual != expected) std::abort(); // A destructive ownership boundary was missed.
        if (size == 4) Write(owner.object, field, UINT32_MAX);
        else Write(owner.object, field, uintptr_t(0));
    });
    generation.attached = false;
    if (domain == ShotDomain::Pose) {
        // Match native logical retirement of the old joints without freeing
        // their vector allocation. In particular old constraints must not dirty
        // the replacement model through an obsolete joint->model backpointer.
        void* joints = Read<void*>(generation.image, 0x538);
        if (joints) logicallyRetireJoints(joints, 2);
    }
}

Identity* ShotIdentity(void* owner)
{
    if constexpr (!EMPTY_EFFECT_QUARANTINE_ENABLED) return nullptr;
    if (!g_lifetimeEnabled.load(std::memory_order_relaxed)) return nullptr;
    auto* identity = Find(owner, false);
    return identity && identity->kind == Kind::Shot && identity->occupancy == Occupancy::Live ? identity : nullptr;
}

void TrackModelAcquisitions(void* owner)
{
    AcquireSRWLockExclusive(&g_lock);
    if (auto* identity = ShotIdentity(owner)) {
        auto at = g_shotDomains.find(identity->id);
        if (at != g_shotDomains.end()) for (size_t domain = 0; domain <= size_t(ShotDomain::Keys); ++domain) {
            int& index = at->second.current[domain];
            if (index < 0) continue;
            auto& generation = g_shotGenerations[index];
            bool changed = false, empty = true;
            VisitDomainFields(ShotDomain(domain), [&](size_t field, size_t size) {
                const uint64_t old = size == 4 ? Read<uint32_t>(generation.image, field) : Read<uint64_t>(generation.image, field);
                const uint64_t live = size == 4 ? Read<uint32_t>(owner, field) : Read<uint64_t>(owner, field);
                changed |= old != live;
                empty &= size == 4 ? old == UINT32_MAX : old == 0;
            });
            if (!changed) continue;
            if (!empty) std::abort(); // Nonempty replacement must retire at its destructive entry.
            generation.attached = false;
            if (!generation.snapshots) generation.id = 0;
            index = -1;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
}

void __fastcall GenerationModel(void* owner, void* resource)
{
    UnitGuard fence;
    RememberKeyframeExtent(owner);
    void* oldKeys = Read<void*>(owner, 0x6D8);
    const size_t allocationBytes = KeyframeCountBytes(owner);
    Identity retiring;
    bool replaces = false;
    AcquireSRWLockExclusive(&g_lock);
    if (!g_collectingShotGeneration) if (auto* identity = ShotIdentity(owner)) {
        if (Read<void*>(owner, 0x100) != resource &&
            (!resource || Read<uint32_t>(resource, 0x78) != Read<uint32_t>(owner, 0x530))) {
            retiring = *identity; replaces = true;
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (replaces) {
        RetireModelDomain(retiring, ShotDomain::Pose);
        RetireModelDomain(retiring, ShotDomain::Work);
    }
    oGenerationModel(owner, resource);
    if (!oldKeys && Read<void*>(owner, 0x6D8)) {
        AcquireSRWLockExclusive(&g_lock);
        g_keyframeExtents[owner] = {Read<void*>(owner, 0x6D8), allocationBytes};
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (!g_collectingShotGeneration) TrackModelAcquisitions(owner);
}

void __fastcall GenerationBlend(void* owner, uint32_t count)
{
    UnitGuard fence;
    RememberKeyframeExtent(owner);
    const bool reallocates = Read<void*>(owner, 0x100) && Read<uint32_t>(owner, 0x530) &&
        Read<uint32_t>(owner, 0x554) != std::min(count, uint32_t(16));
    AcquireSRWLockExclusive(&g_lock);
    if (!g_collectingShotGeneration) if (auto* identity = ShotIdentity(owner)) {
        if (Read<void*>(owner, 0x100) && Read<uint32_t>(owner, 0x530) &&
            Read<uint32_t>(owner, 0x554) != std::min(count, uint32_t(16))) {
            if (Read<uint32_t>(owner, 0x554) > 8 || count > 8) std::abort();
            RetireModelDomain(*identity, ShotDomain::Work);
            RetireModelDomain(*identity, ShotDomain::Keys);
        }
    }
    ReleaseSRWLockExclusive(&g_lock);
    oGenerationBlend(owner, count);
    if (reallocates) {
        AcquireSRWLockExclusive(&g_lock);
        g_keyframeExtents[owner] = {Read<void*>(owner, 0x6D8), KeyframeCountBytes(owner)};
        ReleaseSRWLockExclusive(&g_lock);
    }
    if (!g_collectingShotGeneration) TrackModelAcquisitions(owner);
}

struct GenerationResourceMove {
    void* previous = g_transferredShotResource;
    GenerationResourceMove(void* owner, ShotDomain domain, bool changes) {
        g_transferredShotResource = nullptr;
        AcquireSRWLockExclusive(&g_lock);
        if (changes && !g_collectingShotGeneration) if (auto* identity = ShotIdentity(owner)) {
            auto at = g_shotDomains.find(identity->id);
            if (at != g_shotDomains.end()) {
                int& current = at->second.current[size_t(domain)];
                // A live interpreter can still borrow an earlier binding even
                // after the owned slot changes. Track intermediate acquisitions
                // too, including setters called twice between snapshots.
                if (current < 0 && BorrowedShotResourceDomain(domain)) EnsureShotGeneration(*identity, domain);
                if (current >= 0) {
                    auto& generation = g_shotGenerations[current];
                    if (generation.resource != Read<uintptr_t>(owner, ShotResourceField(domain))) std::abort();
                    current = -1;
                    if (generation.snapshots || (generation.resource && BorrowedShotResourceDomain(domain))) {
                        generation.attached = false;
                        g_transferredShotResource = (void*)generation.resource;
                    } else generation.id = 0;
                }
            }
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    ~GenerationResourceMove() { g_transferredShotResource = previous; }
};

void __fastcall GenerationResourceRelease(void* resource)
{
    if (!g_collectingShotGeneration && resource && resource == g_transferredShotResource) {
        g_transferredShotResource = nullptr;
        return; // Transfer this existing reference, not a new snapshot addRef.
    }
    oGenerationResourceRelease(resource);
}
void __fastcall GenerationBaseModel(void* owner, void* resource)
{
    // Keep +100 intact until native equality/null handling and its old-reference
    // release. Clearing it early would skip the null-input reset branch.
    GenerationResourceMove transfer(owner, ShotDomain::ModelResource, Read<void*>(owner, 0x100) != resource);
    oGenerationBaseModel(owner, resource);
}

void __fastcall GenerationMotionList(void* owner, void* resource, uint32_t slot)
{
    UnitGuard fence;
    if (slot >= 16) std::abort();
    GenerationResourceMove transfer(owner, ShotDomain(unsigned(ShotDomain::Motion0) + slot), true);
    oSetShotMotionList(owner, resource, slot);
}

void __fastcall GenerationCommandResource(void* owner, void* resource, uint32_t slot)
{
    UnitGuard fence;
    if (slot >= 4) std::abort();
    GenerationResourceMove transfer(owner, ShotDomain(unsigned(ShotDomain::Command0) + slot), true);
    oSetShotCommandResource(owner, resource, slot);
}

void* AllocateAttackArray(size_t capacity)
{
    if (!capacity) return nullptr;
    using Get = void* (__fastcall*)(void*);
    using Allocate = void* (__fastcall*)(void*, size_t, uint32_t);
    void* allocator = ((Get)_addr(0x14045D4F0))((void*)_addr(0x140E16870));
    void* array = ((Allocate)Read<void*>(Read<void*>(allocator, 0), 0x40))(allocator, capacity * 8, 0x10);
    if (!array) std::abort();
    std::memset(array, 0, capacity * 8);
    return array;
}

Identity* ManagedAttack(void* root, unsigned& slot)
{
    if (!g_lifetimeEnabled.load(std::memory_order_relaxed)) return nullptr;
    const auto at = g_attackOwners.find(root);
    if (at == g_attackOwners.end()) return nullptr;
    slot = at->second.second;
    const auto owner = g_shotDomains.find(at->second.first);
    if (owner == g_shotDomains.end()) return nullptr;
    auto* identity = Find(owner->second.owner, false);
    if (identity && identity->id == at->second.first && identity->occupancy == Occupancy::Live &&
        Read<void*>(identity->object, 0x2058 + slot * 8) == root) return identity;
    return nullptr;
}

void ReplaceAttackArray(const Identity& owner, unsigned slot, uint32_t capacity, bool releaseChildren)
{
    const auto domain = ShotDomain(unsigned(ShotDomain::Attack0) + slot);
    const int oldIndex = EnsureShotGeneration(owner, domain);
    auto old = g_shotGenerations[oldIndex].attack;
    if (!old) std::abort();
    ValidateAttackCohort(*old);
    void* root = old->root->object;
    const size_t count = Read<uint32_t>(root, 0x20);
    if (capacity < count) std::abort();
    void* array = AllocateAttackArray(capacity);
    if (count) std::memcpy(array, Read<void*>(root, 0x30), count * 8);
    old->array->nativeOwner = false;
    if (releaseChildren) for (size_t i = 0; i < old->entries.size(); ++i) {
        auto& entry = old->entries[i];
        if (!entry.primitive) continue; // Native partial release preserves null-primitive wrappers.
        entry.wrapper->nativeOwner = entry.primitive->nativeOwner = false;
        Write(array, i * 8, uintptr_t(0));
    }
    g_shotGenerations[oldIndex].attached = false;
    g_shotDomains[owner.id].current[size_t(domain)] = -1;
    Write(root, 0x24, capacity); Write(root, 0x30, array);
    const int next = NewShotGeneration(owner, domain);
    auto cohort = ReadAttackCohort(owner, slot, old);
    g_shotGenerations[next].attack = std::move(cohort);
}

bool RetireAttack(const Identity& owner, uint32_t requested, bool full)
{
    if (g_collectingShotGeneration || !g_lifetimeEnabled.load(std::memory_order_relaxed) ||
        !g_shotDomains.count(owner.id)) return false;
    for (unsigned slot = 0; slot < 4; ++slot) {
        if (int32_t(requested) >= 0 && requested != slot) continue;
        void* root = Read<void*>(owner.object, 0x2058 + slot * 8);
        if (!root) continue;
        const auto domain = ShotDomain(unsigned(ShotDomain::Attack0) + slot);
        const int index = EnsureShotGeneration(owner, domain);
        auto cohort = g_shotGenerations[index].attack;
        ValidateAttackCohort(*cohort);
        if (!full) {
            ReplaceAttackArray(owner, slot, Read<uint32_t>(root, 0x24), true);
            continue;
        }
        cohort->root->nativeOwner = cohort->array->nativeOwner = false;
        for (auto& entry : cohort->entries) {
            if (entry.wrapper) entry.wrapper->nativeOwner = false;
            if (entry.primitive) entry.primitive->nativeOwner = false;
        }
        g_shotGenerations[index].attached = false;
        auto& state = g_shotDomains[owner.id];
        state.current[size_t(domain)] = -1;
        state.roots[slot].reset(); // The generation still owns the original root address.
        Write(owner.object, 0x2058 + slot * 8, uintptr_t(0));
        Write(root, 0x20, uint32_t(0)); Write(root, 0x24, uint32_t(0)); Write(root, 0x30, uintptr_t(0));
    }
    return true;
}

void __fastcall GenerationExtendArray(void* array, uint32_t capacity)
{
    unsigned slot = 0;
    auto* root = static_cast<uint8_t*>(array) - 0x18;
    auto* owner = g_collectingShotGeneration ? nullptr : ManagedAttack(root, slot);
    if (!owner) { oGenerationExtendArray(array, capacity); return; }
    UnitGuard fence;
    ReplaceAttackArray(*owner, slot, capacity, false);
}

uint32_t __fastcall GenerationAddChild(void* root, void* geometry, uint8_t clone)
{
    unsigned slot = 0;
    auto* owner = g_collectingShotGeneration ? nullptr : ManagedAttack(root, slot);
    if (!owner) return oGenerationAddChild(root, geometry, clone);
    if (clone) std::abort(); // The normal rShot graph borrows its explicitly owned primitives.
    UnitGuard fence;
    const auto domain = ShotDomain(unsigned(ShotDomain::Attack0) + slot);
    const int index = EnsureShotGeneration(*owner, domain);
    if (g_shotGenerations[index].snapshots)
        ReplaceAttackArray(*owner, slot, Read<uint32_t>(root, 0x24), false);
    const uint32_t result = oGenerationAddChild(root, geometry, clone);
    const int current = g_shotDomains[owner->id].current[size_t(domain)];
    auto previous = g_shotGenerations[current].attack;
    g_shotGenerations[current].attack = ReadAttackCohort(*owner, slot, previous);
    return result;
}

void MaskModelOwnership(void* root)
{
    for (size_t field : {size_t(0x100), size_t(0x108), size_t(0x110), size_t(0x170), size_t(0x538),
                         size_t(0x548), size_t(0x11B0), size_t(0x1250), size_t(0x1258)}) Write(root, field, uintptr_t(0));
    Write(root, 0x118, uint32_t(0));
    Write(root, 0x116C, UINT32_MAX);
    for (size_t i = 0; i < 8; ++i) {
        Write(root, 0x6C0 + i * 0x180, uintptr_t(0));
        Write(root, 0x6D8 + i * 0x180, uintptr_t(0));
    }
    for (size_t i = 0; i < 16; ++i) {
        Write(root, 0x278 + i * 0x28, uintptr_t(0));
        Write(root, 0x11B8 + i * 8, uintptr_t(0));
    }
    for (size_t field = 0x12A0; field < 0x12C0; field += 8) Write(root, field, uintptr_t(0));
    for (size_t field = 0x13E8; field < 0x1440; field += 8) Write(root, field, uintptr_t(0));
}

void ReleaseRetiredAttackObject(std::shared_ptr<NativeAttackObject>& storage)
{
    if (storage && !storage->nativeOwner && storage.use_count() == 1 && storage->object) {
        void* object = storage->object;
        storage->object = nullptr;
        storage->deleting(object, 1);
    }
    storage.reset();
}

void ReleaseRetiredAttack(std::shared_ptr<AttackCohort>& cohort)
{
    if (!cohort) return;
    auto root = cohort->root;
    for (auto& entry : cohort->entries) {
        // Normal shot wrappers borrow primitives, so native wrapper teardown
        // precedes the separate primitive teardown, just as releaseAtkInfo does.
        ReleaseRetiredAttackObject(entry.wrapper);
        ReleaseRetiredAttackObject(entry.primitive);
    }
    cohort->entries.clear();
    auto& array = cohort->array;
    if (array && !array->nativeOwner && array.use_count() == 1 && array->data) {
        using Get = void* (__fastcall*)(void*);
        using Free = void (__fastcall*)(void*, void*);
        void* allocator = ((Get)_addr(0x14045D4F0))((void*)_addr(0x140E16870));
        ((Free)Read<void*>(Read<void*>(allocator, 0), 0x60))(allocator, array->data);
        array->data = nullptr;
    }
    array.reset();
    cohort.reset();
    if (root && !root->nativeOwner && root.use_count() == 1) g_attackOwners.erase(root->object);
    ReleaseRetiredAttackObject(root);
}

void CollectShotGenerations()
{
    UnitGuard fence;
    FenceRetiredShotCollisions();
    // Native child teardown completes before the resource references it borrows.
    for (bool resources : {false, true}) for (size_t i = 0; i < g_shotGenerations.size(); ++i) {
        AcquireSRWLockExclusive(&g_lock);
        auto& stored = g_shotGenerations[i];
        const bool reference = ShotResourceDomain(stored.domain);
        if (!stored.id || stored.attached || stored.snapshots || reference != resources) {
            ReleaseSRWLockExclusive(&g_lock); continue;
        }
        if (BorrowedShotResourceDomain(stored.domain)) {
            const auto* owner = Find(stored.owner, false);
            if (owner && owner->id == stored.ownerId && owner->occupancy != Occupancy::Tombstone) {
                ReleaseSRWLockExclusive(&g_lock); continue;
            }
        }
        // Move ownership out before dropping the lock. Native disposal is
        // explicit; C++ metadata destruction never invokes game code.
        ShotGeneration generation = std::move(stored);
        stored.id = 0;
        ReleaseSRWLockExclusive(&g_lock);
        const bool previous = g_collectingShotGeneration;
        g_collectingShotGeneration = true;
        if (generation.domain <= ShotDomain::Keys) {
            uint8_t live[0x1450];
            std::memcpy(live, generation.owner, sizeof(live));
            std::memcpy(generation.owner, generation.image, sizeof(generation.image));
            MaskModelOwnership(generation.owner);
            VisitDomainFields(generation.domain, [&](size_t field, size_t size) {
                std::memcpy(static_cast<uint8_t*>(generation.owner) + field, generation.image + field, size);
            });
            if (generation.domain == ShotDomain::Pose && generation.jointVtable)
                Write(Read<void*>(generation.owner, 0x538), 0, generation.jointVtable);
            collectModelGeneration(generation.owner);
            std::memcpy(generation.owner, live, sizeof(live));
        } else if (reference) {
            if (generation.resource) releaseResourceRef((void*)generation.resource);
        } else {
            ReleaseRetiredAttack(generation.attack);
        }
        g_collectingShotGeneration = previous;
    }
}

bool HasRetiredShotGenerations(uint64_t owner)
{
    return std::any_of(g_shotGenerations.begin(), g_shotGenerations.end(), [&](const auto& generation) {
        return generation.id && generation.ownerId == owner && !generation.attached &&
            (!BorrowedShotResourceDomain(generation.domain) || generation.snapshots);
    });
}

void ReleaseTerminalShotReferences(uint64_t owner)
{
    // Called after the most-derived native deleting dispatch returns. Concrete
    // and base cleanup can read old command/motion borrowers until then. The
    // resource references do not require the now-disposed owner address.
    for (size_t i = 0; i < g_shotGenerations.size(); ++i) {
        AcquireSRWLockExclusive(&g_lock);
        auto& generation = g_shotGenerations[i];
        if (!generation.id || generation.ownerId != owner || !BorrowedShotResourceDomain(generation.domain)) {
            ReleaseSRWLockExclusive(&g_lock); continue;
        }
        if (generation.attached || generation.snapshots) std::abort();
        void* resource = (void*)generation.resource;
        generation.id = 0;
        ReleaseSRWLockExclusive(&g_lock);
        const bool previous = g_collectingShotGeneration;
        g_collectingShotGeneration = true;
        if (resource) releaseResourceRef(resource);
        g_collectingShotGeneration = previous;
    }
}
void ForgetCurrentShotGenerations(uint64_t owner, void* address)
{
    // Called only after retired generations drain, before intact native teardown.
    // Current storage remains native-owned, so forgetting metadata frees no leaves.
    std::vector<std::shared_ptr<AttackCohort>> cohorts;
    ShotDomains domains;
    AcquireSRWLockExclusive(&g_lock);
    g_keyframeExtents.erase(address);
    auto at = g_shotDomains.find(owner);
    if (at != g_shotDomains.end()) {
        for (int index : at->second.current) if (index >= 0) {
            auto& generation = g_shotGenerations[index];
            if (generation.snapshots) std::abort();
            generation.id = 0;
            cohorts.push_back(std::move(generation.attack));
        }
        domains = std::move(at->second);
        for (const auto& root : domains.roots) if (root) g_attackOwners.erase(root->object);
        g_shotDomains.erase(at);
    }
    ReleaseSRWLockExclusive(&g_lock);
}
void DropShotGenerations(const LifetimeSnapshot* saved, uint32_t bit)
{
    if (!saved) return;
    for (const auto& shot : saved->shots) for (const auto& ref : shot.generations)
        if (ref.index < g_shotGenerations.size() && g_shotGenerations[ref.index].id == ref.id)
            g_shotGenerations[ref.index].snapshots &= ~bit;
}

void AppendGenerationSpan(ShotState& saved, const char* name, void* address, size_t size,
                          void* cell = nullptr, uint16_t group = UINT16_MAX, uint32_t element = UINT32_MAX)
{
    if (!address || !size) return;
    if (size > UINT32_MAX || saved.payload.size() > UINT32_MAX - size) std::abort();
    saved.spans.push_back({name, {address, uint32_t(size), uint32_t(saved.payload.size())}, uint64_t(cell), 0, group, element});
    const auto* bytes = static_cast<const uint8_t*>(address);
    saved.payload.insert(saved.payload.end(), bytes, bytes + size);
}
void CaptureShotGenerations(const Identity& owner, uint32_t bit, ShotState& saved)
{
    for (size_t domain = 0; domain < ShotDomainCount; ++domain) {
        const int index = EnsureShotGeneration(owner, ShotDomain(domain));
        auto& generation = g_shotGenerations[index];
        if (domain <= size_t(ShotDomain::Keys)) VisitDomainFields(ShotDomain(domain), [&](size_t field, size_t size) {
            const uint64_t expected = size == 4 ? Read<uint32_t>(generation.image, field) : Read<uint64_t>(generation.image, field);
            const uint64_t actual = size == 4 ? Read<uint32_t>(owner.object, field) : Read<uint64_t>(owner.object, field);
            if (expected != actual) std::abort();
        });
        if (ShotResourceDomain(ShotDomain(domain)))
            if (generation.resource != Read<uintptr_t>(owner.object, ShotResourceField(ShotDomain(domain)))) std::abort();
        if (domain >= size_t(ShotDomain::Attack0) && generation.attack) {
            // Appends can create children without growing the array. Refresh only
            // an unleased live cohort; leased membership changes use the add hook.
            if (!generation.snapshots) generation.attack = ReadAttackCohort(owner,
                unsigned(domain) - unsigned(ShotDomain::Attack0), generation.attack);
            auto cohort = generation.attack;
            ValidateAttackCohort(*cohort);
            void* root = cohort->root->object;
            const auto slot = uint16_t(domain - size_t(ShotDomain::Attack0));
            AppendGenerationSpan(saved, "collision.root", root, 0xE0, nullptr, slot);
            AppendGenerationSpan(saved, "collision.entries", cohort->array->data,
                size_t(Read<uint32_t>(root, 0x24)) * 8, static_cast<uint8_t*>(root) + 0x30, slot);
            for (size_t element = 0; element < cohort->entries.size(); ++element) {
                const auto& entry = cohort->entries[element];
                if (entry.wrapper) AppendGenerationSpan(saved, "collision.wrapper", entry.wrapper->object,
                    0x60, nullptr, slot, uint32_t(element));
                if (entry.primitive) {
                    const uint32_t type = Read<uint32_t>(entry.primitive->object, 8);
                    if (type != 5 && type != 6) std::abort();
                    AppendGenerationSpan(saved, "collision.primitive", entry.primitive->object,
                        type == 5 ? 0x70 : 0xB0, nullptr, slot, uint32_t(element));
                }
            }
        }
        generation.snapshots |= bit;
        saved.generations.push_back({size_t(index), generation.id});
    }
}

// Storage stage for a future coherent shot/world transaction. It does not
// publish shot IDs, scheduler membership or collider queues. Presentation owners
// and material/vjob payloads stay live; the saved body is not a memcpy contract.
// Callers must not resume simulation after this stage alone.
[[maybe_unused]] void StageShotGenerationStorage(const Identity& owner, const ShotState& saved, uint32_t bit)
{
    UnitGuard fence;
    if (saved.id != owner.id || !(owner.snapshots & bit) || saved.generations.size() != ShotDomainCount) std::abort();
    bool domains[ShotDomainCount]{};
    for (const auto& ref : saved.generations) {
        if (ref.index >= g_shotGenerations.size()) std::abort();
        const auto& generation = g_shotGenerations[ref.index];
        const size_t domain = size_t(generation.domain);
        if (generation.id != ref.id || generation.ownerId != owner.id || generation.owner != owner.object ||
            !(generation.snapshots & bit) || domain >= ShotDomainCount || domains[domain]) std::abort();
        domains[domain] = true;
    }
    bool bodyFound = false;
    for (const auto& span : saved.spans) {
        if (span.memory.offset > saved.payload.size() || span.memory.size > saved.payload.size() - span.memory.offset)
            std::abort();
        if (!std::strcmp(span.name, "body")) {
            if (bodyFound || span.memory.address != owner.object || span.memory.size < 0x2140) std::abort();
            bodyFound = true;
        }
    }
    if (!bodyFound) std::abort();
    for (const auto& ref : saved.generations) {
        const auto domain = g_shotGenerations[ref.index].domain;
        int current = g_shotDomains[owner.id].current[size_t(domain)];
        if (current != int(ref.index)) {
            if (domain <= ShotDomain::Keys) RetireModelDomain(owner, domain, true);
            else if (ShotResourceDomain(domain)) {
                current = EnsureShotGeneration(owner, domain);
                g_shotGenerations[current].attached = false;
                g_shotDomains[owner.id].current[size_t(domain)] = -1;
            } else {
                const auto slot = unsigned(domain) - unsigned(ShotDomain::Attack0);
                RetireAttack(owner, slot, true);
            }
        }
        auto& generation = g_shotGenerations[ref.index];
        generation.attached = true;
        g_shotDomains[owner.id].current[size_t(domain)] = int(ref.index);
        if (domain <= ShotDomain::Keys) {
            VisitDomainFields(domain, [&](size_t field, size_t size) {
                std::memcpy(static_cast<uint8_t*>(owner.object) + field, generation.image + field, size);
            });
        } else if (ShotResourceDomain(domain))
            Write(owner.object, ShotResourceField(domain), generation.resource);
        else {
            const auto slot = unsigned(domain) - unsigned(ShotDomain::Attack0);
            auto cohort = generation.attack;
            Write(owner.object, 0x2058 + slot * 8, cohort ? cohort->root->object : nullptr);
            g_shotDomains[owner.id].roots[slot] = cohort ? cohort->root : nullptr;
            if (cohort) {
                cohort->root->nativeOwner = cohort->array->nativeOwner = true;
                for (auto& entry : cohort->entries) {
                    if (entry.wrapper) entry.wrapper->nativeOwner = true;
                    if (entry.primitive) entry.primitive->nativeOwner = true;
                }
            }
        }
    }
    for (const auto& span : saved.spans) {
        if (!std::strcmp(span.name, "body")) {
            const auto* body = saved.payload.data() + span.memory.offset;
            // These counts/control fields describe the restored owned CPU buffers.
            for (size_t field : {size_t(0x530), size_t(0x550), size_t(0x554), size_t(0x558), size_t(0x1238)})
                Write(owner.object, field, Read<uint32_t>(body, field));
            Write(owner.object, 0x540, Read<uintptr_t>(body, 0x540));
        } else {
            if (span.memory.offset > saved.payload.size() || span.memory.size > saved.payload.size() - span.memory.offset)
                std::abort();
            std::memcpy(span.memory.address, saved.payload.data() + span.memory.offset, span.memory.size);
        }
    }
}
