/****************************************************************************
 * apps/graphics/fpcdesk/desk_alarm.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The desktop's clock service (desk_clock.h): the alarms, the timer and
 * the stopwatch live in the desktop, not in the Clock window, so that they
 * go on with it closed.  Once a second (desk_clock_tick(), the desktop's
 * tick) the alarms whose minute has come since the last tick ring, a
 * snoozed alarm rings again, the timer ends, the events' reminders are
 * due (desk_calendar.c).  What rings is a notification: a banner at the
 * top right of the display (a layer window over the popup glass) with its
 * buttons, Snooze and Stop; its sound repeats every two seconds until it
 * is dismissed.  Nothing else costs anything: the tick compares a few
 * numbers.
 *
 * Kept in .clock in the home directory: the alarms, the world clock's
 * cities, the timer's duration (a line each).
 *
 * Also the drawing both applications share: anti-aliased discs, rings,
 * arcs with round ends and thick lines, as runs of pixels of equal
 * coverage along each row (a FILL or a BLEND each), only in the rectangle
 * asked for: the clock's hands, its face, the timer's ring.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <nuttx/nx/nxbe.h>

#include "desk_clock.h"
#include "desk_kit.h"
#ifdef CONFIG_FPCDESK_SOUND
#  include "desk_sound.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PI_F          3.14159265f
#define NT_W          400              /* The notification */
#define NT_H          72
#define NT_MAX        4                /* Ringing at once */
#define NT_BTN_H      28
#define NT_JUMP_S     1800             /* The clock moved further: no
                                        * alarms for what it skipped (it
                                        * was set) */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct nt_item_s
{
  uint8_t  kind;                       /* CLK_RING_* */
  int8_t   what;                       /* The alarm */
  char     title[32];
  char     text[48];
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

struct clk_svc_s g_clk;

/* The world clock's cities: standard time (no daylight saving), by
 * region (the Clock's menus take them eight to fifteen at a time)
 */

const struct clk_city_s g_clk_cities[] =
{
  /* The Americas */

  { "Honolulu",       -600 },
  { "Anchorage",      -540 },
  { "Los Angeles",    -480 },
  { "Denver",         -420 },
  { "Chicago",        -360 },
  { "Mexico City",    -360 },
  { "New York",       -300 },
  { "Toronto",        -300 },
  { "Bogota",         -300 },
  { "Santiago",       -240 },
  { "Sao Paulo",      -180 },
  { "Buenos Aires",   -180 },

  /* Europe and Africa */

  { "UTC",               0 },
  { "Reykjavik",         0 },
  { "London",            0 },
  { "Lisbon",            0 },
  { "Lagos",            60 },
  { "Paris",            60 },
  { "Berlin",           60 },
  { "Madrid",           60 },
  { "Rome",             60 },
  { "Stockholm",        60 },
  { "Athens",          120 },
  { "Cairo",           120 },
  { "Johannesburg",    120 },
  { "Istanbul",        180 },
  { "Moscow",          180 },
  { "Nairobi",         180 },

  /* Asia and the Pacific */

  { "Dubai",           240 },
  { "Karachi",         300 },
  { "Mumbai",          330 },
  { "Dhaka",           360 },
  { "Bangkok",         420 },
  { "Singapore",       480 },
  { "Hong Kong",       480 },
  { "Beijing",         480 },
  { "Seoul",           540 },
  { "Tokyo",           540 },
  { "Adelaide",        570 },
  { "Sydney",          600 },
  { "Auckland",        720 },
};

const int g_clk_ncities = sizeof(g_clk_cities) / sizeof(g_clk_cities[0]);

/****************************************************************************
 * Private Data
 ****************************************************************************/

extern const struct nx_callback_s g_desk_partcb;

static int g_levels = 16;              /* Coverage levels of runs */
static time_t g_last;                  /* Local time at the last tick */
static time_t g_bootbase = -1;         /* Real time less uptime at start */

static struct
{
  NXWINDOW win;                        /* NULL: nothing rings */
  int      n;
  struct nt_item_s item[NT_MAX];
  int      hot;                        /* Button under the pointer, -1 */
  int      down;                       /* ... pressed, -1 */
  uint8_t  last;
  int      ticks;                      /* Since it began ringing */
  int      x;
  struct desk_ops_s ops;
} g_nt;

/****************************************************************************
 * Private Functions: numbers
 ****************************************************************************/

static float fabs_f(float x)
{
  return x < 0 ? -x : x;
}

/* The square root without libm: a guess from the exponent, then Newton */

static float sqrt_f(float x)
{
  union
  {
    float f;
    uint32_t i;
  } u;

  float y;

  if (x <= 0)
    {
      return 0;
    }

  u.f = x;
  u.i = (u.i >> 1) + 0x1fbd1df5;
  y = u.f;
  y = 0.5f * (y + x / y);
  y = 0.5f * (y + x / y);
  y = 0.5f * (y + x / y);
  return y;
}

static float floor_f(float x)
{
  int i = (int)x;

  return (float)(x < (float)i ? i - 1 : i);
}

float clk_sin(float turn)
{
  float x;
  float x2;

  turn -= floor_f(turn);
  x = turn * 2 * PI_F;
  if (x > PI_F)
    {
      x -= 2 * PI_F;
    }

  if (x > PI_F / 2)
    {
      x = PI_F - x;
    }
  else if (x < -PI_F / 2)
    {
      x = -PI_F - x;
    }

  x2 = x * x;
  return x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 *
                                           (1 - x2 / 72))));
}

float clk_cos(float turn)
{
  return clk_sin(turn + 0.25f);
}

/* The angle of (x, y) in turns, clockwise from up (y grows down): 0..1 */

static float turn_of(float x, float y)
{
  float ax = fabs_f(x);
  float ay = fabs_f(y);
  float mx = ax > ay ? ax : ay;
  float mn = ax > ay ? ay : ax;
  float a;
  float s;
  float r;

  if (mx == 0)
    {
      return 0;
    }

  /* atan(mn / mx), then into the octant: the angle from up, clockwise,
   * is atan2(x, -y)
   */

  a = mn / mx;
  s = a * a;
  r = ((-0.0464964749f * s + 0.15931422f) * s - 0.327622764f) * s * a + a;
  if (ax > ay)                         /* |x| > |y|: from the vertical */
    {
      r = PI_F / 2 - r;
    }

  if (y > 0)                           /* Below the centre */
    {
      r = PI_F - r;
    }

  r /= 2 * PI_F;
  return x < 0 ? 1 - r : r;
}

/****************************************************************************
 * Private Functions: drawing
 ****************************************************************************/

/* A shape: its coverage of the pixel whose centre is (px, py), 0..1, and
 * the bounds of its row y where the coverage may not be 0
 */

struct shape_s
{
  int      kind;
  float    cx;                         /* Disc, ring, arc: the centre; */
  float    cy;
  float    ri;                         /* ... radii (a disc: ro) */
  float    ro;
  float    turn;                       /* Arc: how far */
  float    ax;                          /* Segment: a to b, half its width */
  float    ay;
  float    bx;
  float    by;
  float    hw;
  float    ux;                         /* ... its direction, its length */
  float    uy;
  float    len;
};

#define SH_DISC  0
#define SH_RING  1
#define SH_ARC   2
#define SH_SEG   3

static float clamp01(float v)
{
  return v < 0 ? 0 : v > 1 ? 1 : v;
}

static float cov_disc(float x, float y, float cx, float cy, float r)
{
  float dx = x - cx;
  float dy = y - cy;

  return clamp01(r + 0.5f - sqrt_f(dx * dx + dy * dy));
}

static float coverage(const struct shape_s *s, float x, float y)
{
  float dx;
  float dy;
  float d;
  float rad;

  switch (s->kind)
    {
      case SH_DISC:
        return cov_disc(x, y, s->cx, s->cy, s->ro);

      case SH_RING:
      case SH_ARC:
        dx  = x - s->cx;
        dy  = y - s->cy;
        d   = sqrt_f(dx * dx + dy * dy);
        rad = s->ro + 0.5f - d;
        if (d - s->ri + 0.5f < rad)
          {
            rad = d - s->ri + 0.5f;
          }

        rad = clamp01(rad);
        if (s->kind == SH_ARC && s->turn < 1)
          {
            float t = turn_of(dx, dy);
            float rm = (s->ri + s->ro) / 2;
            float rc = (s->ro - s->ri) / 2;
            float sa;
            float c;

            sa  = t <= s->turn ? (t < s->turn - t ? t : s->turn - t) :
                  -(1 - t < t - s->turn ? 1 - t : t - s->turn);
            rad *= clamp01(sa * 2 * PI_F * d + 0.5f);

            /* The round ends */

            c = cov_disc(x, y, s->cx, s->cy - rm, rc);
            rad = c > rad ? c : rad;
            c = cov_disc(x, y, s->cx + rm * clk_sin(s->turn),
                         s->cy - rm * clk_cos(s->turn), rc);
            rad = c > rad ? c : rad;
          }

        return rad;

      default:
        {
          float t;

          dx = x - s->ax;
          dy = y - s->ay;
          t  = dx * s->ux + dy * s->uy;
          t  = t < 0 ? 0 : t > s->len ? s->len : t;
          dx -= t * s->ux;
          dy -= t * s->uy;
          return clamp01(s->hw + 0.5f - sqrt_f(dx * dx + dy * dy));
        }
    }
}

/* Where row yc (its centre) may be covered: false if nowhere */

static void span_widen(float *lo, float *hi, float a, float b)
{
  if (a > b)
    {
      float t = a;

      a = b;
      b = t;
    }

  *lo = a < *lo ? a : *lo;
  *hi = b > *hi ? b : *hi;
}

static bool row_range(const struct shape_s *s, float yc, float *xa,
                      float *xb)
{
  float lo = 1e9f;
  float hi = -1e9f;
  float d;
  float dy;

  if (s->kind != SH_SEG)
    {
      d  = s->ro + 1;
      dy = yc - s->cy;
      if (dy * dy >= d * d)
        {
          return false;
        }

      d   = sqrt_f(d * d - dy * dy);
      *xa = s->cx - d;
      *xb = s->cx + d;
      return true;
    }

  /* A thick segment is convex: its row is one interval, the hull of where
   * the row meets its end discs and its band
   */

  d = s->hw + 1;
  dy = yc - s->ay;
  if (dy * dy < d * d)
    {
      float h = sqrt_f(d * d - dy * dy);

      span_widen(&lo, &hi, s->ax - h, s->ax + h);
    }

  dy = yc - s->by;
  if (dy * dy < d * d)
    {
      float h = sqrt_f(d * d - dy * dy);

      span_widen(&lo, &hi, s->bx - h, s->bx + h);
    }

  if (fabs_f(s->uy) > 1e-4f)
    {
      /* The band: |(x - ax) uy - (yc - ay) ux| <= d, and 0 <= along <=
       * len
       */

      float b0 = s->ax + ((yc - s->ay) * s->ux - d) / s->uy;
      float b1 = s->ax + ((yc - s->ay) * s->ux + d) / s->uy;
      float blo = b0 < b1 ? b0 : b1;
      float bhi = b0 < b1 ? b1 : b0;

      if (fabs_f(s->ux) > 1e-4f)
        {
          float p0 = s->ax + (0 - (yc - s->ay) * s->uy) / s->ux;
          float p1 = s->ax + (s->len - (yc - s->ay) * s->uy) / s->ux;
          float plo = p0 < p1 ? p0 : p1;
          float phi = p0 < p1 ? p1 : p0;

          blo = blo > plo ? blo : plo;
          bhi = bhi < phi ? bhi : phi;
        }
      else
        {
          float y0 = s->ay < s->by ? s->ay : s->by;
          float y1 = s->ay < s->by ? s->by : s->ay;

          if (yc < y0 || yc > y1)
            {
              blo = 1;
              bhi = 0;
            }
        }

      if (blo <= bhi)
        {
          span_widen(&lo, &hi, blo, bhi);
        }
    }
  else if (yc > s->ay - d && yc < s->ay + d)
    {
      span_widen(&lo, &hi, s->ax, s->bx);            /* Horizontal */
    }

  if (lo > hi)
    {
      return false;
    }

  *xa = lo;
  *xb = hi;
  return true;
}

static void run_out(clk_room_t room, void *arg, int x, int y, int n, int q,
                    uint16_t c, uint8_t alpha)
{
  struct desk_ops_s *o;
  int a;

  if (n <= 0 || q <= 0)
    {
      return;
    }

  a = alpha * q / g_levels;
  o = room(arg, 1);
  if (a >= 255)
    {
      ops_fill(o, x, y, n, 1, c);
    }
  else if (a > 0)
    {
      ops_blend(o, x, y, n, 1, c, (uint8_t)a);
    }
}

static void raster(clk_room_t room, void *arg, const struct shape_s *s,
                   uint16_t c, uint8_t alpha, int x0, int y0, int x1,
                   int y1)
{
  float top;
  float bot;
  int y;

  if (s->kind == SH_SEG)
    {
      top = (s->ay < s->by ? s->ay : s->by) - s->hw - 1;
      bot = (s->ay < s->by ? s->by : s->ay) + s->hw + 1;
    }
  else
    {
      top = s->cy - s->ro - 1;
      bot = s->cy + s->ro + 1;
    }

  y0 = (int)top > y0 ? (int)top : y0;
  y1 = (int)bot + 1 < y1 ? (int)bot + 1 : y1;
  for (y = y0; y < y1; y++)
    {
      float xa;
      float xb;
      int xs;
      int xe;
      int x;
      int run = 0;
      int runq = 0;

      if (!row_range(s, y + 0.5f, &xa, &xb))
        {
          continue;
        }

      xs = (int)floor_f(xa);
      xe = (int)floor_f(xb) + 1;
      xs = xs < x0 ? x0 : xs;
      xe = xe > x1 ? x1 : xe;
      for (x = xs; x < xe; x++)
        {
          int q = (int)(coverage(s, x + 0.5f, y + 0.5f) * g_levels + 0.5f);

          if (q == runq)
            {
              run++;
              continue;
            }

          run_out(room, arg, x - run, y, run, runq, c, alpha);
          run  = 1;
          runq = q;
        }

      run_out(room, arg, xe - run, y, run, runq, c, alpha);
    }
}

/****************************************************************************
 * Public Functions: drawing
 ****************************************************************************/

void clk_levels(int levels)
{
  g_levels = levels < 2 ? 2 : levels > 16 ? 16 : levels;
}

struct desk_ops_s *clk_room_ops(void *arg, int n)
{
  return arg;                          /* (Dropped when full) */
}

void clk_seg(clk_room_t room, void *arg, float ax, float ay, float bx,
             float by, float hw, uint16_t c, uint8_t alpha, int x0, int y0,
             int x1, int y1)
{
  struct shape_s s;
  float dx = bx - ax;
  float dy = by - ay;

  memset(&s, 0, sizeof(s));
  s.kind = SH_SEG;
  s.ax   = ax;
  s.ay   = ay;
  s.bx   = bx;
  s.by   = by;
  s.hw   = hw;
  s.len  = sqrt_f(dx * dx + dy * dy);
  s.ux   = s.len > 0 ? dx / s.len : 1;
  s.uy   = s.len > 0 ? dy / s.len : 0;
  raster(room, arg, &s, c, alpha, x0, y0, x1, y1);
}

void clk_disc(clk_room_t room, void *arg, float cx, float cy, float r,
              uint16_t c, uint8_t alpha, int x0, int y0, int x1, int y1)
{
  struct shape_s s;

  memset(&s, 0, sizeof(s));
  s.kind = SH_DISC;
  s.cx   = cx;
  s.cy   = cy;
  s.ro   = r;
  raster(room, arg, &s, c, alpha, x0, y0, x1, y1);
}

void clk_ring(clk_room_t room, void *arg, float cx, float cy, float ri,
              float ro, uint16_t c, uint8_t alpha, int x0, int y0, int x1,
              int y1)
{
  struct shape_s s;

  memset(&s, 0, sizeof(s));
  s.kind = SH_RING;
  s.cx   = cx;
  s.cy   = cy;
  s.ri   = ri;
  s.ro   = ro;
  raster(room, arg, &s, c, alpha, x0, y0, x1, y1);
}

void clk_arc(clk_room_t room, void *arg, float cx, float cy, float ri,
             float ro, float turn, uint16_t c, uint8_t alpha, int x0,
             int y0, int x1, int y1)
{
  struct shape_s s;

  if (turn <= 0)
    {
      return;
    }

  memset(&s, 0, sizeof(s));
  s.kind = SH_ARC;
  s.cx   = cx;
  s.cy   = cy;
  s.ri   = ri;
  s.ro   = ro;
  s.turn = turn;
  raster(room, arg, &s, c, alpha, x0, y0, x1, y1);
}

/****************************************************************************
 * Public Functions: time
 ****************************************************************************/

int64_t clk_ms(void)
{
  struct timespec ts;

  if (desk_frozen())
    {
      return (int64_t)desk_time() * 1000;
    }

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

time_t clk_local(int *ms)
{
  struct timespec ts;
  time_t t;

  if (desk_frozen())
    {
      t = desk_time();
      ts.tv_nsec = 0;
    }
  else
    {
      clock_gettime(CLOCK_REALTIME, &ts);
      t = ts.tv_sec;
    }

  if (ms != NULL)
    {
      *ms = ts.tv_nsec / 1000000;
    }

  return t + (time_t)g_prefs.tzoff * 60;
}

/* Real time less the time since boot: where the clock started.  Until the
 * clock is set it is the build's start date (CONFIG_START_YEAR ...), or
 * 1970; setting it moves it.
 */

static time_t boot_base(void)
{
  struct timespec rt;
  struct timespec mono;

  clock_gettime(CLOCK_REALTIME, &rt);
  clock_gettime(CLOCK_MONOTONIC, &mono);
  return rt.tv_sec - mono.tv_sec;
}

bool clk_time_set(void)
{
  if (desk_frozen())
    {
      return desk_time() >= 1577836800;           /* 2020 */
    }

  if (time(NULL) < 1577836800)
    {
      return false;
    }

#ifdef CONFIG_RTC
  return true;
#else
  {
    time_t base = boot_base();
    time_t d = base - g_bootbase;

    /* Started at the build's date, and not set since */

    if (g_bootbase >= 0 && d >= -2 && d <= 2)
      {
        struct tm tm;
        time_t start;

        memset(&tm, 0, sizeof(tm));
        tm.tm_year = CONFIG_START_YEAR - 1900;
        tm.tm_mon  = CONFIG_START_MONTH - 1;
        tm.tm_mday = CONFIG_START_DAY;
        start = timegm(&tm);
        d = base - start;
        return d < -2 || d > 2;
      }

    return true;
  }
#endif
}

const char *clk_home_file(char *buf, size_t len, const char *name)
{
  const char *home = getenv("HOME");

  if (home == NULL)
    {
      home = CONFIG_LIBC_HOMEDIR;
    }

  snprintf(buf, len, "%s%s%s", home,
           home[0] != '\0' && home[strlen(home) - 1] == '/' ? "" : "/",
           name);
  return buf;
}

void clk_fmt_hm(char *buf, size_t len, int hour, int min)
{
  if (g_prefs.clock24)
    {
      snprintf(buf, len, "%02d:%02d", hour, min);
    }
  else
    {
      snprintf(buf, len, "%d:%02d %s", (hour + 11) % 12 + 1, min,
               hour < 12 ? "AM" : "PM");
    }
}

void clk_fmt_days(char *buf, size_t len, int days)
{
  static const char * const names[7] =
  {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
  };

  size_t n = 0;
  int i;

  days &= 0x7f;
  if (days == 0)
    {
      snprintf(buf, len, "Once");
      return;
    }

  if (days == 0x7f)
    {
      snprintf(buf, len, "Every day");
      return;
    }

  if (days == 0x3e)
    {
      snprintf(buf, len, "Weekdays");
      return;
    }

  if (days == 0x41)
    {
      snprintf(buf, len, "Weekends");
      return;
    }

  buf[0] = '\0';
  for (i = 1; i <= 7; i++)                     /* Monday first */
    {
      if (days & (1 << (i % 7)))
        {
          n += snprintf(buf + n, n < len ? len - n : 0, "%s%s",
                        n > 0 ? ", " : "", names[i % 7]);
        }
    }
}

/****************************************************************************
 * Public Functions: the timer and the stopwatch
 ****************************************************************************/

int64_t clk_timer_left(void)
{
  int64_t left;

  if (g_clk.timer_state != CLK_RUNNING)
    {
      return g_clk.timer_left;
    }

  left = g_clk.timer_end - clk_ms();
  return left > 0 ? left : 0;
}

void clk_timer_start(void)
{
  if (g_clk.timer_state == CLK_STOPPED)
    {
      g_clk.timer_left = (int64_t)g_clk.timer_set * 1000;
    }

  if (g_clk.timer_left <= 0)
    {
      return;
    }

  g_clk.timer_end   = clk_ms() + g_clk.timer_left;
  g_clk.timer_state = CLK_RUNNING;
}

void clk_timer_pause(void)
{
  if (g_clk.timer_state == CLK_RUNNING)
    {
      g_clk.timer_left  = clk_timer_left();
      g_clk.timer_state = CLK_PAUSED;
    }
}

void clk_timer_reset(void)
{
  g_clk.timer_state = CLK_STOPPED;
  g_clk.timer_left  = (int64_t)g_clk.timer_set * 1000;
}

int64_t clk_sw_elapsed(void)
{
  return g_clk.sw_acc + (g_clk.sw_state == CLK_RUNNING ?
                         clk_ms() - g_clk.sw_start : 0);
}

void clk_sw_toggle(void)
{
  if (g_clk.sw_state == CLK_RUNNING)
    {
      g_clk.sw_acc  += clk_ms() - g_clk.sw_start;
      g_clk.sw_state = CLK_PAUSED;
    }
  else
    {
      g_clk.sw_start = clk_ms();
      g_clk.sw_state = CLK_RUNNING;
    }
}

void clk_sw_lap_reset(void)
{
  if (g_clk.sw_state == CLK_RUNNING)
    {
      if (g_clk.nlaps < CLK_LAPS_MAX)
        {
          g_clk.lap[g_clk.nlaps++] = clk_sw_elapsed();
        }
    }
  else
    {
      g_clk.sw_state = CLK_STOPPED;
      g_clk.sw_acc   = 0;
      g_clk.nlaps    = 0;
    }
}

/****************************************************************************
 * Private Functions: what is kept
 ****************************************************************************/

static void clk_load(void)
{
  char path[64];
  char line[80];
  FILE *f;

  memset(&g_clk, 0, sizeof(g_clk));
  g_clk.loaded   = true;
  g_clk.timer_set = 5 * 60;
  f = fopen(clk_home_file(path, sizeof(path), ".clock"), "r");
  if (f != NULL)
    {
      while (fgets(line, sizeof(line), f) != NULL)
        {
          char *nl = strchr(line, '\n');
          int h;
          int m;
          int d;
          int on;
          int n;
          int i;

          if (nl != NULL)
            {
              *nl = '\0';
            }

          if (strncmp(line, "city ", 5) == 0 &&
              g_clk.ncities < CLK_CITIES_MAX)
            {
              for (i = 0; i < g_clk_ncities; i++)
                {
                  if (strcmp(g_clk_cities[i].name, line + 5) == 0)
                    {
                      g_clk.city[g_clk.ncities++] = i;
                      break;
                    }
                }
            }
          else if (sscanf(line, "alarm %d %d %d %d %n", &h, &m, &d, &on,
                          &n) == 4 && g_clk.nalarms < CLK_ALARMS_MAX)
            {
              struct clk_alarm_s *a = &g_clk.alarm[g_clk.nalarms++];

              a->hour = h < 0 || h > 23 ? 0 : h;
              a->min  = m < 0 || m > 59 ? 0 : m;
              a->days = d & 0x7f;
              a->on   = on != 0;
              strlcpy(a->label, line + n, sizeof(a->label));
            }
          else if (sscanf(line, "timer %d", &n) == 1 && n > 0 &&
                   n < 100 * 3600)
            {
              g_clk.timer_set = n;
            }
        }

      fclose(f);
    }

  g_clk.timer_left = (int64_t)g_clk.timer_set * 1000;
}

void clk_save(void)
{
  char path[64];
  FILE *f;
  int i;

  f = fopen(clk_home_file(path, sizeof(path), ".clock"), "w");
  if (f == NULL)
    {
      return;
    }

  for (i = 0; i < g_clk.ncities; i++)
    {
      fprintf(f, "city %s\n", g_clk_cities[g_clk.city[i]].name);
    }

  for (i = 0; i < g_clk.nalarms; i++)
    {
      const struct clk_alarm_s *a = &g_clk.alarm[i];

      fprintf(f, "alarm %d %d %d %d %s\n", a->hour, a->min, a->days,
              a->on, a->label);
    }

  fprintf(f, "timer %lu\n", (unsigned long)g_clk.timer_set);
  fclose(f);
}

/****************************************************************************
 * Private Functions: the notification
 ****************************************************************************/

/* Its buttons: [0] the right one (Stop, Dismiss), [1] the other */

static const char *nt_button(const struct nt_item_s *it, int b)
{
  switch (it->kind)
    {
      case CLK_RING_ALARM:
        return b == 0 ? "Stop" : "Snooze";

      case CLK_RING_TIMER:
        return b == 0 ? "Stop" : "Repeat";

      default:
        return b == 0 ? "Dismiss" : "Open";
    }
}

static void nt_button_rect(int b, int *x, int *w)
{
  *w = 76;
  *x = NT_W - 14 - 76 - b * (76 + 8);
}

static void nt_draw(void)
{
  struct desk_ops_s *o = &g_nt.ops;
  const struct nt_item_s *it;
  uint16_t fg = g_theme.popup_text;
  uint16_t tile;
  char title[48];
  int b;

  if (g_nt.win == NULL || g_nt.n == 0)
    {
      return;
    }

  it   = &g_nt.item[g_nt.n - 1];
  tile = it->kind == CLK_RING_EVENT ? RGB(0xff, 0x3b, 0x30) :
         RGB(0xff, 0x95, 0x00);
  ops_reset(o);

  /* A small clock on a round tile */

  ops_rrect(o, 14, 16, 40, 40, LUT_R12, 12, tile, 255);
  ops_disc_atlas(o, 23, 25, 22, C_WHITE, 255);
  ops_fill(o, 33, 28, 2, 9, tile);
  ops_fill(o, 34, 35, 6, 2, tile);

  if (g_nt.n > 1)
    {
      snprintf(title, sizeof(title), "%s (+%d)", it->title, g_nt.n - 1);
    }
  else
    {
      strlcpy(title, it->title, sizeof(title));
    }

  ops_text(o, &g_desk_font_uib, 66, 17, title, fg, 255);
  ops_text(o, &g_desk_font_ui, 66, 37, it->text, fg, 170);

  for (b = 0; b < 2; b++)
    {
      int x;
      int w;
      int look = g_nt.down == b ? KIT_LOOK_DOWN :
                 g_nt.hot == b ? KIT_LOOK_HOT : KIT_LOOK_NORMAL;

      nt_button_rect(b, &x, &w);
      kit_paint_button(o, x, (NT_H - NT_BTN_H) / 2, w, NT_BTN_H,
                       nt_button(it, b), KIT_ICON_NONE,
                       b == 0 ? KIT_BTN_DEFAULT : KIT_BTN_NORMAL, look,
                       false);
    }

  ops_setlist(g_nt.win, o);
}

static void nt_place(void)
{
  struct nxgl_point_s pos;
  struct nxgl_size_s size;

  size.w = NT_W;
  size.h = NT_H;
  pos.x  = DESK_W - NT_W - 12;
  pos.y  = TOPBAR_H + 10;
  g_nt.x = pos.x;
  nx_setsize(g_nt.win, &size);
  nx_setposition(g_nt.win, &pos);
}

static void nt_open(void)
{
  struct nx_effects_s fx;

  if (g_nt.win != NULL)
    {
      return;
    }

  g_nt.win = nx_openwindow(g_desk.hnx,
                           NXBE_WINDOW_LAYER | NXBE_WINDOW_HIDDEN,
                           &g_desk_partcb, (void *)(uintptr_t)PART_NOTIFY);
  if (g_nt.win == NULL)
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

  nx_seteffects(g_nt.win, &fx);
  g_nt.hot  = -1;
  g_nt.down = -1;
  nt_place();
  nt_draw();
  nx_setvisibility(g_nt.win, false);
}

static void nt_close(void)
{
  if (g_nt.win != NULL)
    {
      nx_closewindow(g_nt.win);
      g_nt.win = NULL;
    }
}

static void nt_sound(void)
{
#ifdef CONFIG_FPCDESK_SOUND
  int i;

  for (i = 0; i < g_nt.n; i++)
    {
      if (g_nt.item[i].kind != CLK_RING_EVENT)
        {
          desk_sound_play(g_nt.item[i].kind == CLK_RING_TIMER ? SND_TIMER :
                          SND_ALARM);
          return;
        }
    }
#endif
}

/* The one shown (the latest) answered: it goes, the one before shows */

static void nt_answer(int b)
{
  struct nt_item_s it = g_nt.item[--g_nt.n];

  g_nt.hot  = -1;
  g_nt.down = -1;
  if (b == 1)
    {
      switch (it.kind)
        {
          case CLK_RING_ALARM:
            if (it.what >= 0 && it.what < g_clk.nalarms)
              {
                g_clk.alarm[it.what].snooze = desk_time() + CLK_SNOOZE_S;
              }
            break;

          case CLK_RING_TIMER:
            clk_timer_start();
            break;

          default:
            desk_launch(APP_CALENDAR);
            break;
        }
    }

  if (g_nt.n == 0)
    {
      nt_close();
    }
  else
    {
      nt_draw();
    }

  desk_clock_changed();
}

/****************************************************************************
 * Public Functions: the notification
 ****************************************************************************/

void desk_notify_ring(int kind, int what, const char *title,
                      const char *text)
{
  struct nt_item_s *it;

  if (g_nt.n == NT_MAX)
    {
      g_nt.n--;                                /* The oldest goes */
      memmove(&g_nt.item[0], &g_nt.item[1],
              g_nt.n * sizeof(g_nt.item[0]));
    }

  it = &g_nt.item[g_nt.n++];
  memset(it, 0, sizeof(*it));
  it->kind = kind;
  it->what = what;
  strlcpy(it->title, title, sizeof(it->title));
  strlcpy(it->text, text, sizeof(it->text));
  g_nt.ticks = 0;
  if (g_nt.win == NULL)
    {
      nt_open();
    }
  else
    {
      nt_draw();
    }

#ifdef CONFIG_FPCDESK_SOUND
  if (kind == CLK_RING_EVENT)
    {
      desk_sound_play(SND_NOTIFY);
    }
  else
    {
      nt_sound();
    }
#endif
}

/* Alarm i has gone: what rings for it rings for nothing, the others'
 * indices follow
 */

void desk_notify_forget(int i)
{
  int k;

  for (k = 0; k < g_nt.n; k++)
    {
      struct nt_item_s *it = &g_nt.item[k];

      if (it->kind == CLK_RING_ALARM && it->what == i)
        {
          it->what = -1;
        }
      else if (it->kind == CLK_RING_ALARM && it->what > i)
        {
          it->what--;
        }
    }
}

bool desk_notify_ringing(void)
{
  return g_nt.n > 0;
}

void desk_notify_mouse(int x, int y, uint8_t buttons)
{
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(g_nt.last & NX_MOUSE_LEFTBUTTON);
  bool rel = !(buttons & NX_MOUSE_LEFTBUTTON) &&
             (g_nt.last & NX_MOUSE_LEFTBUTTON);
  int hot = -1;
  int b;

  g_nt.last = buttons;
  if (g_nt.win == NULL)
    {
      return;
    }

  for (b = 0; b < 2; b++)
    {
      int bx;
      int bw;

      nt_button_rect(b, &bx, &bw);
      if (x >= bx && x < bx + bw && y >= (NT_H - NT_BTN_H) / 2 &&
          y < (NT_H + NT_BTN_H) / 2)
        {
          hot = b;
        }
    }

  if (press)
    {
      g_nt.down = hot;
    }

  if (rel)
    {
      b = g_nt.down;
      g_nt.down = -1;
      if (b >= 0 && b == hot)
        {
          nt_answer(b);
          return;
        }
    }

  if (hot != g_nt.hot || press || rel)
    {
      g_nt.hot = hot;
      nt_draw();
    }
}

/****************************************************************************
 * Public Functions: the service
 ****************************************************************************/

void desk_clock_init(void)
{
  if (!g_clk.loaded)
    {
      clk_load();
    }

  cal_init();                          /* The events' reminders */
  g_bootbase = boot_base();
  g_last     = clk_local(NULL);
}

/* Alarm a rings if its minute is in (from, to] (local seconds) on one of
 * its days
 */

static bool alarm_due(const struct clk_alarm_s *a, time_t from, time_t to)
{
  time_t day = to - to % 86400;
  int k;

  for (k = 0; k < 2; k++, day -= 86400)
    {
      time_t at = day + a->hour * 3600 + a->min * 60;
      int wday = (int)((at / 86400 + 4) % 7);       /* 1970-1-1: Thursday */

      if (at > from && at <= to &&
          (a->days == 0 || (a->days & (1 << wday)) != 0))
        {
          return true;
        }
    }

  return false;
}

static void alarm_ring(int i)
{
  struct clk_alarm_s *a = &g_clk.alarm[i];
  char text[48];
  char hm[16];

  clk_fmt_hm(hm, sizeof(hm), a->hour, a->min);
  snprintf(text, sizeof(text), "%s%s%s", hm, a->label[0] ? "  " : "",
           a->label);
  desk_notify_ring(CLK_RING_ALARM, i, "Alarm", text);
}

/* The timer at zero: it rings (true), and is ready to start again */

bool clk_timer_check(void)
{
  char text[48];
  int s = g_clk.timer_set;

  if (g_clk.timer_state != CLK_RUNNING || clk_timer_left() > 0)
    {
      return false;
    }

  clk_timer_reset();
  if (s >= 3600)
    {
      snprintf(text, sizeof(text), "%d:%02d:%02d is up", s / 3600,
               s / 60 % 60, s % 60);
    }
  else
    {
      snprintf(text, sizeof(text), "%d:%02d is up", s / 60, s % 60);
    }

  desk_notify_ring(CLK_RING_TIMER, -1, "Timer", text);
  return true;
}

void desk_clock_tick(void)
{
  time_t now = clk_local(NULL);
  time_t utc = desk_time();
  bool changed = false;
  int i;

  if (!g_clk.loaded)
    {
      return;
    }

  /* What has come since the last tick: not what a clock that was set, or
   * jumped, skipped over
   */

  if (now > g_last && now - g_last <= NT_JUMP_S)
    {
      for (i = 0; i < g_clk.nalarms; i++)
        {
          struct clk_alarm_s *a = &g_clk.alarm[i];

          if (a->on && alarm_due(a, g_last, now))
            {
              a->snooze = 0;
              if (a->days == 0)
                {
                  a->on = false;                  /* Once: done */
                  changed = true;
                }

              alarm_ring(i);
            }
        }

      cal_reminders(g_last, now);
    }

  g_last = now;

  for (i = 0; i < g_clk.nalarms; i++)
    {
      if (g_clk.alarm[i].snooze != 0 && utc >= g_clk.alarm[i].snooze)
        {
          g_clk.alarm[i].snooze = 0;
          alarm_ring(i);
        }
    }

  changed |= clk_timer_check();
  if (changed)
    {
      clk_save();
      desk_clock_changed();
    }

  /* Ringing: the sound again every two seconds until it is answered */

  if (g_nt.n > 0 && ++g_nt.ticks % 2 == 0)
    {
      nt_sound();
    }
}

void desk_clock_relayout(void)
{
  desk_calpop_close();
  if (g_nt.win != NULL)
    {
      nt_close();                     /* The theme's glass, the new place */
      nt_open();
    }
}

void desk_clock_mouse_any(int part, uint8_t buttons)
{
  if (part != PART_NOTIFY && g_nt.win != NULL &&
      (g_nt.hot >= 0 || g_nt.down >= 0))
    {
      g_nt.hot  = -1;
      g_nt.down = -1;
      g_nt.last = buttons;
      nt_draw();
    }

  desk_calpop_mouse_any(part, buttons);
}
