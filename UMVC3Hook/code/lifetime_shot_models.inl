// Included in the recorder namespace. Rewinds the uGameEffectModel units that
// shots drive. A lease alone keeps such a model allocated but not in step: the
// replay advances it once per resimulated frame on top of where it already was,
// so its animation ends early, it retires itself, and the shot that waits on it
// (uShotVulcanWeaponCart::updateState, phase 0) never proceeds.
constexpr uintptr_t GameEffectModelDti = 0x140D46ED0;
// uGameEffectModel's own words follow the actor prefix's inline tables: watched
// unit, control flags, life and elapsed clocks, bounds mode and step flags.
constexpr size_t ModelLifecycleOffset = 0x1CC0, ModelLifecycleEnd = 0x1D00;
// Borrowed cUnit links the restored bytes carry: parent, actor owner, command
// return value, and the watched unit.
constexpr size_t ModelUnitFields[] = {0x40, 0x1308, 0x13D8, ModelLifecycleOffset};

// Caller holds g_lock. Attack spaces are the same class family with their own
// phase machine and restore path.
bool IsDrivenModel(const Identity& identity)
{
    if (identity.kind != Kind::Unit || identity.extent < ModelLifecycleEnd || IsAttackSpace(identity.object))
        return false;
    uint64_t parent = identity.dti;
    for (unsigned depth = 0; parent && depth < 128; ++depth) {
        if (parent == uintptr_t(_addr(GameEffectModelDti))) return true;
        parent = Read<uint64_t>((void*)parent, 0x20);
    }
    return false;
}

// The model's own pose and motion storage, as for a shot body. Counts and
// pointers are part of the body image; contents are copied once per buffer.
template<class Visit> void VisitModelBuffers(const void* body, Visit visit)
{
    const auto* joints = Read<uint8_t*>(body, 0x538);
    const size_t count = joints ? Read<uint32_t>(joints - 0x10, 0) : 0;
    if (count && count <= 65536) {
        visit(size_t(0x538), count * 0xC0);
        visit(size_t(0x548), count);
        visit(size_t(0x11B0), count * 0x40);
        const size_t workCount = std::min(count, size_t(Read<uint32_t>(body, 0x530)));
        const size_t blends = std::min(size_t(Read<uint32_t>(body, 0x554)), size_t(8));
        for (size_t i = 0; i < blends; ++i) visit(0x6C0 + i * 0x180, workCount * 0x68);
    }
    const size_t channels = Read<uint32_t>(body, 0x1238);
    const size_t motions = std::min(size_t(Read<uint32_t>(body, 0x554)), size_t(8));
    if (channels <= 4096) visit(size_t(0x6D8), channels * motions * 0xC);
}

// Storage identity: the restored cursors and counts describe these buffers.
bool SameModelStorage(const void* live, const uint8_t* saved)
{
    bool same = true;
    for (size_t field : {size_t(0x530), size_t(0x550), size_t(0x554), size_t(0x558), size_t(0x1238)})
        same &= Read<uint32_t>(live, field) == Read<uint32_t>(saved, field);
    for (size_t field : {size_t(0x538), size_t(0x540), size_t(0x548), size_t(0x11B0)})
        same &= Read<uintptr_t>(live, field) == Read<uintptr_t>(saved, field);
    for (size_t motion = 0; motion < 8; ++motion)
        for (size_t field : {size_t(0x6C0), size_t(0x6D8)})
            same &= Read<uintptr_t>(live, field + motion * 0x180) == Read<uintptr_t>(saved, field + motion * 0x180);
    return same;
}

// Caller holds g_lock at the capture fence, after the shot's own leases. Adds
// each live model the shot names, once, and retains what the model points at.
void CollectDrivenModels(SnapshotManifest& manifest, const Identity& owner, uint32_t bit,
                         std::vector<Identity>& models)
{
    const auto add = [&](size_t, void* object) {
        if (!object || object == owner.object) return;
        const Identity* identity = Find(object, false);
        if (!identity || identity->occupancy != Occupancy::Live || !IsDrivenModel(*identity)) return;
        if (std::any_of(models.begin(), models.end(), [&](const Identity& model) { return model.object == object; }))
            return;
        models.push_back(*identity);
    };
    const size_t first = models.size();
    VisitShotUnitReferences(owner.object, add);
    VisitShotTailUnits(owner, add);
    for (size_t i = first; i < models.size(); ++i) {
        const Identity model = models[i]; // Leasing may adopt and move registry state.
        // Known units only: a model that is not being moved can still name a
        // unit that has since been freed, and adoption would dispatch through it.
        for (size_t field : ModelUnitFields) {
            void* target = Read<void*>(model.object, field);
            if (target && Find(target, false))
                LeaseUnitDependency(manifest, target, model.object, model.id, field, bit, false);
        }
    }
}

// Unit fence held, recorder lock released: plain copies of quiescent memory.
void CaptureDrivenModel(const Identity& model, ModelUnitState& saved)
{
    LifetimeFailureScope failureScope("model.capture", model.object, model.id);
    saved.object = model.object; saved.id = model.id; saved.vtable = Read<uintptr_t>(model.object, 0);
    saved.extent = model.extent;
    std::memcpy(saved.control, static_cast<uint8_t*>(model.object) + 8, sizeof(saved.control));
    saved.deltaTime = Read<float>(model.object, 0x28);
    saved.groups = Read<uint64_t>(model.object, 0x30);
    saved.spans.clear(); saved.payload.clear();
    const auto copy = [&](size_t field, void* address, size_t size) {
        LIFE_REQUIRE(size <= UINT32_MAX && saved.payload.size() <= UINT32_MAX - size);
        saved.spans.push_back({field, address, uint32_t(size), uint32_t(saved.payload.size())});
        const auto* bytes = static_cast<const uint8_t*>(address);
        saved.payload.insert(saved.payload.end(), bytes, bytes + size);
    };
    copy(0, model.object, model.extent);
    VisitModelBuffers(model.object, [&](size_t field, size_t size) {
        void* address = Read<void*>(model.object, field);
        if (address && size) copy(field, address, size);
    });
}

// Caller holds g_lock and the unit fence; no native call runs. Membership and
// revival are the restore transaction's; this writes the model's own state.
// Returns whether the pose and animation were rewound too.
bool RestoreDrivenModel(const ModelUnitState& saved, uint32_t bit)
{
    auto* live = static_cast<uint8_t*>(saved.object);
    LIFE_REQUIRE(!saved.spans.empty() && !saved.spans[0].field && saved.spans[0].address == saved.object &&
        saved.spans[0].size == saved.extent && saved.extent >= ModelLifecycleEnd);
    const uint8_t* body = saved.payload.data() + saved.spans[0].offset;
    void* current[std::size(ModelUnitFields)];
    for (size_t i = 0; i < std::size(ModelUnitFields); ++i) current[i] = Read<void*>(live, ModelUnitFields[i]);
    // A model set up again since the snapshot has other buffers; its saved
    // cursors would index them. Leave its animation live and rewind the rest.
    const bool storage = SameModelStorage(live, body);
    if (storage) {
        RestoreActorModelFields(live, body);
        for (size_t i = 1; i < saved.spans.size(); ++i) {
            const auto& span = saved.spans[i];
            std::memcpy(span.address, saved.payload.data() + span.offset, span.size);
        }
    }
    std::memcpy(live + ModelLifecycleOffset, body + ModelLifecycleOffset, ModelLifecycleEnd - ModelLifecycleOffset);
    // A saved link is usable only if this snapshot retained its target.
    for (size_t i = 0; i < std::size(ModelUnitFields); ++i) {
        const size_t field = ModelUnitFields[i];
        if (!storage && field != ModelLifecycleOffset) continue;
        void* target = Read<void*>(live, field);
        if (!target || target == current[i]) continue;
        const Identity* identity = Find(target, false);
        if (!identity || !(identity->snapshots & bit)) Write(live, field, (void*)nullptr);
    }
    std::memcpy(live + 8, saved.control, sizeof(saved.control));
    Write(live, 0x28, saved.deltaTime);
    Write(live, 0x30, saved.groups);
    return storage;
}

// Construction order of every uGameEffectModel, on the identity clock. A model
// is adopted (and numbered) only when something first leases it, which says
// nothing about when it was made.
std::unordered_map<void*, uint64_t> g_modelBirths;
void* (__fastcall* oConstructModel)(void*) = nullptr;
void (__fastcall* oDestroyModel)(void*) = nullptr;

void* __fastcall ConstructModel(void* model)
{
    void* result = oConstructModel(model);
    AcquireSRWLockExclusive(&g_lock);
    g_modelBirths[model] = ++g_nextId;
    ReleaseSRWLockExclusive(&g_lock);
    return result;
}

void __fastcall DestroyModel(void* model)
{
    AcquireSRWLockExclusive(&g_lock);
    g_modelBirths.erase(model);
    ReleaseSRWLockExclusive(&g_lock);
    oDestroyModel(model);
}

// Models of the branch being abandoned. A shot born after the restored frame
// is withdrawn; the model it created would stay on screen and keep animating.
// Only a model constructed after that frame qualifies: a shot can also name a
// model that an effect or character owns, and that one existed all along even
// if no shot named it when the frame was captured. Runs before the effect
// transaction, which removes the marked models from the scheduler.
void MarkAbandonedModels(int32_t frame)
{
    const unsigned slot = unsigned((frame % ROLLBACK_WINDOW + ROLLBACK_WINDOW) % ROLLBACK_WINDOW);
    std::vector<void*> candidates;
    AcquireSRWLockExclusive(&g_lock);
    const auto* saved = g_liveSnapshots[slot];
    if (!saved || !saved->valid || saved->frame != frame || !saved->unitManager) { ReleaseSRWLockExclusive(&g_lock); return; }
    const uint64_t watermark = saved->birthWatermark;
    for (const auto& shot : ActiveIdentities()) {
        if (shot.kind != Kind::Shot || shot.occupancy != Occupancy::Live || shot.extent < ShotCpuCommonSize) continue;
        VisitShotUnitReferences(shot.object, [&](size_t, void* object) {
            if (!object || object == shot.object) return;
            const auto born = g_modelBirths.find(object);
            if (born != g_modelBirths.end() && born->second > watermark &&
                std::find(candidates.begin(), candidates.end(), object) == candidates.end())
                candidates.push_back(object);
        });
    }
    // One made since the last capture has no identity yet. Native DTI dispatch
    // and hook registration stay outside the recorder lock.
    for (void* object : candidates) {
        if (Find(object, false)) continue;
        ReleaseSRWLockExclusive(&g_lock);
        Birth(object, Kind::Unit); RegisterUnit(object);
        AcquireSRWLockExclusive(&g_lock);
    }
    for (void* object : candidates) {
        Identity* identity = Find(object, false);
        if (identity && identity->occupancy == Occupancy::Live && IsDrivenModel(*identity))
            identity->abandonedModel = true;
    }
    ReleaseSRWLockExclusive(&g_lock);
}
