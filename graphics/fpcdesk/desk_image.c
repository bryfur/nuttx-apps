/****************************************************************************
 * apps/graphics/fpcdesk/desk_image.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Images in the GPU's memory (desk_image.h): decoding BMP (1, 4, 8, 16,
 * 24 and 32 bits, uncompressed or with bit fields) and binary PPM, PGM
 * and PBM a row at a time, scaling down by averaging boxes of pixels,
 * and putting strips of RGB565 rows into a hidden window's surface.
 * Saving a 24-bit BMP.  Drawing an image into a window with COPY, BLEND
 * and, enlarged, COPYs of columns and rows.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include <nuttx/nx/nxbe.h>

#include "desk_ui.h"
#include "desk_image.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define STRIP          16                /* Rows put at a time */

#define T_BMP          1
#define T_PNM          2

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A file being decoded, a row at a time, in the file's order */

struct dec_s
{
  FILE *f;
  int type;
  int w;
  int h;
  bool topdown;                          /* Rows top first (else bottom) */
  int bpp;                               /* Bits a pixel in the file */
  int stride;                            /* Bytes a row in the file */
  uint32_t mask[3];                      /* BMP 16/32: R, G, B */
  uint8_t shift[3];
  uint8_t bits[3];
  uint8_t pal[256][3];                   /* BMP 1/4/8: RGB */
  int maxval;                            /* PNM */
  int pnm;                               /* 4, 5, 6: P4, P5, P6 */
  uint8_t *raw;                          /* One row of the file */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The 4 x 4 ordered dither, row by row */

static const uint8_t g_bayer[16] =
{
  0, 8, 2, 10,   12, 4, 14, 6,   3, 11, 1, 9,   15, 7, 13, 5
};

/****************************************************************************
 * Private Functions: reading headers
 ****************************************************************************/

static uint32_t le32(const uint8_t *p)
{
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t *p)
{
  return p[0] | p[1] << 8;
}

static void mask_info(uint32_t m, uint8_t *shift, uint8_t *bits)
{
  int s = 0;
  int b = 0;

  if (m != 0)
    {
      while (!(m & 1))
        {
          m >>= 1;
          s++;
        }

      while (m & 1)
        {
          m >>= 1;
          b++;
        }
    }

  *shift = s;
  *bits = b > 8 ? 8 : b;                 /* The top 8 bits are enough */
  if (b > 8)
    {
      *shift = s + b - 8;
    }
}

static int bmp_open(struct dec_s *d, char *err, int errlen)
{
  uint8_t h[124];
  uint32_t off;
  uint32_t dib;
  uint32_t comp = 0;
  uint32_t ncol = 0;
  int32_t sh;
  int palent;
  int i;

  if (fread(h, 1, 18, d->f) != 18)
    {
      snprintf(err, errlen, "the file is cut short");
      return -EINVAL;
    }

  off = le32(h + 10);
  dib = le32(h + 14);
  if (dib < 12 || dib > sizeof(h) - 14 + 14 ||
      fread(h + 18, 1, dib - 4, d->f) != dib - 4)
    {
      snprintf(err, errlen, "a BMP header this viewer does not know");
      return -EINVAL;
    }

  if (dib == 12)                         /* OS/2 BITMAPCOREHEADER */
    {
      d->w = le16(h + 18);
      sh = (int16_t)le16(h + 20);
      d->bpp = le16(h + 24);
      palent = 3;
    }
  else
    {
      d->w = (int32_t)le32(h + 18);
      sh = (int32_t)le32(h + 22);
      d->bpp = le16(h + 28);
      comp = le32(h + 30);
      ncol = le32(h + 46);
      palent = 4;
    }

  d->topdown = sh < 0;
  d->h = sh < 0 ? -sh : sh;

  if (comp == 1 || comp == 2)
    {
      snprintf(err, errlen, "compressed (RLE) BMP is not supported");
      return -ENOTSUP;
    }

  if (comp != 0 && comp != 3 && comp != 6)
    {
      snprintf(err, errlen, "compressed BMP (%s) is not supported",
               comp == 4 ? "JPEG" : comp == 5 ? "PNG" : "unknown");
      return -ENOTSUP;
    }

  if (d->bpp != 1 && d->bpp != 4 && d->bpp != 8 && d->bpp != 16 &&
      d->bpp != 24 && d->bpp != 32)
    {
      snprintf(err, errlen, "%d-bit BMP is not supported", d->bpp);
      return -ENOTSUP;
    }

  /* Bit fields: in the header (V2 and later) or just after a 40-byte
   * one; else the defaults
   */

  if (d->bpp == 16 || d->bpp == 32)
    {
      if (comp == 3 || comp == 6)
        {
          if (dib >= 52)
            {
              d->mask[0] = le32(h + 54);
              d->mask[1] = le32(h + 58);
              d->mask[2] = le32(h + 62);
            }
          else
            {
              uint8_t m[12];

              if (fread(m, 1, 12, d->f) != 12)
                {
                  snprintf(err, errlen, "the file is cut short");
                  return -EINVAL;
                }

              d->mask[0] = le32(m);
              d->mask[1] = le32(m + 4);
              d->mask[2] = le32(m + 8);
            }
        }
      else if (d->bpp == 16)
        {
          d->mask[0] = 0x7c00;
          d->mask[1] = 0x03e0;
          d->mask[2] = 0x001f;
        }
      else
        {
          d->mask[0] = 0xff0000;
          d->mask[1] = 0x00ff00;
          d->mask[2] = 0x0000ff;
        }

      for (i = 0; i < 3; i++)
        {
          mask_info(d->mask[i], &d->shift[i], &d->bits[i]);
        }
    }

  /* The palette follows the header (and the masks) */

  if (d->bpp <= 8)
    {
      int n = ncol == 0 || ncol > (1u << d->bpp) ? 1 << d->bpp : (int)ncol;
      uint8_t e[4];

      memset(d->pal, 0, sizeof(d->pal));
      for (i = 0; i < n; i++)
        {
          if (fread(e, 1, palent, d->f) != (size_t)palent)
            {
              snprintf(err, errlen, "the file is cut short");
              return -EINVAL;
            }

          d->pal[i][0] = e[2];
          d->pal[i][1] = e[1];
          d->pal[i][2] = e[0];
        }
    }

  d->stride = ((d->w * d->bpp + 31) / 32) * 4;
  if (fseek(d->f, off, SEEK_SET) < 0)
    {
      snprintf(err, errlen, "the file is cut short");
      return -EINVAL;
    }

  return 0;
}

/* A PNM header field: a number, skipping blanks and comments */

static int pnm_num(FILE *f)
{
  int c;
  int n = 0;

  do
    {
      c = fgetc(f);
      if (c == '#')
        {
          while (c != '\n' && c != EOF)
            {
              c = fgetc(f);
            }
        }
    }
  while (c == ' ' || c == '\t' || c == '\r' || c == '\n');

  if (c < '0' || c > '9')
    {
      return -1;
    }

  while (c >= '0' && c <= '9')
    {
      n = n * 10 + c - '0';
      if (n > 1000000)
        {
          return -1;
        }

      c = fgetc(f);
    }

  /* One blank ends the field (the last one, before the pixels) */

  return n;
}

static int pnm_open(struct dec_s *d, int kind, char *err, int errlen)
{
  d->pnm = kind;
  d->w = pnm_num(d->f);
  d->h = pnm_num(d->f);
  d->maxval = kind == 4 ? 1 : pnm_num(d->f);
  d->topdown = true;
  if (d->w <= 0 || d->h <= 0 || d->maxval <= 0 || d->maxval > 65535)
    {
      snprintf(err, errlen, "a damaged PNM header");
      return -EINVAL;
    }

  d->stride = kind == 4 ? (d->w + 7) / 8 :
              d->w * (kind == 6 ? 3 : 1) * (d->maxval > 255 ? 2 : 1);
  return 0;
}

static void fmt_name(struct dec_s *d, char *fmt, int fmtlen)
{
  if (d->type == T_BMP)
    {
      snprintf(fmt, fmtlen, "BMP, %d-bit", d->bpp);
    }
  else
    {
      snprintf(fmt, fmtlen, "%s", d->pnm == 6 ? "PPM" : d->pnm == 5 ?
               "PGM" : "PBM");
    }
}

static void dec_close(struct dec_s *d)
{
  if (d->f != NULL)
    {
      fclose(d->f);
      d->f = NULL;
    }

  free(d->raw);
  d->raw = NULL;
}

static int dec_open(struct dec_s *d, const char *path, char *err,
                    int errlen)
{
  uint8_t magic[2];
  int ret;

  memset(d, 0, sizeof(*d));
  d->f = fopen(path, "rb");
  if (d->f == NULL)
    {
      ret = -errno;
      snprintf(err, errlen, "%s", strerror(errno));
      return ret;
    }

  if (fread(magic, 1, 2, d->f) != 2)
    {
      snprintf(err, errlen, "the file is empty or cut short");
      ret = -EINVAL;
    }
  else if (magic[0] == 'B' && magic[1] == 'M')
    {
      d->type = T_BMP;
      fseek(d->f, 0, SEEK_SET);
      ret = bmp_open(d, err, errlen);
    }
  else if (magic[0] == 'P' && magic[1] >= '4' && magic[1] <= '6')
    {
      d->type = T_PNM;
      ret = pnm_open(d, magic[1] - '0', err, errlen);
    }
  else if (magic[0] == 'P' && magic[1] >= '1' && magic[1] <= '3')
    {
      snprintf(err, errlen, "text (ASCII) PNM is not supported");
      ret = -ENOTSUP;
    }
  else
    {
      snprintf(err, errlen, "not an image this viewer knows (BMP, PPM, "
               "PGM, PBM)");
      ret = -EINVAL;
    }

  if (ret == 0 && (d->w <= 0 || d->h <= 0))
    {
      snprintf(err, errlen, "an image of no size");
      ret = -EINVAL;
    }

  if (ret == 0 && (d->w > IMG_MAXDIM || d->h > IMG_MAXDIM))
    {
      snprintf(err, errlen, "too large (%d x %d; at most %d a side)", d->w,
               d->h, IMG_MAXDIM);
      ret = -EFBIG;
    }

  if (ret == 0)
    {
      d->raw = malloc(d->stride + 4);
      if (d->raw == NULL)
        {
          snprintf(err, errlen, "out of memory");
          ret = -ENOMEM;
        }
    }

  if (ret < 0)
    {
      dec_close(d);
    }

  return ret;
}

/* The next row of the file (in its order) as RGB888 */

static int dec_row(struct dec_s *d, uint8_t *rgb)
{
  const uint8_t *p = d->raw;
  int x;

  if (fread(d->raw, 1, d->stride, d->f) != (size_t)d->stride)
    {
      return -EINVAL;
    }

  if (d->type == T_PNM)
    {
      int bps = d->maxval > 255 ? 2 : 1;

      for (x = 0; x < d->w; x++)
        {
          int c;
          int k;

          if (d->pnm == 4)
            {
              uint8_t v = (p[x >> 3] >> (7 - (x & 7))) & 1 ? 0 : 255;

              rgb[3 * x] = rgb[3 * x + 1] = rgb[3 * x + 2] = v;
              continue;
            }

          for (c = 0; c < 3; c++)
            {
              int i = d->pnm == 6 ? 3 * x + c : x;
              uint32_t v = bps == 2 ? (p[2 * i] << 8 | p[2 * i + 1]) : p[i];

              k = d->maxval == 255 ? (int)v : (int)(v * 255 / d->maxval);
              rgb[3 * x + c] = k > 255 ? 255 : k;
            }
        }

      return 0;
    }

  switch (d->bpp)
    {
      case 24:
        for (x = 0; x < d->w; x++, p += 3)
          {
            rgb[3 * x]     = p[2];
            rgb[3 * x + 1] = p[1];
            rgb[3 * x + 2] = p[0];
          }
        break;

      case 16:
      case 32:
        for (x = 0; x < d->w; x++)
          {
            uint32_t v = d->bpp == 16 ? le16(p + 2 * x) : le32(p + 4 * x);
            int c;

            for (c = 0; c < 3; c++)
              {
                uint32_t k = (v & d->mask[c]) >> d->shift[c];
                int b = d->bits[c];

                if (d->mask[c] == 0 || b == 0)
                  {
                    k = 0;
                  }
                else if (b < 8)
                  {
                    k = (k & ((1u << b) - 1)) * 255 / ((1u << b) - 1);
                  }

                rgb[3 * x + c] = (uint8_t)k;
              }
          }
        break;

      default:
        for (x = 0; x < d->w; x++)
          {
            int i;

            if (d->bpp == 8)
              {
                i = p[x];
              }
            else if (d->bpp == 4)
              {
                i = (p[x >> 1] >> (x & 1 ? 0 : 4)) & 15;
              }
            else
              {
                i = (p[x >> 3] >> (7 - (x & 7))) & 1;
              }

            memcpy(rgb + 3 * x, d->pal[i], 3);
          }
        break;
    }

  return 0;
}

/****************************************************************************
 * Private Functions: the hidden window
 ****************************************************************************/

static void img_redraw(NXWINDOW hwnd, const struct nxgl_rect_s *rect,
                       bool more, void *arg)
{
  struct desk_image_s *img = arg;

  /* After it was made, the server asks for its pixels only when it has
   * lost them
   */

  if (img->armed)
    {
      img->lost = true;
      if (img->owner != NULL)
        {
          desk_wake(img->owner);
        }
    }
}

static void img_event(NXWINDOW hwnd, enum nx_event_e event, void *arg1,
                      void *arg2)
{
  struct desk_image_s *img = arg2 != NULL ? arg2 : arg1;

  if (event == NXEVENT_SYNCHED || event == NXEVENT_BLOCKED)
    {
      sem_post(&img->sem);
    }
}

static const struct nx_callback_s g_imgcb =
{
  .redraw = img_redraw,
  .event  = img_event,
};

static void img_wait(struct desk_image_s *img)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 2;
  while (sem_timedwait(&img->sem, &ts) < 0 && errno == EINTR)
    {
    }
}

static void img_close(struct desk_image_s *img)
{
  if (img->hwnd == NULL)
    {
      return;
    }

  /* Nothing more from the server for it (a redraw), then it goes */

  img->armed = false;
  if (nx_block(img->hwnd, img) == 0)
    {
      img_wait(img);
    }

  nx_closewindow(img->hwnd);
  img->hwnd = NULL;
}

/* A hidden window of w x h: true if it has a surface in the GPU */

static bool img_open(struct desk_image_s *img, int w, int h)
{
  struct nxgl_size_s size;
  struct nxgl_point_s pos;
  int ret;

  img->hwnd = nx_openwindow(g_desk.hnx,
                            NXBE_WINDOW_RAMBACKED | NXBE_WINDOW_HIDDEN,
                            &g_imgcb, img);
  if (img->hwnd == NULL)
    {
      return false;
    }

  size.w = w;
  size.h = h;
  pos.x  = 0;
  pos.y  = 0;
  nx_setsize(img->hwnd, &size);
  nx_setposition(img->hwnd, &pos);

  /* The redraws of the new territory come before the handshake's end */

  if (nx_synch(img->hwnd, img) == 0)
    {
      img_wait(img);
    }

  img->w = w;
  img->h = h;
  ret = nx_surface(img->hwnd);
  img->gpu = ret > 0;
  img->armed = true;
  return img->gpu;
}

/* A new image of at most w x h: smaller (the same shape) until the GPU
 * has room for it, down to a quarter of the size; then in memory
 */

static struct desk_image_s *img_make(int *w, int *h,
                                     struct desk_win_s *owner)
{
  struct desk_image_s *img = calloc(1, sizeof(*img));
  int w0 = *w;
  int h0 = *h;
  int k;

  if (img == NULL)
    {
      return NULL;
    }

  sem_init(&img->sem, 0, 0);
  for (k = 8; k >= 2; k--)
    {
      int tw = w0 * k / 8 > 0 ? w0 * k / 8 : 1;
      int th = h0 * k / 8 > 0 ? h0 * k / 8 : 1;

      if (img_open(img, tw, th) || k == 2 || tw * th < 128 * 128)
        {
          break;
        }

      img_close(img);
    }

  if (img->hwnd == NULL)
    {
      sem_destroy(&img->sem);
      free(img);
      return NULL;
    }

  *w = img->w;
  *h = img->h;
  img->owner = owner;
  return img;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

bool img_is_image(const char *name)
{
  static const char * const ext[] =
  {
    "bmp", "dib", "ppm", "pgm", "pbm", "pnm", NULL
  };

  const char *dot = strrchr(name, '.');
  int i;

  for (i = 0; dot != NULL && ext[i] != NULL; i++)
    {
      if (strcasecmp(dot + 1, ext[i]) == 0)
        {
          return true;
        }
    }

  return false;
}

int img_info(const char *path, int *w, int *h, char *fmt, int fmtlen,
             char *err, int errlen)
{
  struct dec_s d;
  int ret = dec_open(&d, path, err, errlen);

  if (ret < 0)
    {
      return ret;
    }

  *w = d.w;
  *h = d.h;
  fmt_name(&d, fmt, fmtlen);
  dec_close(&d);
  return 0;
}

struct desk_image_s *img_load(const char *path, int maxw, int maxh,
                              struct desk_win_s *owner, char *err,
                              int errlen)
{
  struct desk_image_s *img = NULL;
  struct dec_s d;
  uint8_t *rgb = NULL;
  uint32_t *acc = NULL;
  uint16_t *cnt = NULL;
  uint16_t *strip = NULL;
  int tw;
  int th;
  int blk = -1;                          /* The strip's first row / STRIP */
  int slo = 0;                           /* Rows of it filled */
  int shi = -1;
  int ty = -1;                           /* The output row accumulating */
  int nrows = 0;
  bool dither;
  int r;
  int x;

  if (dec_open(&d, path, err, errlen) < 0)
    {
      return NULL;
    }

  dither = !(d.type == T_BMP && d.bpp == 16);

  /* The size: into maxw x maxh (and the GPU's space) and IMG_MAXPIX, the
   * same shape
   */

  maxw = maxw > IMG_SPACE ? IMG_SPACE : maxw;
  maxh = maxh > IMG_SPACE ? IMG_SPACE : maxh;
  tw = d.w;
  th = d.h;
  if (tw > maxw)
    {
      th = (int)((int64_t)th * maxw / tw);
      tw = maxw;
    }

  if (th > maxh)
    {
      tw = (int)((int64_t)tw * maxh / th);
      th = maxh;
    }

  while ((int64_t)tw * th > IMG_MAXPIX)
    {
      tw = tw * 7 / 8;
      th = th * 7 / 8;
    }

  tw = tw < 1 ? 1 : tw;
  th = th < 1 ? 1 : th;

  img = img_make(&tw, &th, owner);
  rgb = malloc(d.w * 3);
  acc = calloc(tw * 3, sizeof(uint32_t));
  cnt = calloc(tw, sizeof(uint16_t));
  strip = malloc(tw * STRIP * 2);
  if (img == NULL || rgb == NULL || acc == NULL || cnt == NULL ||
      strip == NULL)
    {
      snprintf(err, errlen, "out of memory");
      goto fail;
    }

  img->fw = d.w;
  img->fh = d.h;
  fmt_name(&d, img->fmt, sizeof(img->fmt));

  /* Columns: source column c goes to output column c * tw / d.w */

  for (x = 0; x < d.w; x++)
    {
      cnt[(int)((int64_t)x * tw / d.w)]++;
    }

  /* Rows, in the file's order; image row sy goes to output row
   * sy * th / d.h: an output row is made when the next source row goes
   * elsewhere (or at the end)
   */

  for (r = 0; r < d.h; r++)
    {
      int sy = d.topdown ? r : d.h - 1 - r;
      int oy = (int)((int64_t)sy * th / d.h);
      int nx;

      if (dec_row(&d, rgb) < 0)
        {
          snprintf(err, errlen, "the file is cut short");
          goto fail;
        }

      if (oy != ty)
        {
          ty = oy;
          nrows = 0;
          memset(acc, 0, tw * 3 * sizeof(uint32_t));
        }

      if (tw == d.w)
        {
          for (x = 0; x < 3 * tw; x++)
            {
              acc[x] += rgb[x];
            }
        }
      else
        {
          for (x = 0; x < d.w; x++)
            {
              uint32_t *a = acc + 3 * (int)((int64_t)x * tw / d.w);

              a[0] += rgb[3 * x];
              a[1] += rgb[3 * x + 1];
              a[2] += rgb[3 * x + 2];
            }
        }

      nrows++;

      /* The last source row of this output row? */

      nx = d.topdown ? sy + 1 : sy - 1;
      if (nx >= 0 && nx < d.h && (int)((int64_t)nx * th / d.h) == ty)
        {
          continue;
        }

      /* Into the strip; a strip full (or another one) goes to the GPU */

      if (ty / STRIP != blk)
        {
          if (blk >= 0)
            {
              img_write(img, 0, blk * STRIP + slo, tw, shi - slo + 1,
                        strip + slo * tw, tw * 2);
            }

          blk = ty / STRIP;
          slo = STRIP;
          shi = -1;
        }

      for (x = 0; x < tw; x++)
        {
          uint32_t n = cnt[x] * nrows;
          uint32_t b = dither ? 2 * g_bayer[(ty & 3) * 4 + (x & 3)] + 1 : 16;
          uint32_t cr = (acc[3 * x] + n / 2) / n;
          uint32_t cg = (acc[3 * x + 1] + n / 2) / n;
          uint32_t cb = (acc[3 * x + 2] + n / 2) / n;

          /* To RGB565 with an ordered dither (no bands in gradients),
           * unless it came from RGB565
           */

          cr = (cr * 31 * 32 + b * 255) / (255 * 32);
          cg = (cg * 63 * 32 + b * 255) / (255 * 32);
          cb = (cb * 31 * 32 + b * 255) / (255 * 32);
          strip[(ty % STRIP) * tw + x] =
            (uint16_t)((cr > 31 ? 31 : cr) << 11 |
                       (cg > 63 ? 63 : cg) << 5 | (cb > 31 ? 31 : cb));
        }

      slo = ty % STRIP < slo ? ty % STRIP : slo;
      shi = ty % STRIP > shi ? ty % STRIP : shi;
    }

  if (blk >= 0)
    {
      img_write(img, 0, blk * STRIP + slo, tw, shi - slo + 1,
                strip + slo * tw, tw * 2);
    }

  free(rgb);
  free(acc);
  free(cnt);
  free(strip);
  dec_close(&d);
  return img;

fail:
  free(rgb);
  free(acc);
  free(cnt);
  free(strip);
  dec_close(&d);
  if (img != NULL)
    {
      img_free(img);
    }

  return NULL;
}

struct desk_image_s *img_new(int w, int h, uint16_t c,
                             struct desk_win_s *owner)
{
  struct desk_image_s *img = img_make(&w, &h, owner);
  struct desk_ops_s *o;

  if (img == NULL)
    {
      return NULL;
    }

  o = malloc(sizeof(*o));
  if (o != NULL)
    {
      ops_reset(o);
      ops_fill(o, 0, 0, w, h, c);
      ops_draw(img->hwnd, o);
      free(o);
    }

  return img;
}

void img_free(struct desk_image_s *img)
{
  if (img == NULL)
    {
      return;
    }

  img->owner = NULL;
  img_close(img);
  sem_destroy(&img->sem);
  free(img);
}

int img_read(struct desk_image_s *img, int x, int y, int w, int h,
             uint16_t *buf, int stride)
{
  struct nxgl_rect_s r;

  r.pt1.x = x;
  r.pt1.y = y;
  r.pt2.x = x + w - 1;
  r.pt2.y = y + h - 1;
  return nx_getrectangle(img->hwnd, &r, 0, (uint8_t *)buf, stride);
}

int img_write(struct desk_image_s *img, int x, int y, int w, int h,
              const uint16_t *buf, int stride)
{
  struct nxgl_rect_s r;
  struct nxgl_point_s org;
  const void *src[1];

  r.pt1.x = x;
  r.pt1.y = y;
  r.pt2.x = x + w - 1;
  r.pt2.y = y + h - 1;
  org     = r.pt1;
  src[0]  = buf;
  return nx_bitmap(img->hwnd, &r, src, &org, stride);
}

/* 24-bit BMP, bottom row first: read back a strip at a time from the
 * bottom up
 */

static void put32(uint8_t *p, uint32_t v)
{
  p[0] = v;
  p[1] = v >> 8;
  p[2] = v >> 16;
  p[3] = v >> 24;
}

int img_save_bmp(struct desk_image_s *img, const char *path)
{
  uint8_t hdr[54];
  int stride = (img->w * 3 + 3) & ~3;
  uint16_t *pix = malloc(img->w * STRIP * 2);
  uint8_t *row = calloc(1, stride);
  int ret = 0;
  int y;
  FILE *f;

  if (pix == NULL || row == NULL)
    {
      free(pix);
      free(row);
      return -ENOMEM;
    }

  f = fopen(path, "wb");
  if (f == NULL)
    {
      ret = -errno;
      free(pix);
      free(row);
      return ret;
    }

  memset(hdr, 0, sizeof(hdr));
  hdr[0] = 'B';
  hdr[1] = 'M';
  put32(hdr + 2, 54 + stride * img->h);
  put32(hdr + 10, 54);
  put32(hdr + 14, 40);
  put32(hdr + 18, img->w);
  put32(hdr + 22, img->h);
  hdr[26] = 1;
  hdr[28] = 24;
  put32(hdr + 34, stride * img->h);
  put32(hdr + 38, 2835);                 /* 72 dpi */
  put32(hdr + 42, 2835);
  if (fwrite(hdr, 1, 54, f) != 54)
    {
      ret = -EIO;
    }

  for (y = img->h; y > 0 && ret == 0; )
    {
      int n = y >= STRIP ? STRIP : y;
      int j;

      y -= n;
      if (img_read(img, 0, y, img->w, n, pix, img->w * 2) < 0)
        {
          ret = -EIO;
          break;
        }

      for (j = n - 1; j >= 0 && ret == 0; j--)
        {
          const uint16_t *p = pix + j * img->w;
          int x;

          for (x = 0; x < img->w; x++)
            {
              uint16_t c = p[x];
              uint8_t r5 = c >> 11;
              uint8_t g6 = (c >> 5) & 63;
              uint8_t b5 = c & 31;

              row[3 * x]     = (b5 << 3) | (b5 >> 2);
              row[3 * x + 1] = (g6 << 2) | (g6 >> 4);
              row[3 * x + 2] = (r5 << 3) | (r5 >> 2);
            }

          if (fwrite(row, 1, stride, f) != (size_t)stride)
            {
              ret = -EIO;
            }
        }
    }

  if (fclose(f) < 0 && ret == 0)
    {
      ret = -errno;
    }

  free(pix);
  free(row);
  return ret;
}

/* Drawing */

void img_copy(struct desk_ops_s *o, struct desk_image_s *img, int sx,
              int sy, int w, int h, int dx, int dy)
{
  struct nx_gpuop_s *op;

  if (w <= 0 || h <= 0)
    {
      return;
    }

  op = ops_add(o);
  op->op  = NX_GPUOP_COPY;
  op->src = img->hwnd;
  op->sx  = sx;
  op->sy  = sy;
  op->x   = dx;
  op->y   = dy;
  op->w   = w;
  op->h   = h;
}

void img_blend(struct desk_ops_s *o, struct desk_image_s *img, int sx,
               int sy, int w, int h, int dx, int dy, uint8_t alpha)
{
  struct nx_gpuop_s *op;

  if (w <= 0 || h <= 0)
    {
      return;
    }

  op = ops_add(o);
  op->op    = NX_GPUOP_BLEND;
  op->mode  = NX_BLEND_SRC_RGB | NX_BLEND_DITHER;
  op->alpha = alpha;
  op->src   = img->hwnd;
  op->sx    = sx;
  op->sy    = sy;
  op->x     = dx;
  op->y     = dy;
  op->w     = w;
  op->h     = h;
}

/* Enlarged z times at (dx, dy), cut to [cx0, cx1) x [cy0, cy1): the
 * source rows needed are first laid out, widened (a COPY a column and
 * copy of it), in the top rows of the visible rectangle; then, from the
 * last up, each is copied to the z rows it becomes (the lowest last: it
 * may be where it was laid out).  A row's copies land below where the
 * rows above it were laid out, so none is overwritten before it is used.
 */

void img_zoom(NXWINDOW hwnd, struct desk_ops_s *o, struct desk_image_s *img,
              int dx, int dy, int z, int cx0, int cy0, int cx1, int cy1)
{
  int vx0 = dx > cx0 ? dx : cx0;
  int vy0 = dy > cy0 ? dy : cy0;
  int vx1 = dx + img->w * z < cx1 ? dx + img->w * z : cx1;
  int vy1 = dy + img->h * z < cy1 ? dy + img->h * z : cy1;
  int i0;
  int i1;
  int j0;
  int j1;
  int i;
  int j;
  int k;

  if (vx0 >= vx1 || vy0 >= vy1)
    {
      return;
    }

  if (z <= 1)
    {
      img_copy(o, img, vx0 - dx, vy0 - dy, vx1 - vx0, vy1 - vy0, vx0, vy0);
      return;
    }

  i0 = (vx0 - dx) / z;
  i1 = (vx1 - 1 - dx) / z;
  j0 = (vy0 - dy) / z;
  j1 = (vy1 - 1 - dy) / z;

  for (i = i0; i <= i1; i++)
    {
      for (k = 0; k < z; k++)
        {
          int x = dx + i * z + k;

          if (x >= vx0 && x < vx1)
            {
              ui_flush(hwnd, o, 1);
              img_copy(o, img, i, j0, 1, j1 - j0 + 1, x, vy0);
            }
        }
    }

  for (j = j1; j >= j0; j--)
    {
      int s = vy0 + j - j0;

      for (k = z - 1; k >= 0; k--)
        {
          int y = dy + j * z + k;
          struct nx_gpuop_s *op;

          if (y < vy0 || y >= vy1 || y == s)
            {
              continue;
            }

          ui_flush(hwnd, o, 1);
          op = ops_add(o);
          op->op = NX_GPUOP_COPY;
          op->sx = vx0;
          op->sy = s;
          op->x  = vx0;
          op->y  = y;
          op->w  = vx1 - vx0;
          op->h  = 1;
        }
    }
}
