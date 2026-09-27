/****************************************************************************
 * apps/graphics/fpcdesk/desk_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The FPC desktop: a shell for the NX server's compositor
 * (CONFIG_NX_COMPOSITOR).  It starts the NX server, then makes the
 * desktop out of NX windows the server composites:
 *
 *   - the wallpaper: a window in the desktop layer, drawn once;
 *   - the top bar and the dock: layer windows (no pixels, a display list
 *     the compositor replays over what is below them: translucent);
 *   - application windows: framed windows (nxtk) whose toolbar is the
 *     title bar, with round corners and a soft shadow, the active one's
 *     deeper.
 *
 * NX calls its callbacks on the listener thread; they only queue events.
 * Everything else happens on the main thread: the events, a tick once a
 * second (the clock, the monitor), animation frames.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
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

#include <nuttx/nx/nxbe.h>

#include "desk.h"
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

/****************************************************************************
 * Private Functions: the event queue
 ****************************************************************************/

static void q_push(const struct desk_event_s *ev)
{
  bool post = true;

  pthread_mutex_lock(&g_q.lock);

  /* Pointer movement: only the latest counts, as long as the buttons are
   * the same
   */

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

      if (due && !w->closing && g_apps[w->app]->wake != NULL)
        {
          g_apps[w->app]->wake(w);
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
      ev.nch = nch > sizeof(ev.ch) ? sizeof(ev.ch) : nch;
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

/* NX's messages.  An error is said (on the syslog: the desktop has no
 * console) and survived, unless the server has gone or it goes on.
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

static void win_effects(struct desk_win_s *w, uint8_t opacity)
{
  struct nx_effects_s fx;

  memset(&fx, 0, sizeof(fx));
  fx.flags    = NX_FX_ROUND;
  fx.layer    = NX_LAYER_NORMAL;
  fx.opacity  = opacity;
  fx.shadow   = (w->active ? 170 : 100) * opacity / 255;
  fx.shadowdy = w->active ? 8 : 4;
  fx.border   = (uint8_t)(34 * opacity / 255);
  nx_seteffects((NXWINDOW)w->hwnd, &fx);
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

  /* The frame itself (nxtk_setposition() works from what the library last
   * heard of the window, which may not be the latest)
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
  const struct desk_app_s *a = g_apps[w->app];

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

uint32_t desk_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* The title bar: flat, a hairline of light along the top, the traffic
 * lights (with their symbols while the pointer is over them), the title
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

  ops_reset(&o);
  ops_fill(&o, 0, 0, width, TITLE_H,
           w->active ? C_TITLE_ACTIVE : C_TITLE_INACTIVE);
  ops_blend(&o, 0, 0, width, 1, C_WHITE, w->active ? 40 : 20);
  ops_blend(&o, 0, TITLE_H - 1, width, 1, C_BLACK, 120);

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
                    C_WHITE, w->buttonpressed ? 50 : 26);
        }

      ops_text_centre(&o, &g_desk_font_title, bx + BUTTON_W / 2,
                      BUTTON_Y + (BUTTON_H -
                                  text_height(&g_desk_font_title)) / 2,
                      w->button, w->active ? C_TITLE_TEXT : C_TITLE_DIM,
                      255);
    }

  ops_draw((NXWINDOW)w->hwnd, &o);
}

void desk_wake(struct desk_win_s *w)
{
  bool post;

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

  if (cw > DESK_W - 40)
    {
      cw = DESK_W - 40;
    }

  if (ch + TITLE_H > WORK_H)
    {
      ch = WORK_H - TITLE_H;
    }

  win_setsize(w, cw, ch);

  /* Cascade from the top left of the work area */

  x = 90 + 32 * (g_cascade % 8) + 110 * (g_cascade / 8 % 3);
  y = WORK_Y + 18 + 28 * (g_cascade % 8);
  if (x + w->size.w > DESK_W - 20)
    {
      x = DESK_W - 20 - w->size.w;
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

/* Wait until the listener has seen everything the server has said about
 * the window so far (its size, position): the library clips drawing to
 * what it knows of the client area
 */

void desk_win_sync(struct desk_win_s *w)
{
  struct timespec ts;

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

static void win_show(struct desk_win_s *w)
{
#ifdef CONFIG_FPCDESK_SOUND
  desk_sound_play(SND_OPEN);
#endif
  desk_win_title(w);
  nxtk_setvisibility(w->hwnd, false);
  desk_win_activate(w);
}

void desk_win_activate(struct desk_win_s *w)
{
  struct desk_win_s *old = g_desk.active;

  if (w != NULL && !w->minimized)
    {
      nxtk_raise(w->hwnd);
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
          if (g_apps[old->app]->focus != NULL)
            {
              g_apps[old->app]->focus(old);
            }
        }
    }

  if (w != NULL)
    {
      w->active = true;
      desk_win_title(w);
      win_effects(w, w->fade > 0 ? 255 / (w->fade + 1) : 255);
      if (g_apps[w->app]->focus != NULL)
        {
          g_apps[w->app]->focus(w);
        }
    }

  desk_topbar_draw();
  desk_dock_draw();
}

/* The next window to be active: the top one not minimised */

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
  if (w->closing)
    {
      return;
    }

  w->closing = true;
#ifdef CONFIG_FPCDESK_SOUND
  desk_sound_play(SND_CLOSE);
#endif
  if (g_apps[w->app]->close != NULL)
    {
      g_apps[w->app]->close(w);
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
  if (w->minimizing)
    {
      w->minimizing = false;
      w->fadeout    = 0;
      w->fade       = FADE_STEPS;
    }

  if (w->minimized)
    {
      w->minimized = false;
      w->fade      = FADE_STEPS;
      win_effects(w, 0);
      nxtk_setvisibility(w->hwnd, false);
    }

  desk_win_activate(w);
}

/* A maximised window: all of the display between the top bar and the
 * dock
 */

static void win_setmax(struct desk_win_s *w)
{
  win_setpos(w, 0, TOPBAR_H);
  win_setsize(w, DESK_W, DESK_H - TOPBAR_H - TITLE_H - DOCK_H -
              2 * DOCK_MARGIN);
}

void desk_win_maximize(struct desk_win_s *w)
{
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

void desk_launch(int app)
{
  const struct desk_app_s *a = g_apps[app];
  struct desk_win_s *w;

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

  if (a->open != NULL && (w = a->open()) != NULL)
    {
      win_show(w);
    }
}

/* The display has a new size: the panels follow, maximised windows fill
 * it again, the others shrink to fit and stay on it
 */

static void on_mode(void)
{
  struct nxgl_size_s size;
  struct desk_win_s *w;
  int cw;
  int ch;

  if (nx_displaysize(g_desk.hnx, &size) < 0 ||
      (size.w == g_desk.w && size.h == g_desk.h))
    {
      return;
    }

  g_desk.w = size.w;
  g_desk.h = size.h;
  desk_panels_relayout();
#ifdef CONFIG_FPCDESK_SOUND
  desk_volume_relayout();
#endif

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
          cw = w->csize.w > DESK_W - 40 ? DESK_W - 40 : w->csize.w;
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
                     w->pos.x + w->size.w > DESK_W ?
                     DESK_W - w->size.w : w->pos.x,
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

  /* Applications that show the mode (Settings) */

  for (w = g_desk.wins; w != NULL; w = w->next)
    {
      if (!w->closing && w->app == APP_SETTINGS &&
          g_apps[w->app]->draw != NULL)
        {
          g_apps[w->app]->draw(w);
        }
    }
}

/****************************************************************************
 * Private Functions: events
 ****************************************************************************/

/* The pointer over / pressing / dragging an application window: pos is in
 * frame coordinates (while a button is held, relative to where the frame
 * was when it went down)
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
      if (release)
        {
          g_desk.drag = NULL;
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
      if (release && button && g_apps[w->app]->button != NULL)
        {
          g_apps[w->app]->button(w);
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
      g_apps[w->app]->mouse != NULL)
    {
      g_apps[w->app]->mouse(w, x, y - TITLE_H, buttons);
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
                if (g_apps[w->app]->canclose == NULL ||
                    g_apps[w->app]->canclose(w))
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

#ifdef CONFIG_FPCDESK_SOUND
  desk_volume_mouse_any(ev->part, ev->buttons);
#endif

  switch (ev->part)
    {
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

      default:
        break;
    }

  /* Leaving an application's client area: it hears (-1, -1) */

  if (g_desk.pointerwin != NULL && ev->buttons == 0 &&
      (ev->part != PART_CLIENT || ev->win != g_desk.pointerwin))
    {
      w = g_desk.pointerwin;
      g_desk.pointerwin = NULL;
      if (!w->closing && g_apps[w->app]->mouse != NULL)
        {
          g_apps[w->app]->mouse(w, -1, -1, 0);
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

/* Keys go to the active terminal */

static void on_kbd(struct desk_event_s *ev)
{
  struct desk_win_s *w = g_desk.active;

  if (g_desk.launcher != NULL && ev->nch > 0 && ev->ch[0] == 0x1b)
    {
      desk_launcher_toggle();                   /* Escape */
      return;
    }

  if (w != NULL && !w->closing && g_apps[w->app]->key != NULL)
    {
      g_apps[w->app]->key(w, ev->ch, ev->nch);
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

      case EV_REDRAW:
        if (!ev->win->closing && ev->part == PART_TITLE)
          {
            desk_win_title(ev->win);
          }
        else if (!ev->win->closing)
          {
            const struct desk_app_s *a = g_apps[ev->win->app];

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
  bool more = false;

  for (w = g_desk.wins; w != NULL; w = w->next)
    {
      if (w->fade > 0 && !w->closing)
        {
          w->fade--;
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
              /* Nothing more from the server for it, then it goes
               * (EV_BLOCKED)
               */

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

  more |= desk_panels_animate();
#ifdef CONFIG_FPCDESK_SOUND
  more |= desk_volume_animate();
#endif
  return more;
}

static void tick(void)
{
  struct desk_win_s *w;
  struct desk_win_s *next;

  desk_load_sample();
  desk_panels_tick();
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

      if (g_apps[w->app]->tick != NULL && !w->minimized)
        {
          g_apps[w->app]->tick(w);
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
  bool anim = true;
  int caps;
  int ret;

  memset(&g_desk, 0, sizeof(g_desk));
  g_desk.dockhover = -1;
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

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, CONFIG_FPCDESK_LISTENER_STACKSIZE);
  pthread_create(&thread, &attr, listener, NULL);

  caps = nx_compositor(g_desk.hnx);

  /* The display mode saved last time (the listener will see the event of
   * the change; the panels are made at the new size)
   */

  ret = desk_prefs_load();
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
  if (!g_desk.composited)
    {
      fprintf(stderr, "desk: the display is not composited (not a v5 GPU: "
                      "see the boot log): the desktop will look plain\n");
    }

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

  /* Start with a terminal */

  desk_launch(APP_TERMINAL);

  nexttick = desk_ms() + 1000;
  for (; ; )
    {
      int32_t wait = anim ? FRAME_MS : (int32_t)(nexttick - desk_ms());

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
