/****************************************************************************
 * apps/graphics/fpcdesk/desk_python.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Python: the application of the windows Python programs open (MicroPython's
 * fpc module, through deskwin.h: desk_client.c).  From the launcher or the
 * dock it opens a terminal running python.  Each callback goes to the
 * kind of window it is for (the first word of w->priv).
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "desk.h"

#ifdef CONFIG_FPCDESK_PYTHON

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The application of a window's kind */

static const struct desk_app_s *kind(struct desk_win_s *w)
{
  if (w->priv != NULL && *(const uint32_t *)w->priv == PYTHON_CLIENT)
    {
      return &g_app_client;
    }

  return NULL;
}

/* Two snakes' heads, blue and yellow, round a tile */

static void python_icon(struct desk_ops_s *o, int x, int y, int s,
                        uint8_t a)
{
  int u = s / 16;
  int bw = s * 7 / 16;
  int bh = s * 3 / 8;
  int r = s >= 48 ? 12 : 6;
  int slot = s >= 48 ? LUT_R12 : LUT_R6;

  icon_tile(o, x, y, s, RGB(0x2e, 0x3e, 0x52), RGB(0x16, 0x1e, 0x2a), a);
  ops_rrect(o, x + s * 3 / 16, y + s * 3 / 16, bw, bh, slot, r,
            RGB(0x37, 0x76, 0xab), a);
  ops_rrect(o, x + s - s * 3 / 16 - bw, y + s - s * 3 / 16 - bh, bw, bh,
            slot, r, RGB(0xff, 0xd3, 0x43), a);
  ops_rrect(o, x + s * 5 / 16, y + s * 5 / 16, 2 * u, 2 * u, LUT_R3,
            u < 3 ? u : 3, RGB(0xff, 0xff, 0xff), a);
  ops_rrect(o, x + s - s * 5 / 16 - 2 * u, y + s - s * 5 / 16 - 2 * u,
            2 * u, 2 * u, LUT_R3, u < 3 ? u : 3, RGB(0xff, 0xff, 0xff), a);
}

static struct desk_win_s *python_open(void)
{
  return desk_term_run("python");
}

static void python_draw(struct desk_win_s *w)
{
  const struct desk_app_s *a = kind(w);

  if (a != NULL && a->draw != NULL)
    {
      a->draw(w);
    }
  else if (a != NULL && a->expose != NULL)
    {
      a->expose(w, 0, 0, w->csize.w, w->csize.h);
    }
}

static void python_expose(struct desk_win_s *w, int x, int y, int cw,
                          int ch)
{
  const struct desk_app_s *a = kind(w);

  if (a != NULL && a->expose != NULL)
    {
      a->expose(w, x, y, cw, ch);
    }
  else if (a != NULL && a->draw != NULL)
    {
      a->draw(w);
    }
}

#define FORWARD(name, args, call) \
  static void python_##name args \
  { \
    const struct desk_app_s *a = kind(w); \
    if (a != NULL && a->name != NULL) \
      { \
        a->name call; \
      } \
  }

FORWARD(resize, (struct desk_win_s *w), (w))
FORWARD(tick, (struct desk_win_s *w), (w))
FORWARD(mouse, (struct desk_win_s *w, int x, int y, uint8_t buttons),
        (w, x, y, buttons))
FORWARD(key, (struct desk_win_s *w, const uint8_t *ch, int n), (w, ch, n))
FORWARD(keyev, (struct desk_win_s *w, const struct nx_key_s *k), (w, k))
FORWARD(close, (struct desk_win_s *w), (w))
FORWARD(wake, (struct desk_win_s *w), (w))
FORWARD(focus, (struct desk_win_s *w), (w))
FORWARD(button, (struct desk_win_s *w), (w))
FORWARD(frame, (struct desk_win_s *w), (w))

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_python =
{
  .name   = "Python",
  .icon   = python_icon,
  .open   = python_open,
  .draw   = python_draw,
  .expose = python_expose,
  .resize = python_resize,
  .tick   = python_tick,
  .mouse  = python_mouse,
  .key    = python_key,
  .keyev  = python_keyev,
  .close  = python_close,
  .wake   = python_wake,
  .focus  = python_focus,
  .button = python_button,
  .frame  = python_frame,
};

#endif /* CONFIG_FPCDESK_PYTHON */
