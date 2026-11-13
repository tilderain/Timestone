// Host test: share_pose.h against a plain scalar transcription of the native
// helpers (0x14010D370 after its lookups, 0x14010D020), bit for bit.
//   g++ -std=c++20 -O3 -I UMVC3Hook/code tools/test_share_pose.cpp -o /tmp/test_share_pose && /tmp/test_share_pose
#include "share_pose.h"

#include <cmath>
#include <cstdio>
#include <limits>
#include <random>

namespace reference {
using share_pose::Matrix;
using share_pose::FromBits;
float Length(const float* v) { return std::sqrt(((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]) + v[3] * v[3]); }
void Normalize(float* v)
{
    const float length = Length(v);
    if (!(FromBits(0x34000000) <= length)) return;
    const float scale = 1.0f / length;
    for (int i = 0; i < 4; ++i) v[i] = scale * v[i];
}
void Multiply(const Matrix& a, const Matrix& b, Matrix& out)
{
    Matrix r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = ((a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j]) + a.m[i][2] * b.m[2][j]) + a.m[i][3] * b.m[3][j];
    out = r;
}
void ParentFrame(const Matrix& resourceIn, const Matrix& parentIn, Matrix& out)
{
    Matrix resource = resourceIn, parent = parentIn;
    const float z = Length(parent.m[2]), y = Length(parent.m[1]), x = Length(parent.m[0]);
    Normalize(parent.m[0]); Normalize(parent.m[1]); Normalize(parent.m[2]);
    resource.m[3][0] = x * resource.m[3][0]; resource.m[3][1] = y * resource.m[3][1];
    resource.m[3][2] = z * resource.m[3][2]; resource.m[3][3] = 1.0f * resource.m[3][3];
    Multiply(resource, parent, out);
}
void WorldMatrix(const float* q, const float* t, const float* scale, Matrix& frame, uint32_t flags,
                 const float* collapsed, Matrix& world)
{
    float sx = scale[0], sy = scale[1], sz = scale[2];
    if (!(flags & 0x4000)) {
        const float z = Length(frame.m[2]), y = Length(frame.m[1]), x = Length(frame.m[0]);
        sx = x * sx; sy = y * sy; sz = z * sz;
        const float limit = FromBits(0x38D1B717);
        if (sx <= limit && sy <= limit && sz <= limit) { sx = collapsed[0]; sy = collapsed[1]; sz = collapsed[2]; }
    }
    const float q0 = q[0], q1 = q[1], q2 = q[2], q3 = q[3];
    const float a = q2 * 2.0f, zz = q2 * a, b = q1 * 2.0f, wx = q3 * q0 * 2.0f, xx = q0 * q0 * 2.0f;
    Matrix local;
    local.m[0][0] = 1.0f - (zz + q1 * b); local.m[0][1] = q0 * b + q3 * a; local.m[0][2] = q0 * a - q3 * b; local.m[0][3] = 0.0f;
    local.m[1][0] = q0 * b - q3 * a; local.m[1][1] = 1.0f - (zz + xx); local.m[1][2] = q1 * a + wx; local.m[1][3] = 0.0f;
    local.m[2][0] = q0 * a + q3 * b; local.m[2][1] = q1 * a - wx; local.m[2][2] = 1.0f - (q1 * b + xx); local.m[2][3] = 0.0f;
    local.m[3][0] = t[0]; local.m[3][1] = t[1]; local.m[3][2] = t[2]; local.m[3][3] = 1.0f;
    for (int j = 0; j < 4; ++j) { local.m[0][j] = sx * local.m[0][j]; local.m[1][j] = sy * local.m[1][j]; local.m[2][j] = sz * local.m[2][j]; }
    Normalize(frame.m[0]); Normalize(frame.m[1]); Normalize(frame.m[2]);
    Multiply(local, frame, world);
}
}

int main()
{
    std::mt19937_64 rng(20261113);
    std::uniform_real_distribution<float> unit(-1.0f, 1.0f), wide(-1000.0f, 1000.0f);
    const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, 1e-8f, -1e-8f, 1e-4f, 9.9e-5f, 1.2e-7f, 1.1e-7f, 1e20f, -1e20f,
        std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::denorm_min(), std::numeric_limits<float>::max()};
    const auto value = [&](int round, int mode) -> float {
        const uint64_t pick = rng();
        if (mode == 2 || (mode == 1 && pick % 6 == 0)) return specials[(pick >> 8) % std::size(specials)];
        return (round & 1) ? wide(rng) : unit(rng);
    };
    const float collapsed[4] = {1.0f, 1.0f, 1.0f, 0.0f};
    uint64_t rounds = 0, failures = 0, nanOnly = 0;
    for (int round = 0; round < 3000000; ++round) {
        const int mode = round % 20 == 0 ? 2 : round % 4 == 0 ? 1 : 0;
        share_pose::Matrix resource, parent;
        float q[4], t[4], scale[4];
        for (auto& row : resource.m) for (float& v : row) v = value(round, mode);
        for (auto& row : parent.m) for (float& v : row) v = value(round, mode);
        for (float& v : q) v = value(round, mode);
        for (float& v : t) v = value(round, mode);
        for (float& v : scale) v = value(round, mode);
        if (round % 7 == 0) { for (float& v : parent.m[round % 3]) v *= 1e-9f; scale[0] = scale[1] = scale[2] = 1e-6f; }
        const uint32_t flags = (round % 3 == 0) ? 0x4000u : 0u;
        share_pose::Matrix frameA, frameB, worldA, worldB;
        reference::ParentFrame(resource, parent, frameA);
        share_pose::ParentFrame(resource, parent, frameB);
        bool same = !std::memcmp(&frameA, &frameB, sizeof(frameA));
        reference::WorldMatrix(q, t, scale, frameA, flags, collapsed, worldA);
        share_pose::WorldMatrix(q, t, scale, frameB, flags, collapsed, worldB);
        same = same && !std::memcmp(&frameA, &frameB, sizeof(frameA)) && !std::memcmp(&worldA, &worldB, sizeof(worldA));
        ++rounds;
        if (same) continue;
        // A NaN's sign and payload follow operand order, which no two builds of
        // this math share. Anything else that differs is a real failure.
        const auto equivalent = [](const share_pose::Matrix& a, const share_pose::Matrix& b) {
            for (int i = 0; i < 4; ++i)
                for (int j = 0; j < 4; ++j)
                    if (std::memcmp(&a.m[i][j], &b.m[i][j], 4) && !(std::isnan(a.m[i][j]) && std::isnan(b.m[i][j]))) return false;
            return true;
        };
        if (equivalent(frameA, frameB) && equivalent(worldA, worldB)) { ++nanOnly; continue; }
        if (++failures <= 5) std::printf("mismatch in round %d (mode %d, flags %#x)\n", round, mode, flags);
    }
    std::printf("%llu rounds, %llu mismatches, %llu differing only in NaN sign or payload\n",
                (unsigned long long)rounds, (unsigned long long)failures, (unsigned long long)nanOnly);
    return failures != 0;
}
