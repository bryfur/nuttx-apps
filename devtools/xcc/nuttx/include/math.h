/* apps/devtools/xcc/nuttx/include/math.h
 *
 * SPDX-License-Identifier: MIT
 *
 * math.h for xcc's NuttX modules: the firmware's libm (the toolchain's,
 * CONFIG_LIBM_TOOLCHAIN) by the AAPCS hard-float ABI: doubles in d0-d7,
 * floats in s0-s15.  The functions are in the firmware's symbol table
 * (devtools/xcc/nuttx/symtab.list).
 */

#ifndef __XCC_NUTTX_MATH_H
#define __XCC_NUTTX_MATH_H

#define M_E         2.7182818284590452354
#define M_LOG2E     1.4426950408889634074
#define M_LOG10E    0.43429448190325182765
#define M_LN2       0.69314718055994530942
#define M_LN10      2.30258509299404568402
#define M_PI        3.14159265358979323846
#define M_PI_2      1.57079632679489661923
#define M_PI_4      0.78539816339744830962
#define M_1_PI      0.31830988618379067154
#define M_2_PI      0.63661977236758134308
#define M_2_SQRTPI  1.12837916709551257390
#define M_SQRT2     1.41421356237309504880
#define M_SQRT1_2   0.70710678118654752440

#define NAN         (__builtin_nan("0x7ff8000000000000"))
#define INFINITY    (1.0 / 0.0)
#define HUGE_VAL    INFINITY
#define HUGE_VALF   ((float)INFINITY)

#define isnan(x)     ((x) != (x))
#define isinf(x)     ((x) == (x) && (x) != 0 && (x) == (x) * 2)
#define isfinite(x)  (!isnan(x) && !isinf(x))
#define signbit(x)   ((x) < 0 || (1 / (x)) < 0)

double sin(double x);
double cos(double x);
double tan(double x);
double asin(double x);
double acos(double x);
double atan(double x);
double atan2(double y, double x);
double sinh(double x);
double cosh(double x);
double tanh(double x);
double exp(double x);
double expm1(double x);
double log(double x);
double log10(double x);
double log2(double x);
double log1p(double x);
double pow(double x, double y);
double sqrt(double x);
double cbrt(double x);
double hypot(double x, double y);
double ceil(double x);
double floor(double x);
double round(double x);
double trunc(double x);
double fabs(double x);
double fmod(double x, double y);
double frexp(double x, int *exp);
double ldexp(double x, int exp);
double modf(double x, double *iptr);
double fmin(double x, double y);
double fmax(double x, double y);
double copysign(double x, double y);

float sinf(float x);
float cosf(float x);
float tanf(float x);
float asinf(float x);
float acosf(float x);
float atanf(float x);
float atan2f(float y, float x);
float expf(float x);
float logf(float x);
float log10f(float x);
float powf(float x, float y);
float sqrtf(float x);
float ceilf(float x);
float floorf(float x);
float roundf(float x);
float truncf(float x);
float fabsf(float x);
float fmodf(float x, float y);
float hypotf(float x, float y);

#endif
