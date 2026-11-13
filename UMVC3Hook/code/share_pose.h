#pragma once

#include <cstdint>
#include <cstring>
#include <xmmintrin.h>

// The matrix math of uCnsShare::applyConstraint (0x14010CE10), the per-joint
// world-matrix writer of every shared-skeleton character. The native helpers
// 0x14010D370 (parent frame) and 0x14010D020 (world matrix) spend most of their
// time in calls to one-line vector helpers; these do the same operations in the
// same order, so the results are bit-identical. That order is the contract:
// do not reassociate, vectorize by hand, or build this file with fast-math.
//
//   length    sqrt(((x*x + y*y) + z*z) + w*w)
//   normalize v * (1 / length) when length >= 2^-23
//   multiply  r[i][j] = ((a[i][0]*b[0][j] + a[i][1]*b[1][j]) + a[i][2]*b[2][j]) + a[i][3]*b[3][j]
//
// Measured 2026-11-13: an active character runs about 126 of these per frame.
// Verified bit-identical against native over 32 million calls (12 characters).
// About 2.2x faster per helper; resim_tick 0.62-0.72 ms -> 0.55-0.64 ms, which
// is 0.6-0.8 ms off a seven-frame rollback.
namespace share_pose {

struct Matrix { float m[4][4]; };

inline float FromBits(uint32_t bits) { float value; std::memcpy(&value, &bits, sizeof(value)); return value; }

inline float Length(const float* v)
{
    return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]) + v[3] * v[3])));
}

inline void Normalize(float* v)
{
    const float length = Length(v);
    if (!(FromBits(0x34000000) <= length)) return; // Also leaves a NaN row as it is.
    const float scale = 1.0f / length;
    v[0] = scale * v[0]; v[1] = scale * v[1]; v[2] = scale * v[2]; v[3] = scale * v[3];
}

inline void Multiply(const Matrix& a, const Matrix& b, Matrix& out)
{
    Matrix r;
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            r.m[i][j] = ((a.m[i][0] * b.m[0][j] + a.m[i][1] * b.m[1][j]) + a.m[i][2] * b.m[2][j]) + a.m[i][3] * b.m[3][j];
    out = r;
}

// 0x14010D370 after its lookups: `resource` is the joint's resource matrix and
// `parent` the parent joint's (or the model's) world matrix. The resource
// translation takes the parent's axis lengths; the parent axes are normalized.
inline void ParentFrame(const Matrix& resourceIn, const Matrix& parentIn, Matrix& out)
{
    Matrix resource = resourceIn, parent = parentIn;
    const float z = Length(parent.m[2]), y = Length(parent.m[1]), x = Length(parent.m[0]);
    Normalize(parent.m[0]); Normalize(parent.m[1]); Normalize(parent.m[2]);
    resource.m[3][0] = x * resource.m[3][0];
    resource.m[3][1] = y * resource.m[3][1];
    resource.m[3][2] = z * resource.m[3][2];
    resource.m[3][3] = 1.0f * resource.m[3][3];
    Multiply(resource, parent, out);
}

// 0x14010D020. `frame` is ParentFrame's result and is normalized in place, as
// native does. `collapsed` is the vector at 0x140D462B0 (set at startup) used
// when every scale component is at most 0.0001.
inline void WorldMatrix(const float* quaternion, const float* translation, const float* scale,
                        Matrix& frame, uint32_t motionFlags, const float* collapsed, Matrix& world)
{
    float sx = scale[0], sy = scale[1], sz = scale[2];
    if (!(motionFlags & 0x4000)) {
        const float z = Length(frame.m[2]), y = Length(frame.m[1]), x = Length(frame.m[0]);
        sx = x * sx; sy = y * sy; sz = z * sz;
        const float limit = FromBits(0x38D1B717);
        if (sx <= limit && sy <= limit && sz <= limit) { sx = collapsed[0]; sy = collapsed[1]; sz = collapsed[2]; }
    }
    // 0x14010D880: rotation rows from the quaternion, translation in row 3.
    const float q0 = quaternion[0], q1 = quaternion[1], q2 = quaternion[2], q3 = quaternion[3];
    const float a = q2 * 2.0f, zz = q2 * a, b = q1 * 2.0f, wx = q3 * q0 * 2.0f, xx = q0 * q0 * 2.0f;
    Matrix local;
    local.m[0][0] = 1.0f - (zz + q1 * b); local.m[0][1] = q0 * b + q3 * a; local.m[0][2] = q0 * a - q3 * b; local.m[0][3] = 0.0f;
    local.m[1][0] = q0 * b - q3 * a; local.m[1][1] = 1.0f - (zz + xx); local.m[1][2] = q1 * a + wx; local.m[1][3] = 0.0f;
    local.m[2][0] = q0 * a + q3 * b; local.m[2][1] = q1 * a - wx; local.m[2][2] = 1.0f - (q1 * b + xx); local.m[2][3] = 0.0f;
    local.m[3][0] = translation[0]; local.m[3][1] = translation[1]; local.m[3][2] = translation[2]; local.m[3][3] = 1.0f;
    for (int j = 0; j < 4; ++j) {
        local.m[0][j] = sx * local.m[0][j];
        local.m[1][j] = sy * local.m[1][j];
        local.m[2][j] = sz * local.m[2][j];
    }
    Normalize(frame.m[0]); Normalize(frame.m[1]); Normalize(frame.m[2]);
    Multiply(local, frame, world);
}

} // namespace share_pose
