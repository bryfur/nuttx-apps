/****************************************************************************
 * apps/examples/nxdrag/nxdrag_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Window drag benchmark: open a RAM-backed framed window on the running NX
 * server (as twm4nx's windows are), put it on top, then move it N steps of
 * (dx, dy), each followed by nxtk_synch() so that the step is timed until
 * the server has finished it.
 *
 *   nxdrag [steps [dx [dy [w [h]]]]]     defaults: 60 8 4 500 350
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
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
  int steps = argc > 1 ? atoi(argv[1]) : 60;
  int dx    = argc > 2 ? atoi(argv[2]) : 8;
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

  t0 = now_us();
  for (i = 0; i < steps; i++)
    {
      pos.x += dx;
      pos.y += dy;
      nxtk_setposition(hwnd, &pos);
      if (drag_synch(hnx, hwnd) < 0)
        {
          printf("nxdrag: synch failed\n");
          break;
        }
    }

  us = now_us() - t0;
  printf("nxdrag: %d steps of (%d,%d), %dx%d window: %lu ms, "
         "%lu us per step\n", i, dx, dy, size.w, size.h,
         (unsigned long)(us / 1000), (unsigned long)(us / (i ? i : 1)));

  nxtk_closewindow(hwnd);
  nx_disconnect(hnx);
  return EXIT_SUCCESS;
}
