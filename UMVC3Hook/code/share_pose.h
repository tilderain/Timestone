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
// SSE is used only where each lane performs exactly one of those scalar
// operations (packed sqrt, divide, multiply, add). tools/test_share_pose.cpp
// compares this file against a plain scalar transcription of the native code.
//
// Measured 2026-11-13: an active character runs about 126 of these per frame.
// Verified bit-identical against native over 32 million calls (12 characters).
// About 2.2x faster per helper; resim_tick 0.62-0.72 ms -> 0.55-0.64 ms, which
// is 0.6-0.8 ms off a seven-frame rollback. Measuring each row once and packing
// the square roots and divides then took the world matrix from about 380 to
// 220 ticks per call and the math of both helpers from 446 to 314.
namespace share_pose {

struct Matrix { float m[4][4]; };

inline float FromBits(uint32_t bits) { float value; std::memcpy(&value, &bits, sizeof(value)); return value; }

// Lengths of rows 0..2, each sqrt(((x*x + y*y) + z*z) + w*w), from one packed
// square root: per lane it is the same IEEE operation as the scalar one.
inline __m128 RowLengths(const Matrix& m)
{
    const auto squared = [](const float* v) { return ((v[0] * v[0] + v[1] * v[1]) + v[2] * v[2]) + v[3] * v[3]; };
    return _mm_sqrt_ps(_mm_setr_ps(squared(m.m[0]), squared(m.m[1]), squared(m.m[2]), 1.0f));
}

// Rows 0..2 times 1 / length, for the rows whose length is at least 2^-23; a
// shorter (or NaN) row stays as it is. Native measures each row twice, once for
// the length and once inside its normalize; the value is the same, so the
// caller's lengths are reused here.
inline void NormalizeRows(Matrix& m, __m128 lengths)
{
    const __m128 one = _mm_set1_ps(1.0f);
    const __m128 valid = _mm_cmple_ps(_mm_set1_ps(FromBits(0x34000000)), lengths);
    // Divide by 1 in the lanes that are not used, so nothing divides by zero.
    const __m128 scale = _mm_div_ps(one, _mm_or_ps(_mm_and_ps(valid, lengths), _mm_andnot_ps(valid, one)));
    alignas(16) float scales[4];
    _mm_store_ps(scales, scale);
    const int mask = _mm_movemask_ps(valid);
    for (int row = 0; row < 3; ++row)
        if (mask & (1 << row))
            _mm_storeu_ps(m.m[row], _mm_mul_ps(_mm_set1_ps(scales[row]), _mm_loadu_ps(m.m[row])));
}

// One row of a 4x4 product: ((a0*b[0] + a1*b[1]) + a2*b[2]) + a3*b[3].
inline __m128 MultiplyRow(const float* a, const Matrix& b)
{
    __m128 row = _mm_add_ps(_mm_mul_ps(_mm_set1_ps(a[0]), _mm_loadu_ps(b.m[0])),
                            _mm_mul_ps(_mm_set1_ps(a[1]), _mm_loadu_ps(b.m[1])));
    row = _mm_add_ps(row, _mm_mul_ps(_mm_set1_ps(a[2]), _mm_loadu_ps(b.m[2])));
    return _mm_add_ps(row, _mm_mul_ps(_mm_set1_ps(a[3]), _mm_loadu_ps(b.m[3])));
}

// 0x14010D370 after its lookups: `resource` is the joint's resource matrix and
// `parent` the parent joint's (or the model's) world matrix. The resource
// translation takes the parent's axis lengths; the parent axes are normalized.
// `out` may be either input.
inline void ParentFrame(const Matrix& resource, const Matrix& parentIn, Matrix& out)
{
    Matrix parent = parentIn;
    const __m128 lengths = RowLengths(parent);
    alignas(16) float length[4];
    _mm_store_ps(length, lengths);
    NormalizeRows(parent, lengths);
    const float translation[4] = {length[0] * resource.m[3][0], length[1] * resource.m[3][1],
                                  length[2] * resource.m[3][2], 1.0f * resource.m[3][3]};
    const __m128 r0 = MultiplyRow(resource.m[0], parent), r1 = MultiplyRow(resource.m[1], parent),
                 r2 = MultiplyRow(resource.m[2], parent), r3 = MultiplyRow(translation, parent);
    _mm_storeu_ps(out.m[0], r0); _mm_storeu_ps(out.m[1], r1); _mm_storeu_ps(out.m[2], r2); _mm_storeu_ps(out.m[3], r3);
}

// 0x14010D020. `frame` is ParentFrame's result and is normalized in place, as
// native does. `collapsed` is the vector at 0x140D462B0 (set at startup) used
// when every scale component is at most 0.0001. `world` must not be `frame`.
inline void WorldMatrix(const float* quaternion, const float* translation, const float* scale,
                        Matrix& frame, uint32_t motionFlags, const float* collapsed, Matrix& world)
{
    // The frame's row lengths are unchanged until NormalizeRows, so one
    // measurement serves both the scale and the normalization.
    const __m128 lengths = RowLengths(frame);
    float sx = scale[0], sy = scale[1], sz = scale[2];
    if (!(motionFlags & 0x4000)) {
        alignas(16) float length[4];
        _mm_store_ps(length, lengths);
        sx = length[0] * sx; sy = length[1] * sy; sz = length[2] * sz;
        const float limit = FromBits(0x38D1B717);
        if (sx <= limit && sy <= limit && sz <= limit) { sx = collapsed[0]; sy = collapsed[1]; sz = collapsed[2]; }
    }
    // 0x14010D880: rotation rows from the quaternion, translation in row 3.
    const float q0 = quaternion[0], q1 = quaternion[1], q2 = quaternion[2], q3 = quaternion[3];
    const float a = q2 * 2.0f, zz = q2 * a, b = q1 * 2.0f, wx = q3 * q0 * 2.0f, xx = q0 * q0 * 2.0f;
    const float local[4][4] = {
        {sx * (1.0f - (zz + q1 * b)), sx * (q0 * b + q3 * a), sx * (q0 * a - q3 * b), sx * 0.0f},
        {sy * (q0 * b - q3 * a), sy * (1.0f - (zz + xx)), sy * (q1 * a + wx), sy * 0.0f},
        {sz * (q0 * a + q3 * b), sz * (q1 * a - wx), sz * (1.0f - (q1 * b + xx)), sz * 0.0f},
        {translation[0], translation[1], translation[2], 1.0f},
    };
    NormalizeRows(frame, lengths);
    const __m128 r0 = MultiplyRow(local[0], frame), r1 = MultiplyRow(local[1], frame),
                 r2 = MultiplyRow(local[2], frame), r3 = MultiplyRow(local[3], frame);
    _mm_storeu_ps(world.m[0], r0); _mm_storeu_ps(world.m[1], r1); _mm_storeu_ps(world.m[2], r2); _mm_storeu_ps(world.m[3], r3);
}

} // namespace share_pose
