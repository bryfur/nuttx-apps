/* apps/graphics/fpcwasm/jit/test/progs/wide.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * 64-bit arithmetic in a loop with many locals used in it (the JIT once ran
 * out of registers on sdk/examples/mandel's: pairs of temporaries left too
 * few), fixed point and double, a checksum printed.
 */

#include <stdint.h>
#include <stdio.h>

static uint32_t mandel(int w, int h, int depth, double cx, double cy,
                       double scale)
{
  int32_t step = (int32_t)(scale * 65536.0);
  int32_t x0 = (int32_t)((cx - scale * w / 2) * 65536.0);
  int32_t y0 = (int32_t)((cy - scale * h / 2) * 65536.0);
  uint32_t sum = 0;
  int x;
  int y;

  for (y = 0; y < h; y++)
    {
      int32_t ci = y0 + y * step;

      for (x = 0; x < w; x++)
        {
          int32_t cr = x0 + x * step;
          int32_t zr = 0;
          int32_t zi = 0;
          int n;

          for (n = 0; n < depth; n++)
            {
              int64_t rr = (int64_t)zr * zr;
              int64_t ii = (int64_t)zi * zi;

              if (rr + ii > ((int64_t)4 << 32))
                {
                  break;
                }

              zi = (int32_t)(((int64_t)zr * zi) >> 15) + ci;
              zr = (int32_t)((rr - ii) >> 16) + cr;
            }

          sum = sum * 31 + (uint32_t)n;
        }
    }

  return sum;
}

int main(void)
{
  printf("%08x\n", (unsigned)mandel(64, 48, 64, -0.6, 0.0, 3.0 / 64));
  printf("%08x\n", (unsigned)mandel(40, 30, 200, -0.745, 0.11, 0.0005));
  return 0;
}
