/* apps/graphics/fpcwasm/jit/test/progs/kernels.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * sdk/bench/kernels.c made small, each kernel alone by its letter (the
 * first argument) or all of them: make progs runs it JIT and interpreted.
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
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
static int tree(void) { struct node *t = 0; uint32_t x = 1; np = 0; for (int i = 0; i < 1000; i++) { x = x * 1103515245 + 12345; t = ins(t, x >> 8); } return sum(t); }
int main(int argc, char **argv) {
  static uint8_t buf[4096]; for (int i = 0; i < 4096; i++) buf[i] = i * 7;
  int k = argc > 1 ? argv[1][0] : 0;
  long long r = 0;
  if (!k || k == 'f') r += fib(18);
  if (!k || k == 's') r += sieve(10000);
  if (!k || k == 'l') r += (int)fl(10000);
  if (!k || k == 'd') r += (long long)dl(2000);
  if (!k || k == 'i') r += ll(10000);
  if (!k || k == 'c') r += crc(buf, 4096);
  if (!k || k == 'm') r += mat(16);
  if (!k || k == 't') r += tree();
  printf("%lld\n", r);
  return 0;
}
