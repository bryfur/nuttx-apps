/****************************************************************************
 * apps/graphics/fpcdesk/desk_log.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * System Log: the syslog as it grows.  The kernel keeps it in a RAM log
 * (CONFIG_RAMLOG_SYSLOG, /dev/kmsg, a channel beside the console's); the
 * application reads what is there when it opens, then a thread of its own
 * waits in poll() for more and wakes the desktop's main thread
 * (desk_wake()), which reads it.
 *
 * Lines are kept as they come (the latest LOG_MAX_LINES of them), with a
 * level guessed from their words (error, warning, the rest).  The list
 * shows the ones the filter matches, wrapped to the window's width, in
 * the monospaced font: errors and warnings in their colours.  It follows
 * the end (new lines scroll it with one COPY within the window) until it
 * is paused or scrolled back.  A click selects a line, a drag more;
 * Copy (Ctrl-C) puts them on the desktop's clipboard (all of the lines
 * shown when none is selected); Clear empties the list (not the kernel's
 * log).  Keys: typing filters, Escape clears the filter, arrows, Page
 * Up/Down, Home and End scroll (End follows again), Ctrl-A selects all.
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "desk.h"
#include "desk_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifdef CONFIG_RAMLOG_SYSLOG
#  define LOG_DEV        CONFIG_SYSLOG_DEVPATH
#else
#  define LOG_DEV        "/dev/kmsg"
#endif

#define LOG_MAX_LINES    1000          /* Kept; then the oldest go ... */
#define LOG_TRIM         200           /* ... this many at a time */
#define LOG_LINE_MAX     240           /* Longer lines are cut */
#define READER_STACK     2048
#define POLL_MS          500

#define TB_H             44            /* The toolbar */
#define ST_H             26            /* The status bar */
#define ROW_H            18            /* A row: the mono font's cell */
#define CELL_W           9
#define CELL_BASE        14            /* Its baseline below the top */
#define PAD_X            10
#define SB_W             12            /* The scroll bar */
#define FILTER_W         220

#define LEVEL_INFO       0
#define LEVEL_WARN       1
#define LEVEL_ERROR      2

/* Toolbar zones (desk_ui.h) */

#define Z_FOLLOW         1
#define Z_CLEAR          2
#define Z_COPY           3
#define Z_FILTER         4

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* What the window shares with its reader thread.  Once quit is set, the
 * thread owns it (and frees it).
 */

struct log_io_s
{
  pthread_mutex_t lock;
  sem_t go;                            /* The main thread has read */
  struct desk_win_s *w;
  int fd;
  bool quit;
};

struct log_line_s
{
  uint8_t level;
  uint16_t len;
  char text[1];
};

/* A row of the list: part of a line (by its number, seq) */

struct log_row_s
{
  uint32_t seq;
  uint16_t off;
  uint16_t len;
};

struct log_s
{
  struct log_io_s *io;
  bool nolog;                          /* No RAM log to read */

  /* The lines: line[i] is number first + i */

  struct log_line_s **line;
  int nline;
  int capline;
  uint32_t first;
  char part[LOG_LINE_MAX];             /* A line still coming */
  int npart;
  int nerrs;
  int nwarns;

  /* The rows of the lines the filter matches, wrapped at cols */

  struct log_row_s *row;
  int nrow;
  int caprow;
  int cols;

  /* The view */

  int top;                             /* First row shown */
  bool follow;                         /* Kept at the end */
  bool sel;                            /* Lines sela..selb are selected */
  uint32_t sela;
  uint32_t selb;
  uint32_t anchor;
  char msg[48];                        /* In the status bar, for a while */
  int msgticks;

  /* The layout (frame coordinates) */

  int lx;                              /* The list */
  int ly;
  int lw;
  int lh;
  int vis;                             /* Rows that fit */
  int sbx;
  int sbty;                            /* The thumb as drawn */
  int sbth;

  /* The pointer and the keys */

  uint8_t buttons;
  bool dragsel;
  bool sbdrag;
  int sbgrab;
  struct ui_zones_s zs;
  struct ui_field_s filter;
  struct ui_keys_s keys;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* The operations being built: from the heap while a window is open */

static struct desk_ops_s *g_op;
static NXWINDOW g_hwnd;

/****************************************************************************
 * Private Functions: the lines
 ****************************************************************************/

static bool has_word(const char *s, int n, const char *word)
{
  int wl = strlen(word);
  int i;
  int j;

  for (i = 0; i + wl <= n; i++)
    {
      for (j = 0; j < wl && tolower((unsigned char)s[i + j]) == word[j];
           j++)
        {
        }

      if (j == wl)
        {
          return true;
        }
    }

  return false;
}

/* What a line is: an error or a warning by its words (or by the priority
 * CONFIG_SYSLOG_PRIORITY puts in front)
 */

static uint8_t line_level(const char *s, int n)
{
  static const char * const errors[] =
  {
    "error", "fail", "fatal", "panic", "assert", "cannot", "can't",
    "unable", "[emerg", "[alert", "[crit", "[  err", "abort", "fault"
  };

  static const char * const warnings[] =
  {
    "warn", "not found", "timeout", "timed out", "retry", "gone"
  };

  size_t i;

  for (i = 0; i < sizeof(errors) / sizeof(errors[0]); i++)
    {
      if (has_word(s, n, errors[i]))
        {
          return LEVEL_ERROR;
        }
    }

  for (i = 0; i < sizeof(warnings) / sizeof(warnings[0]); i++)
    {
      if (has_word(s, n, warnings[i]))
        {
          return LEVEL_WARN;
        }
    }

  return LEVEL_INFO;
}

static bool line_matches(struct log_s *l, const struct log_line_s *ln)
{
  return l->filter.len == 0 ||
         has_word(ln->text, ln->len, l->filter.text);
}

static struct log_line_s *line_get(struct log_s *l, uint32_t seq)
{
  return seq >= l->first && seq - l->first < (uint32_t)l->nline ?
         l->line[seq - l->first] : NULL;
}

/* The rows of one line, at the end */

static void rows_add(struct log_s *l, uint32_t seq)
{
  struct log_line_s *ln = line_get(l, seq);
  int off = 0;

  if (ln == NULL || !line_matches(l, ln))
    {
      return;
    }

  do
    {
      int len = ln->len - off > l->cols ? l->cols : ln->len - off;

      if (l->nrow == l->caprow)
        {
          int cap = l->caprow > 0 ? 2 * l->caprow : 64;
          struct log_row_s *r = realloc(l->row, cap * sizeof(*r));

          if (r == NULL)
            {
              return;
            }

          l->row    = r;
          l->caprow = cap;
        }

      l->row[l->nrow].seq = seq;
      l->row[l->nrow].off = off;
      l->row[l->nrow].len = len;
      l->nrow++;
      off += len;
    }
  while (off < ln->len);
}

static void rows_build(struct log_s *l)
{
  int i;

  l->nrow = 0;
  for (i = 0; i < l->nline; i++)
    {
      rows_add(l, l->first + i);
    }
}

/* A line has come: kept (the oldest go when there are too many) */

static void line_add(struct log_s *l, const char *s, int n)
{
  struct log_line_s *ln;
  int i;

  if (l->nline == l->capline)
    {
      int cap = l->capline > 0 ? 2 * l->capline : 64;
      struct log_line_s **p;

      cap = cap > LOG_MAX_LINES + LOG_TRIM ? LOG_MAX_LINES + LOG_TRIM : cap;
      p   = realloc(l->line, cap * sizeof(*p));
      if (p == NULL)
        {
          return;
        }

      l->line    = p;
      l->capline = cap;
    }

  ln = malloc(sizeof(*ln) + n);
  if (ln == NULL)
    {
      return;
    }

  for (i = 0; i < n; i++)
    {
      uint8_t c = (uint8_t)s[i];

      ln->text[i] = c == '\t' ? ' ' : c < 0x20 || c > 0x7e ? '?' : c;
    }

  ln->text[n] = '\0';
  ln->len     = n;
  ln->level   = line_level(ln->text, n);
  l->nerrs    += ln->level == LEVEL_ERROR;
  l->nwarns   += ln->level == LEVEL_WARN;
  l->line[l->nline++] = ln;
  rows_add(l, l->first + l->nline - 1);

  /* Too many: the oldest go, with their rows */

  if (l->nline >= LOG_MAX_LINES + LOG_TRIM)
    {
      int r = 0;

      for (i = 0; i < LOG_TRIM; i++)
        {
          l->nerrs  -= l->line[i]->level == LEVEL_ERROR;
          l->nwarns -= l->line[i]->level == LEVEL_WARN;
          free(l->line[i]);
        }

      memmove(l->line, l->line + LOG_TRIM,
              (l->nline - LOG_TRIM) * sizeof(l->line[0]));
      l->nline -= LOG_TRIM;
      l->first += LOG_TRIM;

      while (r < l->nrow && l->row[r].seq < l->first)
        {
          r++;
        }

      memmove(l->row, l->row + r, (l->nrow - r) * sizeof(l->row[0]));
      l->nrow -= r;
      l->top   = l->top > r ? l->top - r : 0;
    }
}

static void lines_clear(struct log_s *l)
{
  int i;

  for (i = 0; i < l->nline; i++)
    {
      free(l->line[i]);
    }

  l->first += l->nline;
  l->nline  = 0;
  l->nrow   = 0;
  l->top    = 0;
  l->nerrs   = 0;
  l->nwarns  = 0;
  l->sel    = false;
}

/* Everything the log has for us: split into lines */

static void log_read(struct log_s *l)
{
  char buf[256];
  ssize_t n;
  int i;

  if (l->io == NULL)
    {
      return;
    }

  while ((n = read(l->io->fd, buf, sizeof(buf))) > 0)
    {
      for (i = 0; i < n; i++)
        {
          if (buf[i] == '\r')
            {
              continue;
            }

          if (buf[i] == '\n' || l->npart == LOG_LINE_MAX)
            {
              line_add(l, l->part, l->npart);
              l->npart = 0;
              if (buf[i] == '\n')
                {
                  continue;
                }
            }

          l->part[l->npart++] = buf[i];
        }
    }
}

/****************************************************************************
 * Private Functions: the reader thread
 ****************************************************************************/

static void *log_reader(void *arg)
{
  struct log_io_s *io = arg;

  for (; ; )
    {
      struct pollfd p;
      bool wait = false;

      p.fd      = io->fd;
      p.events  = POLLIN;
      p.revents = 0;
      poll(&p, 1, POLL_MS);

      pthread_mutex_lock(&io->lock);
      if (io->quit)
        {
          pthread_mutex_unlock(&io->lock);
          break;
        }

      if ((p.revents & POLLIN) != 0)
        {
          desk_wake(io->w);
          wait = true;
        }

      pthread_mutex_unlock(&io->lock);

      /* Until the main thread has read it (else poll() says the same) */

      if (wait)
        {
          while (sem_wait(&io->go) < 0 && errno == EINTR)
            {
            }
        }
    }

  close(io->fd);
  sem_destroy(&io->go);
  pthread_mutex_destroy(&io->lock);
  free(io);
  return NULL;
}

/****************************************************************************
 * Private Functions: drawing
 ****************************************************************************/

static void need(int n)
{
  ui_flush(g_hwnd, g_op, n);
}

static void log_layout(struct desk_win_s *w, struct log_s *l)
{
  int cols;

  l->lx   = 0;
  l->ly   = TITLE_H + TB_H;
  l->sbx  = w->csize.w - SB_W;
  l->lw   = l->sbx - l->lx;
  l->lh   = w->csize.h - TB_H - ST_H;
  l->lh   = l->lh < ROW_H ? ROW_H : l->lh;
  l->vis  = l->lh / ROW_H;
  cols    = (l->lw - 2 * PAD_X) / CELL_W;
  cols    = cols < 8 ? 8 : cols;
  if (cols != l->cols)
    {
      l->cols = cols;
      rows_build(l);
    }
}

static int max_top(struct log_s *l)
{
  return l->nrow > l->vis ? l->nrow - l->vis : 0;
}

static uint16_t list_bg(void)
{
  return g_theme.field;
}

static bool row_selected(struct log_s *l, int r)
{
  return l->sel && r >= 0 && r < l->nrow && l->row[r].seq >= l->sela &&
         l->row[r].seq <= l->selb;
}

/* Rows r0 to r1 (list rows, not all shown) */

static void rows_draw(struct log_s *l, int r0, int r1)
{
  char text[LOG_LINE_MAX + 1];
  int r;

  r0 = r0 < l->top ? l->top : r0;
  r1 = r1 > l->top + l->vis + 1 ? l->top + l->vis + 1 : r1;
  for (r = r0; r < r1; r++)
    {
      int y = l->ly + (r - l->top) * ROW_H;
      bool sel = row_selected(l, r);
      struct log_line_s *ln;
      uint16_t fg;
      int start;
      int h = ROW_H;

      if (y >= l->ly + l->lh)
        {
          break;
        }

      if (r >= l->nrow || (ln = line_get(l, l->row[r].seq)) == NULL)
        {
          break;                        /* Past the end: list_draw()'s */
        }

      h = y + h > l->ly + l->lh ? l->ly + l->lh - y : h;
      need(l->cols + 4);
      start = g_op->n;
      ops_fill(g_op, l->lx, y, l->lw, h, sel ? C_SELECTION : list_bg());

      fg = ln->level == LEVEL_ERROR ? g_theme.error :
           ln->level == LEVEL_WARN ? g_theme.warning : g_theme.text;
      if (ln->level != LEVEL_INFO)
        {
          ops_fill(g_op, l->lx + 2, y, 3, h, fg);
        }

      memcpy(text, ln->text + l->row[r].off, l->row[r].len);
      text[l->row[r].len] = '\0';
      ops_text(g_op, &g_desk_font_mono, l->lx + PAD_X,
               y + CELL_BASE - g_desk_font_mono.ascent, text, fg, 255);
      ui_clip(g_op, start, l->lx, l->ly, l->lx + l->lw, l->ly + l->lh);
    }
}

static void scrollbar_draw(struct log_s *l, bool force)
{
  int th;
  int ty;

  if (l->nrow <= l->vis)
    {
      th = 0;
      ty = 0;
    }
  else
    {
      th = l->lh * l->vis / l->nrow;
      th = th < 24 ? 24 : th;
      ty = (l->lh - th) * l->top / max_top(l);
    }

  if (!force && th == l->sbth && ty == l->sbty)
    {
      return;
    }

  l->sbth = th;
  l->sbty = ty;
  need(2);
  ops_fill(g_op, l->sbx, l->ly, SB_W, l->lh, list_bg());
  if (th > 0)
    {
      ops_rrect(g_op, l->sbx + 3, l->ly + ty + 2, 6, th - 4, LUT_R3, 3,
                C_THUMB, 200);
    }
}

static void list_draw(struct log_s *l)
{
  need(2);
  ops_fill(g_op, l->lx, l->ly, l->lw, l->lh, list_bg());
  if (l->nolog)
    {
      ops_text_centre(g_op, &g_desk_font_ui, l->lx + l->lw / 2,
                      l->ly + 30, "No system log to read: the kernel needs "
                      "CONFIG_RAMLOG_SYSLOG (" LOG_DEV ")",
                      g_theme.text_dim, 255);
    }
  else if (l->nrow == 0)
    {
      ops_text_centre(g_op, &g_desk_font_ui, l->lx + l->lw / 2,
                      l->ly + 30, l->nline == 0 ? "Nothing logged yet" :
                      "No lines match the filter", g_theme.text_dim, 255);
    }

  rows_draw(l, l->top, l->top + l->vis + 1);
  scrollbar_draw(l, true);
}

/* Show rows from top: the rows still shown move with one COPY, the rest
 * are drawn, and any from row from on (new ones)
 */

static void scroll_to(struct log_s *l, int top, int from)
{
  int d;

  top = top > max_top(l) ? max_top(l) : top < 0 ? 0 : top;
  d   = top - l->top;
  if (d == 0)
    {
      if (from < l->nrow)
        {
          rows_draw(l, from, l->nrow);
          scrollbar_draw(l, false);
        }

      return;
    }

  if (d >= l->vis || -d >= l->vis)
    {
      l->top = top;
      list_draw(l);
      return;
    }

  need(1);
  {
    struct nx_gpuop_s *op = ops_add(g_op);
    int px = (d > 0 ? d : -d) * ROW_H;

    op->op = NX_GPUOP_COPY;
    op->x  = l->lx;
    op->w  = l->lw;
    op->h  = l->vis * ROW_H - px;
    op->sx = l->lx;
    op->y  = d > 0 ? l->ly : l->ly + px;
    op->sy = d > 0 ? l->ly + px : l->ly;
  }

  l->top = top;
  if (d > 0)
    {
      int r0 = top + l->vis - d;

      rows_draw(l, from < r0 ? from : r0, top + l->vis + 1);
    }
  else
    {
      rows_draw(l, top, top - d);
      rows_draw(l, top + l->vis, top + l->vis + 1);    /* The part row */
    }

  scrollbar_draw(l, false);
}

static void toolbar_draw(struct desk_win_s *w, struct log_s *l)
{
  int y = TITLE_H + (TB_H - 28) / 2;
  int x = 12;
  int bw;

  ui_zones_clear(&l->zs, 0);
  need(40);
  ops_fill(g_op, 0, TITLE_H, w->csize.w, TB_H, C_UI_BAR);
  ops_fill(g_op, 0, TITLE_H + TB_H - 1, w->csize.w, 1, C_UI_LINE);

  bw = ui_button_width("Pause");
  ui_button(g_op, x, y, bw, 28, l->follow ? "Pause" : "Follow",
            l->follow ? ui_look(&l->zs, Z_FOLLOW, true) : UI_DEFAULT);
  ui_zone_add(&l->zs, 0, Z_FOLLOW, x, y, bw, 28, true);
  x += bw + 8;

  bw = ui_button_width("Clear");
  ui_button(g_op, x, y, bw, 28, "Clear",
            ui_look(&l->zs, Z_CLEAR, l->nline > 0));
  ui_zone_add(&l->zs, 0, Z_CLEAR, x, y, bw, 28, l->nline > 0);
  x += bw + 8;

  bw = ui_button_width("Copy");
  ui_button(g_op, x, y, bw, 28, "Copy",
            ui_look(&l->zs, Z_COPY, l->nrow > 0));
  ui_zone_add(&l->zs, 0, Z_COPY, x, y, bw, 28, l->nrow > 0);

  x = w->csize.w - 12 - FILTER_W;
  need(LOG_LINE_MAX / 4);
  ui_field_draw(g_op, &l->filter, x, y, FILTER_W, 28, true);
  if (l->filter.len == 0)
    {
      ops_text(g_op, &g_desk_font_ui, x + 10,
               y + (28 - text_height(&g_desk_font_ui)) / 2, "Filter",
               g_theme.text_faint, 255);
    }

  ui_zone_add(&l->zs, 0, Z_FILTER, x, y, FILTER_W, 28, true);
}

static void status_draw(struct desk_win_s *w, struct log_s *l)
{
  const struct desk_font_s *f = &g_desk_font_ui;
  int y = TITLE_H + w->csize.h - ST_H;
  int ty = y + (ST_H - text_height(f)) / 2;
  char buf[64];

  need(80);
  ops_fill(g_op, 0, y, w->csize.w, ST_H, C_UI_BAR);
  ops_fill(g_op, 0, y, w->csize.w, 1, C_UI_LINE);
  if (l->msgticks > 0)
    {
      snprintf(buf, sizeof(buf), "%s", l->msg);
    }
  else if (l->filter.len > 0)
    {
      snprintf(buf, sizeof(buf), "%d lines, %d rows shown", l->nline,
               l->nrow);
    }
  else
    {
      snprintf(buf, sizeof(buf), "%d lines", l->nline);
    }

  ops_text(g_op, f, 14, ty, buf, g_theme.text_dim, 255);

  snprintf(buf, sizeof(buf), "%d error%s  %d warning%s  %s", l->nerrs,
           l->nerrs == 1 ? "" : "s", l->nwarns, l->nwarns == 1 ? "" : "s",
           l->follow ? "Following" : "Paused");
  ops_text(g_op, f, w->csize.w - 14 - text_width(f, buf), ty, buf,
           g_theme.text_dim, 255);
  if (l->nerrs > 0)
    {
      ops_disc(g_op, w->csize.w - 22 - text_width(f, buf), y + ST_H / 2,
               LUT_R3, 3, g_theme.error, 255);
    }
}

static void log_draw(struct desk_win_s *w)
{
  struct log_s *l = w->priv;

  if (l == NULL)
    {
      return;
    }

  g_hwnd = (NXWINDOW)w->hwnd;
  ops_reset(g_op);
  log_layout(w, l);
  if (l->follow || l->top > max_top(l))
    {
      l->top = max_top(l);
    }

  toolbar_draw(w, l);
  list_draw(l);
  status_draw(w, l);
  ops_draw(g_hwnd, g_op);
}

/* After new lines or a new filter: the list where it should be */

static void log_update(struct desk_win_s *w, struct log_s *l, int oldrows)
{
  g_hwnd = (NXWINDOW)w->hwnd;
  ops_reset(g_op);
  if (oldrows == 0 && l->nrow > 0)
    {
      /* The first lines: "Nothing logged yet" goes with the rest */

      l->top = l->follow ? max_top(l) : 0;
      list_draw(l);
    }
  else if (l->follow)
    {
      scroll_to(l, max_top(l), oldrows);
    }
  else
    {
      scrollbar_draw(l, false);
    }

  status_draw(w, l);
  toolbar_draw(w, l);
  ops_draw(g_hwnd, g_op);
}

/****************************************************************************
 * Private Functions: acting
 ****************************************************************************/

static void set_follow(struct desk_win_s *w, struct log_s *l, bool follow)
{
  if (l->follow != follow)
    {
      l->follow = follow;
      g_hwnd = (NXWINDOW)w->hwnd;
      ops_reset(g_op);
      if (follow)
        {
          scroll_to(l, max_top(l), INT_MAX);
        }

      toolbar_draw(w, l);
      status_draw(w, l);
      ops_draw(g_hwnd, g_op);
    }
}

/* Scroll by the user: away from the end, it stops following */

static void user_scroll(struct desk_win_s *w, struct log_s *l, int top)
{
  top = top > max_top(l) ? max_top(l) : top < 0 ? 0 : top;
  g_hwnd = (NXWINDOW)w->hwnd;
  ops_reset(g_op);
  scroll_to(l, top, INT_MAX);
  ops_draw(g_hwnd, g_op);
  set_follow(w, l, top >= max_top(l));
}

static void refilter(struct desk_win_s *w, struct log_s *l)
{
  rows_build(l);
  l->top = l->follow ? max_top(l) : 0;
  log_draw(w);
}

static void log_copy(struct desk_win_s *w, struct log_s *l)
{
  size_t size = 1;
  size_t n = 0;
  char *buf;
  int lines = 0;
  int i;

  for (i = 0; i < l->nline; i++)
    {
      size += l->line[i]->len + 1;
    }

  buf = malloc(size);
  if (buf == NULL)
    {
      return;
    }

  for (i = 0; i < l->nline; i++)
    {
      struct log_line_s *ln = l->line[i];
      uint32_t seq = l->first + i;

      if (!line_matches(l, ln) ||
          (l->sel && (seq < l->sela || seq > l->selb)))
        {
          continue;
        }

      memcpy(buf + n, ln->text, ln->len);
      n += ln->len;
      buf[n++] = '\n';
      lines++;
    }

  desk_clip_set(buf, n);
  free(buf);
  snprintf(l->msg, sizeof(l->msg), "Copied %d line%s", lines,
           lines == 1 ? "" : "s");
  l->msgticks = 3;
  g_hwnd = (NXWINDOW)w->hwnd;
  ops_reset(g_op);
  status_draw(w, l);
  ops_draw(g_hwnd, g_op);
}

static void select_lines(struct desk_win_s *w, struct log_s *l, bool sel,
                         uint32_t a, uint32_t b)
{
  l->sel  = sel;
  l->sela = a < b ? a : b;
  l->selb = a < b ? b : a;
  g_hwnd  = (NXWINDOW)w->hwnd;
  ops_reset(g_op);
  rows_draw(l, l->top, l->top + l->vis + 1);
  ops_draw(g_hwnd, g_op);
}

static void log_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct log_s *l = w->priv;
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(l->buttons & NX_MOUSE_LEFTBUTTON);
  bool release = !(buttons & NX_MOUSE_LEFTBUTTON) &&
                 (l->buttons & NX_MOUSE_LEFTBUTTON);
  struct ui_zone_s *z;
  int hot;

  if (l == NULL)
    {
      return;
    }

  l->buttons = buttons;
  y += TITLE_H;

  /* Dragging the thumb, or a selection */

  if (l->sbdrag)
    {
      int span = l->lh - l->sbth;

      if (span > 0)
        {
          user_scroll(w, l, (y - l->ly - l->sbgrab) * max_top(l) / span);
        }

      l->sbdrag = !release;
      return;
    }

  if (l->dragsel)
    {
      int r = l->top + (y - l->ly) / ROW_H;

      r = y < l->ly ? l->top - 1 : r;
      if (r >= 0 && r < l->nrow)
        {
          if (y < l->ly || y >= l->ly + l->lh)
            {
              user_scroll(w, l, y < l->ly ? l->top - 1 : l->top + 1);
            }

          select_lines(w, l, true, l->anchor, l->row[r].seq);
        }

      l->dragsel = !release;
      return;
    }

  /* The toolbar */

  z   = x >= 0 ? ui_zone_at(&l->zs, x, y) : NULL;
  hot = z != NULL && z->enabled ? z->id : 0;
  if (hot != l->zs.hot || (press && hot != 0) ||
      (release && l->zs.pressed != 0))
    {
      int pressed = l->zs.pressed;

      l->zs.hot     = hot;
      l->zs.pressed = press ? hot : release ? 0 : l->zs.pressed;
      g_hwnd = (NXWINDOW)w->hwnd;
      ops_reset(g_op);
      toolbar_draw(w, l);
      ops_draw(g_hwnd, g_op);

      if (release && pressed == hot && hot != 0)
        {
          switch (hot)
            {
              case Z_FOLLOW:
                set_follow(w, l, !l->follow);
                break;

              case Z_CLEAR:
                lines_clear(l);
                log_draw(w);
                break;

              case Z_COPY:
                log_copy(w, l);
                break;
            }
        }
    }

  if (!press || hot != 0)
    {
      return;
    }

  /* The scroll bar: the thumb is dragged, the track pages */

  if (x >= l->sbx && y >= l->ly && y < l->ly + l->lh)
    {
      if (l->sbth > 0 && y >= l->ly + l->sbty &&
          y < l->ly + l->sbty + l->sbth)
        {
          l->sbdrag = true;
          l->sbgrab = y - l->ly - l->sbty;
        }
      else if (l->sbth > 0)
        {
          user_scroll(w, l, y < l->ly + l->sbty ? l->top - l->vis :
                                                  l->top + l->vis);
        }

      return;
    }

  /* A line: selected (a drag selects more); a click past the end none */

  if (y >= l->ly && y < l->ly + l->lh)
    {
      int r = l->top + (y - l->ly) / ROW_H;

      if (r < l->nrow)
        {
          l->anchor  = l->row[r].seq;
          l->dragsel = true;
          select_lines(w, l, true, l->anchor, l->anchor);
        }
      else if (l->sel)
        {
          select_lines(w, l, false, 0, 0);
        }
    }
}

static void log_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct log_s *l = w->priv;
  int keys[16];
  int nk;
  int i;

  if (l == NULL)
    {
      return;
    }

  nk = ui_keys(&l->keys, ch, n, keys, 16);
  for (i = 0; i < nk; i++)
    {
      switch (keys[i] & KEY_MASK)
        {
          case KEY_UP:
            user_scroll(w, l, l->top - 1);
            break;

          case KEY_DOWN:
            user_scroll(w, l, l->top + 1);
            break;

          case KEY_PGUP:
            user_scroll(w, l, l->top - l->vis);
            break;

          case KEY_PGDN:
            user_scroll(w, l, l->top + l->vis);
            break;

          case KEY_HOME:
            user_scroll(w, l, 0);
            break;

          case KEY_END:
            set_follow(w, l, true);
            break;

          case KEY_CTL('c'):
            log_copy(w, l);
            break;

          case KEY_CTL('a'):
            if (l->nline > 0)
              {
                select_lines(w, l, true, l->first,
                             l->first + l->nline - 1);
              }
            break;

          case KEY_ESC:
            if (l->filter.len > 0)
              {
                ui_field_set(&l->filter, "");
                refilter(w, l);
              }
            else if (l->sel)
              {
                select_lines(w, l, false, 0, 0);
              }
            break;

          default:
            if ((keys[i] & KEY_MASK) != KEY_CTL('a') &&
                ui_field_key(&l->filter, keys[i]))
              {
                refilter(w, l);
              }
            break;
        }
    }
}

/* The thread says there is more */

static void log_wake(struct desk_win_s *w)
{
  struct log_s *l = w->priv;
  int oldrows;
  int oldtop;

  if (l == NULL || l->io == NULL)
    {
      return;
    }

  oldrows = l->nrow;
  oldtop  = l->top;
  log_read(l);
  sem_post(&l->io->go);

  if (l->top != oldtop || l->nrow < oldrows)
    {
      log_draw(w);                           /* Old lines went */
    }
  else if (l->nrow != oldrows)
    {
      log_update(w, l, oldrows);
    }
}

static void log_tick(struct desk_win_s *w)
{
  struct log_s *l = w->priv;

  if (l != NULL && l->msgticks > 0 && --l->msgticks == 0)
    {
      g_hwnd = (NXWINDOW)w->hwnd;
      ops_reset(g_op);
      status_draw(w, l);
      ops_draw(g_hwnd, g_op);
    }
}

static void log_close(struct desk_win_s *w)
{
  struct log_s *l = w->priv;
  int i;

  if (l == NULL)
    {
      return;
    }

  if (l->io != NULL)
    {
      pthread_mutex_lock(&l->io->lock);
      l->io->quit = true;
      pthread_mutex_unlock(&l->io->lock);
      sem_post(&l->io->go);
      l->io = NULL;                          /* The thread's now */
    }

  for (i = 0; i < l->nline; i++)
    {
      free(l->line[i]);
    }

  free(l->line);
  free(l->row);
  free(g_op);
  g_op = NULL;
}

static struct desk_win_s *log_open(void)
{
  struct desk_win_s *w;
  struct log_s *l;
  pthread_attr_t attr;
  pthread_t thread;
  int fd;

  l    = calloc(1, sizeof(*l));
  g_op = malloc(sizeof(*g_op));
  if (l == NULL || g_op == NULL)
    {
      free(l);
      free(g_op);
      g_op = NULL;
      return NULL;
    }

  l->follow = true;
  l->zs.hot = 0;
  w = desk_win_open(APP_LOG, "System Log", 760, 460);
  if (w == NULL)
    {
      free(l);
      free(g_op);
      g_op = NULL;
      return NULL;
    }

  w->priv = l;
  log_layout(w, l);

  fd = open(LOG_DEV, O_RDONLY | O_NONBLOCK);
  if (fd < 0)
    {
      l->nolog = true;
    }
  else
    {
      l->io = calloc(1, sizeof(*l->io));
      if (l->io == NULL)
        {
          close(fd);
          l->nolog = true;
        }
      else
        {
          l->io->fd = fd;
          l->io->w  = w;
          pthread_mutex_init(&l->io->lock, NULL);
          sem_init(&l->io->go, 0, 0);
          log_read(l);                        /* What it has already */

          pthread_attr_init(&attr);
          pthread_attr_setstacksize(&attr, READER_STACK);
          if (pthread_create(&thread, &attr, log_reader, l->io) != 0)
            {
              close(fd);
              sem_destroy(&l->io->go);
              pthread_mutex_destroy(&l->io->lock);
              free(l->io);
              l->io = NULL;
            }
          else
            {
              pthread_detach(thread);
            }
        }
    }

  log_draw(w);
  return w;
}

/* A dark tile with lines of text, a red and a yellow one among them */

static void log_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  static const uint8_t len[5] =
  {
    70, 50, 80, 40, 60
  };

  static const uint16_t col[5] =
  {
    RGB(0xd0, 0xd0, 0xd8), RGB(0xd0, 0xd0, 0xd8), RGB(0xff, 0x5f, 0x57),
    RGB(0xfe, 0xbc, 0x2e), RGB(0xd0, 0xd0, 0xd8)
  };

  int i;

  icon_tile(o, x, y, s, RGB(0x4a, 0x4e, 0x5a), RGB(0x1e, 0x20, 0x28), a);
  for (i = 0; i < 5; i++)
    {
      int ly = y + s / 5 + i * s * 3 / 25;

      ops_blend(o, x + s / 5, ly, (s * 3 / 5) * len[i] / 100,
                s >= 40 ? 3 : 2, col[i], (uint8_t)(a * 230 / 255));
    }
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_log =
{
  .name   = "System Log",
  .icon   = log_icon,
  .open   = log_open,
  .draw   = log_draw,
  .mouse  = log_mouse,
  .key    = log_key,
  .tick   = log_tick,
  .wake   = log_wake,
  .close  = log_close,
  .single = true,
};
