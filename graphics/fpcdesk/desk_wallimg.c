/****************************************************************************
 * apps/graphics/fpcdesk/desk_wallimg.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The wallpaper from an image (Settings, "Image"): a BMP from the home
 * directory (wallpaper.bmp, else the first .bmp there; 24 or 32 bits a
 * pixel, uncompressed), scaled by the CPU to cover the display (cropped
 * at its centre: an area average when it shrinks, bilinear when it
 * grows), encoded as BC1 (DXT1) four rows at a time, and put into an
 * image window of the NX server (NXBE_WINDOW_IMAGE): a W x H/4 surface
 * of blocks, a quarter of the RGB565 surface a W x H wallpaper takes.
 * The background is then a layer window in the desktop layer whose one
 * operation is a BLEND from the blocks (NX_BLEND_SRC_BC1): the GPU
 * decodes them wherever the display is composited.  A display mode
 * encodes it again at the new size.  The blocks are kept in the home
 * directory (.wallpaper.bc1, with the size they are for and the image's
 * size and time): the next boot puts them straight in.
 *
 * The encoder (bc1_block()) takes the principal axis of a block's colours
 * as the line its two colours lie on, their extremes along it (inset a
 * little) as the ends, the nearest of the four colours the GPU decodes for
 * each pixel, then refits the ends to those choices by least squares and
 * keeps whichever is better.
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <semaphore.h>
#include <time.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/stat.h>

#include <nuttx/nx/nxbe.h>

#include "desk.h"

#ifdef CONFIG_FPCDESK_WALLPAPER

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WALL_FILE        "wallpaper.bmp"
#define WALL_CACHE       ".wallpaper.bc1"
#define WALL_MAGIC       0x31434257    /* "WBC1" */
#define WALL_BANDROWS    16            /* Block rows put at once */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A BMP being read, a row at a time, as RGB888 */

struct bmp_s
{
  int fd;
  int w;
  int h;
  int bpp;                             /* 24 or 32 */
  bool topdown;
  uint32_t offset;                     /* Of the pixels in the file */
  uint32_t stride;                     /* Bytes a row in the file */
  FAR uint8_t *raw;                    /* A row as it is in the file */
  int nrows;                           /* Rows kept (RGB888), a ring */
  FAR uint8_t *rows;
  FAR int *rowy;                       /* Which row each slot holds */
};

/* The blocks kept in a file: this, then the rows of blocks */

struct wall_cache_s
{
  uint32_t magic;
  uint16_t w;                          /* The display's */
  uint16_t h;
  uint32_t size;                       /* The image file's */
  uint32_t mtime;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct
{
  NXWINDOW image;                      /* The blocks (hidden) */
  NXWINDOW layer;                      /* The background drawn from them */
  int w;                               /* The size they are for */
  int h;
  volatile bool lost;                  /* The server dropped the blocks */
  volatile bool made;                  /* ... not the requests of making */
  sem_t synched;
  bool init;
} g_wi;

/****************************************************************************
 * Private Functions: the file
 ****************************************************************************/

static uint32_t le32(FAR const uint8_t *p)
{
  return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 |
         (uint32_t)p[3] << 24;
}

static uint16_t le16(FAR const uint8_t *p)
{
  return p[0] | (uint16_t)p[1] << 8;
}

/* The image file: wallpaper.bmp in the home directory, else the first
 * .bmp there
 */

static bool wall_path(FAR char *buf, size_t len)
{
  FAR struct dirent *d;
  FAR const char *home;
  FAR DIR *dir;
  bool found = false;

  home = getenv("HOME");
  if (home == NULL)
    {
      home = CONFIG_LIBC_HOMEDIR;
    }

  snprintf(buf, len, "%s%s%s", home,
           home[strlen(home) - 1] == '/' ? "" : "/", WALL_FILE);
  if (access(buf, R_OK) == 0)
    {
      return true;
    }

  dir = opendir(home);
  if (dir == NULL)
    {
      return false;
    }

  while ((d = readdir(dir)) != NULL)
    {
      size_t n = strlen(d->d_name);

      if (n > 4 && strcasecmp(d->d_name + n - 4, ".bmp") == 0)
        {
          snprintf(buf, len, "%s%s%s", home,
                   home[strlen(home) - 1] == '/' ? "" : "/", d->d_name);
          found = true;
          break;
        }
    }

  closedir(dir);
  return found;
}

/* The file the blocks are kept in */

static void wall_cache_path(FAR char *buf, size_t len)
{
  FAR const char *home = getenv("HOME");

  if (home == NULL)
    {
      home = CONFIG_LIBC_HOMEDIR;
    }

  snprintf(buf, len, "%s%s%s", home,
           home[strlen(home) - 1] == '/' ? "" : "/", WALL_CACHE);
}

static int bmp_open(FAR struct bmp_s *b, FAR const char *path)
{
  uint8_t hdr[54];
  int32_t h;

  memset(b, 0, sizeof(*b));
  b->fd = open(path, O_RDONLY);
  if (b->fd < 0)
    {
      return -errno;
    }

  if (read(b->fd, hdr, sizeof(hdr)) != sizeof(hdr) || hdr[0] != 'B' ||
      hdr[1] != 'M' || le32(hdr + 14) < 40)
    {
      close(b->fd);
      return -EINVAL;
    }

  b->offset  = le32(hdr + 10);
  b->w       = (int32_t)le32(hdr + 18);
  h          = (int32_t)le32(hdr + 22);
  b->bpp     = le16(hdr + 28);
  b->topdown = h < 0;
  b->h       = h < 0 ? -h : h;

  /* Uncompressed (or, at 32 bits, bit fields: taken as BGRA) */

  if ((b->bpp != 24 && b->bpp != 32) || b->w <= 0 || b->h <= 0 ||
      b->w > 16384 || b->h > 16384 ||
      (le32(hdr + 30) != 0 && !(b->bpp == 32 && le32(hdr + 30) == 3)))
    {
      close(b->fd);
      return -ENOTSUP;
    }

  b->stride = ((uint32_t)b->w * (b->bpp / 8) + 3) & ~3u;
  return OK;
}

static void bmp_close(FAR struct bmp_s *b)
{
  free(b->raw);
  free(b->rows);
  free(b->rowy);
  close(b->fd);
}

/* Room for n rows at a time */

static int bmp_keep(FAR struct bmp_s *b, int n)
{
  int i;

  b->nrows = n;
  b->raw   = malloc(b->stride);
  b->rows  = malloc((size_t)n * b->w * 3);
  b->rowy  = malloc(n * sizeof(int));
  if (b->raw == NULL || b->rows == NULL || b->rowy == NULL)
    {
      return -ENOMEM;
    }

  for (i = 0; i < n; i++)
    {
      b->rowy[i] = -1;
    }

  return OK;
}

/* Row y (from the top) as RGB888 */

static FAR const uint8_t *bmp_row(FAR struct bmp_s *b, int y)
{
  FAR uint8_t *row;
  int slot;
  int fy;
  int x;

  y    = y < 0 ? 0 : y >= b->h ? b->h - 1 : y;
  slot = y % b->nrows;
  row  = b->rows + (size_t)slot * b->w * 3;
  if (b->rowy[slot] == y)
    {
      return row;
    }

  fy = b->topdown ? y : b->h - 1 - y;
  if (lseek(b->fd, b->offset + (off_t)fy * b->stride, SEEK_SET) < 0 ||
      read(b->fd, b->raw, b->stride) != (ssize_t)b->stride)
    {
      memset(b->raw, 0, b->stride);
    }

  for (x = 0; x < b->w; x++)
    {
      FAR const uint8_t *p = b->raw + x * (b->bpp / 8);

      row[3 * x]     = p[2];
      row[3 * x + 1] = p[1];
      row[3 * x + 2] = p[0];
    }

  b->rowy[slot] = y;
  return row;
}

/****************************************************************************
 * Private Functions: BC1
 ****************************************************************************/

/* RGB565 from 8-bit channels, rounded; and back, as the GPU widens */

static uint16_t to565(int r, int g, int b)
{
  r = r < 0 ? 0 : r > 255 ? 255 : r;
  g = g < 0 ? 0 : g > 255 ? 255 : g;
  b = b < 0 ? 0 : b > 255 ? 255 : b;
  return (uint16_t)(((r * 31 + 127) / 255) << 11 |
                    ((g * 63 + 127) / 255) << 5 | ((b * 31 + 127) / 255));
}

static void from565(uint16_t c, FAR int *rgb)
{
  int r = c >> 11;
  int g = (c >> 5) & 63;
  int b = c & 31;

  rgb[0] = r << 3 | r >> 2;
  rgb[1] = g << 2 | g >> 4;
  rgb[2] = b << 3 | b >> 2;
}

/* The four colours the GPU decodes from c0 > c1 (docs/gpu.md: the mixes
 * in 5 and 6-bit channels, rounded to nearest), widened to 8 bits
 */

static void bc1_palette(uint16_t c0, uint16_t c1, int pal[4][3])
{
  int a[3];
  int b[3];
  int i;

  a[0] = c0 >> 11;
  a[1] = (c0 >> 5) & 63;
  a[2] = c0 & 31;
  b[0] = c1 >> 11;
  b[1] = (c1 >> 5) & 63;
  b[2] = c1 & 31;
  from565(c0, pal[0]);
  from565(c1, pal[1]);
  for (i = 0; i < 3; i++)
    {
      int m2 = (2 * a[i] + b[i] + 1) / 3;
      int m3 = (a[i] + 2 * b[i] + 1) / 3;

      pal[2][i] = i == 1 ? (m2 << 2 | m2 >> 4) : (m2 << 3 | m2 >> 2);
      pal[3][i] = i == 1 ? (m3 << 2 | m3 >> 4) : (m3 << 3 | m3 >> 2);
    }
}

/* Each pixel's nearest colour; the total squared error */

static uint32_t bc1_indices(FAR const uint8_t px[16][3], uint16_t c0,
                            uint16_t c1, FAR uint32_t *idx)
{
  int pal[4][3];
  uint32_t err = 0;
  int i;
  int k;

  bc1_palette(c0, c1, pal);
  *idx = 0;
  for (i = 0; i < 16; i++)
    {
      uint32_t best = UINT32_MAX;
      int bi = 0;

      for (k = 0; k < 4; k++)
        {
          int dr = px[i][0] - pal[k][0];
          int dg = px[i][1] - pal[k][1];
          int db = px[i][2] - pal[k][2];
          uint32_t e = 2 * dr * dr + 4 * dg * dg + db * db;

          if (e < best)
            {
              best = e;
              bi   = k;
            }
        }

      *idx |= (uint32_t)bi << (2 * i);
      err  += best;
    }

  return err;
}

/* c0 > c1 (four colours): swapped if need be, with the indices */

static void bc1_order(FAR uint16_t *c0, FAR uint16_t *c1,
                      FAR uint32_t *idx)
{
  if (*c0 < *c1)
    {
      uint16_t t = *c0;

      *c0  = *c1;
      *c1  = t;
      *idx ^= 0x55555555;                  /* 0 <-> 1, 2 <-> 3 */
    }
}

/* One block: 16 pixels (RGB888, row by row) into c0, c1, the indices */

static void bc1_block(FAR const uint8_t px[16][3], FAR uint16_t *out)
{
  static const int wts[4] =
  {
    3, 0, 2, 1                             /* Index -> thirds of c0 */
  };

  int mean[3] = { 0, 0, 0 };
  float cov[6] = { 0 };
  float axis[3];
  float lo = 1e9f;
  float hi = -1e9f;
  float ends[2][3];
  uint16_t c0;
  uint16_t c1;
  uint32_t idx = 0;
  uint32_t err;
  int i;
  int k;

  for (i = 0; i < 16; i++)
    {
      for (k = 0; k < 3; k++)
        {
          mean[k] += px[i][k];
        }
    }

  for (k = 0; k < 3; k++)
    {
      mean[k] = (mean[k] + 8) / 16;
    }

  for (i = 0; i < 16; i++)
    {
      float r = px[i][0] - mean[0];
      float g = px[i][1] - mean[1];
      float b = px[i][2] - mean[2];

      cov[0] += r * r;
      cov[1] += r * g;
      cov[2] += r * b;
      cov[3] += g * g;
      cov[4] += g * b;
      cov[5] += b * b;
    }

  /* One colour: both ends the same (index 0 everywhere) */

  if (cov[0] + cov[3] + cov[5] < 1.0f)
    {
      c0     = to565(mean[0], mean[1], mean[2]);
      out[0] = c0;
      out[1] = c0;
      out[2] = 0;
      out[3] = 0;
      return;
    }

  /* The principal axis, by a few power iterations */

  axis[0] = cov[0] + cov[1] + cov[2];
  axis[1] = cov[1] + cov[3] + cov[4];
  axis[2] = cov[2] + cov[4] + cov[5];
  for (i = 0; i < 6; i++)
    {
      float x = cov[0] * axis[0] + cov[1] * axis[1] + cov[2] * axis[2];
      float y = cov[1] * axis[0] + cov[3] * axis[1] + cov[4] * axis[2];
      float z = cov[2] * axis[0] + cov[4] * axis[1] + cov[5] * axis[2];
      float big;

      /* Kept in range without a square root: scaled by its largest */

      big = x < 0 ? -x : x;
      big = (y < 0 ? -y : y) > big ? (y < 0 ? -y : y) : big;
      big = (z < 0 ? -z : z) > big ? (z < 0 ? -z : z) : big;
      if (big < 1e-12f)
        {
          break;
        }

      axis[0] = x / big;
      axis[1] = y / big;
      axis[2] = z / big;
    }

  {
    float n2 = axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2];

    for (i = 0; i < 16; i++)
      {
        float t = ((px[i][0] - mean[0]) * axis[0] +
                   (px[i][1] - mean[1]) * axis[1] +
                   (px[i][2] - mean[2]) * axis[2]) / n2;

        lo = t < lo ? t : lo;
        hi = t > hi ? t : hi;
      }
  }

  /* The ends: the extremes, inset by a sixteenth of the range (the
   * pixels near the ends are better served by the mixes)
   */

  {
    float inset = (hi - lo) / 16.0f;

    lo += inset;
    hi -= inset;
  }

  for (k = 0; k < 3; k++)
    {
      ends[0][k] = mean[k] + hi * axis[k];
      ends[1][k] = mean[k] + lo * axis[k];
    }

  c0 = to565((int)(ends[0][0] + 0.5f), (int)(ends[0][1] + 0.5f),
             (int)(ends[0][2] + 0.5f));
  c1 = to565((int)(ends[1][0] + 0.5f), (int)(ends[1][1] + 0.5f),
             (int)(ends[1][2] + 0.5f));
  if (c0 == c1)
    {
      c1 = c1 & 31 ? c1 - 1 : c1 + 1;      /* Four colours still */
    }

  bc1_order(&c0, &c1, &idx);
  err = bc1_indices(px, c0, c1, &idx);

  /* Refit the ends to the choices: least squares for x0 and x1 in
   * p = (w x0 + (3 - w) x1) / 3
   */

  {
    float aa = 0;
    float bb = 0;
    float ab = 0;
    float ax[3] = { 0, 0, 0 };
    float bx[3] = { 0, 0, 0 };
    float det;

    for (i = 0; i < 16; i++)
      {
        float a = wts[(idx >> (2 * i)) & 3] / 3.0f;
        float b = 1.0f - a;

        aa += a * a;
        bb += b * b;
        ab += a * b;
        for (k = 0; k < 3; k++)
          {
            ax[k] += a * px[i][k];
            bx[k] += b * px[i][k];
          }
      }

    det = aa * bb - ab * ab;
    if (det > 1e-6f || det < -1e-6f)
      {
        uint16_t n0;
        uint16_t n1;
        uint32_t nidx = 0;
        uint32_t nerr;
        int e0[3];
        int e1[3];

        for (k = 0; k < 3; k++)
          {
            e0[k] = (int)((ax[k] * bb - bx[k] * ab) / det + 0.5f);
            e1[k] = (int)((bx[k] * aa - ax[k] * ab) / det + 0.5f);
          }

        n0 = to565(e0[0], e0[1], e0[2]);
        n1 = to565(e1[0], e1[1], e1[2]);
        if (n0 != n1)
          {
            bc1_order(&n0, &n1, &nidx);
            nerr = bc1_indices(px, n0, n1, &nidx);
            if (nerr < err)
              {
                c0  = n0;
                c1  = n1;
                idx = nidx;
              }
          }
      }
  }

  out[0] = c0;
  out[1] = c1;
  out[2] = (uint16_t)idx;
  out[3] = (uint16_t)(idx >> 16);
}

/****************************************************************************
 * Private Functions: scaling and encoding
 ****************************************************************************/

/* The image's pixels (cx0, cy0) .. + (cw, ch) (16.16), which cover the
 * display, into W x H: each output pixel from its footprint in the image
 * (an average, or bilinear if it is under a pixel)
 */

struct scale_s
{
  int32_t cx0;                         /* 16.16 */
  int32_t cy0;
  int32_t sx;                          /* Image pixels an output pixel */
  int32_t sy;
};

static void scale_px(FAR struct bmp_s *b, FAR const struct scale_s *s,
                     int x, int y, FAR uint8_t *out)
{
  int32_t fx0 = s->cx0 + x * s->sx;
  int32_t fy0 = s->cy0 + y * s->sy;
  uint32_t acc[3] = { 0, 0, 0 };
  int n = 0;
  int k;

  if (s->sx >= 65536 && s->sy >= 65536)
    {
      /* Shrinking: the average of the pixels whose centres are in the
       * footprint
       */

      int x0 = (fx0 + 32767) >> 16;
      int x1 = (fx0 + s->sx + 32767) >> 16;
      int y0 = (fy0 + 32767) >> 16;
      int y1 = (fy0 + s->sy + 32767) >> 16;
      int i;
      int j;

      x1 = x1 > x0 ? x1 : x0 + 1;
      y1 = y1 > y0 ? y1 : y0 + 1;
      for (j = y0; j < y1; j++)
        {
          FAR const uint8_t *row = bmp_row(b, j);

          for (i = x0; i < x1; i++)
            {
              int xi = i < 0 ? 0 : i >= b->w ? b->w - 1 : i;

              acc[0] += row[3 * xi];
              acc[1] += row[3 * xi + 1];
              acc[2] += row[3 * xi + 2];
              n++;
            }
        }

      for (k = 0; k < 3; k++)
        {
          out[k] = (uint8_t)((acc[k] + n / 2) / n);
        }
    }
  else
    {
      /* Growing: bilinear at the footprint's centre */

      int32_t cx = fx0 + s->sx / 2 - 32768;
      int32_t cy = fy0 + s->sy / 2 - 32768;
      int xi = cx >> 16;
      int yi = cy >> 16;
      uint32_t fx = (cx >> 8) & 255;
      uint32_t fy = (cy >> 8) & 255;
      FAR const uint8_t *r0 = bmp_row(b, yi);
      FAR const uint8_t *r1 = bmp_row(b, yi + 1);
      int xa = xi < 0 ? 0 : xi >= b->w ? b->w - 1 : xi;
      int xb = xi + 1 < 0 ? 0 : xi + 1 >= b->w ? b->w - 1 : xi + 1;

      for (k = 0; k < 3; k++)
        {
          uint32_t top = r0[3 * xa + k] * (256 - fx) + r0[3 * xb + k] * fx;
          uint32_t bot = r1[3 * xa + k] * (256 - fx) + r1[3 * xb + k] * fx;

          out[k] = (uint8_t)((top * (256 - fy) + bot * fy + 32768) >> 16);
        }
    }
}

/* Encode the image scaled to w x h and put its blocks into the image
 * window, WALL_BANDROWS rows of blocks at a time
 */

static int wall_encode(FAR struct bmp_s *b, int w, int h,
                       FAR int *cachefd)
{
  struct scale_s s;
  struct nxgl_rect_s dest;
  struct nxgl_point_s origin;
  FAR const void *src[1];
  FAR uint16_t *blocks;
  FAR uint8_t *band;
  uint8_t px[16][3];
  int64_t cw;
  int64_t ch;
  int by;
  int bx;
  int i;
  int ret;

  /* Cover: the image's largest centred part with the display's shape */

  if ((int64_t)b->w * h > (int64_t)b->h * w)
    {
      ch = (int64_t)b->h << 16;
      cw = ch * w / h;
    }
  else
    {
      cw = (int64_t)b->w << 16;
      ch = cw * h / w;
    }

  s.cx0 = (int32_t)((((int64_t)b->w << 16) - cw) / 2);
  s.cy0 = (int32_t)((((int64_t)b->h << 16) - ch) / 2);
  s.sx  = (int32_t)(cw / w);
  s.sy  = (int32_t)(ch / h);

  /* The rows one band of 4 output rows reads, and a few more */

  ret = bmp_keep(b, (int)((4 * (int64_t)s.sy) >> 16) + 4);
  if (ret < 0)
    {
      return ret;
    }

  band   = malloc((size_t)w * 4 * 3);
  blocks = malloc((size_t)w * WALL_BANDROWS * 2);
  if (band == NULL || blocks == NULL)
    {
      free(band);
      free(blocks);
      return -ENOMEM;
    }

  for (by = 0; by < h / 4; by++)
    {
      int y;
      int x;

      for (y = 0; y < 4; y++)
        {
          for (x = 0; x < w; x++)
            {
              scale_px(b, &s, x, 4 * by + y, band + (y * w + x) * 3);
            }
        }

      /* A row of blocks: 4 pixels' worth (8 bytes) each */

      for (bx = 0; bx < w / 4; bx++)
        {
          for (i = 0; i < 16; i++)
            {
              memcpy(px[i], band + ((i / 4) * w + bx * 4 + i % 4) * 3, 3);
            }

          bc1_block(px, blocks + (by % WALL_BANDROWS) * w + bx * 4);
        }

      if (by % WALL_BANDROWS == WALL_BANDROWS - 1 || by == h / 4 - 1)
        {
          int y0 = by - by % WALL_BANDROWS;

          dest.pt1.x = 0;
          dest.pt1.y = y0;
          dest.pt2.x = w - 1;
          dest.pt2.y = by;
          origin.x   = 0;
          origin.y   = y0;
          src[0]     = blocks;
          nx_bitmap(g_wi.image, &dest, src, &origin, w * 2);
          if (*cachefd >= 0)
            {
              size_t n = (size_t)w * 2 * (by - y0 + 1);

              if (write(*cachefd, blocks, n) != (ssize_t)n)
                {
                  close(*cachefd);
                  *cachefd = -2;             /* Not kept */
                }
            }
        }
    }

  free(band);
  free(blocks);
  return OK;
}

/* The blocks kept from last time, if they are for this size and this
 * image: put into the image window.  False if not.
 */

static bool wall_cached(FAR const struct wall_cache_s *want, int w, int h)
{
  struct wall_cache_s hdr;
  struct nxgl_rect_s dest;
  struct nxgl_point_s origin;
  FAR const void *src[1];
  FAR uint16_t *blocks;
  char path[96];
  bool ok = true;
  int fd;
  int y;

  wall_cache_path(path, sizeof(path));
  fd = open(path, O_RDONLY);
  if (fd < 0)
    {
      return false;
    }

  blocks = malloc((size_t)w * WALL_BANDROWS * 2);
  if (blocks == NULL ||
      read(fd, &hdr, sizeof(hdr)) != sizeof(hdr) ||
      memcmp(&hdr, want, sizeof(hdr)) != 0)
    {
      free(blocks);
      close(fd);
      return false;
    }

  for (y = 0; y < h / 4 && ok; y += WALL_BANDROWS)
    {
      int rows = h / 4 - y < WALL_BANDROWS ? h / 4 - y : WALL_BANDROWS;
      size_t n = (size_t)w * 2 * rows;

      ok = read(fd, blocks, n) == (ssize_t)n;
      if (ok)
        {
          dest.pt1.x = 0;
          dest.pt1.y = y;
          dest.pt2.x = w - 1;
          dest.pt2.y = y + rows - 1;
          origin.x   = 0;
          origin.y   = y;
          src[0]     = blocks;
          nx_bitmap(g_wi.image, &dest, src, &origin, w * 2);
        }
    }

  free(blocks);
  close(fd);
  return ok;
}

/* The image window lost its pixels (it had to move and found no room):
 * encoded again at the next tick
 */

static void wall_image_redraw(NXWINDOW hwnd,
                              FAR const struct nxgl_rect_s *rect,
                              bool more, FAR void *arg)
{
  /* Not the new territory of the window being made */

  if (g_wi.made)
    {
      g_wi.lost = true;
    }
}

static void wall_image_event(NXWINDOW hwnd, enum nx_event_e event,
                             FAR void *arg1, FAR void *arg2)
{
  if (event == NXEVENT_SYNCHED)
    {
      sem_post(&g_wi.synched);
    }
}

static const struct nx_callback_s g_wall_imagecb =
{
  wall_image_redraw, NULL, NULL, NULL, wall_image_event
};

/* Wait until the listener has had all the server said of the image
 * window so far
 */

static void wall_image_sync(void)
{
  struct timespec ts;

  if (nx_synch(g_wi.image, NULL) < 0)
    {
      return;
    }

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 1;
  while (sem_timedwait(&g_wi.synched, &ts) < 0 && errno == EINTR)
    {
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: desk_wallimg_available
 *
 * Description:
 *   Whether there is an image to show and a GPU to show it (BC1).
 *
 ****************************************************************************/

bool desk_wallimg_available(void)
{
  char path[96];

  return (nx_compositor(g_desk.hnx) & NX_CAP_BC1) != 0 &&
         wall_path(path, sizeof(path));
}

/****************************************************************************
 * Name: desk_wallimg_show, desk_wallimg_hide
 *
 * Description:
 *   Show the image as the wallpaper at the display's size (encoding it
 *   again if the size has changed); 0, or a negated errno value (no
 *   image, not a BMP it reads, no BC1).  Take it away.
 *
 ****************************************************************************/

int desk_wallimg_show(void)
{
  struct nxgl_point_s pos;
  struct nxgl_size_s size;
  struct nx_effects_s fx;
  struct nx_gpuop_s op;
  struct wall_cache_s want;
  struct stat st;
  struct bmp_s b;
  char path[96];
  int w = DESK_W & ~3;
  int h = DESK_H & ~3;
  int cachefd;
  int ret;

  if (g_wi.layer != NULL && g_wi.w == w && g_wi.h == h && !g_wi.lost)
    {
      return OK;
    }

  if ((nx_compositor(g_desk.hnx) & NX_CAP_BC1) == 0)
    {
      return -ENOSYS;
    }

  if (!wall_path(path, sizeof(path)) || stat(path, &st) < 0)
    {
      return -ENOENT;
    }

  memset(&want, 0, sizeof(want));
  want.magic = WALL_MAGIC;
  want.w     = w;
  want.h     = h;
  want.size  = (uint32_t)st.st_size;
  want.mtime = (uint32_t)st.st_mtime;

  ret = bmp_open(&b, path);
  if (ret < 0)
    {
      fprintf(stderr, "desk: %s: not a BMP I can read (%d)\n", path, ret);
      return ret;
    }

  desk_wallimg_hide();
  if (!g_wi.init)
    {
      sem_init(&g_wi.synched, 0, 0);
      g_wi.init = true;
    }

  /* The blocks: W x H/4 pixels of them, hidden */

  g_wi.made = false;
  g_wi.lost = false;

  g_wi.image = nx_openwindow(g_desk.hnx, NXBE_WINDOW_RAMBACKED |
                             NXBE_WINDOW_HIDDEN | NXBE_WINDOW_IMAGE,
                             &g_wall_imagecb, NULL);
  if (g_wi.image == NULL)
    {
      bmp_close(&b);
      return -ENOMEM;
    }

  pos.x  = 0;
  pos.y  = 0;
  size.w = w;
  size.h = h / 4;
  nx_setsize(g_wi.image, &size);
  nx_setposition(g_wi.image, &pos);
  wall_image_sync();
  g_wi.made = true;

  /* The blocks kept from last time, else encoded (and kept) */

  if (!wall_cached(&want, w, h))
    {
      wall_cache_path(path, sizeof(path));
      cachefd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
      if (cachefd >= 0 &&
          write(cachefd, &want, sizeof(want)) != sizeof(want))
        {
          close(cachefd);
          cachefd = -1;
        }

      ret = wall_encode(&b, w, h, &cachefd);
      if (cachefd >= 0)
        {
          close(cachefd);
        }

      if (ret < 0 || cachefd == -2)
        {
          unlink(path);
        }

      if (ret < 0)
        {
          bmp_close(&b);
          desk_wallimg_hide();
          return ret;
        }
    }

  bmp_close(&b);

  /* The background: a layer in the desktop layer, one BLEND */

  g_wi.layer = nx_openwindow(g_desk.hnx, NXBE_WINDOW_LAYER |
                             NXBE_WINDOW_HIDDEN, &g_desk_partcb,
                             (FAR void *)(uintptr_t)PART_WALLPAPER);
  if (g_wi.layer == NULL)
    {
      desk_wallimg_hide();
      return -ENOMEM;
    }

  size.w = DESK_W;
  size.h = DESK_H;
  nx_setsize(g_wi.layer, &size);
  nx_setposition(g_wi.layer, &pos);
  memset(&fx, 0, sizeof(fx));
  fx.layer   = NX_LAYER_DESKTOP;
  fx.opacity = 255;
  nx_seteffects(g_wi.layer, &fx);

  memset(&op, 0, sizeof(op));
  op.op    = NX_GPUOP_BLEND;
  op.w     = w;
  op.h     = h;
  op.src   = g_wi.image;
  op.alpha = 255;
  op.mode  = NX_BLEND_SRC_BC1;
  nx_setdisplaylist(g_wi.layer, &op, 1);
  nx_setvisibility(g_wi.layer, false);

  g_wi.w = w;
  g_wi.h = h;
  return OK;
}

void desk_wallimg_hide(void)
{
  if (g_wi.layer != NULL)
    {
      nx_closewindow(g_wi.layer);
      g_wi.layer = NULL;
    }

  if (g_wi.image != NULL)
    {
      nx_closewindow(g_wi.image);
      g_wi.image = NULL;
    }

  g_wi.w = 0;
  g_wi.h = 0;
}

/****************************************************************************
 * Name: desk_wallimg_preview
 *
 * Description:
 *   A w x h preview of the image shown (its centre, at its size): an
 *   operation into o.  False if it is not shown.
 *
 ****************************************************************************/

bool desk_wallimg_preview(FAR struct desk_ops_s *o, int x, int y, int w,
                          int h)
{
  FAR struct nx_gpuop_s *op;

  if (g_wi.layer == NULL || g_wi.lost || w > g_wi.w || h > g_wi.h)
    {
      return false;
    }

  op = ops_add(o);
  op->op    = NX_GPUOP_BLEND;
  op->x     = x;
  op->y     = y;
  op->w     = w;
  op->h     = h;
  op->src   = g_wi.image;
  op->sx    = (g_wi.w - w) / 2;
  op->sy    = (g_wi.h - h) / 2;
  op->alpha = 255;
  op->mode  = NX_BLEND_SRC_BC1;
  return true;
}

/****************************************************************************
 * Name: desk_wallimg_tick
 *
 * Description:
 *   Once a second: encode the image again if the server dropped it.
 *
 ****************************************************************************/

void desk_wallimg_tick(void)
{
  if (g_wi.lost && g_wi.layer != NULL)
    {
      desk_wallimg_show();
    }
}

#endif /* CONFIG_FPCDESK_WALLPAPER */
