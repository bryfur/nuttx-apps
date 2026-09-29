/****************************************************************************
 * apps/graphics/fpcdesk/desk_bench_demo.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * GPU Bench: the demonstrations (desk_bench.h), each what the GPU does
 * well, drawn on the desktop's frames into the window's stage:
 *
 *   Plasma   bands of colour, each a FILL and two BLENDs along ramps whose
 *            origins sway: all of a panel every other frame (30 a second)
 *   Stars    a star field: FILLs of a pixel or two, near stars discs from
 *            the glyph atlas (A8 BLENDs)
 *   Orbits   soft glows (the glow shape table) circling the middle
 *   Blobs    translucent windows (BLENDs through the round-corner shape
 *            table, their traffic lights discs) bouncing over a grid
 *
 * Everything is a function of the demo's clock t, so a frame draws again
 * only where something moved: for each thing, where it was and where it
 * is now (one rectangle, or two when they are far apart), the background
 * there and whatever is in it, cut to it, sent on its own (the compositor
 * copies each such rectangle to the display, no more).  Plasma and Stars
 * change everything they show: they are panels of 448 x 256 at most.
 * While the desktop is frozen
 * (deskctl freeze) the clock stands at 1.5 s and nothing moves: the tests'
 * pictures are the same every time.
 ****************************************************************************/

#include <nuttx/config.h>

#include <math.h>
#include <string.h>

#include "desk_bench.h"

#ifdef CONFIG_FPCDESK_BENCH

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FROZEN_T         1500           /* The clock while frozen, ms */
#define PLASMA_MS        33             /* Plasma: 30 frames a second */
#define PANEL_W          448            /* Plasma, Stars: a panel, at most */
#define PANEL_H          256
#define BAND             4              /* ... its bands */
#define NSTARS           160
#define NGLOWS           5
#define NBLOBS           5
#define GRID             32             /* Blobs: the grid */
#define GLOW_REACH       24             /* Orbits: the glows' reach, steps */

#define C_STARS_BG       RGB(0x02, 0x02, 0x08)
#define C_ORBITS_BG      RGB(0x06, 0x08, 0x1a)
#define C_BLOBS_BG       RGB(0x1c, 0x20, 0x34)
#define C_BLOBS_GRID     RGB(0x2c, 0x32, 0x4e)
#define C_STAGE_BG       RGB(0x0c, 0x0c, 0x12)   /* Round the panel */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char * const g_names[DEMO_COUNT] =
{
  "Plasma", "Stars", "Orbits", "Blobs"
};

static const char * const g_about[DEMO_COUNT] =
{
  "Plasma: bands of colour, a FILL and two BLENDs along ramps each, "
  "30 frames a second",
  "Stars: FILLs of a pixel or two, near stars A8 discs; only what moved "
  "is drawn",
  "Orbits: soft glows through the glow shape table, circling; only what "
  "moved is drawn",
  "Blobs: translucent windows through the round-corner table, bouncing; "
  "only what moved is drawn",
};

/* The objects' rectangles now (the stage's frame) */

static struct bench_rect_s g_cur[BENCH_MAXOBJ];

/****************************************************************************
 * Private Functions: rectangles and drawing
 ****************************************************************************/

static bool r_empty(const struct bench_rect_s *r)
{
  return r->w <= 0 || r->h <= 0;
}

static bool r_meet(const struct bench_rect_s *a, const struct bench_rect_s *b)
{
  return !r_empty(a) && !r_empty(b) && a->x < b->x + b->w &&
         b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h;
}

static struct bench_rect_s r_and(const struct bench_rect_s *a,
                                 const struct bench_rect_s *b)
{
  struct bench_rect_s r;
  int x1 = a->x + a->w < b->x + b->w ? a->x + a->w : b->x + b->w;
  int y1 = a->y + a->h < b->y + b->h ? a->y + a->h : b->y + b->h;

  r.x = a->x > b->x ? a->x : b->x;
  r.y = a->y > b->y ? a->y : b->y;
  r.w = x1 - r.x;
  r.h = y1 - r.y;
  if (r.w <= 0 || r.h <= 0)
    {
      r.w = 0;
      r.h = 0;
    }

  return r;
}

static struct bench_rect_s r_or(const struct bench_rect_s *a,
                                const struct bench_rect_s *b)
{
  struct bench_rect_s r;
  int x1;
  int y1;

  if (r_empty(a))
    {
      return *b;
    }

  if (r_empty(b))
    {
      return *a;
    }

  x1  = a->x + a->w > b->x + b->w ? a->x + a->w : b->x + b->w;
  y1  = a->y + a->h > b->y + b->h ? a->y + a->h : b->y + b->h;
  r.x = a->x < b->x ? a->x : b->x;
  r.y = a->y < b->y ? a->y : b->y;
  r.w = x1 - r.x;
  r.h = y1 - r.y;
  return r;
}

static struct bench_rect_s r_make(int x, int y, int w, int h)
{
  struct bench_rect_s r;

  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  return r;
}

/* Our list: what was added since the last cut is cut to the clip */

static void cut(struct bench_s *b)
{
  if (!b->inkit)
    {
      ui_clip(b->o, b->from, b->clip.x, b->clip.y, b->clip.x + b->clip.w,
              b->clip.y + b->clip.h);
      b->from = b->o->n;
    }
}

static void send(struct bench_s *b)
{
  cut(b);
  b->ops += b->o->n;
  ops_draw((NXWINDOW)b->w->hwnd, b->o);
  b->from = 0;
}

/* A list with room for n more operations, cut to the clip */

static struct desk_ops_s *room(struct bench_s *b, int n)
{
  if (b->inkit)
    {
      return kit_ops(&b->kit, n);
    }

  cut(b);
  if (b->o->n + n > NX_GPUOPS_MAX)
    {
      send(b);
    }

  return b->o;
}

static void set_clip(struct bench_s *b, const struct bench_rect_s *r)
{
  cut(b);
  b->clip = *r;
}

static float frac(float v)
{
  return v - floorf(v);
}

/* A colour of hue h (turns), value v, fairly saturated */

static uint16_t hue(float h, float v)
{
  float s = 0.78f;
  float f;
  float p;
  float q;
  float t;
  float r;
  float g;
  float bl;
  int i;

  h = frac(h) * 6;
  i = (int)h;
  f = h - i;
  p = v * (1 - s);
  q = v * (1 - s * f);
  t = v * (1 - s * (1 - f));
  switch (i)
    {
      case 0:
        r = v, g = t, bl = p;
        break;

      case 1:
        r = q, g = v, bl = p;
        break;

      case 2:
        r = p, g = v, bl = t;
        break;

      case 3:
        r = p, g = q, bl = v;
        break;

      case 4:
        r = t, g = p, bl = v;
        break;

      default:
        r = v, g = p, bl = q;
        break;
    }

  return RGB((int)(r * 255), (int)(g * 255), (int)(bl * 255));
}

/* A number in [0, 1) for (i, k), always the same */

static float rnd(uint32_t i, uint32_t k)
{
  uint32_t x = i * 2654435761u ^ (k + 1) * 0x9e3779b9u;

  x ^= x >> 15;
  x *= 0x2c1b3c6du;
  x ^= x >> 12;
  x *= 0x297a2d39u;
  x ^= x >> 15;
  return (float)(x >> 8) / 16777216.0f;
}

static void fill_clip(struct bench_s *b, uint16_t c)
{
  ops_fill(room(b, 1), b->clip.x, b->clip.y, b->clip.w, b->clip.h, c);
}

/****************************************************************************
 * Private Functions: Plasma
 ****************************************************************************/

/* Plasma and Stars change all of their area every frame: the compositor
 * copies all of it to the display each time, so it is a panel of a size
 * the GPU does that for cheaply (a COPY of 448 x 256 is 2.2 ms)
 */

static struct bench_rect_s demo_panel(struct bench_s *b)
{
  int w = b->stage.w - 24 < PANEL_W ? b->stage.w - 24 : PANEL_W;
  int h = b->stage.h - 24 < PANEL_H ? b->stage.h - 24 : PANEL_H;

  h -= h % BAND;
  return r_make(b->stage.x + (b->stage.w - w) / 2,
                b->stage.y + (b->stage.h - h) / 2, w, h);
}

static void plasma_paint(struct bench_s *b)
{
  struct bench_rect_s p = demo_panel(b);
  float ts = b->t / 1000.0f;
  int len = p.w / 3 > 8 ? p.w / 3 : 8;
  int step = 255 * 256 / len;
  int i;

  if (b->clip.x < p.x || b->clip.y < p.y ||
      b->clip.x + b->clip.w > p.x + p.w ||
      b->clip.y + b->clip.h > p.y + p.h)
    {
      fill_clip(b, C_STAGE_BG);
    }

  for (i = 0; i < p.h / BAND; i++)
    {
      struct bench_rect_s band = r_make(p.x, p.y + i * BAND, p.w, BAND);
      struct desk_ops_s *o;
      struct nx_gpuop_s *op;
      int o1;
      int o2;

      if (!r_meet(&band, &b->clip))
        {
          continue;
        }

      o1 = p.x + (int)(p.w * (0.5f + 0.42f * sinf(ts * 1.3f + i * 0.19f))) -
           len / 2;
      o2 = p.x + (int)(p.w * (0.5f + 0.42f * sinf(ts * 0.9f - i * 0.13f +
                                                   2.1f))) - len / 2;

      o = room(b, 3);
      ops_fill(o, band.x, band.y, band.w, band.h,
               hue(ts * 0.03f + i * 0.004f, 0.28f));

      /* Rising to the right from o1; falling to the right from o2 */

      op = ops_add(o);
      op->op     = NX_GPUOP_BLEND;
      op->x      = band.x;
      op->y      = band.y;
      op->w      = band.w;
      op->h      = band.h;
      op->colour = hue(ts * 0.05f + 0.33f + i * 0.010f, 1.0f);
      op->alpha  = 230;
      op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_HRAMP |
                   NX_BLEND_DITHER;
      op->p[0]   = o1;
      op->p[1]   = step;
      op->p[2]   = 0;

      op = ops_add(o);
      *op = o->op[o->n - 2];
      op->colour = hue(ts * 0.07f + 0.66f - i * 0.008f, 0.92f);
      op->p[0]   = o2 + len;                /* 0 there, 255 at o2 */
      op->p[1]   = -step;
    }
}

/****************************************************************************
 * Private Functions: Stars
 ****************************************************************************/

/* Star i now: where (its centre) and how big; false if not on the stage */

static bool star_at(struct bench_s *b, int i, int *x, int *y, int *s,
                    uint16_t *c)
{
  float d = 0.04f + 0.96f * frac(rnd(i, 2) - b->t * 0.00022f);
  float sx = rnd(i, 0) * 2 - 1;
  float sy = rnd(i, 1) * 2 - 1;
  struct bench_rect_s p = demo_panel(b);
  int cx = p.x + p.w / 2;
  int cy = p.y + p.h / 2;
  int v;

  *x = cx + (int)(sx * p.w * 0.07f / d);
  *y = cy + (int)(sy * p.w * 0.07f / d);
  *s = d < 0.16f ? 6 : d < 0.4f ? 2 : 1;
  v  = (int)(255 * (1.1f - d));
  v  = v > 255 ? 255 : v < 40 ? 40 : v;
  *c = RGB(v * 7 / 8, v * 7 / 8, v);
  return *x - 3 >= p.x && *y - 3 >= p.y && *x + 3 < p.x + p.w &&
         *y + 3 < p.y + p.h;
}

static int stars_objs(struct bench_s *b, struct bench_rect_s *r)
{
  int x;
  int y;
  int s;
  int i;
  uint16_t c;

  for (i = 0; i < NSTARS; i++)
    {
      r[i] = star_at(b, i, &x, &y, &s, &c) ?
             r_make(x - s / 2, y - s / 2, s, s) : r_make(0, 0, 0, 0);
    }

  return NSTARS;
}

static void stars_paint(struct bench_s *b)
{
  struct bench_rect_s p = demo_panel(b);
  struct bench_rect_s in = r_and(&p, &b->clip);
  int x;
  int y;
  int s;
  int i;
  uint16_t c;

  if (in.w != b->clip.w || in.h != b->clip.h)
    {
      fill_clip(b, C_STAGE_BG);
    }

  if (!r_empty(&in))
    {
      ops_fill(room(b, 1), in.x, in.y, in.w, in.h, C_STARS_BG);
    }

  for (i = 0; i < NSTARS; i++)
    {
      struct bench_rect_s r;

      if (!star_at(b, i, &x, &y, &s, &c))
        {
          continue;
        }

      r = r_make(x - s / 2, y - s / 2, s, s);
      if (!r_meet(&r, &b->clip))
        {
          continue;
        }

      if (s < 6 || !ops_disc_atlas(room(b, 1), r.x, r.y, s, c, 255))
        {
          ops_fill(room(b, 1), r.x, r.y, s, s, c);
        }
    }
}

/****************************************************************************
 * Private Functions: Orbits
 ****************************************************************************/

static void glow_at(struct bench_s *b, int i, int *x, int *y, int *reach,
                    int *shift)
{
  float ts = b->t / 1000.0f;
  float a = ts * (0.45f + 0.12f * i) * (i & 1 ? -1 : 1) + i * 1.9f;
  float rx = b->stage.w * (0.1f + 0.065f * i);
  float ry = b->stage.h * (0.1f + 0.07f * i);

  *shift = 1;
  *reach = (GLOW_REACH << *shift) + 2;
  *x = b->stage.x + b->stage.w / 2 + (int)(rx * cosf(a));
  *y = b->stage.y + b->stage.h / 2 + (int)(ry * sinf(a));
}

/* A soft glow: the glow table (a Gaussian, 2 % at 24 of its steps) round
 * a point, only as far as it shows
 */

static void glow(struct desk_ops_s *o, int x, int y, int reach, int shift,
                 uint16_t c, uint8_t alpha)
{
  struct nx_gpuop_s *op = ops_add(o);

  op->op     = NX_GPUOP_BLEND;
  op->x      = x - reach;
  op->y      = y - reach;
  op->w      = 2 * reach + 1;
  op->h      = 2 * reach + 1;
  op->colour = c;
  op->alpha  = alpha;
  op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_LUT | NX_BLEND_DITHER;
  op->slot   = LUT_GLOW;
  op->shift  = shift;
  op->p[0]   = x - 2;
  op->p[1]   = y - 2;
  op->p[2]   = x + 2;
  op->p[3]   = y + 2;
}

static int orbits_objs(struct bench_s *b, struct bench_rect_s *r)
{
  int x;
  int y;
  int reach;
  int shift;
  int i;

  for (i = 0; i < NGLOWS; i++)
    {
      glow_at(b, i, &x, &y, &reach, &shift);
      r[i] = r_make(x - reach, y - reach, 2 * reach + 1, 2 * reach + 1);
    }

  return NGLOWS;
}

static void orbits_paint(struct bench_s *b)
{
  int x;
  int y;
  int reach;
  int shift;
  int i;

  fill_clip(b, C_ORBITS_BG);
  for (i = 0; i < NGLOWS; i++)
    {
      struct bench_rect_s r;
      uint16_t c = hue(i / (float)NGLOWS + b->t * 0.00002f, 1.0f);

      glow_at(b, i, &x, &y, &reach, &shift);
      r = r_make(x - reach, y - reach, 2 * reach + 1, 2 * reach + 1);
      if (!r_meet(&r, &b->clip))
        {
          continue;
        }

      glow(room(b, 1), x, y, reach, shift, c, 200);
      ops_disc_atlas(room(b, 1), x - 3, y - 3, 6, C_WHITE, 230);
    }
}

/****************************************************************************
 * Private Functions: Blobs
 ****************************************************************************/

/* A bounce between 0 and range: p in pixels travelled */

static int bounce(float p, int range)
{
  int v;

  if (range <= 0)
    {
      return 0;
    }

  v = (int)p % (2 * range);
  return v < range ? v : 2 * range - v;
}

static struct bench_rect_s blob_at(struct bench_s *b, int i)
{
  int w = 90 + i * 10;
  int h = 62 + (i % 3) * 10;
  float ts = b->t / 1000.0f;

  w = w < b->stage.w ? w : b->stage.w;
  h = h < b->stage.h ? h : b->stage.h;
  return r_make(b->stage.x +
                bounce(rnd(i, 3) * 1000 + ts * (60 + 20 * i),
                       b->stage.w - w),
                b->stage.y +
                bounce(rnd(i, 4) * 1000 + ts * (45 + 14 * (NBLOBS - i)),
                       b->stage.h - h), w, h);
}

static int blobs_objs(struct bench_s *b, struct bench_rect_s *r)
{
  int i;

  for (i = 0; i < NBLOBS; i++)
    {
      r[i] = blob_at(b, i);
    }

  return NBLOBS;
}

static void blobs_paint(struct bench_s *b)
{
  static const uint16_t lights[3] =
  {
    C_CLOSE, C_MINIMIZE, C_MAXIMIZE
  };

  struct desk_ops_s *o;
  int x0 = b->clip.x;
  int y0 = b->clip.y;
  int x1 = b->clip.x + b->clip.w;
  int y1 = b->clip.y + b->clip.h;
  int g;
  int i;
  int j;

  /* The grid, in the stage's coordinates wherever it is cut */

  fill_clip(b, C_BLOBS_BG);
  for (g = (x0 - b->stage.x + GRID - 1) / GRID * GRID + b->stage.x; g < x1;
       g += GRID)
    {
      ops_fill(room(b, 1), g, y0, 1, y1 - y0, C_BLOBS_GRID);
    }

  for (g = (y0 - b->stage.y + GRID - 1) / GRID * GRID + b->stage.y; g < y1;
       g += GRID)
    {
      ops_fill(room(b, 1), x0, g, x1 - x0, 1, C_BLOBS_GRID);
    }

  for (i = 0; i < NBLOBS; i++)
    {
      struct bench_rect_s r = blob_at(b, i);

      if (!r_meet(&r, &b->clip))
        {
          continue;
        }

      o = room(b, 6);
      ops_rrect(o, r.x, r.y, r.w, r.h, LUT_R12, 12,
                hue(0.55f + i * 0.17f, 0.95f), 150);
      ops_blend(o, r.x + 1, r.y + 22, r.w - 2, 1, C_WHITE, 70);
      for (j = 0; j < 3; j++)
        {
          ops_disc_atlas(o, r.x + 9 + j * 14, r.y + 7, 8, lights[j], 255);
        }
    }
}

/****************************************************************************
 * Private Functions: any demo
 ****************************************************************************/

static void paint(struct bench_s *b)
{
  switch (b->demo)
    {
      case DEMO_PLASMA:
        plasma_paint(b);
        break;

      case DEMO_STARS:
        stars_paint(b);
        break;

      case DEMO_ORBITS:
        orbits_paint(b);
        break;

      default:
        blobs_paint(b);
        break;
    }
}

static int objs(struct bench_s *b, struct bench_rect_s *r)
{
  switch (b->demo)
    {
      case DEMO_STARS:
        return stars_objs(b, r);

      case DEMO_ORBITS:
        return orbits_objs(b, r);

      case DEMO_BLOBS:
        return blobs_objs(b, r);

      default:
        return 0;
    }
}

/* Draw again rectangle r (cut to the stage); into what has changed.  The
 * compositor takes what one nx_gpuops() draws as one rectangle to
 * composite again (all it covers): each rectangle goes on its own, so
 * that five things moving cost five small rectangles, not one round all
 * of them.  The stars go together: they are everywhere in their panel.
 */

static void repaint(struct bench_s *b, const struct bench_rect_s *r,
                    struct bench_rect_s *changed)
{
  struct bench_rect_s c = r_and(r, &b->stage);

  if (!r_empty(&c))
    {
      set_clip(b, &c);
      paint(b);
      *changed = r_or(changed, &c);
      if (b->demo != DEMO_STARS)
        {
          send(b);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

const char *bench_demo_name(int demo)
{
  return g_names[demo];
}

const char *bench_demo_about(int demo)
{
  return g_about[demo];
}

/* From the start (a demo chosen, or shown again): all of it at the next
 * frame
 */

void bench_demo_start(struct bench_s *b)
{
  b->t        = desk_frozen() ? FROZEN_T : 0;
  b->last     = desk_ms();
  b->lastdraw = 0;
  b->full     = true;
  b->nobj     = 0;
}

/* The kit asks for [x0, x1) x [y0, y1) of the stage (an expose): what is
 * there now, into the kit's list
 */

void bench_demo_draw(struct bench_s *b, int x0, int y0, int x1, int y1)
{
  struct bench_rect_s r = r_make(x0, y0, x1 - x0, y1 - y0);

  if (desk_frozen())
    {
      b->t = FROZEN_T;
    }

  b->inkit = true;
  b->clip  = r_and(&r, &b->stage);
  if (!r_empty(&b->clip))
    {
      paint(b);
    }

  b->inkit = false;
  b->nobj  = objs(b, b->prev);
}

/* A frame: the clock on, and what moved drawn again; false if it was not
 * time for one
 */

bool bench_demo_frame(struct bench_s *b)
{
  struct bench_rect_s changed = r_make(0, 0, 0, 0);
  uint32_t now = desk_ms();
  uint32_t dt = now - b->last;
  int n;
  int i;

  if (b->demo == DEMO_PLASMA && !b->full && now - b->lastdraw < PLASMA_MS)
    {
      return false;
    }

  b->last     = now;
  b->lastdraw = now;
  b->t       += dt > 50 ? 50 : dt;
  if (desk_frozen())
    {
      b->t = FROZEN_T;
    }

  ops_reset(b->o);
  b->from = 0;
  if (b->full || b->demo == DEMO_PLASMA)
    {
      struct bench_rect_s p = b->full ? b->stage : demo_panel(b);

      repaint(b, &p, &changed);
      b->full = false;
      b->nobj = objs(b, b->prev);
    }
  else
    {
      n = objs(b, g_cur);
      for (i = 0; i < n; i++)
        {
          struct bench_rect_s *a = i < b->nobj ? &b->prev[i] : &g_cur[i];
          struct bench_rect_s *c = &g_cur[i];
          struct bench_rect_s u;

          if (a->x == c->x && a->y == c->y && a->w == c->w &&
              a->h == c->h && i < b->nobj)
            {
              continue;
            }

          /* Where it was and where it is: one rectangle, or two when a
           * rectangle round both would be mostly neither
           */

          u = r_or(a, c);
          if ((int)u.w * u.h <= 2 * ((int)a->w * a->h + (int)c->w * c->h) +
              256)
            {
              repaint(b, &u, &changed);
            }
          else
            {
              repaint(b, a, &changed);
              repaint(b, c, &changed);
            }
        }

      memcpy(b->prev, g_cur, n * sizeof(g_cur[0]));
      b->nobj = n;
    }

  send(b);
  b->frames++;
  if (!r_empty(&changed))
    {
      kit_touched(&b->kit, changed.x, changed.y, changed.w, changed.h);
    }

  return true;
}

#endif /* CONFIG_FPCDESK_BENCH */
