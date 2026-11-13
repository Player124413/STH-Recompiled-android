#pragma once
// SSE2 by hand on a host that has none. sdk/runtime/gpu/render_thread.cpp compares the recorded draw state 16 bytes at
// a time with _mm_* intrinsics; on ARM (Android, Apple silicon) the compiler has none, so the few intrinsics that file
// uses are done over a plain 16-byte struct instead. The state is a couple of hundred bytes, so the scalar loop costs
// nothing next to the draw it guards.
//
// On x86 this is the real header, unchanged.
#if defined(__x86_64__) || defined(__i386__) || defined(_M_X64) || defined(_M_IX86)
#include_next <smmintrin.h>
#else

#include <cstdint>
#include <cstring>

// the compiler's own __m128i only exists where the intrinsics do, which is exactly the case handled above
typedef struct {
    std::uint8_t v[16];
} __m128i;

inline __m128i _mm_setzero_si128() {
    __m128i r;
    std::memset(&r, 0, sizeof r);
    return r;
}

inline __m128i _mm_loadu_si128(const __m128i* p) {
    __m128i r;
    std::memcpy(&r, p, sizeof r);
    return r;
}

inline __m128i _mm_or_si128(__m128i a, __m128i b) {
    for (int i = 0; i < 16; i++) a.v[i] = std::uint8_t(a.v[i] | b.v[i]);
    return a;
}

inline __m128i _mm_xor_si128(__m128i a, __m128i b) {
    for (int i = 0; i < 16; i++) a.v[i] = std::uint8_t(a.v[i] ^ b.v[i]);
    return a;
}

// nonzero when every bit of a & b is zero, as the real one does
inline int _mm_testz_si128(__m128i a, __m128i b) {
    std::uint8_t acc = 0;
    for (int i = 0; i < 16; i++) acc = std::uint8_t(acc | (a.v[i] & b.v[i]));
    return acc == 0;
}

#endif
