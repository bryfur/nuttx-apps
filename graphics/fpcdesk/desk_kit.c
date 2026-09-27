/****************************************************************************
 * apps/graphics/fpcdesk/desk_kit.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The widget kit (desk_kit.h, docs/uikit.md): building the tree of
 * widgets, laying it out, finding what the pointer is on, the focus and
 * the keys, and drawing what changed.
 *
 * The widgets are an array in tree order: a box is followed by what it
 * holds, and its end says where that stops.  Nothing points anywhere, so
 * a window's widgets are one allocation of the application's.
 *
 * Drawing: a change marks a widget (KIT_F_DIRTY), a part of a list (its
 * rows, its header, its scroll bar), or a rectangle (damage: what a menu
 * or a dialog uncovered, an expose).  kit_flush() draws them in tree
 * order, each cut to its own rectangle (ui_clip()), into one list of
 * operations that goes to the window in as few nx_gpuops() calls as it
 * takes; then it draws again any menu, tooltip or dialog over what it
 * drew.  A list scrolls with a COPY within the window and draws only the
 * rows that come into view.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/param.h>

#include "desk_kit.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ROOM          200              /* Operations a widget may need */
#define PART_NONE     (-1)
#define PART_SB       (-2)             /* A list's scroll bar */
#define PART_THUMB    (-3)             /* ... its thumb */
#define PART_HEAD     (-10)            /* A list's column: PART_HEAD - col */
#define MAX_COLS      8
#define MAX_CRUMBS    16
#define CRUMB_SEP     14               /* The "/" between crumbs */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* Where a list's parts are */

struct lgeom_s
{
  int hh;                              /* Header height, 0: none */
  int rx;                              /* The rows */
  int ry;
  int rw;
  int rh;
  int sbw;                             /* Scroll bar width, 0: none */
  int rowh;
  int maxtop;
  int ncols;
  int16_t cx[MAX_COLS];                /* Columns (cw 0: left out) */
  int16_t cw[MAX_COLS];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct desk_ops_s *g_kops;     /* Every kit's (the main thread's),
                                        * from the heap (PSRAM on the
                                        * board): the first kit makes it */
static NXWINDOW g_khwnd;               /* ... the window it is for */
static struct kit_rect_s g_cut;        /* What is being drawn is cut to */
static int g_cutfrom = -1;             /* ... from this operation (-1: no) */
static bool g_dropped;                 /* Told once */
static struct kit_widget_s g_spare;    /* When a kit's array is full */

/****************************************************************************
 * Private Functions: the list of operations
 ****************************************************************************/

/* Everything drawn goes into g_kops, for the window of the kit drawing.
 * What a widget draws is cut to a rectangle (cut_begin() to cut_end());
 * when the list fills up in between, what is there is cut first, then
 * sent, so nothing ever lands outside it.  need() makes room: every
 * drawing asks for what it may add (a character is an operation), so
 * none is dropped (ops_add() would drop it quietly: that is reported).
 */

static void send(void)
{
  if (g_kops->n >= NX_GPUOPS_MAX && !g_dropped)
    {
      g_dropped = true;
      syslog(LOG_WARNING, "desk_kit: operations dropped (list full)\n");
    }

  if (g_khwnd != NULL)
    {
      ops_draw(g_khwnd, g_kops);
    }

  ops_reset(g_kops);
}

static void target(struct kit_s *k)
{
  NXWINDOW hwnd = (NXWINDOW)k->w->hwnd;

  if (hwnd != g_khwnd)
    {
      if (g_kops->n > 0)
        {
          send();
        }

      g_khwnd = hwnd;
    }
}

static void cut_begin(struct kit_s *k, const struct kit_rect_s *c)
{
  target(k);
  g_cut     = *c;
  g_cutfrom = g_kops->n;
}

static void cut_end(void)
{
  if (g_cutfrom >= 0)
    {
      ui_clip(g_kops, g_cutfrom, g_cut.x, g_cut.y, g_cut.x + g_cut.w,
              g_cut.y + g_cut.h);
      g_cutfrom = -1;
    }
}

static void need(struct kit_s *k, int n)
{
  target(k);
  if (g_kops->n + n > NX_GPUOPS_MAX)
    {
      bool cutting = g_cutfrom >= 0;

      cut_end();
      send();
      g_cutfrom = cutting ? 0 : -1;
    }
}

static void need_text(struct kit_s *k, const char *s)
{
  need(k, (s != NULL ? strlen(s) : 0) + 4);
}

/* Everything drawn so far goes to the window */

static void kit_send(struct kit_s *k)
{
  target(k);
  cut_end();
  send();
}

/****************************************************************************
 * Private Functions: rectangles
 ****************************************************************************/

static bool r_cross(const struct kit_rect_s *a, const struct kit_rect_s *b)
{
  return a->w > 0 && a->h > 0 && b->w > 0 && b->h > 0 &&
         a->x < b->x + b->w && b->x < a->x + a->w &&
         a->y < b->y + b->h && b->y < a->y + a->h;
}

static struct kit_rect_s r_and(const struct kit_rect_s *a,
                               const struct kit_rect_s *b)
{
  struct kit_rect_s r;
  int x1 = MIN(a->x + a->w, b->x + b->w);
  int y1 = MIN(a->y + a->h, b->y + b->h);

  r.x = MAX(a->x, b->x);
  r.y = MAX(a->y, b->y);
  r.w = x1 > r.x ? x1 - r.x : 0;
  r.h = y1 > r.y ? y1 - r.y : 0;
  return r;
}

static void r_add(struct kit_rect_s *a, const struct kit_rect_s *b)
{
  int x1;
  int y1;

  if (b->w <= 0 || b->h <= 0)
    {
      return;
    }

  if (a->w <= 0 || a->h <= 0)
    {
      *a = *b;
      return;
    }

  x1 = MAX(a->x + a->w, b->x + b->w);
  y1 = MAX(a->y + a->h, b->y + b->h);
  a->x = MIN(a->x, b->x);
  a->y = MIN(a->y, b->y);
  a->w = x1 - a->x;
  a->h = y1 - a->y;
}

static struct kit_rect_s r_make(int x, int y, int w, int h)
{
  struct kit_rect_s r;

  r.x = x;
  r.y = y;
  r.w = w;
  r.h = h;
  return r;
}

static struct kit_rect_s r_of(const struct kit_widget_s *wg)
{
  return r_make(wg->x, wg->y, wg->w, wg->h);
}

/* a is all inside b */

static bool r_within(const struct kit_rect_s *a, const struct kit_rect_s *b)
{
  return a->x >= b->x && a->y >= b->y && a->x + a->w <= b->x + b->w &&
         a->y + a->h <= b->y + b->h;
}

static bool r_has(const struct kit_rect_s *r, int x, int y)
{
  return x >= r->x && x < r->x + r->w && y >= r->y && y < r->y + r->h;
}

/****************************************************************************
 * Private Functions: the tree
 ****************************************************************************/

/* The box holding widget i, or -1 */

static int parent_of(struct kit_s *k, int i)
{
  int j;

  for (j = i - 1; j >= 0; j--)
    {
      if (k->wg[j].type == KIT_BOX && k->wg[j].end > i)
        {
          return j;
        }
    }

  return -1;
}

/* Shown: it and every box it is in */

static bool shown(struct kit_s *k, int i)
{
  for (; i >= 0; i = parent_of(k, i))
    {
      if (k->wg[i].flags & KIT_F_HIDDEN)
        {
          return false;
        }
    }

  return true;
}

static int index_of(struct kit_s *k, struct kit_widget_s *wg)
{
  return wg >= k->wg && wg < k->wg + k->n ? wg - k->wg : KIT_NONE;
}

static struct kit_widget_s *get(struct kit_s *k, int i)
{
  return i != KIT_NONE && i < k->n ? &k->wg[i] : NULL;
}

/* The colour under a widget: its nearest box's that has one */

static uint16_t box_colour(struct kit_s *k, struct kit_widget_s *b, bool *has)
{
  *has = true;
  switch (b->style)
    {
      case KIT_BOX_WINDOW:
        return g_theme.window_bg;

      case KIT_BOX_BAR:
      case KIT_BOX_STATUS:
        return g_theme.bar;

      case KIT_BOX_SIDEBAR:
        return g_theme.sidebar;

      case KIT_BOX_CARD:
      case KIT_BOX_GROUP:
      case KIT_BOX_DIALOG:
        return g_theme.card;

      default:
        *has = false;
        return g_theme.window_bg;
    }
}

static uint16_t ground(struct kit_s *k, int i)
{
  bool has;

  for (i = parent_of(k, i); i >= 0; i = parent_of(k, i))
    {
      uint16_t c = box_colour(k, &k->wg[i], &has);

      if (has)
        {
          return c;
        }
    }

  return k->parent != NULL ? g_theme.card : g_theme.window_bg;
}

static bool enabled(struct kit_widget_s *wg)
{
  return !(wg->flags & KIT_F_DISABLED);
}

static bool focusable(struct kit_s *k, int i)
{
  struct kit_widget_s *wg = &k->wg[i];

  switch (wg->type)
    {
      case KIT_BUTTON:
      case KIT_CHECK_BOX:
      case KIT_RADIO_BTN:
      case KIT_SWITCH:
      case KIT_SLIDER:
      case KIT_FIELD:
      case KIT_DROPDOWN:
      case KIT_LIST:
      case KIT_TABS:
      case KIT_CUSTOM:
      case KIT_STEPPER:
      case KIT_SWATCHES:
        return !(wg->flags & (KIT_F_NOFOCUS | KIT_F_DISABLED)) &&
               wg->w > 0 && shown(k, i);

      default:
        return false;
    }
}

/* Controls that show a ring when focused (lists and fields show it their
 * own way)
 */

static bool has_ring(struct kit_widget_s *wg)
{
  switch (wg->type)
    {
      case KIT_BUTTON:
      case KIT_CHECK_BOX:
      case KIT_RADIO_BTN:
      case KIT_SWITCH:
      case KIT_SLIDER:
      case KIT_DROPDOWN:
      case KIT_TABS:
      case KIT_STEPPER:
      case KIT_SWATCHES:
        return wg->type != KIT_BUTTON || wg->style != KIT_BTN_CUSTOM;

      default:
        return false;
    }
}

static bool ringed(struct kit_s *k, int i)
{
  struct kit_widget_s *wg = &k->wg[i];

  if (!has_ring(wg) || (wg->type == KIT_TABS &&
                        wg->style == KIT_TABS_STRIP))
    {
      return false;
    }

  return (wg->flags & KIT_F_RING) != 0 ||
         (k->focus == i && k->ring && k->w->active && enabled(wg));
}

/* What a widget covers when drawn: its rectangle and its ring */

/* What of a scrolling box's contents shows: the box, and any it is in */

static struct kit_rect_s viewport(struct kit_s *k, int i,
                                  struct kit_rect_s r)
{
  int p;

  if (!k->scrolls)
    {
      return r;
    }

  for (p = parent_of(k, i); p >= 0; p = parent_of(k, p))
    {
      if (k->wg[p].flags & KIT_F_SCROLL)
        {
          struct kit_rect_s v = r_of(&k->wg[p]);

          r = r_and(&r, &v);
        }
    }

  return r;
}

static struct kit_rect_s extent(struct kit_s *k, int i)
{
  struct kit_rect_s r = r_of(&k->wg[i]);

  if (has_ring(&k->wg[i]))
    {
      r.x -= KIT_RING;
      r.y -= KIT_RING;
      r.w += 2 * KIT_RING;
      r.h += 2 * KIT_RING;
    }

  return viewport(k, i, r);
}

/* A scrolling box's thumb: false if all of it shows */

static bool box_thumb(struct kit_widget_s *wg, int *ty, int *th)
{
  int page = wg->min;
  int total = wg->max;

  if (!(wg->flags & KIT_F_SCROLL) || total <= page || wg->h <= 0)
    {
      return false;
    }

  *th = wg->h * page / total;
  *th = *th < KIT_THUMB_MIN ? KIT_THUMB_MIN : *th;
  *ty = wg->y + (wg->h - *th) * wg->value / (total - page);
  return true;
}

static void box_scroll_to(struct kit_s *k, int i, int v)
{
  struct kit_widget_s *wg = &k->wg[i];
  int range = wg->max - wg->min;

  range = range < 0 ? 0 : range;
  v = v < 0 ? 0 : v > range ? range : v;
  if (v != wg->value)
    {
      wg->value  = v;
      wg->flags |= KIT_F_RELAYOUT;      /* Placed again, drawn again */
    }
}

/* Widget i in view: every scrolling box it is in scrolled to it */

static void box_show(struct kit_s *k, int i)
{
  struct kit_widget_s *wg = &k->wg[i];
  int p;

  if (!k->scrolls)
    {
      return;
    }

  for (p = parent_of(k, i); p >= 0; p = parent_of(k, p))
    {
      struct kit_widget_s *b = &k->wg[p];

      if (!(b->flags & KIT_F_SCROLL))
        {
          continue;
        }

      if (wg->y < b->y)
        {
          box_scroll_to(k, p, b->value - (b->y - wg->y) - KIT_GAP);
        }
      else if (wg->y + wg->h > b->y + b->h)
        {
          box_scroll_to(k, p, b->value + (wg->y + wg->h - b->y - b->h) +
                        KIT_GAP);
        }
    }
}

static void dirty(struct kit_s *k, int i)
{
  if (i != KIT_NONE && i < k->n)
    {
      k->wg[i].flags |= KIT_F_DIRTY;
    }
}

static void emit(struct kit_s *k, int i, int ev)
{
  if (k->event != NULL)
    {
      k->event(k, i == KIT_NONE ? NULL : &k->wg[i], ev);
    }
}

/****************************************************************************
 * Private Functions: sizes and layout
 ****************************************************************************/

static const struct desk_font_s *label_font(int style)
{
  switch (style & KIT_TEXT_FONT)
    {
      case KIT_TEXT_BOLD:
        return KIT_FONT_BOLD;

      case KIT_TEXT_SMALL:
        return KIT_FONT_SMALL;

      case KIT_TEXT_HEADING:
        return KIT_FONT_HEADING;

      case KIT_TEXT_DISPLAY:
        return KIT_FONT_DISPLAY;

      default:
        return KIT_FONT;
    }
}

/* A label's text: its first line, and its caption (after a '\n') */

static const char *label_line(const char *text, char *buf, int size)
{
  const char *nl = text != NULL ? strchr(text, '\n') : NULL;

  if (nl == NULL)
    {
      return text;
    }

  snprintf(buf, size, "%.*s", (int)(nl - text), text);
  return buf;
}

static const char *label_caption(const char *text)
{
  const char *nl = text != NULL ? strchr(text, '\n') : NULL;

  return nl != NULL ? nl + 1 : NULL;
}

static int items_width(struct kit_widget_s *wg, const struct desk_font_s *f)
{
  const char * const *items = wg->data;
  int m = 0;
  int i;

  for (i = 0; items != NULL && i < wg->nitems; i++)
    {
      int tw = text_width(f, items[i]);

      m = tw > m ? tw : m;
    }

  return m;
}

static int group_title_h(void)
{
  return text_height(KIT_FONT_BOLD) + 8;
}

/* The size a widget would like */

static void measure(struct kit_s *k, int i, int *nw, int *nh)
{
  struct kit_widget_s *wg = &k->wg[i];
  int w = 0;
  int h = 0;

  switch (wg->type)
    {
      case KIT_BOX:
        {
          bool row = (wg->flags & KIT_F_ROW) != 0;
          int count = 0;
          int j;

          for (j = i + 1; j < wg->end; j = k->wg[j].end)
            {
              int cw;
              int ch;

              if (k->wg[j].flags & KIT_F_HIDDEN)
                {
                  continue;
                }

              measure(k, j, &cw, &ch);
              if (row)
                {
                  w += cw;
                  h  = ch > h ? ch : h;
                }
              else
                {
                  h += ch;
                  w  = cw > w ? cw : w;
                }

              count++;
            }

          if (count > 1)
            {
              *(row ? &w : &h) += wg->gap * (count - 1);
            }

          w += 2 * wg->padx;
          h += 2 * wg->pady;
          if (wg->style == KIT_BOX_GROUP)
            {
              h += group_title_h();
            }
        }
        break;

      case KIT_LABEL:
        {
          const struct desk_font_s *f = label_font(wg->style);
          const char *cap = label_caption(wg->text);
          char line[96];

          w = wg->text != NULL ?
              text_width(f, label_line(wg->text, line, sizeof(line))) : 0;
          h = text_height(f);
          if (cap != NULL)
            {
              w  = MAX(w, text_width(KIT_FONT_SMALL, cap));
              h += 1 + text_height(KIT_FONT_SMALL);
            }
        }
        break;

      case KIT_STEPPER:
        w = KIT_STEPPER_W;
        h = KIT_STEP;
        break;

      case KIT_SWATCHES:
        w = wg->nitems * KIT_SWATCH_DX;
        h = 2 * KIT_SWATCH_R + 4;
        break;

      case KIT_IMAGE:
        if (wg->data != NULL)
          {
            w = ((struct kit_surface_s *)wg->data)->w;
            h = ((struct kit_surface_s *)wg->data)->h;
          }
        break;

      case KIT_BUTTON:
        h = KIT_CONTROL_H;
        if (wg->text == NULL || wg->text[0] == '\0')
          {
            w = KIT_ICON_BTN_W;
          }
        else
          {
            w = text_width(KIT_FONT, wg->text) + 2 * KIT_BTN_PAD;
            if (wg->icon != KIT_ICON_NONE)
              {
                w += KIT_ICON + 6;
              }
          }
        break;

      case KIT_CHECK_BOX:
      case KIT_RADIO_BTN:
        w = KIT_CHECK + (wg->text != NULL ?
                         8 + text_width(KIT_FONT, wg->text) : 0);
        h = KIT_CHECK + 4;
        break;

      case KIT_SWITCH:
        w = KIT_SWITCH_W + (wg->text != NULL ?
                            12 + text_width(KIT_FONT, wg->text) : 0);
        h = KIT_SWITCH_H;
        break;

      case KIT_SLIDER:
        w = KIT_FIELD_W;
        h = KIT_SLIDER_H;
        break;

      case KIT_PROGRESS:
        w = KIT_FIELD_W;
        h = KIT_PROGRESS_H;
        break;

      case KIT_FIELD:
        w = KIT_FIELD_W;
        h = KIT_CONTROL_H;
        break;

      case KIT_DROPDOWN:
        w = items_width(wg, KIT_FONT) + 48;
        w = w < 100 ? 100 : w;
        h = KIT_CONTROL_H;
        break;

      case KIT_LIST:
        {
          struct kit_list_s *l = wg->data;

          w = 120;
          h = 3 * (l != NULL && l->rowh ? l->rowh : KIT_ROW_H);
        }
        break;

      case KIT_TABS:
        if (wg->style == KIT_TABS_SEGMENTS)
          {
            w = wg->nitems * (items_width(wg, KIT_FONT_BOLD) + 24);
            h = KIT_CONTROL_H;
          }
        else
          {
            w = 120;
            h = KIT_TAB_H;
          }
        break;

      case KIT_SCROLL:
        w = KIT_SB_W;
        h = 40;
        break;

      case KIT_CRUMBS:
        w = 120;
        h = KIT_CONTROL_H;
        break;

      case KIT_SEPARATOR:
        w = 1;
        h = 1;
        break;

      default:
        break;
    }

  *nw = wg->pw > 0 ? wg->pw : w;
  *nh = wg->ph > 0 ? wg->ph : h;
}

/* How a widget sits across its box */

static int cross_align(struct kit_widget_s *wg, bool row)
{
  if (wg->align != KIT_A_AUTO)
    {
      return wg->align;
    }

  switch (wg->type)
    {
      case KIT_BOX:
      case KIT_LIST:
      case KIT_CUSTOM:
      case KIT_SEPARATOR:
      case KIT_SCROLL:
      case KIT_SPACER:
        return KIT_A_FILL;

      case KIT_TABS:
        return wg->style == KIT_TABS_STRIP || !row ? KIT_A_FILL :
               KIT_A_CENTRE;

      case KIT_FIELD:
      case KIT_SLIDER:
      case KIT_PROGRESS:
      case KIT_CRUMBS:
      case KIT_LABEL:
      case KIT_SWITCH:
        return row ? KIT_A_CENTRE : KIT_A_FILL;

      default:
        return row ? KIT_A_CENTRE : KIT_A_START;
    }
}

static void place(struct kit_s *k, int i, int x, int y, int w, int h)
{
  struct kit_widget_s *wg = &k->wg[i];
  bool row;
  int ix;
  int iy;
  int iw;
  int ih;
  int sum = 0;
  int grow = 0;
  int count = 0;
  int room;
  int pos;
  int j;

  wg->x = x;
  wg->y = y;
  wg->w = w < 0 ? 0 : w;
  wg->h = h < 0 ? 0 : h;
  if (wg->type != KIT_BOX)
    {
      return;
    }

  row = (wg->flags & KIT_F_ROW) != 0;
  ix  = x + wg->padx;
  iy  = y + wg->pady;
  iw  = w - 2 * wg->padx;
  ih  = h - 2 * wg->pady;
  if (wg->style == KIT_BOX_GROUP)
    {
      iy += group_title_h();
      ih -= group_title_h();
    }

  for (j = i + 1; j < wg->end; j = k->wg[j].end)
    {
      int cw;
      int ch;

      if (k->wg[j].flags & KIT_F_HIDDEN)
        {
          continue;
        }

      measure(k, j, &cw, &ch);
      sum  += row ? cw : ch;
      grow += k->wg[j].grow;
      count++;
    }

  room = (row ? iw : ih) - sum - (count > 1 ? wg->gap * (count - 1) : 0);
  pos  = row ? ix : iy;

  /* A scrolling column: what does not fit is there, below, offset by its
   * value (min and max: what shows and all there is)
   */

  if ((wg->flags & KIT_F_SCROLL) && !row)
    {
      wg->min = h;
      wg->max = h - room;
      if (room < 0)
        {
          int range = -room;

          wg->value = wg->value > range ? range : wg->value < 0 ? 0 :
                      wg->value;
          pos  -= wg->value;
          room  = 0;
          iw   -= MAX(0, KIT_SB_W + 2 - wg->padx);
        }
      else
        {
          wg->value = 0;
        }
    }

  for (j = i + 1; j < wg->end; j = k->wg[j].end)
    {
      struct kit_widget_s *c = &k->wg[j];
      int cw;
      int ch;
      int m;
      int span;
      int n;
      int a;
      int off;

      if (c->flags & KIT_F_HIDDEN)
        {
          continue;
        }

      measure(k, j, &cw, &ch);
      m = row ? cw : ch;
      if (c->grow > 0 && grow > 0)
        {
          int share = room * c->grow / grow;

          room -= share;
          grow -= c->grow;
          m    += share;
          m     = m < 0 ? 0 : m;
        }

      /* Across */

      span = row ? ih : iw;
      n    = row ? ch : cw;
      a    = cross_align(c, row);
      if (a == KIT_A_FILL || n > span)
        {
          n = span;
        }

      off = a == KIT_A_END ? span - n : a == KIT_A_CENTRE ? (span - n) / 2 :
            0;

      if (row)
        {
          place(k, j, pos, iy + off, m, n);
        }
      else
        {
          place(k, j, ix + off, pos, n, m);
        }

      pos += m + wg->gap;
    }
}

/****************************************************************************
 * Private Functions: lists
 ****************************************************************************/

static void ground_paint(struct kit_s *k, int i, int x, int y, int w,
                         int h);

static void list_geom(struct kit_s *k, struct kit_widget_s *wg,
                      struct lgeom_s *g)
{
  struct kit_list_s *l = wg->data;
  int fixed = 0;
  int x;
  int c;

  g->hh   = wg->style == KIT_LIST_TABLE && l->ncols > 0 && l->cols != NULL &&
            l->cols[0].name != NULL ? KIT_HEAD_H : 0;
  g->sbw  = wg->style == KIT_LIST_SIDEBAR ? 0 : KIT_SB_W;
  g->rx   = wg->x;
  g->ry   = wg->y + g->hh;
  g->rw   = wg->w - g->sbw;
  g->rh   = wg->h - g->hh;
  g->rh   = g->rh < 0 ? 0 : g->rh;
  g->rowh = l->rowh ? l->rowh : wg->style == KIT_LIST_SIDEBAR ?
            KIT_SIDE_ROW_H : KIT_ROW_H;
  g->maxtop = l->n * g->rowh - g->rh;
  g->maxtop = g->maxtop < 0 ? 0 : g->maxtop;

  /* Columns: the fixed ones that fit, the rest to the one of width 0 */

  g->ncols = l->ncols > 0 && l->cols != NULL ? MIN(l->ncols, MAX_COLS) : 1;
  for (c = 0; c < g->ncols; c++)
    {
      const struct kit_col_s *col = l->ncols > 0 ? &l->cols[c] : NULL;

      g->cw[c] = 0;
      if (col != NULL && col->hide > 0 && g->rw < col->hide)
        {
          g->cw[c] = -1;                             /* Left out */
        }
      else if (col != NULL && col->w > 0)
        {
          g->cw[c] = col->w;
          fixed += col->w;
        }
    }

  x = g->rx;
  for (c = 0; c < g->ncols; c++)
    {
      if (g->cw[c] < 0)
        {
          g->cw[c] = 0;
        }
      else if (g->cw[c] == 0)
        {
          g->cw[c] = g->rw - fixed > 0 ? g->rw - fixed : 0;
          fixed = g->rw;                             /* Only one */
        }

      g->cx[c] = x;
      x += g->cw[c];
    }
}

static bool list_thumb(struct kit_widget_s *wg, struct lgeom_s *g, int *ty,
                       int *th)
{
  struct kit_list_s *l = wg->data;
  int total = l->n * g->rowh;

  if (total <= g->rh || g->rh <= 0 || g->sbw == 0)
    {
      return false;
    }

  *th = g->rh * g->rh / total;
  *th = *th < KIT_THUMB_MIN ? KIT_THUMB_MIN : *th;
  *ty = g->ry + (g->rh - *th) * l->top / g->maxtop;
  return true;
}

static void list_row_dirty(struct kit_widget_s *wg, int row)
{
  struct kit_list_s *l = wg->data;
  int j;

  if (row < 0 || l->alldirty)
    {
      return;
    }

  for (j = 0; j < l->ndirty; j++)
    {
      if (l->dirty[j] == row)
        {
          return;
        }
    }

  if (l->ndirty == sizeof(l->dirty) / sizeof(l->dirty[0]))
    {
      l->alldirty = true;
      return;
    }

  l->dirty[l->ndirty++] = row;
}

/* Rows of a list between y0 and y1, cut to clip */

static void list_rows_draw(struct kit_s *k, int i,
                           const struct kit_rect_s *clip, int row0,
                           int row1)
{
  struct kit_widget_s *wg = &k->wg[i];
  struct kit_list_s *l = wg->data;
  struct desk_ops_s *o = g_kops;
  const struct desk_font_s *f = KIT_FONT;
  uint16_t bg = ground(k, i);
  bool accent = wg->style == KIT_LIST_SIDEBAR ||
                (k->focus == i && k->w->active);
  struct kit_rect_s rows;
  struct kit_rect_s c;
  struct lgeom_s g;
  char buf[128];
  char fit[128];
  int r;

  list_geom(k, wg, &g);
  rows = r_make(g.rx, g.ry, g.rw, g.rh);
  c = r_and(clip, &rows);
  if (c.w <= 0 || c.h <= 0)
    {
      return;
    }

  if (row0 < (c.y - g.ry + l->top) / g.rowh)
    {
      row0 = (c.y - g.ry + l->top) / g.rowh;
    }

  if (row1 > (c.y + c.h - 1 - g.ry + l->top) / g.rowh)
    {
      row1 = (c.y + c.h - 1 - g.ry + l->top) / g.rowh;
    }

  for (r = row0; r <= row1; r++)
    {
      int y = g.ry + r * g.rowh - l->top;
      int ty = y + (g.rowh - text_height(f)) / 2;
      int col;

      cut_begin(k, &c);
      need(k, 48);
      ground_paint(k, i, g.rx, y, g.rw, g.rowh);
      if (r < l->n)
        {
          bool sel = r == l->sel;
          uint16_t fg = g_theme.text;
          uint16_t rowbg = bg;
          int x0 = g.rx + 12;

          if (sel)
            {
              rowbg = accent ? g_theme.accent : g_theme.selection;
              fg = accent ? g_theme.accent_text : g_theme.text;
              ops_rrect(o, g.rx + 4, y + 1, g.rw - 8, g.rowh - 2, LUT_R6, 6,
                        rowbg, 255);
            }
          else if (r == l->hover && k->down == KIT_NONE)
            {
              ops_rrect(o, g.rx + 4, y + 1, g.rw - 8, g.rowh - 2, LUT_R6, 6,
                        g_theme.hilite, wg->style == KIT_LIST_SIDEBAR ?
                        THEMED(20, 14) : THEMED(16, 10));
            }

          if (l->icon != NULL)
            {
              l->icon(k, wg, o, r, x0, y + (g.rowh - KIT_ICON) / 2, fg,
                      rowbg);
              x0 += KIT_ICON + 12;
            }

          for (col = 0; col < g.ncols && l->text != NULL; col++)
            {
              const struct kit_col_s *cd = l->ncols > 0 ? &l->cols[col] :
                                           NULL;
              int left = col == 0 ? x0 : g.cx[col] + 12;
              int right = g.cx[col] + g.cw[col] - 12;
              const char *s;
              uint16_t tc = fg;
              int tw;

              if (g.cw[col] == 0 || right <= left)
                {
                  continue;
                }

              s = l->text(k, wg, r, col, buf, sizeof(buf));
              if (s == NULL || s[0] == '\0')
                {
                  continue;
                }

              if (cd != NULL && (cd->flags & KIT_COL_DIM))
                {
                  tc = sel && accent ? colour_mix(g_theme.accent_text,
                                                  g_theme.accent, 70) :
                                       g_theme.text_dim;
                }

              {
                const struct desk_font_s *cf =
                  sel && wg->style == KIT_LIST_SIDEBAR ? KIT_FONT_BOLD : f;

                tw = ui_fit(cf, s, right - left, fit, sizeof(fit));
                need_text(k, fit);
                ops_text(o, cf, cd != NULL && (cd->flags & KIT_COL_RIGHT) ?
                         right - tw : left, ty, fit, tc, 255);
              }
            }
        }

      cut_end();
    }

  if (l->n == 0 && l->empty != NULL)
    {
      cut_begin(k, &c);
      need_text(k, l->empty);
      ops_text_centre(o, f, g.rx + g.rw / 2, g.ry + g.rh / 2 - 20, l->empty,
                      g_theme.text_dim, 255);
      cut_end();
    }

  r_add(&k->drawn, &c);
}

static void list_head_draw(struct kit_s *k, int i,
                           const struct kit_rect_s *clip)
{
  struct kit_widget_s *wg = &k->wg[i];
  struct kit_list_s *l = wg->data;
  struct desk_ops_s *o = g_kops;
  const struct desk_font_s *f = KIT_FONT_SMALL;
  struct kit_rect_s head;
  struct kit_rect_s c;
  struct lgeom_s g;
  int col;

  list_geom(k, wg, &g);
  head = r_make(wg->x, wg->y, wg->w, g.hh);
  c = r_and(clip, &head);
  if (g.hh == 0 || c.w <= 0 || c.h <= 0)
    {
      return;
    }

  cut_begin(k, &c);
  need(k, 8);
  ground_paint(k, i, wg->x, wg->y, wg->w, g.hh);
  ops_fill(o, wg->x + 8, wg->y + g.hh - 1, wg->w - 16, 1, g_theme.bar);
  for (col = 0; col < g.ncols; col++)
    {
      const char *name = l->cols[col].name;
      bool right = (l->cols[col].flags & KIT_COL_RIGHT) != 0;
      bool on = l->sortcol == col;
      bool hot = k->hot == i && k->hotpart == PART_HEAD - col;
      uint16_t tc = on || hot ? g_theme.text : g_theme.text_dim;
      int x;
      int tw;

      if (g.cw[col] == 0 || name == NULL)
        {
          continue;
        }

      need_text(k, name);
      tw = text_width(f, name);
      x  = right ? g.cx[col] + g.cw[col] - 12 - tw :
           col == 0 && l->icon != NULL ? g.cx[col] + 12 + KIT_ICON + 12 :
           g.cx[col] + 12;
      ops_text(o, f, x, wg->y + (g.hh - text_height(f)) / 2, name, tc, 255);
      if (on)
        {
          ui_triangle(o, right ? x - 10 : x + tw + 10, wg->y + g.hh / 2, 4,
                      l->sortdesc ? 1 : 0, tc, 255);
        }
    }

  cut_end();
  r_add(&k->drawn, &c);
}

static void list_sb_draw(struct kit_s *k, int i,
                         const struct kit_rect_s *clip)
{
  struct kit_widget_s *wg = &k->wg[i];
  struct kit_list_s *l = wg->data;
  struct desk_ops_s *o = g_kops;
  struct kit_rect_s sb;
  struct kit_rect_s c;
  struct lgeom_s g;
  int ty;
  int th;

  list_geom(k, wg, &g);
  sb = r_make(g.rx + g.rw, g.ry, g.sbw, g.rh);
  c = r_and(clip, &sb);
  if (g.sbw == 0 || c.w <= 0 || c.h <= 0)
    {
      return;
    }

  cut_begin(k, &c);
  need(k, 8);
  ground_paint(k, i, sb.x, sb.y, sb.w, sb.h);
  if (list_thumb(wg, &g, &ty, &th))
    {
      kit_paint_thumb(o, sb.x, ty, th, k->hot == i && k->hotpart <= PART_SB &&
                      k->hotpart > PART_HEAD, l->thumbdrag);
    }

  cut_end();
  r_add(&k->drawn, &c);
}

/* Scroll a list to top: what stays is copied, the rest drawn */

static void list_scroll_to(struct kit_s *k, int i, int top)
{
  struct kit_widget_s *wg = &k->wg[i];
  struct kit_list_s *l = wg->data;
  struct kit_rect_s rows;
  struct nx_gpuop_s *op;
  struct lgeom_s g;
  int d;

  list_geom(k, wg, &g);
  top = top > g.maxtop ? g.maxtop : top < 0 ? 0 : top;
  d = top - l->top;
  if (d == 0)
    {
      return;
    }

  l->top = top;
  rows = r_make(g.rx, g.ry, g.rw, g.rh);
  if (!shown(k, i) || (wg->flags & KIT_F_DIRTY) || l->alldirty ||
      d >= g.rh || -d >= g.rh)
    {
      l->alldirty = true;
      l->sbdirty  = true;
      return;
    }

  need(k, 1);
  op = ops_add(g_kops);
  op->op = NX_GPUOP_COPY;
  op->x  = g.rx;
  op->w  = g.rw;
  op->h  = g.rh - (d > 0 ? d : -d);
  op->sx = g.rx;
  if (d > 0)
    {
      struct kit_rect_s c = r_make(g.rx, g.ry + g.rh - d, g.rw, d);

      op->y  = g.ry;
      op->sy = g.ry + d;
      list_rows_draw(k, i, &c, 0, l->n);
    }
  else
    {
      struct kit_rect_s c = r_make(g.rx, g.ry, g.rw, -d);

      op->y  = g.ry - d;
      op->sy = g.ry;
      list_rows_draw(k, i, &c, 0, l->n);
    }

  r_add(&k->drawn, &rows);
  l->sbdirty = true;
}

static void list_set_hover(struct kit_s *k, int i, int row)
{
  struct kit_widget_s *wg = &k->wg[i];
  struct kit_list_s *l = wg->data;

  if (row != l->hover)
    {
      list_row_dirty(wg, l->hover);
      list_row_dirty(wg, row);
      l->hover = row;
    }
}

/****************************************************************************
 * Private Functions: tabs and crumbs
 ****************************************************************************/

static int tab_width(struct kit_widget_s *wg)
{
  int n = wg->nitems > 0 ? wg->nitems : 1;
  int tw;

  if (wg->style == KIT_TABS_SEGMENTS)
    {
      return wg->w / n;
    }

  tw = (wg->w - 12) / n;
  return tw > KIT_TAB_MAXW ? KIT_TAB_MAXW : tw;
}

static int tab_x(struct kit_widget_s *wg, int t)
{
  int n = wg->nitems;

  if (wg->style == KIT_TABS_SEGMENTS)
    {
      return wg->x + wg->w * t / (n > 0 ? n : 1);
    }

  return wg->x + (wg->w - tab_width(wg) * n) / 2 + t * tab_width(wg);
}

/* The part of tabs at x: tab * 2, + 1 on its close button, or -1 */

static int tab_part(struct kit_widget_s *wg, int x, int y)
{
  int t;

  for (t = 0; t < wg->nitems; t++)
    {
      int tx = tab_x(wg, t);
      int tw = wg->style == KIT_TABS_SEGMENTS ?
               tab_x(wg, t + 1) - tx : tab_width(wg);

      if (x >= tx && x < tx + tw)
        {
          int cx = tx + tw - 8 - 18;

          return t * 2 + ((wg->flags & KIT_F_CLOSABLE) && x >= cx &&
                          x < cx + 18);
        }
    }

  return PART_NONE;
}

static void tabs_draw(struct kit_s *k, int i)
{
  struct kit_widget_s *wg = &k->wg[i];
  const char * const *items = wg->data;
  struct desk_ops_s *o = g_kops;
  const struct desk_font_s *f = KIT_FONT;
  bool active = k->w->active;
  int hot = k->hot == i ? k->hotpart : PART_NONE;
  int y = wg->y;
  int h = wg->h;
  int t;

  if (wg->style == KIT_TABS_SEGMENTS)
    {
      for (t = 0; t < wg->nitems; t++)
        {
          need_text(k, items[t]);
        }

      kit_paint_segments(o, wg->x, y, wg->w, h, items, wg->nitems,
                         wg->value, hot >= 0 ? hot / 2 : -1, k->down == i,
                         !enabled(wg));
      return;
    }

  /* A strip below the title bar, in its colours */

  ops_fill(o, wg->x, y, wg->w, h,
           active ? g_theme.title_active : g_theme.title_inactive);
  ops_blend(o, wg->x, y + h - 1, wg->w, 1, g_theme.shade,
            THEMED(120, 36));
  for (t = 0; t < wg->nitems; t++)
    {
      int tw = tab_width(wg);
      int x = tab_x(wg, t);
      int cx = x + tw - 8 - 18;
      int cy = y + (h - 1 - 18) / 2;
      bool th = hot >= 0 && hot / 2 == t;
      bool cur = t == wg->value;
      bool closable = (wg->flags & KIT_F_CLOSABLE) != 0;
      char fit[64];

      need(k, 80);
      if (cur)
        {
          ops_rrect(o, x + 3, y + 3, tw - 6, h - 7, LUT_R6, 6,
                    g_theme.title_hilite, 255);
        }
      else if (th)
        {
          ops_rrect(o, x + 3, y + 3, tw - 6, h - 7, LUT_R6, 6,
                    g_theme.hilite, THEMED(14, 10));
        }
      else if (t + 1 != wg->value && t + 1 < wg->nitems &&
               !(hot >= 0 && hot / 2 == t + 1))
        {
          ops_blend(o, x + tw - 1, y + 8, 1, h - 17, g_theme.hilite,
                    THEMED(30, 40));
        }

      ui_fit(f, items[t], tw - 2 * (closable ? 18 + 12 : 12), fit,
             sizeof(fit));
      ops_text_centre(o, f, x + tw / 2, y + (h - 1 - text_height(f)) / 2,
                      fit, cur && active ? g_theme.title_text : g_theme.title_dim,
                      255);

      if (closable && (cur || th))
        {
          if (th && (hot & 1))
            {
              bool held = k->down == i && k->downpart == hot;

              ops_rrect(o, cx, cy, 18, 18, LUT_R6, 6, g_theme.hilite,
                        THEMED(held ? 60 : 30, held ? 36 : 18));
            }

          kit_paint_icon(o, KIT_ICON_CLOSE, cx + 1, cy + 1,
                         cur ? g_theme.title_text : g_theme.title_dim, 0);
        }
    }
}

/* Crumbs: the first shown (the ones before are an ellipsis), and each
 * one's x and width
 */

static int crumbs_geom(struct kit_widget_s *wg, int16_t *xs, int16_t *ws)
{
  const char * const *items = wg->data;
  int n = MIN(wg->nitems, MAX_CRUMBS);
  int ell = text_width(KIT_FONT, DESK_CH_ELLIPSIS) + 16;
  int total = -CRUMB_SEP;
  int first = 0;
  int x;
  int c;

  for (c = 0; c < n; c++)
    {
      const struct desk_font_s *f = c == n - 1 ? KIT_FONT_BOLD : KIT_FONT;

      ws[c] = MIN(text_width(f, items[c]), 180) + 16;
      total += ws[c] + CRUMB_SEP;
    }

  while (total > wg->w - 8 && first < n - 1)
    {
      total -= ws[first] + CRUMB_SEP;
      if (first == 0)
        {
          total += ell + CRUMB_SEP;
        }

      first++;
    }

  x = wg->x + 4;
  if (first > 0)
    {
      xs[first - 1] = x;                 /* The ellipsis: the one before */
      ws[first - 1] = ell;
      x += ell + CRUMB_SEP;
    }

  for (c = first; c < n; c++)
    {
      xs[c] = x;
      x += ws[c] + CRUMB_SEP;
    }

  return first;
}

static int crumb_part(struct kit_widget_s *wg, int x)
{
  int16_t xs[MAX_CRUMBS];
  int16_t ws[MAX_CRUMBS];
  int first = crumbs_geom(wg, xs, ws);
  int c;

  for (c = first > 0 ? first - 1 : 0; c < MIN(wg->nitems, MAX_CRUMBS) - 1;
       c++)
    {
      if (x >= xs[c] && x < xs[c] + ws[c])
        {
          return c;
        }
    }

  return PART_NONE;
}

static void crumbs_draw(struct kit_s *k, int i)
{
  struct kit_widget_s *wg = &k->wg[i];
  const char * const *items = wg->data;
  struct desk_ops_s *o = g_kops;
  const struct desk_font_s *f = KIT_FONT;
  int ty = wg->y + (wg->h - text_height(f)) / 2;
  int n = MIN(wg->nitems, MAX_CRUMBS);
  int16_t xs[MAX_CRUMBS];
  int16_t ws[MAX_CRUMBS];
  int first;
  int c;

  ops_rrect(o, wg->x, wg->y, wg->w, wg->h, LUT_R6, 6, g_theme.field, 255);
  if (n == 0)
    {
      return;
    }

  first = crumbs_geom(wg, xs, ws);
  for (c = first > 0 ? first - 1 : 0; c < n; c++)
    {
      bool last = c == n - 1;
      bool hot = k->hot == i && k->hotpart == c;
      char fit[48];

      if (hot)
        {
          ops_rrect(o, xs[c], wg->y + 3, ws[c], wg->h - 6, LUT_R6, 6,
                    k->down == i ? g_theme.control_pressed : g_theme.control_hover,
                    255);
        }

      need(k, 48);
      if (c == first - 1)
        {
          ops_text(o, f, xs[c] + 8, ty, DESK_CH_ELLIPSIS, g_theme.text_dim, 255);
        }
      else
        {
          ui_fit(last ? KIT_FONT_BOLD : f, items[c], 180, fit, sizeof(fit));
          ops_text(o, last ? KIT_FONT_BOLD : f, xs[c] + 8, ty, fit,
                   last ? g_theme.text : g_theme.text_dim, 255);
        }

      if (!last)
        {
          ops_text(o, f, xs[c] + ws[c] + 4, ty, "/", g_theme.text_dim, 160);
        }
    }
}

/****************************************************************************
 * Private Functions: scroll bars
 ****************************************************************************/

static bool sb_thumb(struct kit_widget_s *wg, int *ty, int *th)
{
  int page = wg->min;
  int total = wg->max;

  if (total <= page || page <= 0 || wg->h <= 0)
    {
      return false;
    }

  *th = wg->h * page / total;
  *th = *th < KIT_THUMB_MIN ? KIT_THUMB_MIN : *th > wg->h ? wg->h : *th;
  *ty = wg->y + (wg->h - *th) * wg->value / (total - page);
  return true;
}

static void sb_set(struct kit_s *k, int i, int pos)
{
  struct kit_widget_s *wg = &k->wg[i];
  int range = wg->max - wg->min;

  range = range < 0 ? 0 : range;
  pos = pos < 0 ? 0 : pos > range ? range : pos;
  if (pos != wg->value)
    {
      wg->value = pos;
      dirty(k, i);
      emit(k, i, KIT_EV_CHANGE);
    }
}

/****************************************************************************
 * Private Functions: drawing
 ****************************************************************************/

static int look_of(struct kit_s *k, int i)
{
  struct kit_widget_s *wg = &k->wg[i];

  if (!enabled(wg))
    {
      return KIT_LOOK_DISABLED;
    }

  if ((wg->flags & KIT_F_DOWN) || (k->down == i && k->hot == i))
    {
      return KIT_LOOK_DOWN;
    }

  if ((wg->flags & KIT_F_HOT) || (k->hot == i && k->down == KIT_NONE) ||
      k->down == i)
    {
      return KIT_LOOK_HOT;
    }

  return KIT_LOOK_NORMAL;
}

/* A box's own look, whole (the caller cuts it) */

static void box_ops(struct kit_s *k, int i)
{
  struct kit_widget_s *wg = &k->wg[i];
  struct desk_ops_s *o = g_kops;
  bool has;
  uint16_t bg = box_colour(k, wg, &has);

  switch (wg->style)
    {
      case KIT_BOX_BAR:
        ops_fill(o, wg->x, wg->y, wg->w, wg->h, bg);
        ops_fill(o, wg->x, wg->y + wg->h - 1, wg->w, 1, g_theme.separator);
        break;

      case KIT_BOX_STATUS:
        ops_fill(o, wg->x, wg->y, wg->w, wg->h, bg);
        ops_fill(o, wg->x, wg->y, wg->w, 1, g_theme.separator);
        break;

      case KIT_BOX_SIDEBAR:
        ops_fill(o, wg->x, wg->y, wg->w, wg->h, bg);
        ops_fill(o, wg->x + wg->w - 1, wg->y, 1, wg->h, g_theme.separator);
        break;

      case KIT_BOX_GROUP:
      case KIT_BOX_CARD:
        {
          int ty = wg->y;
          int th = wg->h;

          if (wg->style == KIT_BOX_GROUP)
            {
              if (wg->text != NULL)
                {
                  ops_text(o, KIT_FONT_BOLD, wg->x + 4, wg->y, wg->text,
                           g_theme.text_dim, 255);
                }

              ty += group_title_h() - 2;
              th -= group_title_h() - 2;
            }

          kit_paint_card(o, wg->x, ty, wg->w, th);
          if (wg->flags & KIT_F_LINES)
            {
              int j;
              bool first = true;

              for (j = i + 1; j < wg->end; j = k->wg[j].end)
                {
                  if (k->wg[j].flags & KIT_F_HIDDEN)
                    {
                      continue;
                    }

                  if (!first)
                    {
                      kit_paint_card_lines(o, wg->x,
                                           k->wg[j].y - (wg->gap + 1) / 2,
                                           wg->w);
                    }

                  first = false;
                }
            }
        }
        break;

      case KIT_BOX_DIALOG:

        /* Over the window: its corners show what is under it */

        ops_rrect(o, wg->x, wg->y, wg->w, wg->h, LUT_R12, 12,
                  g_theme.separator, 255);
        ops_rrect(o, wg->x + 1, wg->y + 1, wg->w - 2, wg->h - 2, LUT_R12, 11,
                  bg, 255);
        ops_blend(o, wg->x + 14, wg->y + 1, wg->w - 28, 1, C_WHITE,
                  g_theme.dark ? 30 : 0);
        break;

      default:
        ops_fill(o, wg->x, wg->y, wg->w, wg->h, bg);
        break;
    }
}

/* What is under widget i: its boxes' looks, whole (a card's round corners
 * and hairlines are the card's, whatever its rows are)
 */

static void ground_ops(struct kit_s *k, int i)
{
  int p;

  for (p = parent_of(k, i); p >= 0; p = parent_of(k, p))
    {
      struct kit_widget_s *b = &k->wg[p];

      switch (b->style)
        {
          case KIT_BOX_NONE:
            continue;

          case KIT_BOX_CARD:
          case KIT_BOX_GROUP:
            ground_ops(k, p);
            box_ops(k, p);
            return;

          case KIT_BOX_DIALOG:
            ops_fill(g_kops, b->x, b->y, b->w, b->h, g_theme.card);
            return;

          default:
            box_ops(k, p);
            return;
        }
    }

  ops_fill(g_kops, k->area.x, k->area.y, k->area.w, k->area.h,
           k->parent != NULL ? g_theme.card : g_theme.window_bg);
}

/* ... cut to (x, y, w, h) */

static void ground_paint(struct kit_s *k, int i, int x, int y, int w,
                         int h)
{
  int start;

  need(k, 128);
  start = g_kops->n;
  ground_ops(k, i);
  ui_clip(g_kops, start, x, y, x + w, y + h);
}

static void box_draw(struct kit_s *k, int i, const struct kit_rect_s *clip)
{
  struct kit_widget_s *wg = &k->wg[i];

  cut_begin(k, clip);
  if (wg->style == KIT_BOX_NONE || wg->style == KIT_BOX_CARD ||
      wg->style == KIT_BOX_GROUP)
    {
      ground_paint(k, i, wg->x, wg->y, wg->w, wg->h);
    }

  if (wg->style != KIT_BOX_NONE)
    {
      need(k, 32 + (wg->text != NULL ? strlen(wg->text) : 0));
      box_ops(k, i);
    }

  if (wg->flags & KIT_F_SCROLL)
    {
      int ty;
      int th;

      if (box_thumb(wg, &ty, &th))
        {
          need(k, 2);
          kit_paint_thumb(g_kops, wg->x + wg->w - KIT_SB_W, ty, th, false,
                          k->down == i);
        }
    }

  cut_end();
}

/* The application draws: after what the kit has (sent first, in case it
 * sends its own), into kit_ops() or its own list, cut to c
 */

static void custom_draw(struct kit_s *k, struct kit_widget_s *wg,
                        const struct kit_rect_s *c)
{
  kit_send(k);
  if (k->draw != NULL)
    {
      cut_begin(k, c);
      k->draw(k, wg, c->x, c->y, c->x + c->w, c->y + c->h);
      cut_end();
    }
}

static void widget_draw(struct kit_s *k, int i,
                        const struct kit_rect_s *clip)
{
  struct kit_widget_s *wg = &k->wg[i];
  struct desk_ops_s *o = g_kops;
  int look = look_of(k, i);
  uint16_t gr = ground(k, i);
  struct kit_rect_s e = extent(k, i);
  struct kit_rect_s c = r_and(clip, &e);
  bool ring = ringed(k, i);
  int x = wg->x;
  int y = wg->y;
  int w = wg->w;
  int h = wg->h;

  if (c.w <= 0 || c.h <= 0 || w <= 0 || h <= 0)
    {
      return;
    }

  if (wg->type == KIT_LIST)
    {
      struct kit_list_s *l = wg->data;
      struct lgeom_s g;

      list_geom(k, wg, &g);
      list_head_draw(k, i, &c);
      list_rows_draw(k, i, &c, 0, l->n + g.rh / g.rowh + 1);
      list_sb_draw(k, i, &c);
      return;
    }

  if (wg->type == KIT_CUSTOM)
    {
      custom_draw(k, wg, &c);
      r_add(&k->drawn, &c);
      return;
    }

  cut_begin(k, &c);
  need(k, ROOM);

  /* The ground, and the ring's band around it */

  if (has_ring(wg))
    {
      ground_paint(k, i, x - KIT_RING, y - KIT_RING, w + 2 * KIT_RING,
                   h + 2 * KIT_RING);
    }
  else
    {
      ground_paint(k, i, x, y, w, h);
    }

  switch (wg->type)
    {
      case KIT_LABEL:
        {
          const struct desk_font_s *f = label_font(wg->style);
          uint16_t tc = !enabled(wg) ? g_theme.text_faint :
                        (wg->style & KIT_TEXT_ERROR) ? g_theme.error :
                        (wg->style & KIT_TEXT_DIM) ? g_theme.text_dim : g_theme.text;
          char fit[160];
          int tw;

          if (wg->text == NULL)
            {
              break;
            }

          {
            const char *cap = label_caption(wg->text);
            int th = text_height(f) +
                     (cap != NULL ? 1 + text_height(KIT_FONT_SMALL) : 0);
            int ty = y + (h - th) / 2;
            char line[160];

            tw = ui_fit(f, label_line(wg->text, line, sizeof(line)), w, fit,
                        sizeof(fit));
            need_text(k, fit);
            ops_text(o, f, (wg->style & KIT_TEXT_RIGHT) ? x + w - tw :
                     (wg->style & KIT_TEXT_CENTRE) ? x + (w - tw) / 2 : x,
                     ty, fit, tc, 255);
            if (cap != NULL)
              {
                tw = ui_fit(KIT_FONT_SMALL, cap, w, fit, sizeof(fit));
                need_text(k, fit);
                ops_text(o, KIT_FONT_SMALL,
                         (wg->style & KIT_TEXT_RIGHT) ? x + w - tw :
                         (wg->style & KIT_TEXT_CENTRE) ? x + (w - tw) / 2 :
                         x, ty + text_height(f) + 1, fit,
                         enabled(wg) ? g_theme.text_dim : g_theme.text_faint,
                         255);
              }
          }
        }
        break;

      case KIT_BUTTON:
        if (ring)
          {
            kit_paint_ring(o, x, y, w, h, KIT_R_CONTROL, gr);
          }

        if (wg->style != KIT_BTN_CUSTOM)
          {
            kit_paint_button(o, x, y, w, h, wg->text, wg->icon, wg->style,
                             look, wg->value != 0);
          }

        if (wg->icon == KIT_ICON_CUSTOM || wg->style == KIT_BTN_CUSTOM)
          {
            custom_draw(k, wg, &c);
            cut_begin(k, &c);
          }
        break;

      case KIT_CHECK_BOX:
      case KIT_RADIO_BTN:
        {
          int by = y + (h - KIT_CHECK) / 2;
          int bs = wg->type == KIT_CHECK_BOX ? KIT_CHECK : KIT_RADIO;

          if (ring && wg->type == KIT_CHECK_BOX)
            {
              kit_paint_ring(o, x, by, bs, bs, KIT_R_CONTROL, gr);
            }
          else if (ring)
            {
              kit_paint_ring_round(o, x + 1, by + 1, bs, gr);
            }

          if (wg->type == KIT_CHECK_BOX)
            {
              kit_paint_check(o, x, by, wg->value != 0, look);
            }
          else
            {
              kit_paint_radio(o, x + 1, by + 1, wg->value != 0, look);
            }

          if (wg->text != NULL)
            {
              ops_text(o, KIT_FONT, x + KIT_CHECK + 8,
                       y + (h - text_height(KIT_FONT)) / 2, wg->text,
                       enabled(wg) ? g_theme.text : g_theme.text_faint, 255);
            }
        }
        break;

      case KIT_SWITCH:
        {
          int sx = x + w - KIT_SWITCH_W;
          int sy = y + (h - KIT_SWITCH_H) / 2;

          if (wg->text != NULL)
            {
              ops_text(o, KIT_FONT, x, y + (h - text_height(KIT_FONT)) / 2,
                       wg->text, enabled(wg) ? g_theme.text : g_theme.text_faint,
                       255);
            }

          if (ring)
            {
              kit_paint_ring(o, sx, sy, KIT_SWITCH_W, KIT_SWITCH_H,
                             KIT_R_CARD, gr);
            }

          kit_paint_switch(o, sx, sy, wg->value != 0, look);
        }
        break;

      case KIT_SLIDER:
        if (ring)
          {
            kit_paint_ring(o, x, y, w, h, KIT_R_CARD, gr);
          }

        kit_paint_slider(o, x, y + (h - KIT_SLIDER_H) / 2, w, wg->value,
                         wg->min, wg->max, look);
        break;

      case KIT_PROGRESS:
        kit_paint_progress(o, x, y + (h - KIT_PROGRESS_H) / 2, w, wg->value,
                           wg->min, wg->max);
        break;

      case KIT_FIELD:
        if (wg->data != NULL)
          {
            kit_paint_field(o, wg->data, x, y, w, h, wg->text,
                            k->focus == i && enabled(wg) &&
                            (k->w->active || (wg->flags & KIT_F_RING)),
                            k->caret && !(wg->flags & KIT_F_SELECTED),
                            !enabled(wg), (wg->flags & KIT_F_SELECTED) != 0);
          }
        break;

      case KIT_DROPDOWN:
        {
          const char * const *items = wg->data;

          if (ring)
            {
              kit_paint_ring(o, x, y, w, h, KIT_R_CONTROL, gr);
            }

          kit_paint_dropdown(o, x, y, w, h, items != NULL &&
                             wg->value >= 0 && wg->value < wg->nitems ?
                             items[wg->value] : NULL,
                             k->pop.open && k->pop.owner == i ?
                             KIT_LOOK_DOWN : look);
        }
        break;

      case KIT_TABS:
        if (ring)
          {
            kit_paint_ring(o, x, y, w, h, KIT_R_CONTROL, gr);
          }

        tabs_draw(k, i);
        break;

      case KIT_SCROLL:
        {
          int ty;
          int th;

          if (sb_thumb(wg, &ty, &th))
            {
              kit_paint_thumb(o, x + (w - KIT_SB_W) / 2, ty, th,
                              k->hot == i, k->down == i &&
                              k->downpart == PART_THUMB);
            }
        }
        break;

      case KIT_CRUMBS:
        crumbs_draw(k, i);
        break;

      case KIT_STEPPER:
        if (ring)
          {
            kit_paint_ring(o, x, y + (h - KIT_STEP) / 2, w, KIT_STEP,
                           KIT_R_CONTROL, gr);
          }

        kit_paint_stepper(o, x, y + (h - KIT_STEP) / 2, w, wg->text,
                          k->hot == i && enabled(wg) ?
                          (k->hotpart == 0 ? -1 : k->hotpart == 1 ? 1 : 0) :
                          0);
        break;

      case KIT_SWATCHES:
        {
          const uint16_t *cols = wg->data;
          int t;

          if (ring)
            {
              kit_paint_ring(o, x, y, w, h, KIT_R_CARD, gr);
            }

          for (t = 0; cols != NULL && t < wg->nitems; t++)
            {
              kit_paint_swatch(o, x + KIT_SWATCH_DX / 2 + t * KIT_SWATCH_DX,
                               y + h / 2 - 1, cols[t], t == wg->value,
                               k->hot == i && k->hotpart == t);
            }
        }
        break;

      case KIT_IMAGE:
        {
          struct kit_surface_s *sf = wg->data;

          /* In the middle of its rectangle (cut to it), unless lost */

          if (sf != NULL && sf->hwnd != NULL && !sf->lost)
            {
              kit_surface_copy(o, sf, 0, 0, sf->w, sf->h,
                               x + (w - sf->w) / 2, y + (h - sf->h) / 2);
            }
        }
        break;

      case KIT_SEPARATOR:
        if (w >= h)
          {
            ops_fill(o, x, y + h / 2, w, 1, g_theme.separator);
          }
        else
          {
            ops_fill(o, x + w / 2, y, 1, h, g_theme.separator);
          }
        break;

      default:
        break;
    }

  cut_end();
  r_add(&k->drawn, &c);
}

/* Everything that crosses r, cut to it, in tree order */

static void area_draw(struct kit_s *k, const struct kit_rect_s *r)
{
  int i = 0;

  while (i < k->n)
    {
      struct kit_widget_s *wg = &k->wg[i];
      struct kit_rect_s e = extent(k, i);

      if ((wg->flags & KIT_F_HIDDEN) || !r_cross(&e, r))
        {
          i = wg->type == KIT_BOX ? wg->end : i + 1;
          continue;
        }

      if (wg->type == KIT_BOX)
        {
          struct kit_rect_s c = r_and(r, &e);

          box_draw(k, i, &c);
        }
      else
        {
          widget_draw(k, i, r);
        }

      /* Drawn: unless only part of it was (a widget that changed is
       * drawn whole later)
       */

      if (r_within(&e, r))
        {
          wg->flags &= ~KIT_F_DIRTY;
          if (wg->type == KIT_LIST)
            {
              struct kit_list_s *l = wg->data;

              l->alldirty = l->sbdirty = l->headdirty = false;
              l->ndirty = 0;
            }
        }

      i++;
    }

  r_add(&k->drawn, r);
}

/* A menu or a drop-down's list */

static int pop_item_h(struct kit_popup_s *p, int t)
{
  return strcmp(p->items[t], "-") == 0 ? 9 : KIT_MENU_ROW_H;
}

static int pop_item_at(struct kit_popup_s *p, int x, int y)
{
  int iy = p->r.y + 4;
  int t;

  if (x < p->r.x || x >= p->r.x + p->r.w)
    {
      return -1;
    }

  for (t = 0; t < p->n; t++)
    {
      int ih = pop_item_h(p, t);

      if (y >= iy && y < iy + ih)
        {
          return ih == KIT_MENU_ROW_H && !(p->disabled & (1u << t)) ? t : -1;
        }

      iy += ih;
    }

  return -1;
}

static void pop_draw(struct kit_s *k)
{
  struct kit_popup_s *p = &k->pop;
  struct desk_ops_s *o = g_kops;
  const struct desk_font_s *f = KIT_FONT;
  bool marks = p->owner != KIT_NONE;
  int iy = p->r.y + 4;
  int t;

  need(k, 8);
  ops_rrect(o, p->r.x, p->r.y, p->r.w, p->r.h, LUT_R6, 6, g_theme.separator, 255);
  ops_rrect(o, p->r.x + 1, p->r.y + 1, p->r.w - 2, p->r.h - 2, LUT_R6, 5,
            g_theme.card, 255);
  for (t = 0; t < p->n; t++)
    {
      int ih = pop_item_h(p, t);
      bool dis = (p->disabled & (1u << t)) != 0;
      uint16_t fg = dis ? g_theme.text_faint : g_theme.text;

      need_text(k, p->items[t]);
      need(k, 24);
      if (ih != KIT_MENU_ROW_H)
        {
          ops_fill(o, p->r.x + 8, iy + 4, p->r.w - 16, 1, g_theme.separator);
          iy += ih;
          continue;
        }

      if (t == p->hover)
        {
          ops_rrect(o, p->r.x + 4, iy, p->r.w - 8, ih, LUT_R6, 6, g_theme.accent,
                    255);
          fg = g_theme.accent_text;
        }

      if (marks && t == p->sel)
        {
          kit_paint_icon(o, KIT_ICON_CHECK, p->r.x + 8,
                         iy + (ih - KIT_ICON) / 2, fg, 0);
        }

      ops_text(o, f, p->r.x + (marks ? 30 : 14),
               iy + (ih - text_height(f)) / 2, p->items[t], fg, 255);
      iy += ih;
    }
}

static void tip_draw(struct kit_s *k)
{
  struct kit_widget_s *wg = get(k, k->hot);

  if (wg != NULL && wg->tip != NULL)
    {
      need_text(k, wg->tip);
      need(k, 8);
      kit_paint_tooltip(g_kops, k->tiprect.x, k->tiprect.y, k->tiprect.w,
                        k->tiprect.h, wg->tip);
    }
}

/* Menus, tooltips and a dialog over what was drawn: drawn again */

static void overlays_draw(struct kit_s *k, const struct kit_rect_s *drawn)
{
  if (k->dlg != NULL && r_cross(drawn, &k->dlg->area))
    {
      struct kit_rect_s c = r_and(drawn, &k->dlg->area);

      kit_send(k);
      kit_damage(k->dlg, c.x, c.y, c.w, c.h);
      kit_flush(k->dlg);
    }

  if (k->pop.open && r_cross(drawn, &k->pop.r))
    {
      pop_draw(k);
    }

  if (k->tipshown && r_cross(drawn, &k->tiprect))
    {
      tip_draw(k);
    }
}

static void tip_hide(struct kit_s *k)
{
  if (k->tipshown)
    {
      k->tipshown = false;
      kit_damage(k, k->tiprect.x, k->tiprect.y, k->tiprect.w,
                 k->tiprect.h);
    }
}

/****************************************************************************
 * Private Functions: acting
 ****************************************************************************/

static void mark_part(struct kit_s *k, int i, int part);

static void set_focus(struct kit_s *k, int i)
{
  int old = k->focus;

  if (old == i)
    {
      return;
    }

  k->focus = i;
  k->caret = true;
  if (old != KIT_NONE && old < k->n)
    {
      struct kit_rect_s e = extent(k, old);

      k->wg[old].flags &= ~KIT_F_SELECTED;
      dirty(k, old);
      if (has_ring(&k->wg[old]))
        {
          kit_damage(k, e.x, e.y, e.w, e.h);
        }

      if (k->wg[old].type == KIT_LIST)
        {
          ((struct kit_list_s *)k->wg[old].data)->alldirty = true;
        }
    }

  if (i != KIT_NONE)
    {
      dirty(k, i);
      box_show(k, i);
    }
}

/* A radio button goes on: the others of its box go off */

static void radio_set(struct kit_s *k, int i)
{
  int p = parent_of(k, i);
  int j;

  for (j = p + 1; p >= 0 && j < k->wg[p].end; j = k->wg[j].end)
    {
      if (k->wg[j].type == KIT_RADIO_BTN && j != i && k->wg[j].value)
        {
          k->wg[j].value = 0;
          dirty(k, j);
        }
    }

  k->wg[i].value = 1;
  dirty(k, i);
}

/* A widget clicked (the pointer, Space, or a shortcut) */

static void click(struct kit_s *k, int i)
{
  struct kit_widget_s *wg = &k->wg[i];

  if (!enabled(wg) || !shown(k, i))
    {
      return;
    }

  dirty(k, i);
  switch (wg->type)
    {
      case KIT_BUTTON:
        if (wg->style == KIT_BTN_TOGGLE)
          {
            wg->value = !wg->value;
          }

        emit(k, i, KIT_EV_CLICK);
        break;

      case KIT_CHECK_BOX:
      case KIT_SWITCH:
        wg->value = !wg->value;
        emit(k, i, KIT_EV_CHANGE);
        break;

      case KIT_RADIO_BTN:
        if (!wg->value)
          {
            radio_set(k, i);
            emit(k, i, KIT_EV_CHANGE);
          }
        break;

      default:
        break;
    }
}

static void pop_close(struct kit_s *k)
{
  if (k->pop.open)
    {
      int owner = k->pop.owner;

      k->pop.open = false;
      kit_damage(k, k->pop.r.x, k->pop.r.y, k->pop.r.w, k->pop.r.h);
      dirty(k, owner);

      /* What the pointer is on is found again when it next moves */

      mark_part(k, k->hot, k->hotpart);
      k->hot     = KIT_NONE;
      k->hotpart = PART_NONE;
    }
}

/* Open a drop-down's list, under it (or over it if there is no room) */

static void pop_open(struct kit_s *k, int owner, int id, int x, int y,
                     int minw, const char * const *items, int n,
                     uint32_t disabled, int sel)
{
  struct kit_popup_s *p = &k->pop;
  int w = minw;
  int h = 8;
  int t;

  tip_hide(k);
  pop_close(k);
  n = MIN(n, KIT_POP_MAX);
  for (t = 0; t < n; t++)
    {
      int tw = text_width(KIT_FONT, items[t]) + (owner != KIT_NONE ? 44 : 32);

      p->items[t] = items[t];
      w = tw > w ? tw : w;
      h += pop_item_h(p, t);
    }

  p->open     = true;
  p->owner    = owner;
  p->id       = id;
  p->n        = n;
  p->sel      = sel;
  p->hover    = sel;
  p->disabled = disabled;
  p->armed    = false;

  /* In the kit's area: below (x, y), else above y - (the owner's height),
   * else as low as it fits
   */

  if (owner != KIT_NONE && y + h > k->area.y + k->area.h)
    {
      struct kit_widget_s *wg = &k->wg[owner];

      if (wg->y - 2 - h >= k->area.y)
        {
          y = wg->y - 2 - h;
        }
    }

  if (y + h > k->area.y + k->area.h)
    {
      y = k->area.y + k->area.h - h;
    }

  if (y < k->area.y)
    {
      y = k->area.y;
      h = MIN(h, k->area.h);
    }

  if (x + w > k->area.x + k->area.w)
    {
      x = k->area.x + k->area.w - w;
    }

  x = x < k->area.x ? k->area.x : x;
  p->r = r_make(x, y, w, h);
  pop_draw(k);
  r_add(&k->drawn, &p->r);
  dirty(k, owner);
}

static void pop_choose(struct kit_s *k, int t)
{
  struct kit_popup_s *p = &k->pop;
  int owner = p->owner;

  pop_close(k);
  if (owner != KIT_NONE)
    {
      if (t >= 0 && t != k->wg[owner].value)
        {
          k->wg[owner].value = t;
          dirty(k, owner);
          emit(k, owner, KIT_EV_CHANGE);
        }
    }
  else
    {
      k->arg = t;
      k->key = p->id;
      emit(k, KIT_NONE, KIT_EV_MENU);
    }
}

static void pop_mouse(struct kit_s *k, int x, int y, bool press,
                      bool release)
{
  struct kit_popup_s *p = &k->pop;
  int t = pop_item_at(p, x, y);

  if (t != p->hover && (t >= 0 || r_has(&p->r, x, y)))
    {
      p->hover = t;
      pop_draw(k);
    }

  if (press && !r_has(&p->r, x, y))
    {
      if (p->owner == KIT_NONE)
        {
          pop_choose(k, -1);
        }
      else
        {
          pop_close(k);
        }
    }
  else if (release && t >= 0)
    {
      pop_choose(k, t);
    }
}

static bool pop_key(struct kit_s *k, int key)
{
  struct kit_popup_s *p = &k->pop;
  int t = p->hover;
  int j;

  switch (key & KEY_MASK)
    {
      case KEY_UP:
      case KEY_DOWN:

        /* The next item that can be chosen, round; from none, the first
         * (down) or the last (up)
         */

        if (t < 0)
          {
            t = (key & KEY_MASK) == KEY_UP ? 0 : p->n - 1;
          }

        for (j = 0; j < p->n; j++)
          {
            t = (t + ((key & KEY_MASK) == KEY_UP ? p->n - 1 : 1)) % p->n;
            if (strcmp(p->items[t], "-") != 0 && !(p->disabled & (1u << t)))
              {
                break;
              }
          }

        p->hover = t;
        pop_draw(k);
        return true;

      case '\r':
      case '\n':
      case ' ':
        pop_choose(k, p->hover);
        return true;

      case KEY_ESC:
        if (p->owner == KIT_NONE)
          {
            pop_choose(k, -1);
          }
        else
          {
            pop_close(k);
          }
        return true;

      default:
        return true;                    /* A menu takes every key */
    }
}

/* The widget and its part at (x, y) */

static int hit(struct kit_s *k, int x, int y, int *part)
{
  int found = KIT_NONE;
  int i = 0;

  *part = PART_NONE;
  while (i < k->n)
    {
      struct kit_widget_s *wg = &k->wg[i];
      struct kit_rect_s r = r_of(wg);
      int ty;
      int th;

      if (wg->type != KIT_BOX)
        {
          r = viewport(k, i, r);
        }

      if ((wg->flags & KIT_F_HIDDEN) || !r_has(&r, x, y))
        {
          i = wg->type == KIT_BOX ? wg->end : i + 1;
          continue;
        }

      if (wg->type != KIT_BOX && wg->type != KIT_SPACER &&
          wg->type != KIT_SEPARATOR)
        {
          found = i;
        }
      else if (box_thumb(wg, &ty, &th) && x >= wg->x + wg->w - KIT_SB_W)
        {
          found = i;                     /* A scrolling box's bar */
        }

      i++;
    }

  if (found != KIT_NONE)
    {
      struct kit_widget_s *wg = &k->wg[found];

      switch (wg->type)
        {
          case KIT_LIST:
            {
              struct kit_list_s *l = wg->data;
              struct lgeom_s g;
              int c;

              list_geom(k, wg, &g);
              if (y < g.ry)
                {
                  for (c = 0; c < g.ncols; c++)
                    {
                      if (g.cw[c] > 0 && x >= g.cx[c] &&
                          x < g.cx[c] + g.cw[c] + (c == g.ncols - 1 ?
                                                   g.sbw : 0))
                        {
                          *part = PART_HEAD - c;
                        }
                    }
                }
              else if (x >= g.rx + g.rw)
                {
                  int ty;
                  int th;

                  *part = PART_SB;
                  if (list_thumb(wg, &g, &ty, &th) && y >= ty && y < ty + th)
                    {
                      *part = PART_THUMB;
                    }
                }
              else
                {
                  int row = (y - g.ry + l->top) / g.rowh;

                  *part = row < l->n ? row : PART_NONE;
                }
            }
            break;

          case KIT_TABS:
            *part = tab_part(wg, x, y);
            break;

          case KIT_CRUMBS:
            *part = crumb_part(wg, x);
            break;

          case KIT_STEPPER:
            *part = kit_stepper_at(wg->x, wg->w, x);
            *part = *part < 0 ? 0 : *part > 0 ? 1 : PART_NONE;
            break;

          case KIT_SWATCHES:
            *part = (x - wg->x) / KIT_SWATCH_DX;
            *part = *part < wg->nitems ? *part : PART_NONE;
            break;

          case KIT_BOX:
            {
              int ty;
              int th;

              *part = box_thumb(wg, &ty, &th) && y >= ty && y < ty + th ?
                      PART_THUMB : PART_SB;
            }
            break;

          case KIT_SCROLL:
            {
              int ty;
              int th;

              *part = sb_thumb(wg, &ty, &th) && y >= ty && y < ty + th ?
                      PART_THUMB : PART_SB;
            }
            break;

          default:
            break;
        }
    }

  return found;
}

static void mark_part(struct kit_s *k, int i, int part)
{
  struct kit_widget_s *wg = get(k, i);

  if (wg == NULL)
    {
      return;
    }

  if (wg->type == KIT_LIST)
    {
      struct kit_list_s *l = wg->data;

      if (part >= 0)
        {
          list_row_dirty(wg, part);
        }
      else if (part <= PART_HEAD)
        {
          l->headdirty = true;
        }
      else if (part == PART_SB || part == PART_THUMB)
        {
          l->sbdirty = true;
        }
    }
  else if (wg->type != KIT_CUSTOM && wg->type != KIT_BOX)
    {
      dirty(k, i);
    }
}

static void set_hot(struct kit_s *k, int i, int part)
{
  struct kit_widget_s *wg;

  if (i == k->hot && part == k->hotpart)
    {
      return;
    }

  if (i != k->hot)
    {
      tip_hide(k);
      k->hotms = desk_ms();
    }

  mark_part(k, k->hot, k->hotpart);
  if ((wg = get(k, k->hot)) != NULL && wg->type == KIT_LIST && i != k->hot)
    {
      list_set_hover(k, k->hot, -1);
    }

  k->hot = i;
  k->hotpart = part;
  mark_part(k, i, part);
  if ((wg = get(k, i)) != NULL && wg->type == KIT_LIST)
    {
      list_set_hover(k, i, part >= 0 ? part : -1);
    }
}

static void list_press(struct kit_s *k, int i, int part, int y)
{
  struct kit_widget_s *wg = &k->wg[i];
  struct kit_list_s *l = wg->data;
  struct lgeom_s g;
  uint32_t now = desk_ms();
  int ty;
  int th;

  list_geom(k, wg, &g);
  if (part == PART_THUMB && list_thumb(wg, &g, &ty, &th))
    {
      l->thumbdrag = true;
      l->grab = y - ty;
      l->sbdirty = true;
      return;
    }

  if (part == PART_SB && list_thumb(wg, &g, &ty, &th))
    {
      list_scroll_to(k, i, l->top + (y < ty ? -g.rh : g.rh));
      return;
    }

  if (part <= PART_HEAD || wg->style == KIT_LIST_SIDEBAR)
    {
      if (wg->style == KIT_LIST_SIDEBAR && part >= 0)
        {
          kit_list_select(k, wg->id, part);
          emit(k, i, KIT_EV_CHANGE);
        }

      return;
    }

  if (!(wg->flags & KIT_F_NOFOCUS))
    {
      set_focus(k, i);
      list_row_dirty(wg, l->sel);        /* Its colour: focused */
    }

  if (part >= 0)
    {
      if (part != l->sel)
        {
          kit_list_select(k, wg->id, part);
          emit(k, i, KIT_EV_CHANGE);
        }

      if (k->clickwg == i && k->clickpart == part &&
          now - k->clickms < KIT_DBLCLICK_MS)
        {
          k->clickwg = KIT_NONE;
          emit(k, i, KIT_EV_ACTIVATE);
          return;
        }
    }
  else if (l->sel >= 0)
    {
      kit_list_select(k, wg->id, -1);
      emit(k, i, KIT_EV_CHANGE);
    }

  k->clickwg   = i;
  k->clickpart = part;
  k->clickms   = now;
}

static void press_at(struct kit_s *k, int i, int part, int x, int y)
{
  struct kit_widget_s *wg = &k->wg[i];

  k->down = i;
  k->downpart = part;
  switch (wg->type)
    {
      case KIT_SLIDER:
        {
          int v = kit_slider_value(wg->x, wg->w, wg->min, wg->max, x);

          if (v != wg->value)
            {
              wg->value = v;
              emit(k, i, KIT_EV_CHANGE);
            }
        }
        break;

      case KIT_FIELD:
        if (wg->data != NULL)
          {
            struct ui_field_s *f = wg->data;

            set_focus(k, i);
            f->cur = kit_field_pos(f, wg->x, wg->w, x);
            wg->flags &= ~KIT_F_SELECTED;
            k->caret = true;
          }
        break;

      case KIT_DROPDOWN:
        k->down = KIT_NONE;
        if (!(wg->flags & KIT_F_NOFOCUS))
          {
            set_focus(k, i);
          }

        pop_open(k, i, 0, wg->x, wg->y + wg->h + 2, wg->w, wg->data,
                 wg->nitems, 0, wg->value);
        break;

      case KIT_LIST:
        list_press(k, i, part, y);
        break;

      case KIT_TABS:
        if (part >= 0 && !(part & 1) && part / 2 != wg->value)
          {
            wg->value = part / 2;
            emit(k, i, KIT_EV_CHANGE);
          }
        break;

      case KIT_SCROLL:
        {
          int ty;
          int th;

          if (!sb_thumb(wg, &ty, &th))
            {
              break;
            }

          if (part == PART_THUMB)
            {
              k->grab = y - ty;
            }
          else
            {
              sb_set(k, i, wg->value + (y < ty ? -wg->min : wg->min));
            }
        }
        break;

      case KIT_CUSTOM:
        if (!(wg->flags & KIT_F_NOFOCUS))
          {
            set_focus(k, i);
          }
        break;

      case KIT_STEPPER:
        if (part >= 0)
          {
            k->arg = part ? 1 : -1;
            emit(k, i, KIT_EV_CHANGE);
          }
        break;

      case KIT_BOX:
        {
          int ty;
          int th;

          if (!box_thumb(wg, &ty, &th))
            {
              break;
            }

          if (part == PART_THUMB)
            {
              k->grab = y - ty;
            }
          else
            {
              box_scroll_to(k, i, wg->value + (y < ty ? -wg->min :
                                               wg->min) * 3 / 4);
            }
        }

        return;                         /* Drawn when laid out again */

      case KIT_SWATCHES:
        if (part >= 0 && part != wg->value)
          {
            wg->value = part;
            emit(k, i, KIT_EV_CHANGE);
          }
        break;

      default:
        break;
    }

  dirty(k, i);
  if (wg->type == KIT_LIST)
    {
      wg->flags &= ~KIT_F_DIRTY;
    }
}

/* The pointer moves while a widget holds it */

static void drag(struct kit_s *k, int x, int y)
{
  struct kit_widget_s *wg = &k->wg[k->down];
  int i = k->down;

  switch (wg->type)
    {
      case KIT_SLIDER:
        {
          int v = kit_slider_value(wg->x, wg->w, wg->min, wg->max, x);

          if (v != wg->value)
            {
              wg->value = v;
              dirty(k, i);
              emit(k, i, KIT_EV_CHANGE);
            }
        }
        break;

      case KIT_LIST:
        {
          struct kit_list_s *l = wg->data;
          struct lgeom_s g;
          int ty;
          int th;

          list_geom(k, wg, &g);
          if (l->thumbdrag && list_thumb(wg, &g, &ty, &th) && g.rh > th)
            {
              list_scroll_to(k, i, (y - l->grab - g.ry) * g.maxtop /
                                   (g.rh - th));
            }
        }
        break;

      case KIT_BOX:
        {
          int ty;
          int th;

          if (k->downpart == PART_THUMB && box_thumb(wg, &ty, &th) &&
              wg->h > th)
            {
              box_scroll_to(k, i, (y - k->grab - wg->y) *
                                  (wg->max - wg->min) / (wg->h - th));
            }
        }
        break;

      case KIT_SCROLL:
        {
          int ty;
          int th;

          if (k->downpart == PART_THUMB && sb_thumb(wg, &ty, &th) &&
              wg->h > th)
            {
              sb_set(k, i, (y - k->grab - wg->y) * (wg->max - wg->min) /
                           (wg->h - th));
            }
        }
        break;

      default:
        break;
    }
}

static void release(struct kit_s *k, int over, int part)
{
  int i = k->down;
  struct kit_widget_s *wg = &k->wg[i];
  bool same = over == i && part == k->downpart;

  k->down = KIT_NONE;
  if (wg->type != KIT_BOX)
    {
      dirty(k, i);
    }

  switch (wg->type)
    {
      case KIT_BUTTON:
      case KIT_CHECK_BOX:
      case KIT_RADIO_BTN:
      case KIT_SWITCH:
        if (over == i)
          {
            click(k, i);
          }
        break;

      case KIT_LIST:
        {
          struct kit_list_s *l = wg->data;

          wg->flags &= ~KIT_F_DIRTY;
          if (l->thumbdrag)
            {
              l->thumbdrag = false;
              l->sbdirty = true;
            }
          else if (same && part <= PART_HEAD && l->sortcol >= 0)
            {
              int col = PART_HEAD - part;

              l->sortdesc = l->sortcol == col ? !l->sortdesc : false;
              l->sortcol  = col;
              l->headdirty = true;
              emit(k, i, KIT_EV_SORT);
            }
        }
        break;

      case KIT_TABS:
        if (same && part >= 0 && (part & 1))
          {
            k->arg = part / 2;
            emit(k, i, KIT_EV_CLOSE);
          }
        break;

      case KIT_CRUMBS:
        if (same && part >= 0)
          {
            k->arg = part;
            emit(k, i, KIT_EV_CLICK);
          }
        break;

      case KIT_SLIDER:
        emit(k, i, KIT_EV_ACTIVATE);        /* Let go: the value is final */
        break;

      default:
        break;
    }
}

/* Tab and Shift-Tab */

static void focus_step(struct kit_s *k, int dir)
{
  int i = k->focus == KIT_NONE ? (dir > 0 ? k->n - 1 : 0) : k->focus;
  int n;

  for (n = 0; n < k->n; n++)
    {
      i = (i + dir + k->n) % k->n;
      if (focusable(k, i))
        {
          set_focus(k, i);
          k->ring = true;
          return;
        }
    }
}

static int items_step(struct kit_widget_s *wg, int d)
{
  int v = wg->value + d;

  return v < 0 ? 0 : v >= wg->nitems ? wg->nitems - 1 : v;
}

/* A key for the focused widget i: true if it took it */

static bool widget_key(struct kit_s *k, int i, int key)
{
  struct kit_widget_s *wg = &k->wg[i];
  int base = key & ~KEY_SHIFT;

  switch (wg->type)
    {
      case KIT_BUTTON:
      case KIT_CHECK_BOX:
      case KIT_SWITCH:
      case KIT_RADIO_BTN:
        if (key == ' ' || (wg->type == KIT_BUTTON &&
                           (key == '\r' || key == '\n')))
          {
            click(k, i);
            return true;
          }

        if (wg->type == KIT_RADIO_BTN &&
            (base == KEY_UP || base == KEY_DOWN || base == KEY_LEFT ||
             base == KEY_RIGHT))
          {
            int p = parent_of(k, i);
            int back = base == KEY_UP || base == KEY_LEFT;
            int j;
            int last = KIT_NONE;

            for (j = p + 1; p >= 0 && j < k->wg[p].end; j = k->wg[j].end)
              {
                if (k->wg[j].type != KIT_RADIO_BTN || !enabled(&k->wg[j]))
                  {
                    continue;
                  }

                if (j == i && back && last != KIT_NONE)
                  {
                    break;
                  }

                if (!back && last == i)
                  {
                    last = j;
                    break;
                  }

                last = j;
              }

            if (last != KIT_NONE && last != i &&
                (back ? last < i : last > i))
              {
                set_focus(k, last);
                click(k, last);
              }

            return true;
          }
        return false;

      case KIT_SLIDER:
        {
          int step = (wg->max - wg->min) / 20;
          int v = wg->value;

          step = step < 1 ? 1 : step;
          switch (base)
            {
              case KEY_LEFT:
              case KEY_DOWN:
                v -= step;
                break;

              case KEY_RIGHT:
              case KEY_UP:
                v += step;
                break;

              case KEY_HOME:
                v = wg->min;
                break;

              case KEY_END:
                v = wg->max;
                break;

              default:
                return false;
            }

          v = v < wg->min ? wg->min : v > wg->max ? wg->max : v;
          if (v != wg->value)
            {
              wg->value = v;
              dirty(k, i);
              emit(k, i, KIT_EV_CHANGE);
              emit(k, i, KIT_EV_ACTIVATE);
            }

          return true;
        }

      case KIT_STEPPER:
        if (base == KEY_LEFT || base == KEY_DOWN || key == '-')
          {
            k->arg = -1;
          }
        else if (base == KEY_RIGHT || base == KEY_UP || key == '+')
          {
            k->arg = 1;
          }
        else
          {
            return false;
          }

        emit(k, i, KIT_EV_CHANGE);
        return true;

      case KIT_SWATCHES:
        {
          int v = wg->value;

          if (base == KEY_LEFT || base == KEY_UP)
            {
              v = v > 0 ? v - 1 : 0;
            }
          else if (base == KEY_RIGHT || base == KEY_DOWN)
            {
              v = v + 1 < wg->nitems ? v + 1 : v;
            }
          else
            {
              return false;
            }

          if (v != wg->value)
            {
              wg->value = v;
              dirty(k, i);
              emit(k, i, KIT_EV_CHANGE);
            }

          return true;
        }

      case KIT_DROPDOWN:
      case KIT_TABS:
        {
          int v = wg->value;

          if (wg->type == KIT_DROPDOWN &&
              (key == ' ' || key == '\r' || base == (KEY_DOWN | KEY_CTRL)))
            {
              pop_open(k, i, 0, wg->x, wg->y + wg->h + 2, wg->w, wg->data,
                       wg->nitems, 0, wg->value);
              return true;
            }

          if (base == KEY_UP || base == KEY_LEFT)
            {
              v = items_step(wg, -1);
            }
          else if (base == KEY_DOWN || base == KEY_RIGHT)
            {
              v = items_step(wg, 1);
            }
          else
            {
              return false;
            }

          if (v != wg->value)
            {
              wg->value = v;
              dirty(k, i);
              emit(k, i, KIT_EV_CHANGE);
            }

          return true;
        }

      case KIT_LIST:
        {
          struct kit_list_s *l = wg->data;
          int page = kit_list_visible(k, wg->id) - 1;
          int row = l->sel;

          page = page < 1 ? 1 : page;
          switch (base)
            {
              case KEY_UP:
                row = row < 0 ? l->n - 1 : row > 0 ? row - 1 : 0;
                break;

              case KEY_DOWN:
                row = row + 1 < l->n ? row + 1 : row;
                break;

              case KEY_PGUP:
                row = row - page < 0 ? 0 : row - page;
                break;

              case KEY_PGDN:
                row = row + page >= l->n ? l->n - 1 : row + page;
                break;

              case KEY_HOME:
                row = 0;
                break;

              case KEY_END:
                row = l->n - 1;
                break;

              case '\r':
              case '\n':
                if (l->sel >= 0)
                  {
                    emit(k, i, KIT_EV_ACTIVATE);
                    return true;
                  }
                return false;

              default:
                return false;
            }

          if (l->n > 0 && row != l->sel)
            {
              kit_list_select(k, wg->id, row);
              emit(k, i, KIT_EV_CHANGE);
            }
          else if (l->n > 0)
            {
              kit_list_show(k, wg->id, row);
            }

          return true;
        }

      case KIT_FIELD:
        {
          struct ui_field_s *f = wg->data;
          char before[UI_FIELD_MAX];

          if (f == NULL)
            {
              return false;
            }

          if (key == '\r' || key == '\n')
            {
              emit(k, i, KIT_EV_ACTIVATE);
              return true;
            }

          if (key == '\t' || key == KEY_ESC || base == KEY_UP ||
              base == KEY_DOWN || base == KEY_PGUP || base == KEY_PGDN)
            {
              return false;
            }

          memcpy(before, f->text, f->len + 1);

          /* All of it selected: a character (or a paste) replaces it,
           * Backspace and Delete clear it, a move just ends it
           */

          if (wg->flags & KIT_F_SELECTED)
            {
              wg->flags &= ~KIT_F_SELECTED;
              dirty(k, i);
              if (base == KEY_LEFT || base == KEY_HOME)
                {
                  f->cur = 0;
                  return true;
                }

              if (base == KEY_RIGHT || base == KEY_END)
                {
                  f->cur = f->len;
                  return true;
                }

              if (key == 0x7f || key == 0x08 || base == KEY_DEL ||
                  (key >= 0x20 && key <= 0xff) || key == KEY_CTL('v'))
                {
                  ui_field_set(f, "");
                  if (key == 0x7f || key == 0x08 || base == KEY_DEL)
                    {
                      emit(k, i, KIT_EV_CHANGE);
                      return true;
                    }
                }
            }

          if (!ui_field_key(f, key))
            {
              return false;
            }

          k->caret = true;
          dirty(k, i);
          if (strcmp(before, f->text) != 0)
            {
              emit(k, i, KIT_EV_CHANGE);
            }

          return true;
        }

      default:
        return false;
    }
}

static int default_button(struct kit_s *k)
{
  int i;

  for (i = 0; i < k->n; i++)
    {
      struct kit_widget_s *wg = &k->wg[i];

      if (wg->type == KIT_BUTTON && wg->style == KIT_BTN_DEFAULT &&
          enabled(wg) && shown(k, i))
        {
          return i;
        }
    }

  return KIT_NONE;
}

static void key_one(struct kit_s *k, int key)
{
  struct kit_widget_s *f;
  int j;

  tip_hide(k);
  if (k->pop.open)
    {
      pop_key(k, key);
      return;
    }

  for (j = 0; j < KIT_MAX_SHORTCUTS; j++)
    {
      struct kit_widget_s *wg;

      if (k->sc[j].id != 0 && k->sc[j].key == key &&
          (wg = kit_get(k, k->sc[j].id)) != NULL)
        {
          click(k, index_of(k, wg));
          return;
        }
    }

  f = get(k, k->focus);
  if (f != NULL && (!shown(k, k->focus) || !enabled(f)))
    {
      f = NULL;
    }

  if (f != NULL && f->type == KIT_CUSTOM && (f->flags & KIT_F_KEYS))
    {
      k->key = key;
      emit(k, k->focus, KIT_EV_KEY);
      return;
    }

  if (key == '\t' || key == (KEY_SHIFT | '\t'))
    {
      focus_step(k, key == '\t' ? 1 : -1);
      return;
    }

  if (f != NULL && widget_key(k, k->focus, key))
    {
      if (f->type != KIT_FIELD)
        {
          k->ring = true;
        }

      return;
    }

  k->key = key;
  emit(k, f != NULL ? k->focus : KIT_NONE, KIT_EV_KEY);
  if (k->key != 0 && (key == '\r' || key == '\n') &&
      (j = default_button(k)) != KIT_NONE)
    {
      click(k, j);
    }
}

/****************************************************************************
 * Public Functions: building
 ****************************************************************************/

void kit_init(struct kit_s *k, struct desk_win_s *w,
              struct kit_widget_s *wg, int max, kit_event_t event)
{
  memset(k, 0, sizeof(*k));
  if (g_kops == NULL)
    {
      g_kops = malloc(sizeof(*g_kops));
      if (g_kops != NULL)
        {
          ops_reset(g_kops);
        }
    }

  k->w         = w;
  k->wg        = wg;
  k->max       = g_kops == NULL ? 0 :
                 max > KIT_NONE - 1 ? KIT_NONE - 1 : max;
  k->event     = event;
  k->hot       = KIT_NONE;
  k->down      = KIT_NONE;
  k->focus     = KIT_NONE;
  k->clickwg   = KIT_NONE;
  k->hotpart   = PART_NONE;
  k->downpart  = PART_NONE;
  k->caret     = true;
  k->pop.owner = KIT_NONE;
}

struct kit_widget_s *kit_add(struct kit_s *k, int type, int id,
                             const char *text)
{
  struct kit_widget_s *wg;
  int i;
  int d;

  if (k->n >= k->max)
    {
      memset(&g_spare, 0, sizeof(g_spare));
      return &g_spare;
    }

  i  = k->n++;
  wg = &k->wg[i];
  memset(wg, 0, sizeof(*wg));
  wg->type  = type;
  wg->id    = id;
  wg->text  = text;
  wg->end   = i + 1;
  wg->flags = KIT_F_DIRTY;
  for (d = 0; d < k->depth; d++)
    {
      k->wg[k->stack[d]].end = i + 1;
    }

  return wg;
}

static struct kit_widget_s *box(struct kit_s *k, int style, bool row)
{
  struct kit_widget_s *wg = kit_add(k, KIT_BOX, 0, NULL);

  wg->style = style;
  wg->flags |= row ? KIT_F_ROW : 0;
  wg->gap   = KIT_GAP;
  switch (style)
    {
      case KIT_BOX_WINDOW:
        wg->gap  = 0;
        break;

      case KIT_BOX_BAR:
        wg->padx = 10;
        wg->pady = (KIT_BAR_H - KIT_CONTROL_H) / 2;
        wg->gap  = 6;
        break;

      case KIT_BOX_STATUS:
        wg->padx = KIT_PAD;
        wg->gap  = KIT_PAD;
        wg->ph   = KIT_STATUS_H;
        break;

      case KIT_BOX_SIDEBAR:
        wg->padx = 8;
        wg->pady = 8;
        wg->gap  = 4;
        break;

      case KIT_BOX_CARD:
      case KIT_BOX_GROUP:
        wg->padx = KIT_PAD;
        wg->pady = KIT_PAD - 2;
        break;

      case KIT_BOX_DIALOG:
        wg->padx = 18;
        wg->pady = 16;
        wg->gap  = 10;
        break;

      default:
        break;
    }

  if (wg != &g_spare && k->depth < KIT_DEPTH)
    {
      k->stack[k->depth++] = wg - k->wg;
    }

  return wg;
}

void kit_box_scroll(struct kit_s *k, int id, int offset)
{
  struct kit_widget_s *wg = kit_get(k, id);

  if (wg != NULL && wg->type == KIT_BOX)
    {
      box_scroll_to(k, index_of(k, wg), offset);
    }
}

struct kit_widget_s *kit_row(struct kit_s *k, int style)
{
  return box(k, style, true);
}

struct kit_widget_s *kit_col(struct kit_s *k, int style)
{
  return box(k, style, false);
}

void kit_end(struct kit_s *k)
{
  if (k->depth > 0)
    {
      k->depth--;
    }
}

struct kit_widget_s *kit_label(struct kit_s *k, int id, const char *text,
                               int style)
{
  struct kit_widget_s *wg = kit_add(k, KIT_LABEL, id, text);

  wg->style = style;
  return wg;
}

struct kit_widget_s *kit_button(struct kit_s *k, int id, const char *text,
                                int icon, int style)
{
  struct kit_widget_s *wg = kit_add(k, KIT_BUTTON, id, text);

  wg->icon  = icon;
  wg->style = style;
  return wg;
}

struct kit_widget_s *kit_check(struct kit_s *k, int id, const char *text,
                               bool on)
{
  struct kit_widget_s *wg = kit_add(k, KIT_CHECK_BOX, id, text);

  wg->value = on;
  return wg;
}

struct kit_widget_s *kit_radio(struct kit_s *k, int id, const char *text,
                               bool on)
{
  struct kit_widget_s *wg = kit_add(k, KIT_RADIO_BTN, id, text);

  wg->value = on;
  return wg;
}

struct kit_widget_s *kit_switch(struct kit_s *k, int id, bool on)
{
  struct kit_widget_s *wg = kit_add(k, KIT_SWITCH, id, NULL);

  wg->value = on;
  return wg;
}

struct kit_widget_s *kit_slider(struct kit_s *k, int id, int value,
                                int min, int max)
{
  struct kit_widget_s *wg = kit_add(k, KIT_SLIDER, id, NULL);

  wg->value = value;
  wg->min   = min;
  wg->max   = max;
  return wg;
}

struct kit_widget_s *kit_progress(struct kit_s *k, int id, int value,
                                  int max)
{
  struct kit_widget_s *wg = kit_add(k, KIT_PROGRESS, id, NULL);

  wg->value = value;
  wg->max   = max;
  return wg;
}

struct kit_widget_s *kit_field(struct kit_s *k, int id,
                               struct ui_field_s *f,
                               const char *placeholder)
{
  struct kit_widget_s *wg = kit_add(k, KIT_FIELD, id, placeholder);

  wg->data = f;
  return wg;
}

struct kit_widget_s *kit_dropdown(struct kit_s *k, int id,
                                  const char * const *items, int n,
                                  int value)
{
  struct kit_widget_s *wg = kit_add(k, KIT_DROPDOWN, id, NULL);

  wg->data   = (void *)items;
  wg->nitems = n;
  wg->value  = value;
  return wg;
}

struct kit_widget_s *kit_list(struct kit_s *k, int id,
                              struct kit_list_s *l, int style)
{
  struct kit_widget_s *wg = kit_add(k, KIT_LIST, id, NULL);

  wg->data  = l;
  wg->style = style;
  l->hover  = -1;
  l->alldirty = true;
  return wg;
}

struct kit_widget_s *kit_tabs(struct kit_s *k, int id,
                              const char * const *items, int n, int value,
                              int style)
{
  struct kit_widget_s *wg = kit_add(k, KIT_TABS, id, NULL);

  wg->data   = (void *)items;
  wg->nitems = n;
  wg->value  = value;
  wg->style  = style;
  return wg;
}

struct kit_widget_s *kit_scroll(struct kit_s *k, int id)
{
  return kit_add(k, KIT_SCROLL, id, NULL);
}

struct kit_widget_s *kit_crumbs(struct kit_s *k, int id,
                                const char * const *items, int n)
{
  struct kit_widget_s *wg = kit_add(k, KIT_CRUMBS, id, NULL);

  wg->data   = (void *)items;
  wg->nitems = n;
  return wg;
}

struct kit_widget_s *kit_separator(struct kit_s *k)
{
  return kit_add(k, KIT_SEPARATOR, 0, NULL);
}

struct kit_widget_s *kit_spacer(struct kit_s *k, int grow)
{
  struct kit_widget_s *wg = kit_add(k, KIT_SPACER, 0, NULL);

  wg->grow = grow;
  return wg;
}

struct kit_widget_s *kit_custom(struct kit_s *k, int id)
{
  return kit_add(k, KIT_CUSTOM, id, NULL);
}

struct kit_widget_s *kit_stepper(struct kit_s *k, int id,
                                 const char *value)
{
  return kit_add(k, KIT_STEPPER, id, value);
}

struct kit_widget_s *kit_swatches(struct kit_s *k, int id,
                                  const uint16_t *colours, int n,
                                  int value)
{
  struct kit_widget_s *wg = kit_add(k, KIT_SWATCHES, id, NULL);

  wg->data   = (void *)colours;
  wg->nitems = n;
  wg->value  = value;
  return wg;
}

struct kit_widget_s *kit_image(struct kit_s *k, int id,
                               struct kit_surface_s *s)
{
  struct kit_widget_s *wg = kit_add(k, KIT_IMAGE, id, NULL);

  wg->data = s;
  return wg;
}

struct kit_widget_s *kit_card(struct kit_s *k)
{
  struct kit_widget_s *wg = kit_col(k, KIT_BOX_CARD);

  wg->padx   = 0;
  wg->pady   = 0;
  wg->gap    = 0;
  wg->flags |= KIT_F_LINES;
  return wg;
}

struct desk_ops_s *kit_ops(struct kit_s *k, int n)
{
  need(k, n);
  return g_kops;
}

struct kit_widget_s *kit_card_row(struct kit_s *k, int id,
                                  const char *label)
{
  struct kit_widget_s *row = kit_row(k, KIT_BOX_NONE);

  row->padx = 14;
  row->gap  = 12;
  row->ph   = KIT_CARD_ROW_H;
  kit_label(k, id, label, KIT_TEXT_BODY);
  kit_grow(k, 1);
  return row;
}

int kit_look(struct kit_s *k, struct kit_widget_s *wg)
{
  int i = index_of(k, wg);

  if (i == KIT_NONE)
    {
      return KIT_LOOK_NORMAL;
    }

  return look_of(k, i) | ((wg->flags & KIT_F_RING) ||
                          (k->focus == i && k->ring && k->w->active) ?
                          KIT_LOOK_FOCUS : 0);
}

void kit_grow(struct kit_s *k, int grow)
{
  if (k->n > 0)
    {
      k->wg[k->n - 1].grow = grow;
    }
}

void kit_size(struct kit_s *k, int w, int h)
{
  if (k->n > 0)
    {
      k->wg[k->n - 1].pw = w;
      k->wg[k->n - 1].ph = h;
    }
}

void kit_tip(struct kit_s *k, const char *tip)
{
  if (k->n > 0)
    {
      k->wg[k->n - 1].tip = tip;
    }
}

void kit_shortcut(struct kit_s *k, int key, int id)
{
  int j;

  for (j = 0; j < KIT_MAX_SHORTCUTS; j++)
    {
      if (k->sc[j].id == 0 || k->sc[j].key == key)
        {
          k->sc[j].key = key;
          k->sc[j].id  = id;
          return;
        }
    }
}

/****************************************************************************
 * Public Functions: changing widgets
 ****************************************************************************/

struct kit_widget_s *kit_get(struct kit_s *k, int id)
{
  int i;

  for (i = 0; id != 0 && i < k->n; i++)
    {
      if (k->wg[i].id == id)
        {
          return &k->wg[i];
        }
    }

  return NULL;
}

/* The box of a widget whose size may have changed: laid out again */

static void relayout_parent(struct kit_s *k, struct kit_widget_s *wg)
{
  int p = parent_of(k, index_of(k, wg));

  if (p >= 0)
    {
      k->wg[p].flags |= KIT_F_RELAYOUT;
    }
}

void kit_set_text(struct kit_s *k, int id, const char *text)
{
  struct kit_widget_s *wg = kit_get(k, id);

  if (wg == NULL)
    {
      return;
    }

  /* Its size may change (the same buffer may hold a new text): its box
   * is laid out again, unless its size is not its text's
   */

  if (wg->grow == 0 && wg->pw == 0 && wg->type != KIT_FIELD &&
      wg->type != KIT_STEPPER)
    {
      relayout_parent(k, wg);
    }

  wg->text = text;
  wg->flags |= KIT_F_DIRTY;
}

void kit_set_value(struct kit_s *k, int id, int value)
{
  struct kit_widget_s *wg = kit_get(k, id);

  if (wg != NULL && wg->value != value)
    {
      if (wg->type == KIT_RADIO_BTN && value)
        {
          radio_set(k, index_of(k, wg));
        }

      wg->value = value;
      wg->flags |= KIT_F_DIRTY;
    }
}

void kit_set_items(struct kit_s *k, int id, const char * const *items,
                   int n)
{
  struct kit_widget_s *wg = kit_get(k, id);

  if (wg != NULL)
    {
      /* Other items: the part under the pointer is found again when it
       * moves
       */

      if (n != wg->nitems && index_of(k, wg) == k->hot)
        {
          k->hotpart = PART_NONE;
        }

      wg->data   = (void *)items;
      wg->nitems = n;
      wg->flags |= KIT_F_DIRTY;
      if (wg->type == KIT_DROPDOWN || wg->type == KIT_TABS)
        {
          wg->value = wg->value >= n ? n - 1 : wg->value;
        }

    }
}

void kit_enable(struct kit_s *k, int id, bool on)
{
  struct kit_widget_s *wg = kit_get(k, id);

  if (wg != NULL && enabled(wg) != on)
    {
      wg->flags ^= KIT_F_DISABLED;
      wg->flags |= KIT_F_DIRTY;
      if (!on && index_of(k, wg) == k->focus)
        {
          struct kit_rect_s e = extent(k, k->focus);

          kit_damage(k, e.x, e.y, e.w, e.h);
        }
    }
}

void kit_show(struct kit_s *k, int id, bool on)
{
  struct kit_widget_s *wg = kit_get(k, id);

  if (wg != NULL && !(wg->flags & KIT_F_HIDDEN) != on)
    {
      wg->flags ^= KIT_F_HIDDEN;
      relayout_parent(k, wg);
      if (!on && index_of(k, wg) == k->hot)
        {
          k->hot = KIT_NONE;
        }
    }
}

void kit_set_style(struct kit_s *k, int id, int style)
{
  struct kit_widget_s *wg = kit_get(k, id);

  if (wg != NULL && wg->style != style)
    {
      wg->style = style;
      wg->flags |= KIT_F_DIRTY;
    }
}

void kit_scroll_set(struct kit_s *k, int id, int pos, int page, int total)
{
  struct kit_widget_s *wg = kit_get(k, id);

  if (wg != NULL && (wg->value != pos || wg->min != page ||
                     wg->max != total))
    {
      wg->value = pos;
      wg->min   = page;
      wg->max   = total;
      wg->flags |= KIT_F_DIRTY;
    }
}

void kit_focus(struct kit_s *k, int id)
{
  struct kit_widget_s *wg = kit_get(k, id);

  set_focus(k, wg != NULL ? index_of(k, wg) : KIT_NONE);
}

void kit_select_all(struct kit_s *k, int id)
{
  struct kit_widget_s *wg = kit_get(k, id);

  if (wg != NULL && wg->type == KIT_FIELD)
    {
      wg->flags |= KIT_F_SELECTED | KIT_F_DIRTY;
    }
}

void kit_invalidate(struct kit_s *k, int id)
{
  struct kit_widget_s *wg = kit_get(k, id);

  if (wg != NULL)
    {
      wg->flags |= KIT_F_DIRTY;
      if (wg->type == KIT_LIST)
        {
          ((struct kit_list_s *)wg->data)->alldirty = true;
        }
    }
}

void kit_damage(struct kit_s *k, int x, int y, int w, int h)
{
  struct kit_rect_s r = r_make(x, y, w, h);

  r_add(&k->damage, &r);
}

/****************************************************************************
 * Public Functions: lists
 ****************************************************************************/

static struct kit_widget_s *get_list(struct kit_s *k, int id)
{
  struct kit_widget_s *wg = kit_get(k, id);

  return wg != NULL && wg->type == KIT_LIST && wg->data != NULL ? wg : NULL;
}

void kit_list_changed(struct kit_s *k, int id)
{
  struct kit_widget_s *wg = get_list(k, id);
  struct kit_list_s *l;
  struct lgeom_s g;

  if (wg == NULL)
    {
      return;
    }

  l = wg->data;
  list_geom(k, wg, &g);
  l->top = l->top > g.maxtop ? g.maxtop : l->top < 0 ? 0 : l->top;
  l->sel = l->sel >= l->n ? l->n - 1 : l->sel;
  l->hover = -1;
  if (k->hot == index_of(k, wg))
    {
      k->hotpart = PART_NONE;
    }

  wg->flags |= KIT_F_DIRTY;
  l->alldirty = true;
}

void kit_list_row(struct kit_s *k, int id, int row)
{
  struct kit_widget_s *wg = get_list(k, id);

  if (wg != NULL)
    {
      list_row_dirty(wg, row);
    }
}

void kit_list_show(struct kit_s *k, int id, int row)
{
  struct kit_widget_s *wg = get_list(k, id);
  struct kit_list_s *l;
  struct lgeom_s g;
  int y;

  if (wg == NULL || row < 0)
    {
      return;
    }

  l = wg->data;
  list_geom(k, wg, &g);
  y = row * g.rowh;
  if (y < l->top)
    {
      list_scroll_to(k, index_of(k, wg), y);
    }
  else if (y + g.rowh > l->top + g.rh)
    {
      list_scroll_to(k, index_of(k, wg), y + g.rowh - g.rh);
    }
}

void kit_list_select(struct kit_s *k, int id, int row)
{
  struct kit_widget_s *wg = get_list(k, id);
  struct kit_list_s *l;

  if (wg == NULL)
    {
      return;
    }

  l = wg->data;
  row = row >= l->n ? l->n - 1 : row < -1 ? -1 : row;
  if (row != l->sel)
    {
      list_row_dirty(wg, l->sel);
      list_row_dirty(wg, row);
      l->sel = row;
    }

  kit_list_show(k, id, row);
}

void kit_list_scroll(struct kit_s *k, int id, int top)
{
  struct kit_widget_s *wg = get_list(k, id);

  if (wg != NULL)
    {
      list_scroll_to(k, index_of(k, wg), top);
    }
}

int kit_list_at(struct kit_s *k, int id, int x, int y)
{
  struct kit_widget_s *wg = get_list(k, id);
  struct kit_list_s *l;
  struct lgeom_s g;
  int row;

  if (wg == NULL)
    {
      return -1;
    }

  l = wg->data;
  list_geom(k, wg, &g);
  if (x < g.rx || x >= g.rx + g.rw || y < g.ry || y >= g.ry + g.rh)
    {
      return -1;
    }

  row = (y - g.ry + l->top) / g.rowh;
  return row < l->n ? row : -1;
}

int kit_list_visible(struct kit_s *k, int id)
{
  struct kit_widget_s *wg = get_list(k, id);
  struct lgeom_s g;

  if (wg == NULL)
    {
      return 1;
    }

  list_geom(k, wg, &g);
  return g.rh / g.rowh > 0 ? g.rh / g.rowh : 1;
}

/****************************************************************************
 * Public Functions: layout and drawing
 ****************************************************************************/

void kit_place(struct kit_s *k, int x, int y, int w, int h)
{
  int i;

  k->area    = r_make(x, y, w, h);
  k->scrolls = false;
  for (i = 0; i < k->n; i++)
    {
      k->scrolls |= (k->wg[i].flags & KIT_F_SCROLL) != 0;
    }

  if (k->n > 0)
    {
      place(k, 0, x, y, w, h);
    }

  kit_damage(k, x, y, w, h);
}

void kit_natural(struct kit_s *k, int *w, int *h)
{
  *w = 0;
  *h = 0;
  if (k->n > 0)
    {
      measure(k, 0, w, h);
    }
}

void kit_layout(struct kit_s *k)
{
  kit_place(k, 0, TITLE_H, k->w->csize.w, k->w->csize.h);
}

void kit_redraw(struct kit_s *k)
{
  kit_damage(k, k->area.x, k->area.y, k->area.w, k->area.h);
  kit_flush(k);
}

void kit_flush(struct kit_s *k)
{
  struct kit_rect_s drawn;
  int i;

  /* A window going, a dialog closed: nothing more is drawn */

  if (k->w == NULL || k->w->closing || g_kops == NULL ||
      (k->parent != NULL && k->parent->dlg != k))
    {
      if (g_kops != NULL && k->w != NULL &&
          (NXWINDOW)k->w->hwnd == g_khwnd)
        {
          ops_reset(g_kops);
          g_cutfrom = -1;
        }

      return;
    }

  /* No tooltip over a dialog */

  if (k->dlg != NULL && k->tipshown)
    {
      tip_hide(k);
    }

  /* Boxes whose contents changed size */

  for (i = 0; i < k->n; i++)
    {
      struct kit_widget_s *wg = &k->wg[i];

      if (wg->type == KIT_BOX && (wg->flags & KIT_F_RELAYOUT))
        {
          wg->flags &= ~KIT_F_RELAYOUT;
          place(k, i, wg->x, wg->y, wg->w, wg->h);
          kit_damage(k, wg->x, wg->y, wg->w, wg->h);
        }
    }

  /* What was uncovered or exposed: everything there */

  if (k->damage.w > 0 && k->damage.h > 0)
    {
      struct kit_rect_s d = r_and(&k->damage, &k->area);

      k->damage.w = 0;
      area_draw(k, &d);
    }

  /* Widgets that changed (a box: all of it) */

  i = 0;
  while (i < k->n)
    {
      struct kit_widget_s *wg = &k->wg[i];

      if (wg->flags & KIT_F_HIDDEN)
        {
          i = wg->type == KIT_BOX ? wg->end : i + 1;
          continue;
        }

      if (wg->flags & KIT_F_DIRTY)
        {
          if (wg->type == KIT_BOX)
            {
              struct kit_rect_s r = r_of(wg);

              area_draw(k, &r);
              i = wg->end;
              continue;
            }

          wg->flags &= ~KIT_F_DIRTY;
          if (wg->type == KIT_LIST)
            {
              ((struct kit_list_s *)wg->data)->alldirty = true;
            }
          else
            {
              struct kit_rect_s e = extent(k, i);

              widget_draw(k, i, &e);
            }
        }

      /* A list's parts */

      if (wg->type == KIT_LIST)
        {
          struct kit_list_s *l = wg->data;
          struct kit_rect_s r = r_of(wg);
          int j;

          if (l->alldirty)
            {
              list_rows_draw(k, i, &r, 0, l->n + wg->h / KIT_ROW_H + 2);
              l->headdirty = true;
              l->sbdirty = true;
            }
          else
            {
              for (j = 0; j < l->ndirty; j++)
                {
                  list_rows_draw(k, i, &r, l->dirty[j], l->dirty[j]);
                }
            }

          if (l->headdirty)
            {
              list_head_draw(k, i, &r);
            }

          if (l->sbdirty)
            {
              list_sb_draw(k, i, &r);
            }

          l->alldirty  = false;
          l->ndirty    = 0;
          l->headdirty = false;
          l->sbdirty   = false;
        }

      i++;
    }

  drawn = k->drawn;
  k->drawn.w = 0;
  if (drawn.w > 0)
    {
      overlays_draw(k, &drawn);
    }

  kit_send(k);
}

void kit_touched(struct kit_s *k, int x, int y, int w, int h)
{
  struct kit_rect_s r = r_make(x, y, w, h);

  if (k->n == 0)
    {
      return;
    }

  overlays_draw(k, &r);
  kit_send(k);
}

/****************************************************************************
 * Public Functions: events
 ****************************************************************************/

static bool mouse_at(struct kit_s *k, int x, int y, uint8_t buttons)
{
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(k->buttons & NX_MOUSE_LEFTBUTTON);
  bool rel = !(buttons & NX_MOUSE_LEFTBUTTON) &&
             (k->buttons & NX_MOUSE_LEFTBUTTON);
  bool rpress = (buttons & NX_MOUSE_RIGHTBUTTON) &&
                !(k->buttons & NX_MOUSE_RIGHTBUTTON);
  bool mine;
  int part;
  int i;

  k->buttons = buttons;
  k->px = x;
  k->py = y;

  /* A dialog has the window */

  if (k->dlg != NULL)
    {
      struct kit_s *d = k->dlg;

      mouse_at(d, x, y, buttons);
      kit_flush(d);
      return true;
    }

  if (k->pop.open)
    {
      pop_mouse(k, x, y, press, rel);
      return true;
    }

  if (press || rpress)
    {
      tip_hide(k);
    }

  /* A widget holds the pointer while its button is held */

  if (k->down != KIT_NONE)
    {
      struct kit_widget_s *wg = &k->wg[k->down];

      i = hit(k, x, y, &part);
      if (wg->type == KIT_CUSTOM)
        {
          k->mx = x;
          k->my = y;
          k->mbuttons = buttons;
          emit(k, k->down, KIT_EV_MOUSE);
          if (rel)
            {
              k->down = KIT_NONE;
            }

          return true;
        }

      if (buttons & NX_MOUSE_LEFTBUTTON)
        {
          drag(k, x, y);
          if (wg->type != KIT_LIST && wg->type != KIT_SLIDER &&
              wg->type != KIT_SCROLL)
            {
              set_hot(k, i == k->down ? i : KIT_NONE,
                      i == k->down ? part : PART_NONE);
            }

          return true;
        }

      release(k, i, part);
      set_hot(k, i, part);
      return true;
    }

  i = hit(k, x, y, &part);
  mine = i != KIT_NONE || r_has(&k->area, x, y);
  set_hot(k, i, part);
  if (i == KIT_NONE)
    {
      return mine;
    }

  if (k->wg[i].type == KIT_CUSTOM || rpress)
    {
      k->mx = x;
      k->my = y;
      k->mbuttons = buttons;
      if (press && enabled(&k->wg[i]))
        {
          press_at(k, i, part, x, y);
        }

      emit(k, i, KIT_EV_MOUSE);
      return true;
    }

  if (press && enabled(&k->wg[i]))
    {
      press_at(k, i, part, x, y);
    }

  return true;
}

bool kit_mouse(struct kit_s *k, int x, int y, uint8_t buttons)
{
  bool mine;

  if (k->n == 0)
    {
      return false;
    }

  /* The pointer has left the window */

  if (x < 0 && y < 0 && buttons == 0)
    {
      if (k->dlg != NULL)
        {
          kit_mouse(k->dlg, x, y, buttons);
        }

      tip_hide(k);
      if (k->down != KIT_NONE)
        {
          release(k, KIT_NONE, PART_NONE);
        }

      set_hot(k, KIT_NONE, PART_NONE);
      k->buttons = 0;
      kit_flush(k);
      return false;
    }

  mine = mouse_at(k, x, y + TITLE_H, buttons);
  kit_flush(k);
  kit_dialog_reap();
  return mine;
}

int kit_keys(struct kit_s *k, const uint8_t *ch, int n, int *keys,
             int max)
{
  return ui_keys(&k->keys, ch, n, keys, max);
}

bool kit_press_key(struct kit_s *k, int key)
{
  if (k->dlg != NULL)
    {
      return kit_press_key(k->dlg, key);
    }

  k->key = 0;
  key_one(k, key);
  return k->key == 0;
}

bool kit_key(struct kit_s *k, const uint8_t *ch, int n)
{
  int keys[16];
  int nk;
  int j;
  bool all = true;

  if (k->n == 0)
    {
      return false;
    }

  if (k->dlg != NULL)
    {
      kit_key(k->dlg, ch, n);
      kit_flush(k);
      kit_dialog_reap();
      return true;
    }

  nk = kit_keys(k, ch, n, keys, 16);
  for (j = 0; j < nk && !k->w->closing; j++)
    {
      if (k->parent != NULL && k->parent->dlg != k)
        {
          break;                         /* A dialog that has closed */
        }

      if (!kit_press_key(k, keys[j]))
        {
          all = false;
        }

      /* A dialog opened: the rest of the keys are its */

      if (k->dlg != NULL && j + 1 < nk)
        {
          break;
        }
    }

  kit_flush(k);
  if (k->parent == NULL)
    {
      kit_dialog_reap();
    }

  return all;
}

void kit_tick(struct kit_s *k)
{
  struct kit_widget_s *f = get(k, k->focus);
  struct kit_widget_s *h = get(k, k->hot);

  if (k->dlg != NULL)
    {
      kit_tick(k->dlg);
      return;
    }

  /* A field's caret blinks (not while the desktop is frozen: tests) */

  if (f != NULL && f->type == KIT_FIELD && k->w->active &&
      (!desk_frozen() || !k->caret))
    {
      k->caret = !k->caret;
      dirty(k, k->focus);
    }

  /* A tooltip, under the widget (or over it); none while frozen, when
   * they would come or not depending on how long a test takes
   */

  if (h != NULL && h->tip != NULL && !k->tipshown && k->buttons == 0 &&
      !desk_frozen() &&
      !k->pop.open && desk_ms() - k->hotms >= KIT_TIP_MS)
    {
      int th;
      int tw = kit_tooltip_size(h->tip, &th);
      int tx = MIN(h->x, k->area.x + k->area.w - tw - 4);
      int ty = h->y + h->h + 6;

      if (ty + th > k->area.y + k->area.h)
        {
          ty = h->y - th - 6;
        }

      k->tiprect  = r_make(MAX(tx, k->area.x + 4), ty, tw, th);
      k->tipshown = true;
      tip_draw(k);
    }

  kit_flush(k);
}

void kit_expose(struct kit_s *k, int x, int y, int w, int h)
{
  kit_damage(k, x, y + TITLE_H, w, h);
  kit_flush(k);
}

void kit_resize(struct kit_s *k)
{
  kit_layout(k);
  if (k->dlg != NULL)
    {
      /* The dialog stays in the middle */

      struct kit_s *d = k->dlg;
      int w = d->area.w > k->area.w - 24 ? k->area.w - 24 : d->area.w;
      int h = d->area.h > k->area.h - 24 ? k->area.h - 24 : d->area.h;

      kit_place(d, k->area.x + (k->area.w - w) / 2,
                k->area.y + (k->area.h - h) / 2, w, h);
    }

  if (k->pop.open)
    {
      k->pop.open = false;
    }

  k->tipshown = false;
  kit_flush(k);
}

void kit_activate(struct kit_s *k)
{
  int i;

  for (i = 0; i < k->n; i++)
    {
      struct kit_widget_s *wg = &k->wg[i];

      if (wg->type == KIT_LIST)
        {
          list_row_dirty(wg, ((struct kit_list_s *)wg->data)->sel);
        }
      else if ((wg->type == KIT_TABS && wg->style == KIT_TABS_STRIP) ||
               i == k->focus)
        {
          wg->flags |= KIT_F_DIRTY;
          if (i == k->focus && has_ring(wg))
            {
              struct kit_rect_s e = extent(k, i);

              kit_damage(k, e.x, e.y, e.w, e.h);
            }
        }
    }

  if (!k->w->active)
    {
      k->ring = false;
      tip_hide(k);
      pop_close(k);
    }

  if (k->dlg != NULL)
    {
      kit_activate(k->dlg);
    }

  kit_flush(k);
}

void kit_menu(struct kit_s *k, int id, int x, int y,
              const char * const *items, int n, uint32_t disabled)
{
  if (k->n == 0)
    {
      return;
    }

  pop_open(k, KIT_NONE, id, x + 1, y + 1, 120, items, n, disabled, -1);
  kit_send(k);
}

void kit_menu_close(struct kit_s *k)
{
  pop_close(k);
}
