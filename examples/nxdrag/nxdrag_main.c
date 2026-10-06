/****************************************************************************
 * apps/examples/nxdrag/nxdrag_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Window drag benchmark: a RAM-backed framed window on the running NX
 * server, moved N steps of (dx, dy), each timed to nxtk_synch().  scroll
 * scrolls its content a text line at a time; text draws one glyph bitmap
 * per character cell, as NxTerm does.
 *
 *   nxdrag [steps [dx [dy [w [h]]]]]     defaults: 60 8 4 500 350
 *   nxdrag scroll [steps [line]]         defaults: 60 16
 *   nxdrag text [screens]                defaults: 10
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <nuttx/nx/nx.h>
#include <nuttx/nx/nxtk.h>
#include <nuttx/nx/nxbe.h>

static volatile bool g_synched;

static void drag_redraw(NXWINDOW hwnd, FAR const struct nxgl_rect_s *rect,
                        bool more, FAR void *arg)
{
}

static void drag_position(NXWINDOW hwnd, FAR const struct nxgl_size_s *size,
                          FAR const struct nxgl_point_s *pos,
                          FAR const struct nxgl_rect_s *bounds,
                          FAR void *arg)
{
}

static void drag_event(NXWINDOW hwnd, enum nx_event_e event,
                       FAR void *arg1, FAR void *arg2)
{
  if (event == NXEVENT_SYNCHED)
    {
      g_synched = true;
    }
}

static const struct nx_callback_s g_cb =
{
  drag_redraw, drag_position,
#ifdef CONFIG_NX_XYINPUT
  NULL,
#endif
#ifdef CONFIG_NX_KBD
  NULL,
#endif
  drag_event
};

static int drag_synch(NXHANDLE hnx, NXTKWINDOW hwnd)
{
  g_synched = false;
  if (nxtk_synch(hwnd, NULL) < 0)
    {
      return -1;
    }

  while (!g_synched)
    {
      if (nx_eventhandler(hnx) < 0)
        {
          return -1;
        }
    }

  return 0;
}

static uint32_t now_us(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
}

int main(int argc, FAR char *argv[])
{
  bool scroll = argc > 1 && strcmp(argv[1], "scroll") == 0;
  bool text = argc > 1 && strcmp(argv[1], "text") == 0;
  int mode = scroll || text;
  int steps = argc > 1 + mode ? atoi(argv[1 + mode]) : text ? 10 : 60;
  int dx    = argc > 2 + mode ? atoi(argv[2 + mode]) : scroll ? 16 : 8;
  int dy    = argc > 3 ? atoi(argv[3]) : 4;
  struct nxgl_size_s size =
  {
    argc > 4 ? atoi(argv[4]) : 500, argc > 5 ? atoi(argv[5]) : 350
  };

  struct nxgl_point_s pos =
  {
    40, 40
  };

  struct nxgl_rect_s rect;
  nxgl_mxpixel_t color[CONFIG_NX_NPLANES];
  NXTKWINDOW hwnd;
  NXHANDLE hnx;
  uint32_t t0;
  uint32_t us;
  int i;

  hnx = nx_connect();
  if (hnx == NULL)
    {
      printf("nxdrag: no NX server\n");
      return EXIT_FAILURE;
    }

  /* The first message is the CONNECTED handshake */

  if (nx_eventhandler(hnx) < 0)
    {
      printf("nxdrag: connect failed\n");
      nx_disconnect(hnx);
      return EXIT_FAILURE;
    }

  hwnd = nxtk_openwindow(hnx, NXBE_WINDOW_RAMBACKED, &g_cb, NULL);
  if (hwnd == NULL)
    {
      printf("nxdrag: open window failed\n");
      nx_disconnect(hnx);
      return EXIT_FAILURE;
    }

  nxtk_setsize(hwnd, &size);
  nxtk_setposition(hwnd, &pos);
  nxtk_raise(hwnd);

  /* Content: stripes, so that a wrong move shows */

  for (i = 0; i < 8; i++)
    {
      rect.pt1.x = 0;
      rect.pt1.y = i * size.h / 8;
      rect.pt2.x = size.w - 1;
      rect.pt2.y = (i + 1) * size.h / 8 - 1;
      color[0]   = (i & 1) ? 0xf800 >> (i / 2) : 0x07e0 >> (i / 2);
      nxtk_fillwindow(hwnd, &rect, color);
    }

  drag_synch(hnx, hwnd);

  if (text)
    {
      static uint16_t glyph[16][8];
      FAR const void *src[CONFIG_NX_NPLANES];
      struct nxgl_point_s origin;
      int cols = size.w / 8;
      int rows = size.h / 16;
      int n = 0;
      int r;
      int c;

      src[0] = glyph;
      t0 = now_us();
      for (i = 0; i < steps; i++)
        {
          for (r = 0; r < rows; r++)
            {
              for (c = 0; c < cols; c++, n++)
                {
                  int gy;
                  int gx;

                  /* A different two-colour "character" each cell */

                  for (gy = 0; gy < 16; gy++)
                    {
                      for (gx = 0; gx < 8; gx++)
                        {
                          glyph[gy][gx] = ((n * 2654435761u >> (gy + gx)) & 1)
                                          ? 0xffff : 0x0010;
                        }
                    }

                  rect.pt1.x = c * 8;
                  rect.pt1.y = r * 16;
                  rect.pt2.x = rect.pt1.x + 7;
                  rect.pt2.y = rect.pt1.y + 15;
                  origin     = rect.pt1;
                  nxtk_bitmapwindow(hwnd, &rect, src, &origin, 16);
                }
            }

          if (drag_synch(hnx, hwnd) < 0)
            {
              break;
            }
        }

      us = now_us() - t0;
      printf("nxdrag: %d screens of %dx%d glyphs: %lu ms, %lu us per glyph\n",
             i, cols, rows, (unsigned long)(us / 1000),
             (unsigned long)(us / (n ? n : 1)));
      nxtk_closewindow(hwnd);
      nx_disconnect(hnx);
      return EXIT_SUCCESS;
    }

  t0 = now_us();
  for (i = 0; i < steps; i++)
    {
      if (scroll)
        {
          /* Up one line (dx is the line height here), then a new line */

          struct nxgl_point_s up =
          {
            0, -dx
          };

          rect.pt1.x = 0;
          rect.pt1.y = dx;
          rect.pt2.x = size.w - 1;
          rect.pt2.y = size.h - 1;
          nxtk_movewindow(hwnd, &rect, &up);

          rect.pt1.y = size.h - dx;
          color[0]   = (i & 1) ? 0x001f : 0xffe0;
          nxtk_fillwindow(hwnd, &rect, color);
        }
      else
        {
          pos.x += dx;
          pos.y += dy;
          nxtk_setposition(hwnd, &pos);
        }

      if (drag_synch(hnx, hwnd) < 0)
        {
          printf("nxdrag: synch failed\n");
          break;
        }
    }

  us = now_us() - t0;
  printf("nxdrag: %d %s (%d,%d), %dx%d window: %lu ms, %lu us each\n",
         i, scroll ? "scrolls of" : "steps of", scroll ? 0 : dx,
         scroll ? dx : dy, size.w, size.h, (unsigned long)(us / 1000),
         (unsigned long)(us / (i ? i : 1)));

  nxtk_closewindow(hwnd);
  nx_disconnect(hnx);
  return EXIT_SUCCESS;
}
