/****************************************************************************
 * apps/graphics/fpcdesk/desk_fx.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The desktop's 3D effects, where the compositor shows windows in
 * perspective (NX_FX_QUAD: NX_CAP_QUAD, a v10 GPU) and Settings have them
 * on (g_prefs.effects, and each of them in g_prefs.fx):
 *
 *   - a window being dragged swings with its velocity, like a card held
 *     by its title bar: it turns away on the side it moves from, its
 *     bottom lags, and it settles with a damped overshoot when let go (a
 *     spring on each angle);
 *   - maximising and restoring zoom between the two rectangles with a
 *     tilt that comes and goes;
 *   - Alt-Tab (or Super-Tab) shows the windows as a cover flow: the
 *     selected one in the middle facing the viewer, the others turned
 *     on either side, most recently used first.  Tab and Shift-Tab, the
 *     arrows or a click choose; letting go of Alt, Enter or a click
 *     brings the chosen one forward; Escape (or a click elsewhere) goes
 *     back.  Without the effects it is a row of icons.
 *
 * Every effect is a quadrilateral for win_effects() (desk_fx_quad()): the
 * window's corners projected, as offsets from where they are.  Effects
 * run on the main thread; desk_fx_animate() steps them every frame.
 ****************************************************************************/

#include <nuttx/config.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <nuttx/nx/nxbe.h>
#include <nuttx/nx/nxkeys.h>
#include <nuttx/input/kbd_codec.h>

#include "desk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FX_FOCAL        1000.0f           /* The viewer, pixels away */

/* The swing: angles (radians) per pixel a second, at most, and the
 * spring (its frequency, Hz, and damping ratio: a little overshoot)
 */

#define SWING_YAW       0.00022f          /* Turned about the vertical */
#define SWING_YAWMAX    0.26f
#define SWING_PITCH     0.00016f          /* Tipped about the title bar */
#define SWING_PITCHMAX  0.20f
#define SWING_ROLL      0.00004f          /* The bottom lagging */
#define SWING_ROLLMAX   0.05f
#define SWING_HZ        2.4f
#define SWING_DAMP      0.5f
#define SWING_VELMS     45.0f             /* The velocity's smoothing */
#define SWING_STEP      0.004f            /* Integration step, seconds */
#define SWING_REST      0.0025f           /* Below: flat again */
#define SWING_FOCAL     1600.0f           /* (Further: a gentler look) */

/* Maximise and restore */

#define ZOOM_MS         260
#define ZOOM_TILT       0.22f

/* The switcher */

#define SW_MAX          16                /* Windows it shows */
#define SW_OPEN_MS      240
#define SW_CLOSE_MS     220
#define SW_SLIDE_MS     70.0f             /* The carousel following */
#define SW_YAW          0.9f             /* Side cards, turned */
#define SW_ICON         64
#define SW_LABEL_H      34

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sw_card_s
{
  struct desk_win_s *w;
  bool minimized;                  /* Was: back to its icon, unless chosen */
  float x0[4];                     /* Where its corners were (home) */
  float y0[4];
  float x[4];                      /* ... are shown */
  float y[4];
  float xc[4];                     /* ... were, closing */
  float yc[4];
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

const char *const g_fx_names[FX_COUNT] =
{
  "open", "genie", "swing", "zoom", "flow"   /* FX_OPEN ... FX_FLOW */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static volatile int g_hold;              /* deskctl fx hold: permille, 0 off */

static struct
{
  bool open;                             /* Shown (opening, or open) */
  bool closing;                          /* Going: the cards fly home */
  bool flat;                             /* Icons only (no perspective) */
  uint8_t mods;                          /* The modifier that holds it */
  int n;
  int sel;
  float pos;                             /* The carousel, following sel */
  float t;                               /* 0 home .. 1 in the carousel */
  float shown;                           /* ... as the cards were placed */
  float shownpos;
  uint32_t t0;                           /* Opening or closing since */
  uint32_t ms;                           /* Last stepped */
  struct desk_win_s *chosen;
  struct sw_card_s card[SW_MAX];
  NXWINDOW stage;                        /* The veil, over the wallpaper */
  NXWINDOW label;                        /* The title (flat: the icons) */
  int lx;                                /* ... where it is */
  int ly;
  int lw;
  int lh;
  uint8_t veil;                          /* The veil's alpha now */
} g_sw;

static struct desk_ops_s g_swops;

/****************************************************************************
 * Private Functions: helpers
 ****************************************************************************/

static float clampf(float v, float lo, float hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

static float ease_out(float t)                   /* Cubic */
{
  t = 1.0f - clampf(t, 0.0f, 1.0f);
  return 1.0f - t * t * t;
}

static float ease_inout(float t)                 /* Smootherstep */
{
  t = clampf(t, 0.0f, 1.0f);
  return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
}

/* The window's corners (top-left, top-right, bottom-right, bottom-left)
 * as the offsets win_effects() wants, from where they are shown
 */

static void corners_to_quad(struct desk_win_s *w, const float *x,
                            const float *y, int16_t q[8])
{
  static const int cx[4] = { 0, 1, 1, 0 };
  static const int cy[4] = { 0, 0, 1, 1 };
  int k;

  for (k = 0; k < 4; k++)
    {
      float ox = x[k] - (float)(w->pos.x + cx[k] * w->size.w);
      float oy = y[k] - (float)(w->pos.y + cy[k] * w->size.h);

      q[2 * k]     = (int16_t)lrintf(clampf(ox, -8000.0f, 8000.0f));
      q[2 * k + 1] = (int16_t)lrintf(clampf(oy, -8000.0f, 8000.0f));
    }
}

/* A w x h rectangle centred at (cx, cy, cz), turned by yaw about its
 * vertical axis, seen from FX_FOCAL in front of (ox, oy): its corners
 */

static void card_project(float w, float h, float cx, float cy, float cz,
                         float yaw, float ox, float oy, float *x, float *y)
{
  static const float cu[4] = { -0.5f, 0.5f, 0.5f, -0.5f };
  static const float cv[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
  float c = cosf(yaw);
  float s = sinf(yaw);
  int k;

  for (k = 0; k < 4; k++)
    {
      float px = cu[k] * w;
      float py = cv[k] * h;
      float x3 = cx + px * c;
      float z3 = cz + px * s;
      float f  = FX_FOCAL / (FX_FOCAL + (z3 > -0.8f * FX_FOCAL ? z3 :
                                         -0.8f * FX_FOCAL));

      x[k] = ox + (x3 - ox) * f;
      y[k] = oy + (cy + py - oy) * f;
    }
}

/****************************************************************************
 * Private Functions: the swing
 ****************************************************************************/

/* The window's corners turned about its pivot (the grab point): roll in
 * its plane, yaw about the vertical, pitch about the horizontal, seen from
 * FX_FOCAL in front of the pivot
 */

static void swing_quad(struct desk_win_s *w, int16_t q[8])
{
  static const int cx[4] = { 0, 1, 1, 0 };
  static const int cy[4] = { 0, 0, 1, 1 };
  float cy_ = cosf(w->fxang[0]);
  float sy  = sinf(w->fxang[0]);
  float cp  = cosf(w->fxang[1]);
  float sp  = sinf(w->fxang[1]);
  float cr  = cosf(w->fxang[2]);
  float sr  = sinf(w->fxang[2]);
  float px  = (float)w->fxpivot.x;
  float py  = (float)w->fxpivot.y;
  int k;

  for (k = 0; k < 4; k++)
    {
      float x  = (float)(cx[k] * w->size.w) - px;
      float y  = (float)(cy[k] * w->size.h) - py;
      float x1 = x * cr - y * sr;
      float y1 = x * sr + y * cr;
      float x2 = x1 * cy_;
      float z2 = x1 * sy;
      float y3 = y1 * cp - z2 * sp;
      float z3 = y1 * sp + z2 * cp;
      float f  = SWING_FOCAL / (SWING_FOCAL + z3);

      q[2 * k]     = (int16_t)lrintf(px + x2 * f - (float)(cx[k] * w->size.w));
      q[2 * k + 1] = (int16_t)lrintf(py + y3 * f - (float)(cy[k] * w->size.h));
    }
}

/* What the angles head for, from a velocity (pixels a second) */

static void swing_target(const float *v, float *t)
{
  t[0] = clampf(-v[0] * SWING_YAW, -SWING_YAWMAX, SWING_YAWMAX);
  t[1] = clampf(-v[1] * SWING_PITCH, -SWING_PITCHMAX * 0.4f, SWING_PITCHMAX);
  t[2] = clampf(v[0] * SWING_ROLL, -SWING_ROLLMAX, SWING_ROLLMAX);
}

/* One frame of a window's swing: true if it is (still) swinging */

static bool swing_step(struct desk_win_s *w, uint32_t now)
{
  float dt = (float)(uint32_t)(now - w->fxms) / 1000.0f;
  float k  = (2.0f * (float)M_PI * SWING_HZ) * (2.0f * (float)M_PI * SWING_HZ);
  float c  = 2.0f * SWING_DAMP * 2.0f * (float)M_PI * SWING_HZ;
  float target[3];
  float a;
  int i;

  w->fxms = now;
  if (g_hold > 0)
    {
      return true;                        /* (Held: as the last move set) */
    }

  dt = clampf(dt, 0.0f, 0.05f);

  /* The velocity, smoothed, from where the drag has moved it since the
   * last frame (none once it is let go)
   */

  if (dt > 0.0f)
    {
      float v[2];

      v[0] = (float)(w->pos.x - w->fxlast.x) / dt;
      v[1] = (float)(w->pos.y - w->fxlast.y) / dt;
      if (!w->fxdrag)
        {
          v[0] = v[1] = 0.0f;
        }

      a = 1.0f - expf(-dt * 1000.0f / SWING_VELMS);
      w->fxv[0] += (v[0] - w->fxv[0]) * a;
      w->fxv[1] += (v[1] - w->fxv[1]) * a;
    }

  w->fxlast = w->pos;
  swing_target(w->fxv, target);

  /* The springs */

  for (; dt > 0.0f; dt -= SWING_STEP)
    {
      float h = dt < SWING_STEP ? dt : SWING_STEP;

      for (i = 0; i < 3; i++)
        {
          w->fxvel[i] += h * (-k * (w->fxang[i] - target[i]) -
                              c * w->fxvel[i]);
          w->fxang[i] += h * w->fxvel[i];
        }
    }

  /* At rest and flat: shown as it is (and done, if let go) */

  {
    bool rest = true;

    for (i = 0; i < 3; i++)
      {
        rest &= fabsf(w->fxang[i]) < SWING_REST &&
                fabsf(w->fxvel[i]) < 10.0f * SWING_REST;
      }

    rest &= fabsf(w->fxv[0]) < 5.0f && fabsf(w->fxv[1]) < 5.0f;
    if (rest)
      {
        memset(w->fxang, 0, sizeof(w->fxang));
        memset(w->fxvel, 0, sizeof(w->fxvel));
        w->fxv[0] = w->fxv[1] = 0.0f;
        w->fxswing = false;
        return w->fxdrag;
      }
  }

  w->fxswing = true;
  return true;
}

/****************************************************************************
 * Private Functions: maximise and restore
 ****************************************************************************/

/* Between the rectangle it had and the one it has, with a tilt about its
 * middle row that peaks halfway
 */

static void zoom_quad(struct desk_win_s *w, float t, int16_t q[8])
{
  static const float cu[4] = { -0.5f, 0.5f, 0.5f, -0.5f };
  static const float cv[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
  const struct nxgl_rect_s *a = &w->fxzoomfrom;
  float e  = ease_inout(t);
  float x0 = a->pt1.x + (w->pos.x - a->pt1.x) * e;
  float y0 = a->pt1.y + (w->pos.y - a->pt1.y) * e;
  float x1 = a->pt2.x + (w->pos.x + w->size.w - a->pt2.x) * e;
  float y1 = a->pt2.y + (w->pos.y + w->size.h - a->pt2.y) * e;
  float tilt = ZOOM_TILT * sinf((float)M_PI * clampf(t, 0.0f, 1.0f));
  float c  = cosf(tilt);
  float s  = sinf(tilt);
  float mx = (x0 + x1) / 2.0f;
  float my = (y0 + y1) / 2.0f;
  float x[4];
  float y[4];
  int k;

  if (!w->maximized)
    {
      s = -s;                             /* Restoring: the other way */
    }

  for (k = 0; k < 4; k++)
    {
      float px = cu[k] * (x1 - x0);
      float py = cv[k] * (y1 - y0);
      float z  = py * s;
      float f  = FX_FOCAL / (FX_FOCAL + z);

      x[k] = mx + px * f;
      y[k] = my + py * c * f;
    }

  corners_to_quad(w, x, y, q);
}

static float zoom_t(struct desk_win_s *w, uint32_t now)
{
  if (g_hold > 0)
    {
      return (float)g_hold / 1000.0f;
    }

  return (float)(uint32_t)(now - w->fxzoomt0) / ZOOM_MS;
}

/****************************************************************************
 * Private Functions: the switcher
 ****************************************************************************/

static int sw_find(struct desk_win_s *w)
{
  int i;

  for (i = 0; i < g_sw.n; i++)
    {
      if (g_sw.card[i].w == w)
        {
          return i;
        }
    }

  return -1;
}

/* Where the carousel shows card i: its corners */

static void sw_pose(int i, float *x, float *y)
{
  struct desk_win_s *w = g_sw.card[i].w;
  float boxw = WORK_W * 0.42f;
  float boxh = WORK_H * 0.52f;
  float s  = fminf(fminf(boxw / w->size.w, boxh / w->size.h), 1.0f);
  float ox = WORK_X + WORK_W / 2.0f;
  float oy = WORK_Y + WORK_H * 0.46f;
  float d  = (float)i - g_sw.pos;
  float ad = fabsf(d);
  float sg = d < 0.0f ? -1.0f : 1.0f;
  float s1 = WORK_W * 0.34f;              /* The first side card, out */
  float s2 = WORK_W * 0.07f;              /* ... the next ones, further */
  float z1 = WORK_W * 0.14f;              /* ... back */
  float cx;
  float cz;
  float yaw;

  if (ad <= 1.0f)
    {
      cx  = d * s1;
      yaw = d * SW_YAW;
      cz  = ad * z1;
    }
  else
    {
      cx  = sg * (s1 + (ad - 1.0f) * s2);
      yaw = sg * SW_YAW;
      cz  = z1 + (ad - 1.0f) * WORK_W * 0.03f;
    }

  card_project(w->size.w * s, w->size.h * s, ox + cx, oy, cz, yaw, ox, oy,
               x, y);
}

/* The cards' stacking: the nearest the selection on top */

static void sw_restack(void)
{
  int order[SW_MAX];
  int i;
  int j;

  for (i = 0; i < g_sw.n; i++)
    {
      order[i] = i;
    }

  for (i = 1; i < g_sw.n; i++)            /* Farthest first */
    {
      int v = order[i];
      float dv = fabsf((float)v - (float)g_sw.sel);

      for (j = i; j > 0 &&
           fabsf((float)order[j - 1] - (float)g_sw.sel) < dv; j--)
        {
          order[j] = order[j - 1];
        }

      order[j] = v;
    }

  for (i = 0; i < g_sw.n; i++)
    {
      nxtk_raise(g_sw.card[order[i]].w->hwnd);
    }
}

/* The label: the selected window's title under the middle card; flat,
 * the windows' icons in a row with the selected one lit, and its title
 */

static void sw_label(void)
{
  struct desk_win_s *w;
  const char *title;
  int tw;
  int x;
  int y;
  int w0;
  int h0;
  int first = 0;
  int count = g_sw.n;
  int i;

  if (g_sw.label == NULL || g_sw.n == 0)
    {
      return;
    }

  w     = g_sw.card[g_sw.sel].w;
  title = w->title[0] != '\0' ? w->title : g_apps[w->app]->name;
  tw    = text_width(&g_desk_font_uib, title);
  ops_reset(&g_swops);

  if (g_sw.flat)
    {
      int pitch = SW_ICON + 24;
      int most  = (WORK_W - 48) / pitch;

      if (count > most)
        {
          count = most;
          first = g_sw.sel - count / 2;
          first = first < 0 ? 0 : first + count > g_sw.n ? g_sw.n - count :
                                                           first;
        }

      w0 = count * pitch + 24;
      w0 = w0 < tw + 48 ? tw + 48 : w0;
      h0 = SW_ICON + 24 + 40;
      ops_rrect(&g_swops, 0, 0, w0, h0, LUT_R12, 12, g_theme.popup,
                g_theme.popup_alpha);
      x = (w0 - count * pitch) / 2 + 12;
      for (i = first; i < first + count; i++, x += pitch)
        {
          struct desk_win_s *c = g_sw.card[i].w;

          if (i == g_sw.sel)
            {
              ops_rrect(&g_swops, x - 8, 12 - 8 + 4, SW_ICON + 16,
                        SW_ICON + 16, LUT_R12, 12, C_HILITE,
                        THEMED(60, 40));
            }

          desk_icon_put(&g_swops, c->app, x, 16, SW_ICON,
                        c->minimized && i != g_sw.sel ? 150 : 255);
        }

      ops_text_centre(&g_swops, &g_desk_font_uib, w0 / 2,
                      SW_ICON + 24 + (40 - text_height(&g_desk_font_uib)) / 2
                      - 4, title, g_theme.popup_text, 255);
      x = WORK_X + (WORK_W - w0) / 2;
      y = WORK_Y + (WORK_H - h0) / 2;
    }
  else
    {
      w0 = tw + 40;
      w0 = w0 > WORK_W - 40 ? WORK_W - 40 : w0;
      h0 = SW_LABEL_H;
      ops_rrect(&g_swops, 0, 0, w0, h0, LUT_R12, h0 / 2, g_theme.popup,
                g_theme.popup_alpha);
      ops_text_centre(&g_swops, &g_desk_font_uib, w0 / 2,
                      (h0 - text_height(&g_desk_font_uib)) / 2, title,
                      g_theme.popup_text, 255);
      x = WORK_X + (WORK_W - w0) / 2;
      y = WORK_Y + (int)(WORK_H * 0.46f + WORK_H * 0.25f) + 22;
      y = y + h0 > WORK_Y + WORK_H - 6 ? WORK_Y + WORK_H - 6 - h0 : y;
    }

  if (x != g_sw.lx || y != g_sw.ly || w0 != g_sw.lw || h0 != g_sw.lh)
    {
      struct nxgl_point_s pos;
      struct nxgl_size_s size;

      pos.x  = x;
      pos.y  = y;
      size.w = w0;
      size.h = h0;
      nx_setposition(g_sw.label, &pos);
      nx_setsize(g_sw.label, &size);
      g_sw.lx = x;
      g_sw.ly = y;
      g_sw.lw = w0;
      g_sw.lh = h0;
    }

  ops_setlist(g_sw.label, &g_swops);
}

/* The veil over the wallpaper, under the cards: alpha a */

static void sw_veil(uint8_t a)
{
  struct nxgl_point_s pos;
  struct nxgl_size_s size;

  if (g_sw.stage == NULL || a == g_sw.veil)
    {
      return;
    }

  pos.x  = 0;
  pos.y  = 0;
  size.w = DESK_W;
  size.h = DESK_H;
  nx_setposition(g_sw.stage, &pos);
  nx_setsize(g_sw.stage, &size);
  ops_reset(&g_swops);
  if (a > 0)
    {
      ops_blend(&g_swops, 0, 0, DESK_W, DESK_H, g_theme.scrim, a);
    }

  ops_setlist(g_sw.stage, &g_swops);
  g_sw.veil = a;
}

static NXWINDOW sw_window(uint8_t layer)
{
  struct nx_effects_s fx;
  NXWINDOW hwnd;

  hwnd = nx_openwindow(g_desk.hnx, NXBE_WINDOW_LAYER | NXBE_WINDOW_HIDDEN,
                       &g_desk_partcb, (void *)(uintptr_t)PART_SWITCH);
  if (hwnd != NULL)
    {
      memset(&fx, 0, sizeof(fx));
      fx.layer   = layer;
      fx.opacity = 255;
      nx_seteffects(hwnd, &fx);
    }

  return hwnd;
}

/* Where card i is shown now: home, the carousel, or between */

static void sw_place(int i, float e)
{
  struct sw_card_s *c = &g_sw.card[i];
  float x[4];
  float y[4];
  int k;

  sw_pose(i, x, y);
  for (k = 0; k < 4; k++)
    {
      c->x[k] = c->x0[k] + (x[k] - c->x0[k]) * e;
      c->y[k] = c->y0[k] + (y[k] - c->y0[k]) * e;
    }
}

/* Its home: where it is, or (minimised) its dock icon */

static void sw_home(int i)
{
  struct sw_card_s *c = &g_sw.card[i];
  struct desk_win_s *w = c->w;
  int ix;
  int iy;

  if (c->minimized && c->w != g_sw.chosen)
    {
      float s;

      if (!desk_dock_where(w->app, &ix, &iy))
        {
          ix = g_desk.dockx + g_desk.dockw / 2;
          iy = g_desk.docky + g_desk.dockh / 2;
        }

      s = 32.0f / (float)(w->size.w > w->size.h ? w->size.w : w->size.h);
      card_project(w->size.w * s, w->size.h * s, ix, iy, 0.0f, 0.0f, ix,
                   iy, c->x0, c->y0);
    }
  else
    {
      card_project(w->size.w, w->size.h, w->pos.x + w->size.w / 2.0f,
                   w->pos.y + w->size.h / 2.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                   c->x0, c->y0);
    }
}

static void sw_effects_all(void)
{
  int i;

  for (i = 0; i < g_sw.n; i++)
    {
      desk_win_effects(g_sw.card[i].w);
    }
}

/* Done: the windows as they were, the chosen one forward (or none) */

static void sw_finish(void)
{
  struct desk_win_s *chosen = g_sw.chosen;
  int i;

  g_sw.open    = false;
  g_sw.closing = false;
  for (i = 0; i < g_sw.n; i++)
    {
      struct desk_win_s *w = g_sw.card[i].w;

      if (g_sw.card[i].minimized && w != chosen)
        {
          nxtk_setvisibility(w->hwnd, true);  /* (Hidden again) */
        }

      desk_win_effects(w);
    }

  g_sw.n = 0;
  if (g_sw.label != NULL)
    {
      nx_setvisibility(g_sw.label, true);
    }

  sw_veil(0);
  if (g_sw.stage != NULL)
    {
      nx_setvisibility(g_sw.stage, true);
    }

  g_sw.chosen = NULL;
  desk_dock_draw();
}

/* Close: the stacking as it was, the chosen window on top, active; then
 * (in perspective) the cards fly home
 */

static void sw_close(struct desk_win_s *chosen)
{
  int i;

  if (!g_sw.open || g_sw.closing)
    {
      return;
    }

  g_sw.chosen = chosen;
  for (i = g_sw.n - 1; i >= 0; i--)
    {
      struct desk_win_s *w = g_sw.card[i].w;

      if (!(g_sw.card[i].minimized && w != chosen))
        {
          nxtk_raise(w->hwnd);
        }
    }

  if (chosen != NULL)
    {
      if (chosen->minimized)
        {
          chosen->minimized = false;      /* (Already shown) */
          chosen->restoring = false;
        }

      desk_win_activate(chosen);
    }

  if (g_sw.label != NULL)
    {
      nx_setvisibility(g_sw.label, true);
    }

  if (g_sw.flat)
    {
      sw_finish();
      return;
    }

  /* From where they are now, home */

  for (i = 0; i < g_sw.n; i++)
    {
      struct sw_card_s *c = &g_sw.card[i];

      memcpy(c->xc, c->x, sizeof(c->x));
      memcpy(c->yc, c->y, sizeof(c->y));
      sw_home(i);
    }

  g_sw.closing = true;
  g_sw.t0      = desk_ms();
}

static void sw_select(int sel)
{
  if (g_sw.n == 0)
    {
      return;
    }

  sel = (sel % g_sw.n + g_sw.n) % g_sw.n;
  if (sel == g_sw.sel)
    {
      return;
    }

  g_sw.sel = sel;
  if (!g_sw.flat)
    {
      sw_restack();
    }

  sw_label();
}

static void sw_open(uint8_t mods, bool back)
{
  struct desk_win_s *w;
  int i;

  for (w = g_desk.wins, g_sw.n = 0; w != NULL && g_sw.n < SW_MAX;
       w = w->next)
    {
      if (!w->closing && !w->minimizing)
        {
          struct sw_card_s *c = &g_sw.card[g_sw.n++];

          memset(c, 0, sizeof(*c));
          c->w         = w;
          c->minimized = w->minimized;
        }
    }

  if (g_sw.n == 0)
    {
      return;
    }

  if (g_desk.menu != NULL)
    {
      desk_menu_close();
    }

  if (g_desk.launcher != NULL)
    {
      desk_launcher_toggle();
    }

  if (g_desk.drag != NULL)
    {
      g_desk.drag->fxdrag = false;
      g_desk.drag = NULL;
    }

  g_desk.resize = NULL;
  g_sw.open    = true;
  g_sw.closing = false;
  g_sw.flat    = !desk_fx_on(FX_FLOW);
  g_sw.mods    = mods;
  g_sw.chosen  = NULL;
  g_sw.sel     = g_sw.n > 1 ? (back ? g_sw.n - 1 : 1) : 0;

  /* The active window first, unless it is minimised (the top one is) */

  if (g_sw.card[0].w != g_desk.active && g_sw.n > 1 && !back)
    {
      g_sw.sel = 0;
    }

  g_sw.pos = (float)g_sw.sel;
  g_sw.t   = 0.0f;
  g_sw.shown    = 0.0f;
  g_sw.shownpos = g_sw.pos;
  g_sw.t0  = desk_ms();
  g_sw.ms  = g_sw.t0;
  g_sw.lw  = -1;

  if (g_sw.label == NULL)
    {
      g_sw.label = sw_window(NX_LAYER_OVERLAY);
    }

  if (!g_sw.flat)
    {
      if (g_sw.stage == NULL)
        {
          g_sw.stage = sw_window(NX_LAYER_DESKTOP);
          g_sw.veil  = 255;
        }

      if (g_sw.stage != NULL)
        {
          sw_veil(0);
          nx_setvisibility(g_sw.stage, false);
          nx_raise(g_sw.stage);
        }

      for (i = 0; i < g_sw.n; i++)
        {
          sw_home(i);
          sw_place(i, 0.0f);
        }

      sw_restack();

      /* Minimised windows come out of their icons */

      sw_effects_all();
      for (i = 0; i < g_sw.n; i++)
        {
          if (g_sw.card[i].minimized)
            {
              nxtk_setvisibility(g_sw.card[i].w->hwnd, false);
            }
        }
    }

  sw_label();
  if (g_sw.label != NULL)
    {
      nx_setvisibility(g_sw.label, false);
      nx_raise(g_sw.label);
    }
}

/* One frame of the switcher: true while it moves */

static bool sw_step(uint32_t now)
{
  float dt = clampf((float)(uint32_t)(now - g_sw.ms) / 1000.0f, 0.0f,
                    0.05f);
  bool more = false;
  float e;
  int i;

  g_sw.ms = now;
  if (!g_sw.open || g_sw.flat)
    {
      return false;
    }

  if (g_hold > 0)
    {
      float t = (float)g_hold / 1000.0f;

      g_sw.t   = g_sw.closing ? 1.0f - t : t;
      g_sw.pos = (float)g_sw.sel;
    }
  else if (g_sw.closing)
    {
      g_sw.t = 1.0f - (float)(uint32_t)(now - g_sw.t0) / SW_CLOSE_MS;
    }
  else
    {
      g_sw.t = (float)(uint32_t)(now - g_sw.t0) / SW_OPEN_MS;
      if (fabsf(g_sw.pos - g_sw.sel) > 0.002f)
        {
          g_sw.pos += ((float)g_sw.sel - g_sw.pos) *
                      (1.0f - expf(-dt * 1000.0f / SW_SLIDE_MS));
          more = true;
        }
      else if (g_sw.pos != (float)g_sw.sel)
        {
          g_sw.pos = (float)g_sw.sel;
          more = true;
        }
    }

  if (g_sw.closing && g_sw.t <= 0.0f && g_hold == 0)
    {
      sw_finish();
      return false;
    }

  more |= g_sw.closing || g_sw.t < 1.0f;
  g_sw.t = clampf(g_sw.t, 0.0f, 1.0f);
  if (g_sw.t == g_sw.shown && g_sw.pos == g_sw.shownpos)
    {
      return more || g_hold > 0;          /* (As they are) */
    }

  g_sw.shown    = g_sw.t;
  g_sw.shownpos = g_sw.pos;

  e = g_sw.closing ? ease_inout(g_sw.t) : ease_out(g_sw.t);
  for (i = 0; i < g_sw.n; i++)
    {
      if (g_sw.closing)
        {
          /* Home from where they were when it closed */

          struct sw_card_s *c = &g_sw.card[i];
          int k;

          for (k = 0; k < 4; k++)
            {
              c->x[k] = c->x0[k] + (c->xc[k] - c->x0[k]) * e;
              c->y[k] = c->y0[k] + (c->yc[k] - c->y0[k]) * e;
            }
        }
      else
        {
          sw_place(i, e);
        }
    }

  sw_veil((uint8_t)(e * g_theme.scrim_alpha * 0.7f));
  sw_effects_all();
  return true;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

bool desk_fx_on(int fx)
{
  return g_desk.quad && g_prefs.effects && (g_prefs.fx & fx) != 0;
}

void desk_fx_hold(int t)
{
  g_hold = t < 0 ? 0 : t > 1000 ? 1000 : t;
}

/* Held: the step at which windows' fades (opening, closing, minimising,
 * restoring: steps down to 0) stop, that far of the way; else 0
 */

int desk_fx_fadestop(int steps)
{
  return g_hold > 0 ? (steps + 1) * (1000 - g_hold) / 1000 : 0;
}

/* The quadrilateral an effect shows w as, if any (shadow: whether its
 * shadow goes with it)
 */

bool desk_fx_quad(struct desk_win_s *w, int16_t q[8], bool *shadow)
{
  int i;

  if (g_sw.open && !g_sw.flat && (i = sw_find(w)) >= 0)
    {
      corners_to_quad(w, g_sw.card[i].x, g_sw.card[i].y, q);
      *shadow = false;
      return true;
    }

  if (w->fxzoom && desk_fx_on(FX_ZOOM))
    {
      zoom_quad(w, zoom_t(w, desk_ms()), q);
      *shadow = true;
      return true;
    }

  if (w->fxswing && desk_fx_on(FX_SWING))
    {
      swing_quad(w, q);
      *shadow = true;
      return true;
    }

  return false;
}

/* The switcher shows w (it may not be parked) */

bool desk_fx_shows(struct desk_win_s *w)
{
  return g_sw.open && !g_sw.flat && sw_find(w) >= 0;
}

/* A drag: started (pressed on the title bar at grab), moved, let go */

void desk_fx_drag(struct desk_win_s *w, int how)
{
  if (!desk_fx_on(FX_SWING))
    {
      return;
    }

  switch (how)
    {
      case FX_DRAG_START:
        w->fxdrag   = true;
        w->fxpivot  = g_desk.grab;
        w->fxlast   = w->pos;
        w->fxms     = desk_ms();
        break;

      case FX_DRAG_MOVE:
        if (g_hold > 0)
          {
            /* Held (tests): the angles of this move's velocity as if it
             * took a frame, at once
             */

            float v[2];

            v[0] = (float)(w->pos.x - w->fxlast.x) * 60.0f;
            v[1] = (float)(w->pos.y - w->fxlast.y) * 60.0f;
            swing_target(v, w->fxang);
            w->fxlast  = w->pos;
            w->fxswing = true;
            desk_win_effects(w);
          }
        break;

      default:
        w->fxdrag = false;
        break;
    }
}

/* Maximised or restored: from rectangle from (the frame it had) */

void desk_fx_zoom(struct desk_win_s *w, const struct nxgl_rect_s *from)
{
  if (!desk_fx_on(FX_ZOOM) || desk_fx_shows(w))
    {
      return;
    }

  w->fxzoomfrom = *from;
  w->fxzoomt0   = desk_ms();
  w->fxzoom     = true;
  desk_win_effects(w);
}

/* Every frame: true while any effect moves */

bool desk_fx_animate(void)
{
  struct desk_win_s *w;
  uint32_t now = desk_ms();
  bool more = false;

  for (w = g_desk.wins; w != NULL; w = w->next)
    {
      bool was = w->fxswing || w->fxzoom;
      bool is;

      if (w->closing)
        {
          continue;
        }

      if (w->fxzoom && zoom_t(w, now) >= 1.0f && g_hold == 0)
        {
          w->fxzoom = false;
        }

      if (w->fxswing || w->fxdrag)
        {
          more |= swing_step(w, now);
        }

      /* Shown anew while it moves (held: only as it stops being) */

      is    = w->fxswing || w->fxzoom;
      more |= is;
      if ((was || is) && (g_hold == 0 || was != is) && w->fade == 0 &&
          w->fadeout == 0 && !desk_fx_shows(w))
        {
          desk_win_effects(w);
        }
    }

  more |= sw_step(now);
  return more || (g_sw.open && g_hold > 0);
}

/* A key event: the switcher's if it opens it or is open */

bool desk_switch_key(const struct nx_key_s *k)
{
  bool press = (k->flags & NX_KEY_RELEASE) == 0;
  uint8_t mods = k->flags & (NX_KEY_ALT | NX_KEY_GUI);
  bool shift = (k->flags & NX_KEY_SHIFT) != 0;

  if (!g_sw.open || g_sw.closing)
    {
      if (k->code == NX_KEY_TAB && press && mods != 0)
        {
          sw_open(mods, shift);
          return g_sw.open;
        }

      return false;
    }

  /* Letting go of the modifier that holds it: the chosen one */

  if ((k->flags & g_sw.mods) == 0)
    {
      if (press && k->code == NX_KEY_ESCAPE)
        {
          sw_close(NULL);
          return true;
        }

      sw_close(g_sw.card[g_sw.sel].w);
      return true;
    }

  if (!press)
    {
      return true;
    }

  switch (k->code)
    {
      case NX_KEY_TAB:
        sw_select(g_sw.sel + (shift ? -1 : 1));
        break;

      case NX_KEY_SPECIAL(KEYCODE_LEFT):
      case NX_KEY_SPECIAL(KEYCODE_UP):
        sw_select(g_sw.sel - 1);
        break;

      case NX_KEY_SPECIAL(KEYCODE_RIGHT):
      case NX_KEY_SPECIAL(KEYCODE_DOWN):
        sw_select(g_sw.sel + 1);
        break;

      case NX_KEY_SPECIAL(KEYCODE_HOME):
        sw_select(0);
        break;

      case NX_KEY_SPECIAL(KEYCODE_END):
        sw_select(g_sw.n - 1);
        break;

      case NX_KEY_ENTER:
      case ' ':
        sw_close(g_sw.card[g_sw.sel].w);
        break;

      case NX_KEY_ESCAPE:
        sw_close(NULL);
        break;

      default:
        break;
    }

  return true;
}

/* The pointer (x, y on the display): while it is open, a press on a card
 * chooses it, elsewhere closes it.  True if the switcher took it.
 */

bool desk_switch_mouse(int x, int y, bool press)
{
  int i;

  if (!g_sw.open || g_sw.closing)
    {
      return false;
    }

  if (!press)
    {
      return true;
    }

  if (g_sw.flat)
    {
      int pitch = SW_ICON + 24;
      int most  = (WORK_W - 48) / pitch;
      int count = g_sw.n > most ? most : g_sw.n;
      int first = g_sw.n > most ? g_sw.sel - count / 2 : 0;
      int x0;

      first = first < 0 ? 0 : first + count > g_sw.n ? g_sw.n - count :
                                                       first;
      x0 = g_sw.lx + (g_sw.lw - count * pitch) / 2;
      if (y >= g_sw.ly && y < g_sw.ly + g_sw.lh && x >= x0 &&
          x < x0 + count * pitch)
        {
          sw_close(g_sw.card[first + (x - x0) / pitch].w);
        }
      else
        {
          sw_close(NULL);
        }

      return true;
    }

  /* The card on top there: the nearest the selection first */

  for (i = 0; i < g_sw.n; i++)
    {
      int j;

      for (j = 0; j < g_sw.n; j++)
        {
          struct sw_card_s *c = &g_sw.card[j];
          bool in = true;
          int k;

          if (abs(j - g_sw.sel) != i)
            {
              continue;
            }

          for (k = 0; k < 4 && in; k++)
            {
              float ex = c->x[(k + 1) & 3] - c->x[k];
              float ey = c->y[(k + 1) & 3] - c->y[k];

              in = ex * ((float)y + 0.5f - c->y[k]) -
                   ey * ((float)x + 0.5f - c->x[k]) >= 0.0f;
            }

          if (in)
            {
              sw_close(c->w);
              return true;
            }
        }
    }

  sw_close(NULL);
  return true;
}

bool desk_switch_open(void)
{
  return g_sw.open;
}

/* A window going (closing, or gone): out of the switcher */

void desk_switch_forget(struct desk_win_s *w)
{
  int i = sw_find(w);

  if (i < 0)
    {
      return;
    }

  if (g_sw.chosen == w)
    {
      g_sw.chosen = NULL;
    }

  memmove(&g_sw.card[i], &g_sw.card[i + 1],
          (g_sw.n - i - 1) * sizeof(g_sw.card[0]));
  g_sw.n--;
  if (g_sw.n == 0)
    {
      sw_finish();
      return;
    }

  if (g_sw.sel > i || g_sw.sel >= g_sw.n)
    {
      g_sw.sel--;
    }

  g_sw.sel = g_sw.sel < 0 ? 0 : g_sw.sel;
  if (!g_sw.closing)
    {
      if (!g_sw.flat)
        {
          sw_restack();
        }

      sw_label();
    }
}

/* Something else needs the screen (a new window, a new mode): closed at
 * once, nothing chosen
 */

void desk_switch_cancel(void)
{
  if (g_sw.open)
    {
      if (!g_sw.closing)
        {
          sw_close(NULL);
        }

      sw_finish();
    }
}
