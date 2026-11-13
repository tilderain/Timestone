// Included in the lifetime recorder's private namespace.
// The unit fence precedes the manager and global list locks. These list locks
// are separate globals, not fields in the sShot singleton.
struct ShotWorldGuard {
    void* manager = nullptr;
    ShotWorldGuard() {
        manager = Read<void*>((void*)_addr(0x140D47F40), 0);
        if (!manager) return;
        EnterCriticalSection((LPCRITICAL_SECTION)(static_cast<uint8_t*>(manager) + 8));
        for (size_t team = 0; team < 10; ++team)
            EnterCriticalSection((LPCRITICAL_SECTION)(_addr(0x140D48120) + team * 0x28));
    }
    ~ShotWorldGuard() {
        if (!manager) return;
        for (size_t team = 10; team; --team)
            LeaveCriticalSection((LPCRITICAL_SECTION)(_addr(0x140D48120) + (team - 1) * 0x28));
        LeaveCriticalSection((LPCRITICAL_SECTION)(static_cast<uint8_t*>(manager) + 8));
    }
};

// Caller owns both native fences and the recorder lock. No historical address
// is dereferenced by the eventual registration stage; list entries carry IDs.
void CaptureShotWorld(SnapshotManifest& saved, void* manager)
{
    saved.shotManager = manager;
    saved.shotCursor = manager ? Read<uint32_t>(manager, 0x438) : 0;
    saved.shotConfig = Read<void*>((void*)_addr(0x140D50F88), 0);
    saved.shotSlotFlags = saved.shotConfig ? Read<uint32_t>(saved.shotConfig, 0x4A89C) : 0;
    saved.shotOccupancy.fill(0); saved.shotControls.fill(0);
    saved.shotPending.clear();
    for (auto& list : saved.shotLists) list.clear();
    if (!manager) return;
    std::memcpy(saved.shotOccupancy.data(), static_cast<uint8_t*>(manager) + 0x38, 0x400);
    std::memcpy(saved.shotControls.data(), static_cast<uint8_t*>(manager) + 0x43C, 0xC);
    const auto id = [](void* object) {
        const auto* entry = Find(object, false);
        return entry && entry->kind == Kind::Shot ? entry->id : uint64_t(0);
    };
    for (size_t team = 0; team < 10; ++team) {
        void* sentinel = (void*)(_addr(0x140D47F90) + team * 0x28 + 8);
        void* previous = sentinel;
        void* node = Read<void*>(sentinel, 8);
        for (; node && node != sentinel; node = Read<void*>(node, 8)) {
            void* object = static_cast<uint8_t*>(node) - 0x1450;
            saved.shotLists[team].push_back({object, id(object)});
            previous = node;
        }
        // Native insertAfter never writes the header's previous link; only
        // unlinking the tail node does, so it is stale and is not checked.
        // Zeroed, not-yet-initialized heads are empty; a nonempty list is
        // circular through its sentinel (insertion into a zeroed head faults).
    }
    const uint32_t count = Read<uint32_t>(manager, 0x2448);
    for (uint32_t i = 0; i < count; ++i) {
        void* object = Read<void*>(manager, 0x448 + size_t(i) * 0x10);
        saved.shotPending.push_back({object, Read<uint64_t>(manager, 0x450 + size_t(i) * 0x10), id(object)});
    }
}

// Registration stage of the unpublished whole-shot transaction. The caller must
// first restore every participating body/dependency, then reconcile sUnit and
// constraints before any native execution resumes. Never call this by itself
// from Rollback_RestoreSnapshot.
[[maybe_unused]] void StageShotManagerState(const SnapshotManifest& saved, uint32_t bit)
{
    RollbackUnitFailureScope failureScope("shot.manager.stage", saved.shotManager, 0, saved.frame);
    UnitGuard unitFence;
    ShotWorldGuard shotFence;
    // The snapshot, its managers and thread were checked by RollbackUnit_RestoreShots.
    // Restore-fence scratch, reused across restores.
    static std::vector<const Identity*> members;
    static std::vector<Identity*> identities;
    static std::vector<uint8_t> linked;
    static std::vector<std::array<uint32_t, 2>> ids;
    members.clear(); identities.clear(); ids.clear();
    AcquireSRWLockExclusive(&g_lock);
    for (const auto& member : saved.members) {
        if (member.kind != Kind::Shot || member.occupancy != Occupancy::Live) continue;
        auto* current = Find(member.object, false);
        LIFE_REQUIRE(current && current->id == member.id && (current->snapshots & bit) &&
            !(current->occupancy != Occupancy::Live && current->occupancy != Occupancy::Quarantined));
        members.push_back(&member); identities.push_back(current);
    }
    // Saved members have distinct addresses (one registry identity per address).
    static ScratchMap<size_t> memberIndex;
    memberIndex.clear();
    for (size_t i = 0; i < members.size(); ++i) *memberIndex.insert(members[i]->object).first = i;
    const auto memberAt = [&](void* object, uint64_t id) {
        const size_t* at = memberIndex.find(object);
        return at && members[*at]->id == id ? *at : SIZE_MAX;
    };
    ReleaseSRWLockExclusive(&g_lock);
    linked.assign(members.size(), 0);
    const auto index = [&](const ShotListEntry& entry) {
        const size_t i = memberAt(entry.object, entry.id);
        if (i != SIZE_MAX) return i;
        LIFE_FAIL("saved shot-list entry has no matching retained identity", LIFE_VALUE(entry.object),
            LIFE_VALUE(entry.id), LIFE_VALUE(saved.frame), LIFE_VALUE(bit), LIFE_VALUE(members.size()));
    };
    const auto contains = [&](void* object) { return memberIndex.contains(object); };
    for (const auto* member : members) {
        const auto at = std::find_if(saved.shots.begin(), saved.shots.end(),
            [&](const auto& shot) { return shot.id == member->id; });
        const void* image = at->payload.data() + ShotBodySpan(*at, member->object).memory.offset;
        const uint32_t nativeId = Read<uint32_t>(image, 0x1F50), team = Read<uint32_t>(image, 0x1F54);
        // IDs/team belong to registration, not the CPU kernel. Reflection can
        // change both; validate saved claims before publishing them together.
        ids.push_back({nativeId, team});
    }
    for (size_t team = 0; team < 10; ++team) {
        for (const auto& entry : saved.shotLists[team]) {
            const size_t i = index(entry);
            linked[i] = true;
        }
    }
    static std::vector<ShotPendingEntry> foreign, pending;
    foreign.clear(); pending.clear();
    void* manager = shotFence.manager;
    const uint32_t count = Read<uint32_t>(manager, 0x2448);
    AcquireSRWLockExclusive(&g_lock);
    for (uint32_t i = 0; i < count; ++i) {
        void* object = Read<void*>(manager, 0x448 + size_t(i) * 0x10);
        const auto* current = Find(object, false);
        if (!current || current->kind != Kind::Shot) foreign.push_back({object, Read<uint64_t>(manager, 0x450 + size_t(i) * 0x10), 0});
    }
    ReleaseSRWLockExclusive(&g_lock);
    // Historical non-shot queue pointers confer no lease. Only use a matching
    // *current* foreign record, never dereference or resurrect an old address.
    static std::vector<uint8_t> used;
    used.assign(foreign.size(), 0);
    for (const auto& entry : saved.shotPending) {
        if (entry.id) {
            index({entry.object, entry.id});
            pending.push_back(entry);
        } else for (size_t i = 0; i < foreign.size(); ++i) {
            if (used[i] || foreign[i].object != entry.object || foreign[i].category != entry.category) continue;
            used[i] = true; pending.push_back(foreign[i]); break;
        }
    }
    for (size_t i = 0; i < foreign.size(); ++i) if (!used[i]) pending.push_back(foreign[i]);
    // The native queue has 512 entries; current foreign records plus restored
    // shots must fit before it is rewritten.
    LIFE_REQUIRE(pending.size() <= 512);
    // Preflight above performs no native state mutation. Commit contains no
    // native callback or allocation and preserves node vtables/self pointers.
    for (size_t i = 0; i < members.size(); ++i) {
        void* object = members[i]->object;
        Write(object, 0x1F50, ids[i][0]); Write(object, 0x1F54, ids[i][1]);
        Write(object, 0x1458, uintptr_t(0)); Write(object, 0x1460, uintptr_t(0));
    }
    for (size_t team = 0; team < 10; ++team) {
        void* sentinel = (void*)(_addr(0x140D47F90) + team * 0x28 + 8);
        void* previous = sentinel;
        for (const auto& entry : saved.shotLists[team]) {
            void* node = static_cast<uint8_t*>(entry.object) + 0x1450;
            Write(previous, 8, node); Write(node, 0x10, previous); previous = node;
        }
        Write(previous, 8, sentinel); Write(sentinel, 0x10, previous);
    }
    std::memcpy(static_cast<uint8_t*>(manager) + 0x38, saved.shotOccupancy.data(), 0x400);
    Write(manager, 0x438, saved.shotCursor);
    std::memcpy(static_cast<uint8_t*>(manager) + 0x43C, saved.shotControls.data(), 0xC);
    for (size_t i = 0; i < pending.size(); ++i) {
        Write(manager, 0x448 + i * 0x10, pending[i].object);
        Write(manager, 0x450 + i * 0x10, pending[i].category);
    }
    Write(manager, 0x2448, uint32_t(pending.size()));
    if (saved.shotConfig) Write(saved.shotConfig, 0x4A89C, saved.shotSlotFlags);
    AcquireSRWLockExclusive(&g_lock);
    for (size_t i = 0; i < members.size(); ++i) {
        identities[i]->shotLinked = linked[i];
        identities[i]->shotRegistrationReleased = members[i]->shotRegistrationReleased;
    }
    Push("shot_manager_staged", nullptr, nullptr, uint32_t(saved.frame), members.size(), pending.size(), 0, false);
    ReleaseSRWLockExclusive(&g_lock);
}

// Caller must have finished body/dependency repair at a quiescent main-tick
// boundary. This publishes scheduler state only, not constraints or shot lists.
[[maybe_unused]] void StageShotExecutionState(const SnapshotManifest& saved, uint32_t bit)
{
    RollbackUnitFailureScope failureScope("shot.execution.stage", saved.unitManager, 0, saved.frame);
    UnitGuard fence;
    const uint32_t categories = Read<uint32_t>(fence.manager, 0xC38);
    const uint32_t groupCount = Read<uint32_t>(fence.manager, 0x1840);
    struct Member {
        void* object;
        Identity* identity;
        const Identity* historical;
        uint32_t flags;
        uint64_t groups;
        uint64_t construction = 0;
    };
    // Restore-fence scratch: every container below keeps its storage between
    // restores, so the stage allocates nothing once warm.
    static std::vector<Member> members;
    static ScratchSet memberObjects, memberIdentities;
    members.clear(); memberObjects.clear(); memberIdentities.clear();
    const auto addMember = [&](const Member& member) {
        members.push_back(member);
        memberObjects.insert(member.object);
        if (member.identity) memberIdentities.insert(member.identity);
    };
    static std::vector<void*> primary[64], groups[64];
    for (auto& entries : primary) entries.clear();
    for (auto& entries : groups) entries.clear();
    AcquireSRWLockExclusive(&g_lock);
    for (const auto& member : saved.members) {
        if (member.kind != Kind::Shot || (member.occupancy != Occupancy::Live &&
            member.occupancy != Occupancy::Quarantined)) continue;
        auto* current = Find(member.object, false);
        LIFE_REQUIRE(current && current->id == member.id && (current->snapshots & bit) &&
            !(current->occupancy != Occupancy::Live && current->occupancy != Occupancy::Quarantined) &&
            !memberIdentities.contains(current));
        const auto shot = std::find_if(saved.shots.begin(), saved.shots.end(),
            [&](const ShotState& shot) { return shot.id == member.id; });
        const void* image = shot->payload.data() + ShotBodySpan(*shot, member.object).memory.offset;
        const uint32_t flags = Read<uint32_t>(image, 0x10), category = (flags >> 3) & 0x7F;
        const uint64_t mask = Read<uint64_t>(image, 0x30);
        addMember({current->object, current, &member, flags, mask});
    }
    // Shot body ranges, sorted for containment lookups of their inline constraints.
    static std::vector<std::pair<uintptr_t, uintptr_t>> shotRanges;
    shotRanges.clear();
    for (const auto& member : members)
        if (member.identity->extent >= 0x90)
            shotRanges.push_back({uintptr_t(member.object), uintptr_t(member.object) + member.identity->extent - 0x90});
    std::sort(shotRanges.begin(), shotRanges.end());
    // An incomplete constraint capture is never restored in part.
    LIFE_REQUIRE(saved.constraintsComplete);
    for (const auto& node : saved.constraints) {
        const auto current = g_constructedConstraints.find(node.object);
        auto* identity = Find(node.object, false);
        const Identity* historical = nullptr;
        if (identity) {
            const auto at = std::find_if(saved.members.begin(), saved.members.end(), [&](const Identity& member) {
                return member.object == node.object && member.id == identity->id;
            });
            LIFE_REQUIRE(identity->kind == Kind::Unit && at != saved.members.end() && (identity->snapshots & bit) &&
                !(at->occupancy != Occupancy::Live && at->occupancy != Occupancy::Quarantined) &&
                !(identity->occupancy != Occupancy::Live && identity->occupancy != Occupancy::Quarantined));
            historical = &*at;
        }
        addMember({node.object, identity, historical, Read<uint32_t>(node.image, 0x10),
                   Read<uint64_t>(node.image, 0x30), node.construction});
    }
    // Constraints initialized after the snapshot inside a member shot. Shot
    // allocations are disjoint, so each shot's inline constraints are one run of
    // the sorted address list: no pass over every constraint in the game.
    if (!shotRanges.empty() && !g_constructedConstraints.empty()) {
        const std::vector<void*>& sorted = SortedConstraints();
        for (const auto& range : shotRanges)
            for (auto at = std::lower_bound(sorted.begin(), sorted.end(), (void*)range.first, std::less<void*>{});
                 at != sorted.end() && uintptr_t(*at) <= range.second; ++at) {
                void* object = *at;
                const uint64_t construction = g_constructedConstraints.at(object);
                if (construction <= saved.constraintWatermark || memberObjects.contains(object)) continue;
                addMember({object, nullptr, nullptr, (Read<uint32_t>(object, 0x10) & ~0x3FFu) | 0x3F8u,
                           0, construction});
            }
    }
    const auto captured = [&](void* object) { return memberObjects.contains(object); };
    // Future-only live shots must be withdrawn before any publication.
    const auto reconcile = [&](const std::vector<void*>& historical, const std::vector<void*>& foreign,
                               std::vector<void*>& output) {
        static ScratchSet foreignSet, emitted;
        foreignSet.clear(); emitted.clear();
        for (void* object : foreign) foreignSet.insert(object);
        for (void* object : historical) {
            if (!captured(object) && !foreignSet.contains(object)) continue;
            if (emitted.insert(object)) output.push_back(object); // Once each.
        }
        for (void* object : foreign)
            if (emitted.insert(object)) output.push_back(object);
    };
    // Primary lines a member is saved in or sits in now. RollbackUnit_RestoreEffects
    // ran just before and reconciled every line against this same snapshot; a
    // line with no member here would come out of the pass below exactly as it
    // went in, so only these are walked and rewritten. (The walk dereferences
    // every unit on a line; it was most of this stage's 80 us.)
    uint64_t memberLines = 0;
    for (const auto& member : members)
        for (const uint32_t flags : {member.flags, Read<uint32_t>(member.object, 0x10)}) {
            const uint32_t category = (flags >> 3) & 0x7F;
            if (category < 64) memberLines |= uint64_t(1) << category;
        }
    static ScratchSet seen;
    seen.clear();
    static std::vector<void*> foreign;
    for (uint32_t category = 0; category < 64; ++category) {
        if (category >= categories || !(memberLines & (uint64_t(1) << category))) {
            continue;
        }
        foreign.clear();
        void* previous = nullptr;
        void* line = static_cast<uint8_t*>(fence.manager) + 0x50 + category * 0x30;
        for (void* unit = Read<void*>(line, 0); unit; unit = Read<void*>(unit, 0x18)) {
            if (seen.size() == RegistrySize || seen.contains(unit) ||
                Read<void*>(unit, 0x20) != previous || ((Read<uint32_t>(unit, 0x10) >> 3) & 0x7F) != category)
                LIFE_FAIL("current unit primary list is inconsistent or cyclic", LIFE_VALUE(category),
                    LIFE_VALUE(unit), LIFE_VALUE(previous), LIFE_VALUE(seen.size()));
            seen.insert(unit); previous = unit;
            if (!captured(unit)) foreign.push_back(unit);
        }
        reconcile(saved.primary[category], foreign, primary[category]);
    }
    for (uint32_t group = 0; group < 64; ++group) {
        if (group >= groupCount) {
            continue;
        }
        void* descriptor = static_cast<uint8_t*>(fence.manager) + 0xC40 + group * 0x30;
        const uint32_t size = Read<uint32_t>(descriptor, 0x18), capacity = Read<uint32_t>(descriptor, 0x1C);
        void* data = Read<void*>(descriptor, 0x28);
        if (Read<uint8_t>(descriptor, 0x20) || size > capacity || (capacity && !data))
            LIFE_FAIL("unit group backing storage violates the non-owning array contract", LIFE_VALUE(group),
                LIFE_VALUE(descriptor), LIFE_VALUE(size), LIFE_VALUE(capacity), LIFE_VALUE(data));
        foreign.clear();
        for (uint32_t i = 0; i < size; ++i) {
            void* unit = Read<void*>(data, size_t(i) * 8);
            if (!captured(unit)) foreign.push_back(unit);
        }
        reconcile(saved.groups[group], foreign, groups[group]);
    }
    size_t held = 0;
    for (const auto& member : members)
        held += member.identity && member.historical && member.identity->occupancy == Occupancy::Quarantined &&
                member.historical->occupancy == Occupancy::Live;
    // Only allocation preparation invokes native code. No topology is changed
    // until every list/mask/identity check above has passed.
    for (uint32_t group = 0; group < groupCount; ++group) {
        void* descriptor = static_cast<uint8_t*>(fence.manager) + 0xC40 + group * 0x30;
        if (groups[group].size() <= Read<uint32_t>(descriptor, 0x1C)) continue;
        ReleaseSRWLockExclusive(&g_lock);
        ((void(__fastcall*)(void*, uint32_t))_addr(0x140011660))(
            static_cast<uint8_t*>(descriptor) + 0x10, (uint32_t(groups[group].size()) + 31) & ~31u);
        AcquireSRWLockExclusive(&g_lock);
    }
    for (const auto& member : members) {
        void* object = member.object;
        // Lifecycle/category, move and fixed flags. Draw/presentation bits stay live.
        constexpr uint32_t simulation = 0x27FF;
        Write(object, 0x10, (Read<uint32_t>(object, 0x10) & ~simulation) | (member.flags & simulation));
        Write(object, 0x30, member.groups);
        Write(object, 0x18, (void*)nullptr); Write(object, 0x20, (void*)nullptr);
        if (member.identity && member.historical && member.identity->occupancy != member.historical->occupancy) {
            if (member.historical->occupancy == Occupancy::Quarantined) ++g_quarantined;
            else --g_quarantined;
            member.identity->occupancy = member.historical->occupancy;
        }
        if (member.identity && member.historical) member.identity->futureShot = member.historical->futureShot;
    }
    for (uint32_t category = 0; category < categories && category < 64; ++category) {
        if (!(memberLines & (uint64_t(1) << category))) continue; // Not walked: left as it is.
        const auto& entries = primary[category];
        void* line = static_cast<uint8_t*>(fence.manager) + 0x50 + category * 0x30;
        Write(line, 0, entries.empty() ? nullptr : entries.front());
        Write(line, 8, entries.empty() ? nullptr : entries.back());
        for (size_t i = 0; i < entries.size(); ++i) {
            Write(entries[i], 0x20, i ? entries[i - 1] : nullptr);
            Write(entries[i], 0x18, i + 1 < entries.size() ? entries[i + 1] : nullptr);
        }
    }
    for (uint32_t group = 0; group < groupCount; ++group) {
        void* descriptor = static_cast<uint8_t*>(fence.manager) + 0xC40 + group * 0x30;
        const auto& entries = groups[group];
        if (!entries.empty()) std::memcpy(Read<void*>(descriptor, 0x28), entries.data(), entries.size() * sizeof(void*));
        Write(descriptor, 0x18, uint32_t(entries.size()));
    }
    Push("shot_execution_staged", nullptr, nullptr, uint32_t(saved.frame), members.size(), held, 0, false);
    ValidateActiveSlots();
    ReleaseSRWLockExclusive(&g_lock);
}

template<class Visit> void VisitCollisionLists(void* collider, Visit visit)
{
    const uint32_t count = Read<uint32_t>(collider, 0x10);
    void* categories = Read<void*>(collider, 0x20);
    for (uint32_t i = 0; i < count; ++i) {
        void* category = Read<void*>(categories, size_t(i) * 8);
        if (!category) continue;
        const uint32_t passes = Read<uint32_t>(category, 8);
        void* lists = Read<void*>(category, 0x18);
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
            for (uint32_t i = 0; i < count; ++i) {
                void* record = Read<void*>(data, size_t(i) * 8);
                void* root = Read<void*>(record, rootField);
                if (std::binary_search(roots.begin(), roots.end(), root, std::less<void*>{})) {
                    LIFE_FAIL("cannot withdraw a shot root with continuous collision registration",
                        LIFE_VALUE(root), LIFE_VALUE(list), LIFE_VALUE(array), LIFE_VALUE(rootField),
                        LIFE_VALUE(record), LIFE_VALUE(i), LIFE_VALUE(count));
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
    for (size_t index : OwnerGenerationSlots(shot.id)) {
        const auto& generation = g_shotGenerations[index];
        if (generation.attack && generation.attack->root->object) roots.push_back(generation.attack->root->object);
    }
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
    for (const auto& unit : ActiveIdentities()) {
        if (unit.object == object || (unit.occupancy != Occupancy::Live &&
            unit.occupancy != Occupancy::Quarantined)) continue;
        if (unit.kind == Kind::Effect)
            for (size_t field : {size_t(0x40), size_t(0x138), size_t(0x1F0),
                                 size_t(0x278), size_t(0x280), size_t(0x288)})
                if (Read<void*>(unit.object, field) == object) return true;
        if (unit.kind == Kind::Shot && unit.extent >= 0x2140) {
            bool found = false;
            VisitShotUnitReferences(unit.object, [&](size_t, void* reference) { found |= reference == object; });
            if (found) return true;
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

std::vector<uint64_t> FenceFutureShotCollection()
{
    std::vector<Identity> candidates;
    std::vector<void*> roots;
    std::vector<uint64_t> collectable;
    AcquireSRWLockExclusive(&g_lock);
    for (const auto& shot : ActiveIdentities())
        if (shot.kind == Kind::Shot && shot.futureShot && shot.occupancy == Occupancy::Quarantined &&
            !shot.snapshots && !HasRetiredShotGenerations(shot.id) && !HasLiveUnitReference(shot.object))
            candidates.push_back(shot);
    ReleaseSRWLockExclusive(&g_lock);
    void* units = Read<void*>((void*)_addr(0x140E17698), 0);
    for (const auto& shot : candidates)
        if (!HasCharacterShotReference(units, shot.object)) {
            AppendShotRoots(shot, roots);
            collectable.push_back(shot.id);
        }
    DetachTransientShotCollisions(std::move(roots));
    return collectable;
}

void WithdrawFutureShot(const Identity& shot, void* manager, void* units)
{
    RollbackUnitFailureScope failureScope("shot.withdraw_future", shot.object, shot.id);
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
    LIFE_REQUIRE(current && current->id == shot.id && current->occupancy == Occupancy::Live);
    current->occupancy = Occupancy::Quarantined;
    current->shotLinked = false; current->shotRegistrationReleased = true; current->futureShot = true;
    ++g_quarantined;
    Push("quarantine_hold", current, shot.object, current->snapshots, g_quarantined, 0, 1, false);
    Push("future_shot_withdrawn", current, shot.object, Read<uint32_t>(shot.object, 0x1F50), 0, 0, 0, false);
    ReleaseSRWLockExclusive(&g_lock);
}

} // namespace

void RollbackUnit_RetireFutureShots(int32_t frame)
{
    RollbackUnitFailureScope failureScope("shots.withdraw_future", nullptr, 0, frame);
    if constexpr (!ROLLBACK_UNIT_ENABLED || !EMPTY_EFFECT_QUARANTINE_ENABLED || !EFFECT_RESTORE_ENABLED) return;
    if (!g_rollbackUnitEnabled.load(std::memory_order_relaxed)) return;
    UnitGuard fence;
    // While the abandoned shots still name them.
    if constexpr (SAVED_SHOT_RESTORE_ENABLED) MarkAbandonedModels(frame);
    const unsigned slot = unsigned((frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    std::vector<Identity> future;
    std::vector<void*> roots;
    AcquireSRWLockExclusive(&g_lock);
    const auto* saved = g_liveSnapshots[slot];
    if (!saved || !saved->valid || saved->frame != frame || !fence.manager ||
        saved->unitManager != fence.manager) { ReleaseSRWLockExclusive(&g_lock); return; }
    void* manager = Read<void*>((void*)_addr(0x140D47F40), 0);
    for (auto& shot : ActiveIdentities())
        if (shot.kind == Kind::Shot && (shot.occupancy == Occupancy::Live ||
            shot.occupancy == Occupancy::Quarantined) && shot.id > saved->birthWatermark) {
            future.push_back(shot);
            if (shot.occupancy == Occupancy::Live) AppendShotRoots(shot, roots);
            else shot.futureShot = true;
        }
    // A withdrawn shot never runs its kill, which is what ends the effects it
    // drives. One the snapshot cannot rewind would be left running for good
    // (rock debris of a mispredicted throw), so it leaves with its shot. The
    // replay creates its own.
    for (const auto& shot : future) {
        if (shot.occupancy != Occupancy::Live) continue;
        VisitShotUnitReferences(shot.object, [&](size_t, void* object) {
            Identity* effect = object ? Find(object, false) : nullptr;
            if (effect && effect->kind == Kind::Effect && effect->occupancy == Occupancy::Live &&
                effect->id > saved->birthWatermark) effect->abandonedModel = true;
        });
    }
    ReleaseSRWLockExclusive(&g_lock);
    if (!manager) return;
    void* config = Read<void*>((void*)_addr(0x140D50F88), 0);
    DetachTransientShotCollisions(std::move(roots));
    for (const auto& shot : future)
        if (shot.occupancy == Occupancy::Live) WithdrawFutureShot(shot, manager, fence.manager);
    // The pending queue stores {unit*, line} pairs, not just pointers. Preserve
    // foreign entries and ordering; abandoned entries must not be published later.
    const uint32_t count = Read<uint32_t>(manager, 0x2448);
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
