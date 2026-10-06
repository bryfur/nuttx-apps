/* apps/graphics/fpcwasm/sdk/examples/mandel/mandel.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A WebAssembly app that is a desktop window (fpc_win_*, API version 2):
 * the Mandelbrot set computed by the app (fixed point, as fast as the
 * runtime runs it), its pixels put into the window, the desktop's widgets
 * beside it (the depth, a button, a label), the mouse to zoom (left: in,
 * right: out), keys (+, -, Home, arrows).  The window can be resized: the
 * picture follows.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fpc_wasm.h"

#define BAR       40                     /* The widgets' strip at the top */
#define ID_DEPTH  1
#define ID_RESET  2
#define ID_INFO   3
#define ID_DLABEL 4

static int g_win;
static int g_w;
static int g_h;
static uint16_t *g_pix;
static int g_pw;
static int g_ph;
static double g_cx = -0.6;               /* The centre, the scale (units a */
static double g_cy = 0.0;                /* pixel) */
static double g_scale;
static int g_depth = 96;

static int slen(const char *s)
{
  return (int)strlen(s);
}

static uint16_t shade(int n)
{
  int t;

  if (n >= g_depth)
    {
      return 0;
    }

  /* A cycle of 96 steps: night blue, cyan, gold, red, back */

  t = (n * 4) % 384;
  if (t < 128)
    {
      return FPC_RGB(0, t * 2, 64 + t);
    }
  else if (t < 256)
    {
      return FPC_RGB((t - 128) * 2, 255, 192 - (t - 128) * 3 / 2);
    }

  return FPC_RGB(255, 255 - (t - 256) * 2, (t - 256) / 2);
}

/* The picture: 16.16 fixed point, an escape count a pixel */

static void render(void)
{
  int32_t step;
  int32_t x0;
  int32_t y0;
  int y;
  int x;

  if (g_pix == NULL)
    {
      return;
    }

  step = (int32_t)(g_scale * 65536.0);
  x0   = (int32_t)((g_cx - g_scale * g_pw / 2) * 65536.0);
  y0   = (int32_t)((g_cy - g_scale * g_ph / 2) * 65536.0);
  for (y = 0; y < g_ph; y++)
    {
      int32_t ci = y0 + y * step;
      uint16_t *row = g_pix + y * g_pw;

      for (x = 0; x < g_pw; x++)
        {
          int32_t cr = x0 + x * step;
          int32_t zr = 0;
          int32_t zi = 0;
          int n;

          for (n = 0; n < g_depth; n++)
            {
              int64_t rr = (int64_t)zr * zr;
              int64_t ii = (int64_t)zi * zi;

              if (rr + ii > ((int64_t)4 << 32))
                {
                  break;
                }

              zi = (int32_t)(((int64_t)zr * zi) >> 15) + ci;
              zr = (int32_t)((rr - ii) >> 16) + cr;
            }

          row[x] = shade(n);
        }
    }
}

static void layout(void)
{
  int w = fpc_win_width(g_win);
  int h = fpc_win_height(g_win);

  if (w == g_w && h == g_h && g_pix != NULL)
    {
      return;
    }

  g_w  = w;
  g_h  = h;
  g_pw = w;
  g_ph = h > BAR ? h - BAR : 1;
  free(g_pix);
  g_pix = malloc((size_t)g_pw * g_ph * 2);
  if (g_scale == 0)
    {
      g_scale = 3.0 / g_pw;
    }

  fpc_win_widget(g_win, ID_DLABEL, FPC_WIN_LABEL, 8, 12, 0, 0, "Depth", 5,
                 0, FPC_WIN_FONT_BODY);
  fpc_win_widget(g_win, ID_DEPTH, FPC_WIN_SLIDER, 56, 8, 140, 0, "", 0,
                 g_depth, 0);
  fpc_win_widget_set(g_win, ID_DEPTH, FPC_WIN_SET_RANGE, 0, "16 400", 6);
  fpc_win_widget(g_win, ID_RESET, FPC_WIN_BUTTON, 208, 6, 0, 0, "Reset", 5,
                 0, 0);
  fpc_win_widget(g_win, ID_INFO, FPC_WIN_LABEL, 290, 12, w - 298, 0, "", 0,
                 0, FPC_WIN_FONT_SMALL);
}

static void draw(void)
{
  char info[96];
  uint32_t t0 = fpc_time_ms();
  uint32_t ms;

  layout();
  render();
  ms = fpc_time_ms() - t0;
  fpc_win_fill(g_win, 0, 0, g_w, BAR, fpc_win_colour("window_bg", 9));
  if (g_pix != NULL)
    {
      fpc_win_put(g_win, 0, BAR, g_pw, g_ph, g_pix, g_pw);
    }

  snprintf(info, sizeof(info), "%d x %d, %u ms, x%.0f", g_pw, g_ph,
           (unsigned)ms, 3.0 / g_pw / g_scale);
  fpc_win_widget_set(g_win, ID_INFO, FPC_WIN_SET_TEXT, 0, info, slen(info));
  fpc_win_flush(g_win);
}

static void zoom(int x, int y, double f)
{
  g_cx += (x - g_pw / 2) * g_scale;
  g_cy += (y - BAR - g_ph / 2) * g_scale;
  g_scale *= f;
}

int main(int argc, char **argv)
{
  fpc_win_event_t ev;
  bool dirty = false;
  int ret;

  g_win = fpc_win_open("Mandelbrot", 10, 480, 360 + BAR);
  if (g_win < 0)
    {
      printf("mandel: no window (%d): it runs on the desktop\n", g_win);
      return 1;
    }

  draw();

  /* (events that change the picture are taken together: it is drawn
   * again once none is waiting)
   */

  while ((ret = fpc_win_event(g_win, &ev, dirty ? 0 : -1)) >= 0)
    {
      bool again = false;

      if (ret == 0)
        {
          if (dirty)
            {
              draw();
              dirty = false;
            }

          continue;
        }

      switch (ev.type)
        {
          case FPC_WIN_EV_CLOSE:
            fpc_win_close(g_win);
            free(g_pix);
            return 0;

          case FPC_WIN_EV_RESIZE:
          case FPC_WIN_EV_EXPOSE:
            again = true;
            break;

          case FPC_WIN_EV_MOUSE:
            if (ev.buttons != 0 && ev.y >= BAR)
              {
                zoom(ev.x, ev.y, (ev.buttons & FPC_WIN_LEFT) ? 0.5 : 2.0);
                again = true;
              }
            break;

          case FPC_WIN_EV_KEY:
            if (ev.ch == '+' || ev.ch == '=')
              {
                g_scale *= 0.5;
              }
            else if (ev.ch == '-')
              {
                g_scale *= 2.0;
              }
            else if (ev.key == FPC_KEY_LEFT || ev.key == FPC_KEY_RIGHT)
              {
                g_cx += (ev.key == FPC_KEY_LEFT ? -40 : 40) * g_scale;
              }
            else if (ev.key == FPC_KEY_UP || ev.key == FPC_KEY_DOWN)
              {
                g_cy += (ev.key == FPC_KEY_UP ? -40 : 40) * g_scale;
              }
            else if (ev.key == FPC_KEY_HOME)
              {
                g_cx = -0.6;
                g_cy = 0;
                g_scale = 3.0 / g_pw;
              }
            else
              {
                break;
              }

            again = true;
            break;

          case FPC_WIN_EV_WIDGET:
            if (ev.id == ID_DEPTH && ev.kind == FPC_WIN_CHANGE)
              {
                g_depth = ev.value < 16 ? 16 : ev.value;
                again = true;
              }
            else if (ev.id == ID_RESET)
              {
                g_cx = -0.6;
                g_cy = 0;
                g_scale = 3.0 / g_pw;
                again = true;
              }
            break;
        }

      dirty = dirty || again;
    }

  free(g_pix);
  return 0;
}
