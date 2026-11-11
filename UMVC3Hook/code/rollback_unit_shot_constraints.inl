// Native construction provenance, not ownership inferred from pointer values.
std::unordered_map<void*, uint64_t> g_constructedConstraints;
uint64_t g_nextConstraintConstruction = 0;
// Registered addresses in ascending order, rebuilt only after the set changed.
// The version counts every insert and erase and is never reset; the size check
// also catches the map being emptied directly.
uint64_t g_constraintSetVersion = 0, g_sortedConstraintVersion = UINT64_MAX;
std::vector<void*> g_sortedConstraints;
// Caller holds g_lock. The reference is valid until the lock is released.
const std::vector<void*>& SortedConstraints()
{
    if (g_sortedConstraintVersion != g_constraintSetVersion ||
        g_sortedConstraints.size() != g_constructedConstraints.size()) {
        g_sortedConstraints.clear();
        g_sortedConstraints.reserve(g_constructedConstraints.size());
        for (const auto& entry : g_constructedConstraints) g_sortedConstraints.push_back(entry.first);
        std::sort(g_sortedConstraints.begin(), g_sortedConstraints.end(), std::less<void*>{});
        g_sortedConstraintVersion = g_constraintSetVersion;
    }
    return g_sortedConstraints;
}
void (__fastcall* oInitializeConstraint)(void*) = nullptr;
void (__fastcall* oConstraintObjectDtor)(void*) = nullptr;

// The teardown hook below sits on MtObject's base destructor: every MtObject in
// the engine, on every thread, including per-frame stack temporaries. Each bucket
// counts the registered constraints hashing to it, so an unregistered object is
// rejected with one atomic load and no lock. Counts change only under g_lock with
// the map. A registered object keeps its own bucket nonzero until its own
// teardown erases it, so the unlocked check has no false negatives.
constexpr size_t ConstraintBucketCount = size_t(1) << 14;
std::atomic<uint16_t> g_constraintBuckets[ConstraintBucketCount];
std::atomic<uint16_t>& ConstraintBucket(void* object)
{
    return g_constraintBuckets[((uintptr_t(object) >> 4) * 11400714819323198485ull) >> (64 - 14)];
}
// Caller holds g_lock. Re-initialization renews the construction identity.
void RegisterConstraint(void* object)
{
    const auto [at, inserted] = g_constructedConstraints.try_emplace(object);
    at->second = ++g_nextConstraintConstruction;
    if (inserted) {
        ++g_constraintSetVersion;
        ConstraintBucket(object).fetch_add(1, std::memory_order_relaxed);
    }
}

void __fastcall InitializeConstraint(void* object)
{
    oInitializeConstraint(object);
    if (!g_rollbackUnitEnabled.load(std::memory_order_relaxed)) return;
    AcquireSRWLockExclusive(&g_lock);
    RegisterConstraint(object);
    ReleaseSRWLockExclusive(&g_lock);
}

void __fastcall ForgetConstraintObject(void* object)
{
    // Observed in-place and owner-inline teardown paths converge here. Dying
    // storage is never inspected, including calls for unrelated MtObjects.
    if (ConstraintBucket(object).load(std::memory_order_relaxed)) {
        AcquireSRWLockExclusive(&g_lock);
        if (g_constructedConstraints.erase(object)) {
            ++g_constraintSetVersion;
            ConstraintBucket(object).fetch_sub(1, std::memory_order_relaxed);
        }
        ReleaseSRWLockExclusive(&g_lock);
    }
    oConstraintObjectDtor(object);
}

// Caller owns the capture fences and recorder lock. The callback may adopt a
// model owner, so no registry iterator survives across it.
// sortedConstraints: g_constructedConstraints addresses in ascending order.
template<class Visit> void VisitShotConstraintModels(const Identity& owner,
    const std::vector<void*>& sortedConstraints, Visit visit)
{
    auto* joints = Read<uint8_t*>(owner.object, 0x538);
    const uint32_t count = joints ? Read<uint32_t>(joints - 0x10, 0) : 0;
    if (count > 65536) {
        RollbackUnitFailureScope failureScope("shot.constraint_dependencies.capture", owner.object, owner.id);
        LIFE_FAIL("constraint dependency scan found an invalid joint allocation count",
            LIFE_VALUE(owner.object), LIFE_VALUE(owner.id), LIFE_VALUE(owner.extent), LIFE_VALUE(joints), LIFE_VALUE(count));
    }
    // Every node below must be a registered constraint: with none, nothing to visit.
    if (sortedConstraints.empty()) return;
    RollbackUnitFailureScope failureScope("shot.constraint_dependencies.capture", owner.object, owner.id);
    // Capture-fence scratch, reused across shots and frames.
    static std::vector<void*> nodes;
    static ScratchSet appended;
    nodes.clear(); appended.clear();
    const auto append = [&](void* object) {
        if (object && g_constructedConstraints.contains(object) && appended.insert(object))
            nodes.push_back(object);
    };
    // Inline constraints lie within [owner, owner + extent - 0x90].
    if (owner.extent >= 0x90)
        for (auto at = std::lower_bound(sortedConstraints.begin(), sortedConstraints.end(), owner.object, std::less<void*>{});
             at != sortedConstraints.end() && uintptr_t(*at) - uintptr_t(owner.object) <= owner.extent - 0x90; ++at)
            append(*at);
    for (uint32_t i = 0; i < count; ++i) {
        void* head = Read<void*>(joints + i*0xC0, 8);
        if (head) append((void*)(uintptr_t(head) - 0x40));
    }
    for (size_t i = 0; i < nodes.size(); ++i) {
        if (!g_constructedConstraints.contains(nodes[i])) LIFE_FAIL("constraint disappeared during dependency capture",
            LIFE_VALUE(nodes[i]), LIFE_VALUE(i), LIFE_VALUE(owner.id));
        void* model = Read<void*>(nodes[i], 0x78);
        append(Read<void*>(nodes[i], 0x50)); append(Read<void*>(nodes[i], 0x58));
        if (model) visit(model);
    }
}

void CaptureShotConstraints(SnapshotManifest& saved)
{
    RollbackUnitFailureScope failureScope("shot.constraints.capture", nullptr, 0, saved.frame);
    saved.constraints.clear();
    saved.constraintsComplete = true;
    AcquireSRWLockExclusive(&g_lock);
    if (saved.shots.empty()) {
        // Chains are discovered only through shot bodies and joints: with no
        // shot there is nothing to capture, and no reason to walk every constraint.
        saved.constraintWatermark = g_nextConstraintConstruction;
        ReleaseSRWLockExclusive(&g_lock);
        return;
    }
    static ScratchSet captured; // Capture-fence scratch, reused across frames.
    captured.clear();
    const auto append = [&](void* object) {
        if (!object || captured.contains(object)) return;
        const auto at = g_constructedConstraints.find(object);
        if (at == g_constructedConstraints.end()) {
            saved.constraintsComplete = false;
            Push("constraint_capture_untracked", nullptr, object, uint32_t(saved.frame), 0, 0, 0, false);
            return; // No pointee access; the restore preflight rejects this entire graph.
        }
        ShotConstraintState node;
        node.object = object; node.construction = at->second;
        std::memcpy(node.image, object, sizeof(node.image));
        const auto self = Read<void*>(node.image, 0x48);
        if (self != object) LIFE_FAIL("registered constraint adapter has an invalid complete-object backpointer",
            LIFE_VALUE(object), LIFE_VALUE(self), LIFE_VALUE(node.construction), LIFE_VALUE(saved.frame),
            LIFE_VALUE(saved.constraints.size()));
        saved.constraints.push_back(node);
        captured.insert(object);
    };
    // Completed shot allocations prove containment of their inline children.
    // Include detached constraints as well as the complete neighboring chains.
    // Allocations are disjoint, so each body's constraints are one run of the
    // sorted address list: no pass over every constraint in the game.
    if (!g_constructedConstraints.empty()) {
        const std::vector<void*>& sorted = SortedConstraints();
        for (const auto& shot : saved.shots) {
            const auto body = std::find_if(shot.spans.begin(), shot.spans.end(),
                [](const ShotSpan& span) { return span.kind == ShotSpanKind::Body; });
            LIFE_REQUIRE(body != shot.spans.end());
            if (body->memory.size < 0x90) continue;
            for (auto at = std::lower_bound(sorted.begin(), sorted.end(), body->memory.address, std::less<void*>{});
                 at != sorted.end() && uintptr_t(*at) - uintptr_t(body->memory.address) <= body->memory.size - 0x90; ++at)
                append(*at);
        }
    }
    for (const auto& shot : saved.shots) for (const auto& span : shot.spans) {
        if (span.kind != ShotSpanKind::ModelJoints) continue;
        LIFE_REQUIRE(span.memory.offset <= shot.payload.size() && span.memory.size <= shot.payload.size() - span.memory.offset &&
            !(span.memory.size % 0xC0));
        const uint8_t* bytes = shot.payload.data() + span.memory.offset;
        for (size_t i = 0; i < span.memory.size; i += 0xC0) {
            void* head = Read<void*>(bytes + i, 8);
            if (head) append((void*)(uintptr_t(head) - 0x40));
        }
    }
    for (size_t i = 0; i < saved.constraints.size(); ++i) {
        // Copy before append: vector growth must not invalidate the source.
        void* previous = Read<void*>(saved.constraints[i].image, 0x50);
        void* next = Read<void*>(saved.constraints[i].image, 0x58);
        append(previous); append(next);
    }
    saved.constraintWatermark = g_nextConstraintConstruction;
    ReleaseSRWLockExclusive(&g_lock);
}
