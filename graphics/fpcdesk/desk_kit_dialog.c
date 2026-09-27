/****************************************************************************
 * apps/graphics/fpcdesk/desk_kit_dialog.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The widget kit's dialogs (desk_kit.h, kit_dialog()): a message, a
 * question, a prompt for a line of text, and a file to open or to save
 * as.  A dialog is a kit of its own (its widgets allocated when it opens)
 * laid out in a card in the middle of the window; while it is open it
 * has the window's pointer and keys.  Its answer goes to the window's
 * kit as KIT_EV_DIALOG, after it has closed (so the answer may open
 * another).
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "desk_kit.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define DLG_MAX        24                /* Widgets */
#define DLG_W          400               /* A question's width */
#define DLG_FILE_W     520               /* A file dialog's size */
#define DLG_FILE_H     400
#define DLG_MARGIN     12                /* At least, around it */

/* Its widgets */

enum
{
  D_NONE = 0,
  D_TITLE,
  D_TEXT,
  D_FIELD,
  D_UP,
  D_PATH,
  D_LIST,
  D_OK,
  D_CANCEL,
  D_ALT
};

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct dlg_ent_s
{
  char *name;
  bool dir;
};

struct dlg_s
{
  struct kit_s kit;                      /* The dialog's own kit */
  struct kit_widget_s wg[DLG_MAX];
  struct dlg_s *dead;                    /* Closed, to be freed */
  int id;
  int kind;
  char title[64];
  char text[160];
  struct ui_field_s field;

  /* Files */

  char dir[PATH_MAX];
  struct dlg_ent_s *ent;
  int n;
  struct kit_list_s list;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct dlg_s *g_dead;             /* Closed dialogs */
static char g_answer[PATH_MAX];

/****************************************************************************
 * Private Functions: folders
 ****************************************************************************/

static void ents_free(struct dlg_s *d)
{
  int i;

  for (i = 0; i < d->n; i++)
    {
      free(d->ent[i].name);
    }

  free(d->ent);
  d->ent = NULL;
  d->n = 0;
}

static int ent_cmp(const void *a, const void *b)
{
  const struct dlg_ent_s *x = a;
  const struct dlg_ent_s *y = b;

  if (x->dir != y->dir)
    {
      return x->dir ? -1 : 1;
    }

  return strcasecmp(x->name, y->name);
}

static void path_join(char *out, const char *dir, const char *name)
{
  snprintf(out, PATH_MAX, "%s%s%s", dir,
           dir[0] != '\0' && dir[strlen(dir) - 1] == '/' ? "" : "/", name);
}

static bool is_dir(const char *path)
{
  struct stat st;

  return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

/* Read the folder: its folders first, then its files, by name (names
 * starting with a dot are left out)
 */

static void dir_load(struct dlg_s *d)
{
  struct dirent *de;
  int cap = 0;
  DIR *dp;

  ents_free(d);
  dp = opendir(d->dir);
  while (dp != NULL && (de = readdir(dp)) != NULL)
    {
      struct dlg_ent_s *e;

      if (de->d_name[0] == '.')
        {
          continue;
        }

      if (d->n == cap)
        {
          int ncap = cap ? cap * 2 : 32;
          struct dlg_ent_s *ne = realloc(d->ent, ncap * sizeof(*ne));

          if (ne == NULL)
            {
              break;
            }

          d->ent = ne;
          cap = ncap;
        }

      e = &d->ent[d->n];
      e->name = strdup(de->d_name);
      if (e->name == NULL)
        {
          break;
        }

      e->dir = de->d_type == DT_DIR;
      d->n++;
    }

  if (dp != NULL)
    {
      closedir(dp);
    }

  if (d->n > 1)
    {
      qsort(d->ent, d->n, sizeof(d->ent[0]), ent_cmp);
    }

  d->list.n   = d->n;
  d->list.sel = -1;
  d->list.top = 0;
}

static void dlg_update(struct dlg_s *d);

static void dir_go(struct dlg_s *d, const char *path, const char *select)
{
  int i;

  if (path != d->dir)
    {
      strlcpy(d->dir, path, sizeof(d->dir));
    }

  dir_load(d);
  for (i = 0; select != NULL && i < d->n; i++)
    {
      if (strcmp(d->ent[i].name, select) == 0)
        {
          d->list.sel = i;
        }
    }

  kit_set_text(&d->kit, D_PATH, d->dir);
  kit_list_changed(&d->kit, D_LIST);
  kit_list_show(&d->kit, D_LIST, d->list.sel);
  kit_enable(&d->kit, D_UP, strcmp(d->dir, "/") != 0);
  dlg_update(d);
}

static void dir_up(struct dlg_s *d)
{
  char child[NAME_MAX + 1];
  char *s = strrchr(d->dir, '/');

  if (s == NULL || strcmp(d->dir, "/") == 0)
    {
      return;
    }

  strlcpy(child, s + 1, sizeof(child));
  if (s == d->dir)
    {
      s[1] = '\0';
    }
  else
    {
      *s = '\0';
    }

  dir_go(d, d->dir, child);
}

/****************************************************************************
 * Private Functions: the dialog
 ****************************************************************************/

static const char *row_text(struct kit_s *k, struct kit_widget_s *wg,
                            int row, int col, char *buf, int size)
{
  struct dlg_s *d = k->priv;

  return row < d->n ? d->ent[row].name : NULL;
}

static void row_icon(struct kit_s *k, struct kit_widget_s *wg,
                     struct desk_ops_s *o, int row, int x, int y,
                     uint16_t fg, uint16_t bg)
{
  struct dlg_s *d = k->priv;

  if (row < d->n)
    {
      kit_paint_icon(o, d->ent[row].dir ? KIT_ICON_FOLDER : KIT_ICON_FILE,
                     x, y, fg, bg);
    }
}

/* The default button: can it be pressed? */

static void dlg_update(struct dlg_s *d)
{
  bool ok = true;

  switch (d->kind)
    {
      case KIT_DLG_PROMPT:
      case KIT_DLG_SAVE:
        ok = d->field.len > 0;
        break;

      case KIT_DLG_OPEN:
        ok = d->list.sel >= 0;
        break;

      default:
        break;
    }

  kit_enable(&d->kit, D_OK, ok);
}

static void dlg_answer(struct dlg_s *d, int ans)
{
  struct kit_s *k = d->kit.parent;
  int id = d->id;

  g_answer[0] = '\0';
  if (ans != KIT_ANS_CANCEL)
    {
      switch (d->kind)
        {
          case KIT_DLG_PROMPT:
            strlcpy(g_answer, d->field.text, sizeof(g_answer));
            break;

          case KIT_DLG_OPEN:
            if (d->list.sel >= 0 && d->list.sel < d->n)
              {
                path_join(g_answer, d->dir, d->ent[d->list.sel].name);
              }
            break;

          case KIT_DLG_SAVE:
            if (d->field.text[0] == '/')
              {
                strlcpy(g_answer, d->field.text, sizeof(g_answer));
              }
            else
              {
                path_join(g_answer, d->dir, d->field.text);
              }
            break;

          default:
            break;
        }
    }

  kit_dialog_close(k);
  k->key    = id;
  k->arg    = ans;
  k->answer = g_answer;
  if (k->event != NULL)
    {
      k->event(k, NULL, KIT_EV_DIALOG);
    }
}

/* The default button (or Enter): for files, a folder opens */

static void dlg_ok(struct dlg_s *d)
{
  char path[PATH_MAX];

  switch (d->kind)
    {
      case KIT_DLG_OPEN:
        if (d->list.sel < 0 || d->list.sel >= d->n)
          {
            return;
          }

        if (d->ent[d->list.sel].dir)
          {
            path_join(path, d->dir, d->ent[d->list.sel].name);
            dir_go(d, path, NULL);
            return;
          }
        break;

      case KIT_DLG_SAVE:
        if (d->field.len == 0)
          {
            return;
          }

        if (d->field.text[0] == '/')
          {
            strlcpy(path, d->field.text, sizeof(path));
          }
        else
          {
            path_join(path, d->dir, d->field.text);
          }

        if (is_dir(path))
          {
            ui_field_set(&d->field, "");
            kit_invalidate(&d->kit, D_FIELD);
            dir_go(d, path, NULL);
            return;
          }
        break;

      case KIT_DLG_PROMPT:
        if (d->field.len == 0)
          {
            return;
          }
        break;

      default:
        break;
    }

  dlg_answer(d, KIT_ANS_OK);
}

static void dlg_event(struct kit_s *dk, struct kit_widget_s *wg, int ev)
{
  struct dlg_s *d = dk->priv;
  int id = wg != NULL ? wg->id : 0;

  switch (ev)
    {
      case KIT_EV_CLICK:
        if (id == D_OK)
          {
            dlg_ok(d);
          }
        else if (id == D_CANCEL)
          {
            dlg_answer(d, KIT_ANS_CANCEL);
          }
        else if (id == D_ALT)
          {
            dlg_answer(d, KIT_ANS_ALT);
          }
        else if (id == D_UP)
          {
            dir_up(d);
          }
        break;

      case KIT_EV_CHANGE:
        if (id == D_LIST && d->kind == KIT_DLG_SAVE && d->list.sel >= 0 &&
            !d->ent[d->list.sel].dir)
          {
            ui_field_set(&d->field, d->ent[d->list.sel].name);
            kit_invalidate(dk, D_FIELD);
          }

        dlg_update(d);
        break;

      case KIT_EV_ACTIVATE:
        if (id == D_LIST && d->list.sel >= 0 && d->list.sel < d->n)
          {
            if (d->ent[d->list.sel].dir || d->kind == KIT_DLG_OPEN)
              {
                dlg_ok(d);
              }
            else
              {
                ui_field_set(&d->field, d->ent[d->list.sel].name);
                dlg_ok(d);
              }
          }
        else if (id == D_FIELD)
          {
            dlg_ok(d);
          }
        break;

      case KIT_EV_KEY:
        if (dk->key == KEY_ESC)
          {
            dk->key = 0;
            dlg_answer(d, d->kind == KIT_DLG_MESSAGE ? KIT_ANS_OK :
                       KIT_ANS_CANCEL);
          }
        else if (dk->key == 0x7f && d->kind == KIT_DLG_OPEN)
          {
            dk->key = 0;
            dir_up(d);                   /* Backspace: up */
          }
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* Free closed dialogs (the kit calls this when it is done with events) */

void kit_dialog_reap(void)
{
  while (g_dead != NULL)
    {
      struct dlg_s *d = g_dead;

      g_dead = d->dead;
      ents_free(d);
      free(d);
    }
}

void kit_dialog(struct kit_s *k, int id, int kind, const char *title,
                const char *text, const char * const *buttons, int n)
{
  static const char * const ok[] =
  {
    "OK"
  };

  static const char * const okcancel[] =
  {
    "OK", "Cancel"
  };

  static const char * const open[] =
  {
    "Open", "Cancel"
  };

  static const char * const save[] =
  {
    "Save", "Cancel"
  };

  bool danger = (kind & KIT_DLG_DANGER) != 0;
  bool files;
  struct kit_s *dk;
  struct dlg_s *d;
  int w;
  int h;

  kind &= ~KIT_DLG_DANGER;
  files = kind == KIT_DLG_OPEN || kind == KIT_DLG_SAVE;
  kit_dialog_close(k);
  kit_menu_close(k);

  d = calloc(1, sizeof(*d));
  if (d == NULL)
    {
      return;
    }

  if (buttons == NULL || n <= 0)
    {
      buttons = kind == KIT_DLG_MESSAGE ? ok : kind == KIT_DLG_OPEN ? open :
                kind == KIT_DLG_SAVE ? save : okcancel;
      n = kind == KIT_DLG_MESSAGE ? 1 : 2;
    }

  d->id   = id;
  d->kind = kind;
  strlcpy(d->title, title != NULL ? title : "", sizeof(d->title));
  if (!files && kind != KIT_DLG_PROMPT && text != NULL)
    {
      strlcpy(d->text, text, sizeof(d->text));
    }

  dk = &d->kit;
  kit_init(dk, k->w, d->wg, DLG_MAX, dlg_event);
  dk->parent = k;
  dk->priv   = d;

  kit_col(dk, KIT_BOX_DIALOG);
  kit_label(dk, D_TITLE, d->title, KIT_TEXT_BOLD);
  if (d->text[0] != '\0')
    {
      kit_label(dk, D_TEXT, d->text, KIT_TEXT_DIM);
    }

  if (kind == KIT_DLG_PROMPT)
    {
      ui_field_set(&d->field, text != NULL ? text : "");
      kit_field(dk, D_FIELD, &d->field, NULL);
    }

  if (files)
    {
      kit_row(dk, KIT_BOX_NONE)->gap = 6;
        kit_button(dk, D_UP, NULL, KIT_ICON_UP, KIT_BTN_NORMAL);
        kit_tip(dk, "The folder it is in");
        kit_label(dk, D_PATH, d->dir, KIT_TEXT_DIM);
        kit_grow(dk, 1);
      kit_end(dk);
      d->list.icon  = row_icon;
      d->list.text  = row_text;
      d->list.empty = "Folder is empty";
      d->list.sortcol = -1;
      kit_list(dk, D_LIST, &d->list, KIT_LIST_PLAIN);
      kit_grow(dk, 1);
      if (kind == KIT_DLG_SAVE)
        {
          kit_row(dk, KIT_BOX_NONE);
            kit_label(dk, 0, "Name", KIT_TEXT_DIM);
            kit_field(dk, D_FIELD, &d->field, "File name");
            kit_grow(dk, 1);
          kit_end(dk);
        }
    }

  kit_row(dk, KIT_BOX_NONE);
    if (n > 2)
      {
        kit_button(dk, D_ALT, buttons[2], KIT_ICON_NONE, KIT_BTN_NORMAL);
      }

    kit_spacer(dk, 1);
    if (n > 1)
      {
        kit_button(dk, D_CANCEL, buttons[1], KIT_ICON_NONE, KIT_BTN_NORMAL);
        kit_size(dk, 84, 0);
      }

    kit_button(dk, D_OK, buttons[0], KIT_ICON_NONE,
               danger ? KIT_BTN_DANGER : KIT_BTN_DEFAULT);
    kit_size(dk, 84, 0);
  kit_end(dk);
  kit_end(dk);

  /* In the middle of the window's kit */

  if (files)
    {
      w = DLG_FILE_W;
      h = DLG_FILE_H;
    }
  else
    {
      kit_natural(dk, &w, &h);
      w = w < DLG_W ? DLG_W : w;
    }

  w = w > k->area.w - 2 * DLG_MARGIN ? k->area.w - 2 * DLG_MARGIN : w;
  h = h > k->area.h - 2 * DLG_MARGIN ? k->area.h - 2 * DLG_MARGIN : h;
  k->dlg = dk;
  kit_place(dk, k->area.x + (k->area.w - w) / 2,
            k->area.y + (k->area.h - h) / 2, w, h);

  if (files)
    {
      char path[PATH_MAX];
      const char *name = NULL;
      char *s;

      /* A folder, or a file in one: start there */

      strlcpy(path, text != NULL && text[0] == '/' ? text : "/",
              sizeof(path));
      if (!is_dir(path) && (s = strrchr(path, '/')) != NULL)
        {
          name = text + (s - path) + 1;
          if (s == path)
            {
              s[1] = '\0';
            }
          else
            {
              *s = '\0';
            }

          if (kind == KIT_DLG_SAVE)
            {
              ui_field_set(&d->field, name);
            }
        }

      /* A folder that is not there: the nearest one that is */

      while (!is_dir(path) && (s = strrchr(path, '/')) != NULL)
        {
          if (s == path)
            {
              s[1] = '\0';
              break;
            }

          *s = '\0';
        }

      dir_go(d, is_dir(path) ? path : "/", name);
    }

  dlg_update(d);
  kit_focus(dk, kind == KIT_DLG_PROMPT || kind == KIT_DLG_SAVE ? D_FIELD :
            files ? D_LIST : D_OK);
  if ((kind == KIT_DLG_PROMPT || kind == KIT_DLG_SAVE) && d->field.len > 0)
    {
      kit_select_all(dk, D_FIELD);             /* Typing replaces it */
    }
  dk->ring = false;
  kit_flush(dk);
}

void kit_dialog_close(struct kit_s *k)
{
  struct dlg_s *d;
  struct kit_rect_s r;

  if (k->dlg == NULL)
    {
      return;
    }

  d = (struct dlg_s *)k->dlg;
  r = d->kit.area;
  k->dlg = NULL;
  d->dead = g_dead;
  g_dead = d;
  kit_damage(k, r.x, r.y, r.w, r.h);
}

bool kit_dialog_open(struct kit_s *k)
{
  return k->dlg != NULL;
}
