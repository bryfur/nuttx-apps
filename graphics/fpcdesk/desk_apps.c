/****************************************************************************
 * apps/graphics/fpcdesk/desk_apps.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The applications: the table of them, the icon tile they share, and the
 * small ones (the launcher's entry, About); the settings kept across boots.
 * The others have a file each: desk_term.c, desk_files.c, desk_edit.c,
 * desk_viewer.c, desk_paint.c, desk_monitor.c, desk_settings.c, desk_log.c,
 * desk_calc.c, desk_tetris.c, desk_solitaire.c, desk_breakout.c,
 * desk_game.c (Doom), desk_clock.c, desk_calendar.c, desk_bench.c (GPU
 * Bench), desk_chess.c, desk_games.c (the full-screen games).
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/boardctl.h>
#include <sys/stat.h>

#include <nuttx/nx/nxfonts.h>

#include "nshlib/nshlib.h"
#include "desk.h"
#include "desk_clock.h"
#ifdef CONFIG_FPCDESK_SOUND
#  include "desk_sound.h"
#endif

static struct desk_ops_s g_o;

/****************************************************************************
 * Public Functions: icons
 ****************************************************************************/

/* A round tile with a top-to-bottom gradient, a hairline of light at the
 * top: the base of every icon
 */

void icon_tile(struct desk_ops_s *o, int x, int y, int s,
                      uint16_t top, uint16_t bottom, uint8_t a)
{
  ops_rrect_vgrad(o, x, y, s, s, LUT_R12, 12, top, bottom, a);
  ops_blend(o, x + 10, y, s - 20, 1, C_WHITE, (uint8_t)(a * 60 / 255));
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void icon_apps(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  int i;
  int j;

  icon_tile(o, x, y, s, RGB(0x6a, 0x70, 0x80), RGB(0x3a, 0x3e, 0x4a), a);
  for (j = 0; j < 3; j++)
    {
      for (i = 0; i < 3; i++)
        {
          int cx = x + s / 2 + (i - 1) * s / 4;
          int cy = y + s / 2 + (j - 1) * s / 4;

          ops_text_centre(o, &g_desk_font_title, cx,
                          cy - text_height(&g_desk_font_title) / 2 - 1,
                          DESK_CH_BULLET, C_WHITE, a);
        }
    }
}

static void icon_about(struct desk_ops_s *o, int x, int y, int s,
                       uint8_t a)
{
  const struct desk_font_s *f = s >= 64 ? &g_desk_font_title :
                                          &g_desk_font_uib;

  icon_tile(o, x, y, s, RGB(0x4a, 0x8c, 0xff), RGB(0x1c, 0x48, 0xc8), a);
  ops_text_centre(o, f, x + s / 2, y + s / 2 - text_height(f) / 2, "FPC",
                  C_WHITE, a);
}

/* About this computer: the About window, and Settings' last page.  At
 * (x, y) in hwnd (frame coordinates), w wide.
 */

void desk_about_draw(NXWINDOW hwnd, int x, int y, int w)
{
  static char display[40];
  static char graphics[40];
  static const char * const rows[][2] =
  {
    { "Processor", "RP2350, 2 x Cortex-M33" },
    { "Memory",    "520 KB SRAM, 8 MB PSRAM" },
    { "Graphics",  graphics },
    { "Display",   display },
    { "System",    "NuttX, NX" },
  };

  int cx = x + w / 2;
  int i;

  snprintf(display, sizeof(display), "%d x %d, %s", DESK_W, DESK_H,
           g_desk.composited ? "composited" : "not composited");
  snprintf(graphics, sizeof(graphics), "Tang Nano 20K, GPU protocol v%d",
           (nx_compositor(g_desk.hnx) & NX_CAP_BC1) != 0 ? 6 : 5);

  ops_reset(&g_o);
  icon_about(&g_o, cx - 36, y + 22, 72, 255);
  ops_text_centre(&g_o, &g_desk_font_title, cx, y + 106, "FPC",
                  C_TEXT, 255);
  ops_text_centre(&g_o, &g_desk_font_ui, cx, y + 134,
                  "A small computer", C_TEXT_DIM, 255);
  ops_draw(hwnd, &g_o);

  for (i = 0; i < 5; i++)
    {
      int ry = y + 170 + i * 24;

      ops_text(&g_o, &g_desk_font_ui, cx - 12 -
               text_width(&g_desk_font_ui, rows[i][0]), ry, rows[i][0],
               C_TEXT_DIM, 255);
      ops_text(&g_o, &g_desk_font_ui, cx + 4, ry, rows[i][1], C_TEXT,
               255);
      ops_draw(hwnd, &g_o);
    }
}

static void about_draw(struct desk_win_s *w)
{
  ops_reset(&g_o);
  ops_fill(&g_o, 0, TITLE_H, w->csize.w, w->csize.h, C_WINDOW_BG);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
  desk_about_draw((NXWINDOW)w->hwnd, 0, TITLE_H, w->csize.w);
}

static struct desk_win_s *about_open(void)
{
  struct desk_win_s *w = desk_win_open(APP_ABOUT, "About", 400, 300);

  if (w != NULL)
    {
      about_draw(w);
    }

  return w;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

static const struct desk_app_s g_app_launcher =
{
  .name = "Applications",
  .icon = icon_apps,
};

static const struct desk_app_s g_app_about =
{
  .name   = "About",
  .icon   = icon_about,
  .open   = about_open,
  .draw   = about_draw,
  .single = true,
};

const struct desk_app_s * const g_apps[APP_COUNT] =
{
  [APP_LAUNCHER] = &g_app_launcher,
  [APP_TERMINAL] = &g_app_terminal,
  [APP_FILES]    = &g_app_files,
  [APP_EDITOR]   = &g_app_editor,
  [APP_VIEWER]   = &g_app_viewer,
  [APP_PAINT]    = &g_app_paint,
#ifdef CONFIG_FPCDESK_MUSIC
  [APP_MUSIC]    = &g_app_music,
#endif
  [APP_MONITOR]  = &g_app_monitor,
  [APP_CALC]     = &g_app_calc,
  [APP_TETRIS]   = &g_app_tetris,
  [APP_SOLITAIRE] = &g_app_solitaire,
  [APP_BREAKOUT] = &g_app_breakout,
#ifdef CONFIG_GAMES_NXDOOM_DESKTOP
  [APP_DOOM]     = &g_app_doom,
#endif
  [APP_SETTINGS] = &g_app_settings,
  [APP_ABOUT]    = &g_app_about,
  [APP_LOG]      = &g_app_log,
#ifdef CONFIG_FPCDESK_WIDGETS
  [APP_WIDGETS]  = &g_app_widgets,
#endif
  [APP_CLOCK]    = &g_app_clock,
  [APP_CALENDAR] = &g_app_calendar,
#ifdef CONFIG_FPCDESK_BENCH
  [APP_BENCH]    = &g_app_bench,
#endif
#ifdef CONFIG_FPCDESK_CHESS
  [APP_CHESS]    = &g_app_chess,
#endif
  [APP_SHEET]    = &g_app_sheet,
#ifdef CONFIG_FPCDESK_PYTHON
  [APP_PYTHON]   = &g_app_python,
#endif
#ifdef CONFIG_GAMES_THUNDERBOLT
  [APP_THUNDERBOLT] = &g_app_thunderbolt,
#endif
#ifdef CONFIG_GAMES_QUAKE
  [APP_QUAKE]    = &g_app_quake,
#endif
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* The settings kept across boots, in CONFIG_FPCDESK_PREFS, or .fpcdesk in
 * the home directory: a line each, "name value" (the dock's favourites by
 * their names, so that new applications do not upset them)
 */

static const char *prefs_path(char *buf, size_t len)
{
  const char *home;

  if (CONFIG_FPCDESK_PREFS[0] != '\0')
    {
      return CONFIG_FPCDESK_PREFS;
    }

  home = getenv("HOME");
  if (home == NULL)
    {
      home = CONFIG_LIBC_HOMEDIR;
    }

  snprintf(buf, len, "%s%s.fpcdesk", home,
           home[strlen(home) - 1] == '/' ? "" : "/");
  return buf;
}

/* The settings until there are any: the dock has the usual applications */

static void prefs_defaults(void)
{
  static const uint8_t pinned[] =
  {
    APP_TERMINAL, APP_FILES, APP_EDITOR, APP_MONITOR, APP_SETTINGS
  };

  memset(&g_prefs, 0, sizeof(g_prefs));
  g_prefs.theme     = THEME_DARK;
  g_prefs.accent    = ACCENT_BLUE;
  g_prefs.shadow    = SHADOW_NORMAL;
  g_prefs.border    = true;
  g_prefs.clock24   = true;
  g_prefs.clockdate = true;
  g_prefs.dockicon  = 48;
  g_prefs.dockpos   = DOCK_BOTTOM;
  g_prefs.repdelay  = 500;
  g_prefs.reprate   = 40;
  g_prefs.ptrspeed  = 100;
  g_prefs.effects   = true;
  g_prefs.fx        = FX_ALL;
  g_prefs.npinned   = sizeof(pinned);
  memcpy(g_prefs.pinned, pinned, sizeof(pinned));
}

static int app_by_name(const char *name)
{
  int i;

  for (i = 1; i < APP_COUNT; i++)
    {
      if (strcmp(g_apps[i]->name, name) == 0)
        {
          return i;
        }
    }

  return -1;
}

static int clamp(int v, int lo, int hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

int desk_prefs_load(void)
{
  char path[64];
  char line[64];
  bool pins = false;
  int mode = -1;
  int n;
  FILE *f;

  prefs_defaults();
  f = fopen(prefs_path(path, sizeof(path)), "r");
  if (f == NULL)
    {
      return -1;
    }

  while (fgets(line, sizeof(line), f) != NULL)
    {
      char *v = strchr(line, ' ');
      char *nl = strchr(line, '\n');

      if (nl != NULL)
        {
          *nl = '\0';
        }

      if (v == NULL)
        {
          continue;
        }

      *v++ = '\0';
      n = atoi(v);
      if (strcmp(line, "mode") == 0)
        {
          mode = n;
        }
      else if (strcmp(line, "wallpaper") == 0 && n >= 0 && n <= 3)
        {
          g_desk.wallstyle = n;
        }
      else if (strcmp(line, "theme") == 0)
        {
          g_prefs.theme = clamp(n, 0, THEME_COUNT - 1);
        }
      else if (strcmp(line, "accent") == 0)
        {
          g_prefs.accent = clamp(n, 0, ACCENT_COUNT - 1);
        }
      else if (strcmp(line, "shadow") == 0)
        {
          g_prefs.shadow = clamp(n, SHADOW_OFF, SHADOW_STRONG);
        }
      else if (strcmp(line, "border") == 0)
        {
          g_prefs.border = n != 0;
        }
      else if (strcmp(line, "effects") == 0)
        {
          g_prefs.effects = n != 0;          /* All of them (the master) */
        }
      else if (strcmp(line, "fx") == 0)      /* "fx NAME 0|1": each one */
        {
          char *on = strchr(v, ' ');
          int i;

          for (i = 0; on != NULL && i < FX_COUNT; i++)
            {
              if (strncmp(v, g_fx_names[i], on - v) == 0 &&
                  g_fx_names[i][on - v] == '\0')
                {
                  if (atoi(on + 1) != 0)
                    {
                      g_prefs.fx |= 1 << i;
                    }
                  else
                    {
                      g_prefs.fx &= ~(1 << i);
                    }
                }
            }
        }
      else if (strcmp(line, "clock24") == 0)
        {
          g_prefs.clock24 = n != 0;
        }
      else if (strcmp(line, "clockdate") == 0)
        {
          g_prefs.clockdate = n != 0;
        }
      else if (strcmp(line, "clocksecs") == 0)
        {
          g_prefs.clocksecs = n != 0;
        }
      else if (strcmp(line, "dockicon") == 0)
        {
          g_prefs.dockicon = clamp(n, DOCK_ICON_MIN, DOCK_ICON_MAX);
        }
      else if (strcmp(line, "dockpos") == 0)
        {
          g_prefs.dockpos = clamp(n, DOCK_BOTTOM, DOCK_RIGHT);
        }
      else if (strcmp(line, "dockhide") == 0)
        {
          g_prefs.dockhide = n != 0;
        }
      else if (strcmp(line, "repdelay") == 0)
        {
          g_prefs.repdelay = n == 0 ? 0 : clamp(n, 100, 2000);
        }
      else if (strcmp(line, "reprate") == 0)
        {
          g_prefs.reprate = clamp(n, 10, 500);
        }
      else if (strcmp(line, "ptrspeed") == 0)
        {
          g_prefs.ptrspeed = clamp(n, 10, 1000);
        }
      else if (strcmp(line, "tzoff") == 0)
        {
          g_prefs.tzoff = clamp(n, -12 * 60, 14 * 60);
        }
      else if (strcmp(line, "dock") == 0)
        {
          int app = app_by_name(v);

          if (!pins)
            {
              g_prefs.npinned = 0;                 /* The file's instead */
              pins = true;
            }

          if (app > 0 && g_prefs.npinned < APP_COUNT)
            {
              g_prefs.pinned[g_prefs.npinned++] = app;
            }
        }
      else if (strcmp(line, "nodock") == 0)
        {
          g_prefs.npinned = 0;
          pins = true;
        }
#ifdef CONFIG_FPCDESK_SOUND
      else if (strcmp(line, "sound") == 0)
        {
          desk_sound_prefs_line(v);
        }
#endif
    }

  fclose(f);
  return mode;
}

void desk_prefs_save(void)
{
  char path[64];
  const char *p = prefs_path(path, sizeof(path));
  char dir[64];
  char *slash;
  FILE *f;
  int i;

  /* The directory may not exist yet (a new SD card) */

  snprintf(dir, sizeof(dir), "%s", p);
  slash = strrchr(dir, '/');
  if (slash != NULL && slash != dir)
    {
      *slash = '\0';
      mkdir(dir, 0777);
    }

  f = fopen(p, "w");
  if (f == NULL)
    {
      fprintf(stderr, "desk: cannot save %s: %d\n", p, errno);
      return;
    }

  fprintf(f, "mode %d\nwallpaper %d\n",
          nx_getdisplaymode(g_desk.hnx, -1, NULL), g_desk.wallstyle);
  fprintf(f, "theme %d\naccent %d\nshadow %d\nborder %d\neffects %d\n",
          g_prefs.theme, g_prefs.accent, g_prefs.shadow, g_prefs.border,
          g_prefs.effects);
  for (i = 0; i < FX_COUNT; i++)
    {
      fprintf(f, "fx %s %d\n", g_fx_names[i], (g_prefs.fx >> i) & 1);
    }

  fprintf(f, "clock24 %d\nclockdate %d\nclocksecs %d\n",
          g_prefs.clock24, g_prefs.clockdate, g_prefs.clocksecs);
  fprintf(f, "dockicon %d\ndockpos %d\ndockhide %d\n",
          g_prefs.dockicon, g_prefs.dockpos, g_prefs.dockhide);
  fprintf(f, "repdelay %d\nreprate %d\nptrspeed %d\ntzoff %d\n",
          g_prefs.repdelay, g_prefs.reprate, g_prefs.ptrspeed,
          g_prefs.tzoff);
  if (g_prefs.npinned == 0)
    {
      fprintf(f, "nodock -\n");
    }

  for (i = 0; i < g_prefs.npinned; i++)
    {
      fprintf(f, "dock %s\n", g_apps[g_prefs.pinned[i]]->name);
    }

#ifdef CONFIG_FPCDESK_SOUND
  desk_sound_prefs_write(f);
#endif
  fclose(f);
}

/* Key repeat and the pointer's speed belong to the desktop session (the
 * kernel thread that reads the keyboard and the mouse): its settings file
 * /dev/desktop takes "name value" lines (fpc/drivers/gpu/fpc_desktop.c)
 */

void desk_input_apply(void)
{
  char buf[96];
  int fd;
  int n;

  fd = open("/dev/desktop", O_WRONLY);
  if (fd < 0)
    {
      return;
    }

  n = snprintf(buf, sizeof(buf),
               "repeat_delay %d\nrepeat_rate %d\npointer_speed %d\n",
               g_prefs.repdelay, g_prefs.reprate, g_prefs.ptrspeed);
  write(fd, buf, n);
  close(fd);
}

/* The load, once a second: /proc/cpuload, /proc/meminfo (all heaps) */

void desk_load_sample(void)
{
  char line[128];
  FILE *f;
  int load = 0;

  f = fopen("/proc/cpuload", "r");
  if (f != NULL)
    {
      if (fgets(line, sizeof(line), f) != NULL)
        {
          load = atoi(line);
        }

      fclose(f);
    }

  g_desk.cpuload = load;
  memmove(g_desk.cpuhist, g_desk.cpuhist + 1, sizeof(g_desk.cpuhist) - 1);
  g_desk.cpuhist[sizeof(g_desk.cpuhist) - 1] = (uint8_t)load;

  f = fopen("/proc/meminfo", "r");
  if (f != NULL)
    {
      unsigned long total = 0;
      unsigned long used = 0;

      while (fgets(line, sizeof(line), f) != NULL)
        {
          unsigned long a;
          unsigned long u;

          if (sscanf(line, "%lu %lu", &a, &u) == 2)
            {
              total += a;
              used  += u;
            }
        }

      fclose(f);
      g_desk.memtotal = total / 1024;
      g_desk.memused  = used / 1024;
    }
}
