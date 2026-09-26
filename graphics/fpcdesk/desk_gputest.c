/****************************************************************************
 * apps/graphics/fpcdesk/desk_gputest.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * deskctl test: BLEND on the GPU itself, checked pixel by pixel.  Each case
 * PUTs a known destination (and source) into off-screen lines, BLENDs,
 * reads the result back (twice, to rule out the readback line) and
 * compares it with docs/gpu.md's arithmetic done here.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef CONFIG_FPC_GPU

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TW       64                  /* Test rectangle */
#define TH       16
#define TY       1980                /* Off-screen, the last lines of the
                                      * 2048-line space: destination */
#define SY       2010                /* ... source lines */
#define SLOT     6

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct fpc_gpu_blend_s
{
  int16_t  dx;
  int16_t  dy;
  int16_t  w;
  int16_t  h;
  int16_t  sx;
  int16_t  sy;
  uint16_t colour;
  uint8_t  alpha;
  uint8_t  mode;
  uint32_t p4;
  uint32_t p5;
  uint16_t key;
  uint8_t  shift;
  uint8_t  slot;
};

/* The driver (a flat build: kernel symbols) */

void fpc_gpu_put(int x, int y, int w, int h, const uint16_t *src,
                 size_t stride_bytes);
void fpc_gpu_blend(const struct fpc_gpu_blend_s *b);
void fpc_gpu_set_lut(int slot, const uint8_t lut[32][32]);
void fpc_gpu_flush(void);
int  fpc_gpu_read(int x, int y, int w, int h, uint16_t *dst, size_t stride);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint16_t g_dst[TH][TW];
static uint16_t g_src[TH][TW];
static uint16_t g_want[TH][TW];
static uint16_t g_got[TH][TW];
static uint16_t g_got2[TH][TW];
static uint8_t  g_lut[32][32];
static uint32_t g_rng = 12345;

static const uint8_t g_bayer[4][4] =
{
  { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t rnd(void)
{
  g_rng = g_rng * 1103515245u + 12345u;
  return g_rng >> 8;
}

static uint32_t mul8(uint32_t p, uint32_t q)
{
  return (p * q + 255) >> 8;
}

static uint16_t argb_rgb(uint16_t s)
{
  uint32_t r = (s >> 8) & 15;
  uint32_t g = (s >> 4) & 15;
  uint32_t b = s & 15;

  return (uint16_t)((r << 1 | r >> 3) << 11 | (g << 2 | g >> 2) << 5 |
                    (b << 1 | b >> 3));
}

static uint16_t blend_px(uint16_t s, uint16_t d, uint32_t a, uint32_t bias)
{
  uint32_t ap = a + (a >> 7);
  uint32_t r = ((s >> 11) * ap + (d >> 11) * (256 - ap) + bias) >> 8;
  uint32_t g = (((s >> 5) & 63) * ap + ((d >> 5) & 63) * (256 - ap) +
                bias) >> 8;
  uint32_t b = ((s & 31) * ap + (d & 31) * (256 - ap) + bias) >> 8;

  return (uint16_t)(r << 11 | g << 5 | b);
}

static int32_t dist(int32_t v, int32_t lo, int32_t hi, uint32_t shift)
{
  int32_t d = v < lo ? lo - v : v > hi ? v - hi : 0;

  d >>= shift;
  return d > 31 ? 31 : d;
}

/* The expected result of b over g_dst (at TY) from g_src (at SY) */

static void model(const struct fpc_gpu_blend_s *b)
{
  uint32_t sm = b->mode & 3;
  uint32_t mm = (b->mode >> 2) & 3;
  int i;
  int j;

  for (j = 0; j < TH; j++)
    {
      for (i = 0; i < TW; i++)
        {
          int32_t x = b->dx + i;
          int32_t y = b->dy + j;
          uint16_t s = g_src[j][i];
          uint16_t S;
          uint32_t fs;
          uint32_t m;
          uint32_t bias;

          switch (sm)
            {
              case 0:  S = b->colour; fs = 255; break;
              case 1:  S = s; fs = 255; break;
              case 2:  S = argb_rgb(s); fs = (s >> 12) * 17u; break;
              default: S = b->colour; fs = s & 0xff; break;
            }

          if ((b->mode & 0x40) && sm && s == b->key)
            {
              fs = 0;
            }

          if (mm == 0)
            {
              m = 255;
            }
          else if (mm == 1)
            {
              m = g_lut[dist(y, (int16_t)(b->p4 >> 16),
                             (int16_t)(b->p5 >> 16), b->shift)]
                       [dist(x, (int16_t)(b->p4 & 0xffff),
                             (int16_t)(b->p5 & 0xffff), b->shift)];
            }
          else
            {
              int32_t c = mm == 2 ? x : y;
              int32_t v = (int32_t)(b->p5 & 0xffff) +
                          (c - (int16_t)(b->p4 & 0xffff)) *
                          (int16_t)(b->p4 >> 16);

              v >>= 8;
              m = v < 0 ? 0 : v > 255 ? 255 : (uint32_t)v;
            }

          bias = (b->mode & 0x80) ? g_bayer[y & 3][x & 3] * 16u + 8 : 128;
          g_want[j][i] = blend_px(S, g_dst[j][i],
                                  mul8(mul8(b->alpha, fs), m), bias);
        }
    }
}

static int run(const char *name, struct fpc_gpu_blend_s *b)
{
  int bad = 0;
  int shown = 0;
  int i;
  int j;

  for (j = 0; j < TH; j++)
    {
      for (i = 0; i < TW; i++)
        {
          g_dst[j][i] = (uint16_t)rnd();
          g_src[j][i] = (uint16_t)rnd();
        }
    }

  b->dx = 3;
  b->dy = TY;
  b->w  = TW;
  b->h  = TH;
  b->sx = 5;
  b->sy = SY;

  fpc_gpu_put(b->dx, TY, TW, TH, &g_dst[0][0], TW * 2);
  fpc_gpu_put(b->sx, SY, TW, TH, &g_src[0][0], TW * 2);
  fpc_gpu_blend(b);
  fpc_gpu_flush();
  model(b);

  if (fpc_gpu_read(b->dx, TY, TW, TH, &g_got[0][0], TW * 2) < 0 ||
      fpc_gpu_read(b->dx, TY, TW, TH, &g_got2[0][0], TW * 2) < 0)
    {
      printf("%-22s readback failed\n", name);
      return 1;
    }

  for (j = 0; j < TH; j++)
    {
      for (i = 0; i < TW; i++)
        {
          if (g_got[j][i] != g_got2[j][i])
            {
              continue;                   /* The readback line, not BLEND */
            }

          if (g_got[j][i] != g_want[j][i])
            {
              if (shown++ < 4)
                {
                  printf("  (%2d,%2d) dst %04x src %04x: got %04x want "
                         "%04x\n", i, j, g_dst[j][i], g_src[j][i],
                         g_got[j][i], g_want[j][i]);
                }

              bad++;
            }
        }
    }

  printf("%-22s %4d of %d wrong\n", name, bad, TW * TH);
  return bad != 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int desk_gputest(void)
{
  struct fpc_gpu_blend_s b;
  int fails = 0;
  int i;
  int j;

  for (j = 0; j < 32; j++)
    {
      for (i = 0; i < 32; i++)
        {
          g_lut[j][i] = (uint8_t)rnd();
        }
    }

  fpc_gpu_set_lut(SLOT, g_lut);

  memset(&b, 0, sizeof(b));
  b.colour = 0xf800; b.alpha = 255; b.mode = 0;
  fails += run("colour a255", &b);
  b.colour = 0x07e0; b.alpha = 128;
  fails += run("colour a128", &b);
  b.alpha = 77; b.mode = 0x80;
  fails += run("colour a77 dither", &b);
  b.alpha = 255; b.mode = 0x01;
  fails += run("rgb565 a255", &b);
  b.alpha = 100;
  fails += run("rgb565 a100", &b);
  b.alpha = 200; b.mode = 0x02;
  fails += run("argb4444", &b);
  b.colour = 0xffff; b.alpha = 255; b.mode = 0x03;
  fails += run("a8 tint", &b);
  b.mode = 0x41; b.key = 0; b.alpha = 180;
  fails += run("rgb565 key", &b);

  b.mode = 0x04 | ((SLOT & 3) << 4); b.slot = SLOT; b.alpha = 255;
  b.colour = 0x001f;
  b.p4 = (uint32_t)(10 & 0xffff) | (uint32_t)(TY + 4) << 16;
  b.p5 = (uint32_t)(40 & 0xffff) | (uint32_t)(TY + 8) << 16;
  fails += run("lut slot 6", &b);
  b.shift = 1;
  fails += run("lut slot 6 shift 1", &b);
  b.shift = 0; b.slot = 0;

  b.mode = 0x08 | 0x80; b.alpha = 255; b.colour = 0xffe0;
  b.p4 = (uint32_t)(0 & 0xffff) | (uint32_t)(uint16_t)(3 * 256) << 16;
  b.p5 = 0;
  fails += run("h ramp dither", &b);
  b.mode = 0x0c;
  b.p4 = (uint32_t)(TY & 0xffff) | (uint32_t)(uint16_t)(16 * 256) << 16;
  fails += run("v ramp", &b);

  printf("%s\n", fails ? "BLEND: FAIL" : "BLEND: all good");
  return fails;
}

#endif
