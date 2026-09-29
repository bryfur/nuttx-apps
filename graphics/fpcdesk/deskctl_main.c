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
 *   deskctl c X Y [N]             click there (N times: 2 a double click)
 *   deskctl d X0 Y0 X1 Y1 [N]     drag with the left button, in N steps
 *   deskctl k TEXT                type (\n Enter, \t Tab, \e Escape,
 *                                 \b Backspace, \xHH any byte)
 *   deskctl key EVENT...          key events (nuttx/nx/nxkeys.h), as a
 *                                 keyboard sends them: +NAME press, -NAME
 *                                 release, NAME both, @MS wait MS ms.
 *                                 NAME: a character (a, 1, -), space,
 *                                 enter, esc, tab, bs, up, down, left,
 *                                 right, home, end, pgup, pgdn, ins, del,
 *                                 f1-f12, shift, ctrl, alt, gui (left),
 *                                 rshift, rctrl, ralt, rgui, vol+, vol-,
 *                                 mute (these through /dev/kbd when there
 *                                 is one to write to, the headless
 *                                 simulator's: as a keyboard has them)
 *   deskctl shot FILE             the screen as a PPM file (simulator)
 *   deskctl test                  BLEND on the GPU, checked pixel by pixel
 *   deskctl stats [clear|N]       GPU commands and pixels since the last
 *                                 clear, with the GPU time they take (the
 *                                 costs in docs/desktop.md); N: per N steps
 *   deskctl mode [N]              the display mode (N: change it)
 *   deskctl redraw                composite all of the display again (a
 *                                 check: the result must not change)
 *   deskctl freeze [SECONDS|off]  stop the desktop's clock at SECONDS
 *                                 since 1970 (default 2026-01-02 09:41
 *                                 UTC), the cursors' blinking and the
 *                                 games' clocks, and seed the games the
 *                                 same way every time, for screenshots
 *                                 that do not depend on when they are
 *                                 taken; off: let them run
 *   deskctl dock APP [r]          click APP's icon in the dock (r: with
 *                                 the right button), wherever the dock
 *                                 has it; APP: a part of its name
 *   deskctl wins                  the windows, the top one first: the
 *                                 application, position, size, state
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <strings.h>

#include <nuttx/nx/nx.h>
#include <nuttx/nx/nxcompose.h>
#include <nuttx/input/keyboard.h>
#include <nuttx/nx/nxkeys.h>

#include "desk.h"

/* The GPU's cursor and the simulator's screenshot are kernel functions of
 * the board (a flat build)
 */

#ifdef CONFIG_FPC_GPU
void fpc_gpu_cursor_pos(int x, int y, bool visible);
void fpc_gpu_flush(void);

/* As fpc_gpu.h has them */

struct fpc_gpu_stats_s
{
  uint32_t cmds[16];
  uint64_t pixels[16];
};

void fpc_gpu_stats(struct fpc_gpu_stats_s *st, bool clear);
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
  /* In pieces: the server's message to the client carries the bytes and
   * must fit in NX_MXCLIMSGLEN (64) with its header
   */

  uint8_t buf[32];
  size_t n = 0;

  for (; *text != '\0'; text++)
    {
      uint8_t ch = (uint8_t)*text;

      /* \n Enter, \t Tab, \e Escape (then e.g. [A: up), \b Backspace,
       * \xHH a byte
       */

      if (text[0] == '\\' && text[1] != '\0')
        {
          text++;
          switch (*text)
            {
              case 'n':
                ch = '\n';
                break;

              case 't':
                ch = '\t';
                break;

              case 'e':
                ch = 0x1b;
                break;

              case 'b':
                ch = 0x7f;
                break;

              case 'x':
                if (text[1] != '\0' && text[2] != '\0')
                  {
                    char hex[3] = { text[1], text[2], '\0' };

                    ch = (uint8_t)strtol(hex, NULL, 16);
                    text += 2;
                  }
                break;

              default:
                ch = (uint8_t)*text;
                break;
            }
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

/* Key events: names of the keys that type no character */

static const struct
{
  const char *name;
  uint8_t code;
  uint8_t ch;
  uint8_t mod;
} g_keynames[] =
{
  { "space",  ' ',                              ' ',  0 },
  { "enter",  NX_KEY_ENTER,                     '\n', 0 },
  { "esc",    NX_KEY_ESCAPE,                    0x1b, 0 },
  { "tab",    NX_KEY_TAB,                       '\t', 0 },
  { "bs",     NX_KEY_BACKSPACE,                 0x7f, 0 },
  { "up",     NX_KEY_SPECIAL(KEYCODE_UP),       0,    0 },
  { "down",   NX_KEY_SPECIAL(KEYCODE_DOWN),     0,    0 },
  { "left",   NX_KEY_SPECIAL(KEYCODE_LEFT),     0,    0 },
  { "right",  NX_KEY_SPECIAL(KEYCODE_RIGHT),    0,    0 },
  { "home",   NX_KEY_SPECIAL(KEYCODE_HOME),     0,    0 },
  { "end",    NX_KEY_SPECIAL(KEYCODE_END),      0,    0 },
  { "pgup",   NX_KEY_SPECIAL(KEYCODE_PAGEUP),   0,    0 },
  { "pgdn",   NX_KEY_SPECIAL(KEYCODE_PAGEDOWN), 0,    0 },
  { "ins",    NX_KEY_SPECIAL(KEYCODE_INSERT),   0,    0 },
  { "del",    NX_KEY_SPECIAL(KEYCODE_FWDDEL),   0,    0 },
  { "pause",  NX_KEY_SPECIAL(KEYCODE_PAUSE),    0,    0 },
  { "shift",  NX_KEY_SPECIAL(KEYCODE_LSHIFT),   0,    NX_KEY_SHIFT },
  { "rshift", NX_KEY_SPECIAL(KEYCODE_RSHIFT),   0,    NX_KEY_SHIFT },
  { "ctrl",   NX_KEY_SPECIAL(KEYCODE_LCTRL),    0,    NX_KEY_CTRL },
  { "rctrl",  NX_KEY_SPECIAL(KEYCODE_RCTRL),    0,    NX_KEY_CTRL },
  { "alt",    NX_KEY_SPECIAL(KEYCODE_LALT),     0,    NX_KEY_ALT },
  { "ralt",   NX_KEY_SPECIAL(KEYCODE_RALT),     0,    NX_KEY_ALT },
  { "gui",    NX_KEY_SPECIAL(KEYCODE_LGUI),     0,    NX_KEY_GUI },
  { "rgui",   NX_KEY_SPECIAL(KEYCODE_RGUI),     0,    NX_KEY_GUI },
  { "vol+",   NX_KEY_SPECIAL(KEYCODE_VOLUP),    0,    0 },
  { "vol-",   NX_KEY_SPECIAL(KEYCODE_VOLDOWN),  0,    0 },
  { "mute",   NX_KEY_SPECIAL(KEYCODE_MUTE),     0,    0 },
};

/* A volume key through the keyboard itself (/dev/kbd, the headless
 * simulator's FPC_SIM_VKBD), where the desktop session takes it for the
 * mixer; false if there is none
 */

static bool deskctl_kbddev(uint8_t code, bool release)
{
  struct keyboard_event_s ev;
  bool ok;
  int fd;

  if (code != NX_KEY_SPECIAL(KEYCODE_VOLUP) &&
      code != NX_KEY_SPECIAL(KEYCODE_VOLDOWN) &&
      code != NX_KEY_SPECIAL(KEYCODE_MUTE))
    {
      return false;
    }

  fd = open("/dev/kbd", O_WRONLY);
  if (fd < 0)
    {
      return false;
    }

  ev.code = NX_KEY_KEYCODE(code);
  ev.type = release ? KEYBOARD_SPECREL : KEYBOARD_SPECPRESS;
  ok = write(fd, &ev, sizeof(ev)) == sizeof(ev);
  close(fd);
  return ok;
}

static void deskctl_keyev(NXHANDLE h, uint8_t *mods, uint8_t code,
                          uint8_t ch, uint8_t mod, bool release)
{
  uint8_t rec[NX_KEY_SIZE];
  struct nx_key_s key;

  if (deskctl_kbddev(code, release))
    {
      return;
    }

  *mods = release ? *mods & ~mod : *mods | mod;

  /* What the key types with the modifiers held, as a US keyboard */

  if (!release && ch >= 'a' && ch <= 'z')
    {
      if (*mods & NX_KEY_CTRL)
        {
          ch &= 0x1f;
        }
      else if (*mods & NX_KEY_SHIFT)
        {
          ch -= 'a' - 'A';
        }
    }

  key.flags = *mods | (release ? NX_KEY_RELEASE : 0);
  key.code  = code;
  key.ch    = release ? 0 : ch;
  nx_key_encode(rec, &key);
  nx_kbdin(h, NX_KEY_SIZE, rec);
}

static int deskctl_key(NXHANDLE h, uint8_t *mods, FAR const char *a)
{
  bool press = true;
  bool release = true;
  uint8_t code = 0;
  uint8_t ch = 0;
  uint8_t mod = 0;
  size_t k;

  if (a[0] == '@')
    {
      usleep(atoi(a + 1) * 1000);
      return 0;
    }

  if ((a[0] == '+' || a[0] == '-') && a[1] != '\0')
    {
      press   = a[0] == '+';
      release = a[0] == '-';
      a++;
    }

  if (a[1] == '\0')
    {
      code = nx_key_code((uint8_t)a[0], false);
      ch   = (uint8_t)a[0];
    }
  else if (a[0] == 'f' && atoi(a + 1) >= 1 && atoi(a + 1) <= 12)
    {
      code = NX_KEY_SPECIAL(KEYCODE_F1 + atoi(a + 1) - 1);
    }
  else
    {
      for (k = 0; k < sizeof(g_keynames) / sizeof(g_keynames[0]); k++)
        {
          if (strcmp(a, g_keynames[k].name) == 0)
            {
              code = g_keynames[k].code;
              ch   = g_keynames[k].ch;
              mod  = g_keynames[k].mod;
              break;
            }
        }

      if (code == 0)
        {
          fprintf(stderr, "deskctl: no key %s\n", a);
          return -1;
        }
    }

  if (press)
    {
      deskctl_keyev(h, mods, code, ch, mod, false);
    }

  if (press && release)
    {
      usleep(30000);
    }

  if (release)
    {
      deskctl_keyev(h, mods, code, ch, mod, true);
    }

  return 0;
}

/* Events are arguments, or words of one (NSH takes few arguments) */

static int deskctl_keys(NXHANDLE h, int argc, FAR char *argv[])
{
  uint8_t mods = 0;
  FAR char *save;
  FAR char *a;
  int i;

  for (i = 0; i < argc; i++)
    {
      for (a = strtok_r(argv[i], " ", &save); a != NULL;
           a = strtok_r(NULL, " ", &save))
        {
          if (deskctl_key(h, &mods, a) < 0)
            {
              return -1;
            }
        }
    }

  return 0;
}

#ifdef CONFIG_FPC_GPU
/* GPU time per pixel, ns, from docs/desktop.md (500 x 350: FILL 1.70 ms,
 * COPY 3.34 ms, BLEND of a colour 3.5 ms, of an image 4.75 ms); PUT and
 * EXPAND are link-bound (about 25 MB/s, and 1/16 of that data)
 */

static const struct
{
  uint8_t op;
  const char *name;
  uint32_t pspx;                 /* ps per pixel */
} g_costs[] =
{
  { 0x01, "FILL",   9714 },
  { 0x03, "COPY",   19086 },
  { 0x09, "BLEND",  20000 },
  { 0x0f, "BLENDi", 27143 },
  { 0x02, "PUT",    80000 },
  { 0x07, "EXPAND", 5000 },
  { 0x08, "READ",   0 },
  { 0x0c, "FLIP",   0 },
  { 0x0b, "MODE",   0 },
};

static void deskctl_stats(FAR const char *arg)
{
  struct fpc_gpu_stats_s st;
  int per = arg != NULL ? atoi(arg) : 0;
  uint64_t total = 0;
  uint64_t px = 0;
  uint32_t cmds = 0;
  size_t i;

  if (arg != NULL && strcmp(arg, "clear") == 0)
    {
      fpc_gpu_stats(NULL, true);
      return;
    }

  fpc_gpu_flush();
  fpc_gpu_stats(&st, false);
  if (per < 1)
    {
      per = 1;
    }

  printf("op        cmds     pixels  GPU ms%s\n",
         per > 1 ? "   (per step)" : "");
  for (i = 0; i < sizeof(g_costs) / sizeof(g_costs[0]); i++)
    {
      uint32_t n = st.cmds[g_costs[i].op];
      uint64_t p = st.pixels[g_costs[i].op];
      uint64_t us = p * g_costs[i].pspx / 1000000 / per;

      if (n == 0)
        {
          continue;
        }

      printf("%-7s %6lu %10lu %4lu.%02lu\n", g_costs[i].name,
             (unsigned long)(n / per), (unsigned long)(p / per),
             (unsigned long)(us / 1000), (unsigned long)(us % 1000 / 10));
      total += us;
      px    += p;
      cmds  += n;
    }

  printf("total   %6lu %10lu %4lu.%02lu\n", (unsigned long)(cmds / per),
         (unsigned long)(px / per), (unsigned long)(total / 1000),
         (unsigned long)(total % 1000 / 10));
}
#endif

/* The desktop's state is read from here, another task of the flat build,
 * without a lock: for tests, once the desktop has settled
 */

static int deskctl_app(FAR const char *name)
{
  int i;

  for (i = 0; i < APP_COUNT; i++)
    {
      FAR const char *n = g_apps[i]->name;
      size_t len = strlen(name);

      for (; *n != '\0'; n++)
        {
          if (strncasecmp(n, name, len) == 0)
            {
              return i;
            }
        }
    }

  return -1;
}

static void deskctl_wins(void)
{
  FAR struct desk_win_s *w;

  for (w = g_desk.wins; w != NULL; w = w->next)
    {
      if (w->closing)
        {
          continue;
        }

      printf("%-14s %4d,%-4d %4dx%-4d %s%s%s\"%s\"\n", g_apps[w->app]->name,
             w->pos.x, w->pos.y, w->size.w, w->size.h,
             w->active ? "active " : "", w->minimized ? "minimized " : "",
             w->maximized ? "maximized " : "", w->title);
    }
}

static void deskctl_usage(void)
{
  fprintf(stderr,
          "usage: deskctl m X Y [BUTTONS] | c X Y | d X0 Y0 X1 Y1 [N] |\n"
          "               k TEXT | key [+|-]NAME|@MS... | shot FILE |\n"
          "               stats [clear|N] | mode [N] | redraw | test |\n"
          "               freeze [SECONDS|off] | dock APP [r] | wins\n");
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

  if (strcmp(argv[1], "stats") == 0)
    {
      deskctl_stats(argc >= 3 ? argv[2] : NULL);
      return EXIT_SUCCESS;
    }
#endif

  if (strcmp(argv[1], "freeze") == 0)
    {
      time_t t = 1767346860;         /* Fri 2026-01-02 09:41 UTC */

      if (argc >= 3)
        {
          t = strcmp(argv[2], "off") == 0 ? 0 : (time_t)atoll(argv[2]);
        }

      desk_freeze(t);
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "wins") == 0)
    {
      deskctl_wins();
      return EXIT_SUCCESS;
    }

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
      int n = argc >= 5 ? atoi(argv[4]) : 1;

      deskctl_pointer(h, x, y, 0);
      while (n-- > 0)
        {
          usleep(30000);
          deskctl_pointer(h, x, y, NX_MOUSE_LEFTBUTTON);
          usleep(30000);
          deskctl_pointer(h, x, y, 0);
        }
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
  else if (strcmp(argv[1], "mode") == 0)
    {
      struct nxgl_size_s size;
      int mode;
      int i;

      if (argc >= 3)
        {
          mode = nx_setdisplaymode(h, atoi(argv[2]));
          if (mode < 0)
            {
              fprintf(stderr, "deskctl: mode %s: %d\n", argv[2], mode);
              ret = EXIT_FAILURE;
            }
        }

      for (i = 0; nx_getdisplaymode(h, i, &size) >= 0; i++)
        {
          printf("%c %d  %d x %d\n",
                 nx_getdisplaymode(h, -1, NULL) == i ? '*' : ' ', i,
                 size.w, size.h);
        }
    }
  else if (strcmp(argv[1], "dock") == 0 && argc >= 3)
    {
      int app = deskctl_app(argv[2]);
      int button = argc >= 4 && argv[3][0] == 'r' ? NX_MOUSE_RIGHTBUTTON :
                                                    NX_MOUSE_LEFTBUTTON;
      int x;
      int y;

      if (app < 0 || !desk_dock_where(app, &x, &y))
        {
          fprintf(stderr, "deskctl: %s is not in the dock\n", argv[2]);
          ret = EXIT_FAILURE;
        }
      else
        {
          deskctl_pointer(h, x, y, 0);
          usleep(30000);
          deskctl_pointer(h, x, y, button);
          usleep(30000);
          deskctl_pointer(h, x, y, 0);
        }
    }
  else if (strcmp(argv[1], "redraw") == 0)
    {
      /* A new background colour damages all of the display (the
       * wallpaper hides it); then back to the theme's
       */

      nxgl_mxpixel_t c = g_theme.desktop ^ 0x0841;

      nx_setbgcolor(h, &c);
      usleep(50000);
      c = g_theme.desktop;
      nx_setbgcolor(h, &c);
    }
  else if (strcmp(argv[1], "k") == 0 && argc >= 3)
    {
      deskctl_type(h, argv[2]);
    }
  else if (strcmp(argv[1], "key") == 0 && argc >= 3)
    {
      if (deskctl_keys(h, argc - 2, &argv[2]) < 0)
        {
          ret = EXIT_FAILURE;
        }
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
