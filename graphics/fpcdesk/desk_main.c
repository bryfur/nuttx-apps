/****************************************************************************
 * apps/graphics/fpcdesk/desk_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The FPC desktop: starts the NX server and builds the desktop out of
 * windows its compositor (CONFIG_NX_COMPOSITOR) composites: the wallpaper
 * (desktop layer), the top bar and dock (layer windows: translucent display
 * lists) and application windows (nxtk frames; the toolbar is the title
 * bar).
 *
 * NX callbacks run on the listener thread and only queue events; the rest
 * (events, the once-a-second tick, animation frames) runs on the main thread.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/boardctl.h>

#include <nuttx/input/kbd_codec.h>
#include <nuttx/nx/nxbe.h>

#include "desk.h"
#include "desk_clock.h"
#ifdef CONFIG_FPCDESK_SOUND
#  include "desk_sound.h"
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define QUEUE_LEN         64
#define FRAME_MS          16
#define DOUBLECLICK_MS    400
#define FADE_STEPS        6
#define RESIZE_GRIP       16
#define MIN_W             240
#define MIN_H             120
#define BUTTON_W          28                 /* A title bar button */
#define BUTTON_H          22
#define BUTTON_X(w)       ((w)->size.w - 10 - BUTTON_W)
#define BUTTON_Y          ((TITLE_H - BUTTON_H) / 2)

/****************************************************************************
 * Private Data
 ****************************************************************************/

struct desk_s g_desk;
struct desk_prefs_s g_prefs;

static struct
{
  pthread_mutex_t lock;
  sem_t count;
  int head;
  int n;
  bool wake;                       /* desk_wake(): some window's is due */
  struct desk_event_s ev[QUEUE_LEN];
} g_q;

static int g_cascade;
static bool g_lostq;                     /* EV_LOST is queued */

/****************************************************************************
 * Private Functions: the event queue
 ****************************************************************************/

static bool q_push(const struct desk_event_s *ev)
{
  bool post = true;
  bool ok   = true;

  pthread_mutex_lock(&g_q.lock);

  /* Pointer moves with the same buttons coalesce: only the latest counts */

  if (ev->type == EV_MOUSE && g_q.n > 0)
    {
      struct desk_event_s *last =
        &g_q.ev[(g_q.head + g_q.n - 1) % QUEUE_LEN];

      if (last->type == EV_MOUSE && last->part == ev->part &&
          last->win == ev->win && last->buttons == ev->buttons)
        {
          *last = *ev;
          post  = false;
        }
    }

  if (post)
    {
      if (g_q.n == QUEUE_LEN)
        {
          post = false;                   /* Full: dropped */
          ok   = false;
        }
      else
        {
          g_q.ev[(g_q.head + g_q.n) % QUEUE_LEN] = *ev;
          g_q.n++;
        }
    }

  pthread_mutex_unlock(&g_q.lock);
  if (post)
    {
      sem_post(&g_q.count);
    }

  return ok;
}

static bool q_pop(struct desk_event_s *ev, int timeout_ms)
{
  struct timespec ts;
  int ret;

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_nsec += (long)timeout_ms * 1000000;
  ts.tv_sec  += ts.tv_nsec / 1000000000;
  ts.tv_nsec %= 1000000000;

  do
    {
      ret = sem_timedwait(&g_q.count, &ts);
    }
  while (ret < 0 && errno == EINTR);

  if (ret < 0)
    {
      return false;
    }

  pthread_mutex_lock(&g_q.lock);
  if (g_q.n == 0)
    {
      pthread_mutex_unlock(&g_q.lock);      /* desk_wake()'s post */
      return false;
    }

  *ev = g_q.ev[g_q.head];
  g_q.head = (g_q.head + 1) % QUEUE_LEN;
  g_q.n--;
  pthread_mutex_unlock(&g_q.lock);
  return true;
}

/* The windows desk_wake() was called for: their wake callbacks */

static void q_wakes(void)
{
  struct desk_win_s *w;

  pthread_mutex_lock(&g_q.lock);
  if (!g_q.wake)
    {
      pthread_mutex_unlock(&g_q.lock);
      return;
    }

  g_q.wake = false;
  pthread_mutex_unlock(&g_q.lock);

  for (w = g_desk.wins; w != NULL; w = w->next)
    {
      bool due;

      pthread_mutex_lock(&g_q.lock);
      due      = w->wakeq;
      w->wakeq = false;
      pthread_mutex_unlock(&g_q.lock);

      if (due && !w->closing && desk_win_app(w)->wake != NULL)
        {
          desk_win_app(w)->wake(w);
        }
    }
}

/****************************************************************************
 * Private Functions: NX callbacks (the listener thread)
 ****************************************************************************/

static void cb_redraw(NXWINDOW hwnd, const struct nxgl_rect_s *rect,
                      bool more, void *arg)
{
  /* Every window keeps its pixels: nothing to redraw */
}

/* A client area that has grown: the new territory needs drawing */

static void cb_client_redraw(NXWINDOW hwnd, const struct nxgl_rect_s *rect,
                             bool more, void *arg)
{
  struct desk_event_s ev;

  memset(&ev, 0, sizeof(ev));
  ev.type  = EV_REDRAW;
  ev.win   = arg;
  ev.pos   = rect->pt1;
  ev.size.w = rect->pt2.x - rect->pt1.x + 1;
  ev.size.h = rect->pt2.y - rect->pt1.y + 1;
  q_push(&ev);
}

static void cb_mouse(uint8_t part, struct desk_win_s *w,
                     const struct nxgl_point_s *pos, uint8_t buttons)
{
  struct desk_event_s ev;

  memset(&ev, 0, sizeof(ev));
  ev.type    = EV_MOUSE;
  ev.part    = part;
  ev.win     = w;
  ev.pos     = *pos;
  ev.buttons = buttons;
  q_push(&ev);
}

static void cb_kbd(uint8_t nch, const uint8_t *ch)
{
  struct desk_event_s ev;

  memset(&ev, 0, sizeof(ev));
  ev.type = EV_KBD;
  while (nch > 0)
    {
      /* Whole key events in each (nxkeys.h) */

      ev.nch = nx_key_split(ch, nch, sizeof(ev.ch));
      memcpy(ev.ch, ch, ev.nch);
      q_push(&ev);
      ch  += ev.nch;
      nch -= ev.nch;
    }
}

/* Client areas and title bars of application windows */

static void cb_client_pos(NXWINDOW hwnd, const struct nxgl_size_s *size,
                          const struct nxgl_point_s *pos,
                          const struct nxgl_rect_s *bounds, void *arg)
{
}

static void cb_client_mouse(NXWINDOW hwnd, const struct nxgl_point_s *pos,
                            uint8_t buttons, void *arg)
{
  cb_mouse(PART_CLIENT, arg, pos, buttons);
}

static void cb_title_mouse(NXWINDOW hwnd, const struct nxgl_point_s *pos,
                           uint8_t buttons, void *arg)
{
  cb_mouse(PART_TITLE, arg, pos, buttons);
}

static void cb_kbdin(NXWINDOW hwnd, uint8_t nch, const uint8_t *ch,
                     void *arg)
{
  cb_kbd(nch, ch);
}

static void cb_event(NXWINDOW hwnd, enum nx_event_e event, void *arg1,
                     void *arg2)
{
  struct desk_event_s ev;

  if (event == NXEVENT_SYNCHED)
    {
      sem_post(&((struct desk_win_s *)arg1)->synced);
    }
  else if (event == NXEVENT_BLOCKED)
    {
      memset(&ev, 0, sizeof(ev));
      ev.type = EV_BLOCKED;
      ev.win  = arg1;
      q_push(&ev);
    }
  else if (event == NXEVENT_DISPLAYMODE)
    {
      memset(&ev, 0, sizeof(ev));
      ev.type = EV_MODE;
      q_push(&ev);
    }
}

static const struct nx_callback_s g_clientcb =
{
  cb_client_redraw, cb_client_pos, cb_client_mouse, cb_kbdin, cb_event
};

/* The title bar has lost its pixels: the desktop draws it again */

static void cb_title_redraw(NXWINDOW hwnd, const struct nxgl_rect_s *rect,
                            bool more, void *arg)
{
  struct desk_event_s ev;

  memset(&ev, 0, sizeof(ev));
  ev.type = EV_REDRAW;
  ev.part = PART_TITLE;
  ev.win  = arg;
  q_push(&ev);
}

static const struct nx_callback_s g_titlecb =
{
  cb_title_redraw, cb_client_pos, cb_title_mouse, cb_kbdin, NULL
};

/* The desktop's own windows: the part is the callback argument */

static void cb_part_mouse(NXWINDOW hwnd, const struct nxgl_point_s *pos,
                          uint8_t buttons, void *arg)
{
  cb_mouse((uint8_t)(uintptr_t)arg, NULL, pos, buttons);
}

static void cb_part_event(NXWINDOW hwnd, enum nx_event_e event,
                          void *arg1, void *arg2)
{
  if (event == NXEVENT_DISPLAYMODE)
    {
      cb_event(hwnd, event, NULL, arg2);
    }
}

const struct nx_callback_s g_desk_partcb =
{
  cb_redraw, cb_client_pos, cb_part_mouse, cb_kbdin, cb_part_event
};

/* NX's messages.  Errors go to the syslog (no console) and are survived,
 * unless the server has gone or they persist.
 */

static void *listener(void *arg)
{
  int errors = 0;

  for (; ; )
    {
      if (nx_eventhandler(g_desk.hnx) == 0)
        {
          errors = 0;
          continue;
        }

      if (errno == EINTR)
        {
          continue;
        }

      syslog(LOG_ERR, "desk: NX: %d\n", errno);
      if (errno == EHOSTDOWN || ++errors >= 10)
        {
          syslog(LOG_ERR, "desk: lost the NX server, exiting\n");
          exit(EXIT_FAILURE);
        }

      usleep(10000);
    }

  return NULL;
}

/****************************************************************************
 * Private Functions: windows
 ****************************************************************************/

/* Transitions, where the compositor can show a window in perspective
 * (NX_FX_QUAD, a v10 GPU; elsewhere only its opacity changes): opening,
 * it swings up from a little smaller and tilted away, and closing back;
 * minimising, it is drawn into its dock icon, the bottom first, and
 * restored out of it.  The corners, as offsets from the window's own.
 */

#define QUAD_FOCAL      1200.0f           /* The viewer, pixels away */
#define QUAD_TILT       0.35f             /* Radians, fully tilted */
#define QUAD_SHRINK     0.12f

static void quad_tilt(struct desk_win_s *w, float t, int16_t q[8])
{
  static const float cu[4] = { -0.5f, 0.5f, 0.5f, -0.5f };
  static const float cv[4] = { -0.5f, -0.5f, 0.5f, 0.5f };
  float s = 1.0f - QUAD_SHRINK * t;
  float c = cosf(QUAD_TILT * t);
  float sn = sinf(QUAD_TILT * t);
  float fw = (float)w->size.w;
  float fh = (float)w->size.h;
  int k;

  for (k = 0; k < 4; k++)
    {
      /* Turned about its middle row, its top away, seen from QUAD_FOCAL */

      float px = cu[k] * fw * s;
      float py = cv[k] * fh * s;
      float pz = QUAD_FOCAL - py * sn;

      py *= c;
      q[2 * k]     = (int16_t)lrintf(px * QUAD_FOCAL / pz - cu[k] * fw);
      q[2 * k + 1] = (int16_t)lrintf(py * QUAD_FOCAL / pz - cv[k] * fh);
    }
}

static void quad_genie(struct desk_win_s *w, float t, int16_t q[8])
{
  static const int cx[4] = { 0, 1, 1, 0 };
  static const int cy[4] = { 0, 0, 1, 1 };
  float fb = t / 0.6f;                    /* The bottom: in by 0.6 */
  float ft = t * t;                       /* The top: following */
  int ix;
  int iy;
  int k;

  if (!desk_dock_where(w->app, &ix, &iy))
    {
      ix = g_desk.dockx;
      iy = g_desk.docky;
    }

  fb = fb > 1.0f ? 1.0f : fb;
  fb = fb * fb * (3.0f - 2.0f * fb);
  for (k = 0; k < 4; k++)
    {
      float f = cy[k] ? fb : ft;
      float x0 = (float)(w->pos.x + cx[k] * w->size.w);
      float y0 = (float)(w->pos.y + cy[k] * w->size.h);
      float x1 = (float)ix + (cx[k] ? 12.0f : -12.0f);   /* The icon's middle */
      float y1 = (float)iy + (cy[k] ? 12.0f : -12.0f);

      q[2 * k]     = (int16_t)lrintf((x1 - x0) * f);
      q[2 * k + 1] = (int16_t)lrintf((y1 - y0) * f);
    }
}

/* A window's effects: the theme's shadow scaled by the setting, its edge;
 * in a transition, where its corners are
 */

static void win_effects(struct desk_win_s *w, uint8_t opacity)
{
  static const uint8_t scale[4] =
  {
    0, 60, 100, 150                          /* SHADOW_*, percent */
  };

  struct nx_effects_s fx;
  bool keep = true;
  int shadow;

  shadow = (w->active ? g_theme.shadow_active : g_theme.shadow_inactive) *
           scale[g_prefs.shadow & 3] / 100;
  shadow = shadow > 255 ? 255 : shadow;

  /* An inactive window may drop its pixels while unseen: applications
   * redraw on expose, the title bar on request (cb_title_redraw()); not
   * while the switcher shows it
   */

  memset(&fx, 0, sizeof(fx));
  fx.flags    = NX_FX_ROUND | (g_theme.darkedge ? NX_FX_DARKEDGE : 0) |
                (w->active || desk_win_app(w)->nopark || desk_fx_shows(w) ?
                 0 : NX_FX_PARK);
  fx.layer    = NX_LAYER_NORMAL;
  fx.opacity  = opacity;
  fx.shadow   = (uint8_t)(shadow * opacity / 255);
  fx.shadowdy = w->active ? 8 : 4;
  fx.border   = g_prefs.border ? (uint8_t)(g_theme.border * opacity / 255) :
                                 0;
  /* (Each effect only where Settings have it on; else only the opacity
   * changes)
   */

  if (w->fade > 0 && !w->closing)         /* Opening, restoring: t 1 to 0 */
    {
      float t = (float)w->fade / (FADE_STEPS + 1);

      if (w->restoring && desk_fx_on(FX_GENIE))
        {
          fx.flags |= NX_FX_QUAD;
          quad_genie(w, t, fx.quad);
          keep = false;
        }
      else if (!w->restoring && desk_fx_on(FX_OPEN))
        {
          fx.flags |= NX_FX_QUAD;
          quad_tilt(w, t, fx.quad);
        }
    }
  else if (w->fadeout > 0)                /* Closing, minimising: 0 to 1 */
    {
      float t = 1.0f - (float)w->fadeout / (FADE_STEPS + 1);

      if (w->minimizing && desk_fx_on(FX_GENIE))
        {
          fx.flags |= NX_FX_QUAD;
          quad_genie(w, t, fx.quad);
          keep = false;
        }
      else if (!w->minimizing && desk_fx_on(FX_OPEN))
        {
          fx.flags |= NX_FX_QUAD;
          quad_tilt(w, t, fx.quad);
        }
    }
  else if (desk_fx_quad(w, fx.quad, &keep))  /* Swinging, zooming... */
    {
      fx.flags |= NX_FX_QUAD;
    }

  /* (In perspective the shadow falls under the quadrilateral's average
   * rectangle: not for the genie's shapes)
   */

  if (!keep)
    {
      fx.shadow = 0;
    }

  nx_seteffects((NXWINDOW)w->hwnd, &fx);
}

/* Its effects again, at the opacity its state has */

void desk_win_effects(struct desk_win_s *w)
{
  if (w->fade > 0 && !w->closing)
    {
      win_effects(w, 255 - w->fade * 255 / (FADE_STEPS + 1));
    }
  else if (w->fadeout > 0)
    {
      win_effects(w, w->fadeout * 255 / (FADE_STEPS + 1));
    }
  else
    {
      win_effects(w, 255);
    }
}

/* Which traffic light is at (x, y) in the title bar, or -1 */

static int light_at(int x, int y)
{
  int i;

  for (i = 0; i < 3; i++)
    {
      int dx = x - (LIGHT_X0 + i * LIGHT_DX);
      int dy = y - LIGHT_Y;

      if (dx * dx + dy * dy <= (LIGHT_R + 3) * (LIGHT_R + 3))
        {
          return i;
        }
    }

  return -1;
}

static void win_setpos(struct desk_win_s *w, int x, int y)
{
  struct nxgl_point_s pos;

  /* Keep the title bar reachable */

  if (y < TOPBAR_H)
    {
      y = TOPBAR_H;
    }

  if (y > DESK_H - TITLE_H)
    {
      y = DESK_H - TITLE_H;
    }

  if (x > DESK_W - 60)
    {
      x = DESK_W - 60;
    }

  if (x + w->size.w < 60)
    {
      x = 60 - w->size.w;
    }

  /* The frame itself (nxtk_setposition() uses the library's possibly stale
   * idea of the window)
   */

  pos.x  = x;
  pos.y  = y;
  w->pos = pos;
  nx_setposition((NXWINDOW)w->hwnd, &pos);
}

static void win_setsize(struct desk_win_s *w, int cw, int ch)
{
  struct nxgl_size_s size;

  size.w     = cw;
  size.h     = ch;
  w->csize   = size;
  w->size.w  = cw;
  w->size.h  = ch + TITLE_H;
  nxtk_setsize(w->hwnd, &size);
}

static void win_client_redraw(struct desk_win_s *w)
{
  const struct desk_app_s *a = desk_win_app(w);

  /* The library must know the new size before drawing into it */

  desk_win_sync(w);
  if (a->resize != NULL)
    {
      a->resize(w);
    }
  else if (a->draw != NULL)
    {
      a->draw(w);
    }
}

/****************************************************************************
 * Public Functions: windows
 ****************************************************************************/

/* deskctl freeze: another task sets it (a flat build) */

static volatile time_t g_frozen;

time_t desk_time(void)
{
  time_t t = g_frozen;

  return t != 0 ? t : time(NULL);
}

bool desk_frozen(void)
{
  return g_frozen != 0;
}

void desk_freeze(time_t t)
{
  g_frozen = t;
}

uint32_t desk_seed(void)
{
  return g_frozen != 0 ? 0x2545f491u : desk_ms();
}

uint32_t desk_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* The title bar: flat, a highlight along the top, the traffic lights
 * (symbols on hover), the title
 */

void desk_win_title(struct desk_win_s *w)
{
  static struct desk_ops_s o;
  static const uint16_t lights[3] =
  {
    C_CLOSE, C_MINIMIZE, C_MAXIMIZE
  };

  static const uint16_t marks[3] =
  {
    RGB(0x70, 0x0a, 0x04), RGB(0x8a, 0x55, 0x00), RGB(0x05, 0x5a, 0x10)
  };

  static const char * const symbols[3] =
  {
    DESK_CH_TIMES, DESK_CH_MINUS, "+"
  };

  int width = w->size.w;
  int i;

  if (desk_program_hwnd(w->hwnd))
    {
      desk_program_title(w);               /* A program's: the desktop's */
      return;
    }

  ops_reset(&o);
  ops_fill(&o, 0, 0, width, TITLE_H,
           w->active ? C_TITLE_ACTIVE : C_TITLE_INACTIVE);
  ops_blend(&o, 0, 0, width, 1, C_WHITE,
            THEMED(w->active ? 40 : 20, 120));
  ops_blend(&o, 0, TITLE_H - 1, width, 1, C_SHADE, THEMED(120, 36));

  for (i = 0; i < 3; i++)
    {
      int cx = LIGHT_X0 + i * LIGHT_DX;
      uint16_t c = w->active || w->hover >= 0 ? lights[i] : C_LIGHT_OFF;

      if (w->pressed == i)
        {
          c = colour_mix(c, C_BLACK, 70);
        }

      ops_disc(&o, cx, LIGHT_Y, LUT_R6, LIGHT_R, c, 255);
      if (w->hover >= 0)
        {
          ops_text_centre(&o, &g_desk_font_uib, cx,
                          LIGHT_Y - text_height(&g_desk_font_uib) / 2 - 1,
                          symbols[i], marks[i], 230);
        }
    }

  ops_text_centre(&o, &g_desk_font_uib, width / 2,
                  (TITLE_H - text_height(&g_desk_font_uib)) / 2, w->title,
                  w->active ? C_TITLE_TEXT : C_TITLE_DIM, 255);

  /* The application's button, at the right */

  if (w->button != NULL)
    {
      int bx = BUTTON_X(w);

      if (w->buttonhover || w->buttonpressed)
        {
          ops_rrect(&o, bx, BUTTON_Y, BUTTON_W, BUTTON_H, LUT_R6, 6,
                    C_HILITE, THEMED(w->buttonpressed ? 50 : 26,
                                     w->buttonpressed ? 30 : 16));
        }

      ops_text_centre(&o, &g_desk_font_title, bx + BUTTON_W / 2,
                      BUTTON_Y + (BUTTON_H -
                                  text_height(&g_desk_font_title)) / 2,
                      w->button, w->active ? C_TITLE_TEXT : C_TITLE_DIM,
                      255);
    }

  ops_draw((NXWINDOW)w->hwnd, &o);
}

/* The GPU lost what its memory held (NX asks each window with a surface
 * to draw itself again: the atlases' callbacks say so): one EV_LOST
 */

void desk_lost(void)
{
  struct desk_event_s ev;

  if (g_lostq)
    {
      return;
    }

  g_lostq = true;
  memset(&ev, 0, sizeof(ev));
  ev.type = EV_LOST;
  if (!q_push(&ev))
    {
      g_lostq = false;
    }
}

void desk_wake(struct desk_win_s *w)
{
  bool post;

  if (desk_program_hwnd(w->hwnd))
    {
      desk_program_wake(w);                /* A program's: its own loop */
      return;
    }

  pthread_mutex_lock(&g_q.lock);
  w->wakeq = true;
  post     = !g_q.wake;
  g_q.wake = true;
  pthread_mutex_unlock(&g_q.lock);

  if (post)
    {
      sem_post(&g_q.count);
    }
}

void desk_post(uint8_t type)
{
  struct desk_event_s ev;

  memset(&ev, 0, sizeof(ev));
  ev.type = type;
  q_push(&ev);
}

struct desk_win_s *desk_win_open(int app, const char *title, int cw, int ch)
{
  struct desk_win_s *w;
  int x;
  int y;

  /* An application running as a program of its own: a window of the
   * desktop's for it, through deskwin (desk_program.c)
   */

  if (desk_program_self())
    {
      return desk_program_win_open(app, title, cw, ch);
    }

  w = calloc(1, sizeof(*w));
  if (w == NULL)
    {
      return NULL;
    }

  w->app     = app;
  w->hover   = -1;
  w->pressed = -1;
  w->pid     = -1;
  w->fade    = FADE_STEPS;
  sem_init(&w->synced, 0, 0);
  snprintf(w->title, sizeof(w->title), "%s", title);

  w->hwnd = nxtk_openwindow(g_desk.hnx,
                            NXBE_WINDOW_RAMBACKED | NXBE_WINDOW_HIDDEN,
                            &g_clientcb, w);
  if (w->hwnd == NULL)
    {
      free(w);
      return NULL;
    }

  if (nxtk_opentoolbar(w->hwnd, TITLE_H, &g_titlecb, w) < 0)
    {
      nxtk_closewindow(w->hwnd);
      free(w);
      return NULL;
    }

  if (cw > WORK_W - 40)
    {
      cw = WORK_W - 40;
    }

  if (ch + TITLE_H > WORK_H)
    {
      ch = WORK_H - TITLE_H;
    }

  win_setsize(w, cw, ch);

  /* Cascade from the top left of the work area */

  x = WORK_X + 90 + 32 * (g_cascade % 8) + 110 * (g_cascade / 8 % 3);
  y = WORK_Y + 18 + 28 * (g_cascade % 8);
  if (x + w->size.w > WORK_X + WORK_W - 20)
    {
      x = WORK_X + WORK_W - 20 - w->size.w;
    }

  if (x < WORK_X)
    {
      x = WORK_X;
    }

  if (y + w->size.h > WORK_Y + WORK_H)
    {
      y = WORK_Y + WORK_H - w->size.h;
    }

  g_cascade++;
  win_setpos(w, x, y);

  w->next    = g_desk.wins;
  g_desk.wins = w;
  win_effects(w, 0);
  return w;
}

/* Wait for the listener to catch up with the window's size and position:
 * the library clips drawing to what it knows of the client area
 */

void desk_win_sync(struct desk_win_s *w)
{
  struct timespec ts;

  if (desk_program_hwnd(w->hwnd))
    {
      desk_program_sync(w);                /* A program's */
      return;
    }

  if (nxtk_synch(w->hwnd, w) < 0)
    {
      return;
    }

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 1;
  while (sem_timedwait(&w->synced, &ts) < 0 && errno == EINTR)
    {
    }
}

/* Show a window just opened (and drawn): it fades in, active */

void desk_win_show(struct desk_win_s *w)
{
  desk_switch_cancel();                   /* (It needs the screen) */
#ifdef CONFIG_FPCDESK_SOUND
  desk_sound_play(SND_OPEN);
#endif
  desk_win_title(w);
  nxtk_setvisibility(w->hwnd, false);
  desk_win_activate(w);
}

/* g_desk.wins is in stacking order, the top first: raising moves to the head */

static void win_to_top(struct desk_win_s *w)
{
  struct desk_win_s **pp;

  for (pp = &g_desk.wins; *pp != NULL; pp = &(*pp)->next)
    {
      if (*pp == w)
        {
          *pp         = w->next;
          w->next     = g_desk.wins;
          g_desk.wins = w;
          break;
        }
    }
}

void desk_win_activate(struct desk_win_s *w)
{
  struct desk_win_s *old = g_desk.active;

  if (w != NULL && desk_program_hwnd(w->hwnd))
    {
      desk_program_raise(w);               /* A program's */
      return;
    }

  if (w != NULL && !w->minimized)
    {
      nxtk_raise(w->hwnd);
      win_to_top(w);
    }

  if (old == w)
    {
      return;
    }

  g_desk.active = w;
  if (old != NULL)
    {
      old->active = false;
      old->hover  = -1;
      if (!old->closing)
        {
          desk_win_title(old);
          win_effects(old, 255);
          if (desk_win_app(old)->focus != NULL)
            {
              desk_win_app(old)->focus(old);
            }
        }
    }

  if (w != NULL)
    {
      w->active = true;
      desk_win_title(w);
      win_effects(w, w->fade > 0 ? 255 / (w->fade + 1) : 255);
      if (desk_win_app(w)->focus != NULL)
        {
          desk_win_app(w)->focus(w);
        }
    }

  desk_topbar_draw();
  desk_dock_draw();
}

/* The next window to be active: the topmost not minimised */

static struct desk_win_s *win_next_active(struct desk_win_s *not)
{
  struct desk_win_s *w;

  for (w = g_desk.wins; w != NULL; w = w->next)
    {
      if (w != not && !w->minimized && !w->minimizing && !w->closing)
        {
          return w;
        }
    }

  return NULL;
}

void desk_win_close(struct desk_win_s *w)
{
  if (desk_program_hwnd(w->hwnd))
    {
      desk_program_win_close(w);            /* (A program's own) */
      return;
    }

  if (w->closing)
    {
      return;
    }

  w->closing = true;
  desk_switch_forget(w);
#ifdef CONFIG_FPCDESK_SOUND
  desk_sound_play(SND_CLOSE);
#endif
  if (desk_win_app(w)->close != NULL)
    {
      desk_win_app(w)->close(w);
    }

  if (g_desk.active == w)
    {
      g_desk.active = NULL;
      desk_win_activate(win_next_active(w));
    }

  if (g_desk.drag == w)
    {
      g_desk.drag = NULL;
    }

  if (g_desk.resize == w)
    {
      g_desk.resize = NULL;
    }

  /* It fades out, then goes (animate()) */

  w->fade    = 0;
  w->fadeout = FADE_STEPS;
}

static void win_destroy(struct desk_win_s *w)
{
  struct desk_win_s **pp;

  for (pp = &g_desk.wins; *pp != NULL; pp = &(*pp)->next)
    {
      if (*pp == w)
        {
          *pp = w->next;
          break;
        }
    }

  if (g_desk.pointerwin == w)
    {
      g_desk.pointerwin = NULL;
    }

  desk_switch_forget(w);
  nxtk_closewindow(w->hwnd);
  sem_destroy(&w->synced);
  free(w->priv);
  free(w);
  desk_dock_draw();
  desk_topbar_draw();
}

void desk_win_minimize(struct desk_win_s *w)
{
  if (w->minimized || w->minimizing)
    {
      return;
    }

  /* It fades out, then hides (animate()) */

#ifdef CONFIG_FPCDESK_SOUND
  desk_sound_play(SND_MINIMIZE);
#endif
  w->minimizing = true;
  w->restoring  = false;
  w->fade       = 0;
  w->fadeout    = FADE_STEPS;
  if (g_desk.active == w)
    {
      w->active = false;
      g_desk.active = NULL;
      desk_win_activate(win_next_active(w));
    }

  desk_dock_draw();
}

void desk_win_restore(struct desk_win_s *w)
{
  if (desk_program_hwnd(w->hwnd))
    {
      desk_program_raise(w);               /* A program's */
      return;
    }

  if (w->minimizing)
    {
      w->minimizing = false;
      w->restoring  = true;
      w->fadeout    = 0;
      w->fade       = FADE_STEPS;
    }

  if (w->minimized)
    {
      w->minimized = false;
      w->restoring = true;
      w->fade      = FADE_STEPS;
      win_effects(w, 0);
      nxtk_setvisibility(w->hwnd, false);
    }

  desk_win_activate(w);
}

/* A maximised window: the display between the top bar and the dock */

static void win_setmax(struct desk_win_s *w)
{
  win_setpos(w, WORK_X, TOPBAR_H);
  win_setsize(w, WORK_W, WORK_Y + WORK_H - TOPBAR_H - TITLE_H);
}

void desk_win_maximize(struct desk_win_s *w)
{
  struct nxgl_rect_s from;

  from.pt1.x = w->pos.x;                  /* (Where it zooms from) */
  from.pt1.y = w->pos.y;
  from.pt2.x = w->pos.x + w->size.w;
  from.pt2.y = w->pos.y + w->size.h;
  if (!w->maximized)
    {
      w->restore.pt1   = w->pos;
      w->restore.pt2.x = w->csize.w;
      w->restore.pt2.y = w->csize.h;
      w->maximized     = true;
      win_setmax(w);
    }
  else
    {
      w->maximized = false;
      win_setsize(w, w->restore.pt2.x, w->restore.pt2.y);
      win_setpos(w, w->restore.pt1.x, w->restore.pt1.y);
    }

  desk_win_title(w);
  win_client_redraw(w);
  desk_fx_zoom(w, &from);
}

struct desk_win_s *desk_app_window(int app)
{
  struct desk_win_s *w;

  for (w = g_desk.wins; w != NULL; w = w->next)
    {
      if (w->app == app && !w->closing)
        {
          return w;
        }
    }

  return NULL;
}

int desk_open_post(int app, const char *path)
{
  struct desk_event_s ev;

  memset(&ev, 0, sizeof(ev));
  ev.type = EV_LAUNCH;
  ev.part = app;
  ev.arg  = path != NULL ? strdup(path) : NULL;
  if (path != NULL && ev.arg == NULL)
    {
      return -ENOMEM;
    }

  if (!q_push(&ev))
    {
      free(ev.arg);
      return -EAGAIN;
    }

  return 0;
}

void desk_launch(int app)
{
  const struct desk_app_s *a = g_apps[app];
  struct desk_win_s *w;

  /* From a program: the desktop's main thread does it */

  if (desk_program_self())
    {
      struct desk_event_s ev;

      memset(&ev, 0, sizeof(ev));
      ev.type = EV_LAUNCH;
      ev.part = app;
      q_push(&ev);
      return;
    }

  if (app == APP_LAUNCHER)
    {
      desk_launcher_toggle();
      return;
    }

  if (g_desk.launcher != NULL)
    {
      desk_launcher_toggle();
    }

  if (a->single && (w = desk_app_window(app)) != NULL)
    {
      desk_win_restore(w);
      return;
    }

  if (a->program != NULL)
    {
      desk_program_start(app);              /* (Its window comes later) */
    }
  else if (a->open != NULL && (w = a->open()) != NULL)
    {
      desk_win_show(w);
    }
}

/* A new display size: the panels follow, maximised windows refill it, the
 * others shrink to stay on it
 */

void desk_relayout(void)
{
  struct desk_win_s *w;
  int cw;
  int ch;

  desk_layout();
  desk_panels_relayout();

  for (w = g_desk.wins; w != NULL; w = w->next)
    {
      if (w->closing)
        {
          continue;
        }

      if (w->maximized)
        {
          win_setmax(w);
        }
      else
        {
          cw = w->csize.w > WORK_W - 40 ? WORK_W - 40 : w->csize.w;
          ch = w->csize.h + TITLE_H > WORK_H ? WORK_H - TITLE_H :
                                               w->csize.h;
          if (cw != w->csize.w || ch != w->csize.h)
            {
              win_setsize(w, cw, ch);
            }
          else
            {
              cw = -1;
            }

          win_setpos(w,
                     w->pos.x + w->size.w > WORK_X + WORK_W ?
                     WORK_X + WORK_W - w->size.w :
                     w->pos.x < WORK_X ? WORK_X : w->pos.x,
                     w->pos.y + w->size.h > WORK_Y + WORK_H ?
                     WORK_Y + WORK_H - w->size.h : w->pos.y);
          if (cw < 0)
            {
              continue;
            }
        }

      desk_win_title(w);
      win_client_redraw(w);
    }

  /* Applications that show the mode (Settings, About) */

  for (w = g_desk.wins; w != NULL; w = w->next)
    {
      if (!w->closing &&
          (w->app == APP_SETTINGS || w->app == APP_ABOUT) &&
          desk_win_app(w)->draw != NULL)
        {
          desk_win_app(w)->draw(w);
        }
    }
}

static void on_mode(void)
{
  struct nxgl_size_s size;
  struct desk_win_s *w;

  if (nx_displaysize(g_desk.hnx, &size) < 0)
    {
      return;
    }

  /* Another mode of the same size (1920 x 1080 at 30 Hz, and at 60 Hz in
   * 4:2:0): only what shows the mode
   */

  if (size.w == g_desk.w && size.h == g_desk.h)
    {
      for (w = g_desk.wins; w != NULL; w = w->next)
        {
          if (!w->closing &&
              (w->app == APP_SETTINGS || w->app == APP_ABOUT) &&
              desk_win_app(w)->draw != NULL)
            {
              desk_win_app(w)->draw(w);
            }
        }

      return;
    }

  g_desk.w = size.w;
  g_desk.h = size.h;
  desk_switch_cancel();
  desk_relayout();
}

/* A new theme, accent or window effects: redraw the panels, title bars,
 * effects and every window's contents
 */

void desk_theme_changed(void)
{
  struct desk_win_s *w;
  nxgl_mxpixel_t bg = g_theme.desktop;

  nx_setbgcolor(g_desk.hnx, &bg);
  desk_panels_relayout();

  for (w = g_desk.wins; w != NULL; w = w->next)
    {
      const struct desk_app_s *a = desk_win_app(w);

      if (w->closing)
        {
          continue;
        }

      desk_win_title(w);
      if (w->fade == 0 && w->fadeout == 0 && !w->minimized)
        {
          win_effects(w, 255);
        }

      if (a->theme != NULL)
        {
          a->theme(w);
        }
      else if (a->expose != NULL)
        {
          a->expose(w, 0, 0, w->csize.w, w->csize.h);
        }
      else if (a->draw != NULL)
        {
          a->draw(w);
        }
    }
}

/* The time zone for the programs the desktop starts (the Terminal's
 * shells: date, ls): TZ, as a POSIX offset (with CONFIG_LIBC_LOCALTIME;
 * the desktop's own clocks add g_prefs.tzoff themselves)
 */

void desk_tz_apply(void)
{
#ifdef CONFIG_LIBC_LOCALTIME
  int off = g_prefs.tzoff < 0 ? -g_prefs.tzoff : g_prefs.tzoff;
  char tz[24];

  snprintf(tz, sizeof(tz), "UTC%c%d:%02d", g_prefs.tzoff > 0 ? '-' : '+',
           off / 60, off % 60);
  setenv("TZ", tz, 1);
  tzset();
#endif
}

void desk_localtime(struct tm *tm)
{
  time_t now = desk_time() + (time_t)g_prefs.tzoff * 60;

  gmtime_r(&now, tm);
}

/* Quit an application: close all its windows (asking where they would) */

void desk_app_quit(int app)
{
  struct desk_win_s *w;
  struct desk_win_s *next;

  for (w = g_desk.wins; w != NULL; w = next)
    {
      next = w->next;
      if (w->app == app && !w->closing)
        {
          if (g_apps[app]->canclose == NULL || g_apps[app]->canclose(w))
            {
              desk_win_close(w);
            }
          else
            {
              desk_win_restore(w);
            }
        }
    }
}

/****************************************************************************
 * Private Functions: events
 ****************************************************************************/

/* The pointer over, pressing or dragging an application window: pos is in
 * frame coordinates (while a button is held, relative to the frame at the
 * press)
 */

static void win_mouse(struct desk_win_s *w, int part, int x, int y,
                      uint8_t buttons)
{
  bool press   = (buttons & NX_MOUSE_LEFTBUTTON) != 0 &&
                 (g_desk.buttons & NX_MOUSE_LEFTBUTTON) == 0;
  bool release = (buttons & NX_MOUSE_LEFTBUTTON) == 0 &&
                 (g_desk.buttons & NX_MOUSE_LEFTBUTTON) != 0;
  bool button;
  int light;

  /* Dragging the window, or its size */

  if (g_desk.drag == w)
    {
      win_setpos(w, g_desk.grabpos.x + x - g_desk.grab.x,
                 g_desk.grabpos.y + y - g_desk.grab.y);
      desk_fx_drag(w, FX_DRAG_MOVE);
      if (release)
        {
          g_desk.drag = NULL;
          desk_fx_drag(w, FX_DRAG_END);
        }

      return;
    }

  if (g_desk.resize == w)
    {
      int cw = g_desk.grabsize.w + x - g_desk.grab.x;
      int ch = g_desk.grabsize.h + y - g_desk.grab.y;

      cw = cw < MIN_W ? MIN_W : cw;
      ch = ch < MIN_H ? MIN_H : ch;
      if (cw != w->csize.w || ch != w->csize.h || release)
        {
          win_setsize(w, cw, ch);
          desk_win_title(w);
          if (release)
            {
              win_client_redraw(w);
            }
        }

      if (release)
        {
          g_desk.resize = NULL;
        }

      return;
    }

  /* The traffic lights light up with their symbols under the pointer */

  light = part == PART_TITLE ? light_at(x, y) : -1;
  if ((light >= 0) != (w->hover >= 0))
    {
      w->hover = light;
      desk_win_title(w);
    }

  /* The application's button: pressed, then released over it */

  button = w->button != NULL && part == PART_TITLE &&
           x >= BUTTON_X(w) && x < BUTTON_X(w) + BUTTON_W &&
           y >= BUTTON_Y && y < BUTTON_Y + BUTTON_H;
  if (button != w->buttonhover)
    {
      w->buttonhover = button;
      desk_win_title(w);
    }

  if (w->buttonpressed && (press || release))
    {
      w->buttonpressed = false;
      desk_win_title(w);
      if (release && button && desk_win_app(w)->button != NULL)
        {
          desk_win_app(w)->button(w);
        }

      return;
    }

  if (press && button)
    {
      if (w != g_desk.active)
        {
          desk_win_activate(w);
        }

      w->buttonpressed = true;
      desk_win_title(w);
      return;
    }

  if (press)
    {
      if (w != g_desk.active)
        {
          desk_win_activate(w);
        }

      if (light >= 0)
        {
          w->pressed = light;
          desk_win_title(w);
        }
      else if (part == PART_TITLE)
        {
          uint32_t now = desk_ms();

          if (g_desk.clickwin == w && now - g_desk.clickms < DOUBLECLICK_MS)
            {
              g_desk.clickwin = NULL;
              desk_win_maximize(w);
              return;
            }

          g_desk.clickwin = w;
          g_desk.clickms  = now;
          g_desk.drag     = w;
          g_desk.grab.x   = x;
          g_desk.grab.y   = y;
          g_desk.grabpos  = w->pos;
          desk_fx_drag(w, FX_DRAG_START);
        }
      else if (x >= w->size.w - RESIZE_GRIP && y >= w->size.h - RESIZE_GRIP)
        {
          g_desk.resize   = w;
          g_desk.grab.x   = x;
          g_desk.grab.y   = y;
          g_desk.grabsize = w->csize;
        }
    }

  /* The rest of the client area's events are the application's */

  if (part == PART_CLIENT && g_desk.resize != w &&
      desk_win_app(w)->mouse != NULL)
    {
      desk_win_app(w)->mouse(w, x, y - TITLE_H, buttons);
    }

  if (release && w->pressed >= 0)
    {
      int which = w->pressed;

      w->pressed = -1;
      if (light == which)
        {
          switch (which)
            {
              case 0:
                if (desk_win_app(w)->canclose == NULL ||
                    desk_win_app(w)->canclose(w))
                  {
                    desk_win_close(w);
                  }
                else
                  {
                    desk_win_title(w);
                  }
                return;

              case 1:
                desk_win_minimize(w);
                return;

              default:
                desk_win_maximize(w);
                return;
            }
        }

      desk_win_title(w);
    }
}

static void on_mouse(struct desk_event_s *ev)
{
  struct desk_win_s *w = ev->win;

  /* The switcher, open, takes the pointer over windows (where they really
   * are is not where it shows them) and its own; a press on the panels
   * closes it, and goes on to them
   */

  if (desk_switch_open())
    {
      bool press = (ev->buttons & ~g_desk.buttons) != 0;
      int x = ev->pos.x;
      int y = ev->pos.y;
      bool mine = true;

      if (ev->part == PART_TITLE || ev->part == PART_CLIENT)
        {
          x += w->pos.x;
          y += w->pos.y + (ev->part == PART_CLIENT ? TITLE_H : 0);
        }
      else if (ev->part != PART_SWITCH)
        {
          mine = false;
        }

      if (mine)
        {
          desk_switch_mouse(x, y, press);
          g_desk.buttons = ev->buttons;
          return;
        }

      if (press)
        {
          desk_switch_cancel();
        }
    }

  /* A press anywhere but on it closes a menu */

  if (g_desk.menu != NULL && ev->part != PART_MENU &&
      (ev->buttons & ~g_desk.buttons) != 0)
    {
      desk_menu_close();
      if (ev->part != PART_DOCK && ev->part != PART_LAUNCHER)
        {
          g_desk.buttons = ev->buttons;         /* Only that */
          return;
        }
    }

  desk_panels_mouse(ev->part, ev->pos.x, ev->pos.y, ev->buttons);
#ifdef CONFIG_FPCDESK_SOUND
  desk_volume_mouse_any(ev->part, ev->buttons);
#endif
  desk_clock_mouse_any(ev->part, ev->buttons);

  switch (ev->part)
    {
      case PART_NOTIFY:
        desk_notify_mouse(ev->pos.x, ev->pos.y, ev->buttons);
        break;

      case PART_CALPOP:
        desk_calpop_mouse(ev->pos.x, ev->pos.y, ev->buttons);
        break;

#ifdef CONFIG_FPCDESK_SOUND
      case PART_SOUND:
        desk_volume_popover_mouse(ev->pos.x, ev->pos.y, ev->buttons);
        break;
#endif

      case PART_TITLE:
        if (!w->closing)
          {
            win_mouse(w, PART_TITLE, ev->pos.x, ev->pos.y, ev->buttons);
          }
        break;

      case PART_CLIENT:
        if (!w->closing)
          {
            win_mouse(w, PART_CLIENT, ev->pos.x, ev->pos.y + TITLE_H,
                      ev->buttons);
          }
        break;

      case PART_DOCK:
        desk_dock_mouse(ev->pos.x, ev->pos.y, ev->buttons);
        break;

      case PART_TOPBAR:
        desk_topbar_mouse(ev->pos.x, ev->pos.y, ev->buttons);
        break;

      case PART_LAUNCHER:
        desk_launcher_mouse(ev->pos.x, ev->pos.y, ev->buttons);
        break;

      case PART_MENU:
        desk_menu_mouse(ev->pos.x, ev->pos.y, ev->buttons);
        break;

      default:
        break;
    }

  /* Leaving an application's client area: it hears (-1, -1) */

  if (g_desk.pointerwin != NULL && ev->buttons == 0 &&
      (ev->part != PART_CLIENT || ev->win != g_desk.pointerwin))
    {
      w = g_desk.pointerwin;
      g_desk.pointerwin = NULL;
      if (!w->closing && desk_win_app(w)->mouse != NULL)
        {
          desk_win_app(w)->mouse(w, -1, -1, 0);
        }
    }

  if (ev->part == PART_CLIENT)
    {
      g_desk.pointerwin = ev->win;
    }

  /* Leaving the dock, the top bar or a title bar: their hover goes */

  if (ev->part != PART_DOCK && g_desk.dockhover >= 0 && ev->buttons == 0)
    {
      desk_dock_mouse(-1, -1, 0);
    }

  if (ev->part != PART_TOPBAR && g_desk.topbarhover)
    {
      desk_topbar_mouse(-1, -1, 0);
    }

  if (ev->part != PART_TITLE)
    {
      struct desk_win_s *t;

      for (t = g_desk.wins; t != NULL; t = t->next)
        {
          if ((t->hover >= 0 || t->buttonhover) && !t->closing &&
              !t->buttonpressed)
            {
              t->hover       = -1;
              t->buttonhover = false;
              desk_win_title(t);
            }
        }
    }

  g_desk.buttons = ev->buttons;
}

/* Keys go to an open menu (Escape closes it), else the launcher's search,
 * else the active window: key events (nxkeys.h) if it takes them, else the
 * bytes they type.  Plain bytes (deskctl k) pass as they are.
 */

static void kbd_bytes(struct desk_win_s *w, const uint8_t *ch, int n)
{
  if (n <= 0)
    {
      return;
    }

  if (g_desk.menu != NULL && n == 1 && ch[0] == 0x1b)
    {
      desk_menu_close();
    }
  else if (g_desk.launcher != NULL)
    {
      desk_launcher_key(ch, n);
    }
  else if (w != NULL && !w->closing && desk_win_app(w)->key != NULL)
    {
      desk_win_app(w)->key(w, ch, n);
    }
}

static void on_kbd(struct desk_event_s *ev)
{
  struct desk_win_s *w = g_desk.active;
  const uint8_t *ch = ev->ch;
  uint8_t bytes[sizeof(ev->ch) + NX_KEY_MAXBYTES];
  uint8_t seq[NX_KEY_MAXBYTES];
  struct nx_key_s key;
  uint8_t byte;
  int left = ev->nch;
  int nb = 0;
  int item;

  while ((item = nx_key_next(&ch, &left, &key, &byte)) != NX_KEYITEM_NONE)
    {
      if (item == NX_KEYITEM_BYTE)
        {
          bytes[nb++] = byte;
          continue;
        }

      /* A key event: the bytes before it first */

      kbd_bytes(w, bytes, nb);
      nb = 0;

      /* Ctrl-Alt-Backspace: a display mode the monitor does not show
       * undone (desk_mode_rescue())
       */

      if (key.code == NX_KEY_BACKSPACE &&
          (key.flags & (NX_KEY_RELEASE | NX_KEY_CTRL | NX_KEY_ALT)) ==
          (NX_KEY_CTRL | NX_KEY_ALT))
        {
          desk_mode_rescue();
          continue;
        }

      /* Alt-Tab: the switcher (and while it is open, every key).  An
       * application that had the modifier's press hears it let go now
       */

      if (!desk_switch_open() && desk_switch_key(&key))
        {
          if (w != NULL && !w->closing && desk_win_app(w)->keyev != NULL)
            {
              struct nx_key_s up;

              up.flags = NX_KEY_RELEASE |
                         (key.flags & ~(NX_KEY_ALT | NX_KEY_GUI) &
                          NX_KEY_MODS);
              up.code  = NX_KEY_SPECIAL((key.flags & NX_KEY_ALT) ?
                                        KEYCODE_LALT : KEYCODE_LGUI);
              up.ch    = 0;
              desk_win_app(w)->keyev(w, &up);
            }

          continue;
        }

      if (desk_switch_open() && desk_switch_key(&key))
        {
          w = g_desk.active;
          continue;
        }

#ifdef CONFIG_FPCDESK_SOUND
          /* Volume keys from elsewhere (deskctl key vol+); the session
           * handles a keyboard's own
           */

      if (desk_sound_key(&key))
        {
          continue;
        }
#endif

      if (g_desk.menu == NULL && g_desk.launcher == NULL && w != NULL &&
          !w->closing && desk_win_app(w)->keyev != NULL)
        {
          desk_win_app(w)->keyev(w, &key);
        }
      else
        {
          /* One call a key: an Escape on its own is the key (ui_keys()) */

          kbd_bytes(w, seq, nx_key_bytes(&key, seq));
        }
    }

  kbd_bytes(w, bytes, nb);
}

/* EV_LAUNCH with a file: the application's own open (desk.h) */

static void app_open(int app, const char *path)
{
  switch (app)
    {
      case APP_EDITOR:
        desk_editor_open(path);
        break;

      case APP_SHEET:
        desk_sheet_open(path);
        break;

      case APP_VIEWER:
        desk_viewer_open(path);
        break;

      case APP_PAINT:
        desk_paint_open(path);
        break;

      default:
        desk_launch(app);
        break;
    }
}

static void on_event(struct desk_event_s *ev)
{
  switch (ev->type)
    {
      case EV_MOUSE:
        on_mouse(ev);
        break;

      case EV_KBD:
        on_kbd(ev);
        break;

      case EV_BLOCKED:
        win_destroy(ev->win);
        break;

      case EV_MODE:
        on_mode();
        break;

#ifdef CONFIG_FPCDESK_SOUND
      case EV_SOUND:
        desk_sound_changed();
        break;
#endif

#ifdef CONFIG_FPCDESK_CLIENTS
      case EV_CLIENT:
        desk_clients_poll();
        break;
#endif

      case EV_LOST:                       /* The atlases, then everything */
        g_lostq = false;
        desk_atlas_redraw();
        desk_icons_redraw();
        desk_theme_changed();
        break;

#ifdef CONFIG_NETUTILS_NETCOP
      case EV_NET:
        desk_net_poll();
        break;

#endif
      case EV_LAUNCH:
        if (ev->part < APP_COUNT && ev->arg != NULL)
          {
            app_open(ev->part, ev->arg);
          }
        else if (ev->part < APP_COUNT)
          {
            desk_launch(ev->part);
          }

        free(ev->arg);
        break;

      case EV_REDRAW:
        if (!ev->win->closing && ev->part == PART_TITLE)
          {
            desk_win_title(ev->win);
          }
        else if (!ev->win->closing)
          {
            const struct desk_app_s *a = desk_win_app(ev->win);   /* (a program's: the client's) */

            if (a->expose != NULL)
              {
                a->expose(ev->win, ev->pos.x, ev->pos.y, ev->size.w,
                          ev->size.h);
              }
            else if (a->draw != NULL && g_desk.resize != ev->win)
              {
                a->draw(ev->win);
              }
          }
        break;

      default:
        break;
    }
}

/* Animation frames: windows fading in */

static bool animate(void)
{
  struct desk_win_s *w;
  int stop = desk_fx_fadestop(FADE_STEPS);   /* Held: they stop there */
  bool more = false;

  for (w = g_desk.wins; w != NULL; w = w->next)
    {
      if ((w->fade > 0 && !w->closing && w->fade <= stop) ||
          (w->fadeout > 0 && w->fadeout <= stop))
        {
          more = true;                       /* Held (deskctl fx hold) */
        }
      else if (w->fade > 0 && !w->closing)
        {
          w->fade--;
          w->restoring &= w->fade > 0;
          win_effects(w, 255 - w->fade * 255 / (FADE_STEPS + 1));
          more |= w->fade > 0;
        }
      else if (w->fadeout > 0)
        {
          w->fadeout--;
          win_effects(w, w->fadeout * 255 / (FADE_STEPS + 1));
          more = true;
          if (w->fadeout == 0 && w->closing && !w->closedone)
            {
              /* Stop the server's events for it; it goes on EV_BLOCKED */

              w->closedone = true;
              nxtk_block(w->hwnd, w);
            }
          else if (w->fadeout == 0 && w->minimizing)
            {
              w->minimizing = false;
              w->minimized  = true;
              nxtk_setvisibility(w->hwnd, true);
              win_effects(w, 255);
              desk_dock_draw();
            }
        }
    }

  more |= desk_fx_animate();
  more |= desk_panels_animate();
#ifdef CONFIG_FPCDESK_SOUND
  more |= desk_volume_animate();
#endif
  return more;
}

/* Application frames for the windows that asked (w->frames); returns
 * whether any did
 */

static bool frames(void)
{
  struct desk_win_s *w;
  struct desk_win_s *next;
  bool more = false;

  for (w = g_desk.wins; w != NULL; w = next)
    {
      next = w->next;
      if (w->frames && !w->closing && !w->minimized &&
          desk_win_app(w)->frame != NULL)
        {
          desk_win_app(w)->frame(w);
          more |= w->frames;
        }
    }

  return more;
}

static void tick(void)
{
  struct desk_win_s *w;
  struct desk_win_s *next;

  desk_load_sample();
  desk_panels_tick();
  desk_clock_tick();
  desk_mode_tick();                    /* A display mode on trial */
  for (w = g_desk.wins; w != NULL; w = next)
    {
      next = w->next;
      if (w->closing)
        {
          continue;
        }

      /* A window whose task (a shell) has exited closes */

      if (w->pid >= 0 && kill(w->pid, 0) < 0)
        {
          w->pid = -1;
          desk_win_close(w);
          continue;
        }

      if (desk_win_app(w)->tick != NULL && !w->minimized)
        {
          desk_win_app(w)->tick(w);
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct desk_event_s ev;
  pthread_attr_t attr;
  pthread_t thread;
  uint32_t nexttick;
  uint32_t nextframe;
  bool anim = true;
  bool framing = false;
  int caps;
  int ret;

  memset(&g_desk, 0, sizeof(g_desk));
  g_desk.tid       = gettid();
  g_desk.dockhover = -1;
  g_desk.dockout   = -1;                  /* Not placed yet */
  pthread_mutex_init(&g_q.lock, NULL);
  sem_init(&g_q.count, 0, 0);

  /* The NX server, then our connection to it */

  ret = boardctl(BOARDIOC_NX_START, 0);
  if (ret < 0)
    {
      fprintf(stderr, "desk: cannot start the NX server: %d\n", errno);
      return EXIT_FAILURE;
    }

  g_desk.hnx = nx_connect();
  if (g_desk.hnx == NULL)
    {
      fprintf(stderr, "desk: nx_connect: %d\n", errno);
      return EXIT_FAILURE;
    }

  /* The CONNECTED event, then the listener takes over */

  if (nx_eventhandler(g_desk.hnx) < 0)
    {
      fprintf(stderr, "desk: not connected: %d\n", errno);
      return EXIT_FAILURE;
    }

  /* Key events, not only bytes: games want presses and releases */

  nx_kbdmode(g_desk.hnx, NX_KBDMODE_KEYS);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FPCDESK_LISTENER_STACKSIZE);
  pthread_create(&thread, &attr, listener, NULL);

  caps = nx_compositor(g_desk.hnx);

  /* The display mode saved last time (the panels are made at the new size) */

  ret = desk_prefs_load();
  desk_tz_apply();
  if (ret >= 0 && ret != nx_getdisplaymode(g_desk.hnx, -1, NULL))
    {
      nx_setdisplaymode(g_desk.hnx, ret);
    }

  {
    struct nxgl_size_s size;

    if (nx_displaysize(g_desk.hnx, &size) < 0)
      {
        size.w = 1024;
        size.h = 768;
      }

    g_desk.w = size.w;
    g_desk.h = size.h;
  }

  g_desk.composited = (caps & NX_CAP_COMPOSITOR) != 0;
  g_desk.quad       = (caps & NX_CAP_QUAD) != 0;
  if (!g_desk.composited)
    {
      fprintf(stderr, "desk: the display is not composited (not a v5 GPU: "
                      "see the boot log): the desktop will look plain\n");
    }

  desk_theme_set(g_prefs.theme, g_prefs.accent);
  {
    nxgl_mxpixel_t bg = g_theme.desktop;

    nx_setbgcolor(g_desk.hnx, &bg);
  }

  desk_input_apply();
  desk_layout();
  desk_luts_init();
  if (desk_atlas_init() < 0)
    {
      fprintf(stderr, "desk: no glyph atlas\n");
    }

  desk_panels_init();
#ifdef CONFIG_FPCDESK_SOUND
  if (desk_sound_init() == 0)
    {
      desk_topbar_draw();
    }
#endif

#ifdef CONFIG_NETUTILS_NETCOP
  desk_net_start();                       /* The WiFi indicator, Settings */
#endif

  desk_clock_init();                      /* Alarms ring from now on */
#ifdef CONFIG_FPCDESK_CLIENTS
  desk_clients_init();
#endif

  /* Start with a terminal */

  desk_launch(APP_TERMINAL);

  nexttick  = desk_ms() + 1000;
  nextframe = desk_ms();
  for (; ; )
    {
      int32_t wait = anim ? FRAME_MS : (int32_t)(nexttick - desk_ms());

      if (framing && (int32_t)(nextframe - desk_ms()) < wait)
        {
          wait = (int32_t)(nextframe - desk_ms());
        }

      if (wait < 1)
        {
          wait = 1;
        }

      if (q_pop(&ev, wait))
        {
          on_event(&ev);
          while (q_pop(&ev, 0))
            {
              on_event(&ev);
            }
        }

      q_wakes();

      anim = animate();

      /* Application frames, at most one per FRAME_MS */

      if ((int32_t)(desk_ms() - nextframe) >= 0)
        {
          nextframe += FRAME_MS;
          if ((int32_t)(desk_ms() - nextframe) >= 0)
            {
              nextframe = desk_ms() + FRAME_MS;
            }

          framing = frames();
        }
      else
        {
          struct desk_win_s *w;

          for (w = g_desk.wins; w != NULL && !framing; w = w->next)
            {
              framing = w->frames && !w->closing && !w->minimized;
            }
        }

      if ((int32_t)(desk_ms() - nexttick) >= 0)
        {
          nexttick += 1000;
          if ((int32_t)(desk_ms() - nexttick) > 0)
            {
              nexttick = desk_ms() + 1000;
            }

          tick();
        }
    }

  return EXIT_SUCCESS;
}
