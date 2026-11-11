// Included in the recorder namespace. One common uShot kernel, with no concrete
// DTI admission or live/quarantined split. This is not a whole-shot transaction.
enum class ShotCpuRestoreStatus { InvalidBuffer, ReviewedSubset };
constexpr size_t ShotCpuCommonSize = 0x2140;

// Only native cUnit references, not resource pointers, opaque words or links.
// Shared by snapshot leases and collection fencing; never follow saved pointees.
template<class Visitor> void VisitShotUnitReferences(const void* body, Visitor visit)
{
    for (size_t field : {size_t(0x40), size_t(0x1308), size_t(0x13D8),
                        size_t(0x1480), size_t(0x1E30), size_t(0x1E38), size_t(0x1E40)})
        visit(field, Read<void*>(body, field));
    for (size_t field = 0x1F70; field < 0x1FE0; field += 8)
        visit(field, Read<void*>(body, field));
    for (size_t field = 0x2000; field < 0x2038; field += 8)
        visit(field, Read<void*>(body, field));
    for (size_t record = 0x14C0; record < 0x1DC0; record += 0x18)
        visit(record + 8, Read<void*>(body, record + 8));
}

// The uActorGameModel prefix every model-driven unit shares (uShot and
// uGameEffectModel both end it at +1450). Same contract as the shot kernel below.
void RestoreActorModelFields(uint8_t* live, const uint8_t* saved)
{
    static constexpr struct { size_t offset, size; } ranges[] = {
        {0x08, 0x08},     // Rno/unit parameter; lifecycle/category/draw mode stay live.
        {0x28, 0x04},     // Unit delta; no list links or group mask.
        {0x40, 0xC0},     // Borrowed parent, selectors, vectors, local/world matrices.
        {0x55C, 0x04},    // Motion interpolation attributes, not allocation bounds.
        {0x1180, 0x30},   // Null translation scale and root-motion delta vectors/quaternion.
        {0x1240, 0x10},   // Inline morph weights, not shader owners at +1250.
        {0x1260, 0x3F},   // Plane/waist/foot/IK parameters; no map owners or padding.
        {0x1300, 0x10},   // Actor time coefficients and borrowed owner.
        {0x1310, 0x34},   // Actor animation requests, time/rate/wait/serial.
        {0x1350, 0x30},   // Interpreter animation/control and borrowed resource mirrors.
        {0x1388, 0x30},   // Type/key and resource-backed entry/descriptor/block cursors.
        {0x13C0, 0x01},   // Command key-change latch.
        {0x13C4, 0x10},   // Compare/jump registers and return flags; no alignment gap.
        {0x13D8, 0x10},   // Borrowed unit return value and command loop state.
    };
    for (const auto& range : ranges) std::memcpy(live + range.offset, saved + range.offset, range.size);

    // Storage staging restores owned work (+160) and key-slab aliases (+178).
    // Here bindings and cached cursors rewind under retained motion resources;
    // preserve sampler alignment gaps and the Motion vtable.
    for (size_t motion = 0; motion < 8; ++motion) {
        const size_t base = 0x560 + motion * 0x180;
        std::memcpy(live + base + 8, saved + base + 8, 6);
        std::memcpy(live + base + 0x10, saved + base + 0x10, 0x64);
        std::memcpy(live + base + 0x80, saved + base + 0x80, 0x60);
        for (size_t sampler : {size_t(0xE0), size_t(0x100)}) {
            std::memcpy(live + base + sampler, saved + base + sampler, 0xC);
            std::memcpy(live + base + sampler + 0x10, saved + base + sampler + 0x10, 0xC);
        }
        std::memcpy(live + base + 0x120, saved + base + 0x120, 0x40);
        std::memcpy(live + base + 0x168, saved + base + 0x168, 4);
        std::memcpy(live + base + 0x170, saved + base + 0x170, 8);
    }
}

ShotCpuRestoreStatus RestoreShotCpuFields(void* destination, size_t destinationSize,
    const uint8_t* saved, size_t savedSize)
{
    if (!destination || !saved || destinationSize < ShotCpuCommonSize || savedSize < ShotCpuCommonSize)
        return ShotCpuRestoreStatus::InvalidBuffer;
    auto* live = static_cast<uint8_t*>(destination);
    if (live == saved) return ShotCpuRestoreStatus::ReviewedSubset;

    // Caller must establish compatible retained resources/pose storage and repair
    // references/topology before publishing execution. No native calls run here.
    RestoreActorModelFields(live, saved);
    static constexpr struct { size_t offset, size; } ranges[] = {
        {0x1480, 0x0C},   // cAtkCtrl borrowed owner and reset-state dword.
        {0x1490, 0x0C},   // Borrowed rAtkInfo from retained rShot and attack index.
        {0x14A0, 0x20},   // Combo count, attack clocks, animation/pending attack IDs.
        {0x1DD0, 0x60},   // Inline cArm orientation/offset/position/error vectors.
        {0x1E30, 0x120},  // cArm self/parents, joint selectors and four shot matrices.
        {0x1F58, 0x10},   // Timer/delay/division start/active-watch flags; not ID/team/resource.
        {0x1F70, 0x70},   // Fourteen borrowed callbacks, including self slot 0.
        {0x1FE4, 0x1C},   // Common gameplay/contact/end flags; borrowed units start at +2000.
        {0x2000, 0x38},   // Seven borrowed units, including the actor-owner mirror.
        {0x2038, 0x20},   // Division/durability/hit clock/priority; not collision-root pointers.
        {0x2078, 0x40},   // Life/vanish and movement/homing parameters/accumulators.
        {0x20C0, 0x0C},   // Contact-displacement XYZ; do not infer the fourth word.
        {0x20D0, 0x04},   // Contact-displacement scale; sound cRefs begin at +20D8.
        {0x2118, 0x1B},   // Animation watch controls, phase/dispatch word and three child latches.
    };
    for (const auto& range : ranges) std::memcpy(live + range.offset, saved + range.offset, range.size);

    // cAtkCtrl::update emits attack sound behind bit 40. uShot's initial sound
    // creation/stop uses bit 10000000 and the four live sound cRefs. Do not rewind
    // these two audio latches independently of their external sound state.
    constexpr uint32_t attackSound = 0x40, soundCookies = 0x10000000;
    Write(live, 0x149C, (Read<uint32_t>(saved, 0x149C) & ~attackSound) |
        (Read<uint32_t>(live, 0x149C) & attackSound));
    Write(live, 0x1FE0, (Read<uint32_t>(saved, 0x1FE0) & ~soundCookies) |
        (Read<uint32_t>(live, 0x1FE0) & soundCookies));
    // TargetInfo owns no reference. Rewind target/count/time together while
    // preserving each constructed inline vtable (native reset does the same).
    for (size_t record = 0x14C0; record < 0x1DC0; record += 0x18)
        std::memcpy(live + record + 8, saved + record + 8, 0x10);
    return ShotCpuRestoreStatus::ReviewedSubset;
}

// Caller owns the quiescent native fence for the complete restore transaction.
// Validating an outer allocation lease is not publication of its dependencies.
[[maybe_unused]] ShotCpuRestoreStatus StageShotCpuFields(const Identity& owner, const ShotState& saved, uint32_t bit)
{
    RollbackUnitFailureScope failureScope("shot.cpu.stage", owner.object, owner.id);
    LIFE_REQUIRE(saved.payloadState == RollbackUnitShotPayload::Capture);
    LIFE_REQUIRE(owner.kind == Kind::Shot && saved.id == owner.id && bit && !(bit & (bit - 1)) &&
        (owner.snapshots & bit) && !(owner.occupancy != Occupancy::Live && owner.occupancy != Occupancy::Quarantined));
    const ShotSpan* body = &ShotBodySpan(saved, owner.object);
    LIFE_REQUIRE(body->memory.size >= ShotCpuCommonSize && body->memory.size <= owner.extent);
    unsigned slot = 0;
    while ((uint32_t(1) << slot) != bit) ++slot;
    LIFE_REQUIRE(slot < ROLLBACK_WINDOW);
    AcquireSRWLockExclusive(&g_lock);
    const auto* manifest = g_liveSnapshots[slot];
    LIFE_REQUIRE(manifest && manifest->valid && manifest->shotPayload == RollbackUnitShotPayload::Capture &&
        std::any_of(manifest->shots.begin(), manifest->shots.end(),
            [&](const ShotState& shot) { return &shot == &saved; }));
    VisitShotUnitReferences(saved.payload.data() + body->memory.offset, [&](size_t, void* object) {
        if (!object || object == owner.object) return;
        const Identity* target = Find(object, false);
        LIFE_REQUIRE(target && (target->snapshots & bit) &&
            !(target->occupancy != Occupancy::Live && target->occupancy != Occupancy::Quarantined) &&
            std::any_of(manifest->members.begin(), manifest->members.end(), [&](const Identity& member) {
                return member.object == object && member.id == target->id;
            }));
    });
    ReleaseSRWLockExclusive(&g_lock);
    const auto status = RestoreShotCpuFields(owner.object, owner.extent,
        saved.payload.data() + body->memory.offset, body->memory.size);
    LIFE_REQUIRE(status != ShotCpuRestoreStatus::InvalidBuffer);
    return status;
}
