/****************************************************************************
 * apps/graphics/fpcwasm/wasm_image.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Image files for WebAssembly apps (fpc_image_decode(), fpc_image_load()):
 * BMP (uncompressed, 8, 24 or 32 bits a pixel, either way up) and QOI
 * (the "Quite OK Image" format: lossless, small, and decoded in a page of
 * code), into a new image of the game SDK, a band of rows at a time.
 * Pixels whose alpha is below 128 become FG_KEY (transparent); a colour
 * that happens to be FG_KEY is moved off it.  Every read is checked
 * against the file's bytes: a broken file is refused, never overrun.
 ****************************************************************************/

#include "wasm_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "fpcgame.h"

#define BAND_PIX  4096                    /* Pixels converted at a time */
#define MAX_SIDE  2048

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
  uint16_t c;

  if (a < 128)
    {
      return FG_KEY;
    }

  c = FG_RGB(r, g, b);
  return c == FG_KEY ? FG_KEY ^ 0x0020 : c;
}

static uint32_t le16(const uint8_t *p)
{
  return p[0] | (uint32_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static uint32_t be32(const uint8_t *p)
{
  return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 |
         (uint32_t)p[2] << 8 | p[3];
}

/* A new image and the band its rows are put through */

struct out_s
{
  int img;
  int w;
  int h;
  int rows;                               /* Rows a band */
  int y0;                                 /* The band's first row */
  int n;                                  /* Rows in it so far */
  FAR uint16_t *band;
};

static int out_open(FAR struct out_s *o, int w, int h)
{
  if (w <= 0 || h <= 0 || w > MAX_SIDE || h > MAX_SIDE)
    {
      return -EINVAL;
    }

  o->w    = w;
  o->h    = h;
  o->rows = BAND_PIX / w > 0 ? BAND_PIX / w : 1;
  o->y0   = 0;
  o->n    = 0;
  o->band = malloc((size_t)o->rows * w * 2);
  if (o->band == NULL)
    {
      return -ENOMEM;
    }

  o->img = fg_image_new(w, h);
  if (o->img < 0)
    {
      free(o->band);
      return o->img;
    }

  return OK;
}

static void out_flush(FAR struct out_s *o)
{
  if (o->n > 0)
    {
      fg_image_put(o->img, 0, o->y0, o->w, o->n, o->band, (size_t)o->w * 2);
      o->y0 += o->n;
      o->n = 0;
    }
}

/* The next row (top to bottom) */

static FAR uint16_t *out_row(FAR struct out_s *o)
{
  if (o->n == o->rows)
    {
      out_flush(o);
    }

  return o->band + (size_t)o->n++ * o->w;
}

static int out_close(FAR struct out_s *o, bool ok)
{
  if (ok)
    {
      out_flush(o);
    }
  else
    {
      fg_image_free(o->img);
    }

  free(o->band);
  return ok ? o->img : -EINVAL;
}

/* BMP: BITMAPFILEHEADER (14 bytes) + an info header of 40 bytes or more;
 * rows padded to 4 bytes, bottom up unless the height is negative
 */

static int bmp_decode(FAR const uint8_t *d, size_t len)
{
  struct out_s o;
  uint32_t off;
  uint32_t hsize;
  int32_t w;
  int32_t h;
  uint32_t bpp;
  uint32_t comp;
  uint32_t ncol;
  size_t stride;
  bool topdown;
  uint16_t pal[256];
  int ret;
  int y;

  if (len < 54)
    {
      return -EINVAL;
    }

  off   = le32(d + 10);
  hsize = le32(d + 14);
  w     = (int32_t)le32(d + 18);
  h     = (int32_t)le32(d + 22);
  bpp   = le16(d + 28);
  comp  = le32(d + 30);
  ncol  = le32(d + 46);
  topdown = h < 0;
  h = topdown ? -h : h;

  if (hsize < 40 || (uint64_t)14 + hsize > len || w <= 0 || h <= 0 ||
      w > MAX_SIDE || h > MAX_SIDE ||
      !(bpp == 8 || bpp == 24 || bpp == 32) ||
      !(comp == 0 || (comp == 3 && bpp == 32)))
    {
      return -EINVAL;
    }

  stride = (((size_t)w * bpp / 8) + 3) & ~(size_t)3;
  if ((uint64_t)off + (uint64_t)stride * h > len)
    {
      return -EINVAL;
    }

  if (bpp == 8)
    {
      uint32_t i;

      ncol = ncol == 0 || ncol > 256 ? 256 : ncol;
      if ((uint64_t)14 + hsize + ncol * 4 > len)
        {
          return -EINVAL;
        }

      memset(pal, 0, sizeof(pal));
      for (i = 0; i < ncol; i++)
        {
          FAR const uint8_t *q = d + 14 + hsize + i * 4;

          pal[i] = rgb565(q[2], q[1], q[0], 255);
        }
    }

  ret = out_open(&o, w, h);
  if (ret < 0)
    {
      return ret;
    }

  for (y = 0; y < h; y++)
    {
      FAR const uint8_t *s = d + off +
                             stride * (size_t)(topdown ? y : h - 1 - y);
      FAR uint16_t *row = out_row(&o);
      int x;

      for (x = 0; x < w; x++)
        {
          if (bpp == 8)
            {
              row[x] = pal[s[x]];
            }
          else if (bpp == 24)
            {
              row[x] = rgb565(s[3 * x + 2], s[3 * x + 1], s[3 * x], 255);
            }
          else
            {
              /* 32 bits: BGRA (BI_BITFIELDS taken as that order too) */

              row[x] = rgb565(s[4 * x + 2], s[4 * x + 1], s[4 * x],
                              s[4 * x + 3]);
            }
        }
    }

  return out_close(&o, true);
}

/* QOI (qoiformat.org): a 14-byte header ("qoif", width, height big-endian,
 * channels, colour space), the chunks, 7 zeros and a 1
 */

static int qoi_decode(FAR const uint8_t *d, size_t len)
{
  uint8_t idx[64][4];
  uint8_t px[4] =
  {
    0, 0, 0, 255
  };

  struct out_s o;
  uint32_t w;
  uint32_t h;
  size_t p = 14;
  size_t end;
  uint32_t run = 0;
  uint32_t y;
  int ret;

  if (len < 14 + 8 || memcmp(d, "qoif", 4) != 0)
    {
      return -EINVAL;
    }

  w = be32(d + 4);
  h = be32(d + 8);
  if (w == 0 || h == 0 || w > MAX_SIDE || h > MAX_SIDE)
    {
      return -EINVAL;
    }

  end = len - 8;
  memset(idx, 0, sizeof(idx));
  ret = out_open(&o, (int)w, (int)h);
  if (ret < 0)
    {
      return ret;
    }

  for (y = 0; y < h; y++)
    {
      FAR uint16_t *row = out_row(&o);
      uint32_t x;

      for (x = 0; x < w; x++)
        {
          if (run > 0)
            {
              run--;
            }
          else
            {
              uint8_t b;

              if (p >= end)
                {
                  return out_close(&o, false);
                }

              b = d[p++];
              if (b == 0xfe || b == 0xff)
                {
                  size_t n = b == 0xfe ? 3 : 4;

                  if (p + n > end)
                    {
                      return out_close(&o, false);
                    }

                  memcpy(px, d + p, n);
                  p += n;
                }
              else if ((b & 0xc0) == 0x00)
                {
                  memcpy(px, idx[b & 63], 4);
                }
              else if ((b & 0xc0) == 0x40)
                {
                  px[0] += ((b >> 4) & 3) - 2;
                  px[1] += ((b >> 2) & 3) - 2;
                  px[2] += (b & 3) - 2;
                }
              else if ((b & 0xc0) == 0x80)
                {
                  int dg;
                  uint8_t b2;

                  if (p >= end)
                    {
                      return out_close(&o, false);
                    }

                  b2 = d[p++];
                  dg = (b & 63) - 32;
                  px[0] += dg - 8 + ((b2 >> 4) & 15);
                  px[1] += dg;
                  px[2] += dg - 8 + (b2 & 15);
                }
              else
                {
                  run = b & 63;           /* This pixel and run more */
                }

              memcpy(idx[(px[0] * 3 + px[1] * 5 + px[2] * 7 + px[3] * 11) &
                         63], px, 4);
            }

          row[x] = rgb565(px[0], px[1], px[2], px[3]);
        }
    }

  return out_close(&o, true);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int wasm_image_decode(FAR const uint8_t *data, size_t len)
{
  if (len >= 4 && memcmp(data, "qoif", 4) == 0)
    {
      return qoi_decode(data, len);
    }

  if (len >= 2 && data[0] == 'B' && data[1] == 'M')
    {
      return bmp_decode(data, len);
    }

  return -EINVAL;
}
