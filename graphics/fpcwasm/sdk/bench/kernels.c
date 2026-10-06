/* apps/graphics/fpcwasm/sdk/bench/kernels.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small kernels, each timed (bench.sh): calls (fib), memory (sieve),
 * f32, f64, i64, bytes and shifts (crc), a matrix, pointers (a tree).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
static uint64_t now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000ull + t.tv_nsec / 1000; }
static uint32_t fib(uint32_t n) { return n < 2 ? n : fib(n - 1) + fib(n - 2); }
static int sieve(int n) {
  static char s[100000]; int c = 0;
  memset(s, 1, n);
  for (int i = 2; i < n; i++) if (s[i]) { c++; for (int j = i + i; j < n; j += i) s[j] = 0; }
  return c;
}
static float fl(int n) { float a = 0.5f, s = 0; for (int i = 0; i < n; i++) { s += a * i; a = a * 0.999f + 0.001f; } return s; }
static double dl(int n) { double a = 0.5, s = 0; for (int i = 0; i < n; i++) { s += a * i; a = a * 0.999 + 0.001; } return s; }
static uint64_t ll(int n) { uint64_t x = 1; for (int i = 0; i < n; i++) { x = x * 6364136223846793005ull + 1442695040888963407ull; x ^= x >> 13; } return x; }
static uint32_t crc(const uint8_t *p, int n) { uint32_t c = ~0u; for (int i = 0; i < n; i++) { c ^= p[i]; for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xedb88320u & -(c & 1)); } return ~c; }
static int mat(int n) { static int a[32][32], b[32][32], c[32][32]; for (int i=0;i<n;i++) for (int j=0;j<n;j++) { a[i][j]=i+j; b[i][j]=i-j; }
  for (int i=0;i<n;i++) for (int j=0;j<n;j++) { int s=0; for (int k=0;k<n;k++) s+=a[i][k]*b[k][j]; c[i][j]=s; } return c[n-1][n-2]; }
struct node { int v; struct node *l, *r; };
static struct node pool[4096]; static int np;
static struct node *ins(struct node *t, int v) { if (!t) { t = &pool[np++]; t->v = v; t->l = t->r = 0; return t; } if (v < t->v) t->l = ins(t->l, v); else t->r = ins(t->r, v); return t; }
static int sum(struct node *t) { return t ? t->v + sum(t->l) + sum(t->r) : 0; }
static int tree(void) { struct node *t = 0; uint32_t x = 1; np = 0; for (int i = 0; i < 4000; i++) { x = x * 1103515245 + 12345; t = ins(t, x >> 8); } return sum(t); }
#define T(name, expr) do { uint64_t t0 = now(); long long r = (long long)(expr); printf("%-8s %8llu us  %lld\n", name, (unsigned long long)(now() - t0), r); } while (0)
int main(void) {
  static uint8_t buf[32768]; for (int i = 0; i < 32768; i++) buf[i] = i * 7;
  T("fib", fib(25));
  T("sieve", sieve(100000));
  T("float", (int)fl(100000));
  T("double", (long long)dl(20000));
  T("i64", ll(100000));
  T("crc", crc(buf, 32768));
  T("matmul", mat(32));
  T("tree", tree());
  return 0;
}
