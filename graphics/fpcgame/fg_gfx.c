/****************************************************************************
 * apps/graphics/fpcgame/fg_gfx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The game SDK's drawing: images in the GPU's memory, sprites, rectangles,
 * tile layers and text, into the canvas being drawn (fg_core.c), clipped
 * to it: the GPU only clips to its space, and the game's neighbours there
 * are its own images and the desktop's windows.
 *
 * Sprites: plain ones (any size) are a BLEND from the image, keyed on
 * FG_KEY; flipped, scaled and rotated ones the GPU's SPRITE (fg_affine():
 * an affine map from the box to the cell, cells of more than 64 x 32 drawn
 * in pieces, each over the same box with the map moved by its offset, so
 * that together they are exact).
 *
 * Layers: the map drawn in a surface of its own (fpc_gpu_game_alloc()),
 * tiles copied in from the tileset (SPRITE for flipped ones) as they
 * change; shown with one to four keyed BLENDs, wherever the scroll puts
 * the map's wrap.  Text: a layer of its own whose pixels are alpha (0 or
 * 255, A8: EXPANDs from the font), shown as BLENDs of the colour through
 * it, the rows that hold text only.
 ****************************************************************************/

#include "fg_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpc_gpu.h"
#include "fg_font8x8.h"

/****************************************************************************
 * Images
 ****************************************************************************/

static struct fg_rect_s g_img[FG_IMAGES];        /* w == 0: free */

FAR const struct fg_rect_s *fg_image_rect(int img)
{
  if (img < 0 || img >= FG_IMAGES || g_img[img].w == 0)
    {
      return NULL;
    }

  return &g_img[img];
}

int fg_image_new(int w, int h)
{
  int x;
  int y;
  int i;

  if (w <= 0 || h <= 0 || w > FPC_GPU_SPACE_W || h > FPC_GPU_SPACE_H)
    {
      return -EINVAL;
    }

  for (i = 0; i < FG_IMAGES && g_img[i].w != 0; i++);
  if (i == FG_IMAGES || fpc_gpu_game_alloc(w, h, &x, &y) < 0)
    {
      return -ENOMEM;
    }

  g_img[i].x = x;
  g_img[i].y = y;
  g_img[i].w = w;
  g_img[i].h = h;
  return i;
}

void fg_image_free(int img)
{
  FAR const struct fg_rect_s *r = fg_image_rect(img);

  if (r != NULL)
    {
      fpc_gpu_game_free(r->x, r->y);
      g_img[img].w = 0;
    }
}

int fg_image_width(int img)
{
  FAR const struct fg_rect_s *r = fg_image_rect(img);
  return r != NULL ? r->w : 0;
}

int fg_image_height(int img)
{
  FAR const struct fg_rect_s *r = fg_image_rect(img);
  return r != NULL ? r->h : 0;
}

void fg_image_put(int img, int x, int y, int w, int h,
                  FAR const uint16_t *px, size_t stride)
{
  FAR const struct fg_rect_s *r = fg_image_rect(img);

  if (r == NULL || x < 0 || y < 0 || w <= 0 || h <= 0 ||
      x + w > r->w || y + h > r->h)
    {
      return;
    }

  fpc_gpu_put(r->x + x, r->y + y, w, h, px, stride);
}

/* 8-bit pixels through a palette, a band of rows at a time */

void fg_image_put8(int img, int x, int y, int w, int h,
                   FAR const uint8_t *px, size_t stride,
                   FAR const uint16_t *pal565)
{
  FAR const struct fg_rect_s *r = fg_image_rect(img);
  uint16_t pal[256];
  FAR uint16_t *band;
  int rows;
  int j;
  int i;

  if (r == NULL || x < 0 || y < 0 || w <= 0 || h <= 0 ||
      x + w > r->w || y + h > r->h)
    {
      return;
    }

  for (i = 0; i < 256; i++)
    {
      pal[i] = pal565[i] == FG_KEY ? FG_KEY ^ 0x0020 : pal565[i];
    }

  pal[0] = FG_KEY;
  rows = 4096 / w > 0 ? 4096 / w : 1;
  band = malloc((size_t)w * rows * 2);
  if (band == NULL)
    {
      return;
    }

  for (j = 0; j < h; j += rows)
    {
      int n = h - j < rows ? h - j : rows;
      int k;

      for (k = 0; k < n; k++)
        {
          FAR const uint8_t *s = px + (size_t)(j + k) * stride;
          FAR uint16_t *d = band + (size_t)k * w;

          for (i = 0; i < w; i++)
            {
              d[i] = pal[s[i]];
            }
        }

      fpc_gpu_put(r->x + x, r->y + y + j, w, n, band, (size_t)w * 2);
    }

  free(band);
}

int fg_image_from8(int w, int h, FAR const uint8_t *px, size_t stride,
                   FAR const uint16_t *pal565)
{
  int img = fg_image_new(w, h);

  if (img >= 0)
    {
      fg_image_put8(img, 0, 0, w, h, px, stride, pal565);
    }

  return img;
}

/****************************************************************************
 * Rectangles and sprites
 ****************************************************************************/

/* Clip x, y, w, h to the canvas; false if nothing is left */

static bool clip(FAR int *x, FAR int *y, FAR int *w, FAR int *h)
{
  if (*x < 0)
    {
      *w += *x;
      *x = 0;
    }

  if (*y < 0)
    {
      *h += *y;
      *y = 0;
    }

  if (*x + *w > g_fg.w)
    {
      *w = g_fg.w - *x;
    }

  if (*y + *h > g_fg.h)
    {
      *h = g_fg.h - *y;
    }

  return *w > 0 && *h > 0;
}

void fg_clear(uint16_t colour)
{
  fpc_gpu_fill(g_fg.ox, g_fg.oy, g_fg.w, g_fg.h, colour);
}

void fg_rect(int x, int y, int w, int h, uint16_t colour)
{
  if (clip(&x, &y, &w, &h))
    {
      fpc_gpu_fill(g_fg.ox + x, g_fg.oy + y, w, h, colour);
    }
}

static uint8_t flags_alpha(uint32_t flags)
{
  return (flags & FG_HALF) ? 128 : (flags & 0xff00) ? (flags >> 8) & 0xff : 255;
}

void fg_affine(int cx, int cy, int cw, int ch, int bx, int by, int bw,
               int bh, int a, int b, int c, int d, int32_t u0, int32_t v0,
               uint32_t flags)
{
  struct fpc_gpu_sprite_s s;
  int ox;
  int oy;

  /* The box cut to the canvas, the map moved with it */

  if (bx < 0)
    {
      u0 -= a * bx;
      v0 -= c * bx;
      bw += bx;
      bx = 0;
    }

  if (by < 0)
    {
      u0 -= b * by;
      v0 -= d * by;
      bh += by;
      by = 0;
    }

  if (bx + bw > g_fg.w)
    {
      bw = g_fg.w - bx;
    }

  if (by + bh > g_fg.h)
    {
      bh = g_fg.h - by;
    }

  if (bw <= 0 || bh <= 0 || cw <= 0 || ch <= 0)
    {
      return;
    }

  s.dx    = g_fg.ox + bx;
  s.dy    = g_fg.oy + by;
  s.w     = bw;
  s.h     = bh;
  s.a     = a;
  s.b     = b;
  s.c     = c;
  s.d     = d;
  s.alpha = flags_alpha(flags);
  s.flags = ((flags & FG_OPAQUE) ? 0 : FPC_SPRITE_KEY) |
            ((flags & FG_ADD) ? FPC_SPRITE_ADD : 0);
  s.key   = FG_KEY;

  /* The cell in pieces the GPU takes, each sampled over the whole box
   * with the map moved by its place in the cell
   */

  for (oy = 0; oy < ch; oy += FG_CELL_H)
    {
      for (ox = 0; ox < cw; ox += FG_CELL_W)
        {
          int32_t u = u0 - ox * 256;
          int32_t v = v0 - oy * 256;

          if (u > 32767 || u < -32768 || v > 32767 || v < -32768)
            {
              continue;                   /* (8.8 in the command: a piece far off) */
            }

          s.cx = cx + ox;
          s.cy = cy + oy;
          s.cw = cw - ox < FG_CELL_W ? cw - ox : FG_CELL_W;
          s.ch = ch - oy < FG_CELL_H ? ch - oy : FG_CELL_H;
          s.u0 = (int16_t)u;
          s.v0 = (int16_t)v;
          fpc_gpu_sprite(&s);
          g_fg.sprites++;
        }
    }
}

void fg_sprite(int img, int sx, int sy, int w, int h, int x, int y,
               uint32_t flags)
{
  FAR const struct fg_rect_s *r = fg_image_rect(img);

  if (r == NULL || w <= 0 || h <= 0 || sx < 0 || sy < 0 ||
      sx + w > r->w || sy + h > r->h)
    {
      return;
    }

  if ((flags & (FG_HFLIP | FG_VFLIP)) == 0)
    {
      struct fpc_gpu_blend_s bl;
      int dx = x;
      int dy = y;
      int dw = w;
      int dh = h;

      if (!clip(&dx, &dy, &dw, &dh))
        {
          return;
        }

      memset(&bl, 0, sizeof(bl));
      bl.dx    = g_fg.ox + dx;
      bl.dy    = g_fg.oy + dy;
      bl.w     = dw;
      bl.h     = dh;
      bl.sx    = r->x + sx + (dx - x);
      bl.sy    = r->y + sy + (dy - y);
      bl.alpha = flags_alpha(flags);
      bl.mode  = FPC_BLEND_SRC_RGB565 |
                 ((flags & FG_OPAQUE) ? 0 : FPC_BLEND_KEY);
      bl.key   = FG_KEY;
      bl.shift = (flags & FG_ADD) ? FPC_BLEND_SHIFT_ADD : 0;
      fpc_gpu_blend(&bl);
      g_fg.sprites++;
      return;
    }

  /* Flipped: the map mirrors the cell (column w - 1 - i: u = w * 256 -
   * 1 - (128 + 256 i)), at one pixel a pixel
   */

  fg_affine(r->x + sx, r->y + sy, w, h, x, y, w, h,
            (flags & FG_HFLIP) ? -256 : 256, 0, 0,
            (flags & FG_VFLIP) ? -256 : 256,
            (flags & FG_HFLIP) ? w * 256 - 129 : 128,
            (flags & FG_VFLIP) ? h * 256 - 129 : 128, flags);
}

/* TRI's affine sprite (TRI_OP_SPRITE_AFFINE, vdp1.c blit_affine): the box
 * the inverse matrix gives the cell's corners, sampled at pixel centres:
 * u = w * 128 + (a + b) / 2 + a (x - cx) + b (y - cy); a flip mirrors the
 * sampled column (or row) of the cell
 */

void fg_sprite_matrix(int img, int sx, int sy, int w, int h, int cx, int cy,
                      uint32_t flags, int a, int b, int c, int d)
{
  FAR const struct fg_rect_s *r = fg_image_rect(img);
  int32_t det;
  int64_t adet;
  int64_t hx;
  int64_t hy;
  int32_t ucen;
  int32_t vcen;
  int32_t u0;
  int32_t v0;
  int x0;
  int y0;

  if (r == NULL || w <= 0 || h <= 0 || sx < 0 || sy < 0 ||
      sx + w > r->w || sy + h > r->h)
    {
      return;
    }

  det = a * d - b * c;
  if (det == 0)
    {
      return;
    }

  adet = det < 0 ? -(int64_t)det : det;
  hx = ((((int64_t)(d < 0 ? -d : d) * w + (int64_t)(b < 0 ? -b : b) * h)
        << 7) / adet) + 1;
  hy = ((((int64_t)(c < 0 ? -c : c) * w + (int64_t)(a < 0 ? -a : a) * h)
        << 7) / adet) + 1;
  if (hx > 512 || hy > 512)
    {
      return;
    }

  x0 = cx - (int)hx;
  y0 = cy - (int)hy;
  ucen = (w << 7) + ((a + b) >> 1);
  vcen = (h << 7) + ((c + d) >> 1);
  u0 = ucen + a * (x0 - cx) + b * (y0 - cy);
  v0 = vcen + c * (x0 - cx) + d * (y0 - cy);

  if (flags & FG_HFLIP)                   /* column w - 1 - (u >> 8) */
    {
      u0 = (w << 8) - 1 - u0;
      a = -a;
      b = -b;
    }

  if (flags & FG_VFLIP)
    {
      v0 = (h << 8) - 1 - v0;
      c = -c;
      d = -d;
    }

  fg_affine(r->x + sx, r->y + sy, w, h, x0, y0, 2 * (int)hx + 1,
            2 * (int)hy + 1, a, b, c, d, u0, v0, flags);
}

void fg_sprite_rot(int img, int sx, int sy, int w, int h, int cx, int cy,
                   uint32_t flags, int angle, int scale)
{
  int co;
  int si;

  scale = scale < 16 ? 16 : scale;
  co = fg_cos(angle);
  si = fg_sin(angle);
  fg_sprite_matrix(img, sx, sy, w, h, cx, cy, flags,
                   co * 256 / scale, si * 256 / scale,
                   -si * 256 / scale, co * 256 / scale);
}

/* An RGB555 colour (TRI's) as RGB565, green's low bit its top one (as
 * TRI's VDP2 widens it), and never FG_KEY
 */

uint16_t fg_rgb555(uint16_t c)
{
  uint16_t g = (c >> 5) & 31;
  uint16_t v = (uint16_t)(((c >> 10) & 31) << 11 | (g << 1 | g >> 4) << 5 | (c & 31));

  return v == FG_KEY ? FG_KEY ^ 0x0020 : v;
}

int fg_cos(int angle)
{
  return (int)(fpc_gpu_cos(angle) >> 7);  /* Q15 -> 8.8 */
}

int fg_sin(int angle)
{
  return (int)(fpc_gpu_cos(angle - 1024) >> 7);
}

/****************************************************************************
 * Tile layers
 ****************************************************************************/

struct fg_layer_s
{
  bool on;
  int tileset;
  int tw;
  int th;
  int mapw;
  int maph;
  int perrow;                             /* Tiles a row of the tileset */
  int ntiles;
  struct fg_rect_s surf;                  /* The map, drawn */
  FAR uint16_t *map;
  FAR uint8_t *dirty;                     /* A bit a tile */
  int ndirty;
  bool all;                               /* Every tile to be drawn */
};

static struct fg_layer_s g_layer[FG_LAYERS];

static FAR struct fg_layer_s *layer_get(int layer)
{
  if (layer < 0 || layer >= FG_LAYERS || !g_layer[layer].on)
    {
      return NULL;
    }

  return &g_layer[layer];
}

void fg_layer_free(int layer)
{
  FAR struct fg_layer_s *l = layer_get(layer);

  if (l != NULL)
    {
      fpc_gpu_game_free(l->surf.x, l->surf.y);
      free(l->map);
      free(l->dirty);
      memset(l, 0, sizeof(*l));
    }
}

int fg_layer_init(int layer, int tileset, int tw, int th, int mapw, int maph)
{
  FAR struct fg_layer_s *l;
  int x;
  int y;

  if (layer < 0 || layer >= FG_LAYERS || tw <= 0 || th <= 0 ||
      tw > FG_CELL_W || th > FG_CELL_H || mapw <= 0 || maph <= 0 ||
      mapw * tw > FPC_GPU_SPACE_W || maph * th > FPC_GPU_SPACE_H)
    {
      return -EINVAL;
    }

  fg_layer_free(layer);
  l = &g_layer[layer];
  l->map   = calloc((size_t)mapw * maph, 2);
  l->dirty = calloc(((size_t)mapw * maph + 7) / 8, 1);
  if (l->map == NULL || l->dirty == NULL ||
      fpc_gpu_game_alloc(mapw * tw, maph * th, &x, &y) < 0)
    {
      free(l->map);
      free(l->dirty);
      memset(l, 0, sizeof(*l));
      return -ENOMEM;
    }

  l->on     = true;
  l->tw     = tw;
  l->th     = th;
  l->mapw   = mapw;
  l->maph   = maph;
  l->surf.x = x;
  l->surf.y = y;
  l->surf.w = mapw * tw;
  l->surf.h = maph * th;
  l->all    = true;
  fg_layer_tileset(layer, tileset);
  return OK;
}

void fg_layer_tileset(int layer, int tileset)
{
  FAR struct fg_layer_s *l = layer_get(layer);
  FAR const struct fg_rect_s *r = fg_image_rect(tileset);

  if (l != NULL)
    {
      l->tileset = tileset;
      l->perrow  = r != NULL ? r->w / l->tw : 0;
      l->ntiles  = r != NULL ? l->perrow * (r->h / l->th) : 0;
      l->all     = true;
    }
}

void fg_layer_set(int layer, int x, int y, uint16_t entry)
{
  FAR struct fg_layer_s *l = layer_get(layer);
  int i;

  if (l == NULL || x < 0 || y < 0 || x >= l->mapw || y >= l->maph)
    {
      return;
    }

  i = y * l->mapw + x;
  if (l->map[i] != entry)
    {
      l->map[i] = entry;
      if (!(l->dirty[i >> 3] & (1 << (i & 7))))
        {
          l->dirty[i >> 3] |= 1 << (i & 7);
          l->ndirty++;
        }
    }
}

uint16_t fg_layer_get(int layer, int x, int y)
{
  FAR struct fg_layer_s *l = layer_get(layer);

  if (l == NULL || x < 0 || y < 0 || x >= l->mapw || y >= l->maph)
    {
      return 0;
    }

  return l->map[y * l->mapw + x];
}

void fg_layer_redraw(int layer)
{
  FAR struct fg_layer_s *l = layer_get(layer);

  if (l != NULL)
    {
      l->all = true;
    }
}

/* Tile i of the map into the layer's surface */

static void layer_tile(FAR struct fg_layer_s *l, int i)
{
  FAR const struct fg_rect_s *ts = fg_image_rect(l->tileset);
  uint16_t e = l->map[i];
  int t = FG_TILE_INDEX(e);
  int dx = l->surf.x + (i % l->mapw) * l->tw;
  int dy = l->surf.y + (i / l->mapw) * l->th;
  int sx;
  int sy;

  if (ts == NULL || t == 0 || t >= l->ntiles)
    {
      fpc_gpu_fill(dx, dy, l->tw, l->th, FG_KEY);   /* Empty: see-through */
      return;
    }

  sx = ts->x + (t % l->perrow) * l->tw;
  sy = ts->y + (t / l->perrow) * l->th;
  if ((e & 0x3000) == 0)
    {
      fpc_gpu_copy(sx, sy, dx, dy, l->tw, l->th);
    }
  else
    {
      struct fpc_gpu_sprite_s s;
      bool hf = (e & 0x1000) != 0;
      bool vf = (e & 0x2000) != 0;

      s.dx    = dx;
      s.dy    = dy;
      s.w     = l->tw;
      s.h     = l->th;
      s.cx    = sx;
      s.cy    = sy;
      s.cw    = l->tw;
      s.ch    = l->th;
      s.a     = hf ? -256 : 256;
      s.b     = 0;
      s.c     = 0;
      s.d     = vf ? -256 : 256;
      s.u0    = hf ? l->tw * 256 - 129 : 128;
      s.v0    = vf ? l->th * 256 - 129 : 128;
      s.alpha = 255;
      s.flags = 0;                        /* (FG_KEY pixels copied as they are) */
      s.key   = 0;
      fpc_gpu_sprite(&s);
    }

  g_fg.tiles++;
}

void fg_layer_draw(int layer, int scrollx, int scrolly)
{
  FAR struct fg_layer_s *l = layer_get(layer);
  int n;

  if (l == NULL)
    {
      return;
    }

  n = l->mapw * l->maph;
  if (l->all)
    {
      int i;

      for (i = 0; i < n; i++)
        {
          layer_tile(l, i);
        }

      memset(l->dirty, 0, (n + 7) / 8);
      l->ndirty = 0;
      l->all = false;
    }
  else if (l->ndirty > 0)
    {
      int i;

      for (i = 0; i < n && l->ndirty > 0; i += 8)
        {
          uint8_t m = l->dirty[i >> 3];
          int k;

          if (m == 0)
            {
              continue;
            }

          for (k = 0; k < 8; k++)
            {
              if (m & (1 << k))
                {
                  layer_tile(l, i + k);
                  l->ndirty--;
                }
            }

          l->dirty[i >> 3] = 0;
        }
    }

  fg_blit_wrap(&l->surf, scrollx, scrolly);
}

/* The canvas from a surface that wraps round (a layer's map), keyed on
 * FG_KEY: bands down, pieces across
 */

void fg_blit_wrap(FAR const struct fg_rect_s *surf, int scrollx, int scrolly)
{
  struct fpc_gpu_blend_s bl;
  int y;

  memset(&bl, 0, sizeof(bl));
  bl.alpha = 255;
  bl.mode  = FPC_BLEND_SRC_RGB565 | FPC_BLEND_KEY;
  bl.key   = FG_KEY;

  scrollx %= surf->w;
  scrolly %= surf->h;
  scrollx += scrollx < 0 ? surf->w : 0;
  scrolly += scrolly < 0 ? surf->h : 0;

  for (y = 0; y < g_fg.h; )
    {
      int my = (scrolly + y) % surf->h;
      int bh = surf->h - my;
      int x;

      bh = bh > g_fg.h - y ? g_fg.h - y : bh;
      for (x = 0; x < g_fg.w; )
        {
          int mx = (scrollx + x) % surf->w;
          int bw = surf->w - mx;

          bw = bw > g_fg.w - x ? g_fg.w - x : bw;
          bl.dx = g_fg.ox + x;
          bl.dy = g_fg.oy + y;
          bl.w  = bw;
          bl.h  = bh;
          bl.sx = surf->x + mx;
          bl.sy = surf->y + my;
          fpc_gpu_blend(&bl);
          x += bw;
        }

      y += bh;
    }
}

/****************************************************************************
 * Text
 ****************************************************************************/

static struct
{
  struct fg_rect_s surf;                  /* 64 x 32 characters, alpha */
  uint16_t colour;
  uint8_t  chars[FG_TEXT_ROWS][FG_TEXT_COLS];
  uint8_t  count[FG_TEXT_ROWS];           /* Characters other than spaces */
  bool     ready;
} g_text;

static bool text_open(void)
{
  int x;
  int y;

  if (!g_text.ready)
    {
      if (fpc_gpu_game_alloc(FG_TEXT_COLS * 8, FG_TEXT_ROWS * 8, &x, &y) < 0)
        {
          return false;
        }

      g_text.surf.x = x;
      g_text.surf.y = y;
      g_text.surf.w = FG_TEXT_COLS * 8;
      g_text.surf.h = FG_TEXT_ROWS * 8;
      fpc_gpu_fill(x, y, g_text.surf.w, g_text.surf.h, 0);
      memset(g_text.chars, ' ', sizeof(g_text.chars));
      memset(g_text.count, 0, sizeof(g_text.count));
      if (g_text.colour == 0)
        {
          g_text.colour = 0xffff;
        }

      g_text.ready = true;
    }

  return true;
}

static void text_char(int col, int row, uint8_t ch)
{
  uint16_t px[64];
  int r;
  int k;

  if (ch < 0x20 || ch > 0x7f)
    {
      ch = '?';
    }

  if (g_text.chars[row][col] == ch)
    {
      return;
    }

  g_text.count[row] += (ch != ' ') - (g_text.chars[row][col] != ' ');
  g_text.chars[row][col] = ch;
  for (r = 0; r < 8; r++)
    {
      uint8_t bits = g_fg_font8x8[ch - 0x20][r];

      for (k = 0; k < 8; k++)
        {
          px[r * 8 + k] = (bits >> k) & 1 ? 0x00ff : 0;   /* A8: alpha */
        }
    }

  fpc_gpu_put(g_text.surf.x + col * 8, g_text.surf.y + row * 8, 8, 8, px,
              16);
}

static void text_vprint(int col, int row, FAR const char *fmt, va_list ap)
{
  char buf[FG_TEXT_COLS + 1];
  int i;

  if (row < 0 || row >= FG_TEXT_ROWS || !text_open())
    {
      return;
    }

  vsnprintf(buf, sizeof(buf), fmt, ap);
  for (i = 0; buf[i] != '\0'; i++)
    {
      if (col + i >= 0 && col + i < FG_TEXT_COLS)
        {
          text_char(col + i, row, (uint8_t)buf[i]);
        }
    }
}

void fg_text_print(int col, int row, FAR const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  text_vprint(col, row, fmt, ap);
  va_end(ap);
}

void fg_text_center(int row, FAR const char *fmt, ...)
{
  char buf[FG_TEXT_COLS + 1];
  va_list ap;
  int n;

  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  n = n > FG_TEXT_COLS ? FG_TEXT_COLS : n;
  fg_text_print((g_fg.w / 8 - n) / 2, row, "%s", buf);
}

void fg_text_clear(void)
{
  if (g_text.ready)
    {
      fpc_gpu_fill(g_text.surf.x, g_text.surf.y, g_text.surf.w,
                   g_text.surf.h, 0);
      memset(g_text.chars, ' ', sizeof(g_text.chars));
      memset(g_text.count, 0, sizeof(g_text.count));
    }
}

void fg_text_color(uint16_t colour)
{
  g_text.colour = colour;
}

void fg_text_draw(void)
{
  struct fpc_gpu_blend_s bl;
  int rows = (g_fg.h + 7) / 8;
  int r;

  if (!g_text.ready)
    {
      return;
    }

  memset(&bl, 0, sizeof(bl));
  bl.alpha  = 255;
  bl.mode   = FPC_BLEND_SRC_A8;
  bl.colour = g_text.colour;
  for (r = 0; r < rows && r < FG_TEXT_ROWS; r++)
    {
      int n = 1;

      if (g_text.count[r] == 0)
        {
          continue;
        }

      while (r + n < rows && r + n < FG_TEXT_ROWS && g_text.count[r + n] != 0)
        {
          n++;                            /* (a band of rows at once) */
        }

      bl.dx = g_fg.ox;
      bl.dy = g_fg.oy + r * 8;
      bl.w  = g_fg.w < g_text.surf.w ? g_fg.w : g_text.surf.w;
      bl.h  = n * 8 > g_fg.h - r * 8 ? g_fg.h - r * 8 : n * 8;
      bl.sx = g_text.surf.x;
      bl.sy = g_text.surf.y + r * 8;
      fpc_gpu_blend(&bl);
      r += n - 1;
    }
}

/****************************************************************************
 * The game has ended
 ****************************************************************************/

void fg_gfx_reset(void)
{
  int i;

  for (i = 0; i < FG_LAYERS; i++)
    {
      if (g_layer[i].on)
        {
          free(g_layer[i].map);
          free(g_layer[i].dirty);
        }
    }

  memset(g_layer, 0, sizeof(g_layer));
  memset(g_img, 0, sizeof(g_img));
  memset(&g_text, 0, sizeof(g_text));     /* (the surfaces: freed all together) */
}
