// floatcmp.h - order comparisons for float values that never rely on the FP
// compare instructions.
//
// Reason: Apple clang 16.0.0 (clang-1600.0.26.6) on macOS 27 miscompiles float
// order comparisons. Measured on that host, at every optimization level:
//
//     float a = -44.77f, b = -100.0f;
//     a < b   ->  0   (wrong: -44.77 is greater than -100)
//     a > b   ->  1
//
// The same values compare correctly as int. A detection path that compares dB
// levels is therefore not trustworthy on that toolchain.
//
// These helpers map a float to a monotonic int and compare the ints, so the
// result is correct on every toolchain. IEEE-754 single precision has this
// property: for non-NaN values, flipping the sign bit of every negative value
// makes the signed integer order equal to the float order.
#pragma once

#include <cstdint>
#include <cstring>

namespace sr {

// Strict total order key for a float. NaN maps to the high end.
inline int32_t FloatOrderKey(float v) {
    uint32_t u = 0;
    std::memcpy(&u, &v, sizeof(u));
    // Negative values: flip every bit. Positive values: flip only the sign bit.
    uint32_t key = (u & 0x80000000u) ? ~u : (u | 0x80000000u);
    return static_cast<int32_t>(key);
}

inline bool FCmpLt(float a, float b) { return FloatOrderKey(a) < FloatOrderKey(b); }
inline bool FCmpLe(float a, float b) { return FloatOrderKey(a) <= FloatOrderKey(b); }
inline bool FCmpGt(float a, float b) { return FloatOrderKey(a) > FloatOrderKey(b); }
inline bool FCmpGe(float a, float b) { return FloatOrderKey(a) >= FloatOrderKey(b); }
inline bool FCmpEq(float a, float b) { return FloatOrderKey(a) == FloatOrderKey(b); }
inline float FMin(float a, float b) { return FCmpLt(a, b) ? a : b; }
inline float FMax(float a, float b) { return FCmpGt(a, b) ? a : b; }

} // namespace sr
