/****************************************************************************
 * apps/graphics/fpcdesk/desk_ui.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small pieces shared by the document applications: clipping lists of
 * operations (the GPU clips only to the window: a list scrolled by a few
 * pixels draws its rows cut to the list), buttons, a text field, keys,
 * and the desktop's clipboard.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdlib.h>
#include <string.h>

#include "desk_ui.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static char *g_clip;
static size_t g_cliplen;

/****************************************************************************
 * Public Functions: operations
 ****************************************************************************/

/* Operations are rectangles with, for images, a source position that moves
 * with them; shapes (LUT, ramps) are in absolute coordinates, so cutting a
 * rectangle leaves its pixels as they were
 */

void ui_clip(struct desk_ops_s *o, int from, int x0, int y0, int x1,
             int y1)
{
  int i;
  int j = from;

  for (i = from; i < o->n; i++)
    {
      struct nx_gpuop_s op = o->op[i];
      int d;

      if ((d = x0 - op.x) > 0)
        {
          op.x += d;
          op.w -= d;
          op.sx += d;
        }

      if ((d = y0 - op.y) > 0)
        {
          op.y += d;
          op.h -= d;
          op.sy += d;
        }

      if (op.x + op.w > x1)
        {
          op.w = x1 - op.x;
        }

      if (op.y + op.h > y1)
        {
          op.h = y1 - op.y;
        }

      if (op.w > 0 && op.h > 0)
        {
          o->op[j++] = op;
        }
    }

  o->n = j;
}

void ui_flush(NXWINDOW hwnd, struct desk_ops_s *o, int need)
{
  if (o->n + need > NX_GPUOPS_MAX)
    {
      ops_draw(hwnd, o);
    }
}

int ui_fit(const struct desk_font_s *f, const char *s, int maxw, char *buf,
           int size)
{
  int ell = text_width(f, DESK_CH_ELLIPSIS);
  int w = 0;
  int i;

  if (text_width(f, s) <= maxw)
    {
      strlcpy(buf, s, size);
      return text_width(f, buf);
    }

  for (i = 0; s[i] != '\0' && i < size - 2; i++)
    {
      char c[2] =
        {
          s[i], '\0'
        };

      int cw = text_width(f, c);

      if (w + cw + ell > maxw)
        {
          break;
        }

      buf[i] = s[i];
      w += cw;
    }

  buf[i] = DESK_CH_ELLIPSIS[0];
  buf[i + 1] = '\0';
  return w + ell;
}

int ui_button_width(const char *label)
{
  return text_width(&g_desk_font_ui, label) + 24;
}

void ui_button(struct desk_ops_s *o, int x, int y, int w, int h,
               const char *label, int look)
{
  uint16_t bg;
  uint16_t fg = g_theme.control_text;

  switch (look)
    {
      case UI_HOVER:
        bg = C_UI_HOVER;
        break;

      case UI_PRESSED:
        bg = C_UI_PRESSED;
        break;

      case UI_DEFAULT:
        bg = C_ACCENT;
        fg = C_ACCENT_TEXT;
        break;

      case UI_DANGER:
        bg = C_UI_DANGER;
        fg = C_WHITE;
        break;

      case UI_DISABLED:
        bg = C_UI_BUTTON;
        fg = C_TEXT_DIM;
        break;

      default:
        bg = C_UI_BUTTON;
        break;
    }

  ops_rrect(o, x, y, w, h, LUT_R6, 6, bg, 255);
  if (label != NULL && label[0] != '\0')
    {
      ops_text_centre(o, &g_desk_font_ui, x + w / 2,
                      y + (h - text_height(&g_desk_font_ui)) / 2, label, fg,
                      look == UI_DISABLED ? 160 : 255);
    }
}

void ui_triangle(struct desk_ops_s *o, int cx, int cy, int size, int dir,
                 uint16_t c, uint8_t alpha)
{
  int i;

  /* Rows (or columns) of 1 pixel narrowing by 2 */

  for (i = 0; i < size; i++)
    {
      int len = 2 * (size - i) - 1;

      switch (dir)
        {
          case 0:
            ops_blend(o, cx - len / 2, cy + size / 2 - i, len, 1, c, alpha);
            break;

          case 1:
            ops_blend(o, cx - len / 2, cy - size / 2 + i, len, 1, c, alpha);
            break;

          case 2:
            ops_blend(o, cx + size / 2 - i, cy - len / 2, 1, len, c, alpha);
            break;

          default:
            ops_blend(o, cx - size / 2 + i, cy - len / 2, 1, len, c, alpha);
            break;
        }
    }
}

/****************************************************************************
 * Public Functions: zones
 ****************************************************************************/

void ui_zones_clear(struct ui_zones_s *zs, int group)
{
  int i;
  int j = 0;

  for (i = 0; i < zs->n; i++)
    {
      if (zs->z[i].group != group)
        {
          zs->z[j++] = zs->z[i];
        }
    }

  zs->n = j;
}

void ui_zone_add(struct ui_zones_s *zs, int group, int id, int x, int y,
                 int w, int h, bool enabled)
{
  struct ui_zone_s *z;

  if (zs->n == UI_ZONES_MAX)
    {
      return;
    }

  z = &zs->z[zs->n++];
  z->x = x;
  z->y = y;
  z->w = w;
  z->h = h;
  z->id = id;
  z->group = group;
  z->enabled = enabled;
}

struct ui_zone_s *ui_zone_at(struct ui_zones_s *zs, int x, int y)
{
  int i;

  for (i = zs->n - 1; i >= 0; i--)
    {
      struct ui_zone_s *z = &zs->z[i];

      if (x >= z->x && x < z->x + z->w && y >= z->y && y < z->y + z->h)
        {
          return z;
        }
    }

  return NULL;
}

int ui_zone_group(struct ui_zones_s *zs, int id)
{
  int i;

  for (i = 0; id != 0 && i < zs->n; i++)
    {
      if (zs->z[i].id == id)
        {
          return zs->z[i].group;
        }
    }

  return -1;
}

int ui_look(struct ui_zones_s *zs, int id, bool enabled)
{
  if (!enabled)
    {
      return UI_DISABLED;
    }

  if (zs->pressed == id && zs->hot == id)
    {
      return UI_PRESSED;
    }

  return zs->hot == id ? UI_HOVER : UI_NORMAL;
}

/****************************************************************************
 * Public Functions: keys
 ****************************************************************************/

/* A complete sequence: ESC [ params final, or ESC O final */

static int key_decode(struct ui_keys_s *k, uint8_t final)
{
  int p[2] =
    {
      0, 0
    };

  int np = 0;
  int key = -1;
  int mod = 0;
  int i;

  for (i = 2; i < k->n; i++)
    {
      if (k->seq[i] >= '0' && k->seq[i] <= '9')
        {
          p[np] = p[np] * 10 + k->seq[i] - '0';
        }
      else if (k->seq[i] == ';' && np < 1)
        {
          np++;
        }
    }

  switch (final)
    {
      case 'A':
        key = KEY_UP;
        break;

      case 'B':
        key = KEY_DOWN;
        break;

      case 'C':
        key = KEY_RIGHT;
        break;

      case 'D':
        key = KEY_LEFT;
        break;

      case 'H':
        key = KEY_HOME;
        break;

      case 'F':
        key = KEY_END;
        break;

      case 'Q':
        key = KEY_F2;
        break;

      case 'Z':
        key = KEY_SHIFT | '\t';
        break;

      case '~':
        switch (p[0])
          {
            case 1:
            case 7:
              key = KEY_HOME;
              break;

            case 2:
              key = KEY_INS;
              break;

            case 3:
              key = KEY_DEL;
              break;

            case 4:
            case 8:
              key = KEY_END;
              break;

            case 5:
              key = KEY_PGUP;
              break;

            case 6:
              key = KEY_PGDN;
              break;

            case 12:
              key = KEY_F2;
              break;

            case 15:
              key = KEY_F5;
              break;

            default:
              break;
          }
        break;

      default:
        break;
    }

  /* Modifiers: 1 + (1 shift, 4 ctrl) */

  if (p[1] > 1)
    {
      mod = p[1] - 1;
    }

  if (key >= 0)
    {
      key |= (mod & 1 ? KEY_SHIFT : 0) | (mod & 4 ? KEY_CTRL : 0);
    }

  return key;
}

int ui_keys(struct ui_keys_s *k, const uint8_t *ch, int n, int *keys,
            int max)
{
  int nk = 0;
  int i;

  for (i = 0; i < n && nk < max; i++)
    {
      uint8_t c = ch[i];

      if (k->n == 1)
        {
          if (c == '[' || c == 'O')
            {
              k->seq[k->n++] = c;
              continue;
            }

          /* Escape on its own, then c */

          keys[nk++] = KEY_ESC;
          k->n = 0;
          if (nk == max)
            {
              break;
            }
        }
      else if (k->n > 1)
        {
          if (c >= 0x40 && c <= 0x7e && !(k->n == 2 && k->seq[1] == '[' &&
                                          c == '['))
            {
              int key = key_decode(k, c);

              if (key >= 0)
                {
                  keys[nk++] = key;
                }

              k->n = 0;
            }
          else if (k->n < sizeof(k->seq))
            {
              k->seq[k->n++] = c;
            }
          else
            {
              k->n = 0;                          /* Not one we know */
            }

          continue;
        }

      if (c == 0x1b)
        {
          k->seq[0] = c;
          k->n = 1;
          continue;
        }

      keys[nk++] = c;
    }

  /* Escape at the end of a short burst (the NX keyboard's events carry up
   * to 8 bytes: a sequence continues only after a full one) is the key
   */

  if (k->n == 1 && n < 8 && nk < max)
    {
      keys[nk++] = KEY_ESC;
      k->n = 0;
    }

  return nk;
}

/****************************************************************************
 * Public Functions: text fields
 ****************************************************************************/

void ui_field_set(struct ui_field_s *f, const char *s)
{
  strlcpy(f->text, s, sizeof(f->text));
  f->len = strlen(f->text);
  f->cur = f->len;
}

bool ui_field_key(struct ui_field_s *f, int key)
{
  switch (key)
    {
      case KEY_LEFT:
        f->cur -= f->cur > 0;
        return true;

      case KEY_RIGHT:
        f->cur += f->cur < f->len;
        return true;

      case KEY_HOME:
      case KEY_CTL('a'):
        f->cur = 0;
        return true;

      case KEY_END:
      case KEY_CTL('e'):
        f->cur = f->len;
        return true;

      case 0x7f:
      case 0x08:
        if (f->cur > 0)
          {
            memmove(f->text + f->cur - 1, f->text + f->cur,
                    f->len - f->cur + 1);
            f->cur--;
            f->len--;
          }
        return true;

      case KEY_DEL:
        if (f->cur < f->len)
          {
            memmove(f->text + f->cur, f->text + f->cur + 1,
                    f->len - f->cur);
            f->len--;
          }
        return true;

      case KEY_CTL('u'):
        f->len = 0;
        f->cur = 0;
        f->text[0] = '\0';
        return true;

      case KEY_CTL('v'):
        {
          size_t n;
          const char *s = desk_clip_get(&n);
          size_t i;

          for (i = 0; s != NULL && i < n && s[i] != '\n' && s[i] != '\r';
               i++)
            {
              ui_field_key(f, (uint8_t)s[i]);
            }
        }
        return true;

      default:
        if (key >= 0x20 && key <= 0xff && key != 0x7f &&
            f->len < (int)sizeof(f->text) - 1)
          {
            memmove(f->text + f->cur + 1, f->text + f->cur,
                    f->len - f->cur + 1);
            f->text[f->cur++] = (char)key;
            f->len++;
            return true;
          }
        return false;
    }
}

void ui_field_draw(struct desk_ops_s *o, struct ui_field_s *f, int x, int y,
                   int w, int h, bool focus)
{
  const struct desk_font_s *font = &g_desk_font_ui;
  int room = w - 16;
  int from = 0;
  int tx = x + 8;
  int ty = y + (h - text_height(font)) / 2;
  int cx = tx;
  int start;
  int i;

  ops_rrect(o, x, y, w, h, LUT_R6, 6, focus ? C_ACCENT : C_UI_HOVER, 255);
  ops_rrect(o, x + 1, y + 1, w - 2, h - 2, LUT_R6, 5, C_UI_FIELD, 255);

  /* Scroll so that the cursor shows */

  for (; ; )
    {
      int cw = 0;

      for (i = from; i < f->cur; i++)
        {
          char c[2] =
            {
              f->text[i], '\0'
            };

          cw += text_width(font, c);
        }

      if (cw <= room || from >= f->cur)
        {
          break;
        }

      from++;
    }

  start = o->n;
  for (i = from; i < f->len; i++)
    {
      char c[2] =
        {
          f->text[i], '\0'
        };

      if (i == f->cur)
        {
          cx = tx;
        }

      if (tx - (x + 8) > room)
        {
          break;
        }

      tx += ops_text(o, font, tx, ty, c, C_TEXT, 255);
    }

  if (f->cur >= i)
    {
      cx = tx;
    }

  ui_clip(o, start, x + 4, y, x + w - 4, y + h);
  if (focus)
    {
      ops_fill(o, cx, y + 4, 1, h - 8, C_TEXT);
    }
}

/****************************************************************************
 * Public Functions: the clipboard
 ****************************************************************************/

void desk_clip_set(const char *s, size_t n)
{
  char *c = malloc(n + 1);

  if (c == NULL)
    {
      return;
    }

  memcpy(c, s, n);
  c[n] = '\0';
  free(g_clip);
  g_clip    = c;
  g_cliplen = n;
}

const char *desk_clip_get(size_t *n)
{
  *n = g_cliplen;
  return g_clip;
}
