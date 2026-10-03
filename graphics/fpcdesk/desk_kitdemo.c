/****************************************************************************
 * apps/graphics/fpcdesk/desk_kitdemo.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Widgets: the widget kit's gallery (desk_kit.h).  Every control in each
 * of its states (normal, under the pointer, pressed, focused, disabled),
 * a sidebar and a table that sort and scroll, dialogs, a menu, tooltips
 * and a custom widget; the status bar says what each did.  It is also
 * the kit's test: it lays itself out at any size and display mode.
 *
 * A program of its own (/mnt/sd0/bin/widgets, apps/graphics/deskapps):
 * built into the desktop (DESK_PROGRAM_STUB) this file is only the icon
 * and the name the dock shows (desk_program.c).
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk_kit.h"

#ifdef CONFIG_FPCDESK_WIDGETS

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DEMO_W         780
#define DEMO_H         470
#define NWG            96
#define NROWS          120

/* The widgets' ids */

enum
{
  W_NONE = 0,
  W_PAGES,
  W_MENU,
  W_PAGE0,
  W_PAGE1,
  W_PAGE2,
  W_STATUS,
  W_COUNT,
  W_BTN,                               /* Buttons: + 0 .. 9 */
  W_CHECK = W_BTN + 10,                /* + 0 .. 3 */
  W_RADIO = W_CHECK + 4,               /* + 0 .. 2 */
  W_SWITCH = W_RADIO + 3,              /* + 0 .. 2 */
  W_SLIDER = W_SWITCH + 3,             /* + 0 .. 1 */
  W_PROGRESS = W_SLIDER + 2,           /* + 0 .. 1 */
  W_FIELD = W_PROGRESS + 2,            /* + 0 .. 1 */
  W_DROP = W_FIELD + 2,                /* + 0 .. 1 */
  W_SIDE = W_DROP + 2,
  W_TABLE,
  W_DLG,                               /* Dialog buttons: + 0 .. 5 */
  W_POPUP = W_DLG + 6,
  W_TIP,                               /* + 0 .. 2 */
  W_ANSWER = W_TIP + 3,
  W_CANVAS,
  W_IMAGE
};

#ifndef DESK_PROGRAM_STUB

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct demo_s
{
  struct kit_s kit;
  struct kit_widget_s wg[NWG];
  struct kit_list_s side;
  struct kit_list_s table;
  struct ui_field_s field[2];
  struct kit_surface_s pic;            /* A picture in the GPU's memory */
  uint8_t order[NROWS];                /* The table's rows, sorted */
  char status[96];
  char answer[96];
  char count[32];
  int16_t cx;                          /* The canvas's pointer */
  int16_t cy;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char * const g_pages[] =
{
  "Controls", "Lists", "Dialogs"
};

static const char * const g_sizes[] =
{
  "Small", "Medium", "Large", "Huge"
};

static const char * const g_places[] =
{
  "Home", "Documents", "Pictures", "Music", "microSD", "Computer"
};

static const char * const g_kinds[] =
{
  "Text", "Image", "Folder", "Program", "Archive"
};

static const char * const g_menu[] =
{
  "Open", "Rename", "Duplicate", "-", "Move to Trash", "Get Info"
};

static const struct kit_col_s g_cols[] =
{
  { "Name", 0,   0,   0 },
  { "Kind", 110, 300, KIT_COL_DIM },
  { "Size", 100, 380, KIT_COL_DIM | KIT_COL_RIGHT },
};

static int g_sortcol;
static bool g_sortdesc;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static int row_size(int i)
{
  return (i * 7919) % 90000 + 120;
}

static int row_cmp(const void *a, const void *b)
{
  int x = *(const uint8_t *)a;
  int y = *(const uint8_t *)b;
  int r;

  switch (g_sortcol)
    {
      case 1:
        r = strcmp(g_kinds[x % 5], g_kinds[y % 5]);
        break;

      case 2:
        r = row_size(x) - row_size(y);
        break;

      default:
        r = 0;
        break;
    }

  r = r != 0 ? r : x - y;
  return g_sortdesc ? -r : r;
}

static const char *table_text(struct kit_s *k, struct kit_widget_s *wg,
                              int row, int col, char *buf, int size)
{
  struct demo_s *d = k->priv;
  int i = d->order[row];

  switch (col)
    {
      case 0:
        snprintf(buf, size, "%s %03d.%s", i % 5 == 2 ? "Folder" : "Item",
                 i + 1, i % 5 == 2 ? "d" : i % 5 == 1 ? "png" : "txt");
        return buf;

      case 1:
        return g_kinds[i % 5];

      default:
        if (i % 5 == 2)
          {
            return NULL;
          }

        snprintf(buf, size, "%d.%d kB", row_size(i) / 1000,
                 row_size(i) % 1000 / 100);
        return buf;
    }
}

static void table_icon(struct kit_s *k, struct kit_widget_s *wg,
                       struct desk_ops_s *o, int row, int x, int y,
                       uint16_t fg, uint16_t bg)
{
  struct demo_s *d = k->priv;

  kit_paint_icon(o, d->order[row] % 5 == 2 ? KIT_ICON_FOLDER :
                 KIT_ICON_FILE, x, y, fg, bg);
}

static const char *side_text(struct kit_s *k, struct kit_widget_s *wg,
                             int row, int col, char *buf, int size)
{
  return g_places[row];
}

static void side_icon(struct kit_s *k, struct kit_widget_s *wg,
                      struct desk_ops_s *o, int row, int x, int y,
                      uint16_t fg, uint16_t bg)
{
  kit_paint_icon(o, row == 0 ? KIT_ICON_HOME : KIT_ICON_FOLDER, x, y, fg,
                 bg);
}

/* The custom widget: a gradient, and a cross where the pointer was */

static void demo_draw(struct kit_s *k, struct kit_widget_s *wg, int x0,
                      int y0, int x1, int y1)
{
  struct demo_s *d = k->priv;
  struct desk_ops_s *o;

  if (wg->id != W_CANVAS)
    {
      return;
    }

  /* Into the kit's list (it cuts what goes there to x0, y0, x1, y1) */

  o = kit_ops(k, 80);
  ops_hgrad(o, wg->x, wg->y, wg->w, wg->h, g_theme.accent,
            RGB(0xbf, 0x5a, 0xf2), 255);
  ops_text(o, KIT_FONT_BOLD, wg->x + 12, wg->y + 10, "Custom widget",
           C_WHITE, 255);
  ops_text(o, KIT_FONT_SMALL, wg->x + 12, wg->y + 30,
           "Drawn by the application; the kit says where", C_WHITE, 200);
  if (d->cx >= 0)
    {
      ops_fill(o, d->cx - 6, d->cy, 13, 1, C_WHITE);
      ops_fill(o, d->cx, d->cy - 6, 1, 13, C_WHITE);
    }
}

static void set_status(struct demo_s *d, const char *fmt, ...)
{
  va_list ap;

  va_start(ap, fmt);
  vsnprintf(d->status, sizeof(d->status), fmt, ap);
  va_end(ap);
  kit_set_text(&d->kit, W_STATUS, d->status);
}

/* Where the file dialogs start: the home directory */

static const char *demo_home(void)
{
  const char *home = getenv("HOME");

#ifdef CONFIG_LIBC_HOMEDIR
  if (home == NULL)
    {
      home = CONFIG_LIBC_HOMEDIR;
    }
#endif

  return home != NULL ? home : "/";
}

static void demo_event(struct kit_s *k, struct kit_widget_s *wg, int ev)
{
  static const char * const deleting[] =
  {
    "Delete", "Cancel"
  };

  static const char * const saving[] =
  {
    "Save", "Cancel", "Don't Save"
  };

  struct demo_s *d = k->priv;
  int id = wg != NULL ? wg->id : 0;
  const char *what = wg != NULL && wg->text != NULL ? wg->text : "";

  switch (ev)
    {
      case KIT_EV_CLICK:
        if (id == W_MENU || id == W_POPUP)
          {
            kit_menu(k, 1, wg->x, wg->y + wg->h + 2, g_menu, 6, 1u << 5);
            break;
          }

        if (id >= W_DLG && id < W_DLG + 6)
          {
            switch (id - W_DLG)
              {
                case 0:
                  kit_dialog(k, 1, KIT_DLG_MESSAGE, "The kit is here",
                             "One set of widgets for every application.",
                             NULL, 0);
                  break;

                case 1:
                  kit_dialog(k, 2, KIT_DLG_CONFIRM,
                             "Save changes to \"notes.txt\"?",
                             "If you don't save, the changes are lost.",
                             saving, 3);
                  break;

                case 2:
                  kit_dialog(k, 3, KIT_DLG_CONFIRM | KIT_DLG_DANGER,
                             "Delete \"Pictures\" and all it holds?",
                             "This cannot be undone.", deleting, 2);
                  break;

                case 3:
                  kit_dialog(k, 4, KIT_DLG_PROMPT, "New folder",
                             "Untitled folder", NULL, 0);
                  break;

                case 4:
                  kit_dialog(k, 5, KIT_DLG_OPEN, "Open", demo_home(), NULL,
                             0);
                  break;

                default:
                  {
                    snprintf(d->answer, sizeof(d->answer),
                             "%s/Untitled.txt", demo_home());
                    kit_dialog(k, 6, KIT_DLG_SAVE, "Save As", d->answer,
                               NULL, 0);
                  }
                  break;
              }

          break;
          }

        set_status(d, "Clicked %s", what);
        break;

      case KIT_EV_CHANGE:
        if (id == W_PAGES)
          {
            kit_show(k, W_PAGE0, wg->value == 0);
            kit_show(k, W_PAGE1, wg->value == 1);
            kit_show(k, W_PAGE2, wg->value == 2);
            set_status(d, "Page %d", (int)wg->value + 1);
          }
        else if (id >= W_SLIDER && id < W_SLIDER + 2)
          {
            kit_set_value(k, W_PROGRESS, wg->value);
            set_status(d, "Slider: %d", (int)wg->value);
          }
        else if (id == W_TABLE || id == W_SIDE)
          {
            struct kit_list_s *l = wg->data;

            set_status(d, "Selected row %d", l->sel + 1);
          }
        else if (id >= W_FIELD && id < W_FIELD + 2)
          {
            set_status(d, "Text: \"%s\"", d->field[id - W_FIELD].text);
          }
        else if (id >= W_DROP && id < W_DROP + 2)
          {
            set_status(d, "Size: %s", g_sizes[wg->value]);
          }
        else
          {
            set_status(d, "%s: %s", what, wg->value ? "on" : "off");
          }
        break;

      case KIT_EV_ACTIVATE:
        if (id == W_TABLE || id == W_SIDE)
          {
            set_status(d, "Opened row %d",
                       ((struct kit_list_s *)wg->data)->sel + 1);
          }
        else if (id >= W_SLIDER && id < W_SLIDER + 2)
          {
            set_status(d, "Slider let go at %d", (int)wg->value);
          }
        break;

      case KIT_EV_SORT:
        g_sortcol  = d->table.sortcol;
        g_sortdesc = d->table.sortdesc;
        qsort(d->order, NROWS, 1, row_cmp);
        kit_list_changed(k, W_TABLE);
        set_status(d, "Sorted by %s", g_cols[g_sortcol].name);
        break;

      case KIT_EV_MENU:
        set_status(d, "Menu: %s", k->arg >= 0 ? g_menu[k->arg] :
                   "nothing chosen");
        break;

      case KIT_EV_DIALOG:
        snprintf(d->answer, sizeof(d->answer), "Dialog %d: %s%s%s", k->key,
                 k->arg == KIT_ANS_OK ? "OK" : k->arg == KIT_ANS_ALT ?
                 "the third button" : "cancelled",
                 k->answer[0] != '\0' ? ", " : "", k->answer);
        kit_set_text(k, W_ANSWER, d->answer);
        set_status(d, "%s", d->answer);
        break;

      case KIT_EV_MOUSE:
        if (id == W_CANVAS)
          {
            d->cx = k->mx;
            d->cy = k->my;
            if (k->mbuttons != 0)
              {
                kit_invalidate(k, W_CANVAS);
              }
          }
        break;

      default:
        break;
    }
}

/* The picture in its surface: drawn when made, and when lost */

static void pic_draw(struct demo_s *d)
{
  struct desk_ops_s *o = malloc(sizeof(*o));     /* Now and then only */
  int i;

  if (o == NULL)
    {
      return;
    }

  ops_reset(o);
  ops_vgrad(o, 0, 0, d->pic.w, d->pic.h, RGB(0x1c, 0x2c, 0x5c),
            RGB(0x08, 0x0c, 0x18), 255);
  for (i = 0; i < 3; i++)
    {
      ops_disc(o, 30 + i * 34, 40, LUT_R12, 12, i == 0 ? C_CLOSE :
               i == 1 ? C_MINIMIZE : C_MAXIMIZE, 255);
    }

  ops_text_centre(o, KIT_FONT_BOLD, d->pic.w / 2, d->pic.h - 30,
                  "GPU surface", C_WHITE, 255);
  kit_surface_draw(&d->pic, o);
  free(o);
}

/* The widgets */

static void demo_build(struct demo_s *d, struct desk_win_s *w)
{
  static const char * const states[] =
  {
    "Normal", "Hover", "Pressed", "Focus", "Disabled"
  };

  static const uint16_t forced[] =
  {
    0, KIT_F_HOT, KIT_F_DOWN, KIT_F_RING, KIT_F_DISABLED
  };

  struct kit_s *k = &d->kit;
  struct kit_widget_s *wg;
  int i;

  kit_init(k, w, d->wg, NWG, demo_event);
  k->priv = d;
  k->draw = demo_draw;

  kit_col(k, KIT_BOX_WINDOW);

  /* The tool bar: pages, a menu */

  kit_row(k, KIT_BOX_BAR);
    kit_tabs(k, W_PAGES, g_pages, 3, 0, KIT_TABS_SEGMENTS);
    kit_tip(k, "Pages of the gallery");
    kit_spacer(k, 1);
    kit_button(k, W_MENU, NULL, KIT_ICON_MENU, KIT_BTN_FLAT);
    kit_tip(k, "A menu");
  kit_end(k);

  /* Controls */

  wg = kit_row(k, KIT_BOX_NONE);
  wg->id   = W_PAGE0;
  wg->padx = KIT_PAD;
  wg->pady = KIT_PAD;
  wg->gap  = KIT_PAD;
  kit_grow(k, 1);
    kit_col(k, KIT_BOX_NONE)->gap = KIT_PAD;
    kit_grow(k, 1);
      wg = kit_col(k, KIT_BOX_GROUP);
      wg->text = "Buttons";
        kit_row(k, KIT_BOX_NONE)->gap = 6;
          for (i = 0; i < 5; i++)
            {
              wg = kit_button(k, W_BTN + i, states[i], KIT_ICON_NONE,
                              KIT_BTN_NORMAL);
              wg->flags |= forced[i];
            }
        kit_end(k);
        kit_row(k, KIT_BOX_NONE)->gap = 6;
          kit_button(k, W_BTN + 5, "Default", KIT_ICON_NONE,
                     KIT_BTN_DEFAULT);
          kit_button(k, W_BTN + 6, "Delete", KIT_ICON_NONE, KIT_BTN_DANGER);
          kit_button(k, W_BTN + 7, "Bold", KIT_ICON_NONE,
                     KIT_BTN_TOGGLE)->value = 1;
          kit_button(k, W_BTN + 8, NULL, KIT_ICON_BACK, KIT_BTN_NORMAL);
          kit_tip(k, "Back");
          kit_button(k, W_BTN + 9, NULL, KIT_ICON_REFRESH, KIT_BTN_FLAT);
          kit_tip(k, "Refresh (a flat button)");
        kit_end(k);
      kit_end(k);

      wg = kit_col(k, KIT_BOX_GROUP);
      wg->text = "Choices";
        kit_row(k, KIT_BOX_NONE)->gap = 14;
          kit_check(k, W_CHECK + 0, "On", true);
          kit_check(k, W_CHECK + 1, "Off", false);
          kit_check(k, W_CHECK + 2, "Hover", false)->flags |= KIT_F_HOT;
          kit_check(k, W_CHECK + 3, "Disabled", true)->flags |=
            KIT_F_DISABLED;
        kit_end(k);
        kit_row(k, KIT_BOX_NONE)->gap = 14;
          kit_radio(k, W_RADIO + 0, "Dark", true);
          kit_radio(k, W_RADIO + 1, "Light", false)->flags |= KIT_F_RING;
          kit_radio(k, W_RADIO + 2, "Auto", false)->flags |= KIT_F_DISABLED;
        kit_end(k);
        kit_row(k, KIT_BOX_NONE)->gap = 14;
          kit_switch(k, W_SWITCH + 0, true);
          kit_switch(k, W_SWITCH + 1, false)->flags |= KIT_F_HOT;
          kit_switch(k, W_SWITCH + 2, true)->flags |= KIT_F_DISABLED;
        kit_end(k);
      kit_end(k);
    kit_end(k);

    kit_col(k, KIT_BOX_NONE)->gap = KIT_PAD;
    kit_grow(k, 1);
      wg = kit_col(k, KIT_BOX_GROUP);
      wg->text = "Values";
        kit_slider(k, W_SLIDER + 0, 40, 0, 100);
        kit_tip(k, "Drag it, or the arrow keys");
        kit_slider(k, W_SLIDER + 1, 70, 0, 100)->flags |= KIT_F_DISABLED;
        kit_progress(k, W_PROGRESS + 0, 40, 100);
        kit_progress(k, W_PROGRESS + 1, 85, 100);
      kit_end(k);

      wg = kit_col(k, KIT_BOX_GROUP);
      wg->text = "Text";
        kit_field(k, W_FIELD + 0, &d->field[0], "Search");
        kit_field(k, W_FIELD + 1, &d->field[1], NULL)->flags |=
          KIT_F_DISABLED;
        kit_row(k, KIT_BOX_NONE)->gap = 8;
          kit_dropdown(k, W_DROP + 0, g_sizes, 4, 1);
          kit_grow(k, 1);
          kit_dropdown(k, W_DROP + 1, g_sizes, 4, 2)->flags |=
            KIT_F_DISABLED;
          kit_grow(k, 1);
        kit_end(k);
      kit_end(k);
    kit_end(k);
  kit_end(k);

  /* Lists */

  wg = kit_row(k, KIT_BOX_NONE);
  wg->id    = W_PAGE1;
  wg->gap   = 0;
  wg->flags |= KIT_F_HIDDEN;
  kit_grow(k, 1);
    kit_col(k, KIT_BOX_SIDEBAR);
    kit_size(k, 170, 0);
      kit_label(k, 0, "Places", KIT_TEXT_SMALL | KIT_TEXT_DIM);
      kit_list(k, W_SIDE, &d->side, KIT_LIST_SIDEBAR);
      kit_grow(k, 1);
    kit_end(k);
    kit_list(k, W_TABLE, &d->table, KIT_LIST_TABLE);
    kit_grow(k, 1);
  kit_end(k);

  /* Dialogs, menus, tooltips, a custom widget */

  wg = kit_col(k, KIT_BOX_NONE);
  wg->id    = W_PAGE2;
  wg->padx  = KIT_PAD;
  wg->pady  = KIT_PAD;
  wg->gap   = KIT_PAD;
  wg->flags |= KIT_F_HIDDEN;
  kit_grow(k, 1);
    wg = kit_col(k, KIT_BOX_GROUP);
    wg->text = "Dialogs";
      kit_row(k, KIT_BOX_NONE)->gap = 6;
        kit_button(k, W_DLG + 0, "Message", KIT_ICON_NONE, KIT_BTN_NORMAL);
        kit_button(k, W_DLG + 1, "Confirm", KIT_ICON_NONE, KIT_BTN_NORMAL);
        kit_button(k, W_DLG + 2, "Delete" DESK_CH_ELLIPSIS, KIT_ICON_NONE,
                   KIT_BTN_NORMAL);
        kit_button(k, W_DLG + 3, "Prompt", KIT_ICON_NONE, KIT_BTN_NORMAL);
        kit_button(k, W_DLG + 4, "Open" DESK_CH_ELLIPSIS, KIT_ICON_NONE,
                   KIT_BTN_NORMAL);
        kit_button(k, W_DLG + 5, "Save As" DESK_CH_ELLIPSIS, KIT_ICON_NONE,
                   KIT_BTN_NORMAL);
      kit_end(k);
      kit_label(k, W_ANSWER, "No answer yet", KIT_TEXT_DIM);
    kit_end(k);

    wg = kit_col(k, KIT_BOX_GROUP);
    wg->text = "Menus and tooltips";
      kit_row(k, KIT_BOX_NONE)->gap = 6;
        kit_button(k, W_POPUP, "Menu", KIT_ICON_CHEVRON, KIT_BTN_NORMAL);
        kit_button(k, W_TIP + 0, NULL, KIT_ICON_PLUS, KIT_BTN_FLAT);
        kit_tip(k, "Add");
        kit_button(k, W_TIP + 1, NULL, KIT_ICON_MINUS, KIT_BTN_FLAT);
        kit_tip(k, "Remove");
        kit_button(k, W_TIP + 2, NULL, KIT_ICON_INFO, KIT_BTN_FLAT);
        kit_tip(k, "Hover over a control for a second for its tooltip");
      kit_end(k);
    kit_end(k);

    wg = kit_row(k, KIT_BOX_NONE);
    wg->gap = KIT_PAD;
    kit_grow(k, 1);
      kit_image(k, W_IMAGE, &d->pic);
      kit_tip(k, "A surface: drawn once, then COPYs from the GPU's memory");
      kit_custom(k, W_CANVAS);
      kit_grow(k, 1);
    kit_end(k);
  kit_end(k);

  /* The status bar */

  kit_row(k, KIT_BOX_STATUS);
    kit_label(k, W_STATUS, d->status, KIT_TEXT_DIM);
    kit_grow(k, 1);
    kit_label(k, W_COUNT, d->count, KIT_TEXT_DIM);
  kit_end(k);
  kit_end(k);

  snprintf(d->count, sizeof(d->count), "%d widgets", k->n);
  kit_shortcut(k, KEY_CTL('o'), W_DLG + 4);
  kit_shortcut(k, KEY_CTL('s'), W_DLG + 5);
}

static void demo_resize(struct desk_win_s *w)
{
  struct demo_s *d = w->priv;

  if (d != NULL)
    {
      kit_resize(&d->kit);
    }
}

static void demo_expose(struct desk_win_s *w, int x, int y, int cw, int ch)
{
  struct demo_s *d = w->priv;

  if (d != NULL)
    {
      kit_expose(&d->kit, x, y, cw, ch);
    }
}

static void demo_draw_all(struct desk_win_s *w)
{
  struct demo_s *d = w->priv;

  if (d != NULL)
    {
      kit_redraw(&d->kit);
    }
}

static void demo_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct demo_s *d = w->priv;

  if (d != NULL)
    {
      kit_mouse(&d->kit, x, y, buttons);
    }
}

static void demo_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct demo_s *d = w->priv;

  if (d != NULL)
    {
      kit_key(&d->kit, ch, n);
    }
}

static void demo_tick(struct desk_win_s *w)
{
  struct demo_s *d = w->priv;

  if (d != NULL)
    {
      kit_tick(&d->kit);
    }
}

static void demo_focus(struct desk_win_s *w)
{
  struct demo_s *d = w->priv;

  if (d != NULL)
    {
      kit_activate(&d->kit);
    }
}

static void demo_close(struct desk_win_s *w)
{
  struct demo_s *d = w->priv;

  if (d != NULL)
    {
      kit_dialog_close(&d->kit);
      kit_surface_close(&d->pic);
    }
}

/* The picture's pixels were lost: again */

static void demo_wake(struct desk_win_s *w)
{
  struct demo_s *d = w->priv;

  if (d != NULL && d->pic.lost)
    {
      pic_draw(d);
      kit_invalidate(&d->kit, W_IMAGE);
      kit_flush(&d->kit);
    }
}

#endif /* !DESK_PROGRAM_STUB */

static void demo_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  icon_tile(o, x, y, s, RGB(0x8e, 0x9a, 0xff), RGB(0x4a, 0x3c, 0xc8), a);
  ops_rrect(o, x + s / 5, y + s / 4, s * 3 / 5, s / 5, LUT_R6, 6, C_WHITE,
            (uint8_t)(a * 235 / 255));
  ops_rrect(o, x + s / 5, y + s * 11 / 20, s / 3, s / 5, LUT_R6, 6,
            C_WHITE, (uint8_t)(a * 150 / 255));
  ops_disc(o, x + s * 2 / 3 + 2, y + s * 13 / 20, LUT_R6, 6, C_WHITE, a);
}

#ifndef DESK_PROGRAM_STUB
static struct desk_win_s *demo_open(void)
{
  struct desk_win_s *w;
  struct demo_s *d;
  int i;

  d = calloc(1, sizeof(*d));
  if (d == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_WIDGETS, "Widgets", DEMO_W, DEMO_H);
  if (w == NULL)
    {
      free(d);
      return NULL;
    }

  w->priv = d;
  d->cx = -1;
  for (i = 0; i < NROWS; i++)
    {
      d->order[i] = i;
    }

  d->side.n        = 6;
  d->side.sel      = 0;
  d->side.text     = side_text;
  d->side.icon     = side_icon;
  d->side.sortcol  = -1;
  d->table.n       = NROWS;
  d->table.sel     = -1;
  d->table.ncols   = 3;
  d->table.cols    = g_cols;
  d->table.text    = table_text;
  d->table.icon    = table_icon;
  d->table.sortcol = 0;
  d->table.empty   = "Nothing here";
  ui_field_set(&d->field[1], "Read only");
  if (kit_surface_open(&d->pic, 132, 96, w) >= 0)
    {
      pic_draw(d);
    }

  strlcpy(d->status, "Every control of the kit, in each of its states",
          sizeof(d->status));

  demo_build(d, w);
  kit_layout(&d->kit);
  kit_flush(&d->kit);
  return w;
}

#endif /* !DESK_PROGRAM_STUB */

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef DESK_PROGRAM_STUB
DESK_PROGRAM_APP(g_app_widgets, "Widgets", demo_icon, "widgets", true);
#else
const struct desk_app_s g_app_widgets =
{
  .name   = "Widgets",
  .icon   = demo_icon,
  .open   = demo_open,
  .draw   = demo_draw_all,
  .expose = demo_expose,
  .resize = demo_resize,
  .tick   = demo_tick,
  .mouse  = demo_mouse,
  .key    = demo_key,
  .focus  = demo_focus,
  .wake   = demo_wake,
  .close  = demo_close,
  .single = true,
};
#endif

#endif /* CONFIG_FPCDESK_WIDGETS */
