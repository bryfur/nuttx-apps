/* apps/graphics/fpcwasm/jit/test/progs/matconst.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A matrix product of a constant size, clang unrolling it: static arrays
 * at offsets over 4 KB from a local (a miscompile once).
 */

#include <stdio.h>
static int mat(int n) { static int a[32][32], b[32][32], c[32][32]; for (int i=0;i<n;i++) for (int j=0;j<n;j++) { a[i][j]=i+j; b[i][j]=i-j; }
  for (int i=0;i<n;i++) for (int j=0;j<n;j++) { int s=0; for (int k=0;k<n;k++) s+=a[i][k]*b[k][j]; c[i][j]=s; } return c[n-1][n-2]; }
int main(int argc, char **argv) { printf("%d\n", mat(16)); return 0; }
