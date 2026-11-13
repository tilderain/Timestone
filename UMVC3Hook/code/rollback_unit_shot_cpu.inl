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
// uGameEffectModel both end it at +1450) is the ActorShot column of the table in
// rollback_character_restore.h. Storage staging restores the owned work (+160)
// and key-slab aliases (+178) of each Motion record; the table rewinds bindings
// and cached cursors under retained motion resources.

ShotCpuRestoreStatus RestoreShotCpuFields(void* destination, size_t destinationSize,
    const uint8_t* saved, size_t savedSize)
{
    if (!destination || !saved || destinationSize < ShotCpuCommonSize || savedSize < ShotCpuCommonSize)
        return ShotCpuRestoreStatus::InvalidBuffer;
    auto* live = static_cast<uint8_t*>(destination);
    if (live == saved) return ShotCpuRestoreStatus::ReviewedSubset;

    // Caller must establish compatible retained resources/pose storage and repair
    // references/topology before publishing execution. No native calls run here.
    Rollback_RestoreActorFields(live, saved, ActorShot);
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
    // Expanded like the actor rows: fixed-size copies, not a memcpy call per range.
    [&]<size_t... I>(std::index_sequence<I...>) {
        ((std::memcpy(live + ranges[I].offset, saved + ranges[I].offset, ranges[I].size)), ...);
    }(std::make_index_sequence<std::size(ranges)>{});

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
    // The owner's identity and lease were checked by RollbackUnit_RestoreShots.
    const ShotSpan* body = &ShotBodySpan(saved, owner.object);
    LIFE_REQUIRE(body->memory.size >= ShotCpuCommonSize && body->memory.size <= owner.extent);
    AcquireSRWLockExclusive(&g_lock);
    // Each unit the saved bytes name must still be an allocation this snapshot
    // holds: the pointers are about to be written back as they were.
    VisitShotUnitReferences(saved.payload.data() + body->memory.offset, [&](size_t, void* object) {
        if (!object || object == owner.object) return;
        const Identity* target = Find(object, false);
        LIFE_REQUIRE(target && (target->snapshots & bit) &&
            !(target->occupancy != Occupancy::Live && target->occupancy != Occupancy::Quarantined));
    });
    ReleaseSRWLockExclusive(&g_lock);
    const auto status = RestoreShotCpuFields(owner.object, owner.extent,
        saved.payload.data() + body->memory.offset, body->memory.size);
    LIFE_REQUIRE(status != ShotCpuRestoreStatus::InvalidBuffer);
    return status;
}
