#include <stdio.h>
#include "abi.h"
int main(void) {
  S3 s = { 1, 2, 3 };
  printf("%g %g\n", g_vsum(1, 0, 7), x_vsum(1, 0, 7));
  printf("%g %g\n", g_vsum(1, 1, 1LL << 40), x_vsum(1, 1, 1LL << 40));
  printf("%g %g\n", g_vsum(1, 2, 3.25), x_vsum(1, 2, 3.25));
  printf("%g %g\n", g_vsum(1, 3, s), x_vsum(1, 3, s));
  printf("%g %g\n", g_vsum(2, 0, 7, 3, s), x_vsum(2, 0, 7, 3, s));
  printf("%g %g\n", g_vsum(3, 0, 7, 2, 3.25, 3, s), x_vsum(3, 0, 7, 2, 3.25, 3, s));
  return 0;
}
