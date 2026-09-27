/****************************************************************************
 * apps/graphics/fpcdesk/desk_volume.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The volume control: a speaker at the right of the top bar (its waves
 * follow the master volume, a cross when muted) and, when it is clicked,
 * a popover below it: the master volume (a slider; the speaker mutes),
 * what is playing (each program with its level meter and its own level),
 * and a way to the Sound settings.  A volume key shows the popover for a
 * moment.  The popover is a layer window (a display list over a
 * translucent backdrop), redrawn whole when anything in it changes; while
 * it is open its meters are read ten times a second.
 *
 * Hooks: see desk_sound.h.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <string.h>

#include <nuttx/nx/nxbe.h>

#include "desk_sound.h"
#include "desk_widgets.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ICON_S          16              /* The speaker in the top bar */
#define POP_W           320
#define POP_PAD         16
#define POP_ROW         44              /* A program's row */
#define POP_TOP         56              /* Below the master volume */
#define POP_METER_MS    100
#define POP_OSD_MS      1500

/****************************************************************************
 * Private Data
 ****************************************************************************/

extern const struct nx_callback_s g_desk_partcb;

static struct
{
  NXWINDOW pop;                         /* The popover, NULL when closed */
  int      w;
  int      h;
  bool     hover;                       /* The pointer is on the speaker */
  int      ix;                          /* The speaker's area in the bar */
  int      iw;
  bool     osd;                         /* Shown by a key: closes itself */
  uint32_t osdms;
  bool     inside;                      /* The pointer is in the popover */
  uint8_t  last;                        /* Buttons, last popover event */
  bool     mutehot;
  bool     linkhot;
  uint32_t meterms;
  struct wdg_slider_s master;
  int      nrows;
  int8_t   chan[FPCMIX_NCHAN_MAX];      /* The channel each row shows */
  struct wdg_slider_s lev[FPCMIX_NCHAN_MAX];
  uint16_t peak[FPCMIX_NCHAN_MAX];      /* What the meters show */
  struct desk_ops_s ops;
} g_vol;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* The programs to list: playing, paused or playing out, but not the
 * desktop's own system sounds
 */

static int pop_rows(const struct fpcmix_info_s *info)
{
  int n = 0;
  int i;

  for (i = 0; i < info->nchan && i < FPCMIX_NCHAN_MAX; i++)
    {
      const struct fpcmix_chan_s *c = &info->chan[i];

      if ((c->state == FPCMIX_PLAYING || c->state == FPCMIX_PAUSED ||
           c->state == FPCMIX_DRAINING) &&
          strcmp(c->name, DESK_SOUND_ALERTS) != 0)
        {
          g_vol.chan[n++] = i;
        }
    }

  return n;
}

static int pop_height(int nrows)
{
  return POP_TOP + 30 + (nrows > 0 ? nrows : 1) * POP_ROW + 50;
}

static void pop_format(const struct fpcmix_chan_s *c, char *buf, size_t len)
{
  unsigned long r = (c->rate + 50) / 100;

  snprintf(buf, len, "%lu.%lu kHz %s%s", r / 10, r % 10,
           c->channels == 1 ? "mono" : "stereo",
           c->state == FPCMIX_PAUSED ? ", paused" : "");
}

static void pop_draw(void)
{
  const struct fpcmix_info_s *info = desk_sound_info();
  struct desk_ops_s *o = &g_vol.ops;
  const struct desk_font_s *f = &g_desk_font_ui;
  char buf[40];
  int y;
  int i;

  if (g_vol.pop == NULL || info == NULL)
    {
      return;
    }

  ops_reset(o);

  /* The master volume: the speaker (a click mutes), the slider, the
   * figure
   */

  if (g_vol.mutehot)
    {
      ops_rrect(o, POP_PAD - 6, 12, 38, 32, LUT_R6, 6, C_WHITE, 30);
    }

  wdg_speaker(o, POP_PAD, 18, 20, wdg_speaker_waves(info->master),
              info->mute, C_WHITE, 255);
  g_vol.master.x     = POP_PAD + 44;
  g_vol.master.y     = 18;
  g_vol.master.w     = g_vol.w - POP_PAD - 44 - 48;
  g_vol.master.dim   = info->mute;
  if (!g_vol.master.drag)
    {
      g_vol.master.value = info->master;
    }

  wdg_slider_draw(o, &g_vol.master);
  snprintf(buf, sizeof(buf), "%d%%", (info->master + 5) / 10);
  ops_text(o, &g_desk_font_uib, g_vol.w - POP_PAD -
           text_width(&g_desk_font_uib, buf), 28 - text_height(f) / 2, buf,
           info->mute ? C_TEXT_DIM : C_WHITE, 255);

  /* What is playing */

  ops_blend(o, POP_PAD, POP_TOP - 6, g_vol.w - 2 * POP_PAD, 1, C_WHITE, 40);
  ops_text(o, &g_desk_font_small, POP_PAD, POP_TOP + 4,
           info->gpu ? "PLAYING" : "PLAYING (NO AUDIO IN THE GPU)",
           C_TEXT_DIM, 255);

  y = POP_TOP + 30;
  if (g_vol.nrows == 0)
    {
      ops_text(o, f, POP_PAD, y + 6, "Nothing is playing", C_TEXT_DIM,
               255);
    }

  for (i = 0; i < g_vol.nrows; i++, y += POP_ROW)
    {
      const struct fpcmix_chan_s *c = &info->chan[g_vol.chan[i]];
      struct wdg_slider_s *s = &g_vol.lev[i];

      ops_text(o, &g_desk_font_uib, POP_PAD, y, c->name[0] ? c->name : "?",
               C_WHITE, 255);
      pop_format(c, buf, sizeof(buf));
      ops_text(o, &g_desk_font_small, POP_PAD, y + 18, buf, C_TEXT_DIM,
               255);
      g_vol.peak[i] = c->peak;
      wdg_meter(o, POP_PAD, y + 34, 110, 4, c->peak, 255);

      s->x   = POP_PAD + 136;
      s->y   = y + 6;
      s->w   = g_vol.w - POP_PAD - 136 - POP_PAD;
      s->dim = info->mute;
      if (!s->drag)
        {
          s->value = c->level;
        }

      wdg_slider_draw(o, s);
    }

  /* The way to the settings */

  y = g_vol.h - 38;
  ops_blend(o, POP_PAD, y, g_vol.w - 2 * POP_PAD, 1, C_WHITE, 40);
  if (g_vol.linkhot)
    {
      ops_rrect(o, POP_PAD / 2, y + 6, g_vol.w - POP_PAD, 26, LUT_R6, 6,
                C_WHITE, 30);
    }

  ops_text(o, f, POP_PAD, y + 11, "Sound Settings" DESK_CH_ELLIPSIS,
           C_WHITE, 255);
  ops_setlist(g_vol.pop, o);
}

/* Size and place it for the rows it has now: below the speaker, on the
 * display
 */

static void pop_layout(void)
{
  const struct fpcmix_info_s *info = desk_sound_info();
  struct nxgl_point_s pos;
  struct nxgl_size_s size;
  int x;

  g_vol.nrows = info != NULL ? pop_rows(info) : 0;
  g_vol.w     = POP_W;
  g_vol.h     = pop_height(g_vol.nrows);
  x = g_vol.ix + g_vol.iw / 2 - g_vol.w / 2;
  x = x + g_vol.w > DESK_W - 8 ? DESK_W - 8 - g_vol.w : x;
  x = x < 8 ? 8 : x;

  size.w = g_vol.w;
  size.h = g_vol.h;
  pos.x  = x;
  pos.y  = TOPBAR_H + 6;
  nx_setsize(g_vol.pop, &size);
  nx_setposition(g_vol.pop, &pos);
}

static void pop_open(bool osd)
{
  struct nx_effects_s fx;

  if (g_vol.pop == NULL)
    {
      g_vol.pop = nx_openwindow(g_desk.hnx,
                                NXBE_WINDOW_LAYER | NXBE_WINDOW_HIDDEN,
                                &g_desk_partcb, (void *)(uintptr_t)PART_SOUND);
      if (g_vol.pop == NULL)
        {
          return;
        }

      memset(&fx, 0, sizeof(fx));
      fx.flags    = NX_FX_ROUND;
      fx.layer    = NX_LAYER_OVERLAY;
      fx.opacity  = 255;
      fx.backdrop = 235;
      fx.tint     = RGB(0x24, 0x24, 0x28);
      fx.shadow   = 140;
      fx.shadowdy = 6;
      nx_seteffects(g_vol.pop, &fx);

      memset(&g_vol.master, 0, sizeof(g_vol.master));
      memset(g_vol.lev, 0, sizeof(g_vol.lev));
      g_vol.inside  = false;
      g_vol.mutehot = false;
      g_vol.linkhot = false;
      desk_sound_refresh();
      pop_layout();
      pop_draw();
      nx_setvisibility(g_vol.pop, false);
    }

  g_vol.osd   = osd;
  g_vol.osdms = desk_ms();
  desk_topbar_draw();
}

static void pop_close(void)
{
  if (g_vol.pop != NULL)
    {
      nx_closewindow(g_vol.pop);
      g_vol.pop = NULL;
      desk_topbar_draw();
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* The speaker, its right edge at right; returns where the bar goes on to
 * the left of it
 */

int desk_volume_topbar(struct desk_ops_s *o, int right, int ty)
{
  const struct fpcmix_info_s *info = desk_sound_info();
  int w = ICON_S * 6 / 5 + 4;
  int x = right - w;

  if (info == NULL)
    {
      g_vol.iw = 0;
      return right;
    }

  g_vol.ix = x - 6;
  g_vol.iw = w + 12;
  if (g_vol.hover || g_vol.pop != NULL)
    {
      ops_rrect(o, g_vol.ix, 4, g_vol.iw, TOPBAR_H - 8, LUT_R6, 6, C_WHITE,
                g_vol.pop != NULL ? 70 : 45);
    }

  wdg_speaker(o, x, (TOPBAR_H - ICON_S) / 2, ICON_S,
              wdg_speaker_waves(info->master), info->mute, C_WHITE,
              info->mute ? 170 : 235);
  return g_vol.ix;
}

bool desk_volume_topbar_mouse(int x, int y, uint8_t buttons)
{
  static uint8_t last;
  bool over = g_vol.iw > 0 && x >= g_vol.ix && x < g_vol.ix + g_vol.iw &&
              y >= 0 && y < TOPBAR_H;

  if (over != g_vol.hover)
    {
      g_vol.hover = over;
      desk_topbar_draw();
    }

  if (over && (buttons & NX_MOUSE_LEFTBUTTON) &&
      !(last & NX_MOUSE_LEFTBUTTON))
    {
      if (g_vol.pop != NULL && !g_vol.osd)
        {
          pop_close();
        }
      else
        {
          pop_open(false);
          g_vol.osd = false;
        }
    }

  last = buttons;
  return over;
}

void desk_volume_popover_mouse(int x, int y, uint8_t buttons)
{
  const struct fpcmix_info_s *info = desk_sound_info();
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(g_vol.last & NX_MOUSE_LEFTBUTTON);
  bool redraw = false;
  bool drag;
  bool was;
  bool hot;
  int i;

  if (g_vol.pop == NULL || info == NULL)
    {
      return;
    }

  g_vol.inside = x >= 0 && y >= 0 && x < g_vol.w && y < g_vol.h;
  g_vol.osd    = false;                   /* Touched: it stays */

  /* The speaker: mute */

  hot = x >= POP_PAD - 6 && x < POP_PAD + 32 && y >= 12 && y < 44;
  if (hot != g_vol.mutehot)
    {
      g_vol.mutehot = hot;
      redraw = true;
    }

  if (hot && press)
    {
      desk_sound_mute(2);
      desk_prefs_save();
      redraw = true;
    }

  /* The master volume; a pop at the new volume when let go */

  was  = g_vol.master.hot;
  drag = g_vol.master.drag;
  if (wdg_slider_mouse(&g_vol.master, x, y, buttons, g_vol.last))
    {
      desk_sound_master(g_vol.master.value);
      desk_topbar_draw();
      redraw = true;
    }

  if (was != g_vol.master.hot)
    {
      redraw = true;
    }

  if (drag && !g_vol.master.drag)
    {
      desk_sound_play(SND_VOLUME);
      desk_prefs_save();
      redraw = true;
    }

  /* The programs' levels */

  for (i = 0; i < g_vol.nrows; i++)
    {
      struct wdg_slider_s *s = &g_vol.lev[i];

      was  = s->hot;
      drag = s->drag;

      if (wdg_slider_mouse(s, x, y, buttons, g_vol.last))
        {
          desk_sound_level(info->chan[g_vol.chan[i]].name, s->value);
          redraw = true;
        }

      if (was != s->hot || (drag && !s->drag))
        {
          redraw = true;
          if (drag && !s->drag)
            {
              desk_prefs_save();
            }
        }
    }

  /* Sound Settings... */

  hot = y >= g_vol.h - 32 && y < g_vol.h - 6 && x >= 0 && x < g_vol.w;
  if (hot != g_vol.linkhot)
    {
      g_vol.linkhot = hot;
      redraw = true;
    }

  g_vol.last = buttons;
  if (hot && press)
    {
      pop_close();
      desk_launch(APP_SETTINGS);
      return;
    }

  if (redraw)
    {
      pop_draw();
    }
}

/* Every pointer event, before it goes where it goes: a press anywhere but
 * in the popover or on the speaker closes it
 */

void desk_volume_mouse_any(int part, uint8_t buttons)
{
  static uint8_t last;
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(last & NX_MOUSE_LEFTBUTTON);

  last = buttons;
  if (part != PART_TOPBAR && g_vol.hover)
    {
      g_vol.hover = false;
      desk_topbar_draw();
    }

  if (part != PART_SOUND)
    {
      if (g_vol.inside || g_vol.mutehot || g_vol.linkhot)
        {
          g_vol.inside  = false;
          g_vol.mutehot = false;
          g_vol.linkhot = false;
          g_vol.last    = 0;
          pop_draw();
        }

      if (press && g_vol.pop != NULL &&
          !(part == PART_TOPBAR && g_vol.hover))
        {
          pop_close();
        }
    }
}

/* While it is open: the meters, ten times a second; shown by a key, it
 * goes after a moment unless the pointer is in it
 */

bool desk_volume_animate(void)
{
  const struct fpcmix_info_s *info;
  uint32_t now = desk_ms();
  bool redraw = false;
  int i;

  if (g_vol.pop == NULL)
    {
      return false;
    }

  if (g_vol.osd && !g_vol.inside && now - g_vol.osdms > POP_OSD_MS)
    {
      pop_close();
      return false;
    }

  if (now - g_vol.meterms >= POP_METER_MS)
    {
      g_vol.meterms = now;
      info = desk_sound_refresh();
      if (info != NULL && pop_rows(info) != g_vol.nrows)
        {
          pop_layout();
          redraw = true;
        }

      for (i = 0; info != NULL && i < g_vol.nrows; i++)
        {
          redraw |= info->chan[g_vol.chan[i]].peak / 256 !=
                    g_vol.peak[i] / 256;
        }

      if (redraw)
        {
          pop_draw();
        }
    }

  return true;
}

/* The mixer's state has changed: the speaker, the popover; a volume key
 * (bykey) shows the popover for a moment
 */

void desk_volume_changed(bool bykey)
{
  desk_topbar_draw();
  if (bykey && g_vol.pop == NULL)
    {
      pop_open(true);
      return;
    }

  if (bykey)
    {
      g_vol.osdms = desk_ms();
    }

  if (g_vol.pop != NULL)
    {
      pop_layout();
      pop_draw();
    }
}

void desk_volume_relayout(void)
{
  pop_close();
}
