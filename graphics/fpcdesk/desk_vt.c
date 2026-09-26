/****************************************************************************
 * apps/graphics/fpcdesk/desk_vt.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The terminal emulator's screen (desk_vt.h): a VT100 / xterm subset.
 *
 *   C0:   BS HT LF VT FF CR SO SI, ESC; CAN SUB abort a sequence
 *   ESC:  7 8 (save, restore) D E M (index, next line, reverse index)
 *         c (reset), ( ) (G0 / G1: ASCII or DEC graphics), ] (OSC 0 / 2:
 *         the title); others are swallowed
 *   CSI:  A B C D E F G H f d e a ` (cursor), J K (erase), L M (lines),
 *         @ P X (characters), S T (scroll), r (scroll region), s u, b
 *         (repeat), I Z (tabs), m (SGR: bold, dim, underline, reverse,
 *         hidden, 16 / 256 / RGB colours), n (DSR 5, 6), c (DA),
 *         h l (4 insert; ?1 cursor keys, ?6 origin, ?7 wrap, ?25 cursor,
 *         ?47 ?1047 ?1048 ?1049 the alternate screen)
 *   UTF-8: the fonts' extras, box drawing and blocks; others are '?'
 *
 * Lines are allocated with the screen's width and move, as pointers, into
 * the scrollback when they scroll off the top (trimmed of trailing
 * blanks); a full scrollback gives its oldest line back for the new one.
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk_font.h"
#include "desk_vt.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ST_GROUND      0
#define ST_ESC         1
#define ST_CSI         2
#define ST_OSC         3
#define ST_OSCESC      4
#define ST_CHARSET     5               /* ESC ( or ESC ): the set next */
#define ST_SKIP1       6               /* ESC # etc.: one byte more */
#define ST_STR         7               /* DCS, APC, PM, SOS: up to ST */
#define ST_STRESC      8

#define LINE_SIZE(n)   (sizeof(struct vt_line_s) + \
                        (n) * sizeof(struct vt_cell_s))
#define DIRTY_ALL      0xffff

#define BOX_L          1
#define BOX_R          2
#define BOX_U          4
#define BOX_D          8
#define BOX(m)         (VT_CH_BOX | (m))

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct vt_cell_s g_blank =
{
  ' ', 7, 0, VT_FGDEF | VT_BGDEF
};

/* DEC special graphics, 0x60-0x7e */

static const uint8_t g_decgfx[31] =
{
  '*', VT_CH_SHADE, 'H', 'F', 'C', 'L', 0x82, '#',        /* ` a b c d e f g */
  'N', 'V', BOX(BOX_L | BOX_U), BOX(BOX_L | BOX_D),        /* h i j k */
  BOX(BOX_R | BOX_D), BOX(BOX_R | BOX_U),                  /* l m */
  BOX(BOX_L | BOX_R | BOX_U | BOX_D), '-', '-',            /* n o p */
  BOX(BOX_L | BOX_R), '-', '_',                            /* q r s */
  BOX(BOX_R | BOX_U | BOX_D), BOX(BOX_L | BOX_U | BOX_D),  /* t u */
  BOX(BOX_L | BOX_R | BOX_U), BOX(BOX_L | BOX_R | BOX_D),  /* v w */
  BOX(BOX_U | BOX_D), '<', '>', 'n', '#', 'L', 0x85        /* x y z { | } ~ */
};

/* U+2500-U+257F: box drawing, as the lines from the centre it has */

#define BH BOX(BOX_L | BOX_R)
#define BV BOX(BOX_U | BOX_D)
#define BDR BOX(BOX_D | BOX_R)
#define BDL BOX(BOX_D | BOX_L)
#define BUR BOX(BOX_U | BOX_R)
#define BUL BOX(BOX_U | BOX_L)
#define BVR BOX(BOX_U | BOX_D | BOX_R)
#define BVL BOX(BOX_U | BOX_D | BOX_L)
#define BHD BOX(BOX_L | BOX_R | BOX_D)
#define BHU BOX(BOX_L | BOX_R | BOX_U)
#define BX BOX(BOX_L | BOX_R | BOX_U | BOX_D)

static const uint8_t g_box[128] =
{
  BH, BH, BV, BV, BH, BH, BV, BV, BH, BH, BV, BV,                 /* 2500 */
  BDR, BDR, BDR, BDR, BDL, BDL, BDL, BDL,                         /* 250c */
  BUR, BUR, BUR, BUR, BUL, BUL, BUL, BUL,                         /* 2514 */
  BVR, BVR, BVR, BVR, BVR, BVR, BVR, BVR,                         /* 251c */
  BVL, BVL, BVL, BVL, BVL, BVL, BVL, BVL,                         /* 2524 */
  BHD, BHD, BHD, BHD, BHD, BHD, BHD, BHD,                         /* 252c */
  BHU, BHU, BHU, BHU, BHU, BHU, BHU, BHU,                         /* 2534 */
  BX, BX, BX, BX, BX, BX, BX, BX, BX, BX, BX, BX, BX, BX, BX, BX, /* 253c */
  BH, BH, BV, BV, BH, BV,                                         /* 254c */
  BDR, BDR, BDR, BDL, BDL, BDL, BUR, BUR, BUR, BUL, BUL, BUL,     /* 2552 */
  BVR, BVR, BVR, BVL, BVL, BVL, BHD, BHD, BHD, BHU, BHU, BHU,     /* 255e */
  BX, BX, BX, BDR, BDL, BUL, BUR,                                 /* 256a */
  '/', '\\', 'X',                                                 /* 2571 */
  BOX(BOX_L), BOX(BOX_U), BOX(BOX_R), BOX(BOX_D),                 /* 2574 */
  BOX(BOX_L), BOX(BOX_U), BOX(BOX_R), BOX(BOX_D),                 /* 2578 */
  BH, BV, BH, BV                                                  /* 257c */
};

/****************************************************************************
 * Private Functions: lines
 ****************************************************************************/

static bool cell_blank(const struct vt_cell_s *c)
{
  return c->ch == ' ' && (c->at & VT_BGDEF) != 0 &&
         (c->at & (VT_REVERSE | VT_UNDER)) == 0;
}

static void dirty(struct vt_line_s *l, int x0, int x1)
{
  if (l->d0 >= l->d1)
    {
      l->d0 = x0;
      l->d1 = x1;
    }
  else
    {
      if (x0 < l->d0)
        {
          l->d0 = x0;
        }

      if (x1 > l->d1)
        {
          l->d1 = x1;
        }
    }
}

static void cells_fill(struct vt_line_s *l, int x0, int x1,
                       struct vt_cell_s e)
{
  int i;

  if (x1 > l->n)
    {
      x1 = l->n;
    }

  for (i = x0; i < x1; i++)
    {
      l->c[i] = e;
    }
}

static struct vt_line_s *line_alloc(int n)
{
  struct vt_line_s *l = malloc(LINE_SIZE(n));

  if (l != NULL)
    {
      l->n       = n;
      l->wrapped = 0;
      l->d0      = 0;
      l->d1      = DIRTY_ALL;
      cells_fill(l, 0, n, g_blank);
    }

  return l;
}

/* At least the screen's width (a line back from the scrollback, or wider
 * columns); NULL (and the line as it was) without the memory
 */

static struct vt_line_s *line_fit(struct vt_s *vt, struct vt_line_s *l)
{
  struct vt_line_s *nl;
  int n = l->n;

  if (n >= vt->cols)
    {
      return l;
    }

  nl = realloc(l, LINE_SIZE(vt->cols));
  if (nl == NULL)
    {
      return NULL;
    }

  if (nl != l)
    {
      vt->epoch++;
    }

  nl->n  = vt->cols;
  nl->d0 = 0;
  nl->d1 = DIRTY_ALL;
  cells_fill(nl, n, vt->cols, g_blank);
  return nl;
}

/* The erased cell: blank, in the pen's background (xterm's BCE) */

static struct vt_cell_s erased(struct vt_s *vt)
{
  struct vt_cell_s e = g_blank;

  e.bg = vt->pen.bg;
  e.at = VT_FGDEF | (vt->pen.at & VT_BGDEF);
  return e;
}

static void erase(struct vt_s *vt, int y, int x0, int x1)
{
  struct vt_line_s *l = vt->scr[y];

  if (x0 < 0)
    {
      x0 = 0;
    }

  if (x1 > vt->cols)
    {
      x1 = vt->cols;
    }

  if (x0 < x1)
    {
      cells_fill(l, x0, x1, erased(vt));
      dirty(l, x0, x1);
    }

  if (x1 >= vt->cols)
    {
      l->wrapped = 0;
    }
}

/****************************************************************************
 * Private Functions: the scrollback
 ****************************************************************************/

static struct vt_line_s *sb_get(struct vt_s *vt, int i)
{
  return vt->sb[(vt->sbhead + i) % vt->sbcap];
}

/* A line into the scrollback, trimmed.  Returns the line given back for
 * reuse: the oldest one when the scrollback is full, NULL otherwise (or
 * l itself when it cannot be kept).
 */

static struct vt_line_s *sb_push(struct vt_s *vt, struct vt_line_s *l)
{
  struct vt_line_s *old;
  int k = l->n;

  if (vt->sbn == vt->sbcap && vt->sbcap < vt->sbmax)
    {
      int cap = vt->sbcap < 32 ? 32 : vt->sbcap * 2;
      struct vt_line_s **sb;
      int i;

      cap = cap > vt->sbmax ? vt->sbmax : cap;
      sb  = malloc(cap * sizeof(*sb));
      if (sb != NULL)
        {
          for (i = 0; i < vt->sbn; i++)
            {
              sb[i] = sb_get(vt, i);
            }

          free(vt->sb);
          vt->sb     = sb;
          vt->sbcap  = cap;
          vt->sbhead = 0;
        }
    }

  if (vt->sbcap == 0)
    {
      return l;
    }

  while (k > 0 && cell_blank(&l->c[k - 1]))
    {
      k--;
    }

  if (k + 16 <= l->n)
    {
      struct vt_line_s *nl = realloc(l, LINE_SIZE(k));

      if (nl != NULL)
        {
          if (nl != l)
            {
              vt->epoch++;                     /* Moved: drawn anew */
              nl->d0 = 0;
              nl->d1 = DIRTY_ALL;
            }

          l    = nl;
          l->n = k;
        }
    }

  if (vt->sbn < vt->sbcap)
    {
      vt->sb[(vt->sbhead + vt->sbn) % vt->sbcap] = l;
      vt->sbn++;
      return NULL;
    }

  old = vt->sb[vt->sbhead];
  vt->sb[vt->sbhead] = l;
  vt->sbhead = (vt->sbhead + 1) % vt->sbcap;
  return old;
}

/* The newest line out of the scrollback (the screen grows) */

static struct vt_line_s *sb_pop(struct vt_s *vt)
{
  if (vt->sbn == 0)
    {
      return NULL;
    }

  vt->sbn--;
  return sb_get(vt, vt->sbn);
}

static void sb_clear(struct vt_s *vt)
{
  int i;

  for (i = 0; i < vt->sbn; i++)
    {
      free(sb_get(vt, i));
    }

  free(vt->sb);
  vt->sb     = NULL;
  vt->sbcap  = 0;
  vt->sbn    = 0;
  vt->sbhead = 0;
  vt->epoch++;
}

/****************************************************************************
 * Private Functions: scrolling
 ****************************************************************************/

/* Rows top to bot up by n: the top lines go (into the scrollback if
 * tosb), blank lines come in at the bottom
 */

static void scroll_up(struct vt_s *vt, int top, int bot, int n, bool tosb)
{
  if (n > bot - top + 1)
    {
      n = bot - top + 1;
    }

  tosb = tosb && top == 0 && vt->scr == vt->main;
  while (n-- > 0)
    {
      struct vt_line_s *l = vt->scr[top];
      struct vt_line_s *nl = l;

      if (tosb)
        {
          /* A new line while the scrollback grows, else its oldest */

          struct vt_line_s *fresh = NULL;

          if (vt->sbn < vt->sbmax)
            {
              fresh = line_alloc(vt->cols);
            }

          if (fresh != NULL || vt->sbn >= vt->sbmax)
            {
              struct vt_line_s *back = sb_push(vt, l);

              if (back == l)
                {
                  free(fresh);                 /* Not kept */
                }
              else
                {
                  if (back != NULL)
                    {
                      free(fresh);
                      nl = back;
                    }
                  else
                    {
                      nl = fresh;
                    }
                }
            }
        }

      memmove(&vt->scr[top], &vt->scr[top + 1],
              (bot - top) * sizeof(vt->scr[0]));

      if (nl->n < vt->cols)
        {
          struct vt_line_s *f = line_fit(vt, nl);

          nl = f != NULL ? f : nl;
        }

      vt->scr[bot] = nl;
      nl->wrapped  = 0;
      erase(vt, bot, 0, vt->cols);
      nl->d0 = 0;
      nl->d1 = DIRTY_ALL;
    }
}

static void scroll_down(struct vt_s *vt, int top, int bot, int n)
{
  if (n > bot - top + 1)
    {
      n = bot - top + 1;
    }

  while (n-- > 0)
    {
      struct vt_line_s *l = vt->scr[bot];

      memmove(&vt->scr[top + 1], &vt->scr[top],
              (bot - top) * sizeof(vt->scr[0]));
      vt->scr[top] = l;
      l->wrapped   = 0;
      erase(vt, top, 0, vt->cols);
      l->d0 = 0;
      l->d1 = DIRTY_ALL;
    }
}

static void index_down(struct vt_s *vt)
{
  if (vt->y == vt->bot)
    {
      scroll_up(vt, vt->top, vt->bot, 1, true);
    }
  else if (vt->y < vt->rows - 1)
    {
      vt->y++;
    }
}

static void index_up(struct vt_s *vt)
{
  if (vt->y == vt->top)
    {
      scroll_down(vt, vt->top, vt->bot, 1);
    }
  else if (vt->y > 0)
    {
      vt->y--;
    }
}

/****************************************************************************
 * Private Functions: the cursor, characters
 ****************************************************************************/

static int clamp(int v, int lo, int hi)
{
  return v < lo ? lo : v > hi ? hi : v;
}

static void cursor_to(struct vt_s *vt, int x, int y)
{
  vt->x = clamp(x, 0, vt->cols - 1);
  vt->y = clamp(y, 0, vt->rows - 1);
  vt->wrapnext = false;
}

/* Absolute positioning: relative to the scroll region in origin mode */

static void cursor_abs(struct vt_s *vt, int x, int y)
{
  if (vt->origin)
    {
      cursor_to(vt, x, clamp(y + vt->top, vt->top, vt->bot));
    }
  else
    {
      cursor_to(vt, x, y);
    }
}

static void put(struct vt_s *vt, uint8_t ch)
{
  struct vt_line_s *l;
  struct vt_cell_s *c;

  if (vt->wrapnext)
    {
      if (vt->autowrap)
        {
          vt->scr[vt->y]->wrapped = 1;
          vt->x = 0;
          index_down(vt);
        }

      vt->wrapnext = false;
    }

  l = vt->scr[vt->y];
  if (vt->x >= l->n)
    {
      return;                                  /* No memory to widen it */
    }

  if (vt->insert && vt->x < vt->cols - 1)
    {
      memmove(&l->c[vt->x + 1], &l->c[vt->x],
              (vt->cols - 1 - vt->x) * sizeof(l->c[0]));
      dirty(l, vt->x, vt->cols);
    }

  c     = &l->c[vt->x];
  *c    = vt->pen;
  c->ch = ch;
  dirty(l, vt->x, vt->x + 1);
  vt->last = ch;

  if (vt->x == vt->cols - 1)
    {
      vt->wrapnext = true;
    }
  else
    {
      vt->x++;
    }
}

static void put_ascii(struct vt_s *vt, uint8_t ch)
{
  if (vt->g[vt->gl] == 1 && ch >= 0x60 && ch <= 0x7e)
    {
      ch = g_decgfx[ch - 0x60];
    }

  put(vt, ch);
}

static void put_ucs(struct vt_s *vt, uint32_t u)
{
  uint8_t ch = '?';

  if (u < 0x80)
    {
      ch = (uint8_t)u;
    }
  else if (u >= 0x2500 && u < 0x2580)
    {
      ch = g_box[u - 0x2500];
    }
  else
    {
      switch (u)
        {
          case 0xa0:
            ch = ' ';
            break;

          case 0xb0:
            ch = 0x82;                         /* DESK_CH_DEGREE */
            break;

          case 0xb7:
          case 0x22c5:
            ch = 0x85;                         /* DESK_CH_MIDDOT */
            break;

          case 0xd7:
            ch = 0x80;                         /* DESK_CH_TIMES */
            break;

          case 0x2022:
          case 0x25cf:
            ch = 0x7f;                         /* DESK_CH_BULLET */
            break;

          case 0x2026:
            ch = 0x83;                         /* DESK_CH_ELLIPSIS */
            break;

          case 0x2212:
            ch = 0x81;                         /* DESK_CH_MINUS */
            break;

          case 0x25b6:
          case 0x25b8:
          case 0x25ba:
            ch = 0x84;                         /* DESK_CH_TRIANGLE */
            break;

          case 0x2010:
          case 0x2013:
          case 0x2014:
            ch = '-';
            break;

          case 0x2018:
          case 0x2019:
            ch = '\'';
            break;

          case 0x201c:
          case 0x201d:
            ch = '"';
            break;

          case 0x2580:
            ch = VT_CH_UPPER;
            break;

          case 0x2584:
            ch = VT_CH_LOWER;
            break;

          case 0x2588:
            ch = VT_CH_BLOCK;
            break;

          case 0x2591:
          case 0x2592:
          case 0x2593:
            ch = VT_CH_SHADE;
            break;

          default:
            break;
        }
    }

  put(vt, ch);
}

/****************************************************************************
 * Private Functions: sequences
 ****************************************************************************/

static void respond(struct vt_s *vt, const char *s)
{
  int n = strlen(s);

  if (vt->nresp + n <= (int)sizeof(vt->resp))
    {
      memcpy(vt->resp + vt->nresp, s, n);
      vt->nresp += n;
    }
}

static void save(struct vt_s *vt)
{
  vt->saved.x      = vt->x;
  vt->saved.y      = vt->y;
  vt->saved.pen    = vt->pen;
  vt->saved.origin = vt->origin;
  vt->saved.g[0]   = vt->g[0];
  vt->saved.g[1]   = vt->g[1];
  vt->saved.gl     = vt->gl;
}

static void restore(struct vt_s *vt)
{
  vt->pen    = vt->saved.pen;
  vt->origin = vt->saved.origin;
  vt->g[0]   = vt->saved.g[0];
  vt->g[1]   = vt->saved.g[1];
  vt->gl     = vt->saved.gl;
  cursor_to(vt, vt->saved.x, vt->saved.y);
}

static void pen_reset(struct vt_s *vt)
{
  vt->pen = g_blank;
}

static void reset(struct vt_s *vt)
{
  int y;

  pen_reset(vt);
  vt->top      = 0;
  vt->bot      = vt->rows - 1;
  vt->origin   = false;
  vt->autowrap = true;
  vt->insert   = false;
  vt->cursor   = true;
  vt->appcursor = false;
  vt->g[0]     = 0;
  vt->g[1]     = 0;
  vt->gl       = 0;
  cursor_to(vt, 0, 0);
  save(vt);
  for (y = 0; y < vt->rows; y++)
    {
      erase(vt, y, 0, vt->cols);
    }
}

/* The alternate screen: on (a new blank screen) or off (freed) */

static void alt_screen(struct vt_s *vt, bool on)
{
  int y;

  if (on && vt->alt == NULL)
    {
      struct vt_line_s **alt = calloc(vt->rows, sizeof(*alt));

      if (alt == NULL)
        {
          return;
        }

      for (y = 0; y < vt->rows; y++)
        {
          alt[y] = line_alloc(vt->cols);
          if (alt[y] == NULL)
            {
              while (y-- > 0)
                {
                  free(alt[y]);
                }

              free(alt);
              return;
            }
        }

      vt->alt = alt;
      vt->scr = alt;
    }
  else if (!on && vt->alt != NULL)
    {
      for (y = 0; y < vt->rows; y++)
        {
          free(vt->alt[y]);
        }

      free(vt->alt);
      vt->alt = NULL;
      vt->scr = vt->main;
      vt->epoch++;
      for (y = 0; y < vt->rows; y++)
        {
          vt->main[y]->d0 = 0;
          vt->main[y]->d1 = DIRTY_ALL;
        }
    }
}

static void mode_priv(struct vt_s *vt, int m, bool on)
{
  switch (m)
    {
      case 1:
        vt->appcursor = on;
        break;

      case 6:
        vt->origin = on;
        cursor_abs(vt, 0, 0);
        break;

      case 7:
        vt->autowrap = on;
        break;

      case 25:
        vt->cursor = on;
        break;

      case 47:
      case 1047:
        alt_screen(vt, on);
        break;

      case 1048:
        on ? save(vt) : restore(vt);
        break;

      case 1049:
        if (on)
          {
            save(vt);
            alt_screen(vt, true);
          }
        else
          {
            alt_screen(vt, false);
            restore(vt);
          }
        break;

      default:
        break;
    }
}

/* 24-bit colour: the nearest of the 256 */

static uint8_t rgb256(int r, int g, int b)
{
  int ri = r < 48 ? 0 : r < 115 ? 1 : (r - 35) / 40;
  int gi = g < 48 ? 0 : g < 115 ? 1 : (g - 35) / 40;
  int bi = b < 48 ? 0 : b < 115 ? 1 : (b - 35) / 40;

  if (ri == gi && gi == bi && r > 4 && r < 244 &&
      abs(r - g) < 12 && abs(g - b) < 12)
    {
      return (uint8_t)(232 + clamp((r - 3) / 10, 0, 23));
    }

  return (uint8_t)(16 + 36 * ri + 6 * gi + bi);
}

static void sgr(struct vt_s *vt)
{
  int i;

  for (i = 0; i <= vt->npar; i++)
    {
      int v = vt->par[i];

      switch (v)
        {
          case 0:
            pen_reset(vt);
            break;

          case 1:
            vt->pen.at |= VT_BOLD;
            break;

          case 2:
            vt->pen.at |= VT_DIM;
            break;

          case 4:
          case 21:
            vt->pen.at |= VT_UNDER;
            break;

          case 7:
            vt->pen.at |= VT_REVERSE;
            break;

          case 8:
            vt->pen.at |= VT_HIDDEN;
            break;

          case 22:
            vt->pen.at &= ~(VT_BOLD | VT_DIM);
            break;

          case 24:
            vt->pen.at &= ~VT_UNDER;
            break;

          case 27:
            vt->pen.at &= ~VT_REVERSE;
            break;

          case 28:
            vt->pen.at &= ~VT_HIDDEN;
            break;

          case 39:
            vt->pen.at |= VT_FGDEF;
            vt->pen.fg = 7;
            break;

          case 49:
            vt->pen.at |= VT_BGDEF;
            vt->pen.bg = 0;
            break;

          case 38:
          case 48:
            {
              int c = -1;

              if (i + 2 <= vt->npar && vt->par[i + 1] == 5)
                {
                  c = vt->par[i + 2] & 0xff;
                  i += 2;
                }
              else if (i + 4 <= vt->npar && vt->par[i + 1] == 2)
                {
                  c = rgb256(vt->par[i + 2], vt->par[i + 3],
                             vt->par[i + 4]);
                  i += 4;
                }
              else
                {
                  i = vt->npar;
                }

              if (c >= 0 && v == 38)
                {
                  vt->pen.fg  = (uint8_t)c;
                  vt->pen.at &= ~VT_FGDEF;
                }
              else if (c >= 0)
                {
                  vt->pen.bg  = (uint8_t)c;
                  vt->pen.at &= ~VT_BGDEF;
                }
            }
            break;

          default:
            if (v >= 30 && v <= 37)
              {
                vt->pen.fg  = (uint8_t)(v - 30);
                vt->pen.at &= ~VT_FGDEF;
              }
            else if (v >= 40 && v <= 47)
              {
                vt->pen.bg  = (uint8_t)(v - 40);
                vt->pen.at &= ~VT_BGDEF;
              }
            else if (v >= 90 && v <= 97)
              {
                vt->pen.fg  = (uint8_t)(v - 90 + 8);
                vt->pen.at &= ~VT_FGDEF;
              }
            else if (v >= 100 && v <= 107)
              {
                vt->pen.bg  = (uint8_t)(v - 100 + 8);
                vt->pen.at &= ~VT_BGDEF;
              }
            break;
        }
    }
}

static void csi(struct vt_s *vt, uint8_t f)
{
  int p0 = vt->par[0];
  int n = p0 > 0 ? p0 : 1;
  struct vt_line_s *l = vt->scr[vt->y];
  char buf[24];
  int i;

  if (vt->priv == '?')
    {
      if (f == 'h' || f == 'l')
        {
          for (i = 0; i <= vt->npar; i++)
            {
              mode_priv(vt, vt->par[i], f == 'h');
            }
        }

      return;
    }

  if (vt->priv != 0 || vt->inter != 0)
    {
      if (vt->priv == '>' && f == 'c')
        {
          respond(vt, "\033[>0;10;1c");
        }

      return;
    }

  switch (f)
    {
      case 'A':
        cursor_to(vt, vt->x, vt->y - n < vt->top && vt->y >= vt->top ?
                             vt->top : vt->y - n);
        break;

      case 'B':
      case 'e':
        cursor_to(vt, vt->x, vt->y + n > vt->bot && vt->y <= vt->bot ?
                             vt->bot : vt->y + n);
        break;

      case 'C':
      case 'a':
        cursor_to(vt, vt->x + n, vt->y);
        break;

      case 'D':
        cursor_to(vt, vt->x - n, vt->y);
        break;

      case 'E':
        cursor_to(vt, 0, vt->y + n);
        break;

      case 'F':
        cursor_to(vt, 0, vt->y - n);
        break;

      case 'G':
      case '`':
        cursor_to(vt, n - 1, vt->y);
        break;

      case 'H':
      case 'f':
        cursor_abs(vt, (vt->par[1] > 0 ? vt->par[1] : 1) - 1, n - 1);
        break;

      case 'd':
        cursor_abs(vt, vt->x, n - 1);
        break;

      case 'I':
        while (n-- > 0 && vt->x < vt->cols - 1)
          {
            cursor_to(vt, (vt->x / 8 + 1) * 8, vt->y);
          }
        break;

      case 'Z':
        while (n-- > 0 && vt->x > 0)
          {
            cursor_to(vt, (vt->x - 1) / 8 * 8, vt->y);
          }
        break;

      case 'J':
        if (p0 == 3)
          {
            sb_clear(vt);
            break;
          }

        for (i = 0; i < vt->rows; i++)
          {
            if ((p0 == 0 && i > vt->y) || (p0 == 1 && i < vt->y) || p0 == 2)
              {
                erase(vt, i, 0, vt->cols);
              }
          }

        /* Fall through: the cursor's line */

        if (p0 == 2)
          {
            break;
          }

      case 'K':
        if (p0 == 0)
          {
            erase(vt, vt->y, vt->x, vt->cols);
          }
        else if (p0 == 1)
          {
            erase(vt, vt->y, 0, vt->x + 1);
          }
        else
          {
            erase(vt, vt->y, 0, vt->cols);
          }
        break;

      case 'L':
        if (vt->y >= vt->top && vt->y <= vt->bot)
          {
            scroll_down(vt, vt->y, vt->bot, n);
            cursor_to(vt, 0, vt->y);
          }
        break;

      case 'M':
        if (vt->y >= vt->top && vt->y <= vt->bot)
          {
            scroll_up(vt, vt->y, vt->bot, n, false);
            cursor_to(vt, 0, vt->y);
          }
        break;

      case 'P':
        if (l->n < vt->cols)
          {
            break;
          }

        n = n > vt->cols - vt->x ? vt->cols - vt->x : n;
        memmove(&l->c[vt->x], &l->c[vt->x + n],
                (vt->cols - vt->x - n) * sizeof(l->c[0]));
        erase(vt, vt->y, vt->cols - n, vt->cols);
        dirty(l, vt->x, vt->cols);
        vt->wrapnext = false;
        break;

      case '@':
        if (l->n < vt->cols)
          {
            break;
          }

        n = n > vt->cols - vt->x ? vt->cols - vt->x : n;
        memmove(&l->c[vt->x + n], &l->c[vt->x],
                (vt->cols - vt->x - n) * sizeof(l->c[0]));
        erase(vt, vt->y, vt->x, vt->x + n);
        dirty(l, vt->x, vt->cols);
        vt->wrapnext = false;
        break;

      case 'X':
        erase(vt, vt->y, vt->x, vt->x + n);
        vt->wrapnext = false;
        break;

      case 'S':
        scroll_up(vt, vt->top, vt->bot, n, true);
        break;

      case 'T':
        if (vt->npar == 0)
          {
            scroll_down(vt, vt->top, vt->bot, n);
          }
        break;

      case 'b':
        while (n-- > 0 && vt->last != 0)
          {
            put(vt, vt->last);
          }
        break;

      case 'm':
        sgr(vt);
        break;

      case 'r':
        {
          int t = n - 1;
          int b = vt->par[1] > 0 ? vt->par[1] - 1 : vt->rows - 1;

          if (vt->npar == 0 && p0 == 0)
            {
              b = vt->rows - 1;
            }

          if (t < b && b < vt->rows)
            {
              vt->top = t;
              vt->bot = b;
              cursor_abs(vt, 0, 0);
            }
        }
        break;

      case 's':
        save(vt);
        break;

      case 'u':
        restore(vt);
        break;

      case 'n':
        if (p0 == 5)
          {
            respond(vt, "\033[0n");
          }
        else if (p0 == 6)
          {
            snprintf(buf, sizeof(buf), "\033[%d;%dR",
                     vt->y + 1 - (vt->origin ? vt->top : 0), vt->x + 1);
            respond(vt, buf);
          }
        break;

      case 'c':
        if (p0 == 0)
          {
            respond(vt, "\033[?1;2c");
          }
        break;

      case 'h':
      case 'l':
        for (i = 0; i <= vt->npar; i++)
          {
            if (vt->par[i] == 4)
              {
                vt->insert = f == 'h';
              }
          }
        break;

      default:
        break;
    }
}

static void esc(struct vt_s *vt, uint8_t c)
{
  vt->state = ST_GROUND;
  switch (c)
    {
      case '[':
        vt->state = ST_CSI;
        vt->npar  = 0;
        vt->priv  = 0;
        vt->inter = 0;
        memset(vt->par, 0, sizeof(vt->par));
        break;

      case ']':
        vt->state = ST_OSC;
        vt->nosc  = 0;
        break;

      case 'P':
      case 'X':
      case '^':
      case '_':
        vt->state = ST_STR;
        break;

      case '(':
      case ')':
        vt->state = ST_CHARSET;
        vt->inter = c;
        break;

      case '#':
      case '%':
      case ' ':
        vt->state = ST_SKIP1;
        break;

      case '7':
        save(vt);
        break;

      case '8':
        restore(vt);
        break;

      case 'D':
        index_down(vt);
        break;

      case 'E':
        vt->x = 0;
        vt->wrapnext = false;
        index_down(vt);
        break;

      case 'M':
        index_up(vt);
        break;

      case 'c':
        alt_screen(vt, false);
        reset(vt);
        break;

      default:
        break;
    }
}

static void osc_done(struct vt_s *vt)
{
  vt->osc[vt->nosc] = '\0';
  if ((vt->osc[0] == '0' || vt->osc[0] == '2') && vt->osc[1] == ';')
    {
      snprintf(vt->title, sizeof(vt->title), "%s", vt->osc + 2);
      vt->titled = true;
    }

  vt->state = ST_GROUND;
}

static void control(struct vt_s *vt, uint8_t c)
{
  switch (c)
    {
      case '\b':
        if (vt->x > 0)
          {
            vt->x--;
          }

        vt->wrapnext = false;
        break;

      case '\t':
        cursor_to(vt, (vt->x / 8 + 1) * 8, vt->y);
        break;

      case '\n':
      case '\v':
      case '\f':
        index_down(vt);
        break;

      case '\r':
        vt->x = 0;
        vt->wrapnext = false;
        break;

      case 0x0e:
        vt->gl = 1;
        break;

      case 0x0f:
        vt->gl = 0;
        break;

      case 0x18:
      case 0x1a:
        vt->state = ST_GROUND;
        break;

      case 0x1b:
        vt->state = ST_ESC;
        break;

      default:
        break;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int vt_init(struct vt_s *vt, int cols, int rows, int sbmax)
{
  int y;

  memset(vt, 0, sizeof(*vt));
  vt->cols  = cols < 1 ? 1 : cols;
  vt->rows  = rows < 1 ? 1 : rows;
  vt->sbmax = sbmax;
  vt->main  = calloc(vt->rows, sizeof(vt->main[0]));
  if (vt->main == NULL)
    {
      return -1;
    }

  for (y = 0; y < vt->rows; y++)
    {
      vt->main[y] = line_alloc(vt->cols);
      if (vt->main[y] == NULL)
        {
          vt_free(vt);
          return -1;
        }
    }

  vt->scr = vt->main;
  reset(vt);
  return 0;
}

void vt_free(struct vt_s *vt)
{
  int y;

  alt_screen(vt, false);
  if (vt->main != NULL)
    {
      for (y = 0; y < vt->rows; y++)
        {
          free(vt->main[y]);
        }

      free(vt->main);
      vt->main = NULL;
    }

  sb_clear(vt);
}

void vt_write(struct vt_s *vt, const uint8_t *buf, int n)
{
  int i;

  for (i = 0; i < n; i++)
    {
      uint8_t c = buf[i];

      switch (vt->state)
        {
          case ST_GROUND:
            if (vt->ulen > 0)
              {
                if ((c & 0xc0) == 0x80)
                  {
                    vt->ucs = vt->ucs << 6 | (c & 0x3f);
                    if (--vt->ulen == 0)
                      {
                        put_ucs(vt, vt->ucs);
                      }

                    break;
                  }

                vt->ulen = 0;
                put(vt, '?');
              }

            if (c < 0x20)
              {
                control(vt, c);
              }
            else if (c < 0x7f)
              {
                put_ascii(vt, c);
              }
            else if ((c & 0xe0) == 0xc0)
              {
                vt->ucs  = c & 0x1f;
                vt->ulen = 1;
              }
            else if ((c & 0xf0) == 0xe0)
              {
                vt->ucs  = c & 0x0f;
                vt->ulen = 2;
              }
            else if ((c & 0xf8) == 0xf0)
              {
                vt->ucs  = c & 0x07;
                vt->ulen = 3;
              }
            else if (c != 0x7f)
              {
                put(vt, '?');
              }
            break;

          case ST_ESC:
            if (c < 0x20)
              {
                control(vt, c);
              }
            else
              {
                esc(vt, c);
              }
            break;

          case ST_CSI:
            if (c >= '0' && c <= '9')
              {
                int *p = &vt->par[vt->npar];

                *p = *p < 10000 ? *p * 10 + c - '0' : *p;
              }
            else if (c == ';' || c == ':')
              {
                if (vt->npar < 15)
                  {
                    vt->npar++;
                  }
              }
            else if (c >= '<' && c <= '?')
              {
                vt->priv = c;
              }
            else if (c >= 0x20 && c <= 0x2f)
              {
                vt->inter = c;
              }
            else if (c >= 0x40 && c <= 0x7e)
              {
                vt->state = ST_GROUND;
                csi(vt, c);
              }
            else if (c < 0x20)
              {
                control(vt, c);
              }
            break;

          case ST_OSC:
            if (c == 0x07)
              {
                osc_done(vt);
              }
            else if (c == 0x1b)
              {
                vt->state = ST_OSCESC;
              }
            else if (c == 0x18 || c == 0x1a)
              {
                vt->state = ST_GROUND;
              }
            else if (vt->nosc < sizeof(vt->osc) - 1)
              {
                vt->osc[vt->nosc++] = c;
              }
            break;

          case ST_OSCESC:
            osc_done(vt);
            if (c != '\\')
              {
                esc(vt, c);
              }
            break;

          case ST_CHARSET:
            vt->g[vt->inter == ')'] = c == '0';
            vt->state = ST_GROUND;
            break;

          case ST_SKIP1:
            vt->state = ST_GROUND;
            break;

          case ST_STR:
            if (c == 0x1b)
              {
                vt->state = ST_STRESC;
              }
            else if (c == 0x07 || c == 0x18 || c == 0x1a)
              {
                vt->state = ST_GROUND;
              }
            break;

          case ST_STRESC:
            vt->state = c == '\\' ? ST_GROUND : ST_STR;
            break;

          default:
            vt->state = ST_GROUND;
            break;
        }
    }
}

/* A screen of new height: fewer rows push the top lines into the
 * scrollback (after dropping blank ones below the cursor), more pull them
 * back from it (then add blank ones at the bottom)
 */

static struct vt_line_s **resize_screen(struct vt_s *vt,
                                        struct vt_line_s **old, int rows,
                                        bool main, int *cy)
{
  struct vt_line_s **scr;
  int orows = vt->rows;
  int last = orows - 1;
  int k;
  int y;

  scr = calloc(rows, sizeof(*scr));
  if (scr == NULL)
    {
      return NULL;
    }

  if (rows < orows)
    {
      k = orows - rows;
      while (k > 0 && last > *cy)
        {
          int x;

          for (x = 0; x < old[last]->n && cell_blank(&old[last]->c[x]);
               x++)
            {
            }

          if (x < old[last]->n)
            {
              break;
            }

          free(old[last--]);
          k--;
        }

      for (y = 0; y < k; y++)
        {
          struct vt_line_s *back = main ? sb_push(vt, old[y]) : old[y];

          free(back);
        }

      memcpy(scr, &old[k], rows * sizeof(*scr));
      *cy -= k;
    }
  else
    {
      int pulled;

      k      = rows - orows;
      pulled = main ? (k < vt->sbn ? k : vt->sbn) : 0;

      /* The new blank lines first: nothing changes without them */

      for (y = pulled + orows; y < rows; y++)
        {
          scr[y] = line_alloc(vt->cols);
          if (scr[y] == NULL)
            {
              while (--y >= pulled + orows)
                {
                  free(scr[y]);
                }

              free(scr);
              return NULL;
            }
        }

      for (y = pulled - 1; y >= 0; y--)
        {
          scr[y] = sb_pop(vt);
        }

      memcpy(&scr[pulled], old, orows * sizeof(*scr));
      *cy += pulled;
    }

  free(old);
  return scr;
}

int vt_resize(struct vt_s *vt, int cols, int rows)
{
  int y;

  cols = cols < 1 ? 1 : cols > 1000 ? 1000 : cols;
  rows = rows < 1 ? 1 : rows > 1000 ? 1000 : rows;
  if (cols == vt->cols && rows == vt->rows)
    {
      return 0;
    }

  vt->cols = cols;
  if (rows != vt->rows)
    {
      struct vt_line_s **scr;
      int cy = vt->y;
      int sy = vt->saved.y;

      if (vt->alt != NULL)
        {
          int acy = vt->y;

          scr = resize_screen(vt, vt->alt, rows, false, &acy);
          if (scr == NULL)
            {
              return -1;
            }

          vt->alt = scr;
          cy = vt->saved.y;
          sy = acy;
        }

      scr = resize_screen(vt, vt->main, rows, true, &cy);
      if (scr == NULL)
        {
          return -1;
        }

      vt->main = scr;
      vt->rows = rows;
      vt->scr  = vt->alt != NULL ? vt->alt : vt->main;

      /* In the alternate screen, the saved cursor is the main's */

      if (vt->alt != NULL)
        {
          vt->saved.y = cy;
          vt->y       = sy;
        }
      else
        {
          vt->y = cy;
        }
    }

  for (y = 0; y < vt->rows; y++)
    {
      struct vt_line_s *l;

      l = line_fit(vt, vt->main[y]);
      vt->main[y] = l != NULL ? l : vt->main[y];
      if (vt->alt != NULL)
        {
          l = line_fit(vt, vt->alt[y]);
          vt->alt[y] = l != NULL ? l : vt->alt[y];
        }
    }

  vt->scr = vt->alt != NULL ? vt->alt : vt->main;
  for (y = 0; y < vt->rows; y++)
    {
      if (vt->scr[y]->n < vt->cols)
        {
          return -1;                           /* No memory */
        }

      vt->scr[y]->d0 = 0;
      vt->scr[y]->d1 = DIRTY_ALL;
    }

  vt->epoch++;
  vt->top       = 0;
  vt->bot       = vt->rows - 1;
  vt->saved.x   = clamp(vt->saved.x, 0, vt->cols - 1);
  vt->saved.y   = clamp(vt->saved.y, 0, vt->rows - 1);
  cursor_to(vt, vt->x, vt->y);
  return 0;
}

struct vt_line_s *vt_line(struct vt_s *vt, int row, int view)
{
  int i;

  if (vt->scr != vt->main || view < 0)
    {
      view = 0;
    }

  if (view > vt->sbn)
    {
      view = vt->sbn;
    }

  i = row - view;
  return i >= 0 ? vt->scr[i] : sb_get(vt, vt->sbn + i);
}
