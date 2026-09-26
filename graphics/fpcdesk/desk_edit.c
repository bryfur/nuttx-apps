/****************************************************************************
 * apps/graphics/fpcdesk/desk_edit.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Text Editor.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>

#include "desk.h"

static struct desk_ops_s g_o;

static void edit_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  int i;

  icon_tile(o, x, y, s, RGB(0xff, 0xd2, 0x5a), RGB(0xf0, 0x96, 0x1e), a);
  ops_rrect(o, x + s / 5, y + s / 6, s * 3 / 5, s * 2 / 3, LUT_R3, 3,
            C_WHITE, a);
  for (i = 0; i < 4; i++)
    {
      ops_blend(o, x + s / 5 + 5, y + s / 6 + 7 + i * s / 8,
                s * 3 / 5 - 10 - (i == 3 ? s / 6 : 0), 2,
                RGB(0x80, 0x80, 0x88), a);
    }
}

static void edit_draw(struct desk_win_s *w)
{
  ops_reset(&g_o);
  ops_fill(&g_o, 0, TITLE_H, w->csize.w, w->csize.h, C_WINDOW_BG);
  ops_text(&g_o, &g_desk_font_ui, 16, TITLE_H + 16, "Text Editor: to come",
           C_TEXT_DIM, 255);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
}

static struct desk_win_s *edit_open(void)
{
  struct desk_win_s *w = desk_win_open(APP_EDITOR, "Text Editor", 600, 420);

  if (w != NULL)
    {
      edit_draw(w);
    }

  return w;
}

int desk_editor_open(const char *path)
{
  desk_launch(APP_EDITOR);
  return 0;
}

const struct desk_app_s g_app_editor =
{
  .name = "Text Editor",
  .icon = edit_icon,
  .open = edit_open,
  .draw = edit_draw,
};
