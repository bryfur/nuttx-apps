#include <stdio.h>
#include <stdarg.h>
#include "abi.h"
double x_mix(int a, long long b, int c, double d, float e, float f, double g, int h, float i, long long j) {
  return a + b * 2 + c * 3 + d * 4 + e * 5 + f * 6 + g * 7 + h * 8 + i * 9 + j * 10;
}
double x_vsum(int n, ...) {
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
int x_s3(S3 s, int x, S1 t, S4 u) { return s.a + s.b * 10 + s.c * 100 + x * 1000 + t.c * 10000 + u.x * 3 + u.y * 7; }
S3 x_rs3(int a, int b, int c) { S3 s = { a, b * 2, c * 3 }; return s; }
double x_sd(int a, SD s, int b) { return a + s.d * 10 + s.i * 100 + b * 1000; }
long long x_sq(int a, SQ s, int b, SQ t) { return a + s.q * 10 + s.c * 100 + b * 1000 + t.q * 10000 + t.c * 7; }
int x_cb(int a, double d) { return a + (int)(d * 10); }

int main(void) {
  int bad = 0;
  S3 s = { 1, 2, 3 }; S1 t = { 4 }; S4 u = { 5, 6 }; SD sd = { 1.5, 2 }; SQ q1 = { 1LL << 33, 3 }, q2 = { 5, 9 };
  #define CHECK(n, c) do { if (!(c)) { printf("FAIL %s\n", n); bad++; } } while (0)
  CHECK("mix", g_mix(1, 2, 3, 4.5, 5.5f, 6.5f, 7.5, 8, 9.5f, 10) == x_mix(1, 2, 3, 4.5, 5.5f, 6.5f, 7.5, 8, 9.5f, 10));
  CHECK("vsum", g_vsum(4, 0, 7, 1, 1LL << 40, 2, 3.25, 3, s) == x_vsum(4, 0, 7, 1, 1LL << 40, 2, 3.25, 3, s));
  CHECK("ll", g_ll(1LL << 33, 3, 5) == (1LL << 33) * 3 + 5);
  CHECK("f", g_f(1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18) == 1+4+9+16+25+36+49+64+81+100+121+144+169+196+225+256+289+324);
  CHECK("s3", g_s3(s, 9, t, u) == x_s3(s, 9, t, u));
  S3 r = g_rs3(4, 5, 6); CHECK("rs3", r.a == 4 && r.b == 10 && r.c == 18);
  S4 r4 = g_rs4(7, 8); CHECK("rs4", r4.x == 8 && r4.y == 10);
  CHECK("sd", g_sd(3, sd, 4) == x_sd(3, sd, 4));
  CHECK("sq", g_sq(1, q1, 2, q2) == x_sq(1, q1, 2, q2));
  CHECK("fp", g_fp(x_cb, 3) == 3 + 25 + 1);
  int cx = g_callx(); CHECK("callx", cx == 0); if (cx) printf("callx bits %x\n", cx);
  printf("abi: %d failures\n", bad);
  return bad;
}
