/****************************************************************************
 * apps/graphics/fpcdesk/desk_draw.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Drawing: lists of GPU operations (nx_gpuop_s, nuttx/nx/nxcompose.h) for
 * anti-aliased round rectangles and discs (BLEND with a shape table),
 * gradients (BLEND with a ramp, dithered), soft glows, and anti-aliased
 * text (BLEND of a colour through the coverage of glyphs kept in a glyph
 * atlas: a hidden window whose surface is in the GPU's memory, so text
 * costs one small command per character and no pixels on the link).
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nuttx/nx/nxbe.h>

#include "desk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ATLAS_W        512             /* Its height: what the glyphs need */
#define NFONTS         7

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct atlas_font_s
{
  const struct desk_font_s *font;
  struct nxgl_point_s pos[128];      /* Where each glyph is in the atlas */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct atlas_font_s g_afont[NFONTS];

static const struct desk_font_s * const g_fonts[NFONTS] =
{
  &g_desk_font_ui, &g_desk_font_uib, &g_desk_font_small,
  &g_desk_font_title, &g_desk_font_big, &g_desk_font_mono,
  &g_desk_font_monob
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static struct atlas_font_s *atlas_font(const struct desk_font_s *f)
{
  int i;

  for (i = 0; i < NFONTS; i++)
    {
      if (g_afont[i].font == f)
        {
          return &g_afont[i];
        }
    }

  return NULL;
}

static const struct desk_glyph_s *glyph(const struct desk_font_s *f,
                                        uint8_t ch)
{
  int i = ch - DESK_FONT_FIRST;

  if (i < 0 || i >= f->nglyphs)
    {
      i = '?' - DESK_FONT_FIRST;
    }

  return &f->glyphs[i];
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ops_reset(struct desk_ops_s *o)
{
  o->n = 0;
}

struct nx_gpuop_s *ops_add(struct desk_ops_s *o)
{
  struct nx_gpuop_s *op;

  if (o->n >= NX_GPUOPS_MAX)
    {
      static struct nx_gpuop_s spare;

      return &spare;                     /* Full: dropped */
    }

  op = &o->op[o->n++];
  memset(op, 0, sizeof(*op));
  op->alpha = 255;
  return op;
}

static struct nx_gpuop_s *ops_rect(struct desk_ops_s *o, uint8_t kind,
                                   int x, int y, int w, int h)
{
  struct nx_gpuop_s *op = ops_add(o);

  op->op = kind;
  op->x  = x;
  op->y  = y;
  op->w  = w;
  op->h  = h;
  return op;
}

void ops_fill(struct desk_ops_s *o, int x, int y, int w, int h, uint16_t c)
{
  ops_rect(o, NX_GPUOP_FILL, x, y, w, h)->colour = c;
}

void ops_blend(struct desk_ops_s *o, int x, int y, int w, int h, uint16_t c,
               uint8_t alpha)
{
  struct nx_gpuop_s *op = ops_rect(o, NX_GPUOP_BLEND, x, y, w, h);

  op->colour = c;
  op->alpha  = alpha;
  op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_DITHER;
}

/* A round rectangle: the table of a disc of radius r (slot) with the inner
 * rectangle inset by r
 */

void ops_rrect(struct desk_ops_s *o, int x, int y, int w, int h, int slot,
               int r, uint16_t c, uint8_t alpha)
{
  struct nx_gpuop_s *op = ops_rect(o, NX_GPUOP_BLEND, x, y, w, h);

  op->colour = c;
  op->alpha  = alpha;
  op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_LUT;
  op->slot   = slot;
  op->p[0]   = x + r;
  op->p[1]   = y + r;
  op->p[2]   = x + w - 1 - r;
  op->p[3]   = y + h - 1 - r;
}

void ops_disc(struct desk_ops_s *o, int cx, int cy, int slot, int r,
              uint16_t c, uint8_t alpha)
{
  ops_rrect(o, cx - r, cy - r, 2 * r, 2 * r, slot, r, c, alpha);
}

/* Gradients: c1 blended over c0 along a ramp, dithered */

static void ops_ramp(struct desk_ops_s *o, int x, int y, int w, int h,
                     uint16_t c1, uint8_t alpha, bool vertical, int len)
{
  struct nx_gpuop_s *op = ops_rect(o, NX_GPUOP_BLEND, x, y, w, h);

  op->colour = c1;
  op->alpha  = alpha;
  op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_DITHER |
               (vertical ? NX_BLEND_MOD_VRAMP : NX_BLEND_MOD_HRAMP);
  op->p[0]   = vertical ? y : x;
  op->p[1]   = len > 1 ? (255 * 256) / (len - 1) : 0;
  op->p[2]   = 0;
}

void ops_vgrad(struct desk_ops_s *o, int x, int y, int w, int h,
               uint16_t c0, uint16_t c1, uint8_t alpha)
{
  if (alpha == 255)
    {
      ops_fill(o, x, y, w, h, c0);
    }
  else
    {
      ops_blend(o, x, y, w, h, c0, alpha);
    }

  ops_ramp(o, x, y, w, h, c1, alpha, true, h);
}

void ops_hgrad(struct desk_ops_s *o, int x, int y, int w, int h,
               uint16_t c0, uint16_t c1, uint8_t alpha)
{
  if (alpha == 255)
    {
      ops_fill(o, x, y, w, h, c0);
    }
  else
    {
      ops_blend(o, x, y, w, h, c0, alpha);
    }

  ops_ramp(o, x, y, w, h, c1, alpha, false, w);
}

/* A round rectangle with a vertical gradient: bands of the same round
 * shape (its parameters are absolute, so the bands meet exactly), each of
 * one colour of the gradient, dithered
 */

uint16_t colour_mix(uint16_t a, uint16_t b, int t256)
{
  int r = ((a >> 11) * (256 - t256) + (b >> 11) * t256) >> 8;
  int g = (((a >> 5) & 63) * (256 - t256) + ((b >> 5) & 63) * t256) >> 8;
  int bl = ((a & 31) * (256 - t256) + (b & 31) * t256) >> 8;

  return (uint16_t)(r << 11 | g << 5 | bl);
}

void ops_rrect_vgrad(struct desk_ops_s *o, int x, int y, int w, int h,
                     int slot, int r, uint16_t c0, uint16_t c1,
                     uint8_t alpha)
{
  int bands = h / 3 > 16 ? 16 : h / 3 < 2 ? 2 : h / 3;
  int i;

  for (i = 0; i < bands; i++)
    {
      int y0 = y + h * i / bands;
      int y1 = y + h * (i + 1) / bands;
      struct nx_gpuop_s *op = ops_rect(o, NX_GPUOP_BLEND, x, y0, w, y1 - y0);

      op->colour = colour_mix(c0, c1, (256 * i + 128) / bands);
      op->alpha  = alpha;
      op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_LUT | NX_BLEND_DITHER;
      op->slot   = slot;
      op->p[0]   = x + r;
      op->p[1]   = y + r;
      op->p[2]   = x + w - 1 - r;
      op->p[3]   = y + h - 1 - r;
    }
}

/* A soft glow: a Gaussian around a square of half-size r, its distances
 * scaled by 2^shift.  Scaled distances come in steps of 2^shift pixels:
 * 2 x 2 passes half a step apart, each with the alpha that makes the four
 * add up to alpha, hide them.
 */

void ops_glow(struct desk_ops_s *o, int cx, int cy, int r, int shift,
              uint16_t c, uint8_t alpha)
{
  int reach = ((NX_BLEND_LUT_N - 1) << shift) + r;
  int passes = shift >= 2 ? 2 : 1;
  int half = shift >= 2 ? 1 << (shift - 1) : 0;
  int a1 = alpha;
  int i;
  int j;

  /* 1 - (1 - a1)^n = alpha: the largest a1 whose n passes stay under */

  if (passes > 1)
    {
      for (a1 = alpha; a1 > 1; a1--)
        {
          int keep = 255;

          for (i = 0; i < passes * passes; i++)
            {
              keep = keep * (255 - a1) / 255;
            }

          if (255 - keep <= alpha)
            {
              break;
            }
        }
    }

  for (j = 0; j < passes; j++)
    {
      for (i = 0; i < passes; i++)
        {
          int ox = cx + i * half;
          int oy = cy + j * half;
          struct nx_gpuop_s *op = ops_rect(o, NX_GPUOP_BLEND, ox - reach,
                                           oy - reach, 2 * reach + 1,
                                           2 * reach + 1);

          op->colour = c;
          op->alpha  = (uint8_t)a1;
          op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_LUT |
                       NX_BLEND_DITHER;
          op->slot   = LUT_GLOW;
          op->shift  = shift;
          op->p[0]   = ox - r;
          op->p[1]   = oy - r;
          op->p[2]   = ox + r;
          op->p[3]   = oy + r;
        }
    }
}

/* Text: y is the top of the line (baseline at y + ascent); returns the
 * width drawn
 */

int ops_text(struct desk_ops_s *o, const struct desk_font_s *f, int x,
             int y, const char *s, uint16_t c, uint8_t alpha)
{
  struct atlas_font_s *af = atlas_font(f);
  int x0 = x;

  for (; *s != '\0'; s++)
    {
      uint8_t ch = (uint8_t)*s;
      const struct desk_glyph_s *g = glyph(f, ch);
      int gi = g - f->glyphs;

      if (g->w > 0 && af != NULL && g_desk.atlas != NULL)
        {
          struct nx_gpuop_s *op = ops_rect(o, NX_GPUOP_BLEND, x + g->bx,
                                           y + f->ascent - g->by, g->w,
                                           g->h);

          op->src    = g_desk.atlas;
          op->sx     = af->pos[gi].x;
          op->sy     = af->pos[gi].y;
          op->colour = c;
          op->alpha  = alpha;
          op->mode   = NX_BLEND_SRC_A8;
        }

      x += g->adv;
    }

  return x - x0;
}

int text_width(const struct desk_font_s *f, const char *s)
{
  int w = 0;

  for (; *s != '\0'; s++)
    {
      w += glyph(f, (uint8_t)*s)->adv;
    }

  return w;
}

int text_height(const struct desk_font_s *f)
{
  return f->ascent + f->descent;
}

void ops_text_centre(struct desk_ops_s *o, const struct desk_font_s *f,
                     int cx, int y, const char *s, uint16_t c,
                     uint8_t alpha)
{
  ops_text(o, f, cx - text_width(f, s) / 2, y, s, c, alpha);
}

void ops_draw(NXWINDOW hwnd, struct desk_ops_s *o)
{
  if (o->n > 0)
    {
      nx_gpuops(hwnd, o->op, o->n);
    }

  o->n = 0;
}

void ops_setlist(NXWINDOW hwnd, struct desk_ops_s *o)
{
  nx_setdisplaylist(hwnd, o->op, o->n);
  o->n = 0;
}

static void list_take(struct desk_list_s *l, struct desk_ops_s *o)
{
  if (l->n + o->n > l->cap)
    {
      int cap = l->n + o->n + NX_GPUOPS_MAX;
      struct nx_gpuop_s *p;

      cap = cap > NX_DLIST_MAX ? NX_DLIST_MAX : cap;
      p   = realloc(l->op, cap * sizeof(*p));
      if (p != NULL)
        {
          l->op  = p;
          l->cap = cap;
        }
    }

  if (l->n + o->n <= l->cap)
    {
      memcpy(l->op + l->n, o->op, o->n * sizeof(o->op[0]));
      l->n += o->n;
    }

  o->n = 0;                              /* (Dropped if there is no room) */
}

void list_room(struct desk_list_s *l, struct desk_ops_s *o, int need)
{
  if (o->n + need > NX_GPUOPS_MAX)
    {
      list_take(l, o);
    }
}

void list_set(NXWINDOW hwnd, struct desk_list_s *l, struct desk_ops_s *o)
{
  if (l->n == 0)
    {
      ops_setlist(hwnd, o);                /* It all fitted in o */
      return;
    }

  list_take(l, o);
  nx_setdisplaylist(hwnd, l->op, l->n);
  l->n = 0;
}

/****************************************************************************
 * Name: desk_luts_init
 *
 * Description:
 *   The client shape tables: discs of radius 12 and 6 (coverage, 8 x 8
 *   samples a pixel), as the server makes its own.
 *
 ****************************************************************************/

static void lut_disc(uint8_t lut[NX_BLEND_LUT_N][NX_BLEND_LUT_N], int r)
{
  int dx;
  int dy;
  int i;
  int j;

  for (dy = 0; dy < NX_BLEND_LUT_N; dy++)
    {
      for (dx = 0; dx < NX_BLEND_LUT_N; dx++)
        {
          int n = 0;

          for (j = 0; j < 8; j++)
            {
              for (i = 0; i < 8; i++)
                {
                  /* Sample offsets from the inner corner, in 1/16 pixel */

                  int u = -dx * 16 + i * 2 + 1;
                  int v = -dy * 16 + j * 2 + 1;

                  u = u < 0 ? -u : 0;
                  v = v < 0 ? -v : 0;
                  n += u * u + v * v <= r * r * 256;
                }
            }

          lut[dy][dx] = (uint8_t)((n * 255 + 32) / 64);
        }
    }
}

/* exp(-(d / 12)^2) at distance d from a point, 0 at the edge of the table
 * (integer: a table of e^-k/16 and linear steps between)
 */

static void lut_glow(uint8_t lut[NX_BLEND_LUT_N][NX_BLEND_LUT_N])
{
  static const uint16_t e[] =                /* 65535 e^-(k/4), k = 0.. */
  {
    65535, 51039, 39749, 30957, 24109, 18776, 14623, 11388, 8869, 6907,
    5379, 4190, 3263, 2541, 1979, 1541, 1200, 935, 728, 567, 442, 344,
    268, 209, 162, 127, 99, 77, 60, 46, 36, 28, 22, 17, 13, 10, 8, 6, 5,
    4, 3, 2, 2, 1, 1, 1, 0
  };

  int dx;
  int dy;

  for (dy = 0; dy < NX_BLEND_LUT_N; dy++)
    {
      for (dx = 0; dx < NX_BLEND_LUT_N; dx++)
        {
          /* (d / 12)^2 in quarters: (dx^2 + dy^2) * 4 / 144 */

          int q16 = (dx * dx + dy * dy) * 16 * 4 / 144;   /* 1/16 quarters */
          int k = q16 >> 4;
          int f = q16 & 15;
          int n = sizeof(e) / sizeof(e[0]) - 1;
          uint32_t v;

          if (k >= n)
            {
              v = 0;
            }
          else
            {
              v = (e[k] * (16 - f) + e[k + 1] * f) / 16;
            }

          if (dx == NX_BLEND_LUT_N - 1 || dy == NX_BLEND_LUT_N - 1)
            {
              v = 0;
            }

          lut[dy][dx] = (uint8_t)((v * 255 + 32767) / 65535);
        }
    }
}

void desk_luts_init(void)
{
  static uint8_t lut[NX_BLEND_LUT_N][NX_BLEND_LUT_N];

  lut_disc(lut, 12);
  nx_setlut(g_desk.hnx, LUT_R12, lut);
  lut_disc(lut, 6);
  nx_setlut(g_desk.hnx, LUT_R6, lut);
  lut_disc(lut, 3);
  nx_setlut(g_desk.hnx, LUT_R3, lut);
  lut_disc(lut, 20);
  nx_setlut(g_desk.hnx, LUT_R20, lut);
  lut_glow(lut);
  nx_setlut(g_desk.hnx, LUT_GLOW, lut);
}

/****************************************************************************
 * Name: desk_atlas_init
 *
 * Description:
 *   The glyph atlas: every glyph of every font, packed in shelves into a
 *   hidden window (so a surface in the GPU's memory), one pixel a glyph
 *   coverage value in its low byte (BLEND's A8 source).
 *
 ****************************************************************************/

static void atlas_redraw(NXWINDOW hwnd, const struct nxgl_rect_s *rect,
                         bool more, void *arg)
{
}

static const struct nx_callback_s g_atlascb =
{
  atlas_redraw, NULL, NULL, NULL, NULL
};

/* Shelf packing: where each glyph goes; returns the height used */

static int atlas_pack(bool record)
{
  int x = 0;
  int y = 0;
  int shelf = 0;
  int f;
  int i;

  for (f = 0; f < NFONTS; f++)
    {
      const struct desk_font_s *font = g_fonts[f];

      for (i = 0; i < font->nglyphs; i++)
        {
          const struct desk_glyph_s *g = &font->glyphs[i];

          if (g->w == 0 || g->h == 0)
            {
              continue;
            }

          if (x + g->w > ATLAS_W)
            {
              x = 0;
              y += shelf + 1;
              shelf = 0;
            }

          if (record)
            {
              g_afont[f].font = font;
              g_afont[f].pos[i].x = x;
              g_afont[f].pos[i].y = y;
            }

          x += g->w + 1;
          if (g->h > shelf)
            {
              shelf = g->h;
            }
        }
    }

  return y + shelf;
}

int desk_atlas_init(void)
{
  static uint16_t pix[64 * 64];
  struct nxgl_rect_s dest;
  struct nxgl_point_s origin;
  struct nxgl_point_s pos;
  struct nxgl_size_s size;
  const void *src[1];
  int f;
  int i;

  g_desk.atlas = nx_openwindow(g_desk.hnx,
                               NXBE_WINDOW_RAMBACKED | NXBE_WINDOW_HIDDEN,
                               &g_atlascb, NULL);
  if (g_desk.atlas == NULL)
    {
      return -1;
    }

  pos.x  = 0;
  pos.y  = 0;
  size.w = ATLAS_W;
  size.h = atlas_pack(true);
  nx_setsize(g_desk.atlas, &size);
  nx_setposition(g_desk.atlas, &pos);

  for (f = 0; f < NFONTS; f++)
    {
      const struct desk_font_s *font = g_fonts[f];

      for (i = 0; i < font->nglyphs; i++)
        {
          const struct desk_glyph_s *g = &font->glyphs[i];
          int k;

          if (g->w == 0 || g->h == 0 || g->w * g->h > 64 * 64)
            {
              continue;
            }

          for (k = 0; k < g->w * g->h; k++)
            {
              pix[k] = font->data[g->off + k];
            }

          dest.pt1   = g_afont[f].pos[i];
          dest.pt2.x = dest.pt1.x + g->w - 1;
          dest.pt2.y = dest.pt1.y + g->h - 1;
          origin     = dest.pt1;
          src[0]     = pix;
          nx_bitmap(g_desk.atlas, &dest, src, &origin, g->w * 2);
        }
    }

  return 0;
}
