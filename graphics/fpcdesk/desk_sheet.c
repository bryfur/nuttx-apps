/****************************************************************************
 * apps/graphics/fpcdesk/desk_sheet.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Spreadsheet: a grid of 65536 rows and 702 columns (A to ZZ) in the
 * spirit of VisiCalc and Lotus 1-2-3, on the engine of desk_sheet.h (cells
 * kept sparsely, formulas, recalculation by dependencies).
 *
 * The window: a tool bar (New, Open, Save, Undo, Redo, bold, alignment,
 * the number format and its decimals, text and fill colours, a chart,
 * a menu of the rest), a formula bar (the cell's name, what it holds as
 * typed), the grid with its frozen column and row headers and its scroll
 * bars, a status bar (a message, or the sum, average and count of the
 * selection; the mode: READY, ENTER, EDIT, POINT).  All of it but the grid
 * is the widget kit's; the grid is a custom widget of it, drawn here.
 *
 * Drawing: nothing is drawn twice.  Scrolling moves what stays with one
 * COPY within the window and draws only the rows or columns that come
 * into view; a change draws the rows whose cells' values changed; a new
 * selection draws the cells whose look changed (inside it, its edges, the
 * active cell) and their headers.  Text runs on into empty cells at its
 * right, so a row is drawn with whatever text comes into it from the
 * left.  The chart is drawn once into a surface in the GPU's memory and
 * shown with one COPY.
 *
 * Keys: arrows (Shift: a range; Ctrl: to the edge of the data), Page
 * Up/Down, Home, Ctrl-Home, Ctrl-End, Enter / Tab / Shift-Tab (and, while
 * typing, the arrows) put what is typed in and move; F2 edits the cell
 * (the arrows then move in the text); Escape cancels; typing a formula,
 * the arrows or a click point at a cell (POINT) and put its name in.
 * Delete clears; Ctrl-C / X / V copy, cut (a move) and paste (a formula's
 * relative references follow it; the desktop's clipboard gets the values
 * as tab-separated text, and such text pastes in); Ctrl-D / Ctrl-R fill
 * down and right; Ctrl-Z / Y undo and redo; Ctrl-B bold; Ctrl-F find;
 * Ctrl-A all; Ctrl-S save, Ctrl-O open, Ctrl-N a new window.
 *
 * A program of its own (/mnt/sd0/bin/sheet, apps/graphics/deskapps):
 * built into the desktop (DESK_PROGRAM_STUB) this file is only the icon
 * and the name the dock shows (desk_program.c).
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "desk_kit.h"
#include "desk_num.h"
#include "desk_sheet.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SHEET_W        760
#define SHEET_H        520

#define NWG            48                /* The kit's widgets */
#define RH             22                /* A row */
#define HDR_H          22                /* The column headers */
#define RHW            46                /* The row headers */
#define PAD            5                 /* Text in a cell */
#define VISMAX         160               /* Columns shown, at most */
#define UNDO_MAX       64
#define SNAP_MAX       200000            /* Cells kept for undo, in all */
#define DBLCLICK_MS    400
#define EDGE           4                 /* A column's edge: resize there */
#define CHART_W        340
#define CHART_H        210
#define CHART_TITLE    24
#define CHART_SERIES   4

/* Colours */

#define C_GRID_BG      THEMED(RGB(0x1c, 0x1c, 0x20), RGB(0xff, 0xff, 0xff))
#define C_GRID_LINE    THEMED(RGB(0x34, 0x34, 0x3b), RGB(0xe2, 0xe2, 0xe7))
#define C_HEAD_BG      THEMED(RGB(0x27, 0x27, 0x2c), RGB(0xf4, 0xf4, 0xf6))
#define C_HEAD_LINE    THEMED(RGB(0x3c, 0x3c, 0x44), RGB(0xd6, 0xd6, 0xdc))
#define C_EDIT_BG      THEMED(RGB(0x24, 0x24, 0x2a), RGB(0xff, 0xff, 0xff))
#define C_CARET        THEMED(RGB(0xf2, 0xf2, 0xf4), RGB(0x1d, 0x1d, 0x1f))
#define A_TINT         THEMED(52, 36)    /* The selection over cells */

/* The widgets */

#define S_NEW          1
#define S_OPEN         2
#define S_SAVE         3
#define S_UNDO         4
#define S_REDO         5
#define S_BOLD         6
#define S_ALEFT        7
#define S_ACENTRE      8
#define S_ARIGHT       9
#define S_NUMFMT       10
#define S_DECLESS      11
#define S_DECMORE      12
#define S_COLOUR       13
#define S_FILL         14
#define S_CHART        15
#define S_MENU         16
#define S_NAME         17
#define S_FX           18
#define S_FIELD        19
#define S_GRID         20
#define S_HSCROLL      21
#define S_VSCROLL      22
#define S_STATUS       23
#define S_MODE         24

/* Dialogs and menus (their ids) */

#define P_CLOSE        1                 /* Unsaved changes: save first? */
#define P_SAVEAS       2
#define P_OPEN         3
#define P_FIND         4
#define M_CONTEXT      10
#define M_COLOUR       11
#define M_FILL         12
#define M_MORE         13

/* The menus' items */

#define I_CUT          0
#define I_COPY         1
#define I_PASTE        2
#define I_CLEAR        4
#define I_INSROW       6
#define I_DELROW       7
#define I_INSCOL       8
#define I_DELCOL       9
#define I_FILLDOWN     11
#define I_FILLRIGHT    12
#define I_SORTUP       14
#define I_SORTDOWN     15

#define J_SAVEAS       0                 /* The More menu */
#define J_FIND         1
#define J_INSROW       3
#define J_DELROW       4
#define J_INSCOL       5
#define J_DELCOL       6
#define J_SORTUP       8
#define J_SORTDOWN     9
#define J_FILLDOWN     11
#define J_FILLRIGHT    12

/* Modes */

#define MODE_READY     0
#define MODE_ENTER     1                 /* Typing: the arrows put it in */
#define MODE_EDIT      2                 /* F2: the arrows move in it */

/* Drags */

#define D_NONE         0
#define D_CELLS        1
#define D_COLS         2                 /* Whole columns */
#define D_ROWS         3
#define D_WIDTH        4                 /* A column's edge */
#define D_POINT        5                 /* A reference into a formula */

/* Undo records: what undoing does */

#define U_CELLS        0                 /* Put cells back */
#define U_INSERT       1                 /* Insert, then put cells back */
#define U_DELETE       2                 /* Delete */

/* How a cell looks: in the selection, its edges, the active one */

#define L_IN           0x01
#define L_TOP          0x02
#define L_BOTTOM       0x04
#define L_LEFT         0x08
#define L_RIGHT        0x10
#define L_ACTIVE       0x20
#define L_PTOP         0x40              /* The edges of what is pointed */
#define L_PBOTTOM      0x80              /* at (POINT) */
#define L_PLEFT        0x100
#define L_PRIGHT       0x200

#ifndef DESK_PROGRAM_STUB

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct sel_s
{
  int cr;                                /* The active cell */
  int cc;
  int er;                                /* The range's other corner */
  int ec;
  int pr;                                /* Pointing at, -1 none */
  int pc;
  int pr2;                               /* ... to (a range) */
  int pc2;
};

struct undo_s
{
  uint8_t kind;                          /* U_* */
  bool rows;
  int at;
  int n;
  struct sh_rect_s rect[2];              /* U_CELLS: cleared first */
  int nrect;
  struct sh_snap_s *snap;
  int nsnap;
  int capsnap;
  struct sel_s sel;                      /* The selection then */
};

/* Where the grid is (frame coordinates) */

struct geo_s
{
  int x0;                                /* The widget */
  int y0;
  int x1;
  int y1;
  int cx0;                               /* The cells */
  int cy0;
  int rows;                              /* Rows shown (the last cut) */
  int full;                              /* ... whole ones */
};

/* The internal clipboard: what was copied, formulas and all */

struct clipcell_s
{
  int dr;
  int dc;
  uint32_t fmt;
  char *input;                           /* Not a formula */
  struct sh_formula_s *f;
};

struct clip_s
{
  int r0;                                /* Where it came from */
  int c0;
  int rows;
  int cols;
  struct clipcell_s *cell;
  int n;
  const char *text;                      /* The desktop's clipboard then */
  struct desk_win_s *cutfrom;            /* Cut: moved from there */
};

struct sw_s
{
  struct sheet_s *sh;
  char path[PATH_MAX];                   /* "": untitled */
  bool modified;
  char title[40];

  /* The view */

  int top;                               /* First row and column shown */
  int left;
  struct sel_s sel;
  struct sel_s dsel;                     /* ... as drawn */
  bool dselvalid;

  /* Editing: the formula bar's field is what is typed, in the cell too */

  int mode;
  int tabc;                              /* Where Tab began: Enter goes back
                                          * there, -1 */
  struct ui_field_s field;
  bool pointing;                         /* The arrows point (POINT) */
  int pstart;                            /* Where the reference begins */
  int edw;                               /* The cell's editor, drawn, 0 */
  int edr;
  int edc;
  bool caret;

  /* The mouse */

  uint8_t buttons;
  int drag;
  int dragcol;
  int dragx;
  int dragw;
  uint32_t clickms;
  int clickr;
  int clickc;

  /* Undo */

  struct undo_s undo[UNDO_MAX];
  int nundo;
  struct undo_s redo[UNDO_MAX];
  int nredo;

  /* The chart */

  bool chart;
  bool line;                             /* A line chart, else bars */
  struct sh_rect_s chartr;
  struct kit_surface_s surf;
  bool surfok;
  bool dark;                             /* The theme it was drawn in */
  uint16_t accent;

  bool later;                            /* A refresh() when woken */
  bool statok;                           /* The selection's sum, of statr */
  struct sh_rect_s statr;
  struct stat_s
  {
    double sum;
    int count;
    int counta;
  } stat;
  char find[64];
  char msg[96];
  bool msgerr;

  /* The widgets and their texts */

  struct kit_s kit;
  struct kit_widget_s wg[NWG];
  char name[24];
  char status[128];
  char modetext[16];
  uint32_t lastfg;                       /* Colours last chosen */
  uint32_t lastbg;

  struct desk_ops_s *o;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char *g_openpath;
static struct clip_s g_clip;

static const char * const g_fmtitems[] =
{
  "General", "Number", "1,234.00", "Currency", "Percent", "Date",
  "Date & time"
};

static const char * const g_colours[SF_COLOURS] =
{
  "Automatic", "Red", "Orange", "Yellow", "Green", "Teal", "Blue",
  "Purple", "Grey"
};

static const char * const g_fills[SF_COLOURS] =
{
  "No fill", "Red", "Orange", "Yellow", "Green", "Teal", "Blue", "Purple",
  "Grey"
};

static const char * const g_context[] =
{
  "Cut", "Copy", "Paste", "-", "Clear", "-", "Insert Row", "Delete Row",
  "Insert Column", "Delete Column", "-", "Fill Down", "Fill Right", "-",
  "Sort A to Z", "Sort Z to A"
};

static const char * const g_more[] =
{
  "Save As" DESK_CH_ELLIPSIS, "Find" DESK_CH_ELLIPSIS, "-", "Insert Row",
  "Delete Row", "Insert Column", "Delete Column", "-", "Sort A to Z",
  "Sort Z to A", "-", "Fill Down", "Fill Right"
};

/****************************************************************************
 * Private Functions: colours
 ****************************************************************************/

static uint16_t text_colour(int i)
{
  switch (i)
    {
      case 1:
        return THEMED(RGB(0xff, 0x6b, 0x6b), RGB(0xd0, 0x21, 0x21));
      case 2:
        return THEMED(RGB(0xff, 0xa4, 0x4a), RGB(0xc8, 0x62, 0x00));
      case 3:
        return THEMED(RGB(0xff, 0xd8, 0x4a), RGB(0xa0, 0x7a, 0x00));
      case 4:
        return THEMED(RGB(0x5c, 0xd6, 0x7a), RGB(0x1c, 0x8a, 0x3a));
      case 5:
        return THEMED(RGB(0x4c, 0xd0, 0xd0), RGB(0x00, 0x80, 0x88));
      case 6:
        return THEMED(RGB(0x6c, 0xa8, 0xff), RGB(0x1a, 0x5f, 0xd0));
      case 7:
        return THEMED(RGB(0xc0, 0x8c, 0xff), RGB(0x7a, 0x3c, 0xc8));
      case 8:
        return THEMED(RGB(0x9a, 0x9a, 0xa2), RGB(0x6e, 0x6e, 0x76));
      default:
        return C_TEXT;
    }
}

static uint16_t fill_colour(int i)
{
  switch (i)
    {
      case 1:
        return THEMED(RGB(0x5a, 0x24, 0x26), RGB(0xfd, 0xdc, 0xdc));
      case 2:
        return THEMED(RGB(0x5a, 0x38, 0x1c), RGB(0xfe, 0xe8, 0xd2));
      case 3:
        return THEMED(RGB(0x55, 0x4a, 0x18), RGB(0xff, 0xf4, 0xc2));
      case 4:
        return THEMED(RGB(0x1e, 0x4a, 0x2a), RGB(0xd8, 0xf4, 0xdc));
      case 5:
        return THEMED(RGB(0x16, 0x44, 0x48), RGB(0xd2, 0xf2, 0xf2));
      case 6:
        return THEMED(RGB(0x1c, 0x36, 0x5c), RGB(0xda, 0xe8, 0xfd));
      case 7:
        return THEMED(RGB(0x3c, 0x28, 0x5a), RGB(0xec, 0xe0, 0xfd));
      case 8:
        return THEMED(RGB(0x3a, 0x3a, 0x40), RGB(0xea, 0xea, 0xee));
      default:
        return C_GRID_BG;
    }
}

static uint16_t series_colour(int i)
{
  switch (i)
    {
      case 0:
        return g_theme.accent;
      case 1:
        return text_colour(2);
      case 2:
        return text_colour(4);
      default:
        return text_colour(7);
    }
}

/****************************************************************************
 * Private Functions: geometry
 ****************************************************************************/

static int colw(struct sw_s *sw, int c)
{
  return sw->sh->colw[c] != 0 ? sw->sh->colw[c] : SH_COLW;
}

static void geo(struct sw_s *sw, struct geo_s *g)
{
  struct kit_widget_s *wg = kit_get(&sw->kit, S_GRID);

  g->x0   = wg->x;
  g->y0   = wg->y;
  g->x1   = wg->x + wg->w;
  g->y1   = wg->y + wg->h;
  g->cx0  = g->x0 + RHW;
  g->cy0  = g->y0 + HDR_H;
  g->rows = g->y1 > g->cy0 ? (g->y1 - g->cy0 + RH - 1) / RH : 0;
  g->full = g->y1 > g->cy0 ? (g->y1 - g->cy0) / RH : 0;
  g->full = g->full < 1 ? 1 : g->full;
}

/* Where column c starts (it may be left of the cells, off them) */

static int col_x(struct sw_s *sw, struct geo_s *g, int c)
{
  int x = g->cx0;
  int i;

  if (c >= sw->left)
    {
      for (i = sw->left; i < c; i++)
        {
          x += colw(sw, i);
        }
    }
  else
    {
      for (i = c; i < sw->left; i++)
        {
          x -= colw(sw, i);
        }
    }

  return x;
}

/* The column at px (>= g->cx0), its start in *x; SH_COLS past the last */

static int col_at(struct sw_s *sw, struct geo_s *g, int px, int *x)
{
  int c = sw->left;
  int cx = g->cx0;

  while (c < SH_COLS && cx + colw(sw, c) <= px)
    {
      cx += colw(sw, c);
      c++;
    }

  *x = cx;
  return c;
}

static int row_at(struct sw_s *sw, struct geo_s *g, int py)
{
  return py >= g->cy0 ? sw->top + (py - g->cy0) / RH :
                        sw->top - 1 - (g->cy0 - py - 1) / RH;
}

/* Columns wholly shown from left */

static int cols_full(struct sw_s *sw, struct geo_s *g, int left)
{
  int x = g->cx0;
  int n = 0;
  int c;

  for (c = left; c < SH_COLS && x + colw(sw, c) <= g->x1; c++)
    {
      x += colw(sw, c);
      n++;
    }

  return n < 1 ? 1 : n;
}

static void sel_rect(const struct sel_s *s, struct sh_rect_s *r)
{
  r->r0 = s->cr < s->er ? s->cr : s->er;
  r->r1 = s->cr < s->er ? s->er : s->cr;
  r->c0 = s->cc < s->ec ? s->cc : s->ec;
  r->c1 = s->cc < s->ec ? s->ec : s->cc;
}

static int look_of(const struct sel_s *s, int r, int c)
{
  struct sh_rect_s x;
  int l = 0;

  sel_rect(s, &x);
  if (r >= x.r0 && r <= x.r1 && c >= x.c0 && c <= x.c1 &&
      (x.r0 != x.r1 || x.c0 != x.c1))
    {
      l = L_IN | (r == x.r0 ? L_TOP : 0) | (r == x.r1 ? L_BOTTOM : 0) |
          (c == x.c0 ? L_LEFT : 0) | (c == x.c1 ? L_RIGHT : 0);
    }

  if (r == s->cr && c == s->cc)
    {
      l |= L_ACTIVE;
    }

  if (s->pr >= 0)
    {
      int r0 = s->pr < s->pr2 ? s->pr : s->pr2;
      int r1 = s->pr < s->pr2 ? s->pr2 : s->pr;
      int c0 = s->pc < s->pc2 ? s->pc : s->pc2;
      int c1 = s->pc < s->pc2 ? s->pc2 : s->pc;

      if (r >= r0 && r <= r1 && c >= c0 && c <= c1)
        {
          l |= (r == r0 ? L_PTOP : 0) | (r == r1 ? L_PBOTTOM : 0) |
               (c == c0 ? L_PLEFT : 0) | (c == c1 ? L_PRIGHT : 0);
        }
    }

  return l;
}

/* A header's look: 0, 1 in the selection, 2 all of it selected */

static int head_look(const struct sel_s *s, bool col, int i)
{
  struct sh_rect_s x;

  sel_rect(s, &x);
  if (col)
    {
      return i < x.c0 || i > x.c1 ? 0 :
             x.r0 == 0 && x.r1 == SH_ROWS - 1 ? 2 : 1;
    }

  return i < x.r0 || i > x.r1 ? 0 :
         x.c0 == 0 && x.c1 == SH_COLS - 1 ? 2 : 1;
}

/****************************************************************************
 * Private Functions: drawing the grid
 ****************************************************************************/

static NXWINDOW hwnd_of(struct sw_s *sw)
{
  return (NXWINDOW)sw->kit.w->hwnd;
}

/* s from x, only what falls in [x0, x1) (s is cut there for a moment:
 * no copy, little stack)
 */

static void clip_text(struct sw_s *sw, const struct desk_font_s *f, int x,
                      int y, char *s, uint16_t c, int x0, int x1)
{
  char ch[2];
  char save;
  int n = 0;
  int xx;

  ch[1] = '\0';
  while (*s != '\0')
    {
      int a;

      ch[0] = *s;
      a = text_width(f, ch);
      if (x + a > x0)
        {
          break;
        }

      x += a;
      s++;
    }

  for (xx = x; s[n] != '\0' && xx < x1 && n < 120; n++)
    {
      ch[0] = s[n];
      xx += text_width(f, ch);
    }

  save = s[n];
  s[n] = '\0';
  ui_flush(hwnd_of(sw), sw->o, n + 2);
  ops_text(sw->o, f, x, y, s, c, 255);
  s[n] = save;
}

static const struct desk_font_s *cell_font(struct sh_cell_s *c)
{
  return c != NULL && (c->fmt & SF_BOLD) ? &g_desk_font_uib :
                                           &g_desk_font_ui;
}

static bool cell_empty(struct sheet_s *sh, int r, int c)
{
  struct sh_cell_s *cell = sheet_get(sh, r, c);

  return cell == NULL || cell->kind == SK_NONE;
}

/* What a cell shows in w pixels: numbers that do not fit are ###, the
 * General format loses digits first
 */

static int cell_text(struct sw_s *sw, struct sh_cell_s *c, int w,
                     char *buf, size_t size)
{
  const struct desk_font_s *f = cell_font(c);
  int align;
  int prec;

  align = sheet_display(sw->sh, c, 11, buf, size);
  if (!sheet_is_number(c) || text_width(f, buf) <= w - 2 * PAD + 2)
    {
      return align;
    }

  if (SF_NF(c->fmt) == SF_GENERAL)
    {
      for (prec = 10; prec >= 1; prec--)
        {
          sheet_display(sw->sh, c, prec, buf, size);
          if (text_width(f, buf) <= w - 2 * PAD + 2)
            {
              return align;
            }
        }
    }

  {
    int n = (w - 2 * PAD) / text_width(f, "#");

    n = n < 1 ? 1 : n >= (int)size ? (int)size - 1 : n;
    memset(buf, '#', n);
    buf[n] = '\0';
  }

  return align;
}

/* A cell's ground, grid lines and selection, cut to [xa, xb) x [ya, yb) */

static void cell_ground(struct sw_s *sw, int r, int c, int x, int w, int y,
                        int xa, int xb, int ya, int yb)
{
  struct desk_ops_s *o = sw->o;
  struct sh_cell_s *cell = sheet_get(sw->sh, r, c);
  int l = look_of(&sw->sel, r, c);
  int start;

  ui_flush(hwnd_of(sw), o, 16);
  start = o->n;
  ops_fill(o, x, y, w, RH, fill_colour(cell != NULL ? SF_BG(cell->fmt) :
                                                      0));
  if ((l & L_IN) && !(l & L_ACTIVE))
    {
      ops_blend(o, x, y, w, RH, g_theme.accent, A_TINT);
    }

  ops_fill(o, x + w - 1, y, 1, RH, C_GRID_LINE);
  ops_fill(o, x, y + RH - 1, w, 1, C_GRID_LINE);
  if (l & L_ACTIVE)
    {
      l |= L_TOP | L_BOTTOM | L_LEFT | L_RIGHT;
    }

  if (l & (L_TOP | L_PTOP))
    {
      ops_fill(o, x, y, w, 2, (l & L_PTOP) ? g_theme.success :
                                             g_theme.accent);
    }

  if (l & (L_BOTTOM | L_PBOTTOM))
    {
      ops_fill(o, x, y + RH - 2, w, 2, (l & L_PBOTTOM) ? g_theme.success :
                                                         g_theme.accent);
    }

  if (l & (L_LEFT | L_PLEFT))
    {
      ops_fill(o, x, y, 2, RH, (l & L_PLEFT) ? g_theme.success :
                                               g_theme.accent);
    }

  if (l & (L_RIGHT | L_PRIGHT))
    {
      ops_fill(o, x + w - 2, y, 2, RH, (l & L_PRIGHT) ? g_theme.success :
                                                        g_theme.accent);
    }

  ui_clip(o, start, xa > x ? xa : x, ya > y ? ya : y,
          xb < x + w ? xb : x + w, yb < y + RH ? yb : y + RH);
}

/* A cell's text, cut to [xa, xb) x [ya, yb) and to where it may go (run
 * on into empty cells at its right if it is text at the left)
 */

static void cell_textdraw(struct sw_s *sw, struct geo_s *g, int r, int c,
                          int x, int y, int xa, int xb, int ya, int yb)
{
  static char buf[256];                  /* The desktop's thread only */
  struct sh_cell_s *cell = sheet_get(sw->sh, r, c);
  const struct desk_font_s *f = cell_font(cell);
  struct desk_ops_s *o = sw->o;
  int w = colw(sw, c);
  int align;
  int tw;
  int tx;
  int end = x + w;
  int start;
  int nc;
  uint16_t k;

  if (cell == NULL || cell->kind == SK_NONE)
    {
      return;
    }

  align = cell_text(sw, cell, w, buf, sizeof(buf));
  if (buf[0] == '\0')
    {
      return;
    }

  tw = text_width(f, buf);
  if (align == SF_RIGHT)
    {
      tx = x + w - PAD - tw;
    }
  else if (align == SF_CENTRE)
    {
      tx = x + (w - tw) / 2;
    }
  else
    {
      tx = x + PAD;

      /* Text runs on into the empty cells at its right */

      for (nc = c + 1; !sheet_is_number(cell) && tx + tw > end &&
                       nc < SH_COLS && nc < c + 40 &&
                       cell_empty(sw->sh, r, nc); nc++)
        {
          end += colw(sw, nc);
        }
    }

  xa = xa > x + (align == SF_LEFT ? 0 : 1) ? xa : x + 1;
  xb = xb < end - 2 ? xb : end - 2;
  if (xb <= xa)
    {
      return;
    }

  k = cell->vt == SV_ERR ? g_theme.error : text_colour(SF_FG(cell->fmt));
  ui_flush(hwnd_of(sw), o, 130);
  start = o->n;
  clip_text(sw, f, tx, y + (RH - text_height(f)) / 2, buf, k, xa, xb);
  ui_clip(o, start, xa, ya > y ? ya : y, xb, yb < y + RH - 1 ? yb :
          y + RH - 1);
}

/* Row r's cells in [xa, xb) (xa >= the cells' left) */

static void row_draw(struct sw_s *sw, struct geo_s *g, int r, int xa,
                     int xb, int ya, int yb)
{
  int y = g->cy0 + (r - sw->top) * RH;
  int first = -1;
  int fx = 0;
  int x;
  int c;

  if (r >= SH_ROWS)
    {
      ui_flush(hwnd_of(sw), sw->o, 1);
      ops_fill(sw->o, xa, ya, xb - xa, yb - ya, g_theme.window_bg);
      return;
    }

  /* The grounds */

  for (c = sw->left, x = g->cx0; c < SH_COLS && x < xb; x += colw(sw, c),
       c++)
    {
      if (x + colw(sw, c) <= xa)
        {
          continue;
        }

      if (first < 0)
        {
          first = c;
          fx = x;
        }

      cell_ground(sw, r, c, x, colw(sw, c), y, xa, xb, ya, yb);
    }

  if (c == SH_COLS && x < xb)
    {
      int x0 = x > xa ? x : xa;

      ui_flush(hwnd_of(sw), sw->o, 1);
      ops_fill(sw->o, x0, ya, xb - x0, yb - ya, g_theme.window_bg);
    }

  if (first < 0)
    {
      return;
    }

  /* Text that comes in from the left, then each cell's */

  {
    int sx = fx;

    for (c = first - 1; c >= 0 && c >= first - 40; c--)
      {
        sx -= colw(sw, c);
        if (!cell_empty(sw->sh, r, c))
          {
            cell_textdraw(sw, g, r, c, sx, y, xa, xb, ya, yb);
            break;
          }
      }
  }

  for (c = first, x = fx; c < SH_COLS && x < xb; x += colw(sw, c), c++)
    {
      cell_textdraw(sw, g, r, c, x, y, xa, xb, ya, yb);
    }
}

static void head_ground(struct sw_s *sw, int x, int y, int w, int h,
                        int look)
{
  struct desk_ops_s *o = sw->o;

  ops_fill(o, x, y, w, h, look == 0 ? C_HEAD_BG :
           colour_mix(C_HEAD_BG, g_theme.accent, look == 2 ? 170 :
                      THEMED(80, 56)));
  ops_fill(o, x + w - 1, y, 1, h, C_HEAD_LINE);
  ops_fill(o, x, y + h - 1, w, 1, C_HEAD_LINE);
}

static void colhead_draw(struct sw_s *sw, struct geo_s *g, int c, int x,
                         int xa, int xb, int ya, int yb)
{
  const struct desk_font_s *f = &g_desk_font_small;
  int look = head_look(&sw->sel, true, c);
  int w = colw(sw, c);
  char name[4];
  int start;

  ui_flush(hwnd_of(sw), sw->o, 8);
  start = sw->o->n;
  head_ground(sw, x, g->y0, w, HDR_H, look);
  sheet_colname(c, name);
  ops_text_centre(sw->o, look ? &g_desk_font_uib : f, x + w / 2,
                  g->y0 + (HDR_H - text_height(look ? &g_desk_font_uib :
                                                      f)) / 2,
                  name, look ? C_TEXT : C_TEXT_DIM, 255);
  ui_clip(sw->o, start, xa > x ? xa : x, ya, xb < x + w ? xb : x + w, yb);
}

static void rowhead_draw(struct sw_s *sw, struct geo_s *g, int r, int xa,
                         int xb, int ya, int yb)
{
  const struct desk_font_s *f = &g_desk_font_small;
  int y = g->cy0 + (r - sw->top) * RH;
  int look = r < SH_ROWS ? head_look(&sw->sel, false, r) : 0;
  char num[12];
  int start;

  ui_flush(hwnd_of(sw), sw->o, 10);
  start = sw->o->n;
  if (r >= SH_ROWS)
    {
      ops_fill(sw->o, g->x0, y, RHW, RH, g_theme.window_bg);
    }
  else
    {
      head_ground(sw, g->x0, y, RHW, RH, look);
      snprintf(num, sizeof(num), "%d", r + 1);
      ops_text_centre(sw->o, look ? &g_desk_font_uib : f, g->x0 + RHW / 2,
                      y + (RH - text_height(look ? &g_desk_font_uib : f)) /
                      2, num, look ? C_TEXT : C_TEXT_DIM, 255);
    }

  ui_clip(sw->o, start, xa, ya > y ? ya : y, xb, yb < y + RH ? yb : y + RH);
}

static void editor_paint(struct sw_s *sw, struct geo_s *g, int xa, int ya,
                         int xb, int yb);
static void chart_paint(struct sw_s *sw, struct geo_s *g, int xa, int ya,
                        int xb, int yb);

/* Everything of the grid in [xa, xb) x [ya, yb) */

static void grid_rect(struct sw_s *sw, struct geo_s *g, int xa, int ya,
                      int xb, int yb)
{
  int r;
  int c;
  int x;

  xa = xa < g->x0 ? g->x0 : xa;
  ya = ya < g->y0 ? g->y0 : ya;
  xb = xb > g->x1 ? g->x1 : xb;
  yb = yb > g->y1 ? g->y1 : yb;
  if (xa >= xb || ya >= yb)
    {
      return;
    }

  /* The corner: all of it */

  if (xa < g->cx0 && ya < g->cy0)
    {
      int start;

      ui_flush(hwnd_of(sw), sw->o, 4);
      start = sw->o->n;
      head_ground(sw, g->x0, g->y0, RHW, HDR_H, 0);
      ui_triangle(sw->o, g->x0 + RHW - 9, g->y0 + HDR_H - 9, 8, 3,
                  C_TEXT_FAINT, 255);
      ui_clip(sw->o, start, xa, ya, g->cx0 < xb ? g->cx0 : xb,
              g->cy0 < yb ? g->cy0 : yb);
    }

  /* The column headers */

  if (ya < g->cy0)
    {
      int a = xa > g->cx0 ? xa : g->cx0;

      for (c = col_at(sw, g, a, &x); c < SH_COLS && x < xb;
           x += colw(sw, c), c++)
        {
          colhead_draw(sw, g, c, x, a, xb, ya, yb < g->cy0 ? yb : g->cy0);
        }

      if (c == SH_COLS && x < xb)
        {
          ui_flush(hwnd_of(sw), sw->o, 1);
          ops_fill(sw->o, x > a ? x : a, ya, xb - (x > a ? x : a),
                   (yb < g->cy0 ? yb : g->cy0) - ya, g_theme.window_bg);
        }
    }

  if (yb <= g->cy0)
    {
      goto overlays;
    }

  ya = ya > g->cy0 ? ya : g->cy0;
  for (r = row_at(sw, g, ya); g->cy0 + (r - sw->top) * RH < yb; r++)
    {
      int y = g->cy0 + (r - sw->top) * RH;
      int ra = ya > y ? ya : y;
      int rb = yb < y + RH ? yb : y + RH;

      if (xa < g->cx0)
        {
          rowhead_draw(sw, g, r, xa, xb < g->cx0 ? xb : g->cx0, ra, rb);
        }

      if (xb > g->cx0)
        {
          row_draw(sw, g, r, xa > g->cx0 ? xa : g->cx0, xb, ra, rb);
        }
    }

overlays:
  editor_paint(sw, g, xa, ya, xb, yb);
  chart_paint(sw, g, xa, ya, xb, yb);
}

static void grid_all(struct sw_s *sw)
{
  struct geo_s g;

  geo(sw, &g);
  grid_rect(sw, &g, g.x0, g.y0, g.x1, g.y1);
}

/* Row r, all of it that shows */

static void grid_row(struct sw_s *sw, struct geo_s *g, int r)
{
  int y = g->cy0 + (r - sw->top) * RH;

  if (r >= sw->top && r < sw->top + g->rows)
    {
      grid_rect(sw, g, g->cx0, y, g->x1, y + RH);
    }
}

/* Send what was drawn; menus, tooltips and dialogs over it again */

static void grid_send(struct sw_s *sw)
{
  struct geo_s g;

  if (sw->o->n > 0)
    {
      ops_draw(hwnd_of(sw), sw->o);
      geo(sw, &g);
      kit_touched(&sw->kit, g.x0, g.y0, g.x1 - g.x0, g.y1 - g.y0);
    }
}

/****************************************************************************
 * Private Functions: the cell's editor
 ****************************************************************************/

static bool editing(struct sw_s *sw)
{
  return sw->mode != MODE_READY;
}

/* Where the editor is: over the cell, as wide as its text needs */

static bool editor_rect(struct sw_s *sw, struct geo_s *g, int *x, int *y,
                        int *w)
{
  const struct desk_font_s *f = &g_desk_font_ui;
  int tw = text_width(f, sw->field.text) + 2 * PAD + 4;

  if (!editing(sw) || sw->sel.cr < sw->top ||
      sw->sel.cr >= sw->top + g->rows || sw->sel.cc < sw->left)
    {
      return false;
    }

  *x = col_x(sw, g, sw->sel.cc);
  *y = g->cy0 + (sw->sel.cr - sw->top) * RH;
  *w = colw(sw, sw->sel.cc);
  *w = tw > *w ? tw : *w;
  *w = *x + *w > g->x1 ? g->x1 - *x : *w;
  return *x < g->x1 && *w > 0;
}

static void editor_paint(struct sw_s *sw, struct geo_s *g, int xa, int ya,
                         int xb, int yb)
{
  const struct desk_font_s *f = &g_desk_font_ui;
  struct desk_ops_s *o = sw->o;
  int x;
  int y;
  int w;
  int tx;
  int cx;
  int start;
  char save;

  if (!editor_rect(sw, g, &x, &y, &w) || x >= xb || x + w <= xa ||
      y >= yb || y + RH <= ya)
    {
      return;
    }

  ui_flush(hwnd_of(sw), o, 140);
  start = o->n;
  ops_fill(o, x, y, w, RH, C_EDIT_BG);
  ops_fill(o, x, y, w, 2, g_theme.accent);
  ops_fill(o, x, y + RH - 2, w, 2, g_theme.accent);
  ops_fill(o, x, y, 2, RH, g_theme.accent);
  ops_fill(o, x + w - 2, y, 2, RH, g_theme.accent);

  /* The caret in view: the text moves left if it must */

  save = sw->field.text[sw->field.cur];
  sw->field.text[sw->field.cur] = '\0';
  cx = text_width(f, sw->field.text);
  sw->field.text[sw->field.cur] = save;
  tx = x + PAD;
  if (tx + cx > x + w - PAD)
    {
      tx = x + w - PAD - cx;
    }

  clip_text(sw, f, tx, y + (RH - text_height(f)) / 2, sw->field.text,
            C_TEXT, x + 2, x + w - 2);
  if (sw->caret && sw->kit.focus != KIT_NONE &&
      sw->kit.wg[sw->kit.focus].id == S_GRID && sw->kit.w->active)
    {
      ops_fill(o, tx + cx, y + 4, 1, RH - 8, C_CARET);
    }

  ui_clip(o, start, xa > x + 2 ? xa : x, ya > y ? ya : y,
          xb < x + w ? xb : x + w, yb < y + RH ? yb : y + RH);
}

/* The editor again: what it no longer covers drawn as cells */

static void editor_update(struct sw_s *sw)
{
  struct geo_s g;
  int x;
  int y;
  int w = 0;

  geo(sw, &g);
  if (!editor_rect(sw, &g, &x, &y, &w))
    {
      w = 0;
    }

  if (sw->edw > 0 && (sw->edr != sw->sel.cr || sw->edc != sw->sel.cc ||
                      w < sw->edw))
    {
      int ex = col_x(sw, &g, sw->edc);
      int ey = g.cy0 + (sw->edr - sw->top) * RH;
      bool same = sw->edr == sw->sel.cr && sw->edc == sw->sel.cc && w > 0;

      grid_rect(sw, &g, same ? ex + w : ex, ey, ex + sw->edw, ey + RH);
    }

  sw->edw = w;
  sw->edr = sw->sel.cr;
  sw->edc = sw->sel.cc;
  if (w > 0)
    {
      editor_paint(sw, &g, x, y, x + w, y + RH);
    }
}

/****************************************************************************
 * Private Functions: the chart
 ****************************************************************************/

static bool chart_rect(struct sw_s *sw, struct geo_s *g, int *x, int *y,
                       int *w, int *h)
{
  if (!sw->chart)
    {
      return false;
    }

  *w = CHART_W < g->x1 - g->cx0 - 16 ? CHART_W : g->x1 - g->cx0 - 16;
  *h = CHART_H < g->y1 - g->cy0 - 16 ? CHART_H : g->y1 - g->cy0 - 16;
  *x = g->x1 - *w - 10;
  *y = g->y1 - *h - 10;
  return *w > 80 && *h > 60;
}

/* The values charted: up to four columns of numbers, a column of text at
 * the left for the names
 */

static double chart_value(struct sw_s *sw, int r, int c, bool *ok)
{
  struct sh_cell_s *cell = sheet_get(sw->sh, r, c);

  *ok = sheet_is_number(cell);
  return *ok ? cell->num : 0;
}

/* Draw the chart into its surface: a card, a title, axes, the bars or
 * lines
 */

static void chart_render(struct sw_s *sw, int w, int h)
{
  struct sh_rect_s *r = &sw->chartr;
  struct desk_ops_s *o = sw->o;
  const struct desk_font_s *f = &g_desk_font_small;
  char title[48];
  char a[12];
  char b[12];
  int labels = 0;
  int nser = 0;
  int ser[CHART_SERIES];
  int n = r->r1 - r->r0 + 1;
  int r0 = r->r0;                        /* The first row of values */
  bool head = false;                     /* The first names the series */
  double lo = 0;
  double hi = 0;
  int px0 = 36;
  int py0 = CHART_TITLE + 6;
  int px1 = w - 10;
  int py1 = h - 20;
  int i;
  int s;
  bool ok;

  if (kit_surface_open(&sw->surf, w, h, sw->kit.w) < 0)
    {
      sw->surfok = false;
      return;
    }

  sw->surfok = true;
  sw->dark = g_theme.dark;
  sw->accent = g_theme.accent;

  /* Which columns have numbers: the series; text in the first: names */

  for (i = r->c0; i <= r->c1 && nser < CHART_SERIES; i++)
    {
      int k;
      int any = 0;
      int text = 0;

      for (k = r->r0; k <= r->r1 && k < r->r0 + 200; k++)
        {
          struct sh_cell_s *cell = sheet_get(sw->sh, k, i);

          any += sheet_is_number(cell);
          text += cell != NULL && cell->vt == SV_STR;
        }

      if (i == r->c0 && text > any && r->c1 > r->c0)
        {
          labels = 1;
        }
      else if (any)
        {
          ser[nser++] = i;
        }
    }

  /* Text over every series: their names, not values */

  if (nser > 0 && n > 1)
    {
      head = true;
      for (s = 0; s < nser; s++)
        {
          struct sh_cell_s *hc = sheet_get(sw->sh, r->r0, ser[s]);

          head &= hc != NULL && hc->vt == SV_STR;
        }

      if (head)
        {
          r0++;
          n--;
          py0 += 14;
        }
    }

  n = n > 200 ? 200 : n;
  for (s = 0; s < nser; s++)
    {
      for (i = 0; i < n; i++)
        {
          double v = chart_value(sw, r0 + i, ser[s], &ok);

          lo = ok && v < lo ? v : lo;
          hi = ok && v > hi ? v : hi;
        }
    }

  if (hi == lo)
    {
      hi = lo + 1;
    }

  ops_reset(o);
  ops_fill(o, 0, 0, w, h, g_theme.window_bg);
  ops_rrect(o, 0, 0, w, h, LUT_R12, 12, g_theme.separator, 255);
  ops_rrect(o, 1, 1, w - 2, h - 2, LUT_R12, 11, g_theme.card, 255);
  sheet_refname(r->r0, r->c0, a);
  sheet_refname(r->r1, r->c1, b);
  snprintf(title, sizeof(title), "%s:%s", a, b);
  ops_text(o, &g_desk_font_uib, 12, 6, title, C_TEXT, 255);
  ops_text(o, f, w - 90, 7, sw->line ? "Bars" : "Line", g_theme.accent,
           255);

  /* The legend: each series' colour and name */

  if (head)
    {
      int lx = 12;

      for (s = 0; s < nser && lx < w - 40; s++)
        {
          struct sh_cell_s *hc = sheet_get(sw->sh, r->r0, ser[s]);
          char name[24];

          ui_fit(f, hc->str, 90, name, sizeof(name));
          ops_fill(o, lx, CHART_TITLE + 4, 8, 8, series_colour(s));
          lx += 12 + ops_text(o, f, lx + 12, CHART_TITLE, name, C_TEXT_DIM,
                              255) + 12;
        }
    }
  ops_text(o, &g_desk_font_uib, w - 20, 5, DESK_CH_TIMES, C_TEXT_DIM, 255);

  /* The axes: zero, the top and the bottom */

  {
    int zy = py1 - (int)((0 - lo) * (py1 - py0) / (hi - lo));

    ops_fill(o, px0, zy, px1 - px0, 1, g_theme.text_faint);
    ops_fill(o, px0, py0, 1, py1 - py0, g_theme.separator);
    num_format(hi, 4, a, sizeof(a));
    ops_text(o, f, px0 - 4 - text_width(f, a), py0 - 6, a, C_TEXT_DIM, 255);
    num_format(lo, 4, a, sizeof(a));
    ops_text(o, f, px0 - 4 - text_width(f, a), py1 - 6, a, C_TEXT_DIM, 255);
    ops_draw(sw->surf.hwnd, o);

    if (nser == 0 || n == 0)
      {
        ops_text_centre(o, f, w / 2, h / 2, "No numbers selected",
                        C_TEXT_DIM, 255);
        ops_draw(sw->surf.hwnd, o);
        return;
      }

    for (s = 0; s < nser; s++)
      {
        int prevx = -1;
        int prevy = 0;

        for (i = 0; i < n; i++)
          {
            double v = chart_value(sw, r0 + i, ser[s], &ok);
            int slot = (px1 - px0) / n;
            int cx = px0 + slot * i + slot / 2;
            int vy = py1 - (int)((v - lo) * (py1 - py0) / (hi - lo));

            if (!ok)
              {
                prevx = -1;
                continue;
              }

            ui_flush(sw->surf.hwnd, o, 80);
            if (!sw->line)
              {
                int bw = (slot - 4) / nser;
                int bx = px0 + slot * i + 2 + bw * s;

                bw = bw < 1 ? 1 : bw;
                ops_fill(o, bx, vy < zy ? vy : zy, bw > 2 ? bw - 1 : bw,
                         vy < zy ? zy - vy : vy - zy + 1,
                         series_colour(s));
              }
            else
              {
                /* A segment from the last point: a run of columns */

                if (prevx >= 0)
                  {
                    int x;

                    for (x = prevx; x < cx; x++)
                      {
                        int ya = prevy + (vy - prevy) * (x - prevx) /
                                         (cx - prevx);
                        int yb = prevy + (vy - prevy) * (x + 1 - prevx) /
                                         (cx - prevx);
                        int t = ya < yb ? ya : yb;
                        int d = ya < yb ? yb - ya : ya - yb;

                        ui_flush(sw->surf.hwnd, o, 1);
                        ops_fill(o, x, t - 1, 1, d + 2, series_colour(s));
                      }
                  }

                ui_flush(sw->surf.hwnd, o, 1);
                ops_disc_atlas(o, cx - 3, vy - 3, 6, series_colour(s), 255);
                prevx = cx;
                prevy = vy;
              }

          }
      }

    /* The names under the axis: the first column's, or the rows' */

    for (i = 0; i < n && n <= 24; i++)
      {
        int slot = (px1 - px0) / n;
        struct sh_cell_s *lc = labels ?
                               sheet_get(sw->sh, r0 + i, r->c0) : NULL;
        char name[16];

        if (lc != NULL && lc->vt == SV_STR)
          {
            ui_fit(f, lc->str, slot - 2, name, sizeof(name));
          }
        else
          {
            snprintf(name, sizeof(name), "%d", (r0 + i + 1) & 0x1ffff);
          }

        ui_flush(sw->surf.hwnd, o, 16);
        ops_text_centre(o, f, px0 + slot * i + slot / 2, py1 + 4, name,
                        C_TEXT_DIM, 255);
      }
  }

  ops_draw(sw->surf.hwnd, o);
}

static void chart_paint(struct sw_s *sw, struct geo_s *g, int xa, int ya,
                        int xb, int yb)
{
  int x;
  int y;
  int w;
  int h;
  int start;

  if (!chart_rect(sw, g, &x, &y, &w, &h) || x >= xb || x + w <= xa ||
      y >= yb || y + h <= ya)
    {
      return;
    }

  if (!sw->surfok || sw->surf.w != w || sw->surf.h != h || sw->surf.lost ||
      sw->dark != g_theme.dark || sw->accent != g_theme.accent)
    {
      ops_draw(hwnd_of(sw), sw->o);
      kit_surface_close(&sw->surf);
      chart_render(sw, w, h);
      if (!sw->surfok)
        {
          return;
        }
    }

  ui_flush(hwnd_of(sw), sw->o, 1);
  start = sw->o->n;
  kit_surface_copy(sw->o, &sw->surf, 0, 0, w, h, x, y);
  ui_clip(sw->o, start, xa, ya, xb, yb);
}

static void chart_redraw(struct sw_s *sw)
{
  struct geo_s g;
  int x;
  int y;
  int w;
  int h;

  sw->surfok = false;
  geo(sw, &g);
  if (chart_rect(sw, &g, &x, &y, &w, &h))
    {
      chart_paint(sw, &g, x, y, x + w, y + h);
    }
}

static void chart_toggle(struct sw_s *sw)
{
  struct geo_s g;
  int x;
  int y;
  int w;
  int h;

  geo(sw, &g);
  if (sw->chart)
    {
      chart_rect(sw, &g, &x, &y, &w, &h);
      sw->chart = false;
      kit_surface_close(&sw->surf);
      sw->surfok = false;
      grid_rect(sw, &g, x, y, x + w, y + h);
    }
  else
    {
      sel_rect(&sw->sel, &sw->chartr);
      sw->chart = true;
      sw->surfok = false;
      chart_redraw(sw);
    }

}

/****************************************************************************
 * Private Functions: scrolling
 ****************************************************************************/

static void scrollbars_sync(struct sw_s *sw)
{
  struct geo_s g;
  int maxr;
  int maxc;
  int total;

  geo(sw, &g);
  sheet_extent(sw->sh, &maxr, &maxc);
  maxr = maxr > sw->sel.cr ? maxr : sw->sel.cr;
  maxc = maxc > sw->sel.cc ? maxc : sw->sel.cc;
  total = maxr + 1 > sw->top + g.full ? maxr + 1 : sw->top + g.full;
  total = total + g.full > SH_ROWS ? SH_ROWS : total + g.full;
  kit_scroll_set(&sw->kit, S_VSCROLL, sw->top, g.full, total);
  total = maxc + 1 > sw->left + cols_full(sw, &g, sw->left) ?
          maxc + 1 : sw->left + cols_full(sw, &g, sw->left);
  total += cols_full(sw, &g, sw->left);
  total = total > SH_COLS ? SH_COLS : total;
  kit_scroll_set(&sw->kit, S_HSCROLL, sw->left,
                 cols_full(sw, &g, sw->left), total);
}

/* The grid's pixels were moved by (dx, dy) with a COPY: the chart stays
 * where it is (where its pixels went is drawn again, then it)
 */

static void chart_moved(struct sw_s *sw, struct geo_s *g, int dx, int dy)
{
  int x;
  int y;
  int w;
  int h;

  if (chart_rect(sw, g, &x, &y, &w, &h))
    {
      grid_rect(sw, g, x + dx, y + dy, x + dx + w, y + dy + h);
      chart_paint(sw, g, x, y, x + w, y + h);
    }
}

/* Scroll to (top, left): what stays is copied, what comes is drawn */

static void scroll_to(struct sw_s *sw, int top, int left)
{
  struct geo_s g;
  struct nx_gpuop_s *op;
  int cx;
  int cy;
  int dx = 0;
  int dy;
  int i;

  geo(sw, &g);
  top = top < 0 ? 0 : top > SH_ROWS - 1 ? SH_ROWS - 1 : top;
  left = left < 0 ? 0 : left > SH_COLS - 1 ? SH_COLS - 1 : left;
  if (top == sw->top && left == sw->left)
    {
      return;
    }

  /* How far the pixels move */

  dy = (top - sw->top) * RH;
  for (i = left; i < sw->left; i++)
    {
      dx -= colw(sw, i);
    }

  for (i = sw->left; i < left; i++)
    {
      dx += colw(sw, i);
    }

  sw->top = top;
  sw->left = left;
  cx = g.x1 - g.cx0;
  cy = g.y1 - g.cy0;

  /* An editor or a chart moves with the pixels: drawn again after */

  if ((dx != 0 && dy != 0) || dx >= cx || -dx >= cx || dy >= cy ||
      -dy >= cy)
    {
      sw->edw = 0;
      grid_all(sw);
      return;
    }

  ui_flush(hwnd_of(sw), sw->o, 1);
  op = ops_add(sw->o);
  op->op = NX_GPUOP_COPY;
  if (dy != 0)
    {
      op->x  = g.x0;
      op->w  = g.x1 - g.x0;
      op->sx = g.x0;
      op->h  = cy - (dy > 0 ? dy : -dy);
      op->y  = dy > 0 ? g.cy0 : g.cy0 - dy;
      op->sy = dy > 0 ? g.cy0 + dy : g.cy0;
      if (dy > 0)
        {
          grid_rect(sw, &g, g.x0, g.y1 - dy, g.x1, g.y1);
        }
      else
        {
          grid_rect(sw, &g, g.x0, g.cy0, g.x1, g.cy0 - dy);
        }
    }
  else
    {
      op->y  = g.y0;
      op->h  = g.y1 - g.y0;
      op->sy = g.y0;
      op->w  = cx - (dx > 0 ? dx : -dx);
      op->x  = dx > 0 ? g.cx0 : g.cx0 - dx;
      op->sx = dx > 0 ? g.cx0 + dx : g.cx0;
      if (dx > 0)
        {
          grid_rect(sw, &g, g.x1 - dx, g.y0, g.x1, g.y1);
        }
      else
        {
          grid_rect(sw, &g, g.cx0, g.y0, g.cx0 - dx, g.y1);
        }
    }

  /* The editor moved with its cell (the cell is drawn under it again if
   * it went); the chart stays where it is: where its pixels went is
   * drawn again, then it
   */

  if (sw->edw > 0)
    {
      sw->edw = 0;
      editor_update(sw);
    }

  chart_moved(sw, &g, -dx, -dy);
}

/* The cell (r, c) into view */

static void follow(struct sw_s *sw, int r, int c)
{
  struct geo_s g;
  int top = sw->top;
  int left = sw->left;

  geo(sw, &g);
  if (r < top)
    {
      top = r;
    }
  else if (r >= top + g.full)
    {
      top = r - g.full + 1;
    }

  if (c < left)
    {
      left = c;
    }
  else
    {
      while (left < c && c >= left + cols_full(sw, &g, left))
        {
          left++;
        }
    }

  scroll_to(sw, top, left);
}

/****************************************************************************
 * Private Functions: what is shown besides the grid
 ****************************************************************************/

static const char *base_name(struct sw_s *sw)
{
  const char *s = strrchr(sw->path, '/');

  return sw->path[0] == '\0' ? "Untitled" : s != NULL ? s + 1 : sw->path;
}

static void set_title(struct sw_s *sw)
{
  char t[40];

  snprintf(t, sizeof(t), "%s%s", base_name(sw),
           sw->modified ? "  " DESK_CH_BULLET : "");
  if (strcmp(t, sw->title) != 0)
    {
      strlcpy(sw->title, t, sizeof(sw->title));
      strlcpy(sw->kit.w->title, t, sizeof(sw->kit.w->title));
      desk_win_title(sw->kit.w);
    }
}

static void set_msg(struct sw_s *sw, bool err, const char *msg,
                    const char *arg)
{
  if (arg == NULL)
    {
      strlcpy(sw->msg, msg, sizeof(sw->msg));
    }
  else
    {
      snprintf(sw->msg, sizeof(sw->msg), msg, arg);
    }

  sw->msgerr = err;
}

/* The status bar: a message, else the selection's sum, average, count */

static bool stat_one(void *arg, struct sh_cell_s *c)
{
  struct stat_s *st = arg;

  st->counta++;
  if (sheet_is_number(c))
    {
      st->sum += c->num;
      st->count++;
    }

  return true;
}

static void status_sync(struct sw_s *sw)
{
  char s[128];
  const char *mode;
  struct sh_rect_s r;

  s[0] = '\0';
  sel_rect(&sw->sel, &r);
  if (sw->msg[0] != '\0')
    {
      strlcpy(s, sw->msg, sizeof(s));
    }
  else if (r.r0 != r.r1 || r.c0 != r.c1)
    {
      struct stat_s st;
      char a[40];
      char b[40];

      /* Worked out again when the selection or the values change */

      if (!sw->statok || memcmp(&r, &sw->statr, sizeof(r)) != 0)
        {
          memset(&sw->stat, 0, sizeof(sw->stat));
          sheet_each_in(sw->sh, &r, stat_one, &sw->stat);
          sw->statr = r;
          sw->statok = true;
        }

      st = sw->stat;
      if (st.count > 0)
        {
          num_format(st.sum, 10, a, sizeof(a));
          num_format(st.sum / st.count, 6, b, sizeof(b));
          snprintf(s, sizeof(s), "Sum %s   Average %s   Count %d", a, b,
                   st.counta);
        }
      else if (st.counta > 0)
        {
          snprintf(s, sizeof(s), "Count %d", st.counta);
        }
    }

  if (strcmp(s, sw->status) != 0)
    {
      strlcpy(sw->status, s, sizeof(sw->status));
      kit_invalidate(&sw->kit, S_STATUS);
    }

  kit_set_style(&sw->kit, S_STATUS, sw->msg[0] != '\0' && sw->msgerr ?
                KIT_TEXT_ERROR : KIT_TEXT_DIM);
  mode = sw->pointing ? "POINT" : sw->mode == MODE_ENTER ? "ENTER" :
         sw->mode == MODE_EDIT ? "EDIT" : "READY";
  if (strcmp(mode, sw->modetext) != 0)
    {
      strlcpy(sw->modetext, mode, sizeof(sw->modetext));
      kit_set_text(&sw->kit, S_MODE, sw->modetext);
    }
}

/* The formula bar: the cell's name, what it holds (unless being typed) */

static void formula_sync(struct sw_s *sw)
{
  struct sh_rect_s r;
  char name[24];
  char a[12];
  char b[12];

  sel_rect(&sw->sel, &r);
  sheet_refname(sw->sel.cr, sw->sel.cc, a);
  if (r.r0 != r.r1 || r.c0 != r.c1)
    {
      sheet_refname(r.r0, r.c0, a);
      sheet_refname(r.r1, r.c1, b);
      snprintf(name, sizeof(name), "%s:%s", a, b);
    }
  else
    {
      strlcpy(name, a, sizeof(name));
    }

  if (strcmp(name, sw->name) != 0)
    {
      strlcpy(sw->name, name, sizeof(sw->name));
      kit_invalidate(&sw->kit, S_NAME);
    }

  if (!editing(sw))
    {
      char *buf = malloc(SH_INPUT_MAX);

      if (buf != NULL)
        {
          sheet_input(sw->sh, sheet_get(sw->sh, sw->sel.cr, sw->sel.cc), buf,
                      SH_INPUT_MAX);
          if (strcmp(buf, sw->field.text) != 0)
            {
              ui_field_set(&sw->field, buf);
              kit_invalidate(&sw->kit, S_FIELD);
            }

          free(buf);
        }
    }
}

static void toolbar_sync(struct sw_s *sw)
{
  uint32_t f = sheet_fmt(sw->sh, sw->sel.cr, sw->sel.cc);
  struct sh_cell_s *c = sheet_get(sw->sh, sw->sel.cr, sw->sel.cc);
  int align = SF_ALIGN(f);

  if (align == SF_AUTO)
    {
      align = sheet_is_number(c) ? SF_RIGHT : c != NULL &&
              (c->vt == SV_BOOL || c->vt == SV_ERR) ? SF_CENTRE : SF_LEFT;
    }

  kit_set_style(&sw->kit, S_SAVE, KIT_BTN_FLAT);
  kit_enable(&sw->kit, S_UNDO, sw->nundo > 0);
  kit_enable(&sw->kit, S_REDO, sw->nredo > 0);
  kit_set_value(&sw->kit, S_BOLD, (f & SF_BOLD) != 0);
  kit_set_value(&sw->kit, S_ALEFT, align == SF_LEFT);
  kit_set_value(&sw->kit, S_ACENTRE, align == SF_CENTRE);
  kit_set_value(&sw->kit, S_ARIGHT, align == SF_RIGHT);
  kit_set_value(&sw->kit, S_NUMFMT, SF_NF(f));
  if (SF_FG(f) != sw->lastfg || SF_BG(f) != sw->lastbg)
    {
      sw->lastfg = SF_FG(f);
      sw->lastbg = SF_BG(f);
      kit_invalidate(&sw->kit, S_COLOUR);
      kit_invalidate(&sw->kit, S_FILL);
    }

}

/* The selection's look: the cells and headers whose look changed */

static void sel_draw(struct sw_s *sw)
{
  struct geo_s g;
  int r;
  int c;
  int x;

  geo(sw, &g);
  if (!sw->dselvalid)
    {
      sw->dsel = sw->sel;
      sw->dselvalid = true;
      return;
    }

  if (memcmp(&sw->dsel, &sw->sel, sizeof(sw->sel)) == 0)
    {
      return;
    }

  /* Headers */

  for (c = sw->left, x = g.cx0; c < SH_COLS && x < g.x1;
       x += colw(sw, c), c++)
    {
      if (head_look(&sw->dsel, true, c) != head_look(&sw->sel, true, c))
        {
          grid_rect(sw, &g, x, g.y0, x + colw(sw, c), g.cy0);
        }
    }

  for (r = sw->top; r < sw->top + g.rows && r < SH_ROWS; r++)
    {
      int y = g.cy0 + (r - sw->top) * RH;
      int run = -1;

      if (head_look(&sw->dsel, false, r) != head_look(&sw->sel, false, r))
        {
          grid_rect(sw, &g, g.x0, y, g.cx0, y + RH);
        }

      /* Runs of cells whose look changed */

      for (c = sw->left, x = g.cx0; ; x += colw(sw, c), c++)
        {
          bool diff = c < SH_COLS && x < g.x1 &&
                      look_of(&sw->dsel, r, c) != look_of(&sw->sel, r, c);

          if (diff && run < 0)
            {
              run = x;
            }
          else if (!diff && run >= 0)
            {
              grid_rect(sw, &g, run, y, x, y + RH);
              run = -1;
            }

          if (c >= SH_COLS || x >= g.x1)
            {
              break;
            }
        }
    }

  sw->dsel = sw->sel;
}

/* After anything: the rows whose values changed, the selection, the
 * widgets, all sent
 */

static void refresh(struct sw_s *sw)
{
  struct sheet_s *sh = sw->sh;
  struct geo_s g;
  int i;

  geo(sw, &g);
  if (sh->alltouched || sh->touched.n > 0)
    {
      sw->statok = false;
    }

  if (sh->alltouched || sh->touched.n > g.rows * 4)
    {
      grid_all(sw);
      if (sw->chart)
        {
          chart_redraw(sw);
        }
    }
  else if (sh->touched.n > 0)
    {
      uint8_t seen[16];                  /* Rows drawn, a bit each */
      bool chart = false;

      memset(seen, 0, sizeof(seen));
      for (i = 0; i < sh->touched.n; i++)
        {
          uint32_t k = sh->touched.k[i];
          int r = SH_ROW(k) - sw->top;
          int c = SH_COL(k);

          if (sw->chart && SH_ROW(k) >= sw->chartr.r0 &&
              SH_ROW(k) <= sw->chartr.r1 && c >= sw->chartr.c0 &&
              c <= sw->chartr.c1)
            {
              chart = true;
            }

          if (r >= 0 && r < g.rows && r < 128 &&
              !(seen[r / 8] & (1 << (r % 8))))
            {
              seen[r / 8] |= 1 << (r % 8);
              grid_row(sw, &g, SH_ROW(k));
            }
        }

      if (chart)
        {
          chart_redraw(sw);
        }
    }

  sh->touched.n = 0;
  sh->alltouched = false;
  sel_draw(sw);
  if (editing(sw))
    {
      editor_update(sw);
    }
  else if (sw->edw > 0)
    {
      editor_update(sw);
    }

  grid_send(sw);
  scrollbars_sync(sw);
  formula_sync(sw);
  status_sync(sw);
  toolbar_sync(sw);
  set_title(sw);
  kit_flush(&sw->kit);
}

/****************************************************************************
 * Private Functions: undo
 ****************************************************************************/

static void undo_free(struct undo_s *u)
{
  int i;

  for (i = 0; i < u->nsnap; i++)
    {
      sheet_snap_free(&u->snap[i]);
    }

  free(u->snap);
  memset(u, 0, sizeof(*u));
}

static void undo_add(struct undo_s *u, struct sheet_s *sh,
                     struct sh_cell_s *c, uint32_t key)
{
  if (u->nsnap == u->capsnap)
    {
      int cap = u->capsnap < 8 ? 8 : u->capsnap * 2;
      struct sh_snap_s *p = realloc(u->snap, cap * sizeof(*p));

      if (p == NULL)
        {
          return;
        }

      u->snap = p;
      u->capsnap = cap;
    }

  if (c != NULL)
    {
      sheet_snap_cell(sh, c, &u->snap[u->nsnap++]);
    }
  else
    {
      sheet_snap(sh, key, &u->snap[u->nsnap++]);
    }
}

/* The cells there are in a rectangle, kept */

static void undo_rect(struct undo_s *u, struct sheet_s *sh,
                      const struct sh_rect_s *r)
{
  struct sh_cell_s **v;
  int n;
  int i;

  if (u->nrect < 2)
    {
      u->rect[u->nrect++] = *r;
    }

  v = sheet_cells_in(sh, r, &n);
  for (i = 0; i < n; i++)
    {
      undo_add(u, sh, v[i], 0);
    }

  free(v);
}

static void snap_cb(void *arg, struct sh_cell_s *c)
{
  struct sw_s *sw = arg;

  undo_add(&sw->undo[UNDO_MAX - 1], sw->sh, c, 0);
}

/* A new step: into the history (the oldest goes when it is full, or too
 * many cells are kept)
 */

static void undo_push(struct undo_s *stack, int *n, struct undo_s *u)
{
  int total = u->nsnap;
  int i;

  for (i = 0; i < *n; i++)
    {
      total += stack[i].nsnap;
    }

  while (*n > 0 && (*n >= UNDO_MAX - 1 || total > SNAP_MAX))
    {
      total -= stack[0].nsnap;
      undo_free(&stack[0]);
      memmove(&stack[0], &stack[1], (*n - 1) * sizeof(*stack));
      (*n)--;
      memset(&stack[*n], 0, sizeof(*stack));
    }

  stack[(*n)++] = *u;
  memset(u, 0, sizeof(*u));
}

static void redo_clear(struct sw_s *sw)
{
  while (sw->nredo > 0)
    {
      undo_free(&sw->redo[--sw->nredo]);
    }
}

/* Begin a step of cells: the rectangle's as they are */

static struct undo_s *step_begin(struct sw_s *sw)
{
  struct undo_s *u = &sw->undo[UNDO_MAX - 1];     /* The one being made */

  undo_free(u);
  u->kind = U_CELLS;
  u->sel = sw->sel;
  return u;
}

static void step_end(struct sw_s *sw)
{
  redo_clear(sw);
  undo_push(sw->undo, &sw->nundo, &sw->undo[UNDO_MAX - 1]);
  sw->modified = true;
}

/* Do a step (undo or redo); what undoes it in *inv */

static void step_apply(struct sw_s *sw, struct undo_s *u, struct undo_s *inv)
{
  struct sheet_s *sh = sw->sh;
  int i;

  memset(inv, 0, sizeof(*inv));
  inv->sel = sw->sel;
  switch (u->kind)
    {
      case U_CELLS:
        inv->kind = U_CELLS;
        for (i = 0; i < u->nrect; i++)
          {
            undo_rect(inv, sh, &u->rect[i]);
          }

        for (i = 0; i < u->nsnap; i++)
          {
            int r = SH_ROW(u->snap[i].key);
            int c = SH_COL(u->snap[i].key);
            bool in = false;
            int k;

            for (k = 0; k < u->nrect; k++)
              {
                in |= r >= u->rect[k].r0 && r <= u->rect[k].r1 &&
                      c >= u->rect[k].c0 && c <= u->rect[k].c1;
              }

            if (!in)
              {
                undo_add(inv, sh, NULL, u->snap[i].key);
              }
          }

        for (i = 0; i < u->nrect; i++)
          {
            sheet_clear_rect(sh, &u->rect[i], true);
          }

        for (i = 0; i < u->nsnap; i++)
          {
            sheet_unsnap(sh, &u->snap[i]);
          }

        sheet_recalc(sh);
        break;

      case U_INSERT:
        inv->kind = U_DELETE;
        inv->rows = u->rows;
        inv->at = u->at;
        inv->n = u->n;
        sheet_insert(sh, u->rows, u->at, u->n);
        for (i = 0; i < u->nsnap; i++)
          {
            sheet_unsnap(sh, &u->snap[i]);
          }

        sheet_recalc(sh);
        break;

      case U_DELETE:
        {
          struct undo_s *tmp = &sw->undo[UNDO_MAX - 1];

          undo_free(tmp);
          sheet_delete(sh, u->rows, u->at, u->n, snap_cb, sw);
          *inv = *tmp;
          memset(tmp, 0, sizeof(*tmp));
          inv->kind = U_INSERT;
          inv->rows = u->rows;
          inv->at = u->at;
          inv->n = u->n;
          inv->sel = sw->sel;
        }
        break;
    }
}

static void undo_do(struct sw_s *sw, bool redo)
{
  struct undo_s *from = redo ? sw->redo : sw->undo;
  int *nfrom = redo ? &sw->nredo : &sw->nundo;
  struct undo_s u;
  struct undo_s inv;

  if (*nfrom == 0)
    {
      return;
    }

  u = from[--(*nfrom)];
  memset(&from[*nfrom], 0, sizeof(*from));
  step_apply(sw, &u, &inv);
  sw->sel = u.sel;
  sw->sel.pr = -1;
  undo_free(&u);
  undo_push(redo ? sw->undo : sw->redo, redo ? &sw->nundo : &sw->nredo,
            &inv);
  sw->modified = true;
  sw->sh->alltouched = true;
  follow(sw, sw->sel.cr, sw->sel.cc);
}

/****************************************************************************
 * Private Functions: editing
 ****************************************************************************/

/* Selecting: the active cell (extending: the range's other corner) */

static void select_to(struct sw_s *sw, int r, int c, bool extend)
{
  r = r < 0 ? 0 : r >= SH_ROWS ? SH_ROWS - 1 : r;
  c = c < 0 ? 0 : c >= SH_COLS ? SH_COLS - 1 : c;
  if (extend)
    {
      sw->sel.er = r;
      sw->sel.ec = c;
    }
  else
    {
      sw->sel.cr = r;
      sw->sel.cc = c;
      sw->sel.er = r;
      sw->sel.ec = c;
    }

  sw->msg[0] = '\0';
  follow(sw, r, c);
}

static void edit_begin(struct sw_s *sw, int mode, const char *text)
{
  sw->mode = mode;
  sw->pointing = false;
  sw->sel.pr = -1;
  ui_field_set(&sw->field, text);
  sw->caret = true;
  kit_invalidate(&sw->kit, S_FIELD);
  follow(sw, sw->sel.cr, sw->sel.cc);
}

static void edit_end(struct sw_s *sw)
{
  sw->mode = MODE_READY;
  sw->pointing = false;
  sw->sel.pr = -1;
  if (sw->kit.focus != KIT_NONE && sw->kit.wg[sw->kit.focus].id != S_GRID)
    {
      kit_focus(&sw->kit, S_GRID);
    }
}

/* Put in what is typed: false if a formula did not parse (it stays) */

static bool commit(struct sw_s *sw)
{
  struct undo_s *u;
  struct sh_rect_s r;
  int ret;

  if (!editing(sw))
    {
      return true;
    }

  r.r0 = r.r1 = sw->sel.cr;
  r.c0 = r.c1 = sw->sel.cc;
  u = step_begin(sw);
  undo_rect(u, sw->sh, &r);
  ret = sheet_set_input(sw->sh, sw->sel.cr, sw->sel.cc, sw->field.text,
                        true);
  if (ret < 0)
    {
      undo_free(u);
      set_msg(sw, true, "There is a mistake in the formula", NULL);
      return false;
    }

  step_end(sw);
  sheet_recalc(sw->sh);
  edit_end(sw);
  sw->msg[0] = '\0';
  return true;
}

static void cancel(struct sw_s *sw)
{
  edit_end(sw);
  sw->msg[0] = '\0';
  sw->field.text[0] = '\1';              /* formula_sync() sets it again */
}

/* POINT: the arrows (or the mouse) put a cell's name in a formula, after
 * an operator, a parenthesis or a comma
 */

static bool can_point(struct sw_s *sw)
{
  char c;

  if (sw->pointing)
    {
      return true;
    }

  if (sw->mode != MODE_ENTER || sw->field.cur != sw->field.len ||
      sw->field.len == 0 || strchr("=+@", sw->field.text[0]) == NULL)
    {
      return false;
    }

  c = sw->field.text[sw->field.len - 1];
  return strchr("=+-*/^&(,<>:", c) != NULL;
}

static void point_at(struct sw_s *sw, int r, int c, int r2, int c2)
{
  char a[12];
  char b[12];
  char ref[28];
  int n;

  r = r < 0 ? 0 : r >= SH_ROWS ? SH_ROWS - 1 : r;
  c = c < 0 ? 0 : c >= SH_COLS ? SH_COLS - 1 : c;
  r2 = r2 < 0 ? 0 : r2 >= SH_ROWS ? SH_ROWS - 1 : r2;
  c2 = c2 < 0 ? 0 : c2 >= SH_COLS ? SH_COLS - 1 : c2;
  if (!sw->pointing)
    {
      sw->pointing = true;
      sw->pstart = sw->field.len;
    }

  sw->sel.pr = r;
  sw->sel.pc = c;
  sw->sel.pr2 = r2;
  sw->sel.pc2 = c2;
  sheet_refname(r < r2 ? r : r2, c < c2 ? c : c2, a);
  sheet_refname(r < r2 ? r2 : r, c < c2 ? c2 : c, b);
  if (r == r2 && c == c2)
    {
      strlcpy(ref, a, sizeof(ref));
    }
  else
    {
      snprintf(ref, sizeof(ref), "%s:%s", a, b);
    }

  n = strlen(ref);
  if (sw->pstart + n < (int)sizeof(sw->field.text))
    {
      memcpy(sw->field.text + sw->pstart, ref, n + 1);
      sw->field.len = sw->pstart + n;
      sw->field.cur = sw->field.len;
    }

  kit_invalidate(&sw->kit, S_FIELD);
  follow(sw, r2, c2);
}

static void point_end(struct sw_s *sw)
{
  sw->pointing = false;
  sw->sel.pr = -1;
}

/* The selection's cells: each given to fn (every cell of it, empty ones
 * too, when it is small enough; else those there are)
 */

static void each_cell(struct sw_s *sw, void (*fn)(struct sw_s *sw, int r,
                                                  int c, void *arg),
                      void *arg)
{
  struct sh_rect_s r;
  long area;
  int y;
  int x;

  sel_rect(&sw->sel, &r);
  area = (long)(r.r1 - r.r0 + 1) * (r.c1 - r.c0 + 1);
  if (area <= 20000)
    {
      for (y = r.r0; y <= r.r1; y++)
        {
          for (x = r.c0; x <= r.c1; x++)
            {
              fn(sw, y, x, arg);
            }
        }
    }
  else
    {
      struct sh_cell_s **v;
      int n;
      int i;

      v = sheet_cells_in(sw->sh, &r, &n);
      for (i = 0; i < n; i++)
        {
          fn(sw, SH_ROW(v[i]->key), SH_COL(v[i]->key), arg);
        }

      free(v);
    }
}

/* Formats: a change to each cell of the selection */

struct fmtop_s
{
  uint32_t mask;
  uint32_t value;
  int dec;                               /* Decimals: -1, 1, 0 */
};

static void fmt_one(struct sw_s *sw, int r, int c, void *arg)
{
  struct fmtop_s *op = arg;
  uint32_t f = sheet_fmt(sw->sh, r, c);

  if (op->dec != 0)
    {
      int d = SF_DEC(f);

      if (SF_NF(f) == SF_GENERAL || SF_NF(f) >= SF_DATE)
        {
          f = SF_SET_DEC(SF_SET_NF(f, SF_FIXED), op->dec > 0 ? 1 : 0);
        }
      else
        {
          d += op->dec;
          f = SF_SET_DEC(f, d < 0 ? 0 : d > 10 ? 10 : d);
        }
    }
  else
    {
      f = (f & ~op->mask) | op->value;
    }

  sheet_set_fmt(sw->sh, r, c, f);
  sheet_keys_add(&sw->sh->touched, SH_KEY(r, c));
}

static void format_sel(struct sw_s *sw, uint32_t mask, uint32_t value,
                       int dec)
{
  struct fmtop_s op;
  struct sh_rect_s r;
  struct undo_s *u;

  if (editing(sw) && !commit(sw))
    {
      return;
    }

  sel_rect(&sw->sel, &r);
  u = step_begin(sw);
  undo_rect(u, sw->sh, &r);
  op.mask = mask;
  op.value = value;
  op.dec = dec;
  each_cell(sw, fmt_one, &op);
  step_end(sw);
}

static void clear_sel(struct sw_s *sw)
{
  struct sh_rect_s r;
  struct undo_s *u;

  sel_rect(&sw->sel, &r);
  u = step_begin(sw);
  undo_rect(u, sw->sh, &r);
  sheet_clear_rect(sw->sh, &r, false);
  step_end(sw);
  sheet_recalc(sw->sh);
}

/* Copying: the selection into the internal clipboard, its values as
 * tab-separated text into the desktop's
 */

static void clip_free(void)
{
  int i;

  for (i = 0; i < g_clip.n; i++)
    {
      free(g_clip.cell[i].input);
      sheet_formula_free(g_clip.cell[i].f);
    }

  free(g_clip.cell);
  memset(&g_clip, 0, sizeof(g_clip));
}

static void copy_sel(struct sw_s *sw, bool cut)
{
  struct sh_cell_s **v;
  struct sh_rect_s r;
  char *buf;
  char *tsv;
  size_t len;
  int n;
  int i;

  sel_rect(&sw->sel, &r);
  if ((long)(r.r1 - r.r0 + 1) * (r.c1 - r.c0 + 1) > 1000000)
    {
      set_msg(sw, true, "Too much to copy", NULL);
      return;
    }

  clip_free();
  buf = malloc(SH_INPUT_MAX);
  v = sheet_cells_in(sw->sh, &r, &n);
  g_clip.cell = calloc(n > 0 ? n : 1, sizeof(*g_clip.cell));
  if (buf == NULL || g_clip.cell == NULL)
    {
      free(buf);
      free(v);
      clip_free();
      return;
    }

  g_clip.r0 = r.r0;
  g_clip.c0 = r.c0;
  g_clip.rows = r.r1 - r.r0 + 1;
  g_clip.cols = r.c1 - r.c0 + 1;
  for (i = 0; i < n; i++)
    {
      struct clipcell_s *cc = &g_clip.cell[g_clip.n++];

      cc->dr = SH_ROW(v[i]->key) - r.r0;
      cc->dc = SH_COL(v[i]->key) - r.c0;
      cc->fmt = v[i]->fmt;
      if (v[i]->kind == SK_FORMULA)
        {
          cc->f = sheet_formula_dup(v[i]->f, 0, 0);
        }
      else if (v[i]->kind != SK_NONE)
        {
          sheet_input(sw->sh, v[i], buf, SH_INPUT_MAX);
          cc->input = strdup(buf);
        }
    }

  free(v);
  free(buf);
  tsv = sheet_tsv(sw->sh, &r, &len);
  if (tsv != NULL)
    {
      desk_clip_set(tsv, len);
      free(tsv);
    }

  g_clip.text = desk_clip_get(&len);
  g_clip.cutfrom = cut ? sw->kit.w : NULL;
  set_msg(sw, false, cut ? "Cut: paste to move it" : "Copied", NULL);
}

/* Pasting at the active cell: the internal clipboard (formulas moved as
 * copies; after a cut, a move), or the desktop's text (tabs and lines)
 */

static void paste(struct sw_s *sw)
{
  struct sheet_s *sh = sw->sh;
  struct sh_rect_s r;
  struct undo_s *u;
  size_t len;
  const char *text = desk_clip_get(&len);
  int r0 = sw->sel.cr < sw->sel.er ? sw->sel.cr : sw->sel.er;
  int c0 = sw->sel.cc < sw->sel.ec ? sw->sel.cc : sw->sel.ec;
  int i;

  if (text == NULL || len == 0)
    {
      return;
    }

  u = step_begin(sw);
  if (text == g_clip.text && g_clip.rows > 0)
    {
      struct sh_rect_s s;
      int ty = 1;
      int tx = 1;

      /* A selection of so many times what was copied: that many copies */

      sel_rect(&sw->sel, &s);
      if ((s.r1 - s.r0 + 1) % g_clip.rows == 0 &&
          (s.c1 - s.c0 + 1) % g_clip.cols == 0 && g_clip.cutfrom == NULL &&
          (long)(s.r1 - s.r0 + 1) * (s.c1 - s.c0 + 1) <= 100000)
        {
          ty = (s.r1 - s.r0 + 1) / g_clip.rows;
          tx = (s.c1 - s.c0 + 1) / g_clip.cols;
        }

      r.r0 = r0;
      r.c0 = c0;
      r.r1 = r0 + g_clip.rows * ty - 1;
      r.c1 = c0 + g_clip.cols * tx - 1;
      r.r1 = r.r1 >= SH_ROWS ? SH_ROWS - 1 : r.r1;
      r.c1 = r.c1 >= SH_COLS ? SH_COLS - 1 : r.c1;

      if (g_clip.cutfrom == sw->kit.w)
        {
          struct sh_rect_s src;

          src.r0 = g_clip.r0;
          src.c0 = g_clip.c0;
          src.r1 = g_clip.r0 + g_clip.rows - 1;
          src.c1 = g_clip.c0 + g_clip.cols - 1;
          undo_rect(u, sh, &src);
          undo_rect(u, sh, &r);
          sheet_move(sh, &src, r0 - g_clip.r0, c0 - g_clip.c0, snap_cb, sw);
          g_clip.cutfrom = NULL;
          clip_free();
        }
      else
        {
          undo_rect(u, sh, &r);
          sheet_clear_rect(sh, &r, true);
          for (i = 0; i < g_clip.n * ty * tx; i++)
            {
              struct clipcell_s *cc = &g_clip.cell[i % g_clip.n];
              int y = r0 + cc->dr + (i / g_clip.n) % ty * g_clip.rows;
              int x = c0 + cc->dc + (i / g_clip.n) / ty * g_clip.cols;

              if (y >= SH_ROWS || x >= SH_COLS)
                {
                  continue;
                }

              sheet_set_fmt(sh, y, x, cc->fmt);
              if (cc->f != NULL)
                {
                  sheet_set_formula(sh, y, x,
                                    sheet_formula_dup(cc->f,
                                                      y - g_clip.r0 - cc->dr,
                                                      x - g_clip.c0 -
                                                      cc->dc));
                }
              else if (cc->input != NULL)
                {
                  sheet_set_input(sh, y, x, cc->input, false);
                  sheet_set_fmt(sh, y, x, cc->fmt);
                }
            }
        }
    }
  else
    {
      /* Text: a cell a tab, a row a line */

      char *field = malloc(SH_INPUT_MAX);
      int y = r0;
      int x = c0;
      int n = 0;
      int maxc = c0;
      size_t k;

      if (field == NULL)
        {
          undo_free(u);
          return;
        }

      /* How far it goes first, for the undo */

      r.r0 = r0;
      r.c0 = c0;
      r.r1 = r0;
      r.c1 = c0;
      for (k = 0; k < len; k++)
        {
          if (text[k] == '\t')
            {
              x++;
              maxc = x > maxc ? x : maxc;
            }
          else if (text[k] == '\n' && k + 1 < len)
            {
              y++;
              x = c0;
            }
        }

      r.r1 = y >= SH_ROWS ? SH_ROWS - 1 : y;
      r.c1 = maxc >= SH_COLS ? SH_COLS - 1 : maxc;
      undo_rect(u, sh, &r);
      y = r0;
      x = c0;
      for (k = 0; k <= len; k++)
        {
          char ch = k < len ? text[k] : '\n';

          if (ch == '\t' || ch == '\n')
            {
              field[n] = '\0';
              if (y < SH_ROWS && x < SH_COLS && (n > 0 ||
                  sheet_get(sh, y, x) != NULL))
                {
                  sheet_set_input(sh, y, x, field, false);
                }

              n = 0;
              if (ch == '\t')
                {
                  x++;
                }
              else
                {
                  y++;
                  x = c0;
                  if (k + 1 >= len)
                    {
                      break;
                    }
                }
            }
          else if (ch != '\r' && n < SH_INPUT_MAX - 1)
            {
              field[n++] = ch;
            }
        }

      free(field);
    }

  step_end(sw);
  sheet_recalc(sh);
  sw->sel.cr = r.r0;
  sw->sel.cc = r.c0;
  sw->sel.er = r.r1;
  sw->sel.ec = r.c1;
  sw->msg[0] = '\0';
}

/* Fill down (right): the first row (column) of the selection copied
 * into the rest of it; one cell selected: from the one above (at left)
 */

static void fill(struct sw_s *sw, bool down)
{
  struct sheet_s *sh = sw->sh;
  struct sh_rect_s r;
  struct sh_rect_s t;
  struct undo_s *u;
  int from;
  int i;
  int j;

  sel_rect(&sw->sel, &r);
  if (down ? r.r0 == r.r1 : r.c0 == r.c1)
    {
      if (down ? r.r0 == 0 : r.c0 == 0)
        {
          return;
        }

      from = down ? r.r0 - 1 : r.c0 - 1;
      t = r;
    }
  else
    {
      from = down ? r.r0 : r.c0;
      t = r;
      if (down)
        {
          t.r0++;
        }
      else
        {
          t.c0++;
        }
    }

  if ((long)(t.r1 - t.r0 + 1) * (t.c1 - t.c0 + 1) > 100000)
    {
      set_msg(sw, true, "Too much to fill", NULL);
      return;
    }

  u = step_begin(sw);
  undo_rect(u, sh, &t);
  for (i = down ? t.c0 : t.r0; i <= (down ? t.c1 : t.r1); i++)
    {
      struct sh_cell_s *src = down ? sheet_get(sh, from, i) :
                                     sheet_get(sh, i, from);
      char *buf = NULL;

      if (src != NULL && src->kind != SK_FORMULA && src->kind != SK_NONE)
        {
          buf = malloc(SH_INPUT_MAX);
          if (buf != NULL)
            {
              sheet_input(sh, src, buf, SH_INPUT_MAX);
            }
        }

      for (j = down ? t.r0 : t.c0; j <= (down ? t.r1 : t.c1); j++)
        {
          int y = down ? j : i;
          int x = down ? i : j;

          sheet_set_fmt(sh, y, x, src != NULL ? src->fmt : 0);
          if (src != NULL && src->kind == SK_FORMULA)
            {
              sheet_set_formula(sh, y, x,
                                sheet_formula_dup(src->f,
                                                  down ? y - from : 0,
                                                  down ? 0 : x - from));
            }
          else
            {
              sheet_set_input(sh, y, x, buf != NULL ? buf : "", false);
              sheet_set_fmt(sh, y, x, src != NULL ? src->fmt : 0);
            }

          src = down ? sheet_get(sh, from, i) : sheet_get(sh, i, from);
        }

      free(buf);
    }

  step_end(sw);
  sheet_recalc(sh);
}

/* Rows or columns: those of the selection inserted before it, or
 * deleted
 */

static void rows_cols(struct sw_s *sw, bool rows, bool insert)
{
  struct sh_rect_s r;
  struct undo_s u;
  struct undo_s inv;
  int at;
  int n;

  if (editing(sw) && !commit(sw))
    {
      return;
    }

  sel_rect(&sw->sel, &r);
  at = rows ? r.r0 : r.c0;
  n = rows ? r.r1 - r.r0 + 1 : r.c1 - r.c0 + 1;
  memset(&u, 0, sizeof(u));
  u.kind = insert ? U_INSERT : U_DELETE;
  u.rows = rows;
  u.at = at;
  u.n = n;
  u.sel = sw->sel;
  step_apply(sw, &u, &inv);
  inv.sel = sw->sel;
  redo_clear(sw);
  undo_push(sw->undo, &sw->nundo, &inv);
  sw->modified = true;
  if (sw->chart)
    {
      chart_toggle(sw);
    }
}

static void sort_sel(struct sw_s *sw, bool desc)
{
  struct sh_rect_s r;
  struct undo_s *u;

  sel_rect(&sw->sel, &r);
  if (r.r0 == r.r1)
    {
      set_msg(sw, true, "Select the rows to sort first", NULL);
      return;
    }

  u = step_begin(sw);
  undo_rect(u, sw->sh, &r);
  sheet_sort(sw->sh, &r, sw->sel.cc, desc);
  step_end(sw);
  sheet_recalc(sw->sh);
  sw->sh->alltouched = true;
}

/* Find: the next cell after the active one whose text or value has it */

static void find_next(struct sw_s *sw)
{
  struct sh_rect_s r;
  struct sh_cell_s **v;
  char *buf;
  int maxr;
  int maxc;
  int n;
  int i;
  int k;
  int start = 0;

  sheet_extent(sw->sh, &maxr, &maxc);
  if (sw->find[0] == '\0' || maxr < 0)
    {
      return;
    }

  r.r0 = 0;
  r.c0 = 0;
  r.r1 = maxr;
  r.c1 = maxc;
  v = sheet_cells_in(sw->sh, &r, &n);
  buf = malloc(SH_INPUT_MAX);
  if (buf == NULL)
    {
      free(v);
      return;
    }

  for (i = 0; i < n; i++)
    {
      if (SH_ROW(v[i]->key) > sw->sel.cr ||
          (SH_ROW(v[i]->key) == sw->sel.cr && SH_COL(v[i]->key) > sw->sel.cc))
        {
          start = i;
          break;
        }
    }

  for (k = 0; k < n; k++)
    {
      struct sh_cell_s *c = v[(start + k) % n];
      bool hit;

      sheet_display(sw->sh, c, 15, buf, SH_INPUT_MAX);
      hit = strcasestr(buf, sw->find) != NULL;
      if (!hit)
        {
          sheet_input(sw->sh, c, buf, SH_INPUT_MAX);
          hit = strcasestr(buf, sw->find) != NULL;
        }

      if (hit && c->kind != SK_NONE)
        {
          select_to(sw, SH_ROW(c->key), SH_COL(c->key), false);
          set_msg(sw, false, "Found \"%s\"", sw->find);
          break;
        }
    }

  if (k == n)
    {
      set_msg(sw, true, "\"%s\" is not in the sheet", sw->find);
    }

  free(buf);
  free(v);
}

/****************************************************************************
 * Private Functions: files
 ****************************************************************************/

static const char *home_dir(void)
{
  const char *home = getenv("HOME");

#ifdef CONFIG_LIBC_HOMEDIR
  if (home == NULL)
    {
      home = CONFIG_LIBC_HOMEDIR;
    }
#endif

  if (home == NULL || access(home, F_OK) != 0)
    {
      home = access("/mnt/sd0", F_OK) == 0 ? "/mnt/sd0" : "/tmp";
    }

  return home;
}

static bool csv_path(const char *path)
{
  const char *ext = strrchr(path, '.');

  return ext != NULL && (strcasecmp(ext, ".csv") == 0 ||
                         strcasecmp(ext, ".tsv") == 0);
}

static int load(struct sw_s *sw, const char *path)
{
  int ret = csv_path(path) ? sheet_load_csv(sw->sh, path) :
                             sheet_load_native(sw->sh, path);

  if (ret < 0)
    {
      sheet_clear_all(sw->sh);
      return ret;
    }

  strlcpy(sw->path, path, sizeof(sw->path));
  sw->modified = false;
  return 0;
}

static void do_save(struct sw_s *sw, const char *path)
{
  char *p = malloc(PATH_MAX);
  int ret;

  if (p == NULL)
    {
      return;
    }

  strlcpy(p, path, PATH_MAX);
  if (strrchr(p, '.') == NULL || strrchr(p, '.') < strrchr(p, '/'))
    {
      strlcat(p, ".sheet", PATH_MAX);
    }

  ret = csv_path(p) ? sheet_save_csv(sw->sh, p) :
                      sheet_save_native(sw->sh, p);
  if (ret < 0)
    {
      snprintf(sw->msg, sizeof(sw->msg), "Cannot save: %s",
               strerror(-ret));
      sw->msgerr = true;
      free(p);
      return;
    }

  strlcpy(sw->path, p, sizeof(sw->path));
  free(p);
  sw->modified = false;
  set_msg(sw, false, csv_path(sw->path) ? "Saved (values only: CSV)" :
          "Saved", NULL);
}

static void ask(struct sw_s *sw, int id)
{
  static const char * const saving[3] =
  {
    "Save", "Cancel", "Don't Save"
  };

  static const char * const finding[2] =
  {
    "Find", "Cancel"
  };

  char *p = malloc(PATH_MAX);
  char title[96];

  if (p == NULL)
    {
      return;
    }

  if (editing(sw) && id != P_CLOSE)
    {
      commit(sw);
    }

  switch (id)
    {
      case P_CLOSE:
        snprintf(title, sizeof(title), "Save changes to \"%s\"?",
                 base_name(sw));
        kit_dialog(&sw->kit, P_CLOSE, KIT_DLG_CONFIRM, title,
                   "If you don't save, the changes are lost.", saving, 3);
        break;

      case P_SAVEAS:
        snprintf(p, PATH_MAX, "%s/%s", home_dir(), sw->path[0] != '\0' ?
                 base_name(sw) : "Untitled.sheet");
        kit_dialog(&sw->kit, P_SAVEAS, KIT_DLG_SAVE, "Save As", p, NULL, 0);
        break;

      case P_OPEN:
        kit_dialog(&sw->kit, P_OPEN, KIT_DLG_OPEN, "Open", home_dir(), NULL,
                   0);
        break;

      case P_FIND:
        kit_dialog(&sw->kit, P_FIND, KIT_DLG_PROMPT, "Find", sw->find,
                   finding, 2);
        break;
    }

  free(p);
}

static void save(struct sw_s *sw)
{
  if (editing(sw) && !commit(sw))
    {
      return;
    }

  if (sw->path[0] == '\0')
    {
      ask(sw, P_SAVEAS);
    }
  else
    {
      do_save(sw, sw->path);
    }
}

static void reset_view(struct sw_s *sw)
{
  int i;

  for (i = 0; i < sw->nundo; i++)
    {
      undo_free(&sw->undo[i]);
    }

  redo_clear(sw);
  sw->nundo = 0;
  sw->top = 0;
  sw->left = 0;
  memset(&sw->sel, 0, sizeof(sw->sel));
  sw->sel.pr = -1;
  sw->dselvalid = false;
  sw->chart = false;
  kit_surface_close(&sw->surf);
  sw->surfok = false;
  sw->mode = MODE_READY;
  sw->edw = 0;
}

static void answer(struct sw_s *sw, int id, int ans, const char *text)
{
  struct desk_win_s *w = sw->kit.w;

  switch (id)
    {
      case P_CLOSE:
        if (ans == KIT_ANS_ALT)
          {
            sw->modified = false;
            desk_win_close(w);
            return;
          }

        if (ans == KIT_ANS_OK)
          {
            if (sw->path[0] == '\0')
              {
                ask(sw, P_SAVEAS);
                sw->find[sizeof(sw->find) - 1] = 1;   /* Close after */
                return;
              }

            do_save(sw, sw->path);
            if (!sw->modified)
              {
                desk_win_close(w);
                return;
              }
          }
        break;

      case P_SAVEAS:
        if (ans == KIT_ANS_OK && text[0] != '\0')
          {
            do_save(sw, text);
            if (!sw->modified && sw->find[sizeof(sw->find) - 1] == 1)
              {
                desk_win_close(w);
                return;
              }
          }

        sw->find[sizeof(sw->find) - 1] = 0;
        break;

      case P_OPEN:
        if (ans == KIT_ANS_OK && text[0] != '\0')
          {
            int maxr;
            int maxc;

            sheet_extent(sw->sh, &maxr, &maxc);
            if (sw->modified || sw->path[0] != '\0' || maxr >= 0)
              {
                desk_sheet_open(text);
                return;
              }

            reset_view(sw);
            if (load(sw, text) < 0)
              {
                set_msg(sw, true, "Cannot open %s", text);
              }

            sw->sh->alltouched = true;
          }
        break;

      case P_FIND:
        if (ans == KIT_ANS_OK && text[0] != '\0')
          {
            strlcpy(sw->find, text, sizeof(sw->find) - 1);
            find_next(sw);
          }
        break;
    }

  /* Drawn when woken: not deep in the dialog's calls (their stack) */

  sw->later = true;
  desk_wake(w);
}

/****************************************************************************
 * Private Functions: keys
 ****************************************************************************/

/* Ctrl-arrow: to the edge of the data (the next change from empty to not,
 * or the sheet's edge)
 */

static void jump(struct sw_s *sw, int dr, int dc, int *r, int *c)
{
  int y = *r;
  int x = *c;
  bool here = !cell_empty(sw->sh, y, x);
  bool next;

  if (y + dr < 0 || y + dr >= SH_ROWS || x + dc < 0 || x + dc >= SH_COLS)
    {
      return;
    }

  next = !cell_empty(sw->sh, y + dr, x + dc);
  if (here && next)
    {
      while (y + dr >= 0 && y + dr < SH_ROWS && x + dc >= 0 &&
             x + dc < SH_COLS && !cell_empty(sw->sh, y + dr, x + dc))
        {
          y += dr;
          x += dc;
        }
    }
  else
    {
      y += dr;
      x += dc;
      while (y + dr >= 0 && y + dr < SH_ROWS && x + dc >= 0 &&
             x + dc < SH_COLS && cell_empty(sw->sh, y, x))
        {
          y += dr;
          x += dc;
        }
    }

  *r = y;
  *c = x;
}

static void move_key(struct sw_s *sw, int key, bool shift, bool ctrl)
{
  struct geo_s g;
  int r = shift ? sw->sel.er : sw->sel.cr;
  int c = shift ? sw->sel.ec : sw->sel.cc;
  int maxr;
  int maxc;

  geo(sw, &g);
  switch (key)
    {
      case KEY_UP:
      case KEY_DOWN:
      case KEY_LEFT:
      case KEY_RIGHT:
        {
          int dr = key == KEY_UP ? -1 : key == KEY_DOWN ? 1 : 0;
          int dc = key == KEY_LEFT ? -1 : key == KEY_RIGHT ? 1 : 0;

          if (ctrl)
            {
              jump(sw, dr, dc, &r, &c);
            }
          else
            {
              r += dr;
              c += dc;
            }
        }
        break;

      case KEY_PGUP:
      case KEY_PGDN:
        {
          int page = g.full > 1 ? g.full - 1 : 1;
          int d = key == KEY_PGUP ? -page : page;

          r += d;
          r = r < 0 ? 0 : r >= SH_ROWS ? SH_ROWS - 1 : r;
          scroll_to(sw, sw->top + d, sw->left);
        }
        break;

      case KEY_HOME:
        c = 0;
        r = ctrl ? 0 : r;
        break;

      case KEY_END:
        sheet_extent(sw->sh, &maxr, &maxc);
        if (ctrl && maxr >= 0)
          {
            r = maxr;
            c = maxc;
          }
        break;
    }

  select_to(sw, r, c, shift);
}

static void grid_key(struct sw_s *sw, int k)
{
  bool shift = (k & KEY_SHIFT) != 0;
  bool ctrl = (k & KEY_CTRL) != 0;
  int key = k & KEY_MASK;
  bool arrow = key == KEY_UP || key == KEY_DOWN || key == KEY_LEFT ||
               key == KEY_RIGHT;
  bool enter = key == '\r' || key == '\n';
  int tabc = sw->tabc;

  /* A row typed with Tabs: Enter goes back to where it began */

  if (key == '\t')
    {
      sw->tabc = tabc >= 0 ? tabc : sw->sel.cc;
    }
  else if (enter || arrow || key == KEY_ESC ||
           (!editing(sw) && (key < 0x20 || key > 0xff || key == 0x7f ||
                             ctrl)))
    {
      sw->tabc = -1;
    }

  if (key != KEY_CTL('s'))
    {
      sw->msg[0] = '\0';
    }

  /* Typing into the cell */

  if (editing(sw))
    {
      if (arrow && can_point(sw) && !ctrl)
        {
          int r = sw->pointing ? sw->sel.pr : sw->sel.cr;
          int c = sw->pointing ? sw->sel.pc : sw->sel.cc;
          int dr = key == KEY_UP ? -1 : key == KEY_DOWN ? 1 : 0;
          int dc = key == KEY_LEFT ? -1 : key == KEY_RIGHT ? 1 : 0;

          if (shift && sw->pointing)
            {
              point_at(sw, r, c, sw->sel.pr2 + dr, sw->sel.pc2 + dc);
            }
          else
            {
              point_at(sw, r + dr, c + dc, r + dr, c + dc);
            }

          return;
        }

      if (key == KEY_ESC)
        {
          cancel(sw);
          return;
        }

      if (key == '\r' || key == '\n' || key == '\t' ||
          (arrow && (sw->mode == MODE_ENTER || key == KEY_UP ||
                     key == KEY_DOWN)))
        {
          if (!commit(sw))
            {
              return;
            }

          if (key == '\t')
            {
              select_to(sw, sw->sel.cr, sw->sel.cc + (shift ? -1 : 1),
                        false);
            }
          else if (enter)
            {
              select_to(sw, sw->sel.cr + 1, tabc >= 0 ? tabc : sw->sel.cc,
                        false);
            }
          else
            {
              move_key(sw, key, false, false);
            }

          return;
        }

      if (key == KEY_F2)
        {
          sw->mode = sw->mode == MODE_EDIT ? MODE_ENTER : MODE_EDIT;
          point_end(sw);
          return;
        }

      point_end(sw);
      if (ui_field_key(&sw->field, key))
        {
          kit_invalidate(&sw->kit, S_FIELD);
        }

      return;
    }

  switch (key)
    {
      case KEY_UP:
      case KEY_DOWN:
      case KEY_LEFT:
      case KEY_RIGHT:
      case KEY_PGUP:
      case KEY_PGDN:
      case KEY_HOME:
      case KEY_END:
        move_key(sw, key, shift, ctrl);
        break;

      case '\r':
      case '\n':
        select_to(sw, sw->sel.cr + 1, tabc >= 0 ? tabc : sw->sel.cc, false);
        break;

      case '\t':
        select_to(sw, sw->sel.cr, sw->sel.cc + (shift ? -1 : 1), false);
        break;

      case KEY_F2:
        {
          char *buf = malloc(SH_INPUT_MAX);

          if (buf != NULL)
            {
              sheet_input(sw->sh, sheet_get(sw->sh, sw->sel.cr, sw->sel.cc),
                          buf, SH_INPUT_MAX);
              edit_begin(sw, MODE_EDIT, buf);
              free(buf);
            }
        }
        break;

      case KEY_DEL:
      case 0x7f:
      case 0x08:
        clear_sel(sw);
        break;

      case KEY_ESC:
        if (g_clip.cutfrom == sw->kit.w)
          {
            g_clip.cutfrom = NULL;
          }
        break;

      case KEY_CTL('a'):
        sw->sel.cr = 0;
        sw->sel.cc = 0;
        sw->sel.er = SH_ROWS - 1;
        sw->sel.ec = SH_COLS - 1;
        break;

      case KEY_CTL('b'):
        format_sel(sw, SF_BOLD, (sheet_fmt(sw->sh, sw->sel.cr, sw->sel.cc) &
                                 SF_BOLD) ? 0 : SF_BOLD, 0);
        break;

      case KEY_CTL('c'):
        copy_sel(sw, false);
        break;

      case KEY_CTL('x'):
        copy_sel(sw, true);
        break;

      case KEY_CTL('v'):
        paste(sw);
        break;

      case KEY_CTL('d'):
        fill(sw, true);
        break;

      case KEY_CTL('r'):
        fill(sw, false);
        break;

      case KEY_CTL('z'):
        undo_do(sw, false);
        break;

      case KEY_CTL('y'):
        undo_do(sw, true);
        break;

      case KEY_CTL('f'):
        ask(sw, P_FIND);
        break;

      case KEY_CTL('s'):
        save(sw);
        break;

      case KEY_CTL('o'):
        ask(sw, P_OPEN);
        break;

      case KEY_CTL('n'):
        desk_sheet_open(NULL);
        return;

      default:
        if (key >= 0x20 && key <= 0xff && key != 0x7f && !ctrl)
          {
            char s[2];

            s[0] = (char)key;
            s[1] = '\0';
            edit_begin(sw, MODE_ENTER, s);
          }
        break;
    }
}

/****************************************************************************
 * Private Functions: the mouse
 ****************************************************************************/

static void context_menu(struct sw_s *sw, int x, int y)
{
  uint32_t off = 0;

  if (g_clip.rows == 0 && desk_clip_get(&(size_t){0}) == NULL)
    {
      off |= 1 << I_PASTE;
    }

  kit_menu(&sw->kit, M_CONTEXT, x, y, g_context,
           sizeof(g_context) / sizeof(g_context[0]), off);
}

static void grid_mouse(struct sw_s *sw, int x, int y, uint8_t buttons)
{
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(sw->buttons & NX_MOUSE_LEFTBUTTON);
  bool rpress = (buttons & NX_MOUSE_RIGHTBUTTON) &&
                !(sw->buttons & NX_MOUSE_RIGHTBUTTON);
  struct geo_s g;
  int cx = 0;
  int c;
  int r;

  sw->buttons = buttons;
  sw->tabc = press || rpress ? -1 : sw->tabc;
  geo(sw, &g);
  c = x >= g.cx0 ? col_at(sw, &g, x, &cx) : sw->left - 1;
  r = row_at(sw, &g, y);

  /* Dragging */

  if (sw->drag != D_NONE)
    {
      if (!(buttons & NX_MOUSE_LEFTBUTTON))
        {
          sw->drag = D_NONE;
          return;
        }

      /* Beyond the cells: they scroll a step */

      if (sw->drag != D_WIDTH)
        {
          if (y >= g.y1 && sw->drag != D_COLS)
            {
              scroll_to(sw, sw->top + 1, sw->left);
            }
          else if (y < g.cy0 && sw->drag != D_COLS && sw->top > 0)
            {
              scroll_to(sw, sw->top - 1, sw->left);
            }

          if (x >= g.x1 && sw->drag != D_ROWS)
            {
              scroll_to(sw, sw->top, sw->left + 1);
            }
          else if (x < g.cx0 && sw->drag != D_ROWS && sw->left > 0)
            {
              scroll_to(sw, sw->top, sw->left - 1);
            }

          geo(sw, &g);
          c = x >= g.cx0 ? col_at(sw, &g, x < g.x1 ? x : g.x1 - 1, &cx) :
              sw->left;
          r = row_at(sw, &g, y < g.cy0 ? g.cy0 : y >= g.y1 ? g.y1 - 1 : y);
          r = r < 0 ? 0 : r >= SH_ROWS ? SH_ROWS - 1 : r;
          c = c < 0 ? 0 : c >= SH_COLS ? SH_COLS - 1 : c;
        }

      switch (sw->drag)
        {
          case D_CELLS:
            sw->sel.er = r;
            sw->sel.ec = c;
            break;

          case D_COLS:
            sw->sel.ec = c;
            break;

          case D_ROWS:
            sw->sel.er = r;
            break;

          case D_POINT:
            point_at(sw, sw->sel.pr, sw->sel.pc, r, c);
            break;

          case D_WIDTH:
            {
              int w = sw->dragw + x - sw->dragx;
              int old = colw(sw, sw->dragcol);
              int x0 = col_x(sw, &g, sw->dragcol);

              w = w < SH_COLW_MIN ? SH_COLW_MIN : w > SH_COLW_MAX ?
                  SH_COLW_MAX : w;
              if (w != old)
                {
                  struct nx_gpuop_s *op;
                  int from = x0 + (w < old ? w : old);

                  /* What is right of the column moves with a COPY; the
                   * column and what comes in at the right are drawn
                   */

                  sw->sh->colw[sw->dragcol] = w == SH_COLW ? 0 : w;
                  sw->modified = true;
                  if (x0 + old < g.x1 && x0 + w < g.x1)
                    {
                      ui_flush(hwnd_of(sw), sw->o, 1);
                      op = ops_add(sw->o);
                      op->op = NX_GPUOP_COPY;
                      op->y = g.y0;
                      op->h = g.y1 - g.y0;
                      op->sy = g.y0;
                      op->sx = x0 + old;
                      op->x = x0 + w;
                      op->w = g.x1 - (x0 + (w > old ? w : old));
                      grid_rect(sw, &g, x0, g.y0, from + 1 + (w > old ?
                                w - old : 0), g.y1);
                      if (w < old)
                        {
                          grid_rect(sw, &g, g.x1 - (old - w), g.y0, g.x1,
                                    g.y1);
                        }

                      chart_moved(sw, &g, w - old, 0);
                    }
                  else
                    {
                      grid_rect(sw, &g, x0, g.y0, g.x1, g.y1);
                    }

                  /* Text running on from the left, over the column */

                  grid_rect(sw, &g, g.cx0, g.cy0, x0, g.y1);
                  sw->edw = 0;
                }
            }
            break;
        }

      refresh(sw);
      return;
    }

  if (rpress && y >= g.cy0 && x >= g.cx0 && r < SH_ROWS && c < SH_COLS)
    {
      struct sh_rect_s s;

      sel_rect(&sw->sel, &s);
      if (editing(sw) && !commit(sw))
        {
          refresh(sw);
          return;
        }

      if (r < s.r0 || r > s.r1 || c < s.c0 || c > s.c1)
        {
          select_to(sw, r, c, false);
        }

      refresh(sw);
      context_menu(sw, x, y);
      return;
    }

  if (!press)
    {
      return;
    }

  /* The chart: its close button, bars or line, else nothing */

  {
    int chx;
    int chy;
    int chw;
    int chh;

    if (chart_rect(sw, &g, &chx, &chy, &chw, &chh) && x >= chx &&
        x < chx + chw && y >= chy && y < chy + chh)
      {
        if (y < chy + CHART_TITLE && x >= chx + chw - 28)
          {
            chart_toggle(sw);
          }
        else if (y < chy + CHART_TITLE && x >= chx + chw - 100)
          {
            sw->line = !sw->line;
            chart_redraw(sw);
          }

        refresh(sw);
        return;
      }
  }

  /* Pointing at a cell for a formula */

  if (editing(sw) && y >= g.cy0 && x >= g.cx0 && c < SH_COLS &&
      r < SH_ROWS && (r != sw->sel.cr || c != sw->sel.cc) && can_point(sw))
    {
      point_at(sw, r, c, r, c);
      sw->drag = D_POINT;
      refresh(sw);
      return;
    }

  if (editing(sw) && !commit(sw))
    {
      refresh(sw);
      return;
    }

  if (y < g.cy0 && x < g.cx0)
    {
      /* The corner: all */

      sw->sel.cr = 0;
      sw->sel.cc = 0;
      sw->sel.er = SH_ROWS - 1;
      sw->sel.ec = SH_COLS - 1;
    }
  else if (y < g.cy0)
    {
      /* A column's edge (either side of it) to resize, else the column */

      int edge = -1;

      if (c < SH_COLS && x >= cx + colw(sw, c) - EDGE)
        {
          edge = c;
        }
      else if (c > sw->left && x < cx + EDGE)
        {
          edge = c - 1;
        }

      if (edge >= 0)
        {
          sw->drag = D_WIDTH;
          sw->dragcol = edge;
          sw->dragx = x;
          sw->dragw = colw(sw, edge);
        }
      else if (c < SH_COLS)
        {
          sw->sel.cr = sw->top;
          sw->sel.cc = c;
          sw->sel.er = SH_ROWS - 1;
          sw->sel.ec = c;
          sw->sel.cr = 0;
          sw->drag = D_COLS;
        }
    }
  else if (x < g.cx0)
    {
      if (r < SH_ROWS)
        {
          sw->sel.cr = r;
          sw->sel.cc = 0;
          sw->sel.er = r;
          sw->sel.ec = SH_COLS - 1;
          sw->drag = D_ROWS;
        }
    }
  else if (r < SH_ROWS && c < SH_COLS)
    {
      uint32_t now = desk_ms();

      if (r == sw->clickr && c == sw->clickc &&
          now - sw->clickms < DBLCLICK_MS)
        {
          /* A double click edits the cell */

          sw->clickms = 0;
          grid_key(sw, KEY_F2);
        }
      else
        {
          sw->clickms = now;
          sw->clickr = r;
          sw->clickc = c;
          select_to(sw, r, c, false);
          sw->drag = D_CELLS;
        }
    }

  sw->msg[0] = '\0';
  refresh(sw);
}

/****************************************************************************
 * Private Functions: the kit's events
 ****************************************************************************/

static void menu_pick(struct sw_s *sw, int menu, int item)
{
  if (item < 0)
    {
      return;
    }

  if (menu == M_COLOUR || menu == M_FILL)
    {
      format_sel(sw, menu == M_COLOUR ? 0xf000 : 0xf0000,
                 menu == M_COLOUR ? (uint32_t)item << 12 :
                                    (uint32_t)item << 16, 0);
      sheet_recalc(sw->sh);
      return;
    }

  if (menu == M_MORE)
    {
      static const int map[] =
      {
        -1, -1, -1, I_INSROW, I_DELROW, I_INSCOL, I_DELCOL, -1, I_SORTUP,
        I_SORTDOWN, -1, I_FILLDOWN, I_FILLRIGHT
      };

      if (item == J_SAVEAS)
        {
          ask(sw, P_SAVEAS);
          return;
        }

      if (item == J_FIND)
        {
          ask(sw, P_FIND);
          return;
        }

      item = item < (int)(sizeof(map) / sizeof(map[0])) ? map[item] : -1;
    }

  if (editing(sw) && !commit(sw))
    {
      return;
    }

  switch (item)
    {
      case I_CUT:
        copy_sel(sw, true);
        break;

      case I_COPY:
        copy_sel(sw, false);
        break;

      case I_PASTE:
        paste(sw);
        break;

      case I_CLEAR:
        clear_sel(sw);
        break;

      case I_INSROW:
      case I_DELROW:
      case I_INSCOL:
      case I_DELCOL:
        rows_cols(sw, item == I_INSROW || item == I_DELROW,
                  item == I_INSROW || item == I_INSCOL);
        break;

      case I_FILLDOWN:
      case I_FILLRIGHT:
        fill(sw, item == I_FILLDOWN);
        break;

      case I_SORTUP:
      case I_SORTDOWN:
        sort_sel(sw, item == I_SORTDOWN);
        break;
    }
}

static void sheet_event(struct kit_s *k, struct kit_widget_s *wg, int ev)
{
  struct desk_win_s *w = k->w;
  struct sw_s *sw = w->priv;
  int id = wg != NULL ? wg->id : 0;

  switch (ev)
    {
      case KIT_EV_CLICK:
        switch (id)
          {
            case S_NEW:
              desk_sheet_open(NULL);
              return;

            case S_OPEN:
              ask(sw, P_OPEN);
              break;

            case S_SAVE:
              save(sw);
              break;

            case S_UNDO:
            case S_REDO:
              if (editing(sw))
                {
                  cancel(sw);
                }

              undo_do(sw, id == S_REDO);
              break;

            case S_BOLD:
              format_sel(sw, SF_BOLD, wg->value ? 0 : SF_BOLD, 0);
              break;

            case S_ALEFT:
            case S_ACENTRE:
            case S_ARIGHT:
              format_sel(sw, 0x300, (uint32_t)(wg->value ? SF_AUTO :
                                     id - S_ALEFT + SF_LEFT) << 8, 0);
              break;

            case S_DECLESS:
            case S_DECMORE:
              format_sel(sw, 0, 0, id == S_DECMORE ? 1 : -1);
              break;

            case S_COLOUR:
            case S_FILL:
              kit_menu(k, id == S_COLOUR ? M_COLOUR : M_FILL, wg->x,
                       wg->y + wg->h + 2, id == S_COLOUR ? g_colours :
                       g_fills, SF_COLOURS, 0);
              return;

            case S_CHART:
              chart_toggle(sw);
              break;

            case S_MENU:
              kit_menu(k, M_MORE, wg->x + wg->w - 170, wg->y + wg->h + 2,
                       g_more, sizeof(g_more) / sizeof(g_more[0]), 0);
              return;
          }

        kit_focus(k, S_GRID);
        refresh(sw);
        break;

      case KIT_EV_CHANGE:
        if (id == S_VSCROLL)
          {
            scroll_to(sw, wg->value, sw->left);
            refresh(sw);
          }
        else if (id == S_HSCROLL)
          {
            scroll_to(sw, sw->top, wg->value);
            refresh(sw);
          }
        else if (id == S_NUMFMT)
          {
            static const uint8_t dec[] =
            {
              0, 2, 2, 2, 0, 0, 0
            };

            format_sel(sw, 0xff, SF_MAKE(wg->value, dec[wg->value], 0),
                       0);
            kit_focus(k, S_GRID);
            refresh(sw);
          }
        else if (id == S_FIELD)
          {
            /* Typing in the formula bar: the cell shows it too */

            if (!editing(sw))
              {
                sw->mode = MODE_EDIT;
                sw->caret = false;
              }

            point_end(sw);
            refresh(sw);
          }
        break;

      case KIT_EV_ACTIVATE:
        if (id == S_FIELD)
          {
            if (editing(sw) && commit(sw))
              {
                select_to(sw, sw->sel.cr + 1, sw->sel.cc, false);
              }

            kit_focus(k, S_GRID);
            refresh(sw);
          }
        break;

      case KIT_EV_MOUSE:
        if (id == S_GRID && !kit_dialog_open(k))
          {
            grid_mouse(sw, k->mx, k->my, k->mbuttons);
          }
        break;

      case KIT_EV_KEY:
        if (kit_dialog_open(k))
          {
            break;
          }

        if (id == S_FIELD)
          {
            int key = k->key & KEY_MASK;

            if (key == KEY_ESC)
              {
                cancel(sw);
                kit_focus(k, S_GRID);
              }
            else if (key == '\t' || key == KEY_UP || key == KEY_DOWN)
              {
                if (!editing(sw) || commit(sw))
                  {
                    kit_focus(k, S_GRID);
                    grid_key(sw, k->key);
                  }
              }
            else
              {
                break;
              }

            k->key = 0;
            refresh(sw);
            break;
          }

        grid_key(sw, k->key);
        k->key = 0;
        if (!w->closing && !kit_dialog_open(k))
          {
            refresh(sw);
          }
        break;

      case KIT_EV_MENU:
        menu_pick(sw, k->key, k->arg);
        if (!w->closing)
          {
            refresh(sw);
          }
        break;

      case KIT_EV_DIALOG:
        answer(sw, k->key, k->arg, k->answer);
        break;

      default:
        break;
    }
}

/* Icons of the tool bar, drawn in the button's colour */

static void icon_paint(struct sw_s *sw, struct kit_widget_s *wg,
                       struct desk_ops_s *o)
{
  int look = kit_look(&sw->kit, wg) & KIT_LOOK_MASK;
  uint16_t k = look == KIT_LOOK_DISABLED ? g_theme.text_faint :
               wg->style == KIT_BTN_TOGGLE && wg->value ?
               g_theme.accent_text : g_theme.control_text;
  int x = wg->x + (wg->w - 16) / 2;
  int y = wg->y + (wg->h - 16) / 2;
  int i;

  switch (wg->id)
    {
      case S_SAVE:
        ops_rrect(o, x + 1, y + 1, 14, 14, LUT_R3, 3, k, 255);
        ops_fill(o, x + 4, y + 3, 7, 4, g_theme.control);
        ops_fill(o, x + 4, y + 9, 8, 6, g_theme.control);
        ops_fill(o, x + 5, y + 11, 6, 1, k);
        break;

      case S_UNDO:
      case S_REDO:
        {
          bool undo = wg->id == S_UNDO;

          ui_triangle(o, undo ? x + 3 : x + 13, y + 6, 8, undo ? 2 : 3, k,
                      255);
          ops_fill(o, undo ? x + 5 : x + 3, y + 5, 8, 2, k);
          ops_fill(o, undo ? x + 12 : x + 2, y + 5, 2, 8, k);
          ops_fill(o, undo ? x + 6 : x + 2, y + 12, 8, 2, k);
        }
        break;

      case S_BOLD:
        ops_text_centre(o, &g_desk_font_title, wg->x + wg->w / 2,
                        wg->y + (wg->h - text_height(&g_desk_font_title)) /
                        2, "B", k, 255);
        break;

      case S_ALEFT:
      case S_ACENTRE:
      case S_ARIGHT:
        for (i = 0; i < 4; i++)
          {
            int lw = i % 2 == 0 ? 14 : 9;
            int lx = wg->id == S_ALEFT ? x + 1 : wg->id == S_ARIGHT ?
                     x + 15 - lw : x + 8 - lw / 2;

            ops_fill(o, lx, y + 2 + i * 4, lw, 2, k);
          }
        break;

      case S_DECLESS:
      case S_DECMORE:
        {
          const struct desk_font_s *f = &g_desk_font_small;
          const char *s = wg->id == S_DECLESS ? ".0" : ".00";

          ops_text_centre(o, f, wg->x + wg->w / 2, y - 1, s, k, 255);
          ops_fill(o, x + 4, y + 13, 9, 1, k);
          ui_triangle(o, wg->id == S_DECLESS ? x + 3 : x + 13, y + 13, 5,
                      wg->id == S_DECLESS ? 2 : 3, k, 255);
        }
        break;

      case S_COLOUR:
        ops_text_centre(o, &g_desk_font_uib, wg->x + wg->w / 2, y - 1, "A",
                        k, 255);
        ops_fill(o, x + 1, y + 13, 14, 3,
                 sw->lastfg ? text_colour(sw->lastfg) : g_theme.text_dim);
        break;

      case S_FILL:
        ops_rrect(o, x + 2, y + 1, 12, 10, LUT_R3, 3, k, 255);
        ops_rrect(o, x + 3, y + 2, 10, 8, LUT_R3, 2,
                  sw->lastbg ? fill_colour(sw->lastbg) : g_theme.control,
                  255);
        ops_fill(o, x + 1, y + 13, 14, 3,
                 sw->lastbg ? fill_colour(sw->lastbg) : g_theme.text_faint);
        break;

      case S_CHART:
        ops_fill(o, x + 1, y + 15, 14, 1, k);
        ops_fill(o, x + 2, y + 8, 3, 7, k);
        ops_fill(o, x + 7, y + 3, 3, 12, k);
        ops_fill(o, x + 12, y + 10, 3, 5, k);
        break;
    }
}

/* The kit's draw function: the grid where the kit draws it (an expose, a
 * dialog gone), the tool bar's icons
 */

static void sheet_paint(struct kit_s *k, struct kit_widget_s *wg, int x0,
                        int y0, int x1, int y1)
{
  struct sw_s *sw = k->w->priv;
  struct geo_s g;

  if (wg->id == S_GRID)
    {
      geo(sw, &g);
      grid_rect(sw, &g, x0, y0, x1, y1);
      if (sw->edw > 0 || editing(sw))
        {
          editor_paint(sw, &g, x0, y0, x1, y1);
        }

      ops_draw(hwnd_of(sw), sw->o);
      return;
    }

  icon_paint(sw, wg, kit_ops(k, 24));
}

/****************************************************************************
 * Private Functions: the window
 ****************************************************************************/

static void sheet_draw(struct desk_win_s *w)
{
  struct sw_s *sw = w->priv;

  if (sw == NULL)
    {
      return;
    }

  sw->dselvalid = false;
  sw->edw = 0;
  scrollbars_sync(sw);
  formula_sync(sw);
  status_sync(sw);
  toolbar_sync(sw);
  kit_redraw(&sw->kit);
  sw->dsel = sw->sel;
  sw->dselvalid = true;
}

static void sheet_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct sw_s *sw = w->priv;

  if (sw != NULL)
    {
      kit_mouse(&sw->kit, x, y, buttons);
    }
}

static void sheet_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct sw_s *sw = w->priv;

  if (sw != NULL)
    {
      kit_key(&sw->kit, ch, n);
    }
}

static void sheet_tick(struct desk_win_s *w)
{
  struct sw_s *sw = w->priv;
  bool caret;

  if (sw == NULL)
    {
      return;
    }

  caret = editing(sw) && w->active && (!sw->caret || desk_frozen());
  if (editing(sw) && caret != sw->caret)
    {
      sw->caret = caret;
      editor_update(sw);
      grid_send(sw);
    }

  kit_tick(&sw->kit);
}

static void sheet_resize(struct desk_win_s *w)
{
  struct sw_s *sw = w->priv;

  kit_layout(&sw->kit);
  kit_resize(&sw->kit);
  sw->surfok = false;
  sheet_draw(w);
}

static void sheet_expose(struct desk_win_s *w, int x, int y, int cw, int ch)
{
  struct sw_s *sw = w->priv;

  if (sw != NULL)
    {
      kit_expose(&sw->kit, x, y, cw, ch);
    }
}

static void sheet_focus(struct desk_win_s *w)
{
  struct sw_s *sw = w->priv;

  if (sw != NULL)
    {
      sw->caret = w->active;
      kit_activate(&sw->kit);
      if (editing(sw))
        {
          editor_update(sw);
          grid_send(sw);
        }
    }
}

static void sheet_wake(struct desk_win_s *w)
{
  struct sw_s *sw = w->priv;

  if (sw == NULL)
    {
      return;
    }

  if (sw->chart && sw->surf.lost)
    {
      chart_redraw(sw);
      grid_send(sw);
    }

  if (sw->later)
    {
      sw->later = false;
      refresh(sw);
    }
}

static bool sheet_canclose(struct desk_win_s *w)
{
  struct sw_s *sw = w->priv;

  if (sw == NULL || !sw->modified)
    {
      return true;
    }

  if (!kit_dialog_open(&sw->kit))
    {
      desk_win_activate(w);
      ask(sw, P_CLOSE);
    }

  return false;
}

static void sheet_close(struct desk_win_s *w)
{
  struct sw_s *sw = w->priv;
  int i;

  if (sw == NULL)
    {
      return;
    }

  for (i = 0; i < UNDO_MAX; i++)
    {
      undo_free(&sw->undo[i]);
      undo_free(&sw->redo[i]);
    }

  if (g_clip.cutfrom == w)
    {
      g_clip.cutfrom = NULL;
    }

  kit_surface_close(&sw->surf);
  kit_dialog_close(&sw->kit);
  sheet_free(sw->sh);
  free(sw->o);
  sw->sh = NULL;
  sw->o = NULL;
}

static double desk_now(void)
{
  struct tm tm;

  desk_localtime(&tm);
  return sheet_serial(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday) +
         (tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec) / 86400.0;
}

static void icon_button(struct kit_s *k, int id, int icon, int style,
                        const char *tip)
{
  kit_button(k, id, NULL, icon, style);
  kit_tip(k, tip);
}

static void gap(struct kit_s *k)
{
  kit_spacer(k, 0);
  kit_size(k, 6, 0);
}

static void sheet_build(struct desk_win_s *w, struct sw_s *sw)
{
  struct kit_s *k = &sw->kit;
  struct kit_widget_s *wg;

  kit_init(k, w, sw->wg, NWG, sheet_event);
  k->priv = sw;
  k->draw = sheet_paint;

  kit_col(k, KIT_BOX_WINDOW);
    wg = kit_row(k, KIT_BOX_BAR);
    wg->gap = 3;
      icon_button(k, S_NEW, KIT_ICON_FILE, KIT_BTN_FLAT,
                  "A new spreadsheet (Ctrl-N)");
      icon_button(k, S_OPEN, KIT_ICON_FOLDER, KIT_BTN_FLAT, "Open (Ctrl-O)");
      icon_button(k, S_SAVE, KIT_ICON_CUSTOM, KIT_BTN_FLAT, "Save (Ctrl-S)");
      gap(k);
      icon_button(k, S_UNDO, KIT_ICON_CUSTOM, KIT_BTN_FLAT, "Undo (Ctrl-Z)");
      icon_button(k, S_REDO, KIT_ICON_CUSTOM, KIT_BTN_FLAT, "Redo (Ctrl-Y)");
      gap(k);
      icon_button(k, S_BOLD, KIT_ICON_CUSTOM, KIT_BTN_TOGGLE,
                  "Bold (Ctrl-B)");
      icon_button(k, S_ALEFT, KIT_ICON_CUSTOM, KIT_BTN_TOGGLE, "Left");
      icon_button(k, S_ACENTRE, KIT_ICON_CUSTOM, KIT_BTN_TOGGLE, "Centre");
      icon_button(k, S_ARIGHT, KIT_ICON_CUSTOM, KIT_BTN_TOGGLE, "Right");
      gap(k);
      kit_dropdown(k, S_NUMFMT, g_fmtitems,
                   sizeof(g_fmtitems) / sizeof(g_fmtitems[0]), 0);
      kit_size(k, 124, 0);
      kit_tip(k, "Number format");
      icon_button(k, S_DECLESS, KIT_ICON_CUSTOM, KIT_BTN_FLAT,
                  "Fewer decimals");
      icon_button(k, S_DECMORE, KIT_ICON_CUSTOM, KIT_BTN_FLAT,
                  "More decimals");
      gap(k);
      icon_button(k, S_COLOUR, KIT_ICON_CUSTOM, KIT_BTN_FLAT, "Text colour");
      icon_button(k, S_FILL, KIT_ICON_CUSTOM, KIT_BTN_FLAT, "Fill colour");
      gap(k);
      icon_button(k, S_CHART, KIT_ICON_CUSTOM, KIT_BTN_FLAT,
                  "A chart of the selection");
      kit_spacer(k, 1);
      icon_button(k, S_MENU, KIT_ICON_MENU, KIT_BTN_FLAT, "More");
    kit_end(k);

    wg = kit_row(k, KIT_BOX_BAR);
    wg->ph = 38;
    wg->gap = 8;
      kit_label(k, S_NAME, sw->name, KIT_TEXT_BOLD | KIT_TEXT_CENTRE);
      kit_size(k, 84, 0);
      kit_label(k, S_FX, "fx", KIT_TEXT_DIM);
      kit_field(k, S_FIELD, &sw->field, "");
      kit_grow(k, 1);
    kit_end(k);

    wg = kit_row(k, KIT_BOX_NONE);
    wg->gap = 0;
    kit_grow(k, 1);
      wg = kit_col(k, KIT_BOX_NONE);
      wg->gap = 0;
      kit_grow(k, 1);
        kit_custom(k, S_GRID)->flags |= KIT_F_KEYS;
        kit_grow(k, 1);
        kit_scroll(k, S_HSCROLL)->style = KIT_SCROLL_HORIZ;
      kit_end(k);
      kit_scroll(k, S_VSCROLL);
    kit_end(k);

    kit_row(k, KIT_BOX_STATUS)->ph = 26;
      kit_label(k, S_STATUS, sw->status, KIT_TEXT_DIM);
      kit_grow(k, 1);
      kit_label(k, S_MODE, sw->modetext, KIT_TEXT_SMALL | KIT_TEXT_DIM);
    kit_end(k);
  kit_end(k);

  kit_focus(k, S_GRID);
  kit_layout(k);
}

#endif /* !DESK_PROGRAM_STUB */

/* The icon: a sheet of cells, its first row and column tinted, on green */

static void sheet_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  int sx = x + s / 6;
  int sy = y + s / 5;
  int sw = s - 2 * (s / 6);
  int sh = s - s / 5 - s / 6;
  int i;

  icon_tile(o, x, y, s, RGB(0x4c, 0xd4, 0x6a), RGB(0x16, 0x8c, 0x3c), a);
  ops_rrect(o, sx, sy, sw, sh, LUT_R3, 3, C_WHITE, a);
  ops_blend(o, sx + 1, sy + 1, sw - 2, sh / 4 - 1, RGB(0xb8, 0xec, 0xc4),
            a);
  ops_blend(o, sx + 1, sy + sh / 4, sw / 4 - 1, sh - sh / 4 - 1,
            RGB(0xe0, 0xf6, 0xe4), a);
  for (i = 1; i < 4; i++)
    {
      ops_blend(o, sx + 1, sy + i * sh / 4, sw - 2, 1,
                RGB(0x9c, 0xb4, 0xa4), a);
    }

  for (i = 1; i < 4; i++)
    {
      ops_blend(o, sx + i * sw / 4, sy + 1, 1, sh - 2,
                RGB(0x9c, 0xb4, 0xa4), a);
    }

  /* A total, in bold green, in the last row */

  ops_blend(o, sx + sw * 3 / 4 + 2, sy + sh * 3 / 4 + 2, sw / 4 - 5,
            sh / 4 - 4, RGB(0x16, 0x8c, 0x3c), a);
}

#ifndef DESK_PROGRAM_STUB
static struct desk_win_s *sheet_open(void)
{
  const char *path = g_openpath;
  struct desk_win_s *w;
  struct sw_s *sw;

  sw = calloc(1, sizeof(*sw));
  if (sw == NULL)
    {
      return NULL;
    }

  sw->sh = sheet_new();
  sw->o = malloc(sizeof(*sw->o));
  if (sw->sh == NULL || sw->o == NULL)
    {
      sheet_free(sw->sh);
      free(sw->o);
      free(sw);
      return NULL;
    }

  ops_reset(sw->o);
  sw->sh->now = desk_now;
  sw->sel.pr = -1;
  sw->tabc = -1;
  if (path != NULL)
    {
      int ret = load(sw, path);

      if (ret == -ENOENT)
        {
          strlcpy(sw->path, path, sizeof(sw->path));
        }
      else if (ret < 0)
        {
          snprintf(sw->msg, sizeof(sw->msg), "Cannot open %s: %s", path,
                   strerror(-ret));
          sw->msgerr = true;
        }
    }

  w = desk_win_open(APP_SHEET, "Spreadsheet", SHEET_W, SHEET_H);
  if (w == NULL)
    {
      sheet_free(sw->sh);
      free(sw->o);
      free(sw);
      return NULL;
    }

  w->priv = sw;
  snprintf(sw->title, sizeof(sw->title), "%s", base_name(sw));
  snprintf(w->title, sizeof(w->title), "%s", base_name(sw));
  sw->sh->touched.n = 0;
  sw->sh->alltouched = false;
  sheet_build(w, sw);
  sheet_draw(w);
  return w;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int desk_sheet_open(const char *path)
{
  struct desk_win_s *w;

  if (desk_program_self())
    {
      return desk_open_post(APP_SHEET, path);    /* The desktop's thread */
    }

  for (w = g_desk.wins; path != NULL && w != NULL; w = w->next)
    {
      struct sw_s *sw = w->priv;

      if (w->app == APP_SHEET && !w->closing && sw != NULL &&
          strcmp(sw->path, path) == 0)
        {
          desk_win_restore(w);
          return 0;
        }
    }

  g_openpath = path;
  desk_launch(APP_SHEET);
  g_openpath = NULL;
  return 0;
}

#endif /* !DESK_PROGRAM_STUB */

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef DESK_PROGRAM_STUB
int desk_sheet_open(const char *path)
{
  if (desk_program_self())
    {
      return desk_open_post(APP_SHEET, path);   /* The desktop's thread */
    }

  if (path == NULL)
    {
      desk_launch(APP_SHEET);
      return 0;
    }

  return desk_program_open(APP_SHEET, path);    /* Its program, with it */
}

DESK_PROGRAM_APP(g_app_sheet, "Spreadsheet", sheet_icon, "sheet", false);
#else
const struct desk_app_s g_app_sheet =
{
  .name     = "Spreadsheet",
  .icon     = sheet_icon,
  .open     = sheet_open,
  .draw     = sheet_draw,
  .expose   = sheet_expose,
  .resize   = sheet_resize,
  .tick     = sheet_tick,
  .mouse    = sheet_mouse,
  .key      = sheet_key,
  .focus    = sheet_focus,
  .close    = sheet_close,
  .canclose = sheet_canclose,
  .wake     = sheet_wake,
};
#endif
