/****************************************************************************
 * apps/graphics/fpcdesk/desk_clock.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clock: four pages in a segmented control, built with the widget kit.
 *
 *   - Clock: an analog clock drawn by the GPU, the time and the date, and
 *     a world clock (cities at fixed offsets from UTC).  The face (its
 *     disc, ticks and numbers) is drawn once into a surface in the GPU's
 *     memory; the hands are anti-aliased runs of pixels (desk_alarm.c)
 *     over a COPY of the face, drawn again only where a hand was or is.
 *     The second hand sweeps as a mechanical watch's does, eight beats a
 *     second, while the window is active; else it ticks once a second.
 *   - Alarms: time, days, label, on or off; added, edited and deleted in
 *     an editor that takes the list's place.
 *   - Stopwatch: start, stop, laps, reset; hundredths every frame while it
 *     runs, nothing when it does not.
 *   - Timer: hours, minutes, seconds, a ring of what is left.
 *
 * The alarms, the timer and the stopwatch are the desktop's (desk_alarm.c):
 * they go on with the window closed.  The clock is what the desktop shows
 * (Settings' time zone; deskctl freeze in tests).  A clock not set since
 * boot is said, with a way to Settings' Date & Time.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "desk_clock.h"
#include "desk_kit.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CLOCK_W        700
#define CLOCK_H        460
#define NWG            200
#define SIDE_W         300             /* The Clock page's right column */
#define PAD            16
#define NPAGES         4
#define ALARM_ROW_H    58
#define ORANGE         RGB(0xff, 0x95, 0x00)

/* The widgets' ids */

enum
{
  C_NONE = 0,
  C_TABS,
  C_NOTSET,
  C_SETTIME,
  C_PAGE,                              /* + page */
  C_FACE = C_PAGE + NPAGES,
  C_TIME,
  C_DATE,
  C_ZONE,
  C_WORLD,
  C_ADDCITY,
  C_DELCITY,
  C_ALIST,
  C_ADDALARM,
  C_NOALARMS,
  C_ACARD,
  C_AROW,                              /* + alarm */
  C_ATIME = C_AROW + CLK_ALARMS_MAX,   /* + alarm */
  C_ADESC = C_ATIME + CLK_ALARMS_MAX,  /* + alarm */
  C_AON = C_ADESC + CLK_ALARMS_MAX,    /* + alarm */
  C_AEDIT = C_AON + CLK_ALARMS_MAX,    /* + alarm */
  C_EDITOR = C_AEDIT + CLK_ALARMS_MAX,
  C_EHEAD,
  C_EHOUR,
  C_EMIN,
  C_EDAY,                              /* + day, Monday first */
  C_ELABEL = C_EDAY + 7,
  C_EDELETE,
  C_ECANCEL,
  C_ESAVE,
  C_SWTIME,
  C_SWLAP,
  C_SWSTART,
  C_LAPS,
  C_RING,
  C_TH,
  C_TM,
  C_TS,
  C_PRESET,                            /* + preset */
  C_TRESET = C_PRESET + 4,
  C_TSTART
};

/* Menus */

#define M_REGIONS      1
#define M_CITIES       2               /* + region */
#define M_CITYROW      8

/* Dialogs */

#define D_DELETE       1

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct hand_s
{
  float    turn;
  float    len;
  float    tail;
  float    hw;
};

struct clock_s
{
  struct kit_s kit;
  struct kit_widget_s wg[NWG];
  struct kit_list_s world;
  struct kit_list_s laps;
  struct ui_field_s label;
  int      page;

  /* The face: a surface of its radius and colours */

  struct kit_surface_s face;
  bool     hasface;
  uint32_t facekey;
  int      r;                          /* Its radius, centre (frame) */
  int      cx;
  int      cy;
  struct hand_s hand[3];               /* Hour, minute, second: drawn */

  /* The timer's ring as drawn */

  float    ringturn;
  char     ringtext[16];
  char     ringsub[24];

  /* The alarm being edited: its index, -1 a new one */

  int      edit;
  int      eh;
  int      em;
  int      edays;

  /* Region of the cities' menu; the minute the world clock shows */

  int      region;
  int      lastmin;

  /* The texts the widgets show */

  char     time[16];
  char     date[48];
  char     zone[48];
  char     atime[CLK_ALARMS_MAX][16];
  char     adesc[CLK_ALARMS_MAX][80];
  char     ehour[8];
  char     emin[8];
  char     swtime[24];
  char     th[8];
  char     tm[8];
  char     ts[8];
  const char *menu[24];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct clock_s *g_cl;           /* While the window is open */

static const char * const g_pages[NPAGES] =
{
  "Clock", "Alarms", "Stopwatch", "Timer"
};

static const char * const g_regions[3] =
{
  "The Americas", "Europe & Africa", "Asia & Pacific"
};

static const uint8_t g_regionat[4] =  /* Where each starts in the table */
{
  0, 12, 28, 41
};

static const char * const g_dayletter[7] =
{
  "M", "T", "W", "T", "F", "S", "S"
};

static const char * const g_wday[7] =
{
  "Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
  "Saturday"
};

static const char * const g_month[12] =
{
  "January", "February", "March", "April", "May", "June", "July",
  "August", "September", "October", "November", "December"
};

static const struct kit_col_s g_worldcols[3] =
{
  { "City", 0,  0, 0 },
  { "Day",  110, 0, KIT_COL_DIM },
  { "Time", 76, 0, KIT_COL_RIGHT },
};

static const struct kit_col_s g_lapcols[3] =
{
  { "Lap",      70, 0, KIT_COL_DIM },
  { "Lap time", 0,  0, 0 },
  { "Total",    120, 0, KIT_COL_RIGHT | KIT_COL_DIM },
};

static const uint16_t g_presets[4] =
{
  60, 300, 600, 1500
};

static const char * const g_presetname[4] =
{
  "1 min", "5 min", "10 min", "25 min"
};

/****************************************************************************
 * Private Functions: helpers
 ****************************************************************************/

static struct desk_ops_s *room_kit(void *arg, int n)
{
  return kit_ops(arg, n);
}

struct sroom_s
{
  struct kit_surface_s *s;
  struct desk_ops_s *o;
};

static struct desk_ops_s *room_surface(void *arg, int n)
{
  struct sroom_s *r = arg;

  if (r->o->n + n > NX_GPUOPS_MAX)
    {
      kit_surface_draw(r->s, r->o);
    }

  return r->o;
}

static void set_text(struct clock_s *cl, int id, char *buf, size_t size,
                     const char *fmt, ...)
{
  char tmp[96];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (strcmp(tmp, buf) != 0)
    {
      strlcpy(buf, tmp, size);
      kit_set_text(&cl->kit, id, buf);
    }
}

/* A fixed text (a button's), only if it is another */

static void set_label(struct clock_s *cl, int id, const char *text)
{
  struct kit_widget_s *wg = kit_get(&cl->kit, id);

  if (wg != NULL && (wg->text == NULL || strcmp(wg->text, text) != 0))
    {
      kit_set_text(&cl->kit, id, text);
    }
}

/* "Friday, 2 January 2026" from local seconds */

static void fmt_date(char *buf, size_t len, time_t t)
{
  struct tm tm;

  gmtime_r(&t, &tm);
  snprintf(buf, len, "%s, %d %s %d", g_wday[tm.tm_wday % 7], tm.tm_mday,
           g_month[tm.tm_mon % 12], tm.tm_year + 1900);
}

static void fmt_offset(char *buf, size_t len, int off)
{
  int a = off < 0 ? -off : off;

  snprintf(buf, len, "UTC%c%02d:%02d", off < 0 ? '-' : '+', a / 60, a % 60);
}

/* A duration: h:mm:ss, or m:ss */

static void fmt_hms(char *buf, size_t len, int s)
{
  if (s >= 3600)
    {
      snprintf(buf, len, "%d:%02d:%02d", s / 3600, s / 60 % 60, s % 60);
    }
  else
    {
      snprintf(buf, len, "%d:%02d", s / 60, s % 60);
    }
}

/* The stopwatch's: mm:ss.cc, or h:mm:ss.cc */

static void fmt_sw(char *buf, size_t len, int64_t ms)
{
  int cs = (int)(ms / 10 % 100);
  int s = (int)(ms / 1000);

  if (s >= 3600)
    {
      snprintf(buf, len, "%d:%02d:%02d.%02d", s / 3600, s / 60 % 60, s % 60,
               cs);
    }
  else
    {
      snprintf(buf, len, "%02d:%02d.%02d", s / 60, s % 60, cs);
    }
}

/****************************************************************************
 * Private Functions: the analog clock
 ****************************************************************************/

static uint32_t face_key(int r)
{
  return (uint32_t)r * 2654435761u ^ g_theme.window_bg ^
         (uint32_t)g_theme.card << 16 ^ g_theme.text * 7u ^
         g_theme.text_dim * 131u;
}

/* The face, centred at (cx, cy), in [x0, x1) x [y0, y1): the ground, the
 * disc, its rim, the ticks and the numbers
 */

static void face_paint(clk_room_t room, void *arg, float cx, float cy,
                       int r, int x0, int y0, int x1, int y1)
{
  const struct desk_font_s *f = r >= 110 ? &g_desk_font_title :
                                           &g_desk_font_uib;
  struct desk_ops_s *o;
  float rn = r >= 110 ? r - 34 : r - 24;
  int i;

  o = room(arg, 1);
  ops_fill(o, x0, y0, x1 - x0, y1 - y0, g_theme.window_bg);
  clk_disc(room, arg, cx, cy, r, g_theme.card, 255, x0, y0, x1, y1);
  clk_ring(room, arg, cx, cy, r - 1.5f, r, g_theme.text_faint, 110, x0, y0,
           x1, y1);

  for (i = 0; i < 60; i++)
    {
      float s = clk_sin(i / 60.0f);
      float c = clk_cos(i / 60.0f);
      bool hour = i % 5 == 0;
      float a = r - 7;
      float b = hour ? r - (r >= 110 ? 19 : 15) : r - 12;

      clk_seg(room, arg, cx + a * s, cy - a * c, cx + b * s, cy - b * c,
              hour ? 1.4f : 0.5f, hour ? g_theme.text : g_theme.text_dim,
              255, x0, y0, x1, y1);
    }

  for (i = 1; i <= 12; i++)
    {
      char num[12];
      int tw;
      int th = f->ascent;
      float px = cx + rn * clk_sin(i / 12.0f);
      float py = cy - rn * clk_cos(i / 12.0f);

      snprintf(num, sizeof(num), "%d", i);
      tw = text_width(f, num);
      o  = room(arg, 3);
      ops_text(o, f, (int)(px - tw / 2.0f + 0.5f),
               (int)(py - th / 2.0f + 0.5f), num, g_theme.text, 255);
    }

  if (r >= 90)
    {
      const struct desk_font_s *sf = &g_desk_font_small;

      o = room(arg, 4);
      ops_text(o, sf, (int)cx - text_width(sf, "FPC") / 2,
               (int)(cy + r * 0.32f), "FPC", g_theme.text_faint, 255);
    }
}

/* The face's surface: made for this radius and theme */

static void face_make(struct clock_s *cl)
{
  struct sroom_s room;
  int size = 2 * cl->r + 4;

  if (cl->hasface && (cl->face.w != size || cl->face.lost))
    {
      kit_surface_close(&cl->face);
      cl->hasface = false;
    }

  if (!cl->hasface)
    {
      if (kit_surface_open(&cl->face, size, size, cl->kit.w) < 0)
        {
          return;
        }

      cl->hasface = true;
      cl->facekey = 0;
    }

  if (cl->facekey == face_key(cl->r))
    {
      return;
    }

  room.s = &cl->face;
  room.o = malloc(sizeof(*room.o));               /* Now and then only */
  if (room.o == NULL)
    {
      return;
    }

  ops_reset(room.o);
  face_paint(room_surface, &room, size / 2.0f, size / 2.0f, cl->r, 0, 0,
             size, size);
  kit_surface_draw(&cl->face, room.o);
  free(room.o);
  cl->facekey = face_key(cl->r);
}

static void hand_ends(struct clock_s *cl, const struct hand_s *h, float *ax,
                      float *ay, float *bx, float *by)
{
  float s = clk_sin(h->turn);
  float c = clk_cos(h->turn);

  *ax = cl->cx - h->tail * s;
  *ay = cl->cy + h->tail * c;
  *bx = cl->cx + h->len * s;
  *by = cl->cy - h->len * c;
}

static void hand_box(struct clock_s *cl, const struct hand_s *h,
                     struct kit_rect_s *r)
{
  float ax;
  float ay;
  float bx;
  float by;
  int m = (int)h->hw + 3;
  int x0;
  int y0;
  int x1;
  int y1;

  hand_ends(cl, h, &ax, &ay, &bx, &by);
  x0 = (int)(ax < bx ? ax : bx) - m;
  y0 = (int)(ay < by ? ay : by) - m;
  x1 = (int)(ax < bx ? bx : ax) + m + 1;
  y1 = (int)(ay < by ? by : ay) + m + 1;

  /* The pivot's caps */

  x0 = x0 < cl->cx - 8 ? x0 : cl->cx - 8;
  y0 = y0 < cl->cy - 8 ? y0 : cl->cy - 8;
  x1 = x1 > cl->cx + 8 ? x1 : cl->cx + 8;
  y1 = y1 > cl->cy + 8 ? y1 : cl->cy + 8;
  r->x = x0;
  r->y = y0;
  r->w = x1 - x0;
  r->h = y1 - y0;
}

/* The hands for now, their turns */

static void hands_now(struct clock_s *cl, struct hand_s *h)
{
  int ms;
  time_t t = clk_local(&ms);
  float r = cl->r;
  float sec = (t % 60) + (ms / 125) / 8.0f;   /* Eight beats a second */
  float min = (t / 60 % 60) + sec / 60;
  float hour = (t / 3600 % 12) + min / 60;

  h[0].turn = hour / 12;
  h[0].len  = r * 0.5f;
  h[0].tail = r * 0.1f;
  h[0].hw   = r >= 100 ? 3.2f : 2.4f;
  h[1].turn = min / 60;
  h[1].len  = r * 0.76f;
  h[1].tail = r * 0.1f;
  h[1].hw   = r >= 100 ? 2.2f : 1.6f;
  h[2].turn = sec / 60;
  h[2].len  = r * 0.86f;
  h[2].tail = r * 0.18f;
  h[2].hw   = 0.75f;
}

static void face_geometry(struct clock_s *cl, struct kit_widget_s *wg)
{
  int s = (wg->w < wg->h ? wg->w : wg->h) - 12;

  cl->r  = s / 2 < 40 ? 40 : s / 2;
  cl->cx = wg->x + wg->w / 2;
  cl->cy = wg->y + wg->h / 2;
}

static void face_draw(struct clock_s *cl, struct kit_widget_s *wg, int x0,
                      int y0, int x1, int y1)
{
  struct kit_s *k = &cl->kit;
  struct desk_ops_s *o;
  int size;
  int fx;
  int fy;
  int i;

  face_geometry(cl, wg);
  if (cl->hand[0].len == 0)
    {
      hands_now(cl, cl->hand);
    }

  face_make(cl);
  size = 2 * cl->r + 4;
  fx = cl->cx - size / 2;
  fy = cl->cy - size / 2;

  if (cl->hasface && cl->facekey == face_key(cl->r))
    {
      /* The ground around the face, then the face: one COPY */

      o = kit_ops(k, 5);
      ops_fill(o, wg->x, wg->y, wg->w, fy - wg->y, g_theme.window_bg);
      ops_fill(o, wg->x, fy + size, wg->w, wg->y + wg->h - fy - size,
               g_theme.window_bg);
      ops_fill(o, wg->x, fy, fx - wg->x, size, g_theme.window_bg);
      ops_fill(o, fx + size, fy, wg->x + wg->w - fx - size, size,
               g_theme.window_bg);
      kit_surface_copy(o, &cl->face, 0, 0, size, size, fx, fy);
    }
  else
    {
      /* No room in the GPU's memory: drawn here, slowly */

      face_paint(room_kit, k, cl->cx, cl->cy, cl->r, x0, y0, x1, y1);
    }

  for (i = 0; i < 3; i++)
    {
      struct hand_s *h = &cl->hand[i];
      float ax;
      float ay;
      float bx;
      float by;

      hand_ends(cl, h, &ax, &ay, &bx, &by);
      if (i == 2)
        {
          clk_disc(room_kit, k, cl->cx, cl->cy, 5.5f, g_theme.text, 255,
                   x0, y0, x1, y1);
        }

      clk_seg(room_kit, k, ax, ay, bx, by, h->hw,
              i == 2 ? ORANGE : g_theme.text, 255, x0, y0, x1, y1);
    }

  clk_disc(room_kit, k, cl->cx, cl->cy, 3.5f, ORANGE, 255, x0, y0, x1, y1);
  clk_disc(room_kit, k, cl->cx, cl->cy, 1.2f, g_theme.card, 255, x0, y0,
           x1, y1);
}

static void rect_union(struct kit_rect_s *a, const struct kit_rect_s *b)
{
  int x0 = a->x < b->x ? a->x : b->x;
  int y0 = a->y < b->y ? a->y : b->y;
  int x1 = a->x + a->w > b->x + b->w ? a->x + a->w : b->x + b->w;
  int y1 = a->y + a->h > b->y + b->h ? a->y + a->h : b->y + b->h;

  a->x = x0;
  a->y = y0;
  a->w = x1 - x0;
  a->h = y1 - y0;
}

/* Move the hands to now: only where a hand moved by half a pixel at its
 * tip is drawn again
 */

static void face_update(struct clock_s *cl)
{
  struct hand_s now[3];
  struct kit_rect_s dmg;
  bool any = false;
  int i;

  if (cl->page != 0 || cl->hand[0].len == 0)
    {
      return;
    }

  hands_now(cl, now);
  for (i = 0; i < 3; i++)
    {
      float d = now[i].turn - cl->hand[i].turn;
      struct kit_rect_s r;

      d = d < 0 ? -d : d;
      d = d > 0.5f ? 1 - d : d;
      if (d * 6.2832f * now[i].len < 0.5f && now[i].len == cl->hand[i].len)
        {
          continue;
        }

      hand_box(cl, &cl->hand[i], &r);
      if (!any)
        {
          dmg = r;
        }
      else
        {
          rect_union(&dmg, &r);
        }

      hand_box(cl, &now[i], &r);
      rect_union(&dmg, &r);
      cl->hand[i] = now[i];
      any = true;
    }

  if (any)
    {
      kit_damage(&cl->kit, dmg.x, dmg.y, dmg.w, dmg.h);
    }
}

/****************************************************************************
 * Private Functions: the timer's ring
 ****************************************************************************/

struct ringgeo_s
{
  float    cx;
  float    cy;
  float    ri;
  float    ro;
};

static void ring_geometry(struct kit_widget_s *wg, struct ringgeo_s *g)
{
  int s = (wg->w < wg->h ? wg->w : wg->h) - 16;
  float ro = s / 2 < 50 ? 50 : s / 2;

  g->cx = wg->x + wg->w / 2;
  g->cy = wg->y + wg->h / 2;
  g->ro = ro;
  g->ri = ro - (ro >= 120 ? 12 : 9);
}

static float ring_turn(void)
{
  int64_t set = (int64_t)g_clk.timer_set * 1000;
  int64_t left = clk_timer_left();

  if (g_clk.timer_state == CLK_STOPPED || set <= 0)
    {
      return 1;
    }

  return (float)left / (float)set;
}

static void ring_texts(struct clock_s *cl, char *main, size_t ml, char *sub,
                       size_t sl)
{
  int64_t left = clk_timer_left();

  fmt_hms(main, ml, (int)((left + 999) / 1000));
  if (g_clk.timer_state == CLK_PAUSED)
    {
      snprintf(sub, sl, "Paused");
    }
  else if (g_clk.timer_state == CLK_RUNNING)
    {
      char t[16];

      fmt_hms(t, sizeof(t), g_clk.timer_set);
      snprintf(sub, sl, "of %s", t);
    }
  else
    {
      snprintf(sub, sl, "Timer");
    }
}

static void ring_textrect(struct ringgeo_s *g, struct kit_rect_s *r)
{
  r->w = (int)(g->ri * 1.6f);
  r->h = 80;
  r->x = (int)g->cx - r->w / 2;
  r->y = (int)g->cy - 44;
}

static void ring_draw(struct clock_s *cl, struct kit_widget_s *wg, int x0,
                      int y0, int x1, int y1)
{
  struct kit_s *k = &cl->kit;
  struct ringgeo_s g;
  struct kit_rect_s tr;
  struct desk_ops_s *o;

  ring_geometry(wg, &g);
  cl->ringturn = ring_turn();
  ring_texts(cl, cl->ringtext, sizeof(cl->ringtext), cl->ringsub,
             sizeof(cl->ringsub));

  o = kit_ops(k, 1);
  ops_fill(o, x0, y0, x1 - x0, y1 - y0, g_theme.window_bg);
  if (cl->ringturn < 1)
    {
      clk_ring(room_kit, k, g.cx, g.cy, g.ri, g.ro, g_theme.control, 255,
               x0, y0, x1, y1);
    }

  clk_arc(room_kit, k, g.cx, g.cy, g.ri, g.ro, cl->ringturn,
          g_clk.timer_state == CLK_PAUSED ? g_theme.text_faint :
          g_theme.accent, 255, x0, y0, x1, y1);

  ring_textrect(&g, &tr);
  if (x0 < tr.x + tr.w && tr.x < x1 && y0 < tr.y + tr.h && tr.y < y1)
    {
      const struct desk_font_s *big = &g_desk_font_big;
      const struct desk_font_s *sm = &g_desk_font_ui;

      o = kit_ops(k, 24);
      ops_text_centre(o, big, (int)g.cx, (int)g.cy - 38, cl->ringtext,
                      g_theme.text, 255);
      ops_text_centre(o, sm, (int)g.cx, (int)g.cy + 18, cl->ringsub,
                      g_theme.text_dim, 255);
    }
}

/* The ring and its text to now: what changed drawn again */

static void ring_update(struct clock_s *cl)
{
  struct kit_widget_s *wg = kit_get(&cl->kit, C_RING);
  struct ringgeo_s g;
  struct kit_rect_s r;
  char text[16];
  char sub[24];
  float t1;
  float t0;
  float rm;
  float rc;
  int i;

  if (wg == NULL || cl->page != 3 || wg->w <= 0)
    {
      return;
    }

  ring_geometry(wg, &g);
  ring_texts(cl, text, sizeof(text), sub, sizeof(sub));
  if (strcmp(text, cl->ringtext) != 0 || strcmp(sub, cl->ringsub) != 0)
    {
      ring_textrect(&g, &r);
      kit_damage(&cl->kit, r.x, r.y, r.w, r.h);
    }

  t1 = ring_turn();
  t0 = cl->ringturn;
  rm = (g.ri + g.ro) / 2;
  rc = (g.ro - g.ri) / 2 + 3;
  if ((t1 - t0) * (t1 - t0) * 39.5f * rm * rm < 0.25f)
    {
      return;                        /* Moved less than half a pixel */
    }

  if (t1 >= 1 || t0 >= 1 || t1 - t0 > 0.2f || t0 - t1 > 0.2f)
    {
      kit_invalidate(&cl->kit, C_RING);   /* All of it */
      return;
    }

  /* The arc's end moved from t0 to t1: its box, the round end's too */

  r.x = 32767;
  r.y = 32767;
  r.w = -32767;
  r.h = -32767;
  for (i = 0; i <= 8; i++)
    {
      float t = t0 + (t1 - t0) * i / 8;
      int px = (int)(g.cx + rm * clk_sin(t));
      int py = (int)(g.cy - rm * clk_cos(t));

      r.x = px - rc < r.x ? px - rc : r.x;
      r.y = py - rc < r.y ? py - rc : r.y;
      r.w = px + rc > r.w ? px + rc : r.w;       /* (x1 for now) */
      r.h = py + rc > r.h ? py + rc : r.h;
    }

  kit_damage(&cl->kit, r.x, r.y, r.w - r.x + 1, r.h - r.y + 1);
}

/****************************************************************************
 * Private Functions: the lists
 ****************************************************************************/

static const char *world_text(struct kit_s *k, struct kit_widget_s *wg,
                              int row, int col, char *buf, int size)
{
  const struct clk_city_s *c = &g_clk_cities[g_clk.city[row]];
  time_t local = clk_local(NULL);
  time_t there = local + (time_t)(c->off - g_prefs.tzoff) * 60;
  int dd = (int)(there / 86400 - local / 86400);
  int diff = c->off - g_prefs.tzoff;
  int a = diff < 0 ? -diff : diff;
  char h[12];

  switch (col)
    {
      case 0:
        return c->name;

      case 1:
        if (a % 60 != 0)
          {
            snprintf(h, sizeof(h), "%c%d:%02d", diff < 0 ? '-' : '+',
                     a / 60, a % 60);
          }
        else
          {
            snprintf(h, sizeof(h), "%c%dh", diff < 0 ? '-' : '+', a / 60);
          }

        snprintf(buf, size, "%s, %s", dd > 0 ? "Tomorrow" :
                 dd < 0 ? "Yesterday" : "Today", h);
        return buf;

      default:
        clk_fmt_hm(buf, size, (int)(there / 3600 % 24),
                   (int)(there / 60 % 60));
        return buf;
    }
}

static const char *lap_text(struct kit_s *k, struct kit_widget_s *wg,
                            int row, int col, char *buf, int size)
{
  int i = g_clk.nlaps - 1 - row;             /* The latest first */

  switch (col)
    {
      case 0:
        snprintf(buf, size, "Lap %d", i + 1);
        break;

      case 1:
        fmt_sw(buf, size, g_clk.lap[i] - (i > 0 ? g_clk.lap[i - 1] : 0));
        break;

      default:
        fmt_sw(buf, size, g_clk.lap[i]);
        break;
    }

  return buf;
}

/****************************************************************************
 * Private Functions: keeping the widgets up to date
 ****************************************************************************/

static void alarms_sync(struct clock_s *cl)
{
  struct kit_s *k = &cl->kit;
  int i;

  kit_show(k, C_NOALARMS, g_clk.nalarms == 0);
  kit_show(k, C_ACARD, g_clk.nalarms > 0);
  kit_enable(k, C_ADDALARM, g_clk.nalarms < CLK_ALARMS_MAX);
  for (i = 0; i < CLK_ALARMS_MAX; i++)
    {
      const struct clk_alarm_s *a = &g_clk.alarm[i];
      char hm[16];
      char days[48];

      kit_show(k, C_AROW + i, i < g_clk.nalarms);
      if (i >= g_clk.nalarms)
        {
          continue;
        }

      clk_fmt_hm(hm, sizeof(hm), a->hour, a->min);
      set_text(cl, C_ATIME + i, cl->atime[i], sizeof(cl->atime[i]), "%s",
               hm);
      clk_fmt_days(days, sizeof(days), a->days);
      set_text(cl, C_ADESC + i, cl->adesc[i], sizeof(cl->adesc[i]),
               "%s%s%s%s", a->label[0] ? a->label : "Alarm", " " DESK_CH_MIDDOT
               " ", days, a->snooze != 0 ? " " DESK_CH_MIDDOT " Snoozed" :
               "");
      kit_set_value(k, C_AON + i, a->on);
    }
}

static void editor_sync(struct clock_s *cl)
{
  struct kit_s *k = &cl->kit;
  int i;

  if (g_prefs.clock24)
    {
      set_text(cl, C_EHOUR, cl->ehour, sizeof(cl->ehour), "%02d", cl->eh);
    }
  else
    {
      set_text(cl, C_EHOUR, cl->ehour, sizeof(cl->ehour), "%d %s",
               (cl->eh + 11) % 12 + 1, cl->eh < 12 ? "AM" : "PM");
    }

  set_text(cl, C_EMIN, cl->emin, sizeof(cl->emin), "%02d", cl->em);
  for (i = 0; i < 7; i++)
    {
      kit_set_value(k, C_EDAY + i, (cl->edays >> ((i + 1) % 7)) & 1);
    }
}

static void timer_sync(struct clock_s *cl)
{
  struct kit_s *k = &cl->kit;
  int s = g_clk.timer_set;
  bool stopped = g_clk.timer_state == CLK_STOPPED;

  set_text(cl, C_TH, cl->th, sizeof(cl->th), "%d h", s / 3600);
  set_text(cl, C_TM, cl->tm, sizeof(cl->tm), "%d min", s / 60 % 60);
  set_text(cl, C_TS, cl->ts, sizeof(cl->ts), "%d s", s % 60);
  kit_enable(k, C_TH, stopped);
  kit_enable(k, C_TM, stopped);
  kit_enable(k, C_TS, stopped);
  set_label(cl, C_TSTART, g_clk.timer_state == CLK_RUNNING ? "Pause" :
            g_clk.timer_state == CLK_PAUSED ? "Resume" : "Start");
  kit_enable(k, C_TSTART, s > 0);
  kit_enable(k, C_TRESET, !stopped);
}

static void sw_sync(struct clock_s *cl)
{
  struct kit_s *k = &cl->kit;
  char buf[24];
  bool running = g_clk.sw_state == CLK_RUNNING;

  fmt_sw(buf, sizeof(buf), clk_sw_elapsed());
  set_text(cl, C_SWTIME, cl->swtime, sizeof(cl->swtime), "%s", buf);
  set_label(cl, C_SWSTART, running ? "Stop" : "Start");
  kit_set_style(k, C_SWSTART, running ? KIT_BTN_DANGER : KIT_BTN_DEFAULT);
  set_label(cl, C_SWLAP, running || g_clk.sw_state == CLK_STOPPED ?
            "Lap" : "Reset");
  kit_enable(k, C_SWLAP, running || g_clk.sw_state == CLK_PAUSED);
  if (cl->laps.n != g_clk.nlaps)
    {
      cl->laps.n = g_clk.nlaps;
      kit_list_changed(k, C_LAPS);
    }
}

static void clock_sync(struct clock_s *cl)
{
  struct kit_s *k = &cl->kit;
  struct tm tm;
  time_t t = clk_local(NULL);
  char buf[48];

  kit_show(k, C_NOTSET, !clk_time_set());

  gmtime_r(&t, &tm);
  if (g_prefs.clock24)
    {
      snprintf(buf, sizeof(buf), "%02d:%02d:%02d", tm.tm_hour, tm.tm_min,
               tm.tm_sec);
    }
  else
    {
      snprintf(buf, sizeof(buf), "%d:%02d:%02d %s",
               (tm.tm_hour + 11) % 12 + 1, tm.tm_min, tm.tm_sec,
               tm.tm_hour < 12 ? "AM" : "PM");
    }

  set_text(cl, C_TIME, cl->time, sizeof(cl->time), "%s", buf);
  fmt_date(buf, sizeof(buf), t);
  set_text(cl, C_DATE, cl->date, sizeof(cl->date), "%s", buf);
  fmt_offset(buf, sizeof(buf), g_prefs.tzoff);
  set_text(cl, C_ZONE, cl->zone, sizeof(cl->zone), "Local time, %s", buf);

  if (cl->world.n != g_clk.ncities)
    {
      cl->world.n = g_clk.ncities;
      if (cl->world.sel >= cl->world.n)
        {
          cl->world.sel = -1;
        }

      kit_list_changed(k, C_WORLD);
    }
  else if (cl->lastmin != (int)(t / 60))
    {
      int i;

      for (i = 0; i < cl->world.n; i++)
        {
          kit_list_row(k, C_WORLD, i);          /* Their minutes */
        }
    }

  cl->lastmin = (int)(t / 60);
  kit_enable(k, C_DELCITY, cl->world.sel >= 0);
  kit_enable(k, C_ADDCITY, g_clk.ncities < CLK_CITIES_MAX);

  alarms_sync(cl);
  sw_sync(cl);
  timer_sync(cl);
}

/* Frames while something moves where it shows and the window is active
 * (not while the clock is frozen: tests)
 */

static void frames_update(struct clock_s *cl)
{
  struct desk_win_s *w = cl->kit.w;

  w->frames = !desk_frozen() && w->active && !w->minimized &&
              ((cl->page == 0) ||
               (cl->page == 2 && g_clk.sw_state == CLK_RUNNING) ||
               (cl->page == 3 && g_clk.timer_state == CLK_RUNNING));
}

static void show_page(struct clock_s *cl, int page)
{
  int i;

  cl->page = page;
  for (i = 0; i < NPAGES; i++)
    {
      kit_show(&cl->kit, C_PAGE + i, i == page);
    }

  kit_set_value(&cl->kit, C_TABS, page);
  if (page == 0)
    {
      cl->hand[0].len = 0;                /* Now, when drawn */
    }

  frames_update(cl);
}

/****************************************************************************
 * Private Functions: acting
 ****************************************************************************/

static void editor_open(struct clock_s *cl, int i)
{
  struct kit_s *k = &cl->kit;

  cl->edit = i;
  if (i >= 0)
    {
      cl->eh    = g_clk.alarm[i].hour;
      cl->em    = g_clk.alarm[i].min;
      cl->edays = g_clk.alarm[i].days;
      ui_field_set(&cl->label, g_clk.alarm[i].label);
    }
  else
    {
      struct tm tm;
      time_t t = clk_local(NULL);

      gmtime_r(&t, &tm);
      cl->eh    = tm.tm_hour;
      cl->em    = tm.tm_min;
      cl->edays = 0;
      ui_field_set(&cl->label, "");
    }

  kit_set_text(k, C_EHEAD, i >= 0 ? "Edit Alarm" : "New Alarm");
  kit_show(k, C_EDELETE, i >= 0);
  kit_show(k, C_ALIST, false);
  kit_show(k, C_ADDALARM, false);
  kit_show(k, C_EDITOR, true);
  kit_invalidate(k, C_ELABEL);
  editor_sync(cl);
  kit_focus(k, C_ELABEL);
}

static void editor_close(struct clock_s *cl)
{
  struct kit_s *k = &cl->kit;

  kit_show(k, C_EDITOR, false);
  kit_show(k, C_ALIST, true);
  kit_show(k, C_ADDALARM, true);
  alarms_sync(cl);
}

static void alarm_delete(int i)
{
  if (i < 0 || i >= g_clk.nalarms)
    {
      return;
    }

  g_clk.nalarms--;
  memmove(&g_clk.alarm[i], &g_clk.alarm[i + 1],
          (g_clk.nalarms - i) * sizeof(g_clk.alarm[0]));
  desk_notify_forget(i);
  clk_save();
}

static void editor_save(struct clock_s *cl)
{
  struct clk_alarm_s *a;

  if (cl->edit >= 0 && cl->edit < g_clk.nalarms)
    {
      a = &g_clk.alarm[cl->edit];
    }
  else if (g_clk.nalarms < CLK_ALARMS_MAX)
    {
      a = &g_clk.alarm[g_clk.nalarms++];
    }
  else
    {
      return;
    }

  a->hour   = cl->eh;
  a->min    = cl->em;
  a->days   = cl->edays;
  a->on     = true;
  a->snooze = 0;
  strlcpy(a->label, cl->label.text, sizeof(a->label));
  clk_save();
}

static void cities_menu(struct clock_s *cl, int region)
{
  struct kit_widget_s *wg = kit_get(&cl->kit, C_ADDCITY);
  uint32_t disabled = 0;
  int n = 0;
  int i;
  int j;

  if (wg == NULL)
    {
      return;
    }

  if (region < 0)
    {
      kit_menu(&cl->kit, M_REGIONS, wg->x, wg->y + wg->h + 2, g_regions, 3,
               0);
      return;
    }

  cl->region = region;
  for (i = g_regionat[region]; i < g_regionat[region + 1] && n < 24; i++)
    {
      for (j = 0; j < g_clk.ncities; j++)
        {
          if (g_clk.city[j] == i)
            {
              disabled |= 1u << n;
            }
        }

      cl->menu[n++] = g_clk_cities[i].name;
    }

  kit_menu(&cl->kit, M_CITIES + region, wg->x, wg->y + wg->h + 2, cl->menu,
           n, disabled);
}

static void clock_event(struct kit_s *k, struct kit_widget_s *wg, int ev)
{
  struct clock_s *cl = k->priv;
  int id = wg != NULL ? wg->id : 0;
  int i;

  switch (ev)
    {
      case KIT_EV_CHANGE:
        if (id == C_TABS)
          {
            show_page(cl, wg->value);
          }
        else if (id == C_WORLD)
          {
            kit_enable(k, C_DELCITY, cl->world.sel >= 0);
          }
        else if (id >= C_AON && id < C_AON + CLK_ALARMS_MAX)
          {
            i = id - C_AON;
            g_clk.alarm[i].on = wg->value != 0;
            g_clk.alarm[i].snooze = 0;
            clk_save();
          }
        else if (id == C_EHOUR)
          {
            cl->eh = (cl->eh + 24 + k->arg) % 24;
          }
        else if (id == C_EMIN)
          {
            cl->em = (cl->em + 60 + k->arg) % 60;
          }
        else if (id == C_TH || id == C_TM || id == C_TS)
          {
            int s = g_clk.timer_set;
            int h = s / 3600;
            int m = s / 60 % 60;

            s %= 60;
            if (id == C_TH)
              {
                h = (h + 24 + k->arg) % 24;
              }
            else if (id == C_TM)
              {
                m = (m + 60 + k->arg) % 60;
              }
            else
              {
                s = (s + 60 + k->arg * 5) % 60;
              }

            g_clk.timer_set = h * 3600 + m * 60 + s;
            clk_timer_reset();
            clk_save();
            kit_invalidate(k, C_RING);
          }
        break;

      case KIT_EV_CLICK:
        if (id == C_SETTIME)
          {
            desk_settings_show(SETTINGS_DATETIME);
            return;
          }
        else if (id == C_ADDCITY)
          {
            cities_menu(cl, -1);
          }
        else if (id == C_DELCITY && cl->world.sel >= 0 &&
                 cl->world.sel < g_clk.ncities)
          {
            i = cl->world.sel;
            g_clk.ncities--;
            memmove(&g_clk.city[i], &g_clk.city[i + 1],
                    g_clk.ncities - i);
            cl->world.sel = -1;
            clk_save();
          }
        else if (id == C_ADDALARM)
          {
            editor_open(cl, -1);
          }
        else if (id >= C_AEDIT && id < C_AEDIT + CLK_ALARMS_MAX)
          {
            editor_open(cl, id - C_AEDIT);
          }
        else if (id >= C_EDAY && id < C_EDAY + 7)
          {
            cl->edays ^= 1 << ((id - C_EDAY + 1) % 7);
          }
        else if (id == C_ECANCEL)
          {
            editor_close(cl);
          }
        else if (id == C_ESAVE)
          {
            editor_save(cl);
            editor_close(cl);
          }
        else if (id == C_EDELETE)
          {
            static const char * const buttons[] =
            {
              "Delete", "Cancel"
            };

            kit_dialog(k, D_DELETE, KIT_DLG_CONFIRM | KIT_DLG_DANGER,
                       "Delete this alarm?", cl->label.text[0] ?
                       cl->label.text : "It will not ring again.",
                       buttons, 2);
          }
        else if (id == C_SWSTART)
          {
            clk_sw_toggle();
          }
        else if (id == C_SWLAP)
          {
            clk_sw_lap_reset();
          }
        else if (id == C_TSTART)
          {
            if (g_clk.timer_state == CLK_RUNNING)
              {
                clk_timer_pause();
              }
            else
              {
                clk_timer_start();
              }

            kit_invalidate(k, C_RING);
          }
        else if (id == C_TRESET)
          {
            clk_timer_reset();
            kit_invalidate(k, C_RING);
          }
        else if (id >= C_PRESET && id < C_PRESET + 4)
          {
            g_clk.timer_set = g_presets[id - C_PRESET];
            clk_timer_reset();
            clk_save();
            kit_invalidate(k, C_RING);
          }
        break;

      case KIT_EV_MENU:
        if (k->key == M_REGIONS && k->arg >= 0)
          {
            cities_menu(cl, k->arg);
            return;
          }

        if (k->key >= M_CITIES && k->key < M_CITIES + 3 && k->arg >= 0 &&
            g_clk.ncities < CLK_CITIES_MAX)
          {
            g_clk.city[g_clk.ncities++] = g_regionat[k->key - M_CITIES] +
                                          k->arg;
            clk_save();
          }
        break;

      case KIT_EV_ACTIVATE:
        if (id == C_ELABEL)                   /* Enter in the label */
          {
            editor_save(cl);
            editor_close(cl);
          }
        break;

      case KIT_EV_DIALOG:
        if (k->key == D_DELETE && k->arg == KIT_ANS_OK)
          {
            alarm_delete(cl->edit);
            editor_close(cl);
          }
        break;

      case KIT_EV_KEY:
        if (k->key == ' ' && cl->page == 2)
          {
            clk_sw_toggle();
            k->key = 0;
          }
        else if (k->key == ' ' && cl->page == 3)
          {
            if (g_clk.timer_state == CLK_RUNNING)
              {
                clk_timer_pause();
              }
            else
              {
                clk_timer_start();
              }

            kit_invalidate(k, C_RING);
            k->key = 0;
          }
        else if (k->key == KEY_ESC && kit_get(k, C_EDITOR) != NULL &&
                 !(kit_get(k, C_EDITOR)->flags & KIT_F_HIDDEN))
          {
            editor_close(cl);
            k->key = 0;
          }
        break;

      default:
        break;
    }

  clock_sync(cl);
  if (cl->page == 2 && ev == KIT_EV_CLICK)
    {
      kit_list_changed(k, C_LAPS);
    }

  frames_update(cl);
}

static void clock_paint(struct kit_s *k, struct kit_widget_s *wg, int x0,
                        int y0, int x1, int y1)
{
  struct clock_s *cl = k->priv;

  if (wg->id == C_FACE)
    {
      face_draw(cl, wg, x0, y0, x1, y1);
    }
  else if (wg->id == C_RING)
    {
      ring_draw(cl, wg, x0, y0, x1, y1);
    }
}

/****************************************************************************
 * Private Functions: the window
 ****************************************************************************/

static struct kit_widget_s *page_begin(struct kit_s *k, int page, bool row)
{
  struct kit_widget_s *wg = row ? kit_row(k, KIT_BOX_NONE) :
                                  kit_col(k, KIT_BOX_NONE);

  wg->id   = C_PAGE + page;
  wg->padx = PAD;
  wg->pady = PAD;
  wg->gap  = PAD;
  kit_grow(k, 1);
  if (page != 0)
    {
      wg->flags |= KIT_F_HIDDEN;
    }

  return wg;
}

static void build_clock(struct clock_s *cl)
{
  struct kit_s *k = &cl->kit;
  struct kit_widget_s *wg;

  page_begin(k, 0, true);
    kit_custom(k, C_FACE)->flags |= KIT_F_NOFOCUS;
    kit_grow(k, 1);
    wg = kit_col(k, KIT_BOX_NONE);
    wg->gap = 6;
    kit_size(k, SIDE_W, 0);
      kit_label(k, C_TIME, cl->time, KIT_TEXT_DISPLAY | KIT_TEXT_CENTRE);
      kit_size(k, SIDE_W, 56);
      kit_label(k, C_DATE, cl->date, KIT_TEXT_BOLD | KIT_TEXT_CENTRE);
      kit_label(k, C_ZONE, cl->zone, KIT_TEXT_SMALL | KIT_TEXT_DIM |
                KIT_TEXT_CENTRE);
      kit_spacer(k, 0);
      kit_size(k, 0, 10);
      kit_row(k, KIT_BOX_NONE)->gap = 4;
        kit_label(k, 0, "World Clock", KIT_TEXT_BOLD);
        kit_grow(k, 1);
        kit_button(k, C_ADDCITY, NULL, KIT_ICON_PLUS, KIT_BTN_FLAT);
        kit_tip(k, "Add a city");
        kit_button(k, C_DELCITY, NULL, KIT_ICON_MINUS, KIT_BTN_FLAT);
        kit_tip(k, "Remove the city chosen");
      kit_end(k);
      kit_list(k, C_WORLD, &cl->world, KIT_LIST_PLAIN);
      kit_grow(k, 1);
      kit_label(k, 0, "Standard time: no daylight saving.",
                KIT_TEXT_SMALL | KIT_TEXT_DIM);
    kit_end(k);
  kit_end(k);
}

static void build_alarms(struct clock_s *cl)
{
  struct kit_s *k = &cl->kit;
  struct kit_widget_s *wg;
  int i;

  page_begin(k, 1, false);
    kit_row(k, KIT_BOX_NONE);
      kit_label(k, 0, "Alarms", KIT_TEXT_HEADING);
      kit_grow(k, 1);
      kit_button(k, C_ADDALARM, "Add Alarm", KIT_ICON_PLUS,
                 KIT_BTN_NORMAL);
    kit_end(k);

    wg = kit_col(k, KIT_BOX_NONE);
    wg->id    = C_ALIST;
    wg->flags |= KIT_F_SCROLL;
    wg->gap   = PAD;
    kit_grow(k, 1);
      kit_label(k, C_NOALARMS, "No alarms.  Add one: it rings even with "
                "this window closed.", KIT_TEXT_DIM | KIT_TEXT_CENTRE);
      wg = kit_card(k);
      wg->id = C_ACARD;
        for (i = 0; i < CLK_ALARMS_MAX; i++)
          {
            wg = kit_row(k, KIT_BOX_NONE);
            wg->id   = C_AROW + i;
            wg->padx = 16;
            wg->gap  = 12;
            wg->ph   = ALARM_ROW_H;
              wg = kit_col(k, KIT_BOX_NONE);
              wg->gap   = 0;
              wg->align = KIT_A_CENTRE;
              kit_grow(k, 1);
                kit_label(k, C_ATIME + i, cl->atime[i], KIT_TEXT_HEADING);
                kit_label(k, C_ADESC + i, cl->adesc[i],
                          KIT_TEXT_SMALL | KIT_TEXT_DIM);
              kit_end(k);
              kit_button(k, C_AEDIT + i, "Edit", KIT_ICON_NONE,
                         KIT_BTN_FLAT);
              kit_switch(k, C_AON + i, false);
            kit_end(k);
          }
      kit_end(k);
    kit_end(k);

    /* The editor, in the list's place */

    wg = kit_col(k, KIT_BOX_NONE);
    wg->id    = C_EDITOR;
    wg->flags |= KIT_F_HIDDEN;
    wg->gap   = PAD;
    kit_grow(k, 1);
      kit_label(k, C_EHEAD, "New Alarm", KIT_TEXT_BOLD);
      kit_card(k);
        kit_card_row(k, 0, "Time");
          kit_stepper(k, C_EHOUR, cl->ehour);
          kit_stepper(k, C_EMIN, cl->emin);
        kit_end(k);
        kit_card_row(k, 0, "Repeat\nNone: once");
          wg = kit_row(k, KIT_BOX_NONE);
          wg->gap = 4;
            for (i = 0; i < 7; i++)
              {
                kit_button(k, C_EDAY + i, g_dayletter[i], KIT_ICON_NONE,
                           KIT_BTN_TOGGLE);
                kit_size(k, 32, 0);
              }
          kit_end(k);
        kit_end(k);
        kit_card_row(k, 0, "Label");
          kit_field(k, C_ELABEL, &cl->label, "Alarm");
          kit_size(k, 240, 0);
        kit_end(k);
      kit_end(k);
      kit_row(k, KIT_BOX_NONE);
        kit_button(k, C_EDELETE, "Delete", KIT_ICON_NONE, KIT_BTN_DANGER);
        kit_spacer(k, 1);
        kit_button(k, C_ECANCEL, "Cancel", KIT_ICON_NONE, KIT_BTN_NORMAL);
        kit_button(k, C_ESAVE, "Save", KIT_ICON_NONE, KIT_BTN_DEFAULT);
      kit_end(k);
    kit_end(k);
  kit_end(k);
}

static void build_stopwatch(struct clock_s *cl)
{
  struct kit_s *k = &cl->kit;
  struct kit_widget_s *wg;

  page_begin(k, 2, false);
    kit_label(k, C_SWTIME, cl->swtime,
              KIT_TEXT_DISPLAY | KIT_TEXT_CENTRE)->align = KIT_A_CENTRE;
    kit_size(k, 300, 80);                 /* (Not its text's: a new text
                                           * lays nothing out again) */
    wg = kit_row(k, KIT_BOX_NONE);
    wg->gap = 12;
      kit_spacer(k, 1);
      kit_button(k, C_SWLAP, "Lap", KIT_ICON_NONE, KIT_BTN_NORMAL);
      kit_size(k, 120, 0);
      kit_button(k, C_SWSTART, "Start", KIT_ICON_NONE, KIT_BTN_DEFAULT);
      kit_size(k, 120, 0);
      kit_spacer(k, 1);
    kit_end(k);
    kit_list(k, C_LAPS, &cl->laps, KIT_LIST_TABLE);
    kit_grow(k, 1);
  kit_end(k);
}

static void build_timer(struct clock_s *cl)
{
  struct kit_s *k = &cl->kit;
  struct kit_widget_s *wg;
  int i;

  page_begin(k, 3, true);
    kit_custom(k, C_RING)->flags |= KIT_F_NOFOCUS;
    kit_grow(k, 1);
    wg = kit_col(k, KIT_BOX_NONE);
    wg->gap = PAD;
    kit_size(k, 290, 0);
      kit_spacer(k, 1);
      kit_card(k);
        kit_card_row(k, 0, "Hours");
          kit_stepper(k, C_TH, cl->th);
        kit_end(k);
        kit_card_row(k, 0, "Minutes");
          kit_stepper(k, C_TM, cl->tm);
        kit_end(k);
        kit_card_row(k, 0, "Seconds");
          kit_stepper(k, C_TS, cl->ts);
        kit_end(k);
      kit_end(k);
      wg = kit_row(k, KIT_BOX_NONE);
      wg->gap = 6;
        for (i = 0; i < 4; i++)
          {
            kit_button(k, C_PRESET + i, g_presetname[i], KIT_ICON_NONE,
                       KIT_BTN_NORMAL);
            kit_grow(k, 1);
          }
      kit_end(k);
      wg = kit_row(k, KIT_BOX_NONE);
      wg->gap = 12;
        kit_button(k, C_TRESET, "Reset", KIT_ICON_NONE, KIT_BTN_NORMAL);
        kit_grow(k, 1);
        kit_button(k, C_TSTART, "Start", KIT_ICON_NONE, KIT_BTN_DEFAULT);
        kit_grow(k, 1);
      kit_end(k);
      kit_spacer(k, 1);
    kit_end(k);
  kit_end(k);
}

static void clock_build(struct clock_s *cl, struct desk_win_s *w)
{
  struct kit_s *k = &cl->kit;
  struct kit_widget_s *wg;

  kit_init(k, w, cl->wg, NWG, clock_event);
  k->priv = cl;
  k->draw = clock_paint;

  cl->world.sel     = -1;
  cl->world.sortcol = -1;
  cl->world.ncols   = 3;
  cl->world.cols    = g_worldcols;
  cl->world.text    = world_text;
  cl->world.empty   = "No cities: add one with +";
  cl->laps.sel      = -1;
  cl->laps.sortcol  = -1;
  cl->laps.ncols    = 3;
  cl->laps.cols     = g_lapcols;
  cl->laps.text     = lap_text;
  cl->laps.empty    = "No laps";

  kit_col(k, KIT_BOX_WINDOW);
    kit_row(k, KIT_BOX_BAR);
      kit_spacer(k, 1);
      kit_tabs(k, C_TABS, g_pages, NPAGES, 0, KIT_TABS_SEGMENTS);
      kit_spacer(k, 1);
    kit_end(k);

    /* The clock has not been set since boot */

    wg = kit_col(k, KIT_BOX_NONE);
    wg->id    = C_NOTSET;
    wg->padx  = PAD;
    wg->flags |= KIT_F_HIDDEN;
    wg->pady  = 10;
      wg = kit_row(k, KIT_BOX_CARD);
      wg->padx = 14;
      wg->pady = 8;
      wg->gap  = 12;
        kit_label(k, 0, "The time has not been set\nThere is no "
                  "battery-backed clock: it starts again at each boot.",
                  KIT_TEXT_BOLD);
        kit_grow(k, 1);
        kit_button(k, C_SETTIME, "Set Time" DESK_CH_ELLIPSIS,
                   KIT_ICON_NONE, KIT_BTN_DEFAULT);
      kit_end(k);
    kit_end(k);

    build_clock(cl);
    build_alarms(cl);
    build_stopwatch(cl);
    build_timer(cl);
  kit_end(k);
}

static void clock_draw(struct desk_win_s *w)
{
  struct clock_s *cl = w->priv;

  if (cl != NULL)
    {
      clock_sync(cl);
      kit_redraw(&cl->kit);
    }
}

static void clock_expose(struct desk_win_s *w, int x, int y, int cw, int ch)
{
  struct clock_s *cl = w->priv;

  if (cl != NULL)
    {
      clock_sync(cl);
      kit_expose(&cl->kit, x, y, cw, ch);
    }
}

static void clock_resize(struct desk_win_s *w)
{
  struct clock_s *cl = w->priv;

  if (cl != NULL)
    {
      cl->hand[0].len = 0;
      kit_resize(&cl->kit);
    }
}

static void clock_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct clock_s *cl = w->priv;

  if (cl != NULL)
    {
      kit_mouse(&cl->kit, x, y, buttons);
    }
}

static void clock_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct clock_s *cl = w->priv;

  if (cl != NULL)
    {
      kit_key(&cl->kit, ch, n);
    }
}

/* Once a second: the time, the lists, the hands of a window that is not
 * animating
 */

static void clock_tick(struct desk_win_s *w)
{
  struct clock_s *cl = w->priv;

  if (cl == NULL)
    {
      return;
    }

  clock_sync(cl);
  if (!w->frames)
    {
      face_update(cl);
      ring_update(cl);
    }

  kit_tick(&cl->kit);
  kit_flush(&cl->kit);
  frames_update(cl);
}

static void clock_frame(struct desk_win_s *w)
{
  struct clock_s *cl = w->priv;

  if (cl == NULL)
    {
      return;
    }

  if (cl->page == 0)
    {
      face_update(cl);
    }
  else if (cl->page == 2)
    {
      sw_sync(cl);
    }
  else if (cl->page == 3)
    {
      if (clk_timer_check())                  /* On time, not at a tick */
        {
          timer_sync(cl);
          kit_invalidate(&cl->kit, C_RING);
        }

      ring_update(cl);
      if (g_clk.timer_state != CLK_RUNNING)
        {
          timer_sync(cl);
        }
    }

  kit_flush(&cl->kit);
  frames_update(cl);
}

static void clock_focus(struct desk_win_s *w)
{
  struct clock_s *cl = w->priv;

  if (cl != NULL)
    {
      kit_activate(&cl->kit);
      frames_update(cl);
    }
}

static void clock_wake(struct desk_win_s *w)
{
  struct clock_s *cl = w->priv;

  if (cl != NULL && cl->hasface && cl->face.lost)
    {
      cl->facekey = 0;
      kit_invalidate(&cl->kit, C_FACE);
      kit_flush(&cl->kit);
    }
}

static void clock_close(struct desk_win_s *w)
{
  struct clock_s *cl = w->priv;

  if (cl != NULL)
    {
      kit_dialog_close(&cl->kit);
      if (cl->hasface)
        {
          kit_surface_close(&cl->face);
        }
    }

  w->frames = false;
  g_cl = NULL;
}

static struct desk_win_s *clock_open(void)
{
  struct clock_s *cl;
  struct desk_win_s *w;

  cl = calloc(1, sizeof(*cl));
  if (cl == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_CLOCK, "Clock", CLOCK_W, CLOCK_H);
  if (w == NULL)
    {
      free(cl);
      return NULL;
    }

  w->priv = cl;
  g_cl    = cl;
  cl->edit = -1;
  clock_build(cl, w);
  clock_sync(cl);
  show_page(cl, 0);
  kit_layout(&cl->kit);
  kit_flush(&cl->kit);
  return w;
}

/* The icon: a white face on a dark tile, hands at three o'clock and a
 * second hand (about 200 operations at 64 pixels: one list).  Small
 * (Settings' list): a disc from the glyph atlas.
 */

static void icon_clock(struct desk_ops_s *o, int x, int y, int s,
                       uint8_t a)
{
  float cx = x + s / 2.0f;
  float cy = y + s / 2.0f;
  float r = s * 0.37f;
  uint16_t ink = RGB(0x1c, 0x1c, 0x1e);
  int i;

  icon_tile(o, x, y, s, RGB(0x4a, 0x4a, 0x50), RGB(0x16, 0x16, 0x18), a);
  if (s <= 32)
    {
      int d = s * 3 / 4 >= 22 ? 22 : s * 3 / 4 >= 20 ? 20 : 18;

      ops_disc_atlas(o, x + (s - d) / 2, y + (s - d) / 2, d, C_WHITE, a);
      ops_fill(o, x + s / 2 - 1, y + s / 2 - d / 2 + 4, 2, d / 2 - 3, ink);
      ops_fill(o, x + s / 2 - 1, y + s / 2 - 1, d / 2 - 5, 2, ink);
      return;
    }

  clk_levels(6);                              /* In one list of ops */
  clk_disc(clk_room_ops, o, cx, cy, r, C_WHITE, a, x, y, x + s, y + s);
  for (i = 0; i < 12; i++)
    {
      float px = cx + (r - 4) * clk_sin(i / 12.0f);
      float py = cy - (r - 4) * clk_cos(i / 12.0f);
      int d = i % 3 == 0 ? 3 : 2;

      ops_blend(o, (int)(px - d / 2.0f + 0.5f), (int)(py - d / 2.0f + 0.5f),
                d, d, ink, (uint8_t)(a * (i % 3 == 0 ? 230 : 150) / 255));
    }

  /* Three o'clock: the hands along the pixels (FILLs, crisp), the second
   * hand anti-aliased
   */

  {
    int w = s >= 48 ? 3 : 2;
    int icx = x + s / 2;
    int icy = y + s / 2;

    ops_fill(o, icx - w / 2, icy - (int)(r * 0.78f), w, (int)(r * 0.78f),
             ink);
    ops_fill(o, icx, icy - w / 2, (int)(r * 0.55f), w, ink);
  }

  clk_seg(clk_room_ops, o, cx - r * 0.15f * clk_sin(0.62f),
          cy + r * 0.15f * clk_cos(0.62f), cx + r * 0.85f * clk_sin(0.62f),
          cy - r * 0.85f * clk_cos(0.62f), 0.6f, ORANGE, a, x, y, x + s,
          y + s);
  clk_levels(16);
  ops_disc_atlas(o, (int)cx - 3, (int)cy - 3, 6, ORANGE, a);
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_clock =
{
  .name   = "Clock",
  .icon   = icon_clock,
  .open   = clock_open,
  .draw   = clock_draw,
  .expose = clock_expose,
  .resize = clock_resize,
  .mouse  = clock_mouse,
  .key    = clock_key,
  .tick   = clock_tick,
  .frame  = clock_frame,
  .focus  = clock_focus,
  .wake   = clock_wake,
  .close  = clock_close,
  .single = true,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* The service changed something (an alarm rang, the timer ended) */

void desk_clock_changed(void)
{
  if (g_cl != NULL)
    {
      clock_sync(g_cl);
      kit_invalidate(&g_cl->kit, C_RING);
      kit_flush(&g_cl->kit);
      frames_update(g_cl);
    }
}
