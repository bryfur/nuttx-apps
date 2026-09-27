/****************************************************************************
 * apps/graphics/fpcdesk/desk_soundpage.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Sound page of Settings: where the sound goes (HDMI, or nowhere
 * without audio in the GPU), the volume, mute, the alert volume (the level
 * of the system sounds) with a test, interface sounds on or off, and each
 * application's level (those the mixer has seen; a dot for those playing).
 * It draws into the window it is given, at the place it is given, and
 * takes that window's pointer events: Settings (or its Sound page) hands
 * them over.  Hooks: see desk_sound.h.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>

#include "desk_sound.h"
#include "desk_ui.h"
#include "desk_widgets.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ROW_H           36
#define LABEL_W         150
#define APPS_MAX        4               /* Application rows shown */
#define APP_ROW         32
#define TOP_ROWS        52              /* The title and the output line */
#define APPS_Y          (TOP_ROWS + 4 * ROW_H + 14)
#define PAGE_H          (APPS_Y + 22 + APPS_MAX * APP_ROW + 8)

#define ID_TEST         1

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct
{
  struct desk_win_s *win;               /* Where it was drawn */
  int      x;                           /* Its place, client coordinates */
  int      y;
  int      w;
  uint8_t  last;
  struct wdg_slider_s master;
  struct wdg_switch_s mute;
  struct wdg_slider_s alerts;
  struct wdg_switch_s effects;
  bool     testhot;
  bool     testdown;
  int      napps;
  struct fpcmix_level_s apps[APPS_MAX];
  struct wdg_slider_s app[APPS_MAX];
  struct desk_ops_s ops;
} g_page;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The applications to list: the mixer's levels but the system sounds' */

static void page_apps(void)
{
  struct fpcmix_levels_s ls;
  int i;

  g_page.napps = 0;
  if (desk_sound_levels(&ls) < 0)
    {
      return;
    }

  for (i = 0; i < ls.n && g_page.napps < APPS_MAX; i++)
    {
      if (strcmp(ls.l[i].name, DESK_SOUND_ALERTS) != 0)
        {
          g_page.apps[g_page.napps++] = ls.l[i];
        }
    }
}

static bool page_playing(const struct fpcmix_info_s *info, const char *name)
{
  int i;

  for (i = 0; i < info->nchan && i < FPCMIX_NCHAN_MAX; i++)
    {
      if (info->chan[i].state == FPCMIX_PLAYING &&
          strcmp(info->chan[i].name, name) == 0)
        {
          return true;
        }
    }

  return false;
}

static void page_label(struct desk_ops_s *o, int y, const char *label)
{
  ops_text(o, &g_desk_font_ui, g_page.x, TITLE_H + y + (ROW_H -
           text_height(&g_desk_font_ui)) / 2 - 2, label, C_TEXT, 255);
}

static void page_percent(struct desk_ops_s *o, int y, int v, bool dim)
{
  char buf[16];

  snprintf(buf, sizeof(buf), "%d%%", (v + 5) / 10);
  ops_text(o, &g_desk_font_ui, g_page.x + g_page.w - text_width(
           &g_desk_font_ui, buf), TITLE_H + y + (ROW_H -
           text_height(&g_desk_font_ui)) / 2 - 2, buf,
           dim ? C_TEXT_DIM : C_TEXT, 255);
}

/* The widgets keep client coordinates (for the pointer); they are drawn
 * in the frame's
 */

static void page_slider(struct desk_ops_s *o, const struct wdg_slider_s *s)
{
  struct wdg_slider_s f = *s;

  f.y += TITLE_H;
  wdg_slider_draw(o, &f);
}

static void page_switch(struct desk_ops_s *o, const struct wdg_switch_s *s)
{
  struct wdg_switch_s f = *s;

  f.y += TITLE_H;
  wdg_switch_draw(o, &f);
}

static void page_draw(void)
{
  const struct fpcmix_info_s *info = desk_sound_refresh();
  struct desk_ops_s *o = &g_page.ops;
  NXWINDOW hwnd;
  int cx = g_page.x + LABEL_W;
  int sw = g_page.w - LABEL_W - 48;
  int y = g_page.y;
  int i;

  if (g_page.win == NULL)
    {
      return;
    }

  hwnd = (NXWINDOW)g_page.win->hwnd;
  ops_reset(o);
  ops_fill(o, g_page.x, TITLE_H + y, g_page.w, PAGE_H, C_WINDOW_BG);
  ops_text(o, &g_desk_font_title, g_page.x, TITLE_H + y, "Sound", C_TEXT,
           255);
  ops_text(o, &g_desk_font_ui, g_page.x, TITLE_H + y + 30,
           info == NULL ? "No audio driver" :
           info->gpu ? "Output: HDMI, played by the GPU" :
                       "No audio in the GPU (a bitstream before v7): "
                       "sound is discarded", C_TEXT_DIM, 255);
  ops_draw(hwnd, o);
  if (info == NULL)
    {
      return;
    }

  /* The volume and mute */

  y += TOP_ROWS;
  page_label(o, y, "Volume");
  g_page.master.x   = cx;
  g_page.master.y   = y + (ROW_H - WDG_SLIDER_H) / 2;
  g_page.master.w   = sw;
  g_page.master.dim = info->mute;
  if (!g_page.master.drag)
    {
      g_page.master.value = info->master;
    }

  page_slider(o, &g_page.master);
  page_percent(o, y, info->master, info->mute);

  y += ROW_H;
  page_label(o, y, "Mute");
  g_page.mute.x  = cx;
  g_page.mute.y  = y + (ROW_H - WDG_SWITCH_H) / 2;
  g_page.mute.on = info->mute;
  page_switch(o, &g_page.mute);
  ops_draw(hwnd, o);

  /* The alert volume, a test */

  y += ROW_H;
  page_label(o, y, "Alert volume");
  g_page.alerts.x = cx;
  g_page.alerts.y = y + (ROW_H - WDG_SLIDER_H) / 2;
  g_page.alerts.w = sw - 80;
  if (!g_page.alerts.drag)
    {
      g_page.alerts.value = desk_sound_getlevel(DESK_SOUND_ALERTS);
    }

  page_slider(o, &g_page.alerts);
  ui_button(o, cx + sw - 64, TITLE_H + y + 5, 64 + 48, ROW_H - 10, "Test",
            g_page.testdown ? UI_PRESSED : g_page.testhot ? UI_HOVER :
            UI_NORMAL);

  y += ROW_H;
  page_label(o, y, "Interface sounds");
  g_page.effects.x  = cx;
  g_page.effects.y  = y + (ROW_H - WDG_SWITCH_H) / 2;
  g_page.effects.on = desk_sound_effects();
  page_switch(o, &g_page.effects);
  ops_text(o, &g_desk_font_small, cx + WDG_SWITCH_W + 12, TITLE_H + y + 12,
           "Windows opening, closing", C_TEXT_DIM, 255);
  ops_draw(hwnd, o);

  /* The applications */

  y = g_page.y + APPS_Y;
  ops_text(o, &g_desk_font_small, g_page.x, TITLE_H + y, "APPLICATIONS",
           C_TEXT_DIM, 255);
  y += 22;
  page_apps();
  if (g_page.napps == 0)
    {
      ops_text(o, &g_desk_font_ui, g_page.x, TITLE_H + y + 6,
               "None has played yet", C_TEXT_DIM, 255);
    }

  for (i = 0; i < g_page.napps; i++, y += APP_ROW)
    {
      struct wdg_slider_s *s = &g_page.app[i];
      bool playing = page_playing(info, g_page.apps[i].name);

      if (playing)
        {
          ops_disc(o, g_page.x + 4, TITLE_H + y + APP_ROW / 2, LUT_R3, 3,
                   RGB(0x34, 0xc7, 0x59), 255);
        }

      ops_text(o, &g_desk_font_ui, g_page.x + 14, TITLE_H + y + (APP_ROW -
               text_height(&g_desk_font_ui)) / 2 - 1, g_page.apps[i].name,
               playing ? C_TEXT : C_TEXT_DIM, 255);
      s->x = cx;
      s->y = y + (APP_ROW - WDG_SLIDER_H) / 2;
      s->w = sw;
      if (!s->drag)
        {
          s->value = g_page.apps[i].level;
        }

      page_slider(o, s);
      page_percent(o, y - (ROW_H - APP_ROW) / 2, s->value, false);
      ui_flush(hwnd, o, 32);
    }

  ops_draw(hwnd, o);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int desk_soundpage_height(void)
{
  return PAGE_H;
}

/* Drawn in w at (x, y) of the frame (the client area starts at TITLE_H) */

void desk_soundpage_draw(struct desk_win_s *w, int x, int y, int width)
{
  if (g_page.win != w)
    {
      memset(&g_page, 0, sizeof(g_page));
    }

  g_page.win = w;
  g_page.x   = x;
  g_page.y   = y - TITLE_H;
  g_page.w   = width;
  page_draw();
}

/* A pointer event in the window's client area; true if it was the page's */

bool desk_soundpage_mouse(struct desk_win_s *w, int x, int y,
                          uint8_t buttons)
{
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(g_page.last & NX_MOUSE_LEFTBUTTON);
  bool release = !(buttons & NX_MOUSE_LEFTBUTTON) &&
                 (g_page.last & NX_MOUSE_LEFTBUTTON);
  uint8_t last = g_page.last;
  bool redraw = false;
  bool save = false;
  bool hot;
  int i;

  if (w != g_page.win)
    {
      return false;
    }

  g_page.last = buttons;
  if (y < g_page.y && !g_page.master.drag && !g_page.alerts.drag)
    {
      return false;
    }

  /* The volume, mute */

  hot = g_page.master.drag;
  if (wdg_slider_mouse(&g_page.master, x, y, buttons, last))
    {
      desk_sound_master(g_page.master.value);
      desk_topbar_draw();
      redraw = true;
    }

  if (hot && !g_page.master.drag)
    {
      desk_sound_play(SND_VOLUME);
      save = true;
    }

  if (wdg_switch_mouse(&g_page.mute, x, y, buttons, last))
    {
      desk_sound_mute(g_page.mute.on ? 1 : 0);
      desk_topbar_draw();
      redraw = save = true;
    }

  /* The alert volume: heard when let go */

  hot = g_page.alerts.drag;
  if (wdg_slider_mouse(&g_page.alerts, x, y, buttons, last))
    {
      desk_sound_level(DESK_SOUND_ALERTS, g_page.alerts.value);
      redraw = true;
    }

  if (hot && !g_page.alerts.drag)
    {
      desk_sound_play(SND_ALERT);
      save = true;
    }

  hot = x >= g_page.alerts.x + g_page.alerts.w + 16 &&
        x < g_page.x + g_page.w && y >= g_page.alerts.y - 4 &&
        y < g_page.alerts.y + WDG_SLIDER_H + 4;
  if (hot != g_page.testhot || (g_page.testdown && release))
    {
      g_page.testhot  = hot;
      g_page.testdown = false;
      redraw = true;
    }

  if (hot && press)
    {
      g_page.testdown = true;
      desk_sound_play(SND_NOTIFY);
      redraw = true;
    }

  if (wdg_switch_mouse(&g_page.effects, x, y, buttons, last))
    {
      desk_sound_set_effects(g_page.effects.on);
      redraw = save = true;
    }

  /* The applications */

  for (i = 0; i < g_page.napps; i++)
    {
      struct wdg_slider_s *s = &g_page.app[i];

      hot = s->drag;
      if (wdg_slider_mouse(s, x, y, buttons, last))
        {
          desk_sound_level(g_page.apps[i].name, s->value);
          g_page.apps[i].level = s->value;
          redraw = true;
        }

      save |= hot && !s->drag;
    }

  if (save)
    {
      desk_prefs_save();
    }

  if (redraw)
    {
      page_draw();
    }

  return true;
}

/* The mixer has changed: the page, if it is showing */

void desk_soundpage_changed(void)
{
  if (g_page.win != NULL)
    {
      if (desk_app_window(APP_SETTINGS) != g_page.win)
        {
          g_page.win = NULL;                /* Its window has gone */
          return;
        }

      page_draw();
    }
}
