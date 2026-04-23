#pragma once

#include "common.h"
#include <math.h>
#include <stdlib.h>

#ifdef USE_FLOAT_REALS

typedef float GMLReal;

// ===[ PSP VFPU-accelerated math — ~1 cycle vs ~50-100 for libm ]===
#ifdef PSP

static inline float vfpu_sinf(float x) {
    float s;
    __asm__ volatile (
        "mtv      %1, s000\n"
        "vcst.s   s001, VFPU_2_PI\n"
        "vmul.s   s000, s000, s001\n"
        "vsin.s   s000, s000\n"
        "mfv      %0, s000\n"
        : "=r"(s) : "r"(x)
    );
    return s;
}

static inline float vfpu_cosf(float x) {
    float c;
    __asm__ volatile (
        "mtv      %1, s000\n"
        "vcst.s   s001, VFPU_2_PI\n"
        "vmul.s   s000, s000, s001\n"
        "vcos.s   s000, s000\n"
        "mfv      %0, s000\n"
        : "=r"(c) : "r"(x)
    );
    return c;
}

static inline float vfpu_atan2f(float y, float x) {
    // VFPU doesn't have atan2, fall back to libm
    return atan2f(y, x);
}

static inline float vfpu_sqrtf(float x) {
    float r;
    __asm__ volatile (
        "mtv      %1, s000\n"
        "vsqrt.s  s000, s000\n"
        "mfv      %0, s000\n"
        : "=r"(r) : "r"(x)
    );
    return r;
}

#define GMLReal_sin vfpu_sinf
#define GMLReal_cos vfpu_cosf
#define GMLReal_atan2 atan2f
#define GMLReal_sqrt vfpu_sqrtf

#else // non-PSP float

#define GMLReal_sin sinf
#define GMLReal_cos cosf
#define GMLReal_atan2 atan2f
#define GMLReal_sqrt sqrtf

#endif // PSP

#define GMLReal_fabs fabsf
#define GMLReal_fmod fmodf
#define GMLReal_floor floorf
#define GMLReal_ceil ceilf
#define GMLReal_round roundf
#define GMLReal_pow powf
#define GMLReal_fmax fmaxf
#define GMLReal_fmin fminf
#define GMLReal_strtod(str, endptr) strtof(str, endptr)

#else // !USE_FLOAT_REALS — double-precision GMLReal (default for PC, PSP)

typedef double GMLReal;

#ifdef PSP
// Even with double GMLReal, use PSP's VFPU for sin/cos/sqrt by round-tripping
// through float. VFPU runs in ~1 cycle vs libm's ~100+ cycles for software
// double-precision math. Precision loss is acceptable for game logic (matches
// what original GameMaker delivered on Windows with float-ish math).
static inline float _vfpu_sinf_dbl(float x) {
    float s;
    __asm__ volatile (
        "mtv      %1, s000\n"
        "vcst.s   s001, VFPU_2_PI\n"
        "vmul.s   s000, s000, s001\n"
        "vsin.s   s000, s000\n"
        "mfv      %0, s000\n"
        : "=r"(s) : "r"(x)
    );
    return s;
}
static inline float _vfpu_cosf_dbl(float x) {
    float c;
    __asm__ volatile (
        "mtv      %1, s000\n"
        "vcst.s   s001, VFPU_2_PI\n"
        "vmul.s   s000, s000, s001\n"
        "vcos.s   s000, s000\n"
        "mfv      %0, s000\n"
        : "=r"(c) : "r"(x)
    );
    return c;
}
static inline float _vfpu_sqrtf_dbl(float x) {
    float r;
    __asm__ volatile (
        "mtv      %1, s000\n"
        "vsqrt.s  s000, s000\n"
        "mfv      %0, s000\n"
        : "=r"(r) : "r"(x)
    );
    return r;
}
#define GMLReal_sin(x)  ((double)_vfpu_sinf_dbl((float)(x)))
#define GMLReal_cos(x)  ((double)_vfpu_cosf_dbl((float)(x)))
#define GMLReal_sqrt(x) ((double)_vfpu_sqrtf_dbl((float)(x)))
#define GMLReal_atan2 atan2
#else // non-PSP double
#define GMLReal_sin sin
#define GMLReal_cos cos
#define GMLReal_atan2 atan2
#define GMLReal_sqrt sqrt
#endif // PSP

#define GMLReal_fabs fabs
#define GMLReal_fmod fmod
#define GMLReal_floor floor
#define GMLReal_ceil ceil
#define GMLReal_round round
#define GMLReal_pow pow
#define GMLReal_fmax fmax
#define GMLReal_fmin fmin
#define GMLReal_strtod(str, endptr) strtod(str, endptr)

#endif // USE_FLOAT_REALS
