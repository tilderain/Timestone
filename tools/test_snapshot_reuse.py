#!/usr/bin/env python3
"""Native checks for snapshot region reuse: a saved buffer always equals live memory."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
CODE = ROOT / "UMVC3Hook/code"

HARNESS = r'''
#include <cassert>
#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <vector>
int logs = 0;
void HookLog(const char*, ...) { ++logs; }

constexpr size_t Joints = 40, BodySize = 0x1600;
struct Actor {
    std::vector<uint8_t> body = std::vector<uint8_t>(BodySize), joints = std::vector<uint8_t>(Joints * 0xC0);
    std::vector<uint8_t> blend[2] = {std::vector<uint8_t>(Joints * 0x68), std::vector<uint8_t>(Joints * 0x68)};
    std::vector<uint8_t> table = std::vector<uint8_t>(2 * 0xBB0);
};
Actor actors[2];
uint8_t misc[64];
std::vector<SnapshotReuseRegion> plan;
size_t total = 0;

void build() {
    plan.clear();
    plan.push_back({misc, sizeof(misc), 0, SnapshotReuseKind::None, 0});
    for (Actor& a : actors) {
        const uint64_t base = uint64_t(uintptr_t(a.body.data()));
        plan.push_back({a.body.data(), a.body.size(), 0, SnapshotReuseKind::ActorBody, 0});
        plan.push_back({a.table.data(), a.table.size(), base + 0x1408, SnapshotReuseKind::MaterialTable, 0});
        for (uint8_t m = 0; m < 2; ++m)
            plan.push_back({a.blend[m].data(), a.blend[m].size(), base + 0x6C0 + m * 0x180,
                            SnapshotReuseKind::MotionBlend, m});
        plan.push_back({a.joints.data(), a.joints.size(), base + 0x538, SnapshotReuseKind::Prefix, 0});
    }
    total = 0;
    for (const auto& region : plan) total += region.size;
    SnapshotReuse_SetPlan(plan.data(), int(plan.size()));
}
bool matches(const uint8_t* buffer) {
    size_t offset = 0;
    for (const auto& region : plan) {
        if (std::memcmp(buffer + offset, region.ptr, region.size)) return false;
        offset += region.size;
    }
    return true;
}
// One simulated frame that honours the signals the reuse rules read.
void tick(Actor& a, unsigned what) {
    ++misc[0];
    ++a.body[0x1500];                                             // Fighter state past the prefix.
    if (what & 1) { ++a.body[0x560 + 0x10]; ++a.blend[0][8]; ++a.joints[4]; }   // Motion slot 0 advances.
    if (what & 2) { ++a.body[0x560 + 0x180 + 0x10]; ++a.blend[1][8]; ++a.joints[9]; }
    if (what & 4) { ++a.body[0x40]; ++a.joints[0x20]; }           // Transform moves the pose only.
    uint8_t& active = a.table[0xBB0 + 0xB64];
    if (what & 8) { active |= 1; ++a.table[0xBB0 + 0x40]; }       // A job runs.
    else if ((what & 16) && (active & 1)) { active &= ~1; ++a.table[0xBB0 + 0x60]; }   // It finishes this frame.
}
'''


class SnapshotReuseTest(unittest.TestCase):
    def run_native(self, mode, main):
        header = (CODE / "snapshot_reuse.h").read_text().replace("#pragma once", "")
        setting = "SNAPSHOT_REUSE_MODE = SnapshotReuseMode::Off"
        self.assertIn(setting, header)
        header = header.replace(setting, f"SNAPSHOT_REUSE_MODE = SnapshotReuseMode::{mode}")
        source = (CODE / "snapshot_reuse.cpp").read_text().replace('#include "snapshot_reuse.h"', "")
        Path("/tmp/opencode").mkdir(exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="snapshot-reuse-", dir="/tmp/opencode") as out:
            cpp, binary = Path(out) / "test.cpp", Path(out) / "test"
            cpp.write_text(header + source + HARNESS + main)
            subprocess.run(["g++", "-std=c++20", "-g1", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", str(cpp), "-o", str(binary)], check=True)
            # This GCC's ASan intermittently faults at startup under high-entropy ASLR.
            subprocess.run(["setarch", platform.machine(), "-R", str(binary)], check=True, timeout=60)

    FOLLOWS_LIVE = r'''
int main() {
    build();
    std::vector<std::vector<uint8_t>> ring(5, std::vector<uint8_t>(total, 0xEE));
    unsigned seed = 12345;
    auto next = [&] { seed = seed * 1664525u + 1013904223u; return seed >> 8; };
    int frame = 0;
    for (; frame < 4000; ++frame) {
        // Actor 0 is busy in bursts; actor 1 is a background character.
        tick(actors[0], (frame / 50) % 2 ? next() & 31 : 0);
        if (frame % 700 == 0) tick(actors[1], 1);
        uint8_t* buffer = ring[frame % ring.size()].data();
        assert(SnapshotReuse_Save(buffer) == total);
        assert(matches(buffer));
        if (frame % 333 == 0) {
            // Restore: live memory jumps back to an older snapshot, with no signal.
            // Blend buffers are deliberately not reported as restored exactly.
            const uint8_t* saved = ring[(frame + 2) % ring.size()].data();
            size_t offset = 0;
            SnapshotReuse_BeginRestore();
            for (size_t i = 0; i < plan.size(); ++i) {
                std::memcpy(const_cast<uint8_t*>(plan[i].ptr), saved + offset, plan[i].size);
                if (plan[i].kind != SnapshotReuseKind::MotionBlend) SnapshotReuse_RegionRestored(int(i));
                offset += plan[i].size;
            }
            if (frame % 666) SnapshotReuse_EndRestore(saved);       // Some restores bail out early.
        }
        if (frame == 1500) build();                               // Region list rebuilt.
        if (frame == 2500) {                                      // A buffer rewritten by someone else.
            std::memset(ring[0].data(), 0x11, total);
            SnapshotReuse_ForgetBuffer(ring[0].data());
        }
    }
    const SnapshotReuseStats& stats = SnapshotReuse_Stats();
    assert(stats.saves == 4000 && !stats.mismatches && !logs);
    assert(stats.copiedBytes + stats.reusedBytes == uint64_t(total) * 4000);
    EXPECT_REUSE;
}
'''

    def test_saved_bytes_follow_live_memory(self):
        for mode in ("Validate", "Trust"):
            with self.subTest(mode=mode):
                # The background actor's buffers and idle slots dominate: most bytes are reused.
                self.run_native(mode, self.FOLLOWS_LIVE.replace(
                    "EXPECT_REUSE", "assert(stats.reusedBytes > stats.copiedBytes)"))

    def test_off_copies_everything(self):
        self.run_native("Off", self.FOLLOWS_LIVE.replace("EXPECT_REUSE", "assert(!stats.reusedBytes)"))

    def test_material_job_that_ran_is_recopied_after_it_stops(self):
        self.run_native("Trust", r'''
int main() {
    build();
    std::vector<uint8_t> a(total), b(total);
    Actor& actor = actors[0];
    SnapshotReuse_Save(a.data()); SnapshotReuse_Save(b.data());
    tick(actor, 8);  SnapshotReuse_Save(a.data()); assert(matches(a.data()));
    tick(actor, 16); SnapshotReuse_Save(b.data()); assert(matches(b.data()));   // Inactive now, active before.
    tick(actor, 0);  SnapshotReuse_Save(a.data()); assert(matches(a.data()));   // Buffer a predates the stop.
    // A job reconfigured without ever being seen active: its control block differs.
    ++actor.table[0xB90];
    SnapshotReuse_Save(b.data()); assert(matches(b.data()));
}
''')

    WRONG_SIGNAL = r'''
int main() {
    build();
    std::vector<uint8_t> buffer(total);
    SnapshotReuse_Save(buffer.data()); SnapshotReuse_Save(buffer.data());
    assert(matches(buffer.data()));
    ++actors[1].joints[7];                                        // Pose changed with an unchanged prefix.
    SnapshotReuse_Save(buffer.data());
    CHECK;
}
'''

    def test_validate_repairs_and_reports_a_wrong_signal(self):
        self.run_native("Validate", self.WRONG_SIGNAL.replace(
            "CHECK", "assert(matches(buffer.data()) && SnapshotReuse_Stats().mismatches == 1 && logs == 1)"))

    def test_trust_really_skips(self):
        self.run_native("Trust", self.WRONG_SIGNAL.replace(
            "CHECK", "assert(!matches(buffer.data()) && !SnapshotReuse_Stats().mismatches)"))

    def test_restore_keeps_unchanged_regions_reusable(self):
        self.run_native("Trust", r'''
void restore(const uint8_t* saved, bool exact) {
    size_t offset = 0;
    SnapshotReuse_BeginRestore();
    for (size_t i = 0; i < plan.size(); ++i) {
        std::memcpy(const_cast<uint8_t*>(plan[i].ptr), saved + offset, plan[i].size);
        if (exact) SnapshotReuse_RegionRestored(int(i));
        offset += plan[i].size;
    }
    SnapshotReuse_EndRestore(saved);
}
int main() {
    build();
    std::vector<std::vector<uint8_t>> ring(4, std::vector<uint8_t>(total));
    int frame = 0;
    auto save = [&] { uint8_t* b = ring[frame++ % 4].data(); SnapshotReuse_Save(b); assert(matches(b)); };
    for (int i = 0; i < 8; ++i) { tick(actors[0], 1); save(); }
    // Roll back three frames and replay them: actor 1 never moved, so none of
    // its buffers is copied again, and actor 0's idle slot is not either.
    frame -= 3;
    restore(ring[(frame - 1) % 4].data(), true);
    uint64_t before = SnapshotReuse_Stats().copiedBytes;
    for (int i = 0; i < 3; ++i) { tick(actors[0], 1); save(); }
    const uint64_t replay = SnapshotReuse_Stats().copiedBytes - before;
    const size_t idle = actors[1].joints.size() + 2 * actors[1].blend[0].size() + actors[1].table.size() +
                        actors[0].blend[1].size() + actors[0].table.size();
    assert(replay == 3 * (total - idle));
    // A restore that did not put a region back exactly makes it changed once.
    frame -= 1;
    restore(ring[(frame - 1) % 4].data(), false);
    before = SnapshotReuse_Stats().copiedBytes;
    tick(actors[0], 1); save();
    assert(SnapshotReuse_Stats().copiedBytes - before == total);
    // A region written again after the copy is caught by the next save's signals or not at all:
    // under Trust the restored version is believed.
}
''')

    def test_validate_reports_region_rewritten_later_in_restore(self):
        self.run_native("Validate", r'''
int main() {
    build();
    std::vector<uint8_t> a(total), b(total);
    SnapshotReuse_Save(a.data()); tick(actors[0], 1); SnapshotReuse_Save(b.data());
    size_t offset = 0;
    SnapshotReuse_BeginRestore();
    for (size_t i = 0; i < plan.size(); ++i) {
        std::memcpy(const_cast<uint8_t*>(plan[i].ptr), a.data() + offset, plan[i].size);
        SnapshotReuse_RegionRestored(int(i));
        offset += plan[i].size;
    }
    ++actors[1].joints[3];                                        // A late fixup.
    SnapshotReuse_EndRestore(a.data());
    assert(SnapshotReuse_Stats().mismatches == 1 && logs == 1);
    SnapshotReuse_Save(a.data());
    assert(matches(a.data()) && SnapshotReuse_Stats().mismatches == 1);
}
''')

    def test_weapon_pose_follows_its_character(self):
        self.run_native("Trust", r'''
int main() {
    // actors[1] is a weapon attached to actors[0].
    build();
    for (auto& region : plan)
        if (region.ptr == actors[1].body.data()) region.index = 1;
    SnapshotReuse_SetPlan(plan.data(), int(plan.size()));
    std::vector<uint8_t> buffer(total);
    SnapshotReuse_Save(buffer.data()); SnapshotReuse_Save(buffer.data());
    // The character moves; the weapon's joints follow with its own prefix untouched.
    ++actors[0].body[0x40]; ++actors[0].joints[0x20]; ++actors[1].joints[0xE0];
    SnapshotReuse_Save(buffer.data());
    assert(matches(buffer.data()));
    // The reverse does not hold: a weapon's prefix says nothing about the character.
    uint64_t before = SnapshotReuse_Stats().copiedBytes;
    ++actors[1].body[0x40];
    SnapshotReuse_Save(buffer.data());
    const uint64_t copied = SnapshotReuse_Stats().copiedBytes - before;
    assert(matches(buffer.data()) && copied == sizeof(misc) + 2 * BodySize + actors[1].joints.size());
}
''')

    def test_material_job_target_rebound_while_inactive_is_seen(self):
        # uMaterialControl::mpTarget (+0x40) changes with the job inactive and its
        # control block untouched: the signal itself must catch it, with no mismatch.
        self.run_native("Validate", r'''
int main() {
    build();
    std::vector<uint8_t> buffer(total);
    SnapshotReuse_Save(buffer.data()); SnapshotReuse_Save(buffer.data());
    ++actors[0].table[0xBB0 + 0x40];
    SnapshotReuse_Save(buffer.data());
    assert(matches(buffer.data()) && !SnapshotReuse_Stats().mismatches && !logs);
}
''')

    def test_material_table_skip_is_confirmed_in_every_mode(self):
        self.run_native("Trust", r'''
int main() {
    build();
    std::vector<uint8_t> buffer(total);
    SnapshotReuse_Save(buffer.data()); SnapshotReuse_Save(buffer.data());
    // Written with no job active, outside everything the signal watches.
    ++actors[0].table[0x300];
    SnapshotReuse_Save(buffer.data());
    assert(matches(buffer.data()) && !SnapshotReuse_Stats().mismatches && !logs);
}
''')

    def test_region_with_misplaced_owner_field_is_always_copied(self):
        self.run_native("Trust", r'''
int main() {
    Actor& a = actors[0];
    const uint64_t base = uint64_t(uintptr_t(a.body.data()));
    plan = {
        {a.body.data(), a.body.size(), 0, SnapshotReuseKind::ActorBody, 0},
        {a.blend[0].data(), a.blend[0].size(), base + 0x6C0 + 0x180, SnapshotReuseKind::MotionBlend, 0},
        {a.table.data(), a.table.size(), base + 0x1410, SnapshotReuseKind::MaterialTable, 0},
        {a.table.data(), 0xBB0 + 8, base + 0x1408, SnapshotReuseKind::MaterialTable, 0},
        {a.joints.data(), a.joints.size(), uint64_t(uintptr_t(misc)), SnapshotReuseKind::Prefix, 0},
    };
    total = 0;
    for (const auto& region : plan) total += region.size;
    SnapshotReuse_SetPlan(plan.data(), int(plan.size()));
    std::vector<uint8_t> buffer(total);
    for (int i = 0; i < 3; ++i) SnapshotReuse_Save(buffer.data());
    assert(!SnapshotReuse_Stats().reusedBytes);
    ++a.blend[0][3]; ++a.table[5]; ++a.joints[1];
    SnapshotReuse_Save(buffer.data());
    assert(matches(buffer.data()));
}
''')

    def test_integration(self):
        snapshot = (CODE / "rollback_snapshot.cpp").read_text()
        restore = (CODE / "rollback_restore.cpp").read_text()
        snapdump = (CODE / "rollback_snapdump.cpp").read_text()
        self.assertIn("SnapshotReuse_Save(slot.data)", snapshot)
        # Every release or foreign write of snapshot storage drops its tags first.
        for source in (snapshot, snapdump):
            for at in range(len(source)):
                if source.startswith("delete[] ", at) and "data" in source[at:at + 40] + "storage":
                    line = source[at:source.index("\n", at)]
                    if "slot.data" in line or "g_slots[i].data" in line or ".storage" in line:
                        self.assertIn("SnapshotReuse_ForgetBuffer(", source[max(0, at - 120):at], line)
        self.assertIn("SnapshotReuse_ForgetBuffer(destination.storage); // Not written by the save.", snapdump)
        body = restore[restore.index("void Rollback_RestoreSnapshot("):]
        self.assertLess(body.index("SnapshotReuse_BeginRestore()"), body.index("std::memcpy("))
        self.assertIn("if (copySize == slot.regions[i].size) SnapshotReuse_RegionRestored(i);", body)
        self.assertGreater(body.index("SnapshotReuse_EndRestore(slot.data)"),
                           body.index("LifetimeTrace_RestoreShots(frame)"))
        build = snapshot[snapshot.index("int Rollback_BuildRegions("):snapshot.index("void Rollback_SaveSnapshot(")]
        self.assertEqual(build.count("PublishReusePlan();"), 1)
        self.assertIn("$(SRCDIR)/code/snapshot_reuse.cpp", (ROOT / "Makefile").read_text())


if __name__ == "__main__":
    unittest.main()
