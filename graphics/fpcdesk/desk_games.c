/****************************************************************************
 * apps/graphics/fpcdesk/desk_games.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Full-screen games (the game SDK, apps/graphics/fpcgame; docs/games.md):
 * a launcher entry each, which starts the game's program as a task of its
 * own and opens no window.  The game takes the display and the keyboard
 * from the desktop while it runs, and gives them back as they were when
 * it ends (Escape).
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <sched.h>
#include <syslog.h>

#include <nuttx/lib/builtin.h>

#include "desk.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Start the program NAME: false if it is not in this system or cannot */

static bool game_start(const char *name, const char *title)
{
  const struct builtin_s *b;
  int index;
  pid_t pid;

  index = builtin_isavail(name);
  b     = index >= 0 ? builtin_for_index(index) : NULL;
  if (b == NULL)
    {
      syslog(LOG_WARNING, "desk: %s: no program %s\n", title, name);
      return false;
    }

  pid = task_create(title, b->priority, b->stacksize, b->main, NULL);
  if (pid < 0)
    {
      syslog(LOG_WARNING, "desk: %s: cannot start: %d\n", title, errno);
      return false;
    }

  syslog(LOG_INFO, "desk: %s (%d)\n", title, pid);
  return true;
}

#ifdef CONFIG_GAMES_THUNDERBOLT

/* A P-47 side on, over the sky and the grass (the shapes' corners are the
 * shape tables' radii: 3)
 */

static void tb_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  uint16_t body = RGB(0x5a, 0x5e, 0x38);
  uint16_t dark = RGB(0x38, 0x3a, 0x22);
  int fh = s / 7 < 6 ? 6 : s / 7;                     /* The fuselage's height */

  icon_tile(o, x, y, s, RGB(0x70, 0xb8, 0xf4), RGB(0x34, 0x8a, 0x40), a);
  ops_rrect(o, x + s / 6, y + s * 2 / 5, s * 2 / 3, fh, LUT_R3, 3,
            body, a);                                  /* Fuselage */
  ops_rrect(o, x + s / 6, y + s / 4, s / 8 < 6 ? 6 : s / 8,
            s * 2 / 5 - s / 4 + fh, LUT_R3, 3, body, a);   /* Tail */
  ops_rrect(o, x + s * 2 / 5, y + s * 2 / 5 + fh / 2, s / 4, 6, LUT_R3, 3,
            dark, a);                                  /* Wing */
  ops_disc(o, x + s * 3 / 5, y + s * 2 / 5, LUT_R3, 3,
           RGB(0xa0, 0xd0, 0xf0), a);                  /* Canopy */
  ops_fill(o, x + s * 5 / 6, y + s * 2 / 5 - s / 8 + fh / 2, 2, s / 4,
           RGB(0x20, 0x20, 0x20));                     /* Propeller */
}

static struct desk_win_s *tb_open(void)
{
  game_start("thunderbolt", "Thunderbolt");
  return NULL;
}

const struct desk_app_s g_app_thunderbolt =
{
  .name = "Thunderbolt",
  .icon = tb_icon,
  .open = tb_open,
};

#endif /* CONFIG_GAMES_THUNDERBOLT */
