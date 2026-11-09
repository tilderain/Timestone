// Included in the recorder namespace. One common uShot kernel, with no concrete
// DTI admission or live/quarantined split. This is not a whole-shot transaction.
enum class ShotCpuRestoreStatus { InvalidBuffer, ReviewedSubset };
constexpr size_t ShotCpuCommonSize = 0x2140;

ShotCpuRestoreStatus RestoreShotCpuFields(void* destination, size_t destinationSize,
    const uint8_t* saved, size_t savedSize)
{
    if (!destination || !saved || destinationSize < ShotCpuCommonSize || savedSize < ShotCpuCommonSize)
        return ShotCpuRestoreStatus::InvalidBuffer;
    auto* live = static_cast<uint8_t*>(destination);
    if (live == saved) return ShotCpuRestoreStatus::ReviewedSubset;

    // Caller must establish compatible retained resources/pose storage and repair
    // references/topology before publishing execution. No native calls run here.
    static constexpr struct { size_t offset, size; } ranges[] = {
        {0x08, 0x08},     // Rno/unit parameter; lifecycle/category/draw mode stay live.
        {0x28, 0x04},     // Unit delta; no list links or group mask.
        {0x48, 0xB8},     // Coordinate selectors, vectors, local/world matrices; no parent.
        {0x55C, 0x04},    // Motion interpolation attributes, not allocation bounds.
        {0x1180, 0x30},   // Null translation scale and root-motion delta vectors/quaternion.
        {0x1240, 0x10},   // Inline morph weights, not shader owners at +1250.
        {0x1260, 0x3F},   // Plane/waist/foot/IK parameters; no map owners or padding.
        {0x1300, 0x08},   // Actor time coefficients; owner at +1308 stays live.
        {0x1310, 0x34},   // Actor animation requests, time/rate/wait/serial.
        {0x1350, 0x10},   // Interpreter animation numbers and control bytes.
        {0x13C0, 0x01},   // Command key-change latch.
        {0x13C4, 0x0C},   // Compare registers/jump count; return flags/value are paired elsewhere.
        {0x13E0, 0x08},   // Command loop count/start frame; no owned command resources.
        {0x1488, 0x04},   // cAtkCtrl reset-state dword, not its resource/owner.
        {0x1498, 0x04},   // Attack-info index; resource binding is a transaction precondition.
        {0x14A0, 0x20},   // Combo count, attack clocks, animation/pending attack IDs.
        {0x1DD0, 0x60},   // Inline cArm orientation/offset/position/error vectors.
        {0x1E48, 0x108},  // cArm joint selectors and four inline shot matrices.
        {0x1F58, 0x10},   // Timer/delay/division start/active-watch flags; not ID/team/resource.
        {0x1FE4, 0x1C},   // Common gameplay/contact/end flags; borrowed units start at +2000.
        {0x2038, 0x20},   // Division/durability/hit clock/priority; not collision-root pointers.
        {0x2078, 0x40},   // Life/vanish and movement/homing parameters/accumulators.
        {0x20C0, 0x0C},   // Contact-displacement XYZ; do not infer the fourth word.
        {0x20D0, 0x04},   // Contact-displacement scale; sound cRefs begin at +20D8.
        {0x2118, 0x1B},   // Animation watch controls, phase/dispatch word and three child latches.
    };
    for (const auto& range : ranges) std::memcpy(live + range.offset, saved + range.offset, range.size);

    // Native Motion samplers (+E0/+100), event bindings/history (+120..160),
    // work/key pointers and key bounds are a separate resource-backed adapter.
    for (size_t motion = 0; motion < 8; ++motion) {
        const size_t base = 0x560 + motion * 0x180;
        std::memcpy(live + base + 8, saved + base + 8, 6);
        std::memcpy(live + base + 0x10, saved + base + 0x10, 0x64);
        std::memcpy(live + base + 0x80, saved + base + 0x80, 0x60);
    }

    // cAtkCtrl::update emits attack sound behind bit 40. uShot's initial sound
    // creation/stop uses bit 10000000 and the four live sound cRefs. Do not rewind
    // these two audio latches independently of their external sound state.
    constexpr uint32_t attackSound = 0x40, soundCookies = 0x10000000;
    Write(live, 0x149C, (Read<uint32_t>(saved, 0x149C) & ~attackSound) |
        (Read<uint32_t>(live, 0x149C) & attackSound));
    Write(live, 0x1FE0, (Read<uint32_t>(saved, 0x1FE0) & ~soundCookies) |
        (Read<uint32_t>(live, 0x1FE0) & soundCookies));
    // Entire TargetInfo records remain live here. Their target+count+time must
    // be restored together after identity leases, never as scalar counters alone.
    return ShotCpuRestoreStatus::ReviewedSubset;
}

// Caller owns the quiescent native fence for the complete restore transaction.
// Validating an outer allocation lease is not publication of its dependencies.
[[maybe_unused]] ShotCpuRestoreStatus StageShotCpuFields(const Identity& owner, const ShotState& saved, uint32_t bit)
{
    if (owner.kind != Kind::Shot || saved.id != owner.id || !bit || (bit & (bit - 1)) ||
        !(owner.snapshots & bit) || (owner.occupancy != Occupancy::Live && owner.occupancy != Occupancy::Quarantined))
        std::abort();
    const ShotSpan* body = nullptr;
    for (const auto& span : saved.spans) {
        if (std::strcmp(span.name, "body")) continue;
        if (body || span.memory.address != owner.object || span.memory.size > owner.extent ||
            span.memory.size < ShotCpuCommonSize || span.memory.offset > saved.payload.size() ||
            span.memory.size > saved.payload.size() - span.memory.offset) std::abort();
        body = &span;
    }
    if (!body) std::abort();
    const auto status = RestoreShotCpuFields(owner.object, owner.extent,
        saved.payload.data() + body->memory.offset, body->memory.size);
    if (status == ShotCpuRestoreStatus::InvalidBuffer) std::abort();
    return status;
}
