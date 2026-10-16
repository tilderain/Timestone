import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


def block(source, marker):
    start = source.index(marker)
    opening = source.index("{", start)
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


class EffectAttachmentSkipTest(unittest.TestCase):
    @unittest.skipUnless(shutil.which("g++"), "native g++ required")
    def test_attached_trails_update_during_resim(self):
        root = Path(__file__).resolve().parents[1]
        source = (root / "UMVC3Hook/code/rollback_effect.cpp").read_text()
        harness = r'''
#include <array>
#include <cassert>
#include <cstdint>
#define __fastcall
enum class RollbackHookProfileId { GameEffectAttachmentUpdate };
struct RollbackHookProfileScope {
    explicit RollbackHookProfileScope(RollbackHookProfileId) {}
};
bool g_isResimulating = false;
unsigned nativeCalls = 0, validations = 0;
alignas(16) std::array<uint8_t, 0x50> effect{};
std::array<float, 3> parentPosition{125.0f, 50.0f, -20.0f};
std::array<float, 3> effectPosition{};
const uint64_t validParent = (uint64_t)parentPosition.data();
bool HasCallableDtiVirtual(void* object) {
    ++validations;
    return (uint64_t)object == validParent;
}
void Native(void* object) {
    assert(object == effect.data());
    ++nativeCalls;
    uint64_t parent = *(uint64_t*)(effect.data() + 0x40);
    assert(parent == 0 || parent == validParent);
    if (parent) effectPosition = parentPosition;
}
void (*oGameEffectAttachmentUpdate)(void*) = Native;
''' + block(source, "static void __fastcall hkGameEffectAttachmentUpdate(") + r'''
int main() {
    uint64_t& attachment = *(uint64_t*)(effect.data() + 0x40);
    attachment = validParent;
    g_isResimulating = true;
    for (int frame = 0; frame < 8; ++frame) {
        parentPosition[0] += 10.0f;
        hkGameEffectAttachmentUpdate(effect.data());
        // A generator sampling after this hook sees the current parent position,
        // including the first update of a freshly reconstructed effect.
        assert(effectPosition == parentPosition);
        assert(attachment == validParent);
    }
    assert(nativeCalls == 8 && validations == 8);

    attachment = 0;
    const auto savedEffect = effect;
    const auto savedPosition = effectPosition;
    hkGameEffectAttachmentUpdate(effect.data());
    assert(nativeCalls == 8 && validations == 8);
    assert(effect == savedEffect && effectPosition == savedPosition);

    // Invalid attachments are cleared before native code sees them, but still
    // receive the native update rather than taking the unattached fast path.
    attachment = 0x1234;
    hkGameEffectAttachmentUpdate(effect.data());
    assert(attachment == 0 && nativeCalls == 9 && validations == 9);

    g_isResimulating = false;
    hkGameEffectAttachmentUpdate(effect.data());
    assert(nativeCalls == 10 && validations == 9);
    attachment = validParent;
    parentPosition[1] += 30.0f;
    hkGameEffectAttachmentUpdate(effect.data());
    assert(nativeCalls == 11 && validations == 10);
    assert(effectPosition == parentPosition);
    attachment = 0x1234;
    hkGameEffectAttachmentUpdate(effect.data());
    assert(attachment == 0 && nativeCalls == 12 && validations == 11);
}
'''
        with tempfile.TemporaryDirectory(prefix="effect-attachment-test-") as temporary:
            cpp = Path(temporary) / "test.cpp"
            executable = Path(temporary) / "test"
            cpp.write_text(harness)
            subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
                            str(cpp), "-o", str(executable)], check=True)
            subprocess.run([str(executable)], check=True)


if __name__ == "__main__":
    unittest.main()
