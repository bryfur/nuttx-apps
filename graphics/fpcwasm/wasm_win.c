/****************************************************************************
 * apps/graphics/fpcwasm/wasm_win.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The fpc_win_* functions of WebAssembly apps (sdk/fpc_wasm.h, API version
 * 2; docs/wasm.md): an app as windows on the desktop, over deskwin.h (the
 * desktop's windows for other programs, desk_client.c): drawing (filled
 * and rounded rectangles, gradients, lines, the desktop's fonts, RGB565
 * pixels, images from the app's directory), the widget kit's controls,
 * the window's events.  A WASI command that opens a window and reads its
 * events until it closes is a desktop application.
 *
 * The sandbox's side: handles are small integers (a window of this app's,
 * an image of that window's), strings come with their lengths and are
 * copied (at most MAX_STR bytes), buffers are checked whole against the
 * linear memory (a trap if one is not inside it).  A bad handle is
 * ignored or answered with -EBADF.  The desktop checks everything a client
 * sends as well: a program in trouble cannot take it down.  The windows go
 * when the app ends (wasm_win_end()), as a program's do when it exits.
 ****************************************************************************/

#include "wasm_internal.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_FPCDESK_CLIENTS
#  include <graphics/deskwin.h>
#  include <nuttx/nx/nxcompose.h>
#endif

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define WIN_MAX      4                    /* Windows an app has at once */
#define MAX_STR      255                  /* A string's bytes (more: cut) */
#define EV_WORDS     12                   /* fpc_win_event_t */

/* deskwin_ops() draws in frame coordinates: the client area is below the
 * title bar (graphics/fpcdesk/desk.h's TITLE_H)
 */

#define WIN_TITLE_H  34

/****************************************************************************
 * Private Types
 ****************************************************************************/

#ifdef CONFIG_FPCDESK_CLIENTS
struct wwin_s
{
  FAR struct deskwin_s *d;
  FAR void *surf;                         /* fpc_win_put()'s pixels */
  int sw;
  int sh;
  volatile bool lost;                     /* (the desktop let them go) */
};
#endif

/****************************************************************************
 * Private Data
 ****************************************************************************/

#ifdef CONFIG_FPCDESK_CLIENTS
static struct wwin_s g_win[WIN_MAX];
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

#ifdef CONFIG_FPCDESK_CLIENTS

/* The app's string (off, len) copied into out (NUL-terminated, cut at
 * MAX_STR); false: outside the linear memory (a trap)
 */

static bool app_str(wasm_exec_env_t env, uint32_t off, int32_t len,
                    FAR char *out)
{
  FAR const char *s;

  if (len <= 0)
    {
      out[0] = '\0';
      return true;
    }

  s = wasm_app_buf(env, off, (uint32_t)len);
  if (s == NULL)
    {
      return false;
    }

  len = len > MAX_STR ? MAX_STR : len;
  memcpy(out, s, len);
  out[len] = '\0';
  return true;
}

static FAR struct deskwin_s *win_d(int32_t win)
{
  if (win < 0 || win >= WIN_MAX)
    {
      return NULL;
    }

  return g_win[win].d;
}

static uint16_t c16(int32_t colour)
{
  return (uint16_t)colour;
}

static int a8(int32_t alpha)
{
  return alpha < 0 ? 0 : alpha > 255 ? 255 : alpha;
}

/****************************************************************************
 * The window
 ****************************************************************************/

static int32_t n_win_open(wasm_exec_env_t env, uint32_t title, int32_t len,
                          int32_t w, int32_t h)
{
  char t[MAX_STR + 1];
  int i;

  if (!app_str(env, title, len, t))
    {
      return -EFAULT;
    }

  if (g_wapp.in_game)
    {
      return -EPERM;                      /* (the display is the game's) */
    }

  for (i = 0; i < WIN_MAX && g_win[i].d != NULL; i++);
  if (i == WIN_MAX)
    {
      return -EMFILE;
    }

  w = w < 32 ? 32 : w > 2048 ? 2048 : w;
  h = h < 32 ? 32 : h > 2048 ? 2048 : h;
  g_win[i].d = deskwin_open(t[0] != '\0' ? t : g_wapp.name, w, h);
  if (g_win[i].d == NULL)
    {
      return errno != 0 ? -errno : -ENODEV;
    }

  g_win[i].surf = NULL;
  g_wapp.windows++;
  return i;
}

static void n_win_close(wasm_exec_env_t env, int32_t win)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL)
    {
      if (g_win[win].surf != NULL)
        {
          deskwin_surface_close(d, g_win[win].surf);
        }

      deskwin_close(d);
      memset(&g_win[win], 0, sizeof(g_win[win]));
      g_wapp.windows--;
    }
}

static int32_t n_win_alive(wasm_exec_env_t env, int32_t win)
{
  FAR struct deskwin_s *d = win_d(win);

  return d != NULL && deskwin_alive(d);
}

static int32_t n_win_width(wasm_exec_env_t env, int32_t win)
{
  FAR struct deskwin_s *d = win_d(win);
  int w = 0;
  int h = 0;

  if (d == NULL)
    {
      return -EBADF;
    }

  deskwin_size(d, &w, &h);
  return w;
}

static int32_t n_win_height(wasm_exec_env_t env, int32_t win)
{
  FAR struct deskwin_s *d = win_d(win);
  int w = 0;
  int h = 0;

  if (d == NULL)
    {
      return -EBADF;
    }

  deskwin_size(d, &w, &h);
  return h;
}

static int32_t n_win_event(wasm_exec_env_t env, int32_t win, uint32_t out,
                           int32_t timeout)
{
  FAR struct deskwin_s *d = win_d(win);
  struct deskwin_event_s ev;
  FAR int32_t *o;
  int ret;
  int k;

  o = wasm_app_buf(env, out, EV_WORDS * 4);
  if (o == NULL)
    {
      return -EFAULT;
    }

  if (d == NULL)
    {
      return -EBADF;
    }

  if (deskwin_flush(d, 1000) == -EPIPE && !deskwin_alive(d))
    {
      timeout = 0;                        /* (what is left of its events) */
    }

  ret = deskwin_event(d, &ev, timeout < 0 ? -1 : timeout);
  if (ret <= 0)
    {
      return ret;
    }

  memset(o, 0, EV_WORDS * 4);
  o[0]  = ev.type;
  o[1]  = ev.x;
  o[2]  = ev.y;
  o[3]  = ev.w;
  o[4]  = ev.h;
  o[5]  = ev.buttons;
  if (ev.type == DESKWIN_EV_KEY || ev.type == DESKWIN_EV_KEYUP)
    {
      k = wasm_fpc_key_code(ev.key);
      o[6] = k >= 0 ? k : 0;
      o[7] = ev.ch >= 0x20 && ev.ch < 0x7f ? ev.ch :
             ev.ch == '\r' || ev.ch == '\t' || ev.ch == '\b' ? ev.ch : 0;
    }

  o[8]  = ev.mods;
  o[9]  = ev.value;
  o[10] = ev.id;
  o[11] = ev.kind;
  return 1;
}

static int32_t n_win_flush(wasm_exec_env_t env, int32_t win)
{
  FAR struct deskwin_s *d = win_d(win);

  return d != NULL ? deskwin_flush(d, 2000) : -EBADF;
}

static void n_win_title(wasm_exec_env_t env, int32_t win, uint32_t s,
                        int32_t len)
{
  FAR struct deskwin_s *d = win_d(win);
  char t[MAX_STR + 1];

  if (app_str(env, s, len, t) && d != NULL)
    {
      deskwin_title(d, t);
    }
}

static void n_win_frames(wasm_exec_env_t env, int32_t win, int32_t on)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL)
    {
      deskwin_frames(d, on != 0);
    }
}

static void n_win_raise(wasm_exec_env_t env, int32_t win)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL)
    {
      deskwin_raise(d);
    }
}

/****************************************************************************
 * Drawing
 ****************************************************************************/

static void n_win_fill(wasm_exec_env_t env, int32_t win, int32_t x,
                       int32_t y, int32_t w, int32_t h, int32_t colour)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL)
    {
      deskwin_fill(d, x, y, w, h, c16(colour));
    }
}

static void n_win_rect(wasm_exec_env_t env, int32_t win, int32_t x,
                       int32_t y, int32_t w, int32_t h, int32_t colour,
                       int32_t alpha, int32_t radius)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL)
    {
      deskwin_rect(d, x, y, w, h, c16(colour), a8(alpha),
                   radius < 0 ? 0 : radius > 255 ? 255 : radius);
    }
}

static void n_win_gradient(wasm_exec_env_t env, int32_t win, int32_t x,
                           int32_t y, int32_t w, int32_t h, int32_t c0,
                           int32_t c1, int32_t alpha, int32_t vertical)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL)
    {
      deskwin_gradient(d, x, y, w, h, c16(c0), c16(c1), a8(alpha),
                       vertical != 0);
    }
}

static void n_win_line(wasm_exec_env_t env, int32_t win, int32_t x0,
                       int32_t y0, int32_t x1, int32_t y1, int32_t colour,
                       int32_t alpha)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL)
    {
      deskwin_line(d, x0, y0, x1, y1, c16(colour), a8(alpha));
    }
}

static void n_win_text(wasm_exec_env_t env, int32_t win, int32_t x,
                       int32_t y, int32_t font, int32_t colour,
                       int32_t alpha, uint32_t s, int32_t len)
{
  FAR struct deskwin_s *d = win_d(win);
  char t[MAX_STR + 1];

  if (app_str(env, s, len, t) && d != NULL && font >= 0 &&
      font < DESKWIN_FONTS)
    {
      deskwin_text(d, x, y, font, c16(colour), a8(alpha), t);
    }
}

static int32_t n_win_text_width(wasm_exec_env_t env, int32_t font,
                                uint32_t s, int32_t len)
{
  char t[MAX_STR + 1];

  if (!app_str(env, s, len, t))
    {
      return 0;
    }

  return font >= 0 && font < DESKWIN_FONTS ? deskwin_text_width(font, t) :
         -EINVAL;
}

static int32_t n_win_text_height(wasm_exec_env_t env, int32_t font)
{
  return font >= 0 && font < DESKWIN_FONTS ? deskwin_text_height(font) :
         -EINVAL;
}

static void n_win_clip(wasm_exec_env_t env, int32_t win, int32_t x,
                       int32_t y, int32_t w, int32_t h)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL)
    {
      deskwin_clip(d, x, y, w, h);
    }
}

static void n_win_clear(wasm_exec_env_t env, int32_t win, int32_t colour)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL)
    {
      deskwin_clear(d, c16(colour));
    }
}

static void n_win_copy(wasm_exec_env_t env, int32_t win, int32_t sx,
                       int32_t sy, int32_t w, int32_t h, int32_t dx,
                       int32_t dy)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL)
    {
      deskwin_copy(d, sx, sy, w, h, dx, dy);
    }
}

/* RGB565 pixels: into a surface of the window's (as big as the client
 * area, made again bigger when it grows), then copied from it into the
 * window by the desktop's GPU
 */

static void n_win_put(wasm_exec_env_t env, int32_t win, int32_t x,
                      int32_t y, int32_t w, int32_t h, uint32_t pix,
                      int32_t stride)
{
  FAR struct deskwin_s *d = win_d(win);
  FAR struct wwin_s *ww;
  FAR const uint16_t *p;
  struct nx_gpuop_s op;
  uint64_t size;
  int cw;
  int ch;

  size = wasm_rect_size(w, h, stride, 2);
  if (size == 0)
    {
      return;
    }

  p = wasm_app_buf(env, pix, size);
  if (p == NULL || d == NULL || (pix & 1) != 0)
    {
      return;
    }

  /* Inside the client area only (the surface is its size) */

  ww = &g_win[win];
  deskwin_size(d, &cw, &ch);
  if (x < 0)
    {
      p -= x;
      w += x;
      x = 0;
    }

  if (y < 0)
    {
      p -= (int64_t)y * stride;
      h += y;
      y = 0;
    }

  w = x + w > cw ? cw - x : w;
  h = y + h > ch ? ch - y : h;
  if (w <= 0 || h <= 0)
    {
      return;
    }

  if (ww->surf == NULL || ww->sw < cw || ww->sh < ch)
    {
      if (ww->surf != NULL)
        {
          deskwin_surface_close(d, ww->surf);
        }

      ww->sw   = cw;
      ww->sh   = ch;
      ww->lost = false;
      ww->surf = deskwin_surface_open(d, cw, ch, &ww->lost);
      if (ww->surf == NULL)
        {
          return;
        }
    }

  deskwin_surface_put(d, ww->surf, x, y, w, h, p, stride * 2);

  memset(&op, 0, sizeof(op));
  op.op    = NX_GPUOP_COPY;
  op.alpha = 255;
  op.x     = x;
  op.y     = y + WIN_TITLE_H;
  op.w     = w;
  op.h     = h;
  op.src   = ww->surf;
  op.sx    = x;
  op.sy    = y;
  deskwin_ops(d, &op, 1);
}

static int32_t n_win_image_load(wasm_exec_env_t env, int32_t win,
                                uint32_t path, int32_t len)
{
  FAR struct deskwin_s *d = win_d(win);
  char full[PATH_MAX];
  char err[64];
  FAR const char *s;
  int w;
  int h;
  int ret;

  if (len <= 0)
    {
      return -EINVAL;
    }

  s = wasm_app_buf(env, path, (uint32_t)len);
  if (s == NULL)
    {
      return -EFAULT;
    }

  if (d == NULL)
    {
      return -EBADF;
    }

  ret = wasm_app_path(s, len, full, sizeof(full));
  if (ret < 0)
    {
      return ret;
    }

  return deskwin_image_load(d, full, 0, 0, &w, &h, err, sizeof(err));
}

static void n_win_image(wasm_exec_env_t env, int32_t win, int32_t img,
                        int32_t sx, int32_t sy, int32_t w, int32_t h,
                        int32_t dx, int32_t dy, int32_t alpha)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL && img >= 0 && img < DESKWIN_IMAGES)
    {
      deskwin_image(d, img, sx, sy, w, h, dx, dy, a8(alpha));
    }
}

static void n_win_image_free(wasm_exec_env_t env, int32_t win, int32_t img)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL && img >= 0 && img < DESKWIN_IMAGES)
    {
      deskwin_image_free(d, img);
    }
}

static int32_t n_win_colour(wasm_exec_env_t env, uint32_t role, int32_t len)
{
  char t[MAX_STR + 1];

  if (!app_str(env, role, len, t))
    {
      return -1;
    }

  return deskwin_colour(t);
}

/****************************************************************************
 * Widgets
 ****************************************************************************/

static void n_win_widget(wasm_exec_env_t env, int32_t win, int32_t id,
                         int32_t type, int32_t x, int32_t y, int32_t w,
                         int32_t h, uint32_t text, int32_t len,
                         int32_t value, int32_t style)
{
  FAR struct deskwin_s *d = win_d(win);
  char t[MAX_STR + 1];

  if (app_str(env, text, len, t) && d != NULL && id >= 1 &&
      id <= DESKWIN_WIDGETS)
    {
      deskwin_widget(d, id, type, x, y, w, h, t, NULL, value, 0, 100,
                     style);
    }
}

static void n_win_widget_set(wasm_exec_env_t env, int32_t win, int32_t id,
                             int32_t what, int32_t value, uint32_t text,
                             int32_t len)
{
  FAR struct deskwin_s *d = win_d(win);
  FAR char *t;

  /* A list's items may be long: up to 4 KB of them */

  if (len < 0 || len > 4095)
    {
      return;
    }

  if (len > 0 && wasm_app_buf(env, text, (uint32_t)len) == NULL)
    {
      return;
    }

  if (d == NULL || id < 1 || id > DESKWIN_WIDGETS)
    {
      return;
    }

  t = malloc(len + 1);
  if (t != NULL)
    {
      if (len > 0)
        {
          memcpy(t, wasm_app_buf(env, text, (uint32_t)len), len);
        }

      t[len] = '\0';
      deskwin_widget_set(d, id, what, value, t);
      free(t);
    }
}

static int32_t n_win_widget_value(wasm_exec_env_t env, int32_t win,
                                  int32_t id)
{
  FAR struct deskwin_s *d = win_d(win);
  int value = 0;

  if (d == NULL || deskwin_flush(d, 1000) < 0 ||
      !deskwin_widget_get(d, id, &value, NULL, 0))
    {
      return 0;
    }

  return value;
}

static int32_t n_win_widget_text(wasm_exec_env_t env, int32_t win,
                                 int32_t id, uint32_t buf, int32_t size)
{
  FAR struct deskwin_s *d = win_d(win);
  char t[MAX_STR + 1];
  FAR char *o;
  int value;
  int n;

  if (size <= 0)
    {
      return -EINVAL;
    }

  o = wasm_app_buf(env, buf, (uint32_t)size);
  if (o == NULL)
    {
      return -EFAULT;
    }

  if (d == NULL)
    {
      return -EBADF;
    }

  if (deskwin_flush(d, 1000) < 0 ||
      !deskwin_widget_get(d, id, &value, t, sizeof(t)))
    {
      return -ENOENT;
    }

  n = strlen(t);
  n = n < size - 1 ? n : size - 1;
  memcpy(o, t, n);
  o[n] = '\0';
  return n;
}

static void n_win_widget_remove(wasm_exec_env_t env, int32_t win,
                                int32_t id)
{
  FAR struct deskwin_s *d = win_d(win);

  if (d != NULL && id >= 1 && id <= DESKWIN_WIDGETS)
    {
      deskwin_widget_remove(d, id);
    }
}

#else /* !CONFIG_FPCDESK_CLIENTS */

/* No desktop to have windows on: the functions are there (an app that
 * imports them starts), a window does not open
 */

static int32_t n_win_open(wasm_exec_env_t env, uint32_t title, int32_t len,
                          int32_t w, int32_t h)
{
  return -ENODEV;
}

static int32_t n_win_none(wasm_exec_env_t env)
{
  return -EBADF;
}

#  define n_win_close         n_win_none
#  define n_win_alive         n_win_none
#  define n_win_width         n_win_none
#  define n_win_height        n_win_none
#  define n_win_event         n_win_none
#  define n_win_flush         n_win_none
#  define n_win_title         n_win_none
#  define n_win_frames        n_win_none
#  define n_win_raise         n_win_none
#  define n_win_fill          n_win_none
#  define n_win_rect          n_win_none
#  define n_win_gradient      n_win_none
#  define n_win_line          n_win_none
#  define n_win_text          n_win_none
#  define n_win_text_width    n_win_none
#  define n_win_text_height   n_win_none
#  define n_win_clip          n_win_none
#  define n_win_clear         n_win_none
#  define n_win_copy          n_win_none
#  define n_win_put           n_win_none
#  define n_win_image_load    n_win_none
#  define n_win_image         n_win_none
#  define n_win_image_free    n_win_none
#  define n_win_colour        n_win_none
#  define n_win_widget        n_win_none
#  define n_win_widget_set    n_win_none
#  define n_win_widget_value  n_win_none
#  define n_win_widget_text   n_win_none
#  define n_win_widget_remove n_win_none

#endif /* CONFIG_FPCDESK_CLIENTS */

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#define N(name, sig) { "fpc_" #name, (void *)n_##name, sig, NULL }

static NativeSymbol g_win_natives[] =
{
  N(win_open,          "(iiii)i"),
  N(win_close,         "(i)"),
  N(win_alive,         "(i)i"),
  N(win_width,         "(i)i"),
  N(win_height,        "(i)i"),
  N(win_event,         "(iii)i"),
  N(win_flush,         "(i)i"),
  N(win_title,         "(iii)"),
  N(win_frames,        "(ii)"),
  N(win_raise,         "(i)"),
  N(win_fill,          "(iiiiii)"),
  N(win_rect,          "(iiiiiiii)"),
  N(win_gradient,      "(iiiiiiiii)"),
  N(win_line,          "(iiiiiii)"),
  N(win_text,          "(iiiiiiii)"),
  N(win_text_width,    "(iii)i"),
  N(win_text_height,   "(i)i"),
  N(win_clip,          "(iiiii)"),
  N(win_clear,         "(ii)"),
  N(win_copy,          "(iiiiiii)"),
  N(win_put,           "(iiiiiii)"),
  N(win_image_load,    "(iii)i"),
  N(win_image,         "(iiiiiiiii)"),
  N(win_image_free,    "(ii)"),
  N(win_colour,        "(ii)i"),
  N(win_widget,        "(iiiiiiiiiii)"),
  N(win_widget_set,    "(iiiiii)"),
  N(win_widget_value,  "(ii)i"),
  N(win_widget_text,   "(iiii)i"),
  N(win_widget_remove, "(ii)"),
};

bool wasm_win_register(void)
{
  return wasm_runtime_register_natives("env", g_win_natives,
                                       sizeof(g_win_natives) /
                                       sizeof(g_win_natives[0]));
}

/* The app has ended: its windows go */

void wasm_win_end(void)
{
#ifdef CONFIG_FPCDESK_CLIENTS
  int i;

  for (i = 0; i < WIN_MAX; i++)
    {
      if (g_win[i].d != NULL)
        {
          if (g_win[i].surf != NULL)
            {
              deskwin_surface_close(g_win[i].d, g_win[i].surf);
            }

          deskwin_close(g_win[i].d);
        }
    }

  memset(g_win, 0, sizeof(g_win));
  g_wapp.windows = 0;
#endif
}

/* Whether one of its windows is still open on the desktop */

bool wasm_win_any_alive(void)
{
#ifdef CONFIG_FPCDESK_CLIENTS
  int i;

  for (i = 0; i < WIN_MAX; i++)
    {
      if (g_win[i].d != NULL && deskwin_alive(g_win[i].d))
        {
          return true;
        }
    }
#endif

  return false;
}
