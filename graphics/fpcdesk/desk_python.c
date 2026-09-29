/****************************************************************************
 * apps/graphics/fpcdesk/desk_python.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Python: the application of the Python IDE's windows (desk_ide.c: the
 * Text Editor with a console) and of the windows Python programs open
 * (MicroPython's fpc module, through deskwin.h: desk_client.c).  The
 * launcher's tile and the dock open an IDE.  Each callback goes to the
 * kind of window it is for (the first word of w->priv).
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdint.h>

#include "desk.h"

#ifdef CONFIG_FPCDESK_PYTHON

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t kind_of(struct desk_win_s *w)
{
  return w->priv != NULL ? *(const uint32_t *)w->priv : 0;
}

/* The application of a window's kind: a program's, or the IDE's editor */

static const struct desk_app_s *kind(struct desk_win_s *w)
{
  switch (kind_of(w))
    {
      case PYTHON_CLIENT:
        return &g_app_client;

      case PYTHON_IDE:
        return &g_app_editor;

      default:
        return NULL;
    }
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
  return desk_ide_open(NULL);
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

static void python_resize(struct desk_win_s *w)
{
  const struct desk_app_s *a = kind(w);

  if (a != NULL && a->resize != NULL)
    {
      a->resize(w);
    }
  else
    {
      python_draw(w);
    }
}

/* Key events: a program's window takes them; the editor, what they type */

static void python_keyev(struct desk_win_s *w, const struct nx_key_s *k)
{
  const struct desk_app_s *a = kind(w);

  if (a != NULL && a->keyev != NULL)
    {
      a->keyev(w, k);
    }
  else if (a != NULL && a->key != NULL)
    {
      uint8_t bytes[NX_KEY_MAXBYTES];
      int n = nx_key_bytes(k, bytes);

      if (n > 0)
        {
          a->key(w, bytes, n);
        }
    }
}

static void python_wake(struct desk_win_s *w)
{
  if (kind_of(w) == PYTHON_IDE)
    {
      desk_ide_wake(w);
    }
  else if (kind_of(w) == PYTHON_CLIENT)
    {
      g_app_client.wake(w);
    }
}

static void python_tick(struct desk_win_s *w)
{
  const struct desk_app_s *a = kind(w);

  if (a != NULL && a->tick != NULL)
    {
      a->tick(w);
    }

  if (kind_of(w) == PYTHON_IDE)
    {
      desk_ide_tick(w);
    }
}

static bool python_canclose(struct desk_win_s *w)
{
  const struct desk_app_s *a = kind(w);

  return a == NULL || a->canclose == NULL || a->canclose(w);
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

FORWARD(mouse, (struct desk_win_s *w, int x, int y, uint8_t buttons),
        (w, x, y, buttons))
FORWARD(key, (struct desk_win_s *w, const uint8_t *ch, int n), (w, ch, n))
FORWARD(close, (struct desk_win_s *w), (w))
FORWARD(focus, (struct desk_win_s *w), (w))
FORWARD(button, (struct desk_win_s *w), (w))
FORWARD(frame, (struct desk_win_s *w), (w))

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_python =
{
  .name     = "Python",
  .icon     = python_icon,
  .open     = python_open,
  .draw     = python_draw,
  .expose   = python_expose,
  .resize   = python_resize,
  .tick     = python_tick,
  .mouse    = python_mouse,
  .key      = python_key,
  .keyev    = python_keyev,
  .close    = python_close,
  .wake     = python_wake,
  .focus    = python_focus,
  .button   = python_button,
  .frame    = python_frame,
  .canclose = python_canclose,
};

#endif /* CONFIG_FPCDESK_PYTHON */
