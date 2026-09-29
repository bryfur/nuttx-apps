/****************************************************************************
 * apps/graphics/fpcdesk/desk_calendar.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Calendar: a month (weeks from Monday, ISO week numbers, today in the
 * accent, the chosen day marked, dots for days with events), the chosen
 * day's events beside it, and an editor for an event in their place:
 * title, date, time or all day, every year or once.  Built with the widget
 * kit; the month is a custom widget drawn by month_paint(), which the top
 * bar's popover shares.
 *
 * Events are kept in .calendar in the home directory, a line each:
 *
 *   2026-01-02 09:30 0 Title        (a time; 0: once, 1: every year)
 *   2026-01-02 - 1 Title            (all day)
 *
 * An event with a time rings a reminder then (desk_alarm.c's
 * notification), with the Calendar closed too.
 *
 * The popover: clicking the top bar's clock shows the month and the
 * chosen day's events (today's at first) below it, in a layer window over
 * the popup glass, drawn whole when anything in it changes.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <nuttx/nx/nxbe.h>

#include "desk_clock.h"
#include "desk_kit.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CAL_W          780
#define CAL_H          500
#define NWG            64
#define SIDE_W         320
#define PAD            16
#define EV_MAX         128             /* Events kept */
#define DAY_MAX        32              /* ... listed for a day */
#define TITLE_MAX      40
#define WEEKS_W        30              /* The week numbers' column */

/* The popover */

#define POP_W          300
#define POP_PAD        14
#define POP_GRID_Y     40
#define POP_HEAD_H     20
#define POP_CELL_H     30
#define POP_LINE_H     20
#define POP_LINES      4

/* Its parts under the pointer */

#define HOT_NONE       (-1)
#define HOT_PREV       100
#define HOT_NEXT       101
#define HOT_OPEN       102
#define HOT_SETTINGS   103

/* The widgets' ids */

enum
{
  K_NONE = 0,
  K_PREV,
  K_NEXT,
  K_TODAY,
  K_TITLE,
  K_ADD,
  K_NOTSET,
  K_SETTIME,
  K_MONTH,
  K_SIDE,
  K_DAYLBL,
  K_EVLIST,
  K_NEW,
  K_EDIT,
  K_DEL,
  K_EDITOR,
  K_EHEAD,
  K_ETITLE,
  K_EDAY,
  K_EMON,
  K_EYEAR,
  K_EALLDAY,
  K_ETIMEROW,
  K_EHOUR,
  K_EMIN,
  K_EYEARLY,
  K_EDELETE,
  K_ECANCEL,
  K_ESAVE
};

#define D_DELETE       1

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct cal_ev_s
{
  int16_t  year;
  uint8_t  mon;                        /* 1..12 */
  uint8_t  day;
  int16_t  min;                        /* Minutes into the day, -1: all
                                        * day */
  bool     yearly;
  char     title[TITLE_MAX];
};

/* A month to draw: its year and month, the day chosen and today (days
 * since 1970), the week numbers or not; hot: a cell under the pointer
 */

struct month_s
{
  int      year;
  int      mon;
  int32_t  sel;
  int32_t  today;
  bool     weeks;
  bool     small;                      /* The popover's */
  int      hot;
};

struct calendar_s
{
  struct kit_s kit;
  struct kit_widget_s wg[NWG];
  struct kit_list_s list;
  struct ui_field_s title;
  int      vyear;                      /* The month shown */
  int      vmon;
  int32_t  sel;                        /* The day chosen */
  int      n;                          /* Its events */
  int      idx[DAY_MAX];
  uint32_t clickms;                    /* Double clicks on a day */
  int32_t  clickday;
  uint8_t  mb;                         /* The month's last buttons */
  bool     editing;                    /* The editor is showing */
  bool     evdirty;                    /* The list to draw again */

  /* The event being edited: -1 a new one */

  int      edit;
  struct cal_ev_s e;

  char     head[32];
  char     daylbl[48];
  char     eday[8];
  char     emon[8];
  char     eyear[8];
  char     ehour[8];
  char     emin[8];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

extern const struct nx_callback_s g_desk_partcb;

static struct cal_ev_s *g_ev;          /* Allocated when first loaded */
static int g_nev;
static bool g_evloaded;
static struct calendar_s *g_cal;       /* While the window is open */

static const char * const g_mname[12] =
{
  "January", "February", "March", "April", "May", "June", "July",
  "August", "September", "October", "November", "December"
};

static const char * const g_mshort[12] =
{
  "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
  "Nov", "Dec"
};

static const char * const g_wdshort[7] =  /* Monday first */
{
  "Mon", "Tue", "Wed", "Thu", "Fri", "Sat", "Sun"
};

static const char * const g_wdname[7] =
{
  "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday",
  "Sunday"
};

static const struct kit_col_s g_evcols[2] =
{
  { "Time",  64, 0, KIT_COL_DIM },
  { "Event", 0,  0, 0 },
};

static struct
{
  NXWINDOW pop;                        /* NULL when closed */
  int      w;
  int      h;
  bool     hover;                      /* The pointer is on the clock */
  int      ix;                         /* The clock in the top bar */
  int      iw;
  int      vyear;
  int      vmon;
  int32_t  sel;
  int      hot;
  uint8_t  last;
  struct desk_list_s list;
  struct desk_ops_s ops;
} g_cp;

/****************************************************************************
 * Private Functions: dates
 ****************************************************************************/

/* Days since 1970-01-01 of a date, and back (proleptic Gregorian) */

static int32_t days_of(int y, int m, int d)
{
  int era;
  unsigned yoe;
  unsigned doy;

  y  -= m <= 2;
  era = (y >= 0 ? y : y - 399) / 400;
  yoe = (unsigned)(y - era * 400);
  doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return era * 146097 + (int32_t)(yoe * 365 + yoe / 4 - yoe / 100 + doy) -
         719468;
}

static void date_of(int32_t z, int *y, int *m, int *d)
{
  int era;
  unsigned doe;
  unsigned yoe;
  unsigned doy;
  unsigned mp;

  z  += 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = (unsigned)(z - era * 146097);
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp  = (5 * doy + 2) / 153;
  *d  = (int)(doy - (153 * mp + 2) / 5 + 1);
  *m  = (int)(mp < 10 ? mp + 3 : mp - 9);
  *y  = (int)yoe + era * 400 + (*m <= 2);
}

static int wday_of(int32_t z)                  /* 0 Monday .. 6 Sunday */
{
  return (int)(((z % 7) + 7 + 3) % 7);
}

static int mdays(int y, int m)
{
  return days_of(m == 12 ? y + 1 : y, m == 12 ? 1 : m + 1, 1) -
         days_of(y, m, 1);
}

/* ISO 8601: the week (1..53) of the day, its Thursday's year's */

static int iso_week(int32_t z)
{
  int32_t thu = z - wday_of(z) + 3;
  int y;
  int m;
  int d;

  date_of(thu, &y, &m, &d);
  return (thu - days_of(y, 1, 1)) / 7 + 1;
}

static int32_t today(void)
{
  time_t t = clk_local(NULL);

  return (int32_t)(t / 86400 - (t < 0 && t % 86400 != 0));
}

/* The first day of a month's grid: the Monday on or before its first */

static int32_t grid_first(int y, int m)
{
  int32_t z = days_of(y, m, 1);

  return z - wday_of(z);
}

/****************************************************************************
 * Private Functions: the events
 ****************************************************************************/

static void cal_load(void)
{
  char path[64];
  char line[96];
  FILE *f;

  g_evloaded = true;
  if (g_ev == NULL)
    {
      g_ev = calloc(EV_MAX, sizeof(*g_ev));
      if (g_ev == NULL)
        {
          return;
        }
    }

  g_nev = 0;
  f = fopen(clk_home_file(path, sizeof(path), ".calendar"), "r");
  if (f == NULL)
    {
      return;
    }

  while (fgets(line, sizeof(line), f) != NULL && g_nev < EV_MAX)
    {
      struct cal_ev_s *e = &g_ev[g_nev];
      char *nl = strchr(line, '\n');
      char tm[8];
      int y;
      int m;
      int d;
      int yearly;
      int n = 0;

      if (nl != NULL)
        {
          *nl = '\0';
        }

      if (sscanf(line, "%d-%d-%d %7s %d %n", &y, &m, &d, tm, &yearly,
                 &n) < 5 || n == 0 || m < 1 || m > 12 || d < 1 || d > 31)
        {
          continue;
        }

      memset(e, 0, sizeof(*e));
      e->year   = y;
      e->mon    = m;
      e->day    = d;
      e->yearly = yearly != 0;
      e->min    = -1;
      if (tm[0] != '-')
        {
          int h = 0;
          int mi = 0;

          sscanf(tm, "%d:%d", &h, &mi);
          e->min = (h % 24) * 60 + mi % 60;
        }

      strlcpy(e->title, line + n, sizeof(e->title));
      g_nev++;
    }

  fclose(f);
}

static void cal_save(void)
{
  char path[64];
  FILE *f;
  int i;

  f = fopen(clk_home_file(path, sizeof(path), ".calendar"), "w");
  if (f == NULL)
    {
      return;
    }

  for (i = 0; i < g_nev; i++)
    {
      const struct cal_ev_s *e = &g_ev[i];

      if (e->min >= 0)
        {
          fprintf(f, "%04d-%02d-%02d %02d:%02d %d %s\n", e->year, e->mon,
                  e->day, e->min / 60, e->min % 60, e->yearly, e->title);
        }
      else
        {
          fprintf(f, "%04d-%02d-%02d - %d %s\n", e->year, e->mon, e->day,
                  e->yearly, e->title);
        }
    }

  fclose(f);
}

static bool ev_on(const struct cal_ev_s *e, int y, int m, int d)
{
  return e->day == d && e->mon == m &&
         (e->year == y || (e->yearly && e->year < y));
}

void cal_init(void)
{
  if (!g_evloaded)
    {
      cal_load();
    }
}

int cal_events_on(int year, int mon, int day, int *idx, int max)
{
  int n = 0;
  int i;
  int j;

  cal_init();
  for (i = 0; i < g_nev; i++)
    {
      if (!ev_on(&g_ev[i], year, mon, day))
        {
          continue;
        }

      /* By time, all day first (an insertion) */

      for (j = n < max ? n : max - 1; j > 0 && idx != NULL &&
           g_ev[idx[j - 1]].min > g_ev[i].min; j--)
        {
          if (j < max)
            {
              idx[j] = idx[j - 1];
            }
        }

      if (idx != NULL && j < max)
        {
          idx[j] = i;
        }

      n++;
    }

  return n;
}

void cal_event_text(int i, char *buf, size_t len)
{
  const struct cal_ev_s *e = &g_ev[i];

  if (e->min >= 0)
    {
      char hm[16];

      clk_fmt_hm(hm, sizeof(hm), e->min / 60, e->min % 60);
      snprintf(buf, len, "%s  %s", hm, e->title);
    }
  else
    {
      snprintf(buf, len, "%s", e->title);
    }
}

/* Reminders: events whose time is in (from, to], local seconds */

void cal_reminders(time_t from, time_t to)
{
  int32_t day = (int32_t)(to / 86400);
  int k;
  int i;

  if (!g_evloaded)
    {
      return;
    }

  for (k = 0; k < 2; k++, day--)
    {
      int y;
      int m;
      int d;

      date_of(day, &y, &m, &d);
      for (i = 0; i < g_nev; i++)
        {
          const struct cal_ev_s *e = &g_ev[i];
          time_t at = (time_t)day * 86400 + e->min * 60;
          char hm[16];

          if (e->min < 0 || !ev_on(e, y, m, d) || at <= from || at > to)
            {
              continue;
            }

          clk_fmt_hm(hm, sizeof(hm), e->min / 60, e->min % 60);
          desk_notify_ring(CLK_RING_EVENT, i, e->title[0] ? e->title :
                           "Event", hm);
        }
    }
}

/****************************************************************************
 * Private Functions: a month
 ****************************************************************************/

struct mgeo_s
{
  int      gx;                         /* The grid of days */
  int      gy;
  int      cw;                         /* A cell */
  int      ch;
  int      hh;                         /* The weekdays' row */
};

static void month_geo(const struct month_s *m, int x, int y, int w, int h,
                      struct mgeo_s *g)
{
  int ww = m->weeks ? WEEKS_W : 0;

  g->hh = m->small ? POP_HEAD_H : 26;
  g->gx = x + ww;
  g->gy = y + g->hh;
  g->cw = (w - ww) / 7;
  g->ch = (h - g->hh) / 6;
}

static int month_hit(const struct month_s *m, int x, int y, int w, int h,
                     int px, int py)
{
  struct mgeo_s g;
  int i;
  int j;

  month_geo(m, x, y, w, h, &g);
  if (g.cw <= 0 || g.ch <= 0 || px < g.gx || py < g.gy)
    {
      return -1;
    }

  i = (px - g.gx) / g.cw;
  j = (py - g.gy) / g.ch;
  return i < 7 && j < 6 ? j * 7 + i : -1;
}

/* The month in [x, y, w, h): fg for text (dim and faint by alpha), the
 * accent for today; bg what it is drawn on (the app fills it, the popover
 * is glass)
 */

static void month_paint(clk_room_t room, void *arg, const struct month_s *m,
                        int x, int y, int w, int h, uint16_t fg)
{
  const struct desk_font_s *f = m->small ? &g_desk_font_ui :
                                           &g_desk_font_uib;
  const struct desk_font_s *sf = &g_desk_font_small;
  struct desk_ops_s *o;
  struct mgeo_s g;
  int32_t z = grid_first(m->year, m->mon);
  int i;

  month_geo(m, x, y, w, h, &g);

  for (i = 0; i < 7; i++)
    {
      char wd[4];

      snprintf(wd, sizeof(wd), "%.*s", m->small ? 2 : 3, g_wdshort[i]);
      o = room(arg, 4);
      ops_text_centre(o, sf, g.gx + i * g.cw + g.cw / 2,
                      y + (g.hh - text_height(sf)) / 2, wd, fg,
                      i >= 5 ? 110 : 150);
    }

  for (i = 0; i < 42; i++, z++)
    {
      int yy;
      int mm;
      int dd;
      int cx = g.gx + (i % 7) * g.cw + g.cw / 2;
      int cy = g.gy + (i / 7) * g.ch;
      int ty = m->small ? cy + (g.ch - text_height(f)) / 2 : cy + 8;
      bool other;
      char num[12];
      int nev;

      date_of(z, &yy, &mm, &dd);
      other = mm != m->mon;
      snprintf(num, sizeof(num), "%d", dd);

      if (m->weeks && i % 7 == 0)
        {
          char wk[12];

          snprintf(wk, sizeof(wk), "%d", iso_week(z));
          o = room(arg, 3);
          ops_text_centre(o, sf, x + WEEKS_W / 2, ty + 2, wk, fg, 90);
        }

      /* Lines between the weeks (the application's) */

      if (!m->small && i % 7 == 0)
        {
          o = room(arg, 1);
          ops_blend(o, g.gx, cy, 7 * g.cw, 1, fg, THEMED(26, 30));
        }

      /* The day chosen: a soft round rectangle; today: a disc in the
       * accent
       */

      if (z == m->sel && z != m->today)
        {
          o = room(arg, 1);
          if (m->small)
            {
              ops_rrect(o, cx - 13, cy + (g.ch - 26) / 2, 26, 26, LUT_R12,
                        12, fg, THEMED(46, 30));
            }
          else
            {
              ops_rrect(o, cx - g.cw / 2 + 3, cy + 3, g.cw - 6, g.ch - 6,
                        LUT_R6, 6, g_theme.accent, THEMED(60, 40));
            }
        }
      else if (i == m->hot)
        {
          o = room(arg, 1);
          ops_rrect(o, cx - 13, cy + (g.ch - 26) / 2, 26, 26, LUT_R12, 12,
                    fg, THEMED(24, 16));
        }

      if (z == m->today)
        {
          o = room(arg, 1);
          ops_disc(o, cx, ty + text_height(f) / 2, LUT_R12, 12,
                   g_theme.accent, 255);
        }

      o = room(arg, 3);
      ops_text_centre(o, f, cx, ty, num, z == m->today ?
                      g_theme.accent_text : fg,
                      z == m->today ? 255 : other ? 80 :
                      (i % 7) >= 5 ? 170 : 255);

      /* Dots for its events (up to three) */

      nev = cal_events_on(yy, mm, dd, NULL, 0);
      if (nev > 0)
        {
          int nd = nev > 3 ? 3 : nev;
          int k;
          int dy = m->small ? cy + g.ch - 5 : ty + text_height(f) + 8;

          o = room(arg, nd);
          for (k = 0; k < nd; k++)
            {
              ops_disc_atlas(o, cx - nd * 4 + k * 8 + 1, dy - 3, 6,
                             other ? fg : m->small && z == m->today ?
                             g_theme.accent_text : g_theme.accent,
                             other ? 90 : 255);
            }
        }
    }
}

/****************************************************************************
 * Private Functions: the application
 ****************************************************************************/

static struct desk_ops_s *room_kit(void *arg, int n)
{
  return kit_ops(arg, n);
}

static void set_text(struct calendar_s *c, int id, char *buf, size_t size,
                     const char *fmt, ...)
{
  char tmp[64];
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (strcmp(tmp, buf) != 0)
    {
      strlcpy(buf, tmp, size);
      kit_set_text(&c->kit, id, buf);
    }
}

static const char *ev_text(struct kit_s *k, struct kit_widget_s *wg,
                           int row, int col, char *buf, int size)
{
  struct calendar_s *c = k->priv;
  const struct cal_ev_s *e = &g_ev[c->idx[row]];

  if (col == 1)
    {
      return e->title[0] ? e->title : "(No title)";
    }

  if (e->min < 0)
    {
      return "All day";
    }

  clk_fmt_hm(buf, size, e->min / 60, e->min % 60);
  return buf;
}

static void cal_month_view(struct calendar_s *c, struct month_s *m)
{
  memset(m, 0, sizeof(*m));
  m->year  = c->vyear;
  m->mon   = c->vmon;
  m->sel   = c->sel;
  m->today = today();
  m->weeks = true;
  m->hot   = -1;
}

static void cal_paint(struct kit_s *k, struct kit_widget_s *wg, int x0,
                      int y0, int x1, int y1)
{
  struct calendar_s *c = k->priv;
  struct desk_ops_s *o;
  struct month_s m;

  if (wg->id != K_MONTH)
    {
      return;
    }

  cal_month_view(c, &m);
  o = kit_ops(k, 1);
  ops_fill(o, x0, y0, x1 - x0, y1 - y0, g_theme.window_bg);
  month_paint(room_kit, k, &m, wg->x + 8, wg->y + 4, wg->w - 16,
              wg->h - 8, g_theme.text);
}

static void cal_sync(struct calendar_s *c)
{
  struct kit_s *k = &c->kit;
  int y;
  int m;
  int d;
  int n;

  kit_show(k, K_NOTSET, !clk_time_set());
  set_text(c, K_TITLE, c->head, sizeof(c->head), "%s %d",
           g_mname[c->vmon - 1], c->vyear);

  date_of(c->sel, &y, &m, &d);
  set_text(c, K_DAYLBL, c->daylbl, sizeof(c->daylbl), "%s, %d %s%s",
           g_wdname[wday_of(c->sel)], d, g_mname[m - 1],
           c->sel == today() ? " (today)" : "");
  n = cal_events_on(y, m, d, c->idx, DAY_MAX);
  c->n = n > DAY_MAX ? DAY_MAX : n;
  if (c->list.n != c->n || c->evdirty)
    {
      c->evdirty = false;
      c->list.n  = c->n;
      if (c->list.sel >= c->n)
        {
          c->list.sel = c->n - 1;
        }

      kit_list_changed(k, K_EVLIST);
    }

  kit_enable(k, K_EDIT, c->list.sel >= 0);
  kit_enable(k, K_DEL, c->list.sel >= 0);
}

static void editor_sync(struct calendar_s *c)
{
  struct kit_s *k = &c->kit;

  set_text(c, K_EDAY, c->eday, sizeof(c->eday), "%d", c->e.day);
  set_text(c, K_EMON, c->emon, sizeof(c->emon), "%s",
           g_mshort[c->e.mon - 1]);
  set_text(c, K_EYEAR, c->eyear, sizeof(c->eyear), "%d", c->e.year);
  if (g_prefs.clock24)
    {
      set_text(c, K_EHOUR, c->ehour, sizeof(c->ehour), "%02d",
               c->e.min < 0 ? 9 : c->e.min / 60);
    }
  else
    {
      int h = c->e.min < 0 ? 9 : c->e.min / 60;

      set_text(c, K_EHOUR, c->ehour, sizeof(c->ehour), "%d %s",
               (h + 11) % 12 + 1, h < 12 ? "AM" : "PM");
    }

  set_text(c, K_EMIN, c->emin, sizeof(c->emin), "%02d",
           c->e.min < 0 ? 0 : c->e.min % 60);
  kit_set_value(k, K_EALLDAY, c->e.min < 0);
  kit_show(k, K_ETIMEROW, c->e.min >= 0);
  kit_set_value(k, K_EYEARLY, c->e.yearly);
}

static void view_day(struct calendar_s *c, int32_t z)
{
  int y;
  int m;
  int d;

  date_of(z, &y, &m, &d);
  c->sel      = z;
  c->list.sel = -1;
  c->evdirty  = true;
  if (y != c->vyear || m != c->vmon)
    {
      c->vyear = y;
      c->vmon  = m;
    }

  kit_invalidate(&c->kit, K_MONTH);
}

static void view_month(struct calendar_s *c, int d)
{
  int y = c->vyear;
  int m = c->vmon + d;
  int yy;
  int mm;
  int dd;

  while (m < 1)
    {
      m += 12;
      y--;
    }

  while (m > 12)
    {
      m -= 12;
      y++;
    }

  c->vyear = y;
  c->vmon  = m;

  /* The chosen day follows into the month (its day, or its last) */

  date_of(c->sel, &yy, &mm, &dd);
  dd = dd > mdays(y, m) ? mdays(y, m) : dd;
  c->sel = days_of(y, m, dd);
  c->list.sel = -1;
  c->evdirty  = true;
  kit_invalidate(&c->kit, K_MONTH);
}

static void editor_open(struct calendar_s *c, int i)
{
  struct kit_s *k = &c->kit;
  int y;
  int m;
  int d;

  c->edit = i;
  if (i >= 0)
    {
      c->e = g_ev[i];
    }
  else
    {
      date_of(c->sel, &y, &m, &d);
      memset(&c->e, 0, sizeof(c->e));
      c->e.year = y;
      c->e.mon  = m;
      c->e.day  = d;
      c->e.min  = 9 * 60;
      if (c->sel == today())
        {
          time_t t = clk_local(NULL);

          c->e.min = (int)(t / 3600 % 24 + 1) % 24 * 60;  /* The next hour */
        }
    }

  ui_field_set(&c->title, c->e.title);
  kit_set_text(k, K_EHEAD, i >= 0 ? "Edit Event" : "New Event");
  kit_show(k, K_EDELETE, i >= 0);
  kit_show(k, K_SIDE, false);
  kit_show(k, K_EDITOR, true);
  c->editing = true;
  kit_invalidate(k, K_ETITLE);
  editor_sync(c);
  kit_focus(k, K_ETITLE);
}

static void editor_close(struct calendar_s *c)
{
  c->editing = false;
  kit_show(&c->kit, K_EDITOR, false);
  kit_show(&c->kit, K_SIDE, true);
}

static void editor_save(struct calendar_s *c)
{
  struct cal_ev_s *e;
  int md = mdays(c->e.year, c->e.mon);

  if (g_ev == NULL)
    {
      return;
    }

  if (c->edit >= 0 && c->edit < g_nev)
    {
      e = &g_ev[c->edit];
    }
  else if (g_nev < EV_MAX)
    {
      e = &g_ev[g_nev++];
    }
  else
    {
      return;
    }

  c->e.day = c->e.day > md ? md : c->e.day;
  strlcpy(c->e.title, c->title.text, sizeof(c->e.title));
  *e = c->e;
  cal_save();
  view_day(c, days_of(c->e.year, c->e.mon, c->e.day));
}

static void ev_delete(int i)
{
  if (i < 0 || i >= g_nev)
    {
      return;
    }

  g_nev--;
  memmove(&g_ev[i], &g_ev[i + 1], (g_nev - i) * sizeof(g_ev[0]));
  cal_save();
}

static void cal_event(struct kit_s *k, struct kit_widget_s *wg, int ev)
{
  struct calendar_s *c = k->priv;
  int id = wg != NULL ? wg->id : 0;
  int md;

  switch (ev)
    {
      case KIT_EV_CLICK:
        switch (id)
          {
            case K_PREV:
              view_month(c, -1);
              break;

            case K_NEXT:
              view_month(c, 1);
              break;

            case K_TODAY:
              view_day(c, today());
              break;

            case K_SETTIME:
              desk_settings_show(SETTINGS_DATETIME);
              return;

            case K_ADD:
            case K_NEW:
              editor_open(c, -1);
              break;

            case K_EDIT:
              if (c->list.sel >= 0 && c->list.sel < c->n)
                {
                  editor_open(c, c->idx[c->list.sel]);
                }
              break;

            case K_DEL:
            case K_EDELETE:
              {
                static const char * const buttons[] =
                {
                  "Delete", "Cancel"
                };

                if (id == K_DEL)
                  {
                    if (c->list.sel < 0 || c->list.sel >= c->n)
                      {
                        break;
                      }

                    c->edit = c->idx[c->list.sel];
                  }

                kit_dialog(k, D_DELETE, KIT_DLG_CONFIRM | KIT_DLG_DANGER,
                           "Delete this event?", g_ev[c->edit].title,
                           buttons, 2);
              }
              break;

            case K_ECANCEL:
              editor_close(c);
              break;

            case K_ESAVE:
              editor_save(c);
              editor_close(c);
              break;

            default:
              break;
          }
        break;

      case KIT_EV_CHANGE:
        md = mdays(c->e.year, c->e.mon);
        switch (id)
          {
            case K_EVLIST:
              break;

            case K_EDAY:
              c->e.day = (c->e.day - 1 + md + k->arg) % md + 1;
              break;

            case K_EMON:
              c->e.mon = (c->e.mon - 1 + 12 + k->arg) % 12 + 1;
              md = mdays(c->e.year, c->e.mon);
              c->e.day = c->e.day > md ? md : c->e.day;
              break;

            case K_EYEAR:
              c->e.year += k->arg;
              c->e.year = c->e.year < 1970 ? 1970 : c->e.year > 2099 ?
                          2099 : c->e.year;
              break;

            case K_EALLDAY:
              c->e.min = wg->value ? -1 : 9 * 60;
              break;

            case K_EHOUR:
              c->e.min = ((c->e.min / 60 + 24 + k->arg) % 24) * 60 +
                         c->e.min % 60;
              break;

            case K_EMIN:
              c->e.min = (c->e.min / 60) * 60 +
                         (c->e.min % 60 + 60 + k->arg * 5) % 60;
              break;

            case K_EYEARLY:
              c->e.yearly = wg->value != 0;
              break;

            default:
              break;
          }

        if (id >= K_EDAY && id <= K_EYEARLY)
          {
            editor_sync(c);
          }
        break;

      case KIT_EV_ACTIVATE:
        if (id == K_EVLIST && c->list.sel >= 0)
          {
            editor_open(c, c->idx[c->list.sel]);
          }
        else if (id == K_ETITLE)
          {
            editor_save(c);
            editor_close(c);
          }
        break;

      case KIT_EV_MOUSE:
        if (id == K_MONTH && (k->mbuttons & NX_MOUSE_LEFTBUTTON) &&
            !(c->mb & NX_MOUSE_LEFTBUTTON))
          {
            struct month_s m;
            int cell;

            cal_month_view(c, &m);
            cell = month_hit(&m, wg->x + 8, wg->y + 4, wg->w - 16,
                             wg->h - 8, k->mx, k->my);
            if (cell >= 0 && c->editing)
              {
                int y;
                int mo;
                int d;

                /* Editing: the day clicked is the event's */

                view_day(c, grid_first(c->vyear, c->vmon) + cell);
                date_of(c->sel, &y, &mo, &d);
                c->e.year = y;
                c->e.mon  = mo;
                c->e.day  = d;
                editor_sync(c);
                kit_focus(k, K_ETITLE);
              }
            else if (cell >= 0)
              {
                int32_t z = grid_first(c->vyear, c->vmon) + cell;
                uint32_t now = desk_ms();

                if (z == c->sel)
                  {
                    if (z == c->clickday &&
                        now - c->clickms < KIT_DBLCLICK_MS)
                      {
                        c->clickday = -1;
                        editor_open(c, -1);
                        break;
                      }
                  }
                else
                  {
                    view_day(c, z);
                  }

                c->clickday = z;
                c->clickms  = now;
              }
          }

        if (id == K_MONTH)
          {
            c->mb = k->mbuttons;
          }
        break;

      case KIT_EV_KEY:
        if (wg != NULL && id == K_MONTH)
          {
            int d = k->key == KEY_LEFT ? -1 : k->key == KEY_RIGHT ? 1 :
                    k->key == KEY_UP ? -7 : k->key == KEY_DOWN ? 7 : 0;

            if (d != 0)
              {
                view_day(c, c->sel + d);
                k->key = 0;
              }
            else if (k->key == '\r' || k->key == '\n')
              {
                editor_open(c, -1);
                k->key = 0;
              }
          }

        if (k->key == KEY_PGUP || k->key == KEY_PGDN)
          {
            view_month(c, k->key == KEY_PGUP ? -1 : 1);
            k->key = 0;
          }
        else if (k->key == KEY_ESC)
          {
            editor_close(c);
            k->key = 0;
          }
        break;

      case KIT_EV_DIALOG:
        if (k->key == D_DELETE && k->arg == KIT_ANS_OK)
          {
            ev_delete(c->edit);
            c->list.sel = -1;
            c->evdirty  = true;
            editor_close(c);
            kit_invalidate(k, K_MONTH);
          }
        break;

      default:
        break;
    }

  cal_sync(c);
}

static void cal_build(struct calendar_s *c, struct desk_win_s *w)
{
  struct kit_s *k = &c->kit;
  struct kit_widget_s *wg;

  kit_init(k, w, c->wg, NWG, cal_event);
  k->priv = c;
  k->draw = cal_paint;

  c->list.sel     = -1;
  c->list.sortcol = -1;
  c->list.ncols   = 2;
  c->list.cols    = g_evcols;
  c->list.text    = ev_text;
  c->list.empty   = "No events";

  kit_col(k, KIT_BOX_WINDOW);
    kit_row(k, KIT_BOX_BAR);
      kit_button(k, K_PREV, NULL, KIT_ICON_BACK, KIT_BTN_NORMAL);
      kit_tip(k, "The month before (Page Up)");
      kit_button(k, K_NEXT, NULL, KIT_ICON_FORWARD, KIT_BTN_NORMAL);
      kit_tip(k, "The month after (Page Down)");
      kit_button(k, K_TODAY, "Today", KIT_ICON_NONE, KIT_BTN_NORMAL);
      kit_label(k, K_TITLE, c->head, KIT_TEXT_HEADING);
      kit_grow(k, 1);
      kit_button(k, K_ADD, "New Event", KIT_ICON_PLUS, KIT_BTN_NORMAL);
    kit_end(k);

    wg = kit_col(k, KIT_BOX_NONE);
    wg->id    = K_NOTSET;
    wg->padx  = PAD;
    wg->pady  = 10;
    wg->flags |= KIT_F_HIDDEN;
      wg = kit_row(k, KIT_BOX_CARD);
      wg->padx = 14;
      wg->pady = 8;
      wg->gap  = 12;
        kit_label(k, 0, "The time has not been set\nToday is not known "
                  "until it is.", KIT_TEXT_BOLD);
        kit_grow(k, 1);
        kit_button(k, K_SETTIME, "Set Time" DESK_CH_ELLIPSIS,
                   KIT_ICON_NONE, KIT_BTN_DEFAULT);
      kit_end(k);
    kit_end(k);

    wg = kit_row(k, KIT_BOX_NONE);
    wg->gap = 0;
    kit_grow(k, 1);
      kit_custom(k, K_MONTH);
      kit_grow(k, 1);

      /* The day's events */

      wg = kit_col(k, KIT_BOX_SIDEBAR);
      wg->id   = K_SIDE;
      wg->padx = PAD;
      wg->pady = PAD;
      wg->gap  = 10;
      kit_size(k, SIDE_W, 0);
        kit_label(k, K_DAYLBL, c->daylbl, KIT_TEXT_BOLD);
        kit_list(k, K_EVLIST, &c->list, KIT_LIST_PLAIN);
        kit_grow(k, 1);
        wg = kit_row(k, KIT_BOX_NONE);
        wg->gap = 8;
          kit_button(k, K_NEW, "New", KIT_ICON_PLUS, KIT_BTN_NORMAL);
          kit_spacer(k, 1);
          kit_button(k, K_EDIT, "Edit", KIT_ICON_NONE, KIT_BTN_NORMAL);
          kit_button(k, K_DEL, "Delete", KIT_ICON_NONE, KIT_BTN_NORMAL);
        kit_end(k);
      kit_end(k);

      /* ... or the editor */

      wg = kit_col(k, KIT_BOX_SIDEBAR);
      wg->id    = K_EDITOR;
      wg->padx  = PAD;
      wg->pady  = PAD;
      wg->gap   = 8;
      wg->flags |= KIT_F_HIDDEN;
      kit_size(k, SIDE_W, 0);
        kit_label(k, K_EHEAD, "New Event", KIT_TEXT_BOLD);
        kit_label(k, 0, "Title", KIT_TEXT_SMALL | KIT_TEXT_DIM);
        kit_field(k, K_ETITLE, &c->title, "Event");
        kit_label(k, 0, "Date", KIT_TEXT_SMALL | KIT_TEXT_DIM);
        wg = kit_row(k, KIT_BOX_NONE);
        wg->gap = 6;
          kit_stepper(k, K_EDAY, c->eday);
          kit_size(k, 80, 0);
          kit_stepper(k, K_EMON, c->emon);
          kit_size(k, 90, 0);
          kit_stepper(k, K_EYEAR, c->eyear);
          kit_size(k, 98, 0);
        kit_end(k);
        kit_row(k, KIT_BOX_NONE);
          kit_label(k, 0, "All day", KIT_TEXT_BODY);
          kit_grow(k, 1);
          kit_switch(k, K_EALLDAY, false);
        kit_end(k);
        wg = kit_row(k, KIT_BOX_NONE);
        wg->id  = K_ETIMEROW;
        wg->gap = 6;
          kit_label(k, 0, "Time", KIT_TEXT_BODY);
          kit_grow(k, 1);
          kit_stepper(k, K_EHOUR, c->ehour);
          kit_size(k, 90, 0);
          kit_stepper(k, K_EMIN, c->emin);
          kit_size(k, 80, 0);
        kit_end(k);
        kit_row(k, KIT_BOX_NONE);
          kit_label(k, 0, "Every year", KIT_TEXT_BODY);
          kit_grow(k, 1);
          kit_switch(k, K_EYEARLY, false);
        kit_end(k);
        kit_spacer(k, 1);
        wg = kit_row(k, KIT_BOX_NONE);
        wg->gap = 8;
          kit_button(k, K_EDELETE, "Delete", KIT_ICON_NONE, KIT_BTN_DANGER);
          kit_spacer(k, 1);
          kit_button(k, K_ECANCEL, "Cancel", KIT_ICON_NONE, KIT_BTN_NORMAL);
          kit_button(k, K_ESAVE, "Save", KIT_ICON_NONE, KIT_BTN_DEFAULT);
        kit_end(k);
      kit_end(k);
    kit_end(k);
  kit_end(k);

  kit_shortcut(k, KEY_PGUP, K_PREV);
  kit_shortcut(k, KEY_PGDN, K_NEXT);
}

static void calw_draw(struct desk_win_s *w)
{
  struct calendar_s *c = w->priv;

  if (c != NULL)
    {
      cal_sync(c);
      kit_redraw(&c->kit);
    }
}

static void calw_expose(struct desk_win_s *w, int x, int y, int cw, int ch)
{
  struct calendar_s *c = w->priv;

  if (c != NULL)
    {
      kit_expose(&c->kit, x, y, cw, ch);
    }
}

static void calw_resize(struct desk_win_s *w)
{
  struct calendar_s *c = w->priv;

  if (c != NULL)
    {
      kit_resize(&c->kit);
    }
}

static void calw_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct calendar_s *c = w->priv;

  if (c != NULL)
    {
      kit_mouse(&c->kit, x, y, buttons);
    }
}

static void calw_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct calendar_s *c = w->priv;

  if (c != NULL)
    {
      kit_key(&c->kit, ch, n);
    }
}

/* Once a second: today may have changed (midnight, the clock set) */

static void calw_tick(struct desk_win_s *w)
{
  static int32_t last;
  struct calendar_s *c = w->priv;

  if (c == NULL)
    {
      return;
    }

  if (today() != last)
    {
      last = today();
      kit_invalidate(&c->kit, K_MONTH);
    }

  cal_sync(c);
  kit_tick(&c->kit);
  kit_flush(&c->kit);
}

static void calw_focus(struct desk_win_s *w)
{
  struct calendar_s *c = w->priv;

  if (c != NULL)
    {
      kit_activate(&c->kit);
    }
}

static void calw_close(struct desk_win_s *w)
{
  struct calendar_s *c = w->priv;

  if (c != NULL)
    {
      kit_dialog_close(&c->kit);
    }

  g_cal = NULL;
}

static struct desk_win_s *calw_open(void)
{
  struct calendar_s *c;
  struct desk_win_s *w;
  int d;

  cal_init();
  c = calloc(1, sizeof(*c));
  if (c == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_CALENDAR, "Calendar", CAL_W, CAL_H);
  if (w == NULL)
    {
      free(c);
      return NULL;
    }

  w->priv  = c;
  g_cal    = c;
  c->edit  = -1;
  c->clickday = -1;
  c->sel   = today();
  date_of(c->sel, &c->vyear, &c->vmon, &d);
  cal_build(c, w);
  cal_sync(c);
  kit_focus(&c->kit, K_MONTH);
  kit_layout(&c->kit);
  kit_flush(&c->kit);
  return w;
}

/* The icon: a white page, a red band, a day (always the 17th: the icons
 * are drawn once)
 */

static void icon_calendar(struct desk_ops_s *o, int x, int y, int s,
                          uint8_t a)
{
  const struct desk_font_s *f = s >= 56 ? &g_desk_font_big :
                                s >= 36 ? &g_desk_font_title :
                                          &g_desk_font_uib;
  uint16_t top = RGB(0xff, 0xff, 0xff);
  uint16_t bot = RGB(0xe4, 0xe4, 0xea);
  uint16_t red = RGB(0xf2, 0x3c, 0x34);
  int band = s * 3 / 10;
  int ty;

  icon_tile(o, x, y, s, top, bot, a);
  ops_rrect(o, x, y, s, band + 12, LUT_R12, 12, red, a);
  ops_blend(o, x, y + band, s, 12,
            colour_mix(top, bot, (band + 6) * 256 / s), a);
  if (s >= 36)
    {
      ops_text_centre(o, &g_desk_font_small, x + s / 2,
                      y + (band - text_height(&g_desk_font_small)) / 2,
                      "JAN", C_WHITE, a);
    }

  ty = y + band + (s - band - text_height(f)) / 2;
  ops_text_centre(o, f, x + s / 2, ty, "17", RGB(0x2a, 0x2a, 0x30), a);
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_calendar =
{
  .name   = "Calendar",
  .icon   = icon_calendar,
  .open   = calw_open,
  .draw   = calw_draw,
  .expose = calw_expose,
  .resize = calw_resize,
  .mouse  = calw_mouse,
  .key    = calw_key,
  .tick   = calw_tick,
  .focus  = calw_focus,
  .close  = calw_close,
  .single = true,
};

/****************************************************************************
 * Private Functions: the popover
 ****************************************************************************/

static struct desk_ops_s *room_list(void *arg, int n)
{
  list_room(&g_cp.list, &g_cp.ops, n);
  return &g_cp.ops;
}

static int pop_events_y(void)
{
  return POP_GRID_Y + POP_HEAD_H + 6 * POP_CELL_H + 12;
}

static int pop_height(int nev)
{
  int lines = nev < 1 ? 1 : nev > POP_LINES ? POP_LINES : nev;

  return pop_events_y() + 28 + lines * POP_LINE_H + 12 + 2 * 28 + 8 +
         (clk_time_set() ? 0 : POP_LINE_H);
}

static void pop_month(struct month_s *m)
{
  memset(m, 0, sizeof(*m));
  m->year  = g_cp.vyear;
  m->mon   = g_cp.vmon;
  m->sel   = g_cp.sel;
  m->today = today();
  m->small = true;
  m->hot   = g_cp.hot >= 0 && g_cp.hot < 42 ? g_cp.hot : -1;
}

static void pop_link(struct desk_ops_s *o, int y, const char *text,
                     bool hot, uint16_t fg)
{
  if (hot)
    {
      ops_rrect(o, POP_PAD / 2, y, g_cp.w - POP_PAD, 26, LUT_R6, 6, fg, 30);
    }

  ops_text(o, &g_desk_font_ui, POP_PAD, y + 5, text, fg, 255);
}

static void pop_draw(void)
{
  struct desk_ops_s *o = &g_cp.ops;
  uint16_t fg = g_theme.popup_text;
  struct month_s m;
  char buf[64];
  int idx[POP_LINES];
  int yy;
  int mm;
  int dd;
  int n;
  int y;
  int i;

  if (g_cp.pop == NULL)
    {
      return;
    }

  ops_reset(o);
  g_cp.list.n = 0;

  /* The month and the arrows */

  snprintf(buf, sizeof(buf), "%s %d", g_mname[g_cp.vmon - 1], g_cp.vyear);
  o = room_list(NULL, 40);
  ops_text(o, &g_desk_font_uib, POP_PAD, 13, buf, fg, 255);
  for (i = 0; i < 2; i++)
    {
      int bx = g_cp.w - POP_PAD - 56 + i * 30;

      if (g_cp.hot == HOT_PREV + i)
        {
          ops_rrect(o, bx, 8, 26, 26, LUT_R6, 6, fg, 30);
        }

      kit_paint_icon(o, i == 0 ? KIT_ICON_BACK : KIT_ICON_FORWARD, bx + 5,
                     13, fg, g_theme.popup);
    }

  pop_month(&m);
  month_paint(room_list, NULL, &m, POP_PAD - 4, POP_GRID_Y,
              g_cp.w - 2 * POP_PAD + 8, POP_HEAD_H + 6 * POP_CELL_H, fg);

  /* The day's events */

  y = pop_events_y();
  o = room_list(NULL, 60);
  ops_blend(o, POP_PAD, y - 6, g_cp.w - 2 * POP_PAD, 1, fg, 40);
  date_of(g_cp.sel, &yy, &mm, &dd);
  snprintf(buf, sizeof(buf), "%s, %d %s", g_cp.sel == today() ? "Today" :
           g_wdname[wday_of(g_cp.sel)], dd, g_mname[mm - 1]);
  ops_text(o, &g_desk_font_uib, POP_PAD, y + 2, buf, fg, 255);
  y += 28;
  if (!clk_time_set())
    {
      ops_text(o, &g_desk_font_ui, POP_PAD, y, "The time has not been set",
               g_theme.warning, 255);
      y += POP_LINE_H;
    }

  n = cal_events_on(yy, mm, dd, idx, POP_LINES);
  if (n == 0)
    {
      ops_text(o, &g_desk_font_ui, POP_PAD, y, "No events", fg, 150);
      y += POP_LINE_H;
    }

  for (i = 0; i < n && i < POP_LINES; i++, y += POP_LINE_H)
    {
      char line[64];

      o = room_list(NULL, 60);
      ops_disc_atlas(o, POP_PAD, y + 5, 6, g_theme.accent, 255);
      if (i == POP_LINES - 1 && n > POP_LINES)
        {
          snprintf(line, sizeof(line), "%d more" DESK_CH_ELLIPSIS,
                   n - POP_LINES + 1);
        }
      else
        {
          cal_event_text(idx[i], line, sizeof(line));
        }

      ui_fit(&g_desk_font_ui, line, g_cp.w - 2 * POP_PAD - 14, buf,
             sizeof(buf));
      ops_text(o, &g_desk_font_ui, POP_PAD + 14, y, buf, fg, 230);
    }

  /* The ways on */

  y = g_cp.h - 2 * 28 - 8;
  o = room_list(NULL, 60);
  ops_blend(o, POP_PAD, y - 4, g_cp.w - 2 * POP_PAD, 1, fg, 40);
  pop_link(o, y, "Open Calendar", g_cp.hot == HOT_OPEN, fg);
  pop_link(o, y + 28, "Date & Time Settings" DESK_CH_ELLIPSIS,
           g_cp.hot == HOT_SETTINGS, fg);
  list_set(g_cp.pop, &g_cp.list, &g_cp.ops);
}

static void pop_layout(void)
{
  struct nxgl_point_s pos;
  struct nxgl_size_s size;
  int yy;
  int mm;
  int dd;
  int x;

  date_of(g_cp.sel, &yy, &mm, &dd);
  g_cp.w = POP_W;
  g_cp.h = pop_height(cal_events_on(yy, mm, dd, NULL, 0));
  x = g_cp.ix + g_cp.iw / 2 - g_cp.w / 2;
  x = x + g_cp.w > DESK_W - 8 ? DESK_W - 8 - g_cp.w : x;
  x = x < 8 ? 8 : x;
  size.w = g_cp.w;
  size.h = g_cp.h;
  pos.x  = x;
  pos.y  = TOPBAR_H + 6;
  nx_setsize(g_cp.pop, &size);
  nx_setposition(g_cp.pop, &pos);
}

static void pop_open(void)
{
  struct nx_effects_s fx;
  int d;

  if (g_cp.pop != NULL)
    {
      return;
    }

  cal_init();
  g_cp.pop = nx_openwindow(g_desk.hnx,
                           NXBE_WINDOW_LAYER | NXBE_WINDOW_HIDDEN,
                           &g_desk_partcb, (void *)(uintptr_t)PART_CALPOP);
  if (g_cp.pop == NULL)
    {
      return;
    }

  memset(&fx, 0, sizeof(fx));
  fx.flags    = NX_FX_ROUND;
  fx.layer    = NX_LAYER_OVERLAY;
  fx.opacity  = 255;
  fx.backdrop = g_theme.popup_alpha;
  fx.tint     = g_theme.popup;
  fx.shadow   = 140;
  fx.shadowdy = 6;
  if (g_theme.darkedge)
    {
      fx.flags |= NX_FX_DARKEDGE;
      fx.border = 40;
    }

  nx_seteffects(g_cp.pop, &fx);
  g_cp.sel = today();
  date_of(g_cp.sel, &g_cp.vyear, &g_cp.vmon, &d);
  g_cp.hot = HOT_NONE;
  pop_layout();
  pop_draw();
  nx_setvisibility(g_cp.pop, false);
  desk_topbar_draw();
}

static int pop_hit(int x, int y)
{
  struct month_s m;
  int i;
  int ly = g_cp.h - 2 * 28 - 8;

  for (i = 0; i < 2; i++)
    {
      int bx = g_cp.w - POP_PAD - 56 + i * 30;

      if (x >= bx && x < bx + 26 && y >= 8 && y < 34)
        {
          return HOT_PREV + i;
        }
    }

  if (x >= 0 && x < g_cp.w && y >= ly && y < ly + 26)
    {
      return HOT_OPEN;
    }

  if (x >= 0 && x < g_cp.w && y >= ly + 28 && y < ly + 54)
    {
      return HOT_SETTINGS;
    }

  pop_month(&m);
  i = month_hit(&m, POP_PAD - 4, POP_GRID_Y, g_cp.w - 2 * POP_PAD + 8,
                POP_HEAD_H + 6 * POP_CELL_H, x, y);
  return i >= 0 ? i : HOT_NONE;
}

static void pop_month_step(int d)
{
  g_cp.vmon += d;
  if (g_cp.vmon < 1)
    {
      g_cp.vmon = 12;
      g_cp.vyear--;
    }
  else if (g_cp.vmon > 12)
    {
      g_cp.vmon = 1;
      g_cp.vyear++;
    }
}

/****************************************************************************
 * Public Functions: the popover
 ****************************************************************************/

void desk_calpop_close(void)
{
  if (g_cp.pop != NULL)
    {
      nx_closewindow(g_cp.pop);
      g_cp.pop = NULL;
      desk_topbar_draw();
    }
}

/* The top bar: where the clock is, a pill behind it under the pointer or
 * while the popover is open
 */

void desk_calpop_topbar(struct desk_ops_s *o, int x, int y, int w)
{
  g_cp.ix = x - 8;
  g_cp.iw = w + 16;
  if (g_cp.hover || g_cp.pop != NULL)
    {
      ops_rrect(o, g_cp.ix, 4, g_cp.iw, TOPBAR_H - 8, LUT_R6, 6,
                g_theme.panel_text, THEMED(g_cp.pop != NULL ? 70 : 45,
                                           g_cp.pop != NULL ? 40 : 24));
    }
}

bool desk_calpop_topbar_mouse(int x, int y, uint8_t buttons)
{
  static uint8_t last;
  bool over = g_cp.iw > 0 && x >= g_cp.ix && x < g_cp.ix + g_cp.iw &&
              y >= 0 && y < TOPBAR_H;

  if (over != g_cp.hover)
    {
      g_cp.hover = over;
      desk_topbar_draw();
    }

  if (over && (buttons & NX_MOUSE_LEFTBUTTON) &&
      !(last & NX_MOUSE_LEFTBUTTON))
    {
      if (g_cp.pop != NULL)
        {
          desk_calpop_close();
        }
      else
        {
          pop_open();
        }
    }

  last = buttons;
  return over;
}

void desk_calpop_mouse(int x, int y, uint8_t buttons)
{
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(g_cp.last & NX_MOUSE_LEFTBUTTON);
  int hot;

  g_cp.last = buttons;
  if (g_cp.pop == NULL)
    {
      return;
    }

  hot = pop_hit(x, y);
  if (press)
    {
      switch (hot)
        {
          case HOT_PREV:
          case HOT_NEXT:
            pop_month_step(hot == HOT_PREV ? -1 : 1);
            break;

          case HOT_OPEN:
            desk_calpop_close();
            desk_launch(APP_CALENDAR);
            return;

          case HOT_SETTINGS:
            desk_calpop_close();
            desk_settings_show(SETTINGS_DATETIME);
            return;

          case HOT_NONE:
            break;

          default:
            {
              int d;

              g_cp.sel = grid_first(g_cp.vyear, g_cp.vmon) + hot;
              date_of(g_cp.sel, &g_cp.vyear, &g_cp.vmon, &d);
              hot = HOT_NONE;
              pop_layout();
            }
            break;
        }

      g_cp.hot = hot;
      pop_draw();
      return;
    }

  if (hot != g_cp.hot)
    {
      g_cp.hot = hot;
      pop_draw();
    }
}

/* Every pointer event: the clock's hover goes when the pointer leaves the
 * top bar; a press anywhere but in the popover or on the clock closes it
 */

void desk_calpop_mouse_any(int part, uint8_t buttons)
{
  static uint8_t last;
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(last & NX_MOUSE_LEFTBUTTON);

  last = buttons;
  if (part != PART_TOPBAR && g_cp.hover)
    {
      g_cp.hover = false;
      desk_topbar_draw();
    }

  if (part != PART_CALPOP && g_cp.pop != NULL)
    {
      if (g_cp.hot != HOT_NONE)
        {
          g_cp.hot  = HOT_NONE;
          g_cp.last = 0;
          pop_draw();
        }

      if (press && !(part == PART_TOPBAR && g_cp.hover))
        {
          desk_calpop_close();
        }
    }
}
