/****************************************************************************
 * apps/graphics/fpcdesk/deskctl_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * deskctl: drive the desktop without a mouse or keyboard (tests, demos, the
 * simulator without X11).  Input goes to the NX server through a
 * connection of its own, as the desktop session's would; the GPU's cursor
 * follows the pointer.
 *
 *   deskctl m X Y [BUTTONS]       move the pointer (buttons: 1 left, 2
 *                                 middle, 4 right; default none)
 *   deskctl c X Y                 click there
 *   deskctl d X0 Y0 X1 Y1 [N]     drag with the left button, in N steps
 *   deskctl k TEXT                type (\n is Enter)
 *   deskctl shot FILE             the screen as a PPM file (simulator)
 *   deskctl test                  BLEND on the GPU, checked pixel by pixel
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <nuttx/nx/nx.h>

/* The GPU's cursor and the simulator's screenshot are kernel functions of
 * the board (a flat build)
 */

#ifdef CONFIG_FPC_GPU
void fpc_gpu_cursor_pos(int x, int y, bool visible);
void fpc_gpu_flush(void);
#endif
#ifdef CONFIG_ARCH_SIM
int fpc_simgpu_screenshot(FAR const char *path);
#endif
int desk_gputest(void);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static NXHANDLE deskctl_connect(void)
{
  NXHANDLE h = nx_connect();

  if (h == NULL)
    {
      fprintf(stderr, "deskctl: no NX server: %d\n", errno);
      return NULL;
    }

  /* The CONNECTED event */

  if (nx_eventhandler(h) < 0)
    {
      fprintf(stderr, "deskctl: not connected: %d\n", errno);
      nx_disconnect(h);
      return NULL;
    }

  return h;
}

static void deskctl_pointer(NXHANDLE h, int x, int y, int buttons)
{
#ifdef CONFIG_FPC_GPU
  fpc_gpu_cursor_pos(x, y, true);
  fpc_gpu_flush();
#endif
  nx_mousein(h, x, y, (uint8_t)buttons);
}

static void deskctl_type(NXHANDLE h, FAR const char *text)
{
  uint8_t buf[64];
  size_t n = 0;

  for (; *text != '\0'; text++)
    {
      uint8_t ch = (uint8_t)*text;

      if (text[0] == '\\' && text[1] == 'n')
        {
          ch = '\n';
          text++;
        }

      buf[n++] = ch;
      if (n == sizeof(buf))
        {
          nx_kbdin(h, (uint8_t)n, buf);
          n = 0;
        }
    }

  if (n > 0)
    {
      nx_kbdin(h, (uint8_t)n, buf);
    }
}

static void deskctl_usage(void)
{
  fprintf(stderr,
          "usage: deskctl m X Y [BUTTONS] | c X Y | d X0 Y0 X1 Y1 [N] |\n"
          "               k TEXT | shot FILE\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  NXHANDLE h;
  int ret = EXIT_SUCCESS;

  if (argc < 2)
    {
      deskctl_usage();
      return EXIT_FAILURE;
    }

#ifdef CONFIG_FPC_GPU
  if (strcmp(argv[1], "test") == 0)
    {
      return desk_gputest() ? EXIT_FAILURE : EXIT_SUCCESS;
    }
#endif

  if (strcmp(argv[1], "shot") == 0)
    {
#ifdef CONFIG_ARCH_SIM
      if (argc < 3 || fpc_simgpu_screenshot(argv[2]) < 0)
        {
          fprintf(stderr, "deskctl: no screenshot\n");
          return EXIT_FAILURE;
        }

      return EXIT_SUCCESS;
#else
      fprintf(stderr, "deskctl: screenshots only in the simulator\n");
      return EXIT_FAILURE;
#endif
    }

  h = deskctl_connect();
  if (h == NULL)
    {
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "m") == 0 && argc >= 4)
    {
      deskctl_pointer(h, atoi(argv[2]), atoi(argv[3]),
                      argc >= 5 ? atoi(argv[4]) : 0);
    }
  else if (strcmp(argv[1], "c") == 0 && argc >= 4)
    {
      int x = atoi(argv[2]);
      int y = atoi(argv[3]);

      deskctl_pointer(h, x, y, 0);
      usleep(30000);
      deskctl_pointer(h, x, y, NX_MOUSE_LEFTBUTTON);
      usleep(30000);
      deskctl_pointer(h, x, y, 0);
    }
  else if (strcmp(argv[1], "d") == 0 && argc >= 6)
    {
      int x0 = atoi(argv[2]);
      int y0 = atoi(argv[3]);
      int x1 = atoi(argv[4]);
      int y1 = atoi(argv[5]);
      int n  = argc >= 7 ? atoi(argv[6]) : 8;
      int i;

      if (n < 1)
        {
          n = 1;
        }

      deskctl_pointer(h, x0, y0, 0);
      usleep(30000);
      deskctl_pointer(h, x0, y0, NX_MOUSE_LEFTBUTTON);
      for (i = 1; i <= n; i++)
        {
          usleep(30000);
          deskctl_pointer(h, x0 + (x1 - x0) * i / n, y0 + (y1 - y0) * i / n,
                          NX_MOUSE_LEFTBUTTON);
        }

      usleep(30000);
      deskctl_pointer(h, x1, y1, 0);
    }
  else if (strcmp(argv[1], "k") == 0 && argc >= 3)
    {
      deskctl_type(h, argv[2]);
    }
  else
    {
      deskctl_usage();
      ret = EXIT_FAILURE;
    }

  /* Let the server take the messages before the connection goes */

  usleep(50000);
  nx_disconnect(h);
  return ret;
}
