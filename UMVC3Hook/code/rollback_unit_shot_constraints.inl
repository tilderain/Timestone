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

struct ShotConstraintPlan {
    const SnapshotManifest* saved = nullptr;
    uint64_t captureId = 0;
    void* manager = nullptr;
    int32_t frame = -1;
    uint32_t thread = 0;
    uint32_t bit = 0;
    struct Model {
        void* object;
        uint64_t id;
        const uint8_t* body;
        const ShotSpan* joints;
        const ShotSpan* depths;
        const ShotState* shot;
    };
    std::vector<Model> models;
    std::vector<void*> currentJoints;
    std::vector<ShotConstraintState> currentNodes;
    std::vector<ShotState::GenerationRef> poses;
};

// Prepare before storage swaps. The native transaction must exclude collection
// until commit, so displaced current poses remain owned by their generations.
[[maybe_unused]] ShotConstraintPlan PrepareShotConstraintGraph(const SnapshotManifest& saved, uint32_t bit)
{
    RollbackUnitFailureScope failureScope("shot.constraints.prepare", nullptr, 0, saved.frame);
    UnitGuard fence;
    ShotConstraintPlan plan;
    plan.saved = &saved; plan.bit = bit; plan.captureId = saved.captureId;
    plan.manager = saved.unitManager; plan.frame = saved.frame; plan.thread = saved.thread;
    LIFE_REQUIRE(saved.shotPayload == RollbackUnitShotPayload::Capture);
    LIFE_REQUIRE(saved.valid && fence.manager && saved.unitManager == fence.manager &&
        saved.thread == GetCurrentThreadId() && bit && !(bit & (bit - 1)));
    const unsigned slot = unsigned((saved.frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    AcquireSRWLockExclusive(&g_lock);
    LIFE_REQUIRE(bit == (uint32_t(1) << slot) && g_liveSnapshots[slot] == &saved && saved.constraintsComplete);
    std::unordered_set<void*> seenJoints, currentObjects;
    for (const auto& shot : saved.shots) {
        const ShotSpan *body = nullptr, *joints = nullptr, *depths = nullptr;
        for (const auto& span : shot.spans) {
            LIFE_REQUIRE(span.memory.offset <= shot.payload.size() && span.memory.size <= shot.payload.size() - span.memory.offset);
            const ShotSpan** destination = span.kind == ShotSpanKind::Body ? &body :
                (span.kind == ShotSpanKind::ModelJoints ? &joints :
                (span.kind == ShotSpanKind::ModelDepths ? &depths : nullptr));
            if (destination) { LIFE_REQUIRE(!*destination); *destination = &span; }
        }
        LIFE_REQUIRE(body && body->memory.size >= 0x2140);
        auto* owner = Find(body->memory.address, false);
        LIFE_REQUIRE(owner && owner->kind == Kind::Shot && owner->id == shot.id && (owner->snapshots & bit) &&
            !(owner->occupancy != Occupancy::Live && owner->occupancy != Occupancy::Quarantined));
        const uint8_t* image = shot.payload.data() + body->memory.offset;
        const ShotGeneration* pose = nullptr;
        for (const auto& ref : shot.generations) {
            LIFE_REQUIRE(ref.index < g_shotGenerations.size());
            const auto& generation = g_shotGenerations[ref.index];
            LIFE_REQUIRE(generation.id == ref.id && generation.ownerId == shot.id && generation.owner == owner->object &&
                (generation.snapshots & bit));
            if (generation.domain == ShotDomain::Pose) { LIFE_REQUIRE(!pose); pose = &generation; }
        }
        LIFE_REQUIRE(pose && Read<void*>(pose->image, 0x538) == Read<void*>(image, 0x538) &&
            Read<void*>(pose->image, 0x548) == Read<void*>(image, 0x548));
        if (joints) {
            LIFE_REQUIRE(joints->memory.address == Read<void*>(image, 0x538) && !(joints->memory.size % 0xC0) &&
                joints->memory.size / 0xC0 == Read<uint32_t>(image, 0x530) && depths &&
                depths->memory.address == Read<void*>(image, 0x548) && depths->memory.size == joints->memory.size / 0xC0);
        } else LIFE_REQUIRE(!Read<void*>(image, 0x538) && !depths);
        plan.models.push_back({owner->object, owner->id, image, joints, depths, &shot});
        const auto domains = g_shotDomains.find(shot.id);
        LIFE_REQUIRE(domains != g_shotDomains.end());
        const int current = domains->second.current[size_t(ShotDomain::Pose)];
        LIFE_REQUIRE(current >= 0 && size_t(current) < g_shotGenerations.size());
        const auto& livePose = g_shotGenerations[current];
        void* liveJoints = Read<void*>(owner->object, 0x538);
        LIFE_REQUIRE(livePose.id && livePose.attached && livePose.ownerId == owner->id &&
            Read<void*>(livePose.image, 0x538) == liveJoints);
        // Detached pose generations are still native-owned here. A constraint
        // can remain attached to their old joints until this transaction repairs it.
        for (size_t index : OwnerGenerationSlots(shot.id)) {
            const auto& generation = g_shotGenerations[index];
            if (generation.domain != ShotDomain::Pose) continue;
            plan.poses.push_back({index, generation.id});
            auto* root = Read<uint8_t*>(generation.image, 0x538);
            const uint32_t count = root ? Read<uint32_t>(root - 0x10, 0) : 0;
            LIFE_REQUIRE(count <= 65536);
            for (uint32_t i = 0; i < count; ++i) {
                void* joint = root + i*0xC0;
                if (seenJoints.insert(joint).second) plan.currentJoints.push_back(joint);
            }
        }
    }
    const auto append = [&](void* object) {
        if (!object || !currentObjects.insert(object).second) return;
        const auto at = g_constructedConstraints.find(object);
        LIFE_REQUIRE(at != g_constructedConstraints.end());
        ShotConstraintState node; node.object = object; node.construction = at->second;
        std::memcpy(node.image, object, sizeof(node.image));
        LIFE_REQUIRE(Read<void*>(node.image, 0x48) == object);
        plan.currentNodes.push_back(node);
    };
    for (const auto& node : saved.constraints) {
        const auto at = g_constructedConstraints.find(node.object);
        LIFE_REQUIRE(node.construction && at != g_constructedConstraints.end() && at->second == node.construction);
        LIFE_REQUIRE(Read<uintptr_t>(node.object, 0) == Read<uintptr_t>(node.image, 0) &&
            Read<uintptr_t>(node.object, 0x40) == Read<uintptr_t>(node.image, 0x40));
        append(node.object);
    }
    for (void* joint : plan.currentJoints) {
        void* head = Read<void*>(joint, 8);
        if (head) append((void*)(uintptr_t(head) - 0x40));
    }
    for (size_t i = 0; i < plan.currentNodes.size(); ++i) {
        void* previous = Read<void*>(plan.currentNodes[i].image, 0x50);
        void* next = Read<void*>(plan.currentNodes[i].image, 0x58);
        append(previous); append(next);
    }
    std::unordered_set<void*> historicalObjects;
    for (const auto& node : saved.constraints) historicalObjects.insert(node.object);
    std::unordered_set<void*> modelObjects;
    for (const auto& model : plan.models) modelObjects.insert(model.object);
    for (const auto& node : plan.currentNodes) {
        if (historicalObjects.count(node.object)) continue;
        // A previously existing foreign node has missing historical state, not
        // permission to erase it. Only proven future inline children withdraw.
        LIFE_REQUIRE(node.construction > saved.constraintWatermark &&
            !std::none_of(plan.models.begin(), plan.models.end(), [&](const ShotConstraintPlan::Model& model) {
                const uintptr_t relative = uintptr_t(node.object) - uintptr_t(model.object);
                const auto* identity = Find(model.object, false);
                return identity && identity->extent >= 0x90 && relative <= identity->extent - 0x90;
            }));
    }
    const auto validate = [&](const std::vector<ShotConstraintState>& nodes, bool historical) {
        // Each object exactly once; chain heads address the node's +0x40 link.
        std::unordered_map<uintptr_t, const ShotConstraintState*> byHead;
        for (const auto& node : nodes) LIFE_REQUIRE(byHead.emplace(uintptr_t(node.object) + 0x40, &node).second);
        std::unordered_set<void*> visited;
        const auto chain = [&](void* joint, void* head) {
            void* previous = nullptr;
            while (head) {
                const auto at = byHead.find(uintptr_t(head));
                LIFE_REQUIRE(at != byHead.end());
                const ShotConstraintState* node = at->second;
                LIFE_REQUIRE(visited.insert(node->object).second &&
                    Read<void*>(node->image, 0x48) == node->object && Read<void*>(node->image, 0x50) == previous &&
                    Read<void*>(node->image, 0x60) == joint);
                previous = node->object;
                void* next = Read<void*>(node->image, 0x58);
                head = next ? (void*)(uintptr_t(next) + 0x40) : nullptr;
            }
        };
        if (historical) {
            for (const auto& model : plan.models) if (model.joints) {
                const uint8_t* bytes = model.shot->payload.data() + model.joints->memory.offset;
                for (size_t i = 0; i < model.joints->memory.size; i += 0xC0)
                    chain(static_cast<uint8_t*>(model.joints->memory.address) + i, Read<void*>(bytes + i, 8));
            }
        } else for (void* joint : plan.currentJoints) chain(joint, Read<void*>(joint, 8));
        for (const auto& node : nodes) {
            LIFE_REQUIRE(Read<void*>(node.image, 0x48) == node.object);
            if (Read<void*>(node.image, 0x60)) {
                LIFE_REQUIRE(visited.count(node.object));
            } else LIFE_REQUIRE(!Read<void*>(node.image, 0x50) && !Read<void*>(node.image, 0x58));
            void* model = Read<void*>(node.image, 0x78);
            LIFE_REQUIRE(!model || modelObjects.count(model));
        }
    };
    validate(saved.constraints, true); validate(plan.currentNodes, false);
    ReleaseSRWLockExclusive(&g_lock);
    return plan;
}

[[maybe_unused]] void StageShotConstraintGraph(const ShotConstraintPlan& plan)
{
    RollbackUnitFailureScope failureScope("shot.constraints.stage", nullptr, 0, plan.frame);
    UnitGuard fence;
    LIFE_REQUIRE(plan.saved && fence.manager && fence.manager == plan.manager && plan.thread == GetCurrentThreadId());
    AcquireSRWLockExclusive(&g_lock);
    const auto slot = unsigned((plan.frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    LIFE_REQUIRE(g_liveSnapshots[slot] == plan.saved && plan.saved->valid && plan.saved->captureId == plan.captureId &&
        plan.saved->frame == plan.frame && plan.bit == (uint32_t(1) << slot));
    for (const auto& pose : plan.poses)
        LIFE_REQUIRE(pose.index < g_shotGenerations.size() && g_shotGenerations[pose.index].id == pose.id);
    for (const auto& node : plan.currentNodes) {
        const auto at = g_constructedConstraints.find(node.object);
        LIFE_REQUIRE(at != g_constructedConstraints.end() && at->second == node.construction &&
            Read<uintptr_t>(node.object, 0) == Read<uintptr_t>(node.image, 0) &&
            Read<uintptr_t>(node.object, 0x40) == Read<uintptr_t>(node.image, 0x40) &&
            Read<void*>(node.object, 0x48) == node.object);
    }
    for (const auto& model : plan.models) {
        const auto* owner = Find(model.object, false);
        LIFE_REQUIRE(owner && owner->id == model.id && (owner->snapshots & plan.bit) &&
            !(owner->occupancy != Occupancy::Live && owner->occupancy != Occupancy::Quarantined) &&
            Read<void*>(model.object, 0x538) == Read<void*>(model.body, 0x538) &&
            Read<void*>(model.object, 0x548) == Read<void*>(model.body, 0x548));
    }
    // All outgoing edges are removed before publishing saved heads. No sorting,
    // dependency getters, model evaluation or native callbacks run here.
    for (void* joint : plan.currentJoints) Write(joint, 8, (void*)nullptr);
    for (const auto& node : plan.currentNodes) {
        Write(node.object, 0x50, (void*)nullptr); Write(node.object, 0x58, (void*)nullptr);
        Write(node.object, 0x60, (void*)nullptr); Write(node.object, 0x70, int32_t(-1));
    }
    for (const auto& node : plan.saved->constraints) {
        std::memcpy(static_cast<uint8_t*>(node.object) + 8, node.image + 8, 8);
        Write(node.object, 0x28, Read<float>(node.image, 0x28));
        std::memcpy(static_cast<uint8_t*>(node.object) + 0x48, node.image + 0x48, 0x42);
    }
    for (const auto& model : plan.models) {
        Write(model.object, 0x558, Read<uint32_t>(model.body, 0x558));
        Write(model.object, 0x123D, Read<uint8_t>(model.body, 0x123D));
        if (!model.joints) continue;
        const auto* bytes = model.shot->payload.data() + model.joints->memory.offset;
        for (size_t i = 0; i < model.joints->memory.size; i += 0xC0) {
            auto* joint = static_cast<uint8_t*>(model.joints->memory.address) + i;
            Write(joint, 0x14, Read<uint32_t>(bytes + i, 0x14));
            Write(joint, 0xA4, Read<uint32_t>(bytes + i, 0xA4));
            Write(joint, 8, Read<void*>(bytes + i, 8));
        }
        std::memcpy(model.depths->memory.address, model.shot->payload.data() + model.depths->memory.offset,
                    model.depths->memory.size);
    }
    Push("shot_constraints_staged", nullptr, nullptr, uint32_t(plan.saved->frame), plan.saved->constraints.size(),
         plan.models.size(), 0, false);
    ReleaseSRWLockExclusive(&g_lock);
}
