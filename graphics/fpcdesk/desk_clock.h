/****************************************************************************
 * apps/graphics/fpcdesk/desk_clock.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clock and Calendar:
 *
 *   desk_alarm.c      the desktop's clock service: alarms, the timer and
 *                     the stopwatch live here, in the desktop, so that they
 *                     go on (and alarms ring) with the Clock window closed;
 *                     their notification (a banner at the top right, with
 *                     its buttons) and the sound that repeats until it is
 *                     dismissed; what is kept (.clock in the home
 *                     directory); drawing helpers (anti-aliased discs,
 *                     rings, arcs and lines as runs of pixels)
 *   desk_clock.c      Clock: the analog clock and world clock, alarms, the
 *                     stopwatch, the timer
 *   desk_calendar.c   Calendar: the events (.calendar in the home
 *                     directory), a month view, the day's events and an
 *                     editor; the top bar's calendar popover
 *
 * Hooks in the desktop (all on its main thread):
 *   main():               desk_clock_init() once the panels are up
 *   tick():               desk_clock_tick() once a second
 *   on_mouse():           desk_clock_mouse_any(part, buttons) first, and
 *                         PART_NOTIFY: desk_notify_mouse(),
 *                         PART_CALPOP: desk_calpop_mouse()
 *   desk_topbar_draw():   desk_calpop_topbar(o, x, y, w) where the clock is
 *   desk_topbar_mouse():  desk_calpop_topbar_mouse() (true: on the clock)
 *   desk_panels_relayout(): desk_clock_relayout() (theme, display mode)
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_CLOCK_H
#define __APPS_GRAPHICS_FPCDESK_DESK_CLOCK_H

#include <stdint.h>
#include <time.h>

#include "desk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CLK_ALARMS_MAX    10
#define CLK_CITIES_MAX    8
#define CLK_LAPS_MAX      99
#define CLK_LABEL_MAX     32
#define CLK_SNOOZE_S      (9 * 60)         /* Snooze: nine minutes */

#define CLK_STOPPED       0                /* The timer and the stopwatch */
#define CLK_RUNNING       1
#define CLK_PAUSED        2

/* What rings (a notification) */

#define CLK_RING_ALARM    0
#define CLK_RING_TIMER    1
#define CLK_RING_EVENT    2

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* An alarm: at hour:min local time on the days of days (bit 0 Sunday ..
 * bit 6 Saturday), or once (days 0: it turns itself off when it rings)
 */

struct clk_alarm_s
{
  uint8_t  hour;
  uint8_t  min;
  uint8_t  days;
  bool     on;
  time_t   snooze;                     /* Rings again then (UTC), 0: not */
  char     label[CLK_LABEL_MAX];
};

/* A city of the world clock: a fixed offset from UTC (no daylight saving
 * time: there is no time zone database, CONFIG_LIBC_LOCALTIME is off)
 */

struct clk_city_s
{
  const char *name;
  int16_t  off;                        /* Minutes east of UTC */
};

struct clk_svc_s
{
  bool     loaded;

  /* Alarms */

  int      nalarms;
  struct clk_alarm_s alarm[CLK_ALARMS_MAX];

  /* The world clock: cities by their index in g_clk_cities */

  int      ncities;
  uint8_t  city[CLK_CITIES_MAX];

  /* The timer: set for timer_set seconds; while running it ends at
   * timer_end (clk_ms()); stopped or paused, timer_left ms are left
   */

  uint8_t  timer_state;
  uint32_t timer_set;
  int64_t  timer_end;
  int64_t  timer_left;

  /* The stopwatch: sw_acc ms before sw_start (clk_ms()) while running;
   * laps as the total at each
   */

  uint8_t  sw_state;
  int64_t  sw_start;
  int64_t  sw_acc;
  int      nlaps;
  int64_t  lap[CLK_LAPS_MAX];
};

/* A list of operations with room: returns a list with room for n more,
 * sending what it has first if it must (the kit's, a surface's)
 */

typedef struct desk_ops_s *(*clk_room_t)(void *arg, int n);

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern struct clk_svc_s g_clk;
extern const struct clk_city_s g_clk_cities[];
extern const int g_clk_ncities;
extern const struct desk_app_s g_app_clock;      /* desk_clock.c */
extern const struct desk_app_s g_app_calendar;   /* desk_calendar.c */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* desk_alarm.c: the service */

void desk_clock_init(void);
void desk_clock_tick(void);
void desk_clock_relayout(void);
void desk_clock_mouse_any(int part, uint8_t buttons);
void clk_save(void);

/* A clock in ms for the stopwatch and the timer: the monotonic clock; while
 * the desktop is frozen (deskctl freeze, tests) the frozen time, so a test
 * moves it by freezing another time
 */

int64_t clk_ms(void);

/* The time as the desktop shows it (desk_time() plus the time zone), with
 * its milliseconds (0 while frozen)
 */

time_t clk_local(int *ms);

/* Whether the clock has been set since boot (there is no battery-backed
 * clock: it starts at the build's date, or 1970)
 */

bool clk_time_set(void);

/* The timer's and the stopwatch's ms now */

int64_t clk_timer_left(void);
int64_t clk_sw_elapsed(void);
void clk_timer_start(void);
void clk_timer_pause(void);
void clk_timer_reset(void);
bool clk_timer_check(void);            /* At zero: it rings (true) */
void clk_sw_toggle(void);
void clk_sw_lap_reset(void);

/* A file in the home directory */

const char *clk_home_file(char *buf, size_t len, const char *name);

/* Formatting: "07:30" or "7:30 AM" as Settings have the clock; repeat days
 * as words ("Weekdays", "Every day", "Mon, Wed")
 */

void clk_fmt_hm(char *buf, size_t len, int hour, int min);
void clk_fmt_days(char *buf, size_t len, int days);

/* Notifications: something rings (an alarm, the timer, an event's
 * reminder) until it is dismissed.  what: the index of the alarm.
 */

void desk_notify_ring(int kind, int what, const char *title,
                      const char *text);
void desk_notify_mouse(int x, int y, uint8_t buttons);
bool desk_notify_ringing(void);
void desk_notify_forget(int alarm);    /* It was deleted */

/* The Clock window is told when the service changed something (an alarm
 * rang and turned itself off, the timer ended)
 */

void desk_clock_changed(void);

/* Drawing (desk_alarm.c): anti-aliased shapes as runs of pixels, only
 * those in [x0, x1) x [y0, y1); coordinates in pixels, a pixel's centre at
 * +0.5
 */

void clk_seg(clk_room_t room, void *arg, float ax, float ay, float bx,
             float by, float hw, uint16_t c, uint8_t alpha, int x0, int y0,
             int x1, int y1);
void clk_disc(clk_room_t room, void *arg, float cx, float cy, float r,
              uint16_t c, uint8_t alpha, int x0, int y0, int x1, int y1);
void clk_ring(clk_room_t room, void *arg, float cx, float cy, float ri,
              float ro, uint16_t c, uint8_t alpha, int x0, int y0, int x1,
              int y1);

/* An arc of the ring from 12 o'clock clockwise to turn (0..1), round
 * ends
 */

void clk_arc(clk_room_t room, void *arg, float cx, float cy, float ri,
             float ro, float turn, uint16_t c, uint8_t alpha, int x0,
             int y0, int x1, int y1);
void clk_levels(int levels);           /* Of coverage: 16, fewer for
                                        * fewer operations (icons) */
float clk_sin(float turn);             /* sin(2 pi turn) */
float clk_cos(float turn);
struct desk_ops_s *clk_room_ops(void *arg, int n);   /* arg: the list */

/* desk_calendar.c: the top bar's calendar popover */

void desk_calpop_topbar(struct desk_ops_s *o, int x, int y, int w);
bool desk_calpop_topbar_mouse(int x, int y, uint8_t buttons);
void desk_calpop_mouse(int x, int y, uint8_t buttons);
void desk_calpop_close(void);
void desk_calpop_mouse_any(int part, uint8_t buttons);

/* Events of a day, for the popover and reminders: how many, and the first
 * max of them (indices into the store, by time)
 */

void cal_init(void);                   /* Load them (once) */
void cal_reminders(time_t from, time_t to);   /* Local seconds */
int  cal_events_on(int year, int mon, int day, int *idx, int max);
void cal_event_text(int i, char *buf, size_t len);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_CLOCK_H */
