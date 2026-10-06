#include <stdarg.h>
#include "abi.h"
double g_mix(int a, long long b, int c, double d, float e, float f, double g, int h, float i, long long j) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10;
}
double g_vsum(int n, ...) {
  va_list ap; va_start(ap, n); double s = 0;
  for (int k = 0; k < n; k++) {
    int t = va_arg(ap, int);
    if (t == 0) s += va_arg(ap, int);
    else if (t == 1) s += va_arg(ap, long long);
    else if (t == 2) s += va_arg(ap, double);
    else { S3 x = va_arg(ap, S3); s += x.a + x.b * 10 + x.c * 100; }
    s *= 1.5;
  }
  va_end(ap); return s;
}
long long g_ll(long long a, long long b, int c) { return a * b + c; }
float g_f(float a, double b, float c, float d, float e, float f, float g, float h, float i, float j, float k, float l, float m, float n, float o, float p, float q, float r) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10 + k * 11 + l * 12 + m * 13 + n * 14 + o * 15 + p * 16 + q * 17 + r * 18;
}
int g_s3(S3 s, int x, S1 t, S4 u) { return s.a + s.b * 10 + s.c * 100 + x * 1000 + t.c * 10000 + u.x * 3 + u.y * 7; }
S3 g_rs3(int a, int b, int c) { S3 s = { a, b * 2, c * 3 }; return s; }
S4 g_rs4(short x, short y) { S4 s = { x + 1, y + 2 }; return s; }
double g_sd(int a, SD s, int b) { return a + s.d * 10 + s.i * 100 + b * 1000; }
long long g_sq(int a, SQ s, int b, SQ t) { return a + s.q * 10 + s.c * 100 + b * 1000 + t.q * 10000 + t.c * 7; }
int g_fp(int (*f)(int, double), int x) { return f(x, 2.5) + 1; }
int g_callx(void) {
  S3 s = { 1, 2, 3 }; S1 t = { 4 }; S4 u = { 5, 6 }; SD sd = { 1.5, 2 }; SQ q1 = { 1LL << 33, 3 }, q2 = { 5, 9 };
  int bad = 0;
  if (x_mix(1, 2, 3, 4.5, 5.5f, 6.5f, 7.5, 8, 9.5f, 10) != g_mix(1, 2, 3, 4.5, 5.5f, 6.5f, 7.5, 8, 9.5f, 10)) bad |= 1;
  if (x_vsum(4, 0, 7, 1, 1LL << 40, 2, 3.25, 3, s) != g_vsum(4, 0, 7, 1, 1LL << 40, 2, 3.25, 3, s)) bad |= 2;
  if (x_s3(s, 9, t, u) != g_s3(s, 9, t, u)) bad |= 4;
  S3 r = x_rs3(4, 5, 6); if (r.a != 4 || r.b != 10 || r.c != 18) bad |= 8;
  if (x_sd(3, sd, 4) != g_sd(3, sd, 4)) bad |= 16;
  if (x_sq(1, q1, 2, q2) != g_sq(1, q1, 2, q2)) bad |= 32;
  if (g_fp(x_cb, 3) != 3 + 25 + 1) bad |= 64;
  return bad;
}
