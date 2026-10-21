// Native DTI, geometry traversal and generator capture are deterministic fixture
// substitutes. Results measure storage/reset costs, NOT complete recipe capture.
static uint64_t GetDti(void* p) { return p ? *(uint64_t*)p : 0; }
static uint32_t DtiSize(uint64_t dti) { return dti == 0x3000 ? 0x3000 : 0; }
static bool IsDtiKindOf(uint64_t dti, uint64_t base) { return dti == base; }
static void CaptureCollisionPrimitives(const uint8_t*, ConcreteShotSnapshot& out) {
    out.collisionPrimitives.resize(4);
    for (size_t i = 0; i < 4; ++i) out.collisionPrimitives[i].index = i;
}
static bool CaptureGeneratorEmissionState(void*, ConcreteShotSnapshot::CallbackEffectState& state) {
    state.generatorEmissionStates.clear();
    state.generatorEmissionStates.resize(4);
    return true;
}
