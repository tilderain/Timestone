"""Benchmark the lifetime tracer's snapshot save and restore against the test mocks.

Builds the production lifetime_trace.cpp with the mocks from test_lifetime_trace.py
at -O2, without sanitizers, and times save and restore over synthetic scenes of
effects (two-node graphs) and shots (24 joints, one attack root). Each figure is
the best batch average, so background load only ever raises it.

    python3 tools/bench_lifetime_trace.py                  # working tree
    python3 tools/bench_lifetime_trace.py --compare HEAD   # alternate with a revision
    python3 tools/bench_lifetime_trace.py --scene 30 4     # one scene: effects shots
    python3 tools/bench_lifetime_trace.py --keep bench.exe # keep the binary, e.g. for
        valgrind --tool=callgrind --toggle-collect='RestoreAll*' ./bench.exe 0 2 30 4
        (0 selects the benchmark; then 1 times saves only, 2 restores only)

The numbers are for comparing two versions of this code on one machine. They are
not game timings: the mocks have none of the game's cache behaviour.
"""

import argparse
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "tools"))
import test_lifetime_trace as harness  # noqa: E402

BENCH = r'''
#include <chrono>
#include <cstdio>
alignas(16) uint8_t units[0x1860]{}, manager[0x3450]{}, config[0x4A8A0]{};
constexpr uint32_t FxCategory = 13, ShotCategory = 0, ForeignCategory = 8;
struct Fx {
    alignas(16) uint8_t owner[0x2C0]{}, node[2][0x240]{}, storage[0x2A0 + 2 * 0x800]{}, resource[0xC0]{};
    Fx() {
        Write(owner, 0, uintptr_t(_addr(0x140A8E180)));
        Write(owner, 0x10, uint32_t(2 | (FxCategory << 3)));
        Write(owner, 0x108, (void*)resource); Write(owner, 0x130, uint32_t(0x01000000));
        Write(resource, 0x4C, uint32_t(1));
        Write(owner, 0x204, uint16_t(2)); Write(owner, 0x208, uint16_t(2));
        Write(owner, 0x218, (void*)node[0]); Write(owner, 0x220, (void*)storage);
        for (int i = 0; i < 2; ++i) {
            uint8_t* pool = storage + 0x2A0 + i * 0x800;
            Write(node[i], 0, uintptr_t(_addr(0x140BC4CB0))); Write(node[i], 0x10, (void*)owner);
            Write(node[i], 0x18, i == 0 ? (void*)node[1] : nullptr);
            Write(node[i], 0x28, (void*)(storage + i * 0x150)); Write(node[i], 0x34, uint32_t(0x800));
            Write(node[i], 0x38, (void*)pool); Write(node[i], 0xD4, int32_t(-1));
            Write(node[i], 0x110, uint16_t(8)); Write(node[i], 0x116, uint16_t(0x100));
            Write(node[i], 0xE0, (void*)pool);
            for (int p = 0; p < 4; ++p) Write(pool + p * 0x100, 8, p < 3 ? (void*)(pool + (p + 1) * 0x100) : nullptr);
            Write(storage + i * 0x150, 0x100, (void*)owner);
        }
        Birth(owner, Kind::Effect);
    }
    void tick() { Write(owner, 0x120, Read<uint32_t>(owner, 0x120) + 1); ++storage[0x2A0 + 0x20]; }
};
std::vector<Fx*> fx;
std::vector<uint8_t*> shots, foreign;
void* head(unsigned team) { return shotHeads[team] + 8; }
void* nodeOf(void* shot) { return static_cast<uint8_t*>(shot) + 0x1450; }
uint8_t* makeShot(unsigned id, size_t size, uint32_t jointCount) {
    auto* shot = static_cast<uint8_t*>(std::calloc(1, size));
    Write(shot, 0, uintptr_t(0xCAFE)); Write(shot, 0x10, uint32_t(2)); Write(shot, 0x116C, UINT32_MAX);
    Write(shot, 0x1450, uintptr_t(0xBEEF)); Write(shot, 0x1468, static_cast<void*>(shot));
    Write(shot, 0x1F50, id); Write(shot, 0x1F58, 3.0f); Write(shot, 0x2078, 97.0f);
    void* model = std::calloc(1, 0x100); Write(model, 0x4C, uint32_t(1)); Write(model, 0x78, jointCount);
    auto* joints = static_cast<uint8_t*>(std::calloc(1, 0x10 + jointCount * 0xC0));
    Write(joints, 0, jointCount);
    for (uint32_t i = 0; i < jointCount; ++i) {
        Write(joints + 0x10 + i * 0xC0, 0, uintptr_t(mockJointVtable)); Write(joints + 0x10 + i * 0xC0, 0xA8, (void*)shot);
    }
    Write(shot, 0x100, model); Write(shot, 0x530, jointCount); Write(shot, 0x538, joints + 0x10);
    Write(shot, 0x548, std::calloc(1, jointCount)); Write(shot, 0x11B0, std::calloc(1, jointCount * 0x40));
    // One attack root with two wrapper/primitive pairs.
    auto* root = static_cast<uint8_t*>(std::calloc(1, 0xE0));
    auto* array = static_cast<uint8_t*>(std::calloc(4, 8));
    for (int i = 0; i < 2; ++i) {
        auto* wrapper = static_cast<uint8_t*>(std::calloc(1, 0x60));
        auto* primitive = static_cast<uint8_t*>(std::calloc(1, 0xB0));
        static uintptr_t vt[] = {uintptr_t(+[](void* p, uint32_t) -> void* { return p; })};
        Write(wrapper, 0, uintptr_t(vt)); Write(primitive, 0, uintptr_t(vt));
        Write(primitive, 8, uint32_t(5)); Write(wrapper, 0x10, (void*)primitive); Write(array, i * 8, (void*)wrapper);
    }
    static uintptr_t rootVt[] = {uintptr_t(+[](void* p, uint32_t) -> void* { return p; })};
    Write(root, 0, uintptr_t(rootVt)); Write(root, 0x20, uint32_t(2)); Write(root, 0x24, uint32_t(4)); Write(root, 0x30, (void*)array);
    Write(shot, 0x2058, (void*)root);
    Birth(shot, Kind::Shot); auto* identity = Find(shot, false);
    identity->extent = uint32_t(size); identity->dti = 0x1234; identity->shotLinked = true;
    identity->deleting = +[](void* p, uint32_t) -> void* { return p; };
    Write(manager, 0x38 + id * 4, id);
    return shot;
}
void lineOf(unsigned category, const std::vector<void*>& entries) {
    void* descriptor = units + 0x50 + category * 0x30;
    Write(descriptor, 0, entries.empty() ? nullptr : entries.front());
    void* previous = nullptr;
    for (void* object : entries) {
        Write(object, 0x20, previous); Write(object, 0x18, (void*)nullptr);
        if (previous) Write(previous, 0x18, object);
        Write(object, 0x10, (Read<uint32_t>(object, 0x10) & ~0x3F8u) | (category << 3));
        previous = object;
    }
    Write(descriptor, 8, previous);
}
void groupOf(unsigned group, const std::vector<void*>& entries) {
    void* descriptor = units + 0xC40 + group * 0x30;
    void** data = static_cast<void**>(std::calloc(entries.size() + 32, 8));
    for (size_t i = 0; i < entries.size(); ++i) {
        data[i] = entries[i];
        Write(entries[i], 0x30, Read<uint64_t>(entries[i], 0x30) | (uint64_t(1) << group));
    }
    Write(descriptor, 0x18, uint32_t(entries.size())); Write(descriptor, 0x1C, uint32_t(entries.size() + 32));
    Write(descriptor, 0x28, (void*)data);
}
void build(int effects, int shotCount, int foreignCount, int constraints) {
    reset(); unitManager = units; shotManager = manager; gameConfig = config;
    Write(units, 0xC38, uint32_t(32)); Write(units, 0x1840, uint32_t(8));
    oInitializeConstraint = +[](void* object) {
        Write(object, 0, uintptr_t(0xC0A5)); Write(object, 0x40, uintptr_t(0xFACE));
        Write(object, 0x48, object); Write(object, 0x10, uint32_t(2)); Write(object, 0x70, int32_t(-1));
    };
    oConstraintObjectDtor = +[](void*) {};
    std::vector<void*> fxLine, shotLine, foreignLine;
    for (int i = 0; i < effects; ++i) { fx.push_back(new Fx); fxLine.push_back(fx.back()->owner); }
    for (int i = 0; i < shotCount; ++i) { shots.push_back(makeShot(5 + i, 0x2400, 24)); shotLine.push_back(shots.back()); }
    for (int i = 0; i < foreignCount; ++i) {
        foreign.push_back(static_cast<uint8_t*>(std::calloc(1, 0x40))); Write(foreign.back(), 0x10, uint32_t(2));
        foreignLine.push_back(foreign.back());
    }
    for (int i = 0; i < constraints; ++i) InitializeConstraint(std::calloc(1, 0x100));
    lineOf(FxCategory, fxLine); lineOf(ShotCategory, shotLine); lineOf(ForeignCategory, foreignLine);
    groupOf(1, fxLine); groupOf(2, shotLine); groupOf(3, foreignLine);
    std::vector<void*> everything = fxLine; everything.insert(everything.end(), shotLine.begin(), shotLine.end());
    everything.insert(everything.end(), foreignLine.begin(), foreignLine.end());
    groupOf(4, everything);
    void* previous = head(0);
    for (auto* shot : shots) { Write(previous, 8, nodeOf(shot)); Write(nodeOf(shot), 0x10, previous); previous = nodeOf(shot); }
    if (!shots.empty()) { Write(previous, 8, head(0)); Write(head(0), 0x10, head(0)); }
    Write(manager, 0x438, uint32_t(7)); Write(config, 0x4A89C, uint32_t(1));
}
void tick() {
    for (auto* e : fx) e->tick();
    for (auto* s : shots) { Write(s, 0x1F58, Read<float>(s, 0x1F58) + 1); ++Read<uint8_t*>(s, 0x538)[0x20]; }
}
double now() { return std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
int g_mode = 0;
[[gnu::noinline]] void SaveAll(int32_t frame) { LifetimeTrace_DropSnapshot(frame); LifetimeTrace_SaveSnapshot(frame); }
[[gnu::noinline]] void RestoreAll(int32_t target) {
    LifetimeTrace_CheckSnapshot(target);
    LifetimeTrace_RetireFutureShots(target); LifetimeTrace_RestoreEffects(target); LifetimeTrace_RestoreShots(target);
}
void run(const char* name, int effects, int shotCount) {
    fx.clear(); shots.clear(); foreign.clear();
    build(effects, shotCount, 60, 300);
    g_recording = false; // As in a match after the first 65536 events.
    int32_t frame = 0;
    for (; frame < 64; ++frame) { tick(); LifetimeTrace_DropSnapshot(frame); LifetimeTrace_SaveSnapshot(frame); LifetimeTrace_Flush(); }
    // Each figure is the best batch average: scheduling noise only ever adds time.
    const int batches = g_mode ? 2 : 20, saves = 200, restores = 40;
    double save = 1e30, flush = 1e30, restore = 1e30, resave = 1e30;
    for (int batch = 0; batch < batches && g_mode != 2; ++batch) {
        double s = 0, fl = 0;
        for (int i = 0; i < saves; ++i, ++frame) {
            tick();
            double t = now(); SaveAll(frame); s += now() - t;
            t = now(); LifetimeTrace_Flush(); fl += now() - t;
        }
        save = std::min(save, s / saves); flush = std::min(flush, fl / saves);
    }
    for (int batch = 0; batch < batches && g_mode != 1; ++batch) {
        double r = 0, rs = 0;
        for (int i = 0; i < restores; ++i) {
            const int32_t target = frame - 7;
            double t = now(); RestoreAll(target); r += now() - t;
            t = now();
            for (int32_t f = target + 1; f < frame; ++f) { tick(); SaveAll(f); }
            rs += now() - t;
            tick(); SaveAll(frame); LifetimeTrace_Flush(); ++frame;
        }
        restore = std::min(restore, r / restores); resave = std::min(resave, rs / (restores * 6));
    }
    size_t bytes = TestManifest(0).payloadBytes();
    for (const auto& shot : TestManifest(0).shots) bytes += shot.payload.size();
    {   // Copy floor: the same bytes, span by span, into a 16-slot ring.
        std::vector<std::vector<uint8_t>> ring(16, std::vector<uint8_t>(bytes * 2 + (1 << 20)));
        double best = 1e30;
        for (int rep = 0; rep < 5; ++rep) {
            double t = now();
            for (int i = 0; i < 400; ++i) {
                uint8_t* out = ring[i & 15].data();
                for (auto* e : fx) { std::memcpy(out, e->owner, 0x2C0); out += 0x2C0; std::memcpy(out, e->node, sizeof(e->node)); out += sizeof(e->node);
                    std::memcpy(out, e->storage, sizeof(e->storage)); out += sizeof(e->storage); }
                for (auto* sh : shots) { std::memcpy(out, sh, 0x2400); out += 0x2400; std::memcpy(out, Read<void*>(sh, 0x538), 24 * 0xC0); out += 24 * 0xC0; }
            }
            best = std::min(best, (now() - t) / 400);
        }
        std::printf("         byte-copy floor for the scene below ~%.2f us\n", best);
    }
    std::printf("%-8s effects=%3d shots=%2d payload=%7zu B  save=%7.2f us  flush=%5.2f us  restore=%7.2f us  resim_save=%7.2f us\n",
        name, effects, shotCount, bytes, save, flush, restore, resave);
    LifetimeTrace_ClearSnapshots();
}
int main(int argc, char** argv) {
    if (argc > 1) { g_mode = std::atoi(argv[1]); run("prof", argc > 2 ? std::atoi(argv[2]) : 30, argc > 3 ? std::atoi(argv[3]) : 4); return 0; }
    run("idle", 0, 0); run("light", 10, 1); run("typical", 30, 4); run("heavy", 120, 35);
    return 0; // Not the real main once merged into the harness: no implicit return.
}
'''


def build(root, exe):
    """Compile the tracer found under `root` with the benchmark as its only check."""
    harness.ROOT = root
    checks = harness.CHECKS[:harness.CHECKS.index("int main() {")] + BENCH
    checks = checks.replace("LifetimeTrace_SaveSnapshot(", "SaveTestSnapshot(")
    unit = harness.LifetimeTraceTests._unit((True, True, ""), [checks])
    with tempfile.TemporaryDirectory(prefix="lifetime-bench-") as directory:
        cpp = Path(directory) / "bench.cpp"
        cpp.write_text(unit)
        subprocess.run(["g++", "-std=c++20", "-O2", "-g", "-fno-omit-frame-pointer", "-w", "-pthread",
                        str(cpp), "-o", str(exe)], check=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--compare", metavar="REV", help="also benchmark this git revision, alternating runs")
    parser.add_argument("--rounds", type=int, default=2, help="alternations with --compare (default 2)")
    parser.add_argument("--scene", nargs=2, type=int, metavar=("EFFECTS", "SHOTS"), help="time one scene only")
    parser.add_argument("--keep", metavar="PATH", help="copy the working-tree binary here")
    args = parser.parse_args()
    scene = ["0", str(args.scene[0]), str(args.scene[1])] if args.scene else []

    with tempfile.TemporaryDirectory(prefix="lifetime-bench-") as directory:
        directory = Path(directory)
        builds = [("working tree", ROOT)]
        if args.compare:
            # The revision's tracer sources, built with the current mocks and benchmark.
            base = directory / "base"
            base.mkdir()
            archive = subprocess.run(["git", "archive", args.compare, "UMVC3Hook/code"], cwd=ROOT,
                                     check=True, capture_output=True).stdout
            subprocess.run(["tar", "-x", "-C", str(base)], input=archive, check=True)
            builds.insert(0, (args.compare, base))
        binaries = []
        for index, (label, root) in enumerate(builds):
            exe = directory / f"bench{index}"
            build(root, exe)
            binaries.append((label, exe))
        if args.keep:
            shutil.copy(binaries[-1][1], args.keep)
        for _ in range(args.rounds if args.compare else 1):
            for label, exe in binaries:
                print(f"== {label}", flush=True)
                subprocess.run([str(exe), "0", *scene], check=True)


if __name__ == "__main__":
    main()
