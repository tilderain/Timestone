// Offline scalar model of follow-mode 0x140839200 / 0x14081B420.
// Not game emulation: attachment poses are supplied, and lifecycle is excluded.
#include <algorithm>
#include <array>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

struct Vec3 {
    float x, y, z;
    Vec3 operator+(Vec3 b) const { return {x + b.x, y + b.y, z + b.z}; }
    Vec3 operator-(Vec3 b) const { return {x - b.x, y - b.y, z - b.z}; }
    Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

static float Distance(Vec3 a, Vec3 b)
{
    Vec3 d = a - b;
    return std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
}

struct Trail {
    // Native follow mode uses N entries per block: bank 0, bank 1, raw history.
    std::vector<Vec3> storage;
    std::array<Vec3, 2> position{};
    unsigned current = 0, previous = 1;
    unsigned particleSteps = 0, attachmentUpdates = 0;

    explicit Trail(unsigned count) : storage(count * 3) { assert(count > 0); }
    unsigned Count() const { return static_cast<unsigned>(storage.size() / 3); }
    Vec3& Bank(unsigned bank, unsigned i) { return storage[bank * Count() + i]; }
    const Vec3& Bank(unsigned bank, unsigned i) const { return storage[bank * Count() + i]; }

    void Derive(bool updateOld)
    {
        for (unsigned i = 0; i < Count(); ++i) {
            Bank(current, i) = Bank(2, i) - position[current];
            if (updateOld)
                Bank(current ^ 1, i) = Bank(2, i) - position[previous];
        }
    }

    void Step(Vec3 emitter, unsigned loopCounter, bool updateOld)
    {
        previous = current;
        current ^= 1;
        position[current] = emitter;
        // Native history shifts only on the owner's first substep.
        if (loopCounter == 0) {
            for (unsigned i = Count() - 1; i > 0; --i)
                Bank(2, i) = Bank(2, i - 1);
            Bank(2, 0) = position[current];
        }
        Derive(updateOld);
        ++particleSteps;
    }

    void Tick(Vec3 emitter, unsigned substeps, bool updateOld, bool updateAttachment)
    {
        if (updateAttachment)
            ++attachmentUpdates;
        for (unsigned i = 0; i < substeps; ++i)
            Step(emitter, i, updateOld);
        if (substeps == 0)
            Derive(updateOld); // Explicit test of recalculation without new history.
    }

    void Repair(Vec3 corrected, bool bothBanks = true)
    {
        ++attachmentUpdates; // One supplied corrected attachment pose.
        position = {corrected, corrected};
        for (unsigned i = 0; i < Count(); ++i)
            Bank(2, i) = corrected;
        Derive(bothBanks);
    }

    Vec3 Visible(unsigned i, float alpha) const
    {
        // Simplified reconstruction of two relative endpoints with their anchors.
        Vec3 now = position[current] + Bank(current, i);
        Vec3 old = position[previous] + Bank(current ^ 1, i);
        return old * (1.0f - alpha) + now * alpha;
    }

    float MaxSegment() const
    {
        float result = 0;
        for (float alpha : {0.0f, 0.5f, 1.0f}) {
            result = std::max(result, Distance(Visible(0, alpha), position[current]));
            for (unsigned i = 1; i < Count(); ++i)
                result = std::max(result, Distance(Visible(i - 1, alpha), Visible(i, alpha)));
        }
        return result;
    }
};

static Vec3 Pose(unsigned frame, unsigned path)
{
    float t = static_cast<float>(frame);
    switch (path) {
    case 0: return {1000 + 12 * t, 150 + 2 * t, -80};
    case 1: return {1000 + 100 * std::cos(t * 0.2f),
                   150 + 60 * std::sin(t * 0.2f), -80 + 5 * t};
    default: return {1000 + (frame >= 4 ? 500.0f : 0.0f), 150, -80};
    }
}

static void Seed(Trail& trail, Vec3 initial)
{
    trail.position = {initial, initial};
    for (unsigned i = 0; i < trail.Count(); ++i)
        trail.Bank(2, i) = initial;
    trail.Derive(true);
}

int main()
{
    unsigned cases = 0;
    for (unsigned depth : {1u, 4u, 8u, 12u})
    for (unsigned count : {1u, 8u, 32u})
    for (unsigned substeps : {0u, 1u, 3u})
    for (unsigned path : {0u, 1u, 2u})
    for (bool updateOld : {false, true})
    for (unsigned initialBank : {0u, 1u}) {
        Trail native(count), repaired(count);
        native.current = repaired.current = initialBank;
        native.previous = repaired.previous = initialBank ^ 1;
        Seed(native, Pose(0, path));
        // Reconstructed effect starts at zero when attachment refresh is skipped.
        Seed(repaired, {});
        for (unsigned frame = 1; frame <= depth; ++frame) {
            native.Tick(Pose(frame, path), substeps, updateOld, true);
            repaired.Tick({}, substeps, updateOld, false);
        }
        repaired.Repair(Pose(depth, path));
        assert(native.particleSteps == repaired.particleSteps);
        assert(native.attachmentUpdates == depth && repaired.attachmentUpdates == 1);
        assert(repaired.MaxSegment() < 0.001f);
        for (unsigned i = 0; i < count; ++i)
            for (float alpha : {0.0f, 0.5f, 1.0f})
                assert(Distance(repaired.Visible(i, alpha), Pose(depth, path)) < 0.001f);

        // Ordinary continuation must not reconnect the repaired trail to origin.
        for (unsigned frame = depth + 1; frame <= depth + count + 2; ++frame) {
            native.Tick(Pose(frame, path), 1, updateOld, true);
            repaired.Tick(Pose(frame, path), 1, updateOld, true);
            for (unsigned i = 0; i < count; ++i)
                for (float alpha : {0.0f, 0.5f, 1.0f})
                    assert(repaired.Visible(i, alpha).x > 899.0f);
        }
        for (unsigned i = 0; i < count; ++i)
            for (float alpha : {0.0f, 0.5f, 1.0f})
                assert(Distance(repaired.Visible(i, alpha), native.Visible(i, alpha)) < 0.001f);
        ++cases;
    }
    std::printf("PASS: %u modeled cases (depth, history size, substeps, motion, flags)\n", cases);

    Trail native(8), positionOnly(8), oneBank(8), repaired(8);
    Seed(native, Pose(0, 0));
    for (unsigned frame = 1; frame <= 8; ++frame) {
        native.Tick(Pose(frame, 0), 1, false, true);
        positionOnly.Tick({}, 1, false, false);
    }
    oneBank = native; // Existing history can leave nonzero previous-bank offsets.
    repaired = positionOnly;
    positionOnly.Tick(Pose(9, 0), 1, false, true);
    oneBank.Repair(Pose(8, 0), false);
    repaired.Repair(Pose(8, 0));
    assert(positionOnly.MaxSegment() > 1000);
    assert(oneBank.MaxSegment() > 1);
    assert(repaired.MaxSegment() == 0);
    std::printf("8-frame linear example, 8 history samples (arbitrary world units):\n");
    std::printf("  Normal attachment resumes only: max segment %.2f\n", positionOnly.MaxSegment());
    std::printf("  Reset raw history + one bank:   max segment %.2f\n", oneBank.MaxSegment());
    std::printf("  Reset raw history + both banks: max segment %.2f\n", repaired.MaxSegment());
    std::printf("  Correct native trail length lost by reset: %.2f\n",
                Distance(native.Bank(2, 0), native.Bank(2, native.Count() - 1)));
    std::printf("Work model only, NOT native timing:\n");
    for (unsigned depth : {1u, 4u, 8u, 12u}) {
        std::printf("  depth %2u: %2u attachment calls -> 1; reset per particle = 3*N+2 Vec3 writes\n",
                    depth, depth);
    }
    std::printf("Not modeled: spawn/death, cloth/other modes, camera/scale, owner callbacks, GPU draw.\n");
}
