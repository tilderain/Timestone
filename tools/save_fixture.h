// Portable fixture support, not game implementations.
struct RollbackSnapshotSlot {
    std::vector<RollbackShotRecipe> shotRecipes;
    std::vector<RollbackEffectRecipe> effectRecipes;
    std::vector<RollbackCharacterCollisionParentReference> characterCollisionParentReferences;
    std::vector<RollbackModelRenderState> modelRenderStates;
};
static uint64_t manager = 0, units = 0;
static uintptr_t _addr(uintptr_t p) {
    if (p == PTR_SCHARACTER) return (uintptr_t)&manager;
    if (p == PTR_SUNIT) return (uintptr_t)&units;
    std::abort();
}
template<class T, size_t N> struct SmallSet {
    std::array<T, N> values{};
    size_t size = 0;
    std::pair<size_t, bool> insert(T p) {
        for (size_t i = 0; i < size; ++i) if (values[i] == p) return {i, false};
        if (size == N) std::abort(); // Bounds derive from the original traversal limits.
        values[size++] = p;
        return {size - 1, true};
    }
    size_t count(T p) const {
        return std::find(values.begin(), values.begin() + size, p) != values.begin() + size;
    }
};
template<size_t N> struct List {
    std::array<uint64_t, N> data{};
    size_t size = 0;
    void add(uint64_t p) { if (size == N) std::abort(); data[size++] = p; }
    auto begin() const { return data.begin(); }
    auto end() const { return data.begin() + size; }
};
struct Inventory {
    List<32> roster;
    List<16> renderRoster;
    List<64> units;
    SmallSet<void*, 96> all;
};
NOINLINE static Inventory gather() {
    Inventory out;
    for (int team = 0; manager && team < 2; ++team) {
        uint64_t node = manager + (team ? TEAM1_OFFSET : TEAM0_OFFSET) + TEAM_FIRST_NODE_OFFSET;
        for (int i = 0; node && i < 16; ++i) {
            uint64_t p = *(uint64_t*)(node + NODE_CHR_OFFSET);
            out.roster.add(p);
            if (i < 8) out.renderRoster.add(p);
            if (p) out.all.insert((void*)p);
            node = *(uint64_t*)(node + NODE_NEXT_OFFSET);
        }
    }
    if (units && *(uint32_t*)(units + SUNIT_CATEGORY_COUNT_OFFSET) > SUNIT_CHARACTER_CATEGORY) {
        uint64_t p = *(uint64_t*)(units + SUNIT_CATEGORY_LISTS_OFFSET +
                                SUNIT_CATEGORY_LIST_STRIDE * SUNIT_CHARACTER_CATEGORY);
        for (int i = 0; p && i < 64; ++i) {
            out.units.add(p);
            out.all.insert((void*)p);
            p = *(uint64_t*)(p + 0x18);
        }
    }
    return out;
}
