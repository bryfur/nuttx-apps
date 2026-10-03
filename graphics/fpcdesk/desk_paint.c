/****************************************************************************
 * apps/graphics/fpcdesk/desk_paint.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Paint: a picture (the canvas) kept in the GPU's memory as a hidden window
 * (desk_image.c) and drawn into by the GPU; the window shows it with a
 * COPY.  Nothing but commands crosses the link while painting:
 *
 *   - brush and eraser: a dab is a BLEND of the colour through the shape
 *     table of an anti-aliased disc of the brush's size (LUT_APP, made for
 *     the size: odd sizes centred on a pixel, even ones on a corner).
 *     Dabs are laid a fifth of the size apart along the pointer's path,
 *     smoothed with quadratic curves through the midpoints of the
 *     samples, so a quick stroke at 50 Hz is still round;
 *   - line (dabs), rectangle (FILLs), ellipse (outlined with dabs; filled
 *     a row at a time: a FILL inside, its anti-aliased ends a BLEND a
 *     pixel or through a ramp), shown in the window while dragged and
 *     drawn into the canvas when let go;
 *   - fill: the canvas read back, flood-filled on the CPU, and the rows
 *     that changed put back;
 *   - the colour: a palette, a hue and lightness box drawn with ramps, and
 *     a picker that reads a pixel of the canvas;
 *   - undo and redo: before anything is drawn over a 32 x 32 tile of the
 *     canvas it is copied into a pool of tiles (another hidden window, a
 *     little larger than the canvas); undoing swaps the saved tiles with
 *     the canvas's, which makes the step one to redo.  The oldest steps go
 *     when the pool is full;
 *   - new, open (any image the viewer opens, scaled to fit 1024 x 768) and
 *     save as a 24-bit BMP.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "desk_ui.h"
#include "desk_image.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CANVAS_W       640              /* A new picture */
#define CANVAS_H       420
#define CANVAS_MAXW    1024             /* Opened ones are made to fit */
#define CANVAS_MAXH    768

#define TB_H           44               /* Tool bar */
#define ST_H           28               /* Status bar */
#define SP_W           152              /* Side panel */
#define SP_X           10               /* Its contents */
#define SP_IW          132
#define SP_MINH        460              /* The client height it needs */
#define BTN_H          28
#define MARGIN         16               /* Around the canvas in the view */

#define C_BACKDROP     RGB(0x1a, 0x1a, 0x1e)
#define C_EDGE         RGB(0x3c, 0x3c, 0x42)
#define C_PAPER        C_WHITE

#define BRUSH_MIN      1
#define BRUSH_MAX      40

/* Undo: tiles of the canvas saved in a pool of them */

#define TILE           32
#define POOL_W         1024             /* 32 tiles a row */
#define POOL_MAXT      512              /* Tiles to keep at most */
#define TEMP_N         32               /* Pool slots for swapping */
#define UNDO_MAX       16

#define FILL_TOL       28               /* Fill: a channel may differ this
                                         * much (of 255) */

/* Tools */

#define T_BRUSH        0
#define T_ERASER       1
#define T_LINE         2
#define T_RECT         3
#define T_ELLIPSE      4
#define T_FILL         5
#define T_PICK         6
#define T_COUNT        7

/* What a drag does */

#define D_NONE         0
#define D_PAINT        1
#define D_SHAPE        2
#define D_PAN          3
#define D_SIZE         4
#define D_HUE          5
#define D_PICK         6

/* Questions */

#define P_NONE         0
#define P_OPEN         1
#define P_SAVEAS       2
#define P_CLOSE        3
#define P_DISCARD      4

#define A_NONE         0                /* After P_DISCARD */
#define A_NEW          1
#define A_OPEN         2

/* Zones */

#define ZG_TOOL        0
#define ZG_SIDE        1
#define ZG_CARD        2

#define Z_NEW          1
#define Z_OPEN         2
#define Z_SAVE         3
#define Z_SAVEAS       4
#define Z_UNDO         5
#define Z_REDO         6
#define Z_OUTLINE      7
#define Z_FILLED       8
#define Z_OK           9
#define Z_CANCEL       10
#define Z_DISCARD      11
#define Z_SIZE         12
#define Z_HUEBOX       13
#define Z_TOOL0        20
#define Z_SWATCH0      40

#define NSWATCH        18

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct play_s
{
  int cw;                               /* Client width */
  int top;                              /* Below the tool bar (frame y) */
  int sy;                               /* The status bar */
  int vx;                               /* The view */
  int vy;
  int vw;
  int vh;
  int ox;                               /* The canvas's top-left in it */
  int oy;
};

/* A step to undo (or redo): canvas tiles and the pool slots they are in */

struct urec_s
{
  int n;
  int cap;
  uint16_t *tile;                       /* ty * ntx + tx */
  uint16_t *slot;
  bool over;                            /* The pool could not hold it */
};

struct paint_s
{
  char path[PATH_MAX];                  /* "": not saved yet */
  bool modified;
  struct desk_image_s *cv;              /* The canvas */

  /* Undo: hist[0 .. nundo) to undo (the last is the latest), then
   * [nundo .. nundo + nredo) to redo (the first is the next); while
   * something is drawn (inop), hist[nundo] collects its tiles
   */

  struct desk_image_s *pool;
  int spr;                              /* Slots a row */
  int nslots;
  int nextslot;
  uint8_t *used;
  uint8_t *saved;                       /* Tile saved in this step */
  int ntx;
  int nty;
  struct urec_s hist[UNDO_MAX];
  int nundo;
  int nredo;
  bool inop;

  /* Tools */

  int tool;
  int size;
  bool filled;
  uint16_t colour;

  /* The view */

  int panx;
  int pany;

  /* A drag */

  int drag;
  float lx;                             /* Stroke: the last sample */
  float ly;
  float mx;                             /* ... the midpoint before it */
  float my;
  float carry;                          /* Distance since the last dab */
  int bx0;                              /* What the canvas changed since */
  int by0;                              /* last shown */
  int bx1;
  int by1;
  int sx0;                              /* Shape: from, to (canvas) */
  int sy0;
  int sx1;
  int sy1;
  bool preview;                         /* Shown in the window: pv* */
  int pvx0;
  int pvy0;
  int pvx1;
  int pvy1;
  int grabx;                            /* Pan */
  int graby;
  int grabpx;
  int grabpy;

  /* Where the side panel put its controls (frame) */

  int sizex;
  int huex;
  int huey;
  int huew;
  int hueh;

  /* Status */

  int curx;                             /* The pointer on the canvas, -1 */
  int cury;
  char msg[160];
  bool msgerr;

  /* The pointer, the keyboard, questions */

  struct ui_zones_s zs;
  uint8_t buttons;
  int prompt;
  int after;                            /* A_* */
  bool closeafter;
  struct ui_field_s field;
  struct ui_keys_s keys;
};

/* Where operations go: the canvas (tiles saved first) or, for a shape
 * being dragged, the window (cut to the canvas)
 */

struct emit_s
{
  struct paint_s *p;
  struct desk_ops_s *o;
  NXWINDOW hwnd;
  bool canvas;
  int tx;                               /* Canvas to target coordinates */
  int ty;
  int from;                             /* Window: operations not cut yet */
  int cx0;                              /* Window: the cut */
  int cy0;
  int cx1;
  int cy1;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct desk_ops_s g_o;            /* Into the window */
static struct desk_ops_s *g_co;          /* Into a canvas (allocated: */
static struct desk_ops_s *g_po;          /* into a pool    PSRAM) */
static const char *g_openpath;

static const char * const g_toolname[T_COUNT] =
{
  "Brush", "Eraser", "Line", "Rectangle", "Ellipse", "Fill", "Colour picker"
};

static const uint16_t g_palette[NSWATCH] =
{
  RGB(0x00, 0x00, 0x00), RGB(0x55, 0x55, 0x5a), RGB(0xa8, 0xa8, 0xae),
  RGB(0xff, 0xff, 0xff), RGB(0x8a, 0x55, 0x2e), RGB(0xf4, 0xc2, 0x9a),
  RGB(0xe8, 0x2c, 0x2c), RGB(0xff, 0x8c, 0x1a), RGB(0xff, 0xd8, 0x20),
  RGB(0x3c, 0xc0, 0x4a), RGB(0x14, 0x78, 0x3c), RGB(0x2c, 0xc8, 0xd8),
  RGB(0x1e, 0x7c, 0xf0), RGB(0x1c, 0x2c, 0x8c), RGB(0x8c, 0x44, 0xe0),
  RGB(0xe0, 0x44, 0xb4), RGB(0xff, 0x9c, 0xc8), RGB(0x9c, 0xe0, 0xa0)
};

/* The hue box's corners of the colour cube, R, G, B each */

static const uint8_t g_hues[7 * 3] =
{
  255, 0, 0,   255, 255, 0,   0, 255, 0,   0, 255, 255,
  0, 0, 255,   255, 0, 255,   255, 0, 0
};

#define HUE(k, n)      g_hues[(k) * 3 + (n)]

/****************************************************************************
 * Private Functions: arithmetic
 ****************************************************************************/

static int ifloor(float v)
{
  int i = (int)v;

  return v < i ? i - 1 : i;
}

static int iceil(float v)
{
  int i = (int)v;

  return v > i ? i + 1 : i;
}

/* No libm: a first guess from the exponent, then Newton */

static float fsqrt(float v)
{
  union
  {
    float f;
    uint32_t i;
  } u;

  float r;

  if (v <= 0.0f)
    {
      return 0.0f;
    }

  u.f = v;
  u.i = 0x1fbd1df5 + (u.i >> 1);
  r = u.f;
  r = 0.5f * (r + v / r);
  r = 0.5f * (r + v / r);
  r = 0.5f * (r + v / r);
  return r;
}

static int clampi(int v, int lo, int hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

static void rgb_of(uint16_t c, int *r, int *g, int *b)
{
  *r = (c >> 11) << 3 | (c >> 13);
  *g = ((c >> 5) & 63) << 2 | ((c >> 9) & 3);
  *b = (c & 31) << 3 | ((c >> 2) & 7);
}

static const char *base_name(const char *path)
{
  const char *s = strrchr(path, '/');

  return s != NULL ? s + 1 : path;
}

static const char *home_dir(void)
{
  const char *home = getenv("HOME");

  if (home == NULL || strcmp(home, "/") == 0)
    {
      home = access("/mnt/sd0", F_OK) == 0 ? "/mnt/sd0" : "/tmp";
    }

  return home;
}

/****************************************************************************
 * Private Functions: the brush's shape table
 ****************************************************************************/

/* A disc of diameter d: entry [ddy][ddx] is the coverage of the pixel that
 * far from the inner rectangle.  Odd sizes: the inner rectangle is the
 * centre pixel, so pixel ddx spans [ddx - 1/2, ddx + 1/2) from the centre;
 * even ones: it is empty, at a pixel corner, and pixel ddx spans
 * [ddx - 1, ddx).  8 x 8 samples a pixel.  Size 1 is a pencil.
 */

/* (1 - c)^k for k a whole or half number */

static float fpowk(float v, int k2)
{
  float r = (k2 & 1) ? fsqrt(v) : 1.0f;
  int i;

  for (i = 0; i < k2 / 2; i++)
    {
      r *= v;
    }

  return r;
}

static void brush_make(uint8_t lut[NX_BLEND_LUT_N][NX_BLEND_LUT_N], int d)
{
  float o = (d & 1) ? -0.5f : -1.0f;
  float rr = d * d / 4.0f;
  float s = d / 5.0f < 1.0f ? 1.0f : d / 5.0f;
  int lim = d / 2 + 2;
  int k2;
  int dx;
  int dy;
  int i;
  int j;

  /* A pixel of the edge is under about k = 2 sqrt(r) / spacing dabs of a
   * stroke, which add up (1 - (1 - c)^k): each dab's coverage c is made
   * c' such that k of them come to c, so strokes keep soft edges
   */

  k2 = (int)(2.0f * 2.0f * fsqrt(d / 2.0f) / s + 0.5f);
  k2 = k2 < 2 ? 2 : k2 > 8 ? 8 : k2;

  memset(lut, 0, NX_BLEND_LUT_N * NX_BLEND_LUT_N);
  if (d == 1)
    {
      lut[0][0] = 255;
      return;
    }

  for (dy = 0; dy < lim && dy < NX_BLEND_LUT_N; dy++)
    {
      for (dx = 0; dx < lim && dx < NX_BLEND_LUT_N; dx++)
        {
          int n = 0;

          for (j = 0; j < 8; j++)
            {
              float v = dy + o + (j + 0.5f) / 8.0f;

              for (i = 0; i < 8; i++)
                {
                  float u = dx + o + (i + 0.5f) / 8.0f;

                  n += u * u + v * v <= rr;
                }
            }

          if (n > 0 && n < 64 && k2 > 2)
            {
              float c = n / 64.0f;
              float lo = 0.0f;
              float hi = c;

              /* 1 - (1 - c')^k = c, by halving */

              for (i = 0; i < 16; i++)
                {
                  float m = (lo + hi) / 2.0f;

                  if (1.0f - fpowk(1.0f - m, k2) < c)
                    {
                      lo = m;
                    }
                  else
                    {
                      hi = m;
                    }
                }

              lut[dy][dx] = (uint8_t)(lo * 255.0f + 0.5f);
              lut[dy][dx] += lut[dy][dx] == 0;
            }
          else
            {
              lut[dy][dx] = (uint8_t)((n * 255 + 32) / 64);
            }
        }
    }
}

static void brush_use(int d)
{
  desk_lut_app(0x50000 | d, brush_make, d);
}

/* A dab of diameter d at (px, py) (1/16 pixel), for the table
 * brush_use(d) loaded
 */

static struct nx_gpuop_s *dab_op(struct desk_ops_s *o, int px, int py,
                                 int d, uint16_t c)
{
  struct nx_gpuop_s *op = ops_add(o);
  int cx;
  int cy;

  op->op     = NX_GPUOP_BLEND;
  op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_LUT;
  op->slot   = LUT_APP;
  op->colour = c;
  op->w      = d;
  op->h      = d;
  if (d & 1)
    {
      cx = px >> 4;
      cy = py >> 4;
      op->p[0] = op->p[2] = cx;
      op->p[1] = op->p[3] = cy;
    }
  else
    {
      cx = (px + 8) >> 4;
      cy = (py + 8) >> 4;
      op->p[0] = cx;
      op->p[1] = cy;
      op->p[2] = cx - 1;
      op->p[3] = cy - 1;
    }

  op->x = cx - d / 2;
  op->y = cy - d / 2;
  return op;
}

/****************************************************************************
 * Private Functions: undo
 ****************************************************************************/

static void rec_free(struct paint_s *p, struct urec_s *r)
{
  int i;

  for (i = 0; i < r->n; i++)
    {
      p->used[r->slot[i]] = 0;
    }

  free(r->tile);
  free(r->slot);
  memset(r, 0, sizeof(*r));
}

static int nrecs(struct paint_s *p)
{
  return p->nundo + p->nredo + (p->inop ? 1 : 0);
}

/* The oldest step goes */

static void drop_oldest(struct paint_s *p)
{
  int n = nrecs(p);

  rec_free(p, &p->hist[0]);
  memmove(&p->hist[0], &p->hist[1], (n - 1) * sizeof(struct urec_s));
  memset(&p->hist[n - 1], 0, sizeof(struct urec_s));
  p->nundo--;
}

static void history_clear(struct paint_s *p)
{
  int n = nrecs(p);
  int i;

  for (i = 0; i < n; i++)
    {
      rec_free(p, &p->hist[i]);
    }

  p->nundo = 0;
  p->nredo = 0;
}

static void pool_free(struct paint_s *p)
{
  if (p->used != NULL)
    {
      history_clear(p);
    }

  p->inop = false;
  p->nundo = 0;
  p->nredo = 0;
  img_free(p->pool);
  p->pool = NULL;
  free(p->used);
  free(p->saved);
  p->used = NULL;
  p->saved = NULL;
  p->nslots = 0;
}

/* A pool for the canvas: as many tiles as it has (at most POOL_MAXT) and
 * a few more, and TEMP_N for swapping.  Without one (no memory) nothing
 * can be undone.
 */

static void pool_alloc(struct desk_win_s *w, struct paint_s *p)
{
  int tiles;
  int rows;

  pool_free(p);
  if (p->cv == NULL)
    {
      return;
    }

  p->ntx = (p->cv->w + TILE - 1) / TILE;
  p->nty = (p->cv->h + TILE - 1) / TILE;
  tiles = p->ntx * p->nty + 32;
  tiles = (tiles > POOL_MAXT ? POOL_MAXT : tiles) + TEMP_N;
  rows = (tiles + POOL_W / TILE - 1) / (POOL_W / TILE);

  p->saved = calloc(p->ntx * p->nty, 1);
  p->pool = img_new(POOL_W, rows * TILE, 0, w);
  if (p->pool == NULL || p->saved == NULL)
    {
      pool_free(p);
      return;
    }

  p->spr = p->pool->w / TILE;
  p->nslots = p->spr * (p->pool->h / TILE);
  p->used = p->nslots > TEMP_N + 4 ? calloc(p->nslots, 1) : NULL;
  if (p->used == NULL)
    {
      pool_free(p);
      return;
    }

  p->nextslot = TEMP_N;
}

static int slot_alloc(struct paint_s *p)
{
  int i;

  for (i = 0; i < p->nslots - TEMP_N; i++)
    {
      int s = p->nextslot;

      p->nextslot = s + 1 < p->nslots ? s + 1 : TEMP_N;
      if (!p->used[s])
        {
          p->used[s] = 1;
          return s;
        }
    }

  return -1;
}

static void tile_rect(struct paint_s *p, int t, int *x, int *y, int *w,
                      int *h)
{
  *x = t % p->ntx * TILE;
  *y = t / p->ntx * TILE;
  *w = p->cv->w - *x < TILE ? p->cv->w - *x : TILE;
  *h = p->cv->h - *y < TILE ? p->cv->h - *y : TILE;
}

static void slot_xy(struct paint_s *p, int s, int *x, int *y)
{
  *x = s % p->spr * TILE;
  *y = s / p->spr * TILE;
}

/* A COPY into dst (src NULL: from dst itself) */

static void copy_op(struct desk_ops_s *o, NXWINDOW dst, NXWINDOW src,
                    int sx, int sy, int x, int y, int w, int h)
{
  struct nx_gpuop_s *op;

  ui_flush(dst, o, 1);
  op = ops_add(o);
  op->op = NX_GPUOP_COPY;
  op->src = src;
  op->sx = sx;
  op->sy = sy;
  op->x = x;
  op->y = y;
  op->w = w;
  op->h = h;
}

/* Something is about to be drawn: a new step (what could be redone goes) */

static void op_begin(struct paint_s *p)
{
  int i;

  for (i = p->nundo; i < p->nundo + p->nredo; i++)
    {
      rec_free(p, &p->hist[i]);
    }

  p->nredo = 0;
  if (p->nundo == UNDO_MAX)
    {
      drop_oldest(p);
    }

  memset(&p->hist[p->nundo], 0, sizeof(struct urec_s));
  if (p->saved != NULL)
    {
      memset(p->saved, 0, p->ntx * p->nty);
    }

  p->inop = true;
}

/* The canvas rectangle [x0, x1) x [y0, y1) is about to change: the tiles
 * of it not saved yet in this step are copied into the pool (queued in
 * g_po, which goes before the canvas's operations)
 */

static void save_rect(struct paint_s *p, int x0, int y0, int x1, int y1)
{
  struct urec_s *r = &p->hist[p->nundo];
  int tx;
  int ty;

  if (!p->inop)
    {
      return;
    }

  if (p->pool == NULL)
    {
      r->over = true;
      return;
    }

  x0 = x0 < 0 ? 0 : x0;
  y0 = y0 < 0 ? 0 : y0;
  x1 = x1 > p->cv->w ? p->cv->w : x1;
  y1 = y1 > p->cv->h ? p->cv->h : y1;
  if (x0 >= x1 || y0 >= y1)
    {
      return;
    }

  for (ty = y0 / TILE; ty <= (y1 - 1) / TILE; ty++)
    {
      for (tx = x0 / TILE; tx <= (x1 - 1) / TILE; tx++)
        {
          int t = ty * p->ntx + tx;
          int s;
          int x;
          int y;
          int w;
          int h;
          int px;
          int py;

          if (p->saved[t] || r->over)
            {
              continue;
            }

          p->saved[t] = 1;
          s = slot_alloc(p);
          while (s < 0 && p->nundo > 0)
            {
              drop_oldest(p);
              r = &p->hist[p->nundo];
              s = slot_alloc(p);
            }

          if (s >= 0 && r->n == r->cap)
            {
              int cap = r->cap + 64;
              uint16_t *nt = realloc(r->tile, cap * sizeof(uint16_t));
              uint16_t *ns = NULL;

              if (nt != NULL)
                {
                  r->tile = nt;
                  ns = realloc(r->slot, cap * sizeof(uint16_t));
                }

              if (ns != NULL)
                {
                  r->slot = ns;
                  r->cap = cap;
                }
              else
                {
                  p->used[s] = 0;
                  s = -1;
                }
            }

          if (s < 0)
            {
              r->over = true;
              continue;
            }

          r->tile[r->n] = t;
          r->slot[r->n++] = s;
          tile_rect(p, t, &x, &y, &w, &h);
          slot_xy(p, s, &px, &py);
          copy_op(g_po, p->pool->hwnd, p->cv->hwnd, x, y, px, py, w, h);
        }
    }
}

/* The canvas's operations, after the tiles they cover are saved */

static void canvas_flush(struct paint_s *p)
{
  if (p->pool != NULL)
    {
      ops_draw(p->pool->hwnd, g_po);
    }

  ops_draw(p->cv->hwnd, g_co);
}

/* The step is drawn: kept, or, if the pool could not hold it, the history
 * is lost (the steps before it cannot be undone without it)
 */

static void op_end(struct paint_s *p)
{
  struct urec_s *r = &p->hist[p->nundo];

  if (!p->inop)
    {
      return;
    }

  canvas_flush(p);
  p->modified = true;
  if (r->over)
    {
      p->nundo++;
      p->inop = false;
      history_clear(p);
      snprintf(p->msg, sizeof(p->msg), p->pool == NULL ?
               "No memory to undo with" :
               "Too large to undo: the undo history is cleared");
      p->msgerr = false;
    }
  else if (r->n > 0)
    {
      p->nundo++;
      p->inop = false;
    }
  else
    {
      rec_free(p, r);
      p->inop = false;
    }
}

/* Undo or redo r: its tiles and the canvas's change places, TEMP_N at a
 * time through the pool's first slots.  Gives the canvas rectangle.
 */

static void rec_swap(struct paint_s *p, struct urec_s *r, int *bx0,
                     int *by0, int *bx1, int *by1)
{
  NXWINDOW pool = p->pool->hwnd;
  NXWINDOW cv = p->cv->hwnd;
  int i0;
  int k;

  *bx0 = *by0 = INT_MAX;
  *bx1 = *by1 = INT_MIN;
  for (i0 = 0; i0 < r->n; i0 += TEMP_N)
    {
      int n = r->n - i0 < TEMP_N ? r->n - i0 : TEMP_N;
      int x;
      int y;
      int w;
      int h;
      int px;
      int py;
      int qx;
      int qy;

      for (k = 0; k < n; k++)
        {
          tile_rect(p, r->tile[i0 + k], &x, &y, &w, &h);
          slot_xy(p, k, &qx, &qy);
          copy_op(g_po, pool, cv, x, y, qx, qy, w, h);
        }

      ops_draw(pool, g_po);
      for (k = 0; k < n; k++)
        {
          tile_rect(p, r->tile[i0 + k], &x, &y, &w, &h);
          slot_xy(p, r->slot[i0 + k], &px, &py);
          copy_op(g_co, cv, pool, px, py, x, y, w, h);
          *bx0 = x < *bx0 ? x : *bx0;
          *by0 = y < *by0 ? y : *by0;
          *bx1 = x + w > *bx1 ? x + w : *bx1;
          *by1 = y + h > *by1 ? y + h : *by1;
        }

      ops_draw(cv, g_co);
      for (k = 0; k < n; k++)
        {
          tile_rect(p, r->tile[i0 + k], &x, &y, &w, &h);
          slot_xy(p, k, &qx, &qy);
          slot_xy(p, r->slot[i0 + k], &px, &py);
          copy_op(g_po, pool, NULL, qx, qy, px, py, w, h);
        }

      ops_draw(pool, g_po);
    }
}

/****************************************************************************
 * Private Functions: layout and the view
 ****************************************************************************/

static void layout(struct desk_win_s *w, struct play_s *l)
{
  struct paint_s *p = w->priv;
  int cw;
  int ch;

  l->cw  = w->csize.w;
  l->top = TITLE_H + TB_H;
  l->sy  = TITLE_H + w->csize.h - ST_H;
  l->vx  = SP_W;
  l->vy  = l->top;
  l->vw  = l->cw - SP_W > 1 ? l->cw - SP_W : 1;
  l->vh  = l->sy - l->top > 1 ? l->sy - l->top : 1;
  l->ox  = l->vx;
  l->oy  = l->vy;
  if (p->cv == NULL)
    {
      return;
    }

  /* Centred if it fits, else panned */

  cw = p->cv->w;
  ch = p->cv->h;
  if (cw + 2 * MARGIN <= l->vw)
    {
      p->panx = 0;
      l->ox = l->vx + (l->vw - cw) / 2;
    }
  else
    {
      p->panx = clampi(p->panx, 0, cw + 2 * MARGIN - l->vw);
      l->ox = l->vx + MARGIN - p->panx;
    }

  if (ch + 2 * MARGIN <= l->vh)
    {
      p->pany = 0;
      l->oy = l->vy + (l->vh - ch) / 2;
    }
  else
    {
      p->pany = clampi(p->pany, 0, ch + 2 * MARGIN - l->vh);
      l->oy = l->vy + MARGIN - p->pany;
    }
}

/* The part of the canvas the view shows (frame) */

static void canvas_vis(struct paint_s *p, struct play_s *l, int *x0,
                       int *y0, int *x1, int *y1)
{
  *x0 = l->ox > l->vx ? l->ox : l->vx;
  *y0 = l->oy > l->vy ? l->oy : l->vy;
  *x1 = l->ox + p->cv->w < l->vx + l->vw ? l->ox + p->cv->w : l->vx + l->vw;
  *y1 = l->oy + p->cv->h < l->vy + l->vh ? l->oy + p->cv->h : l->vy + l->vh;
}

/* A COPY of the canvas rectangle [x0, x1) x [y0, y1) to the window (queued
 * in g_o)
 */

static void show_rect(struct desk_win_s *w, struct play_s *l, int x0,
                      int y0, int x1, int y1)
{
  struct paint_s *p = w->priv;
  int vx0;
  int vy0;
  int vx1;
  int vy1;

  canvas_vis(p, l, &vx0, &vy0, &vx1, &vy1);
  x0 = x0 + l->ox > vx0 ? x0 + l->ox : vx0;
  y0 = y0 + l->oy > vy0 ? y0 + l->oy : vy0;
  x1 = x1 + l->ox < vx1 ? x1 + l->ox : vx1;
  y1 = y1 + l->oy < vy1 ? y1 + l->oy : vy1;
  if (x0 < x1 && y0 < y1)
    {
      copy_op(&g_o, (NXWINDOW)w->hwnd, p->cv->hwnd, x0 - l->ox, y0 - l->oy,
              x0, y0, x1 - x0, y1 - y0);
    }
}

static void view_draw(struct desk_win_s *w, struct play_s *l)
{
  struct paint_s *p = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  int from;

  ui_flush(hwnd, &g_o, 8);
  from = g_o.n;
  ops_fill(&g_o, l->vx, l->vy, l->vw, l->vh, C_BACKDROP);
  if (p->cv != NULL)
    {
      ops_fill(&g_o, l->ox - 1, l->oy - 1, p->cv->w + 2, p->cv->h + 2,
               C_EDGE);
      ui_clip(&g_o, from, l->vx, l->vy, l->vx + l->vw, l->vy + l->vh);
      show_rect(w, l, 0, 0, p->cv->w, p->cv->h);
    }

  p->preview = false;
  ops_draw(hwnd, &g_o);
}

/****************************************************************************
 * Private Functions: the tool bar, the side panel, the status bar
 ****************************************************************************/

static int tb_button(struct paint_s *p, int x, int y, int id,
                     const char *label, bool enabled, bool lit)
{
  int bw = ui_button_width(label);
  int lk = ui_look(&p->zs, id, enabled);

  if (lit && lk == UI_NORMAL)
    {
      lk = UI_DEFAULT;
    }

  ui_button(&g_o, x, y, bw, BTN_H, label, lk);
  ui_zone_add(&p->zs, ZG_TOOL, id, x, y, bw, BTN_H, enabled);
  return x + bw + 6;
}

static void toolbar_draw(struct desk_win_s *w, struct play_s *l)
{
  struct paint_s *p = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  const struct desk_font_s *font = &g_desk_font_ui;
  int y = TITLE_H + (TB_H - BTN_H) / 2;
  int x = 10;
  char fit[96];

  ui_zones_clear(&p->zs, ZG_TOOL);
  ui_flush(hwnd, &g_o, 140);
  ops_fill(&g_o, 0, TITLE_H, l->cw, TB_H, C_UI_BAR);
  ops_fill(&g_o, 0, TITLE_H + TB_H - 1, l->cw, 1, C_UI_LINE);
  x = tb_button(p, x, y, Z_NEW, "New", true, false);
  x = tb_button(p, x, y, Z_OPEN, "Open" DESK_CH_ELLIPSIS, true, false);
  x = tb_button(p, x, y, Z_SAVE, "Save", p->cv != NULL, p->modified);
  x = tb_button(p, x, y, Z_SAVEAS, "Save As" DESK_CH_ELLIPSIS,
                p->cv != NULL, false);
  x += 10;
  x = tb_button(p, x, y, Z_UNDO, "Undo", p->nundo > 0, false);
  x = tb_button(p, x, y, Z_REDO, "Redo", p->nredo > 0, false);
  x += 10;
  ui_fit(font, p->path[0] != '\0' ? p->path : "Not saved yet",
         l->cw - x - 12, fit, sizeof(fit));
  ops_text(&g_o, font, x, TITLE_H + (TB_H - text_height(font)) / 2, fit,
           C_TEXT_DIM, 255);
  ops_draw(hwnd, &g_o);
}

static uint16_t look_bg(int look)
{
  switch (look)
    {
      case UI_HOVER:
        return C_UI_HOVER;

      case UI_PRESSED:
        return C_UI_PRESSED;

      case UI_DEFAULT:
        return C_ACCENT;

      default:
        return C_UI_BUTTON;
    }
}

/* A tool's picture on its 30 x 30 button */

static void tool_icon(struct desk_ops_s *o, int t, int x, int y,
                      uint16_t fg, uint16_t bg)
{
  int k;

  switch (t)
    {
      case T_BRUSH:
        for (k = 0; k < 9; k++)
          {
            ops_blend(o, x + 17 + k / 2, y + 13 - k, 2, 2, fg, 200);
          }

        ops_disc(o, x + 13, y + 18, LUT_R6, 6, fg, 255);
        break;

      case T_ERASER:
        ops_rrect(o, x + 6, y + 9, 18, 12, LUT_R3, 3,
                  RGB(0xf4, 0x9a, 0xb0), 255);
        ops_rrect(o, x + 6, y + 9, 6, 12, LUT_R3, 3, C_WHITE, 255);
        ops_fill(o, x + 11, y + 9, 1, 12, bg);
        break;

      case T_LINE:
        for (k = 0; k < 14; k++)
          {
            ops_blend(o, x + 8 + k, y + 20 - k, 2, 2, fg, 255);
          }
        break;

      case T_RECT:
        ops_fill(o, x + 7, y + 9, 16, 2, fg);
        ops_fill(o, x + 7, y + 19, 16, 2, fg);
        ops_fill(o, x + 7, y + 11, 2, 8, fg);
        ops_fill(o, x + 21, y + 11, 2, 8, fg);
        break;

      case T_ELLIPSE:
        ops_rrect(o, x + 5, y + 8, 20, 14, LUT_R6, 6, fg, 255);
        ops_rrect(o, x + 7, y + 10, 16, 10, LUT_R3, 3, bg, 255);
        break;

      case T_FILL:
        ops_disc(o, x + 15, y + 17, LUT_R6, 6, fg, 255);
        ui_triangle(o, x + 15, y + 10, 5, 0, fg, 255);
        break;

      default:
        for (k = 0; k < 10; k++)
          {
            ops_blend(o, x + 8 + k, y + 20 - k, 2, 2, fg, 255);
          }

        ops_disc(o, x + 20, y + 10, LUT_R3, 3, fg, 255);
        break;
    }
}

/* The hue and lightness box: hues across (six ramps), white fading down
 * to the middle, black fading in below it
 */

static void huebox_draw(struct desk_ops_s *o, int x, int y, int w, int h)
{
  int h1 = h / 2;
  int h2 = h - h1;
  struct nx_gpuop_s *op;
  int k;

  for (k = 0; k < 6; k++)
    {
      int x0 = x + k * w / 6;
      int x1 = x + (k + 1) * w / 6;

      ops_hgrad(o, x0, y, x1 - x0, h,
                RGB(HUE(k, 0), HUE(k, 1), HUE(k, 2)),
                RGB(HUE(k + 1, 0), HUE(k + 1, 1), HUE(k + 1, 2)),
                255);
    }

  op = ops_add(o);
  op->op     = NX_GPUOP_BLEND;
  op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_VRAMP | NX_BLEND_DITHER;
  op->colour = C_WHITE;
  op->x      = x;
  op->y      = y;
  op->w      = w;
  op->h      = h1;
  op->p[0]   = y;
  op->p[1]   = -(255 * 256) / (h1 - 1);
  op->p[2]   = (int16_t)(uint16_t)(255 * 256);

  op = ops_add(o);
  op->op     = NX_GPUOP_BLEND;
  op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_VRAMP | NX_BLEND_DITHER;
  op->colour = C_BLACK;
  op->x      = x;
  op->y      = y + h1;
  op->w      = w;
  op->h      = h2;
  op->p[0]   = y + h1;
  op->p[1]   = (255 * 256) / h2;
  op->p[2]   = (255 * 256) / h2;
}

/* The colour at (i, j) of the box, as it was drawn */

static uint16_t huebox_colour(int i, int j, int w, int h)
{
  int h1 = h / 2;
  int h2 = h - h1;
  int k = 0;
  int x0;
  int x1;
  int m;
  int a;
  int c[3];
  int n;

  while (k < 5 && i >= (k + 1) * w / 6)
    {
      k++;
    }

  x0 = k * w / 6;
  x1 = (k + 1) * w / 6;
  m = x1 - x0 > 1 ? ((i - x0) * ((255 * 256) / (x1 - x0 - 1))) >> 8 : 0;
  m = clampi(m, 0, 255);
  for (n = 0; n < 3; n++)
    {
      c[n] = HUE(k, n) + (HUE(k + 1, n) - HUE(k, n)) * m / 255;
    }

  if (j < h1)
    {
      a = clampi((255 * 256 - j * ((255 * 256) / (h1 - 1))) >> 8, 0, 255);
      for (n = 0; n < 3; n++)
        {
          c[n] += (255 - c[n]) * a / 255;
        }
    }
  else
    {
      a = clampi(((j - h1 + 1) * ((255 * 256) / h2)) >> 8, 0, 255);
      for (n = 0; n < 3; n++)
        {
          c[n] = c[n] * (255 - a) / 255;
        }
    }

  return RGB(c[0], c[1], c[2]);
}

/* Operations of the side panel from from on: cut to it and sent */

static void side_send(struct desk_win_s *w, struct play_s *l, int from)
{
  ui_clip(&g_o, from, 0, l->top, SP_W, l->sy);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
}

static void side_draw(struct desk_win_s *w, struct play_s *l)
{
  struct paint_s *p = w->priv;
  const struct desk_font_s *font = &g_desk_font_ui;
  int x0 = SP_X;
  int y = l->top + 10;
  char buf[32];
  int cr;
  int cg;
  int cb;
  int i;
  int k;

  ui_zones_clear(&p->zs, ZG_SIDE);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
  brush_use(p->size);
  ops_fill(&g_o, 0, l->top, SP_W - 1, l->sy - l->top, C_UI_BAR);
  ops_fill(&g_o, SP_W - 1, l->top, 1, l->sy - l->top, C_UI_LINE);

  /* The tools, four a row */

  for (i = 0; i < T_COUNT; i++)
    {
      int bx = x0 + i % 4 * 34;
      int by = y + i / 4 * 34;
      int lk = ui_look(&p->zs, Z_TOOL0 + i, true);

      if (p->tool == i && lk == UI_NORMAL)
        {
          lk = UI_DEFAULT;
        }

      ui_button(&g_o, bx, by, 30, 30, NULL, lk);
      tool_icon(&g_o, i, bx, by, lk == UI_DEFAULT ? C_WHITE : C_TEXT,
                look_bg(lk));
      ui_zone_add(&p->zs, ZG_SIDE, Z_TOOL0 + i, bx, by, 30, 30, true);
    }

  y += 68 + 6;

  /* Shapes: outlined or filled */

  for (i = 0; i < 2; i++)
    {
      int id = i == 0 ? Z_OUTLINE : Z_FILLED;
      int lk = ui_look(&p->zs, id, true);

      if (p->filled == (i == 1) && lk == UI_NORMAL)
        {
          lk = UI_DEFAULT;
        }

      ui_button(&g_o, x0 + i * 68, y, 64, 24, i == 0 ? "Outline" : "Filled",
                lk);
      ui_zone_add(&p->zs, ZG_SIDE, id, x0 + i * 68, y, 64, 24, true);
    }

  y += 24 + 12;
  side_send(w, l, 0);

  /* The size: the brush itself on paper, and a slider */

  ops_rrect(&g_o, x0, y, 44, 44, LUT_R6, 6, C_PAPER, 255);
  dab_op(&g_o, (x0 + 22) * 16 + 8, (y + 22) * 16 + 8, p->size,
         p->tool == T_ERASER ? C_TEXT_DIM : p->colour);
  ops_text(&g_o, font, x0 + 56, y + 4, "Size", C_TEXT, 255);
  snprintf(buf, sizeof(buf), "%d px", p->size);
  ops_text(&g_o, font, x0 + 56, y + 22, buf, C_TEXT_DIM, 255);
  y += 44 + 6;
  k = x0 + 7 + (p->size - BRUSH_MIN) * (SP_IW - 14) /
               (BRUSH_MAX - BRUSH_MIN);
  ops_fill(&g_o, x0 + 7, y + 9, SP_IW - 14, 3, C_UI_HOVER);
  ops_fill(&g_o, x0 + 7, y + 9, k - x0 - 7, 3, C_ACCENT);
  ops_disc(&g_o, k, y + 10, LUT_R6, 6, ui_look(&p->zs, Z_SIZE, true) ==
           UI_NORMAL ? C_TEXT : C_WHITE, 255);
  ui_zone_add(&p->zs, ZG_SIDE, Z_SIZE, x0, y, SP_IW, 20, true);
  p->sizex = x0;
  y += 20 + 10;

  /* The palette: three rows of six */

  for (i = 0; i < NSWATCH; i++)
    {
      int sx = x0 + i % 6 * 22 + 1;
      int sy = y + i / 6 * 22;

      if (p->colour == g_palette[i] || p->zs.hot == Z_SWATCH0 + i)
        {
          ops_rrect(&g_o, sx - 2, sy - 2, 23, 23, LUT_R3, 3,
                    p->colour == g_palette[i] ? C_WHITE : C_TEXT_DIM, 255);
          ops_rrect(&g_o, sx - 1, sy - 1, 21, 21, LUT_R3, 3, C_UI_BAR, 255);
        }

      ops_rrect(&g_o, sx, sy, 19, 19, LUT_R3, 3, g_palette[i], 255);
      ui_zone_add(&p->zs, ZG_SIDE, Z_SWATCH0 + i, sx - 1, sy - 1, 21, 21,
                  true);
    }

  y += 66 + 6;
  side_send(w, l, 0);

  /* Any colour */

  huebox_draw(&g_o, x0, y, SP_IW, 64);
  ui_zone_add(&p->zs, ZG_SIDE, Z_HUEBOX, x0, y, SP_IW, 64, true);
  p->huex = x0;
  p->huey = y;
  p->huew = SP_IW;
  p->hueh = 64;
  y += 64 + 10;

  /* The colour */

  rgb_of(p->colour, &cr, &cg, &cb);
  snprintf(buf, sizeof(buf), "#%02X%02X%02X", cr, cg, cb);
  ops_rrect(&g_o, x0 - 1, y - 1, SP_IW + 2, 28, LUT_R6, 6, C_UI_LINE, 255);
  ops_rrect(&g_o, x0, y, SP_IW, 26, LUT_R6, 6, p->colour, 255);
  ops_text_centre(&g_o, font, x0 + SP_IW / 2,
                  y + (26 - text_height(font)) / 2, buf,
                  cr * 3 + cg * 6 + cb > 1400 ? C_BLACK : C_WHITE, 255);
  side_send(w, l, 0);
}

static void status_draw(struct desk_win_s *w, struct play_s *l)
{
  struct paint_s *p = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  const struct desk_font_s *font = &g_desk_font_ui;
  int ty = l->sy + (ST_H - text_height(font)) / 2;
  char left[192];
  char right[64];
  char fit[192];
  size_t n;
  int rw;

  right[0] = '\0';
  if (p->cv != NULL)
    {
      snprintf(right, sizeof(right), "%d x %d", p->cv->w, p->cv->h);
      if (p->curx >= 0)
        {
          n = strlen(right);
          snprintf(right + n, sizeof(right) - n, "    %d, %d", p->curx,
                   p->cury);
        }
    }

  if (p->msg[0] != '\0')
    {
      strlcpy(left, p->msg, sizeof(left));
    }
  else
    {
      snprintf(left, sizeof(left), "%s", g_toolname[p->tool]);
      n = strlen(left);
      if (p->tool <= T_LINE ||
          ((p->tool == T_RECT || p->tool == T_ELLIPSE) && !p->filled))
        {
          snprintf(left + n, sizeof(left) - n, "  " DESK_CH_MIDDOT "  %d px",
                   p->size);
          n = strlen(left);
        }

      if (p->tool == T_RECT || p->tool == T_ELLIPSE)
        {
          snprintf(left + n, sizeof(left) - n, "  " DESK_CH_MIDDOT "  %s",
                   p->filled ? "filled" : "outline");
        }
    }

  ui_flush(hwnd, &g_o, 140);
  ops_fill(&g_o, 0, l->sy, l->cw, ST_H, C_UI_BAR);
  ops_fill(&g_o, 0, l->sy, l->cw, 1, C_UI_LINE);
  rw = text_width(font, right);
  ops_text(&g_o, font, l->cw - 14 - rw, ty, right, C_TEXT_DIM, 255);
  ui_fit(font, left, l->cw - 40 - rw, fit, sizeof(fit));
  ops_text(&g_o, font, 12, ty, fit, p->msgerr ? C_UI_ERROR : C_TEXT_DIM,
           255);
  ops_draw(hwnd, &g_o);
}

static void set_msg(struct desk_win_s *w, bool err, const char *fmt,
                    const char *arg)
{
  struct paint_s *p = w->priv;
  struct play_s l;

  snprintf(p->msg, sizeof(p->msg), fmt, arg);
  p->msgerr = err;
  layout(w, &l);
  status_draw(w, &l);
}

/* The question, on a card over the middle of the view */

static void card_draw(struct desk_win_s *w, struct play_s *l)
{
  struct paint_s *p = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  int cw = 440 < l->vw - 24 ? 440 : l->vw - 24;
  int ch = 136;
  int x = l->vx + (l->vw - cw) / 2;
  int y = l->vy + (l->vh - ch) / 2;
  int bx = x + cw - 16;
  int by;
  char buf[96];
  char fit[96];
  const char *ok;
  int bw;
  int lk;

  ui_zones_clear(&p->zs, ZG_CARD);
  if (p->prompt == P_NONE)
    {
      return;
    }

  y = y < l->vy + 4 ? l->vy + 4 : y;
  by = y + ch - 44;
  ops_draw(hwnd, &g_o);
  ops_rrect(&g_o, x - 1, y - 1, cw + 2, ch + 2, LUT_R12, 12, C_UI_LINE, 255);
  ops_rrect(&g_o, x, y, cw, ch, LUT_R12, 12, C_CARD, 255);

  if (p->prompt == P_CLOSE || p->prompt == P_DISCARD)
    {
      snprintf(buf, sizeof(buf), p->prompt == P_CLOSE ?
               "Save changes to \"%s\"?" : "Discard changes to \"%s\"?",
               p->path[0] != '\0' ? base_name(p->path) : "Untitled");
      ui_fit(&g_desk_font_uib, buf, cw - 36, fit, sizeof(fit));
      ops_text(&g_o, &g_desk_font_uib, x + 18, y + 18, fit, C_TEXT, 255);
      ops_text(&g_o, &g_desk_font_ui, x + 18, y + 44,
               p->prompt == P_CLOSE ? "If you don't save, the changes are "
               "lost." : "The picture has not been saved.", C_TEXT_DIM,
               255);

      if (p->prompt == P_CLOSE)
        {
          bw = ui_button_width("Save");
          bx -= bw;
          ui_button(&g_o, bx, by, bw, BTN_H, "Save",
                    ui_look(&p->zs, Z_OK, true) == UI_PRESSED ?
                    UI_PRESSED : UI_DEFAULT);
          ui_zone_add(&p->zs, ZG_CARD, Z_OK, bx, by, bw, BTN_H, true);
        }
      else
        {
          bw = ui_button_width("Discard");
          bx -= bw;
          lk = ui_look(&p->zs, Z_DISCARD, true);
          ui_button(&g_o, bx, by, bw, BTN_H, "Discard",
                    lk == UI_NORMAL ? UI_DANGER : lk);
          ui_zone_add(&p->zs, ZG_CARD, Z_DISCARD, bx, by, bw, BTN_H, true);
        }

      bw = ui_button_width("Cancel");
      bx -= bw + 8;
      ui_button(&g_o, bx, by, bw, BTN_H, "Cancel",
                ui_look(&p->zs, Z_CANCEL, true));
      ui_zone_add(&p->zs, ZG_CARD, Z_CANCEL, bx, by, bw, BTN_H, true);

      if (p->prompt == P_CLOSE)
        {
          bw = ui_button_width("Don't Save");
          lk = ui_look(&p->zs, Z_DISCARD, true);
          ui_button(&g_o, x + 16, by, bw, BTN_H, "Don't Save",
                    lk == UI_NORMAL ? UI_DANGER : lk);
          ui_zone_add(&p->zs, ZG_CARD, Z_DISCARD, x + 16, by, bw, BTN_H,
                      true);
        }
    }
  else
    {
      ok = p->prompt == P_OPEN ? "Open" : "Save";
      ops_text(&g_o, &g_desk_font_uib, x + 18, y + 16, p->prompt == P_OPEN ?
               "Open a picture" : "Save as a BMP file", C_TEXT, 255);
      ui_field_draw(&g_o, &p->field, x + 16, y + 44, cw - 32, 30, true);
      bw = ui_button_width(ok);
      bx -= bw;
      ui_button(&g_o, bx, by, bw, BTN_H, ok, p->field.len == 0 ?
                UI_DISABLED : ui_look(&p->zs, Z_OK, true) == UI_PRESSED ?
                UI_PRESSED : UI_DEFAULT);
      ui_zone_add(&p->zs, ZG_CARD, Z_OK, bx, by, bw, BTN_H,
                  p->field.len > 0);
      bw = ui_button_width("Cancel");
      bx -= bw + 8;
      ui_button(&g_o, bx, by, bw, BTN_H, "Cancel",
                ui_look(&p->zs, Z_CANCEL, true));
      ui_zone_add(&p->zs, ZG_CARD, Z_CANCEL, bx, by, bw, BTN_H, true);
    }

  ops_draw(hwnd, &g_o);
}

static void paint_draw(struct desk_win_s *w)
{
  struct paint_s *p = w->priv;
  struct play_s l;

  if (p == NULL)
    {
      return;
    }

  layout(w, &l);
  ops_reset(&g_o);
  toolbar_draw(w, &l);
  side_draw(w, &l);
  view_draw(w, &l);
  status_draw(w, &l);
  card_draw(w, &l);
}

/****************************************************************************
 * Private Functions: drawing into the canvas
 ****************************************************************************/

static void em_flush(struct emit_s *e)
{
  if (e->canvas)
    {
      canvas_flush(e->p);
    }
  else
    {
      ui_clip(e->o, e->from, e->cx0, e->cy0, e->cx1, e->cy1);
      ops_draw(e->hwnd, e->o);
    }

  e->from = 0;
}

static struct nx_gpuop_s *em_op(struct emit_s *e)
{
  if (e->o->n >= NX_GPUOPS_MAX)
    {
      em_flush(e);
    }

  return ops_add(e->o);
}

static void em_canvas(struct emit_s *e, struct paint_s *p)
{
  memset(e, 0, sizeof(*e));
  e->p = p;
  e->o = g_co;
  e->hwnd = p->cv->hwnd;
  e->canvas = true;
}

static void em_window(struct emit_s *e, struct desk_win_s *w,
                      struct play_s *l)
{
  struct paint_s *p = w->priv;

  memset(e, 0, sizeof(*e));
  e->p = p;
  e->o = &g_o;
  e->hwnd = (NXWINDOW)w->hwnd;
  e->tx = l->ox;
  e->ty = l->oy;
  canvas_vis(p, l, &e->cx0, &e->cy0, &e->cx1, &e->cy1);
  e->from = g_o.n;
}

/* A canvas rectangle is about to change: saved for undo (drawing into the
 * canvas only), and part of what the window must show
 */

static void em_touch(struct emit_s *e, int x0, int y0, int x1, int y1)
{
  struct paint_s *p = e->p;

  if (!e->canvas)
    {
      return;
    }

  save_rect(p, x0, y0, x1, y1);
  p->bx0 = x0 < p->bx0 ? x0 : p->bx0;
  p->by0 = y0 < p->by0 ? y0 : p->by0;
  p->bx1 = x1 > p->bx1 ? x1 : p->bx1;
  p->by1 = y1 > p->by1 ? y1 : p->by1;
}

static void bbox_reset(struct paint_s *p)
{
  p->bx0 = p->by0 = INT_MAX;
  p->bx1 = p->by1 = INT_MIN;
}

static uint16_t ink(struct paint_s *p)
{
  return p->tool == T_ERASER ? C_PAPER : p->colour;
}

/* A dab at (x, y) (pixels: the centre of pixel i is i + 0.5) */

static void em_dab(struct emit_s *e, float x, float y)
{
  struct paint_s *p = e->p;
  int d = p->size;
  int px = ifloor(x * 16.0f);
  int py = ifloor(y * 16.0f);
  struct nx_gpuop_s *op;
  int dx;
  int dy;

  /* Its tiles are saved before it is drawn */

  dx = ((d & 1) ? px >> 4 : (px + 8) >> 4) - d / 2;
  dy = ((d & 1) ? py >> 4 : (py + 8) >> 4) - d / 2;
  em_touch(e, dx, dy, dx + d, dy + d);
  if (e->o->n >= NX_GPUOPS_MAX)
    {
      em_flush(e);
    }

  op = dab_op(e->o, px, py, d, ink(p));
  op->x    += e->tx;
  op->y    += e->ty;
  op->p[0] += e->tx;
  op->p[1] += e->ty;
  op->p[2] += e->tx;
  op->p[3] += e->ty;
}

static float spacing(struct paint_s *p)
{
  float s = p->size / 5.0f;

  return s < 1.0f ? 1.0f : s;
}

/* Dabs along a straight piece: one every spacing, counting from the last
 * dab (carry)
 */

static void em_line(struct emit_s *e, float x0, float y0, float x1,
                    float y1)
{
  struct paint_s *p = e->p;
  float dx = x1 - x0;
  float dy = y1 - y0;
  float len = fsqrt(dx * dx + dy * dy);
  float s = spacing(p);
  float t = s - p->carry;

  if (len <= 0.0f)
    {
      return;
    }

  for (; t <= len; t += s)
    {
      em_dab(e, x0 + dx * t / len, y0 + dy * t / len);
    }

  p->carry = len - (t - s);
}

/* A quadratic curve from a to b, bent towards c, in short straight pieces */

static void em_quad(struct emit_s *e, float ax, float ay, float cx,
                    float cy, float bx, float by)
{
  float l1 = fsqrt((cx - ax) * (cx - ax) + (cy - ay) * (cy - ay));
  float l2 = fsqrt((bx - cx) * (bx - cx) + (by - cy) * (by - cy));
  int n = (int)((l1 + l2) / 3.0f) + 1;
  float px = ax;
  float py = ay;
  int i;

  n = n > 64 ? 64 : n;
  for (i = 1; i <= n; i++)
    {
      float t = (float)i / n;
      float u = 1.0f - t;
      float x = u * u * ax + 2.0f * u * t * cx + t * t * bx;
      float y = u * u * ay + 2.0f * u * t * cy + t * t * by;

      em_line(e, px, py, x, y);
      px = x;
      py = y;
    }
}

/* A line of dabs from one pixel to another, both ends included */

static void em_segment(struct emit_s *e, int x0, int y0, int x1, int y1)
{
  struct paint_s *p = e->p;

  em_dab(e, x0 + 0.5f, y0 + 0.5f);
  p->carry = 0.0f;
  em_line(e, x0 + 0.5f, y0 + 0.5f, x1 + 0.5f, y1 + 0.5f);
  if (p->carry > 0.01f)
    {
      em_dab(e, x1 + 0.5f, y1 + 0.5f);
    }
}

/* Rectangles: filled, or outlined with the brush's width */

static void em_fill(struct emit_s *e, int x, int y, int w, int h,
                    uint16_t c)
{
  struct nx_gpuop_s *op;

  if (w <= 0 || h <= 0)
    {
      return;
    }

  em_touch(e, x, y, x + w, y + h);
  op = em_op(e);
  op->op = NX_GPUOP_FILL;
  op->x = x + e->tx;
  op->y = y + e->ty;
  op->w = w;
  op->h = h;
  op->colour = c;
}

static void em_rect(struct emit_s *e, int x0, int y0, int x1, int y1)
{
  struct paint_s *p = e->p;
  int w = x1 - x0 + 1;
  int h = y1 - y0 + 1;
  int d = p->size;

  if (p->filled || 2 * d >= w || 2 * d >= h)
    {
      em_fill(e, x0, y0, w, h, p->colour);
      return;
    }

  em_fill(e, x0, y0, w, d, p->colour);
  em_fill(e, x0, y1 - d + 1, w, d, p->colour);
  em_fill(e, x0, y0 + d, d, h - 2 * d, p->colour);
  em_fill(e, x1 - d + 1, y0 + d, d, h - 2 * d, p->colour);
}

/* Ellipses: the one inscribed in pixels x0..x1, y0..y1 */

struct ell_s
{
  float cx;
  float cy;
  float a;
  float b;
};

static void ell_init(struct ell_s *el, int x0, int y0, int x1, int y1)
{
  el->cx = (x0 + x1 + 1) / 2.0f;
  el->cy = (y0 + y1 + 1) / 2.0f;
  el->a = (x1 + 1 - x0) / 2.0f;
  el->b = (y1 + 1 - y0) / 2.0f;
}

/* Its half width at height y */

static float ell_hw(struct ell_s *el, float y)
{
  float t = (y - el->cy) / el->b;

  return t >= 1.0f || t <= -1.0f ? 0.0f : el->a * fsqrt(1.0f - t * t);
}

/* How much of pixel (i, j) is inside (0..255): 4 x 4 samples */

static int ell_cover(struct ell_s *el, int i, int j)
{
  int n = 0;
  int u;
  int v;

  for (v = 0; v < 4; v++)
    {
      float y = (j + (v + 0.5f) / 4.0f - el->cy) / el->b;

      for (u = 0; u < 4; u++)
        {
          float x = (i + (u + 0.5f) / 4.0f - el->cx) / el->a;

          n += x * x + y * y <= 1.0f;
        }
    }

  return (n * 255 + 8) / 16;
}

/* A run of partly covered pixels of row j, [i0, i1): in pieces of up to
 * six, each a ramp between the coverage of its ends, or (two or fewer) a
 * BLEND a pixel
 */

static void ell_run(struct emit_s *e, struct ell_s *el, int i0, int i1,
                    int j, uint16_t c)
{
  int i = i0;

  while (i < i1)
    {
      int n = i1 - i > 6 ? 6 : i1 - i;
      struct nx_gpuop_s *op;
      int c0;
      int c1;
      int k;

      if (n <= 2)
        {
          for (k = 0; k < n; k++)
            {
              int cv = ell_cover(el, i + k, j);

              if (cv > 0)
                {
                  op = em_op(e);
                  op->op = NX_GPUOP_BLEND;
                  op->mode = NX_BLEND_SRC_COLOUR;
                  op->colour = c;
                  op->alpha = cv;
                  op->x = i + k + e->tx;
                  op->y = j + e->ty;
                  op->w = 1;
                  op->h = 1;
                }
            }

          i += n;
          continue;
        }

      c0 = ell_cover(el, i, j);
      c1 = ell_cover(el, i + n - 1, j);
      if (c0 > 0 || c1 > 0)
        {
          op = em_op(e);
          op->op = NX_GPUOP_BLEND;
          op->mode = NX_BLEND_SRC_COLOUR | NX_BLEND_MOD_HRAMP;
          op->colour = c;
          op->x = i + e->tx;
          op->y = j + e->ty;
          op->w = n;
          op->h = 1;
          op->p[0] = i + e->tx;
          op->p[1] = ((c1 - c0) * 256) / (n - 1);
          op->p[2] = (int16_t)(uint16_t)(c0 * 256 + 128);
        }

      i += n;
    }
}

static void em_ellipse_fill(struct emit_s *e, int x0, int y0, int x1,
                            int y1)
{
  struct paint_s *p = e->p;
  struct ell_s el;
  int j;

  ell_init(&el, x0, y0, x1, y1);
  em_touch(e, x0, y0, x1 + 1, y1 + 1);
  for (j = y0; j <= y1; j++)
    {
      float ht = ell_hw(&el, (float)j);
      float hb = ell_hw(&el, (float)(j + 1));
      float wmin = ht < hb ? ht : hb;
      float wmax = (j <= el.cy && el.cy <= j + 1) ? el.a :
                   ht > hb ? ht : hb;
      int ol = ifloor(el.cx - wmax);
      int or = iceil(el.cx + wmax);
      int fl = iceil(el.cx - wmin);
      int fr = ifloor(el.cx + wmin);

      ol = ol < x0 ? x0 : ol;
      or = or > x1 + 1 ? x1 + 1 : or;
      if (fr > fl)
        {
          struct nx_gpuop_s *op = em_op(e);

          op->op = NX_GPUOP_FILL;
          op->colour = p->colour;
          op->x = fl + e->tx;
          op->y = j + e->ty;
          op->w = fr - fl;
          op->h = 1;
          ell_run(e, &el, ol, fl, j, p->colour);
          ell_run(e, &el, fr, or, j, p->colour);
        }
      else
        {
          int mid = ifloor(el.cx);

          ell_run(e, &el, ol, mid, j, p->colour);
          ell_run(e, &el, mid, or, j, p->colour);
        }
    }
}

/* Outlined: dabs along the ellipse inset by half the brush, stepped round
 * by a rotation (its sine and cosine from their series: no libm)
 */

static void em_ellipse_line(struct emit_s *e, int x0, int y0, int x1,
                            int y1)
{
  struct paint_s *p = e->p;
  struct ell_s el;
  float a;
  float b;
  float d;
  float cd;
  float sd;
  float u = 1.0f;
  float v = 0.0f;
  float px;
  float py;
  int n;
  int i;

  ell_init(&el, x0, y0, x1, y1);
  a = el.a - p->size / 2.0f;
  b = el.b - p->size / 2.0f;
  a = a < 0.0f ? 0.0f : a;
  b = b < 0.0f ? 0.0f : b;
  if (a < 0.5f && b < 0.5f)
    {
      em_dab(e, el.cx, el.cy);
      return;
    }

  n = (int)(6.2831853f * fsqrt((a * a + b * b) / 2.0f) / 2.0f);
  n = n < 24 ? 24 : n > 2000 ? 2000 : n;
  d = 6.2831853f / n;
  sd = d - d * d * d / 6.0f + d * d * d * d * d / 120.0f;
  cd = 1.0f - d * d / 2.0f + d * d * d * d / 24.0f;

  px = el.cx + a;
  py = el.cy;
  p->carry = spacing(p);
  for (i = 1; i <= n; i++)
    {
      float nu = u * cd - v * sd;
      float nv = u * sd + v * cd;
      float x;
      float y;

      u = nu;
      v = nv;
      x = el.cx + a * u;
      y = el.cy + b * v;
      em_line(e, px, py, x, y);
      px = x;
      py = y;
    }
}

/* The shape being dragged, from (sx0, sy0) to (sx1, sy1) */

static void em_shape(struct emit_s *e)
{
  struct paint_s *p = e->p;
  int x0 = p->sx0 < p->sx1 ? p->sx0 : p->sx1;
  int x1 = p->sx0 < p->sx1 ? p->sx1 : p->sx0;
  int y0 = p->sy0 < p->sy1 ? p->sy0 : p->sy1;
  int y1 = p->sy0 < p->sy1 ? p->sy1 : p->sy0;

  switch (p->tool)
    {
      case T_LINE:
        em_segment(e, p->sx0, p->sy0, p->sx1, p->sy1);
        break;

      case T_RECT:
        em_rect(e, x0, y0, x1, y1);
        break;

      default:
        if (p->filled)
          {
            em_ellipse_fill(e, x0, y0, x1, y1);
          }
        else
          {
            em_ellipse_line(e, x0, y0, x1, y1);
          }
        break;
    }
}

/* Its bounding box (canvas, exclusive), dabs and all */

static void shape_box(struct paint_s *p, int *x0, int *y0, int *x1, int *y1)
{
  int r = p->tool == T_RECT || (p->tool == T_ELLIPSE && p->filled) ? 0 :
          p->size / 2 + 2;

  *x0 = (p->sx0 < p->sx1 ? p->sx0 : p->sx1) - r;
  *y0 = (p->sy0 < p->sy1 ? p->sy0 : p->sy1) - r;
  *x1 = (p->sx0 < p->sx1 ? p->sx1 : p->sx0) + 1 + r;
  *y1 = (p->sy0 < p->sy1 ? p->sy1 : p->sy0) + 1 + r;
}

/* What the canvas changed: shown in the window */

static void show_changes(struct desk_win_s *w)
{
  struct paint_s *p = w->priv;
  struct play_s l;

  if (p->bx0 < p->bx1)
    {
      layout(w, &l);
      show_rect(w, &l, p->bx0, p->by0, p->bx1, p->by1);
      ops_draw((NXWINDOW)w->hwnd, &g_o);
    }

  bbox_reset(p);
}

/****************************************************************************
 * Private Functions: the tools
 ****************************************************************************/

static void stroke_begin(struct desk_win_s *w, int x, int y)
{
  struct paint_s *p = w->priv;
  struct emit_s e;

  op_begin(p);
  brush_use(p->size);
  bbox_reset(p);
  em_canvas(&e, p);
  p->lx = p->mx = x + 0.5f;
  p->ly = p->my = y + 0.5f;
  p->carry = 0.0f;
  em_dab(&e, p->lx, p->ly);
  canvas_flush(p);
  show_changes(w);
  p->drag = D_PAINT;
}

/* A new sample: the curve from the last midpoint to this one's, bent
 * towards the last sample
 */

static void stroke_move(struct desk_win_s *w, int x, int y)
{
  struct paint_s *p = w->priv;
  float nx = x + 0.5f;
  float ny = y + 0.5f;
  float mx = (p->lx + nx) / 2.0f;
  float my = (p->ly + ny) / 2.0f;
  struct emit_s e;

  if (nx == p->lx && ny == p->ly)
    {
      return;
    }

  brush_use(p->size);
  em_canvas(&e, p);
  em_quad(&e, p->mx, p->my, p->lx, p->ly, mx, my);
  p->mx = mx;
  p->my = my;
  p->lx = nx;
  p->ly = ny;
  canvas_flush(p);
  show_changes(w);
}

static void stroke_end(struct desk_win_s *w)
{
  struct paint_s *p = w->priv;
  struct play_s l;
  struct emit_s e;

  brush_use(p->size);
  em_canvas(&e, p);
  em_line(&e, p->mx, p->my, p->lx, p->ly);
  if (p->carry > 0.01f)
    {
      em_dab(&e, p->lx, p->ly);
    }

  op_end(p);
  show_changes(w);
  p->drag = D_NONE;
  layout(w, &l);
  toolbar_draw(w, &l);
  status_draw(w, &l);
}

/* A shape: shown in the window while dragged (the canvas copied back over
 * the last showing first), drawn into the canvas when let go
 */

static void shape_show(struct desk_win_s *w)
{
  struct paint_s *p = w->priv;
  struct play_s l;
  struct emit_s e;

  layout(w, &l);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
  brush_use(p->size);
  if (p->preview)
    {
      show_rect(w, &l, p->pvx0, p->pvy0, p->pvx1, p->pvy1);
    }

  shape_box(p, &p->pvx0, &p->pvy0, &p->pvx1, &p->pvy1);
  p->preview = true;
  em_window(&e, w, &l);
  em_shape(&e);
  em_flush(&e);
}

static void shape_cancel(struct desk_win_s *w)
{
  struct paint_s *p = w->priv;
  struct play_s l;

  layout(w, &l);
  if (p->preview)
    {
      show_rect(w, &l, p->pvx0, p->pvy0, p->pvx1, p->pvy1);
      ops_draw((NXWINDOW)w->hwnd, &g_o);
    }

  p->preview = false;
  p->drag = D_NONE;
}

static void shape_commit(struct desk_win_s *w)
{
  struct paint_s *p = w->priv;
  struct play_s l;
  struct emit_s e;
  int x0;
  int y0;
  int x1;
  int y1;

  op_begin(p);
  brush_use(p->size);
  bbox_reset(p);
  em_canvas(&e, p);
  shape_box(p, &x0, &y0, &x1, &y1);
  em_touch(&e, x0, y0, x1, y1);
  em_shape(&e);
  op_end(p);

  /* The canvas over the preview (they differ where it was cut) */

  if (p->preview)
    {
      em_touch(&e, p->pvx0, p->pvy0, p->pvx1, p->pvy1);
    }

  p->preview = false;
  show_changes(w);
  p->drag = D_NONE;
  layout(w, &l);
  toolbar_draw(w, &l);
  status_draw(w, &l);
}

/* The fill: the canvas read back, a scan-line flood fill of the pixels
 * like the one clicked (FILL_TOL), the rows that changed put back
 */

#define BIT_GET(m, i)  ((m)[(i) >> 3] & (1 << ((i) & 7)))
#define BIT_SET(m, i)  ((m)[(i) >> 3] |= (1 << ((i) & 7)))

static bool like(uint16_t a, int r, int g, int b)
{
  int ar;
  int ag;
  int ab;

  rgb_of(a, &ar, &ag, &ab);
  return abs(ar - r) <= FILL_TOL && abs(ag - g) <= FILL_TOL &&
         abs(ab - b) <= FILL_TOL;
}

static bool fillable(uint16_t *pix, uint8_t *done, int i, int r, int g,
                     int b)
{
  return !BIT_GET(done, i) && like(pix[i], r, g, b);
}

static int flood(uint16_t *pix, uint8_t *done, int w, int h, int x, int y,
                 uint16_t c, int *bx0, int *by0, int *bx1, int *by1)
{
  int32_t *stack;
  int cap = 1024;
  int n = 0;
  int tr;
  int tg;
  int tb;

  stack = malloc(cap * sizeof(int32_t));
  if (stack == NULL)
    {
      return -ENOMEM;
    }

  rgb_of(pix[y * w + x], &tr, &tg, &tb);
  stack[n++] = y << 16 | x;
  *bx0 = *by0 = INT_MAX;
  *bx1 = *by1 = INT_MIN;

  while (n > 0)
    {
      int sx = stack[--n] & 0xffff;
      int sy = stack[n] >> 16;
      int row = sy * w;
      int l;
      int r;
      int dy;

      if (!fillable(pix, done, row + sx, tr, tg, tb))
        {
          continue;
        }

      for (l = sx; l > 0 && fillable(pix, done, row + l - 1, tr, tg, tb);
           l--)
        {
        }

      for (r = sx; r < w - 1 &&
           fillable(pix, done, row + r + 1, tr, tg, tb); r++)
        {
        }

      for (sx = l; sx <= r; sx++)
        {
          BIT_SET(done, row + sx);
          pix[row + sx] = c;
        }

      *bx0 = l < *bx0 ? l : *bx0;
      *bx1 = r + 1 > *bx1 ? r + 1 : *bx1;
      *by0 = sy < *by0 ? sy : *by0;
      *by1 = sy + 1 > *by1 ? sy + 1 : *by1;

      /* Seeds: the start of each run to fill above and below */

      for (dy = -1; dy <= 1; dy += 2)
        {
          int yy = sy + dy;
          bool in = false;

          if (yy < 0 || yy >= h)
            {
              continue;
            }

          for (sx = l; sx <= r; sx++)
            {
              bool ok = fillable(pix, done, yy * w + sx, tr, tg, tb);

              if (ok && !in)
                {
                  if (n == cap)
                    {
                      int32_t *ns = realloc(stack, 2 * cap *
                                            sizeof(int32_t));

                      if (ns == NULL)
                        {
                          free(stack);
                          return -ENOMEM;
                        }

                      stack = ns;
                      cap *= 2;
                    }

                  stack[n++] = yy << 16 | sx;
                }

              in = ok;
            }
        }
    }

  free(stack);
  return 0;
}

static void do_fill(struct desk_win_s *w, int x, int y)
{
  struct paint_s *p = w->priv;
  struct desk_image_s *cv = p->cv;
  struct play_s l;
  uint16_t *pix;
  uint8_t *done;
  int bx0 = 0;
  int by0 = 0;
  int bx1 = 0;
  int by1 = 0;
  int ret = 0;
  int r;

  if (x < 0 || y < 0 || x >= cv->w || y >= cv->h)
    {
      return;
    }

  pix = malloc(cv->w * cv->h * sizeof(uint16_t));
  done = calloc((cv->w * cv->h + 7) / 8, 1);
  if (pix == NULL || done == NULL)
    {
      free(pix);
      free(done);
      set_msg(w, true, "Not enough memory to fill%s", "");
      return;
    }

  for (r = 0; r < cv->h && ret >= 0; r += 64)
    {
      int n = cv->h - r < 64 ? cv->h - r : 64;

      ret = img_read(cv, 0, r, cv->w, n, pix + r * cv->w,
                     cv->w * sizeof(uint16_t));
    }

  if (ret >= 0)
    {
      ret = flood(pix, done, cv->w, cv->h, x, y, p->colour, &bx0, &by0,
                  &bx1, &by1);
    }

  free(done);
  if (ret < 0)
    {
      free(pix);
      set_msg(w, true, "Cannot fill: %s", strerror(-ret));
      return;
    }

  if (bx0 < bx1)
    {
      op_begin(p);
      save_rect(p, bx0, by0, bx1, by1);
      canvas_flush(p);
      img_write(cv, bx0, by0, bx1 - bx0, by1 - by0,
                pix + by0 * cv->w + bx0, cv->w * sizeof(uint16_t));
      op_end(p);
      p->bx0 = bx0;
      p->by0 = by0;
      p->bx1 = bx1;
      p->by1 = by1;
      show_changes(w);
      layout(w, &l);
      toolbar_draw(w, &l);
      status_draw(w, &l);
    }

  free(pix);
}

static void set_colour(struct desk_win_s *w, uint16_t c)
{
  struct paint_s *p = w->priv;
  struct play_s l;

  p->colour = c;
  if (p->tool == T_ERASER)
    {
      p->tool = T_BRUSH;
    }

  layout(w, &l);
  side_draw(w, &l);
  status_draw(w, &l);
}

static void pick_at(struct desk_win_s *w, int x, int y)
{
  struct paint_s *p = w->priv;
  uint16_t c;

  if (x < 0 || y < 0 || x >= p->cv->w || y >= p->cv->h)
    {
      return;
    }

  if (img_read(p->cv, x, y, 1, 1, &c, 2) >= 0 && c != p->colour)
    {
      struct play_s l;

      p->colour = c;
      layout(w, &l);
      side_draw(w, &l);
    }
}

static void hue_at(struct desk_win_s *w, int x, int y)
{
  struct paint_s *p = w->priv;
  uint16_t c = huebox_colour(clampi(x - p->huex, 0, p->huew - 1),
                             clampi(y - p->huey, 0, p->hueh - 1),
                             p->huew, p->hueh);

  if (c != p->colour)
    {
      set_colour(w, c);
    }
}

static void size_at(struct desk_win_s *w, int x)
{
  struct paint_s *p = w->priv;
  int s = BRUSH_MIN + ((x - p->sizex - 7) * (BRUSH_MAX - BRUSH_MIN) +
                      (SP_IW - 14) / 2) / (SP_IW - 14);
  struct play_s l;

  s = clampi(s, BRUSH_MIN, BRUSH_MAX);
  if (s != p->size)
    {
      p->size = s;
      layout(w, &l);
      side_draw(w, &l);
      status_draw(w, &l);
    }
}

static void set_tool(struct desk_win_s *w, int t)
{
  struct paint_s *p = w->priv;
  struct play_s l;

  p->tool = t;
  p->msg[0] = '\0';
  layout(w, &l);
  side_draw(w, &l);
  status_draw(w, &l);
}

/****************************************************************************
 * Private Functions: files
 ****************************************************************************/

static void set_title(struct desk_win_s *w)
{
  struct paint_s *p = w->priv;

  snprintf(w->title, sizeof(w->title), "%s", p->path[0] != '\0' ?
           base_name(p->path) : "Untitled");
  desk_win_title(w);
}

/* A new canvas: the history and the view start again.  The old canvas
 * and the pool go first, so that the new one has the GPU's room.
 */

static void canvas_drop(struct paint_s *p)
{
  pool_free(p);
  img_free(p->cv);
  p->cv = NULL;
  p->panx = 0;
  p->pany = 0;
  p->preview = false;
  p->drag = D_NONE;
  p->modified = false;
  p->curx = -1;
  p->cury = -1;
}

static void do_new(struct desk_win_s *w)
{
  struct paint_s *p = w->priv;

  canvas_drop(p);
  p->cv = img_new(CANVAS_W, CANVAS_H, C_PAPER, w);
  pool_alloc(w, p);
  p->path[0] = '\0';
  p->msg[0] = '\0';
  p->msgerr = false;
  if (p->cv == NULL)
    {
      snprintf(p->msg, sizeof(p->msg), "Not enough memory for a picture");
      p->msgerr = true;
    }

  set_title(w);
  paint_draw(w);
}

static void do_open(struct desk_win_s *w, const char *path)
{
  struct paint_s *p = w->priv;
  char err[128];
  char fmt[24];
  int iw;
  int ih;

  if (img_info(path, &iw, &ih, fmt, sizeof(fmt), err, sizeof(err)) < 0)
    {
      snprintf(p->msg, sizeof(p->msg), "%s: %s", base_name(path), err);
      p->msgerr = true;
      if (p->cv == NULL)
        {
          do_new(w);
          snprintf(p->msg, sizeof(p->msg), "%s: %s", base_name(path), err);
          p->msgerr = true;
        }

      paint_draw(w);
      return;
    }

  canvas_drop(p);
  p->cv = img_load(path, CANVAS_MAXW, CANVAS_MAXH, w, err, sizeof(err));
  if (p->cv == NULL)
    {
      do_new(w);
      snprintf(p->msg, sizeof(p->msg), "%s: %s", base_name(path), err);
      p->msgerr = true;
      paint_draw(w);
      return;
    }

  pool_alloc(w, p);
  strlcpy(p->path, path, sizeof(p->path));
  p->msg[0] = '\0';
  p->msgerr = false;
  if (p->cv->w != p->cv->fw || p->cv->h != p->cv->fh)
    {
      snprintf(p->msg, sizeof(p->msg), "Scaled down from %d x %d",
               p->cv->fw, p->cv->fh);
    }

  set_title(w);
  paint_draw(w);
}

static bool is_bmp(const char *path)
{
  const char *dot = strrchr(path, '.');

  return dot != NULL && strcasecmp(dot, ".bmp") == 0;
}

static void do_save(struct desk_win_s *w, const char *path)
{
  struct paint_s *p = w->priv;
  struct play_s l;
  int ret;

  ret = img_save_bmp(p->cv, path);
  if (ret < 0)
    {
      snprintf(p->msg, sizeof(p->msg), "Cannot save %s: %s",
               base_name(path), strerror(-ret));
      p->msgerr = true;
      p->closeafter = false;
      layout(w, &l);
      status_draw(w, &l);
      return;
    }

  if (path != p->path)
    {
      strlcpy(p->path, path, sizeof(p->path));
    }

  p->modified = false;
  snprintf(p->msg, sizeof(p->msg), "Saved %s", base_name(path));
  p->msgerr = false;
  set_title(w);
  if (p->closeafter)
    {
      desk_win_close(w);
      return;
    }

  layout(w, &l);
  toolbar_draw(w, &l);
  status_draw(w, &l);
}

/* A question: the card over the view */

static void ask(struct desk_win_s *w, int prompt)
{
  struct paint_s *p = w->priv;
  struct play_s l;
  char buf[PATH_MAX];

  if (p->drag == D_SHAPE)
    {
      shape_cancel(w);
    }
  else if (p->drag == D_PAINT)
    {
      stroke_end(w);
    }

  p->drag = D_NONE;
  p->prompt = prompt;
  p->zs.hot = 0;
  p->zs.pressed = 0;
  if (prompt == P_SAVEAS)
    {
      if (p->path[0] != '\0')
        {
          char *dot;

          strlcpy(buf, p->path, sizeof(buf));
          dot = strrchr(buf, '.');
          if (dot != NULL && strchr(dot, '/') == NULL)
            {
              *dot = '\0';
            }

          strlcat(buf, ".bmp", sizeof(buf));
        }
      else
        {
          snprintf(buf, sizeof(buf), "%s/Untitled.bmp", home_dir());
        }

      ui_field_set(&p->field, buf);
    }
  else if (prompt == P_OPEN)
    {
      if (p->path[0] != '\0')
        {
          char *slash;

          strlcpy(buf, p->path, sizeof(buf));
          slash = strrchr(buf, '/');
          if (slash != NULL)
            {
              slash[1] = '\0';
            }
        }
      else
        {
          snprintf(buf, sizeof(buf), "%s/", home_dir());
        }

      ui_field_set(&p->field, buf);
    }

  layout(w, &l);
  card_draw(w, &l);
}

static void save(struct desk_win_s *w)
{
  struct paint_s *p = w->priv;

  if (p->cv == NULL)
    {
      return;
    }

  if (p->path[0] == '\0' || !is_bmp(p->path))
    {
      ask(w, P_SAVEAS);
    }
  else
    {
      do_save(w, p->path);
    }
}

static void answer(struct desk_win_s *w, int id)
{
  struct paint_s *p = w->priv;
  int prompt = p->prompt;
  int after = p->after;
  char path[UI_FIELD_MAX];
  struct play_s l;

  if ((prompt == P_OPEN || prompt == P_SAVEAS) && id == Z_OK &&
      p->field.len == 0)
    {
      return;
    }

  p->prompt = P_NONE;
  p->after = A_NONE;
  ui_zones_clear(&p->zs, ZG_CARD);
  layout(w, &l);
  view_draw(w, &l);
  switch (prompt)
    {
      case P_OPEN:
        if (id == Z_OK)
          {
            strlcpy(path, p->field.text, sizeof(path));
            do_open(w, path);
          }
        break;

      case P_SAVEAS:
        if (id == Z_OK)
          {
            strlcpy(path, p->field.text, sizeof(path));
            do_save(w, path);
          }
        else
          {
            p->closeafter = false;
          }
        break;

      case P_CLOSE:
        if (id == Z_OK)
          {
            p->closeafter = true;
            save(w);
          }
        else if (id == Z_DISCARD)
          {
            p->modified = false;
            desk_win_close(w);
          }
        break;

      case P_DISCARD:
        if (id == Z_DISCARD && after == A_NEW)
          {
            do_new(w);
          }
        else if (id == Z_DISCARD && after == A_OPEN)
          {
            ask(w, P_OPEN);
          }
        break;
    }
}

/****************************************************************************
 * Private Functions: undo and redo
 ****************************************************************************/

static void undo_redo(struct desk_win_s *w, bool redo)
{
  struct paint_s *p = w->priv;
  struct urec_s *r;
  struct play_s l;

  if (p->drag != D_NONE || p->pool == NULL ||
      (redo ? p->nredo == 0 : p->nundo == 0))
    {
      return;
    }

  r = redo ? &p->hist[p->nundo] : &p->hist[p->nundo - 1];
  rec_swap(p, r, &p->bx0, &p->by0, &p->bx1, &p->by1);
  if (redo)
    {
      p->nundo++;
      p->nredo--;
    }
  else
    {
      p->nundo--;
      p->nredo++;
    }

  p->modified = true;
  p->msg[0] = '\0';
  show_changes(w);
  layout(w, &l);
  toolbar_draw(w, &l);
  status_draw(w, &l);
}

/****************************************************************************
 * Private Functions: events
 ****************************************************************************/

static void action(struct desk_win_s *w, int id)
{
  struct paint_s *p = w->priv;
  struct play_s l;

  if (id >= Z_TOOL0 && id < Z_TOOL0 + T_COUNT)
    {
      set_tool(w, id - Z_TOOL0);
      return;
    }

  if (id >= Z_SWATCH0 && id < Z_SWATCH0 + NSWATCH)
    {
      set_colour(w, g_palette[id - Z_SWATCH0]);
      return;
    }

  switch (id)
    {
      case Z_NEW:
      case Z_OPEN:
        if (p->modified)
          {
            ask(w, P_DISCARD);
            p->after = id == Z_NEW ? A_NEW : A_OPEN;
          }
        else if (id == Z_NEW)
          {
            do_new(w);
          }
        else
          {
            ask(w, P_OPEN);
          }
        break;

      case Z_SAVE:
        save(w);
        break;

      case Z_SAVEAS:
        if (p->cv != NULL)
          {
            ask(w, P_SAVEAS);
          }
        break;

      case Z_UNDO:
      case Z_REDO:
        undo_redo(w, id == Z_REDO);
        break;

      case Z_OUTLINE:
      case Z_FILLED:
        p->filled = id == Z_FILLED;
        layout(w, &l);
        side_draw(w, &l);
        status_draw(w, &l);
        break;

      case Z_OK:
      case Z_CANCEL:
      case Z_DISCARD:
        answer(w, id);
        break;

      default:
        break;
    }
}

static void set_hot(struct desk_win_s *w, int id)
{
  struct paint_s *p = w->priv;
  int g0 = ui_zone_group(&p->zs, p->zs.hot);
  int g1 = ui_zone_group(&p->zs, id);
  struct play_s l;

  if (id == p->zs.hot)
    {
      return;
    }

  p->zs.hot = id;
  layout(w, &l);
  if (g0 == ZG_TOOL || g1 == ZG_TOOL)
    {
      toolbar_draw(w, &l);
    }

  if (g0 == ZG_SIDE || g1 == ZG_SIDE)
    {
      side_draw(w, &l);
    }

  if (g0 == ZG_CARD || g1 == ZG_CARD)
    {
      card_draw(w, &l);
    }
}

/* The pointer's place on the canvas, in the status bar */

static void track(struct desk_win_s *w, int x, int y)
{
  struct paint_s *p = w->priv;
  struct play_s l;
  int cx = -1;
  int cy = -1;

  layout(w, &l);
  if (p->cv != NULL && x >= l.vx && y >= l.vy && x < l.vx + l.vw &&
      y < l.vy + l.vh && x >= l.ox && y >= l.oy &&
      x < l.ox + p->cv->w && y < l.oy + p->cv->h)
    {
      cx = x - l.ox;
      cy = y - l.oy;
    }

  if (cx != p->curx || cy != p->cury)
    {
      p->curx = cx;
      p->cury = cy;
      status_draw(w, &l);
    }
}

/* A press in the view: the tool */

static void tool_press(struct desk_win_s *w, int px, int py)
{
  struct paint_s *p = w->priv;

  if (p->msg[0] != '\0')
    {
      p->msg[0] = '\0';
      p->msgerr = false;
    }

  switch (p->tool)
    {
      case T_BRUSH:
      case T_ERASER:
        stroke_begin(w, px, py);
        break;

      case T_LINE:
      case T_RECT:
      case T_ELLIPSE:
        p->sx0 = p->sx1 = px;
        p->sy0 = p->sy1 = py;
        p->preview = false;
        p->drag = D_SHAPE;
        shape_show(w);
        break;

      case T_FILL:
        do_fill(w, px, py);
        break;

      default:
        p->drag = D_PICK;
        pick_at(w, px, py);
        break;
    }
}

/* A drag goes on until its button is let go: true if one did */

static bool drag_move(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct paint_s *p = w->priv;
  bool left = (buttons & NX_MOUSE_LEFTBUTTON) != 0;
  struct play_s l;

  layout(w, &l);
  switch (p->drag)
    {
      case D_PAINT:
        stroke_move(w, x - l.ox, y - l.oy);
        if (!left)
          {
            stroke_end(w);
          }
        break;

      case D_SHAPE:
        p->sx1 = x - l.ox;
        p->sy1 = y - l.oy;
        if (left)
          {
            shape_show(w);
          }
        else
          {
            shape_commit(w);
          }
        break;

      case D_PAN:
        if (buttons & (NX_MOUSE_RIGHTBUTTON | NX_MOUSE_CENTERBUTTON))
          {
            p->panx = p->grabpx - (x - p->grabx);
            p->pany = p->grabpy - (y - p->graby);
            layout(w, &l);
            view_draw(w, &l);
          }
        else
          {
            p->drag = D_NONE;
          }
        break;

      case D_SIZE:
        size_at(w, x);
        p->drag = left ? D_SIZE : D_NONE;
        break;

      case D_HUE:
        hue_at(w, x, y);
        p->drag = left ? D_HUE : D_NONE;
        break;

      case D_PICK:
        pick_at(w, x - l.ox, y - l.oy);
        if (!left)
          {
            p->drag = D_NONE;
            set_colour(w, p->colour);
          }
        break;

      default:
        return false;
    }

  track(w, x, y);
  return true;
}

static void paint_mouse(struct desk_win_s *w, int x, int cy,
                        uint8_t buttons)
{
  struct paint_s *p = w->priv;
  uint8_t other = NX_MOUSE_RIGHTBUTTON | NX_MOUSE_CENTERBUTTON;
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(p->buttons & NX_MOUSE_LEFTBUTTON);
  bool release = !(buttons & NX_MOUSE_LEFTBUTTON) &&
                 (p->buttons & NX_MOUSE_LEFTBUTTON);
  bool opress = (buttons & other) && !(p->buttons & other);
  struct ui_zone_s *z;
  struct play_s l;
  int y = cy + TITLE_H;
  bool inview;

  p->buttons = buttons;
  if (p->cv == NULL || drag_move(w, x, y, buttons))
    {
      return;
    }

  if (x < 0 && cy < 0 && buttons == 0)
    {
      set_hot(w, 0);
      p->zs.pressed = 0;
      track(w, -1, -1);
      return;
    }

  layout(w, &l);
  inview = x >= l.vx && y >= l.vy && x < l.vx + l.vw && y < l.vy + l.vh;
  z = ui_zone_at(&p->zs, x, y);
  if (p->prompt != P_NONE && z != NULL && z->group != ZG_CARD)
    {
      z = NULL;
    }

  if (!(buttons & NX_MOUSE_LEFTBUTTON) || press)
    {
      set_hot(w, z != NULL && z->enabled ? z->id : 0);
    }

  if (press && z != NULL && z->enabled)
    {
      if (z->id == Z_SIZE)
        {
          p->drag = D_SIZE;
          size_at(w, x);
        }
      else if (z->id == Z_HUEBOX)
        {
          p->drag = D_HUE;
          hue_at(w, x, y);
        }
      else
        {
          p->zs.pressed = z->id;
          p->zs.hot = 0;
          set_hot(w, z->id);
        }
    }
  else if (press && z == NULL && p->prompt == P_NONE && inview)
    {
      tool_press(w, x - l.ox, y - l.oy);
    }
  else if (opress && p->prompt == P_NONE && inview)
    {
      p->drag = D_PAN;
      p->grabx = x;
      p->graby = y;
      p->grabpx = p->panx;
      p->grabpy = p->pany;
    }

  if (release && p->zs.pressed != 0)
    {
      int id = p->zs.pressed;

      p->zs.pressed = 0;
      if (z != NULL && z->id == id)
        {
          p->zs.hot = 0;
          action(w, id);
          if (!w->closing)
            {
              set_hot(w, id);
            }
        }
      else
        {
          toolbar_draw(w, &l);
          side_draw(w, &l);
          card_draw(w, &l);
        }
    }

  if (!w->closing)
    {
      track(w, x, y);
    }
}

static void paint_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct paint_s *p = w->priv;
  struct play_s l;
  int keys[16];
  int nk = ui_keys(&p->keys, ch, n, keys, 16);
  int i;

  for (i = 0; i < nk && !w->closing; i++)
    {
      int k = keys[i];

      layout(w, &l);
      if (p->prompt != P_NONE)
        {
          if (k == '\r' || k == '\n')
            {
              if (p->prompt != P_DISCARD)
                {
                  answer(w, Z_OK);
                }
            }
          else if (k == KEY_ESC)
            {
              answer(w, Z_CANCEL);
            }
          else if ((p->prompt == P_OPEN || p->prompt == P_SAVEAS) &&
                   ui_field_key(&p->field, k))
            {
              card_draw(w, &l);
            }

          continue;
        }

      if (p->cv == NULL)
        {
          continue;
        }

      switch (k)
        {
          case 'b':
            set_tool(w, T_BRUSH);
            break;

          case 'e':
            set_tool(w, T_ERASER);
            break;

          case 'l':
            set_tool(w, T_LINE);
            break;

          case 'r':
            set_tool(w, T_RECT);
            break;

          case 'o':
            set_tool(w, T_ELLIPSE);
            break;

          case 'f':
            set_tool(w, T_FILL);
            break;

          case 'i':
            set_tool(w, T_PICK);
            break;

          case 'g':
            action(w, p->filled ? Z_OUTLINE : Z_FILLED);
            break;

          case '[':
          case ']':
            p->size = clampi(p->size + (k == '[' ? -1 : 1) *
                             (p->size >= 10 ? 2 : 1), BRUSH_MIN, BRUSH_MAX);
            side_draw(w, &l);
            status_draw(w, &l);
            break;

          case KEY_ESC:
            if (p->drag == D_SHAPE)
              {
                shape_cancel(w);
              }
            break;

          case KEY_UP:
          case KEY_DOWN:
          case KEY_LEFT:
          case KEY_RIGHT:
            if (p->drag == D_NONE)
              {
                p->panx += k == KEY_LEFT ? -48 : k == KEY_RIGHT ? 48 : 0;
                p->pany += k == KEY_UP ? -48 : k == KEY_DOWN ? 48 : 0;
                layout(w, &l);
                view_draw(w, &l);
              }
            break;

          case KEY_CTL('z'):
            undo_redo(w, false);
            break;

          case KEY_CTL('y'):
            undo_redo(w, true);
            break;

          case KEY_CTL('s'):
            save(w);
            break;

          case KEY_CTL('o'):
            action(w, Z_OPEN);
            break;

          case KEY_CTL('n'):
            action(w, Z_NEW);
            break;

          default:
            break;
        }
    }
}

/* An image lost its pixels (no room after a display mode change) */

static void paint_wake(struct desk_win_s *w)
{
  struct paint_s *p = w->priv;

  if (p->cv != NULL && p->cv->lost)
    {
      int cw = p->cv->w;
      int ch = p->cv->h;

      canvas_drop(p);
      p->cv = img_new(cw, ch, C_PAPER, w);
      pool_alloc(w, p);
      p->modified = p->cv != NULL;
      snprintf(p->msg, sizeof(p->msg), "The picture was lost: the GPU had "
               "no room for it");
      p->msgerr = true;
      paint_draw(w);
    }
  else if (p->pool != NULL && p->pool->lost)
    {
      pool_alloc(w, p);
      paint_draw(w);
    }
}

static bool paint_canclose(struct desk_win_s *w)
{
  struct paint_s *p = w->priv;

  if (p == NULL || !p->modified)
    {
      return true;
    }

  if (p->prompt != P_CLOSE)
    {
      desk_win_activate(w);
      ask(w, P_CLOSE);
    }

  return false;
}

static void paint_close(struct desk_win_s *w)
{
  struct paint_s *p = w->priv;

  if (p != NULL)
    {
      canvas_drop(p);
    }
}

static void paint_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  int r = s >= 64 ? 6 : 3;
  int slot = s >= 64 ? LUT_R6 : LUT_R3;

  icon_tile(o, x, y, s, RGB(0xff, 0xb8, 0x4c), RGB(0xf0, 0x5a, 0x3c), a);

  /* A palette with four colours */

  ops_rrect(o, x + s / 6, y + s / 4, s * 2 / 3, s / 2, LUT_R12,
            s >= 40 ? 12 : 6, C_WHITE, a);
  ops_disc(o, x + s / 3, y + s * 3 / 8 + 1, slot, r,
           RGB(0xe8, 0x2c, 0x2c), a);
  ops_disc(o, x + s / 2, y + s / 3 + 1, slot, r, RGB(0x1e, 0x7c, 0xf0), a);
  ops_disc(o, x + s * 2 / 3, y + s * 3 / 8 + 1, slot, r,
           RGB(0x3c, 0xc0, 0x4a), a);
  ops_disc(o, x + s / 3 + 1, y + s * 9 / 16 + 1, slot, r,
           RGB(0xff, 0xd8, 0x20), a);
}

static struct desk_win_s *paint_open(void)
{
  const char *path = g_openpath;
  struct desk_win_s *w;
  struct paint_s *p;
  int cw = CANVAS_W;
  int ch = CANVAS_H;
  char fmt[24];
  char err[8];
  int iw;
  int ih;

  if (g_co == NULL)
    {
      g_co = malloc(sizeof(struct desk_ops_s));
      g_po = malloc(sizeof(struct desk_ops_s));
      if (g_co == NULL || g_po == NULL)
        {
          free(g_co);
          free(g_po);
          g_co = NULL;
          g_po = NULL;
          return NULL;
        }

      ops_reset(g_co);
      ops_reset(g_po);
    }

  p = calloc(1, sizeof(*p));
  if (p == NULL)
    {
      return NULL;
    }

  p->tool = T_BRUSH;
  p->size = 8;
  p->colour = RGB(0x1e, 0x7c, 0xf0);
  p->curx = -1;
  p->cury = -1;
  bbox_reset(p);

  /* The window takes the picture's size, within reason */

  if (path != NULL &&
      img_info(path, &iw, &ih, fmt, sizeof(fmt), err, sizeof(err)) == 0)
    {
      cw = iw > CANVAS_MAXW ? CANVAS_MAXW : iw < 320 ? 320 : iw;
      ch = ih > CANVAS_MAXH ? CANVAS_MAXH : ih < 240 ? 240 : ih;
    }

  ch = TB_H + ch + 2 * MARGIN + ST_H;
  w = desk_win_open(APP_PAINT, "Paint", SP_W + cw + 2 * MARGIN,
                    ch < SP_MINH ? SP_MINH : ch);
  if (w == NULL)
    {
      free(p);
      return NULL;
    }

  w->priv = p;
  if (path != NULL)
    {
      do_open(w, path);
    }
  else
    {
      do_new(w);
    }

  return w;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int desk_paint_open(const char *path)
{
  struct desk_win_s *w;

  if (desk_program_self())
    {
      return desk_open_post(APP_PAINT, path);    /* The desktop's thread */
    }

  /* Open already: that window comes forward */

  for (w = g_desk.wins; path != NULL && w != NULL; w = w->next)
    {
      struct paint_s *p = w->priv;

      if (w->app == APP_PAINT && !w->closing && p != NULL &&
          strcmp(p->path, path) == 0)
        {
          desk_win_restore(w);
          return 0;
        }
    }

  g_openpath = path;
  desk_launch(APP_PAINT);
  g_openpath = NULL;
  return 0;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_paint =
{
  .name     = "Paint",
  .icon     = paint_icon,
  .open     = paint_open,
  .draw     = paint_draw,
  .mouse    = paint_mouse,
  .key      = paint_key,
  .wake     = paint_wake,
  .close    = paint_close,
  .canclose = paint_canclose,
};
