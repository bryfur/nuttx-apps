/****************************************************************************
 * apps/graphics/fpcdesk/desk_term.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Terminal: tabs, each NSH on a pseudo-terminal with its own screen and
 * scrollback (desk_vt.c emulates the terminal), drawn with the GPU.
 *
 *   - Output: a thread a tab reads the pty's master into a ring and wakes
 *     the desktop's main thread (desk_wake()), which feeds all that is
 *     there to the emulator, then draws once.
 *   - Drawing: only what changed.  The window remembers which line each
 *     row shows; lines that moved (scrolling, the scrollback view) move
 *     with one COPY within the window's surface, then the rows and the
 *     columns of lines that changed are drawn: a FILL a run of cells of
 *     one background, a BLEND a character from the glyph atlas.
 *   - Input: keys go to the pty's master (NSH echoes them).
 *   - Tabs: the title bar's "+" opens one; with two or more, a strip of
 *     them below the title bar, each with its close button.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include "nshlib/nshlib.h"
#include "desk.h"
#include "desk_vt.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TERM_COLS      80
#define TERM_ROWS      24
#define CELL_W         9               /* IBM Plex Mono 15: advance 9 */
#define CELL_H         18
#define CELL_BASE      14              /* Baseline below the cell's top */
#define PAD_X          6
#define PAD_TOP        4
#define PAD_BOTTOM     6               /* Above the round corners */
#define SBAR_W         10              /* The scrollbar, at the right */
#define TAB_H          30              /* The tab strip */
#define TAB_MAXW       240
#define TAB_CLOSE      18              /* A tab's close button */
#define MAX_TABS       8
#define RING           2048            /* Output waiting to be shown */
#define WAKE_BYTES     8192            /* Most output shown at a time */
#define READER_STACK   3072
#define POLL_MS        500

#define C_CURSOR       RGB(0xd8, 0xd8, 0xd8)

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* What a tab shares with its reader thread.  Once quit is set, the thread
 * owns it (and frees it); until then the main thread does.
 */

struct term_io_s
{
  pthread_mutex_t lock;
  sem_t space;                     /* The ring has room, or quit */
  struct desk_win_s *w;
  int fd;                          /* The pty's master */
  pid_t pid;                       /* NSH */
  bool quit;                       /* The tab is gone */
  bool eof;                        /* The shell has gone */
  bool waiting;                    /* The thread waits on space */
  int head;
  int n;
  uint8_t ring[RING];
};

struct term_tab_s
{
  struct term_io_s *io;
  struct vt_s vt;
  int num;                         /* Terminal <num> */
  char title[40];                  /* As big as the window's */
  int view;                        /* Lines scrolled back */
};

struct term_s
{
  struct term_tab_s *tab[MAX_TABS];
  int ntabs;
  int cur;
  int nextnum;

  /* The layout */

  int cols;
  int rows;
  int gx;                          /* The grid's top-left, frame coords */
  int gy;
  int striph;                      /* The tab strip's height, 0: none */

  /* What the window shows of the current tab */

  struct vt_line_s **shown;        /* The line each row shows, or NULL */
  bool full;                       /* Everything to draw again */
  struct vt_line_s *curline;       /* Where the cursor is drawn */
  int curcol;
  int curshape;                    /* 0 none, 1 block, 2 hollow */
  uint32_t epoch;                  /* vt.epoch when curline was set */
  bool blink;                      /* The cursor's blink phase: on */
  int sbty;                        /* The scrollbar's thumb as drawn */
  int sbth;
  bool sbhot;

  /* The pointer */

  uint8_t buttons;
  int hovertab;                    /* Tab under it, -1, -2: scrollbar */
  bool hoverclose;                 /* ... over its close button */
  int pressclose;                  /* Close button pressed, -1 */
  bool sbdrag;                     /* Dragging the scrollbar's thumb */
  int sbgrab;                      /* Pointer y within the thumb */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct desk_ops_s g_o;
static NXWINDOW g_hwnd;                /* Where g_o goes */

/* The 16 colours: a dark theme's, after the desktop's own */

static const uint16_t g_pal16[16] =
{
  RGB(0x3a, 0x3a, 0x40), RGB(0xff, 0x5f, 0x57), RGB(0x32, 0xd7, 0x4b),
  RGB(0xfe, 0xbc, 0x2e), RGB(0x3a, 0x8e, 0xff), RGB(0xbf, 0x5a, 0xf2),
  RGB(0x40, 0xc8, 0xe0), RGB(0xd8, 0xd8, 0xd8),
  RGB(0x6e, 0x6e, 0x76), RGB(0xff, 0x8a, 0x80), RGB(0x6c, 0xf0, 0x85),
  RGB(0xff, 0xd6, 0x5c), RGB(0x78, 0xb4, 0xff), RGB(0xd8, 0x92, 0xff),
  RGB(0x80, 0xe4, 0xf4), RGB(0xff, 0xff, 0xff)
};

/****************************************************************************
 * Private Functions: colours, cells
 ****************************************************************************/

static uint16_t colour(uint8_t i)
{
  static const uint8_t level[6] =
  {
    0, 95, 135, 175, 215, 255
  };

  if (i < 16)
    {
      return g_pal16[i];
    }

  if (i < 232)
    {
      i -= 16;
      return RGB(level[i / 36], level[i / 6 % 6], level[i % 6]);
    }

  i = 8 + (i - 232) * 10;
  return RGB(i, i, i);
}

static const struct vt_cell_s *cell_at(const struct vt_line_s *l, int x)
{
  static const struct vt_cell_s blank =
  {
    ' ', 7, 0, VT_FGDEF | VT_BGDEF
  };

  return x < l->n ? &l->c[x] : &blank;
}

static void cell_colours(const struct vt_cell_s *c, uint16_t *fg,
                         uint16_t *bg)
{
  uint16_t f = (c->at & VT_FGDEF) ? C_TERM_FG : colour(c->fg);
  uint16_t b = (c->at & VT_BGDEF) ? C_TERM_BG : colour(c->bg);

  if (c->at & VT_REVERSE)
    {
      uint16_t t = f;

      f = b;
      b = t;
    }

  if (c->at & VT_DIM)
    {
      f = colour_mix(f, b, 100);
    }

  if (c->at & VT_HIDDEN)
    {
      f = b;
    }

  *fg = f;
  *bg = b;
}

static void line_dirty(struct vt_line_s *l, int x0, int x1)
{
  if (l->d0 >= l->d1)
    {
      l->d0 = x0;
      l->d1 = x1;
    }
  else
    {
      l->d0 = x0 < l->d0 ? x0 : l->d0;
      l->d1 = x1 > l->d1 ? x1 : l->d1;
    }
}

/****************************************************************************
 * Private Functions: drawing
 ****************************************************************************/

/* Room for k more operations in g_o */

static void need(int k)
{
  if (g_o.n + k > NX_GPUOPS_MAX)
    {
      ops_draw(g_hwnd, &g_o);
    }
}

/* A cell's character (or shape) in colour fg */

static void draw_char(int px, int py, const struct vt_cell_s *c,
                      uint16_t fg)
{
  uint8_t ch = c->ch;

  need(5);
  if (ch >= VT_CH_BOX && ch < VT_CH_BOX + 16)
    {
      int cx = px + CELL_W / 2;
      int cy = py + CELL_H / 2;

      if (ch & 1)
        {
          ops_fill(&g_o, px, cy, cx - px + 1, 1, fg);
        }

      if (ch & 2)
        {
          ops_fill(&g_o, cx, cy, px + CELL_W - cx, 1, fg);
        }

      if (ch & 4)
        {
          ops_fill(&g_o, cx, py, 1, cy - py + 1, fg);
        }

      if (ch & 8)
        {
          ops_fill(&g_o, cx, cy, 1, py + CELL_H - cy, fg);
        }
    }
  else if (ch == VT_CH_BLOCK)
    {
      ops_fill(&g_o, px, py, CELL_W, CELL_H, fg);
    }
  else if (ch == VT_CH_LOWER)
    {
      ops_fill(&g_o, px, py + CELL_H / 2, CELL_W, CELL_H - CELL_H / 2, fg);
    }
  else if (ch == VT_CH_UPPER)
    {
      ops_fill(&g_o, px, py, CELL_W, CELL_H / 2, fg);
    }
  else if (ch == VT_CH_SHADE)
    {
      ops_blend(&g_o, px, py, CELL_W, CELL_H, fg, 110);
    }
  else if (ch != ' ')
    {
      const struct desk_font_s *f = (c->at & VT_BOLD) ? &g_desk_font_monob :
                                                        &g_desk_font_mono;
      char s[2];

      s[0] = (char)ch;
      s[1] = '\0';
      ops_text(&g_o, f, px, py + CELL_BASE - f->ascent, s, fg, 255);
    }

  if (c->at & VT_UNDER)
    {
      ops_fill(&g_o, px, py + CELL_BASE + 2, CELL_W, 1, fg);
    }
}

/* Columns x0 to x1 - 1 of line l, at row r */

static void draw_span(struct term_s *t, struct vt_line_s *l, int r, int x0,
                      int x1)
{
  int py = t->gy + r * CELL_H;
  uint16_t fg;
  uint16_t bg;
  int x;

  if (x1 > t->cols)
    {
      x1 = t->cols;
    }

  /* The backgrounds, a FILL a run */

  for (x = x0; x < x1; )
    {
      uint16_t b;
      int e;

      cell_colours(cell_at(l, x), &fg, &bg);
      for (e = x + 1; e < x1; e++)
        {
          cell_colours(cell_at(l, e), &fg, &b);
          if (b != bg)
            {
              break;
            }
        }

      need(1);
      ops_fill(&g_o, t->gx + x * CELL_W, py, (e - x) * CELL_W, CELL_H, bg);
      x = e;
    }

  /* The characters */

  for (x = x0; x < x1; x++)
    {
      const struct vt_cell_s *c = cell_at(l, x);

      if (c->ch != ' ' || (c->at & VT_UNDER))
        {
          cell_colours(c, &fg, &bg);
          draw_char(t->gx + x * CELL_W, py, c, fg);
        }
    }

  /* The cursor: a block (the character in the background's colour), or
   * hollow in an inactive window
   */

  if (l == t->curline && t->curshape != 0 && t->curcol >= x0 &&
      t->curcol < x1)
    {
      const struct vt_cell_s *c = cell_at(l, t->curcol);
      int px = t->gx + t->curcol * CELL_W;

      cell_colours(c, &fg, &bg);
      need(4);
      if (t->curshape == 1)
        {
          ops_fill(&g_o, px, py, CELL_W, CELL_H, C_CURSOR);
          draw_char(px, py, c, bg == C_CURSOR ? C_TERM_BG : bg);
        }
      else
        {
          ops_fill(&g_o, px, py, CELL_W, 1, C_CURSOR);
          ops_fill(&g_o, px, py + CELL_H - 1, CELL_W, 1, C_CURSOR);
          ops_fill(&g_o, px, py, 1, CELL_H, C_CURSOR);
          ops_fill(&g_o, px + CELL_W - 1, py, 1, CELL_H, C_CURSOR);
        }
    }
}

/* The tab strip: the tabs (the current one lit), their titles, their
 * close buttons
 */

static int tab_width(struct desk_win_s *w, struct term_s *t)
{
  int tw = (w->csize.w - 2 * PAD_X) / (t->ntabs > 0 ? t->ntabs : 1);

  return tw > TAB_MAXW ? TAB_MAXW : tw;
}

static int tab_x(struct desk_win_s *w, struct term_s *t, int i)
{
  int tw = tab_width(w, t);

  return (w->csize.w - tw * t->ntabs) / 2 + i * tw;
}

static void draw_strip(struct desk_win_s *w, struct term_s *t)
{
  const struct desk_font_s *f = &g_desk_font_ui;
  int tw = tab_width(w, t);
  int ty = TITLE_H;
  int i;

  if (t->striph == 0)
    {
      return;
    }

  g_hwnd = (NXWINDOW)w->hwnd;
  need(2);
  ops_fill(&g_o, 0, ty, w->csize.w, TAB_H,
           w->active ? C_TITLE_ACTIVE : C_TITLE_INACTIVE);
  ops_blend(&g_o, 0, ty + TAB_H - 1, w->csize.w, 1, C_BLACK, 120);

  for (i = 0; i < t->ntabs; i++)
    {
      struct term_tab_s *tab = t->tab[i];
      int x = tab_x(w, t, i);
      int cx = x + tw - 8 - TAB_CLOSE;
      int cy = ty + (TAB_H - 1 - TAB_CLOSE) / 2;
      int room = tw - 2 * (TAB_CLOSE + 12);
      char label[sizeof(tab->title) + 4];
      bool hover = t->hovertab == i;
      int len;

      need(2);
      if (i == t->cur)
        {
          ops_rrect(&g_o, x + 3, ty + 3, tw - 6, TAB_H - 7, LUT_R6, 6,
                    C_TITLE_HILITE, 255);
        }
      else if (hover)
        {
          ops_rrect(&g_o, x + 3, ty + 3, tw - 6, TAB_H - 7, LUT_R6, 6,
                    C_WHITE, 14);
        }
      else if (i + 1 != t->cur && i + 1 < t->ntabs &&
               t->hovertab != i + 1)
        {
          ops_blend(&g_o, x + tw - 1, ty + 8, 1, TAB_H - 17, C_WHITE, 30);
        }

      /* The title, shortened to fit */

      snprintf(label, sizeof(label), "%s", tab->title);
      len = strlen(label);
      if (text_width(f, label) > room)
        {
          while (len > 0 && text_width(f, label) +
                            text_width(f, DESK_CH_ELLIPSIS) > room)
            {
              label[--len] = '\0';
            }

          strcat(label, DESK_CH_ELLIPSIS);
        }

      need(sizeof(label));
      ops_text_centre(&g_o, f, x + tw / 2,
                      ty + (TAB_H - 1 - text_height(f)) / 2, label,
                      i == t->cur && w->active ? C_TITLE_TEXT : C_TITLE_DIM,
                      255);

      /* The close button: on the current tab and the one under the
       * pointer
       */

      if (i == t->cur || hover)
        {
          need(2);
          if (hover && t->hoverclose)
            {
              ops_rrect(&g_o, cx, cy, TAB_CLOSE, TAB_CLOSE, LUT_R6, 6,
                        C_WHITE, t->pressclose == i ? 60 : 30);
            }

          ops_text_centre(&g_o, &g_desk_font_uib, cx + TAB_CLOSE / 2,
                          cy + (TAB_CLOSE -
                                text_height(&g_desk_font_uib)) / 2,
                          DESK_CH_TIMES,
                          i == t->cur ? C_TITLE_TEXT : C_TITLE_DIM, 255);
        }
    }
}

static void strip_redraw(struct desk_win_s *w, struct term_s *t)
{
  if (t->striph > 0)
    {
      ops_reset(&g_o);
      draw_strip(w, t);
      ops_draw(g_hwnd, &g_o);
    }
}

/* The scrollbar's thumb: where it is, 0 high without scrollback */

static void sbar_thumb(struct term_s *t, struct term_tab_s *tab, int *ty,
                       int *th)
{
  struct vt_s *vt = &tab->vt;
  int track = t->rows * CELL_H;
  int n = vt->scr == vt->main ? vt->sbn : 0;
  int view = tab->view > n ? n : tab->view;

  if (n == 0)
    {
      *ty = t->gy;
      *th = 0;
      return;
    }

  *th = track * t->rows / (t->rows + n);
  *th = *th < 24 ? 24 : *th;
  *ty = t->gy + (track - *th) * (n - view) / n;
}

static void draw_sbar(struct desk_win_s *w, struct term_s *t,
                      struct term_tab_s *tab)
{
  int x = w->csize.w - SBAR_W - 1;
  bool hot = t->sbdrag || t->hovertab == -2;
  int ty;
  int th;

  sbar_thumb(t, tab, &ty, &th);
  if (ty == t->sbty && th == t->sbth && hot == t->sbhot)
    {
      return;
    }

  t->sbty  = ty;
  t->sbth  = th;
  t->sbhot = hot;
  need(2);
  ops_fill(&g_o, x, t->gy, SBAR_W, t->rows * CELL_H, C_TERM_BG);
  if (th > 0)
    {
      ops_rrect(&g_o, x + (hot ? 1 : 2), ty, hot ? 8 : 6, th, LUT_R3, 3,
                C_WHITE, hot ? 120 : 70);
    }
}

/* Lines that moved since they were drawn: a COPY for each run of rows
 * that moved together, all in one direction (the first seen), in the
 * order that reads every source before it is overwritten.  Lines new or
 * wholly changed are drawn instead.
 */

static void term_copies(struct term_s *t, struct vt_line_s **desired)
{
  int dir = 0;
  int r = 0;

  while (r < t->rows)
    {
      int step = dir < 0 ? -1 : 1;
      int i = dir < 0 ? t->rows - 1 - r : r;
      struct vt_line_s *l = desired[i];
      int n = 0;
      int j;
      int d;

      if (l == t->shown[i] || (l->d0 == 0 && l->d1 >= t->cols))
        {
          r++;
          continue;
        }

      for (j = 0; j < t->rows && t->shown[j] != l; j++)
        {
        }

      d = j - i;
      if (j == t->rows || (dir != 0 && (d > 0) != (dir > 0)))
        {
          r++;
          continue;
        }

      if (dir == 0)
        {
          dir = d > 0 ? 1 : -1;               /* From the right end */
          r   = 0;
          continue;
        }

      /* The run: rows i, i + step, ... showing the lines shown d on */

      while (i + n * step >= 0 && i + n * step < t->rows &&
             i + n * step + d >= 0 && i + n * step + d < t->rows &&
             desired[i + n * step] == t->shown[i + n * step + d])
        {
          n++;
        }

      need(1);
      {
        struct nx_gpuop_s *op = ops_add(&g_o);
        int top = step > 0 ? i : i - n + 1;

        op->op = NX_GPUOP_COPY;
        op->x  = t->gx;
        op->y  = t->gy + top * CELL_H;
        op->w  = t->cols * CELL_W;
        op->h  = n * CELL_H;
        op->sx = t->gx;
        op->sy = t->gy + (top + d) * CELL_H;
      }

      for (j = 0; j < n; j++)
        {
          int k = i + j * step;

          t->shown[k + d] = NULL;             /* Unless a destination */
          t->shown[k] = desired[k];
        }

      r += n;
    }
}

/* Draw what changed of the current tab */

static void term_render(struct desk_win_s *w)
{
  struct term_s *t = w->priv;
  struct vt_line_s *want[64];
  struct vt_line_s **desired = want;
  struct term_tab_s *tab;
  struct vt_line_s *cl;
  struct vt_s *vt;
  int shape;
  int r;

  if (t == NULL || t->ntabs == 0 || t->shown == NULL || w->closing)
    {
      return;
    }

  tab    = t->tab[t->cur];
  vt     = &tab->vt;
  g_hwnd = (NXWINDOW)w->hwnd;
  if (vt->rows != t->rows || vt->cols != t->cols)
    {
      return;                              /* It could not resize */
    }

  if (t->rows > 64)
    {
      desired = malloc(t->rows * sizeof(*desired));
      if (desired == NULL)
        {
          return;
        }
    }

  ops_reset(&g_o);
  if (tab->view > vt->sbn || vt->scr != vt->main)
    {
      tab->view = vt->scr != vt->main ? 0 : vt->sbn;
    }

  if (t->full)
    {
      need(1);
      ops_fill(&g_o, 0, TITLE_H + t->striph, w->csize.w,
               w->csize.h - t->striph, C_TERM_BG);
      draw_strip(w, t);
      memset(t->shown, 0, t->rows * sizeof(t->shown[0]));
      t->curline = NULL;
      t->sbty    = -1;
      t->full    = false;
    }

  /* The cursor: drawn again where it was, and where it is now */

  cl    = vt->scr[vt->y];
  shape = !vt->cursor ? 0 : !w->active ? 2 : t->blink ? 1 : 0;
  if (t->epoch != vt->epoch)
    {
      t->curline = NULL;                   /* It may be gone */
      t->epoch   = vt->epoch;
    }

  if (t->curline != cl || t->curcol != vt->x || t->curshape != shape)
    {
      if (t->curline != NULL)
        {
          line_dirty(t->curline, t->curcol, t->curcol + 1);
        }

      line_dirty(cl, vt->x, vt->x + 1);
      t->curline  = cl;
      t->curcol   = vt->x;
      t->curshape = shape;
    }

  for (r = 0; r < t->rows; r++)
    {
      desired[r] = vt_line(vt, r, tab->view);
    }

  term_copies(t, desired);

  /* The rows, and the columns, that changed */

  for (r = 0; r < t->rows; r++)
    {
      struct vt_line_s *l = desired[r];

      if (t->shown[r] != l)
        {
          draw_span(t, l, r, 0, t->cols);
          t->shown[r] = l;
        }
      else if (l->d0 < l->d1)
        {
          draw_span(t, l, r, l->d0, l->d1);
        }
    }

  for (r = 0; r < t->rows; r++)
    {
      desired[r]->d0 = 0;
      desired[r]->d1 = 0;
    }

  draw_sbar(w, t, tab);
  ops_draw(g_hwnd, &g_o);

  if (desired != want)
    {
      free(desired);
    }
}

/****************************************************************************
 * Private Functions: tabs
 ****************************************************************************/

static void tab_winsize(struct term_tab_s *tab)
{
  struct winsize ws;

  memset(&ws, 0, sizeof(ws));
  ws.ws_row    = tab->vt.rows;
  ws.ws_col    = tab->vt.cols;
  ws.ws_xpixel = tab->vt.cols * CELL_W;
  ws.ws_ypixel = tab->vt.rows * CELL_H;
  ioctl(tab->io->fd, TIOCSWINSZ, (unsigned long)(uintptr_t)&ws);
}

/* The grid from the client area's size; every tab's screen follows */

static void term_layout(struct desk_win_s *w)
{
  struct term_s *t = w->priv;
  int cols;
  int rows;
  int i;

  t->striph = t->ntabs > 1 ? TAB_H : 0;
  t->gx     = PAD_X;
  t->gy     = TITLE_H + t->striph + PAD_TOP;
  cols = (w->csize.w - PAD_X - SBAR_W - 2) / CELL_W;
  rows = (w->csize.h - t->striph - PAD_TOP - PAD_BOTTOM) / CELL_H;
  cols = cols < 1 ? 1 : cols;
  rows = rows < 1 ? 1 : rows;

  if (rows != t->rows || t->shown == NULL)
    {
      struct vt_line_s **shown = realloc(t->shown, rows * sizeof(*shown));

      if (shown == NULL)
        {
          return;
        }

      t->shown = shown;
    }

  t->cols = cols;
  t->rows = rows;
  for (i = 0; i < t->ntabs; i++)
    {
      if (t->tab[i]->vt.cols != cols || t->tab[i]->vt.rows != rows)
        {
          vt_resize(&t->tab[i]->vt, cols, rows);
          tab_winsize(t->tab[i]);
        }
    }

  t->full = true;
}

/* The window's title: the current tab's */

static void term_title(struct desk_win_s *w)
{
  struct term_s *t = w->priv;

  snprintf(w->title, sizeof(w->title), "%s", t->tab[t->cur]->title);
  desk_win_title(w);
}

static void tab_select(struct desk_win_s *w, int i)
{
  struct term_s *t = w->priv;

  t->cur  = i;
  t->full = true;
  term_title(w);
  term_render(w);
}

/* NSH on the pty's slave */

static int term_task(int argc, char *argv[])
{
  int fd;

  if (argc < 2)
    {
      return EXIT_FAILURE;
    }

  fd = open(argv[1], O_RDWR);
  if (fd < 0)
    {
      return EXIT_FAILURE;
    }

  fflush(stdout);
  fflush(stderr);
  dup2(fd, 0);
  dup2(fd, 1);
  dup2(fd, 2);
  if (fd > 2)
    {
      close(fd);
    }

  return nsh_consolemain(0, NULL);
}

/* The reader: the pty's output into the ring, then a wake for the main
 * thread.  It also notices the shell going.
 */

static void *term_reader(void *arg)
{
  struct term_io_s *io = arg;

  for (; ; )
    {
      struct pollfd pfd;
      ssize_t nread;
      int tail;
      int span;

      pthread_mutex_lock(&io->lock);
      if (io->quit)
        {
          pthread_mutex_unlock(&io->lock);
          break;
        }

      if (io->n == RING || io->eof)
        {
          io->waiting = true;
          pthread_mutex_unlock(&io->lock);
          sem_wait(&io->space);
          continue;
        }

      tail = (io->head + io->n) % RING;
      span = RING - io->n;
      span = span > RING - tail ? RING - tail : span;
      pthread_mutex_unlock(&io->lock);

      pfd.fd      = io->fd;
      pfd.events  = POLLIN;
      pfd.revents = 0;
      if (poll(&pfd, 1, POLL_MS) == 0)
        {
          if (kill(io->pid, 0) == 0)
            {
              continue;
            }

          nread = 0;                           /* The shell has gone */
        }
      else
        {
          nread = read(io->fd, io->ring + tail, span);
          if (nread < 0 && (errno == EAGAIN || errno == EINTR))
            {
              continue;
            }
        }

      pthread_mutex_lock(&io->lock);
      if (nread > 0)
        {
          io->n += nread;
        }
      else
        {
          io->eof = true;
        }

      if (!io->quit)
        {
          desk_wake(io->w);
        }

      pthread_mutex_unlock(&io->lock);
    }

  close(io->fd);
  sem_destroy(&io->space);
  pthread_mutex_destroy(&io->lock);
  free(io);
  return NULL;
}

/* A tab goes: its shell, its pty (the reader closes it), its screen */

static void tab_free(struct term_tab_s *tab)
{
  struct term_io_s *io = tab->io;

  pthread_mutex_lock(&io->lock);
  if (io->pid > 0 && !io->eof && kill(io->pid, 0) == 0)
    {
      task_delete(io->pid);
    }

  if (io->pid > 0)
    {
      io->quit = true;                        /* The reader frees it */
      pthread_mutex_unlock(&io->lock);
      sem_post(&io->space);
    }
  else
    {
      pthread_mutex_unlock(&io->lock);        /* There is no reader */
      if (io->fd >= 0)
        {
          close(io->fd);
        }

      sem_destroy(&io->space);
      pthread_mutex_destroy(&io->lock);
      free(io);
    }

  vt_free(&tab->vt);
  free(tab);
}

static struct term_tab_s *tab_new(struct desk_win_s *w)
{
  static bool nshinit;
  struct term_s *t = w->priv;
  struct term_tab_s *tab;
  struct term_io_s *io;
  pthread_attr_t attr;
  pthread_t thread;
  char path[32];
  char *argv[2];
  int one = 1;

  tab = calloc(1, sizeof(*tab));
  io  = calloc(1, sizeof(*io));
  if (tab == NULL || io == NULL ||
      vt_init(&tab->vt, t->cols, t->rows,
              CONFIG_FPCDESK_TERM_SCROLLBACK) < 0)
    {
      free(tab);
      free(io);
      return NULL;
    }

  tab->io  = io;
  tab->num = ++t->nextnum;
  snprintf(tab->title, sizeof(tab->title), "Terminal %d", tab->num);
  pthread_mutex_init(&io->lock, NULL);
  sem_init(&io->space, 0, 0);
  io->w   = w;
  io->pid = -1;

  /* The pty.  The master is not inherited by the shells. */

  io->fd = posix_openpt(O_RDWR | O_NOCTTY);
  if (io->fd < 0 || unlockpt(io->fd) < 0 ||
      ptsname_r(io->fd, path, sizeof(path)) < 0)
    {
      fprintf(stderr, "desk: no pty: %d\n", errno);
      tab_free(tab);
      return NULL;
    }

  fcntl(io->fd, F_SETFD, FD_CLOEXEC);
  ioctl(io->fd, FIONBIO, (unsigned long)(uintptr_t)&one);
  tab_winsize(tab);

  if (!nshinit)
    {
      nsh_initialize();
      nshinit = true;
    }

  argv[0] = path;
  argv[1] = NULL;
  io->pid = task_create("Terminal", CONFIG_FPCDESK_TERM_PRIORITY,
                        CONFIG_FPCDESK_TERM_STACKSIZE, term_task, argv);
  if (io->pid < 0)
    {
      fprintf(stderr, "desk: no shell: %d\n", errno);
      io->pid = -1;
      tab_free(tab);
      return NULL;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, READER_STACK);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&thread, &attr, term_reader, io) != 0)
    {
      task_delete(io->pid);
      io->pid = -1;
      tab_free(tab);
      return NULL;
    }

  return tab;
}

/* A new tab, made current */

static void tab_open(struct desk_win_s *w)
{
  struct term_s *t = w->priv;
  struct term_tab_s *tab;

  if (t->ntabs >= MAX_TABS)
    {
      return;
    }

  tab = tab_new(w);
  if (tab == NULL)
    {
      return;
    }

  t->tab[t->ntabs++] = tab;
  if (t->ntabs == 2)
    {
      term_layout(w);                         /* The strip appears */
    }

  tab_select(w, t->ntabs - 1);
}

static void tab_close(struct desk_win_s *w, int i)
{
  struct term_s *t = w->priv;

  tab_free(t->tab[i]);
  memmove(&t->tab[i], &t->tab[i + 1],
          (t->ntabs - i - 1) * sizeof(t->tab[0]));
  t->ntabs--;
  t->hovertab   = -1;
  t->pressclose = -1;

  if (t->ntabs == 0)
    {
      desk_win_close(w);                      /* The last one */
      return;
    }

  if (t->cur > i || t->cur == t->ntabs)
    {
      t->cur--;
    }

  if (t->ntabs == 1)
    {
      term_layout(w);                         /* The strip goes */
    }

  tab_select(w, t->cur);
}

/****************************************************************************
 * Private Functions: the application
 ****************************************************************************/

static void term_icon(struct desk_ops_s *o, int x, int y, int s,
                          uint8_t a)
{
  const struct desk_font_s *f = s >= 64 ? &g_desk_font_title :
                                          &g_desk_font_uib;

  icon_tile(o, x, y, s, RGB(0x46, 0x46, 0x4c), RGB(0x16, 0x16, 0x1a), a);
  ops_blend(o, x + 6, y + s / 5, s - 12, 1, C_WHITE, (uint8_t)(a / 6));
  ops_text(o, f, x + s / 6, y + s / 2 - text_height(f) / 2 + 2, ">_",
           RGB(0x3d, 0xdc, 0x84), a);
}

static struct desk_win_s *term_open(void)
{
  struct desk_win_s *w;
  struct term_s *t;

  w = desk_win_open(APP_TERMINAL, "Terminal",
                    PAD_X + TERM_COLS * CELL_W + SBAR_W + 2,
                    PAD_TOP + TERM_ROWS * CELL_H + PAD_BOTTOM);
  if (w == NULL)
    {
      return NULL;
    }

  t = calloc(1, sizeof(*t));
  if (t == NULL)
    {
      desk_win_close(w);
      return NULL;
    }

  w->priv       = t;
  w->button     = "+";
  t->hovertab   = -1;
  t->pressclose = -1;
  t->blink      = true;
  desk_win_sync(w);
  term_layout(w);
  tab_open(w);
  if (t->ntabs == 0)
    {
      desk_win_close(w);
      return NULL;
    }

  return w;
}

static void term_close(struct desk_win_s *w)
{
  struct term_s *t = w->priv;

  if (t != NULL)
    {
      while (t->ntabs > 0)
        {
          tab_free(t->tab[--t->ntabs]);
        }

      free(t->shown);
      t->shown = NULL;
    }
}

/* Output: all that the readers have, to the screens; then the current
 * tab is drawn once
 */

static void term_wake(struct desk_win_s *w)
{
  struct term_s *t = w->priv;
  bool more = false;
  bool draw = false;
  int i;

  if (t == NULL)
    {
      return;
    }

  for (i = 0; i < t->ntabs; i++)
    {
      struct term_tab_s *tab = t->tab[i];
      struct term_io_s *io = tab->io;
      int total = 0;
      bool eof;

      for (; ; )
        {
          int head;
          int span;

          pthread_mutex_lock(&io->lock);
          head = io->head;
          span = io->n;
          eof  = io->eof && span == 0;        /* Gone, all shown */
          pthread_mutex_unlock(&io->lock);

          span = span > RING - head ? RING - head : span;
          if (span == 0 || total >= WAKE_BYTES)
            {
              more |= span > 0;
              break;
            }

          vt_write(&tab->vt, io->ring + head, span);
          total += span;

          pthread_mutex_lock(&io->lock);
          io->head = (io->head + span) % RING;
          io->n   -= span;
          if (io->waiting)
            {
              io->waiting = false;
              sem_post(&io->space);
            }

          pthread_mutex_unlock(&io->lock);
        }

      /* Replies (the cursor's position, the device's attributes) */

      if (tab->vt.nresp > 0)
        {
          write(io->fd, tab->vt.resp, tab->vt.nresp);
          tab->vt.nresp = 0;
        }

      if (tab->vt.titled)
        {
          tab->vt.titled = false;
          snprintf(tab->title, sizeof(tab->title), "%s",
                   tab->vt.title[0] != '\0' ? tab->vt.title : "Terminal");
          if (i == t->cur)
            {
              term_title(w);
            }

          g_hwnd = (NXWINDOW)w->hwnd;
          strip_redraw(w, t);
        }

      if (eof)
        {
          tab_close(w, i);                    /* The shell has gone */
          if (w->closing)
            {
              return;
            }

          i--;
          continue;
        }

      if (i == t->cur && total > 0)
        {
          tab->view = 0;                      /* Output: back to the end */
          t->blink  = true;
          draw      = true;
        }
    }

  if (draw)
    {
      term_render(w);
    }

  if (more)
    {
      desk_wake(w);                           /* The rest after events */
    }
}

/* Keys to the current tab's shell */

static void term_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct term_s *t = w->priv;
  struct term_tab_s *tab;
  uint8_t buf[3];

  if (t == NULL || t->ntabs == 0)
    {
      return;
    }

  tab = t->tab[t->cur];

  /* Cursor keys in application mode (DECCKM): ESC O x */

  if (tab->vt.appcursor && n == 3 && ch[0] == 0x1b && ch[1] == '[' &&
      ch[2] >= 'A' && ch[2] <= 'H')
    {
      buf[0] = 0x1b;
      buf[1] = 'O';
      buf[2] = ch[2];
      ch = buf;
    }

  write(tab->io->fd, ch, n);

  if (tab->view != 0 || !t->blink)
    {
      tab->view = 0;
      t->blink  = true;
      term_render(w);
    }
}

/* The cursor blinks */

static void term_tick(struct desk_win_s *w)
{
  struct term_s *t = w->priv;

  if (t != NULL && w->active)
    {
      t->blink = !t->blink;
      term_render(w);
    }
}

/* Active or not: the strip's colours, the cursor's shape */

static void term_focus(struct desk_win_s *w)
{
  struct term_s *t = w->priv;

  if (t != NULL && t->ntabs > 0)
    {
      t->blink = true;
      g_hwnd   = (NXWINDOW)w->hwnd;
      strip_redraw(w, t);
      term_render(w);
    }
}

/* Part of the client area to draw again (new territory while resizing) */

static void term_expose(struct desk_win_s *w, int x, int y, int cw, int ch)
{
  struct term_s *t = w->priv;
  int r;

  ops_reset(&g_o);
  ops_fill(&g_o, x, TITLE_H + y, cw, ch, C_TERM_BG);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
  if (t == NULL || t->shown == NULL)
    {
      return;
    }

  /* The rows it crosses are drawn again (while the window grows, the new
   * territory is outside the grid, which keeps its size until the end)
   */

  if (x < t->gx + t->cols * CELL_W)
    {
      for (r = 0; r < t->rows; r++)
        {
          int py = t->gy - TITLE_H + r * CELL_H;

          if (py < y + ch && py + CELL_H > y)
            {
              t->shown[r] = NULL;
            }
        }
    }

  g_hwnd = (NXWINDOW)w->hwnd;
  if (y < t->striph)
    {
      strip_redraw(w, t);
    }

  term_render(w);
}

static void term_resize(struct desk_win_s *w)
{
  if (w->priv != NULL)
    {
      term_layout(w);
      term_render(w);
    }
}

static void term_button(struct desk_win_s *w)
{
  if (w->priv != NULL)
    {
      tab_open(w);
    }
}

/* The pointer: tabs, their close buttons, the scrollbar */

static void term_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct term_s *t = w->priv;
  struct term_tab_s *tab;
  bool hclose = false;
  bool release;
  bool press;
  int hover = -1;
  int top;
  int ty;
  int th;

  if (t == NULL || t->ntabs == 0)
    {
      return;
    }

  tab     = t->tab[t->cur];
  top     = t->gy - TITLE_H;
  press   = (buttons & NX_MOUSE_LEFTBUTTON) != 0 &&
            (t->buttons & NX_MOUSE_LEFTBUTTON) == 0;
  release = (buttons & NX_MOUSE_LEFTBUTTON) == 0 &&
            (t->buttons & NX_MOUSE_LEFTBUTTON) != 0;
  t->buttons = buttons;
  g_hwnd     = (NXWINDOW)w->hwnd;
  sbar_thumb(t, tab, &ty, &th);
  ty -= TITLE_H;

  /* Dragging the scrollbar's thumb: the view follows */

  if (t->sbdrag)
    {
      int track = t->rows * CELL_H - th;
      int n = tab->vt.sbn;
      int pos = y - t->sbgrab - top;
      int view;

      pos  = pos < 0 ? 0 : pos > track ? track : pos;
      view = track > 0 ? n - (pos * n + track / 2) / track : 0;
      if (release)
        {
          t->sbdrag = false;
        }

      if (view != tab->view || release)
        {
          tab->view = view;
          term_render(w);
        }

      return;
    }

  /* What is under the pointer */

  if (t->striph > 0 && y >= 0 && y < t->striph)
    {
      int tw = tab_width(w, t);
      int i;

      for (i = 0; i < t->ntabs; i++)
        {
          int tx = tab_x(w, t, i);

          if (x >= tx && x < tx + tw)
            {
              int cx = tx + tw - 8 - TAB_CLOSE;

              hover  = i;
              hclose = x >= cx && x < cx + TAB_CLOSE;
            }
        }
    }
  else if (x >= w->csize.w - SBAR_W - 4 && y >= top &&
           y < top + t->rows * CELL_H && th > 0)
    {
      hover = -2;                             /* The scrollbar */
    }

  if (hover != t->hovertab || hclose != t->hoverclose)
    {
      bool strip = hover >= 0 || t->hovertab >= 0;

      t->hovertab   = hover;
      t->hoverclose = hclose;
      if (strip)
        {
          strip_redraw(w, t);
        }

      term_render(w);                         /* The scrollbar lights up */
    }

  if (press && hover >= 0)
    {
      if (hclose)
        {
          t->pressclose = hover;
          strip_redraw(w, t);
        }
      else if (hover != t->cur)
        {
          tab_select(w, hover);
        }
    }
  else if (release && t->pressclose >= 0)
    {
      int which = t->pressclose;

      t->pressclose = -1;
      if (hclose && hover == which)
        {
          tab_close(w, which);
        }
      else
        {
          strip_redraw(w, t);
        }
    }
  else if (press && hover == -2)
    {
      if (y >= ty && y < ty + th)
        {
          t->sbdrag = true;                   /* The thumb */
          t->sbgrab = y - ty;
        }
      else
        {
          /* The track: a page towards the pointer */

          tab->view += y < ty ? t->rows - 1 : -(t->rows - 1);
          tab->view  = tab->view < 0 ? 0 : tab->view;
        }

      term_render(w);
    }
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_terminal =
{
  .name   = "Terminal",
  .icon   = term_icon,
  .open   = term_open,
  .key    = term_key,
  .mouse  = term_mouse,
  .expose = term_expose,
  .resize = term_resize,
  .tick   = term_tick,
  .close  = term_close,
  .wake   = term_wake,
  .focus  = term_focus,
  .button = term_button,
};
