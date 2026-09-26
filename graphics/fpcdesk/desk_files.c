/****************************************************************************
 * apps/graphics/fpcdesk/desk_files.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Files: the file manager.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>

#include "desk.h"

static struct desk_ops_s g_o;

static void files_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  icon_tile(o, x, y, s, RGB(0x5a, 0xb4, 0xff), RGB(0x1e, 0x6e, 0xd8), a);
  ops_rrect(o, x + s / 5, y + s / 3, s * 3 / 5, s * 2 / 5, LUT_R3, 3,
            C_WHITE, (uint8_t)(a * 230 / 255));
  ops_rrect(o, x + s / 5, y + s / 4, s / 4, s / 8, LUT_R3, 3, C_WHITE,
            (uint8_t)(a * 230 / 255));
}

static void files_draw(struct desk_win_s *w)
{
  ops_reset(&g_o);
  ops_fill(&g_o, 0, TITLE_H, w->csize.w, w->csize.h, C_WINDOW_BG);
  ops_text(&g_o, &g_desk_font_ui, 16, TITLE_H + 16, "Files: to come",
           C_TEXT_DIM, 255);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
}

static struct desk_win_s *files_open(void)
{
  struct desk_win_s *w = desk_win_open(APP_FILES, "Files", 560, 380);

  if (w != NULL)
    {
      files_draw(w);
    }

  return w;
}

const struct desk_app_s g_app_files =
{
  .name = "Files",
  .icon = files_icon,
  .open = files_open,
  .draw = files_draw,
};
