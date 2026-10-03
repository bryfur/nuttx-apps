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
 *
 * The tool bar, the filter, the scroll bar and the status bar are the
 * widget kit's (desk_kit.h); the list is the log's own, a custom widget
 * of the kit that it draws itself.
 *
 * A program of its own (/mnt/sd0/bin/syslog, apps/graphics/deskapps):
 * built into the desktop (DESK_PROGRAM_STUB) this file is only the icon
 * and the name the dock shows (desk_program.c).
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
#include "desk_kit.h"

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

#define ROW_H            18            /* A row: the mono font's cell */
#define CELL_W           9
#define CELL_BASE        14            /* Its baseline below the top */
#define PAD_X            10
#define FILTER_W         220

#define LEVEL_INFO       0
#define LEVEL_WARN       1
#define LEVEL_ERROR      2

#define LOG_NWG          16            /* The kit's widgets */

/* The widgets' ids */

#define L_FOLLOW         1
#define L_CLEAR          2
#define L_COPY           3
#define L_FILTER         4
#define L_LIST           5
#define L_SCROLL         6
#define L_STATUS         7
#define L_DOT            8
#define L_COUNTS         9

#ifndef DESK_PROGRAM_STUB

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
  pthread_t reader;
  bool join;                           /* A program's: its end waits for
                                        * the reader (else it would end it
                                        * before it had let go) */
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

  /* The tool bar, the scroll bar, the status bar: the widget kit's */

  struct kit_s kit;
  struct kit_widget_s wg[LOG_NWG];
  struct ui_field_s filter;
  char status[64];
  char counts[64];

  /* The pointer on the list */

  uint8_t buttons;
  bool dragsel;
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

/* Where the list is: the kit's custom widget (after kit_layout()) */

static void log_layout(struct desk_win_s *w, struct log_s *l)
{
  struct kit_widget_s *wg = kit_get(&l->kit, L_LIST);
  int cols;

  l->lx   = wg->x;
  l->ly   = wg->y;
  l->lw   = wg->w;
  l->lh   = wg->h < ROW_H ? ROW_H : wg->h;
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
}

/* The tool bar, the scroll bar and the status bar: what they show, into
 * the kit's widgets (only what changed is drawn again)
 */

static void log_sync(struct log_s *l)
{
  struct kit_s *k = &l->kit;
  char buf[64];

  kit_set_text(k, L_FOLLOW, l->follow ? "Pause" : "Follow");
  kit_set_style(k, L_FOLLOW, l->follow ? KIT_BTN_NORMAL : KIT_BTN_DEFAULT);
  kit_enable(k, L_CLEAR, l->nline > 0);
  kit_enable(k, L_COPY, l->nrow > 0);
  kit_scroll_set(k, L_SCROLL, l->top, l->vis, l->nrow);

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

  if (strcmp(buf, l->status) != 0)
    {
      strlcpy(l->status, buf, sizeof(l->status));
      kit_invalidate(k, L_STATUS);
    }

  snprintf(buf, sizeof(buf), "%d error%s  %d warning%s  %s", l->nerrs,
           l->nerrs == 1 ? "" : "s", l->nwarns, l->nwarns == 1 ? "" : "s",
           l->follow ? "Following" : "Paused");
  if (strcmp(buf, l->counts) != 0)
    {
      strlcpy(l->counts, buf, sizeof(l->counts));
      kit_set_text(k, L_COUNTS, l->counts);
    }

  kit_show(k, L_DOT, l->nerrs > 0);
}

/* Send what was drawn: the list's operations, then the kit's */

static void log_flush(struct desk_win_s *w, struct log_s *l)
{
  ops_draw(g_hwnd, g_op);
  log_sync(l);
  kit_flush(&l->kit);
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

  log_sync(l);
  kit_redraw(&l->kit);                       /* The list too: log_paint() */
}

/* The kit's draw function: the list, when the kit draws what is under it
 * again (an expose, a tooltip gone)
 */

static void log_paint(struct kit_s *k, struct kit_widget_s *wg, int x0,
                      int y0, int x1, int y1)
{
  struct log_s *l = k->priv;

  g_hwnd = (NXWINDOW)k->w->hwnd;
  list_draw(l);
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

  log_flush(w, l);
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

      log_flush(w, l);
    }
}

/* Scroll by the user: away from the end, it stops following */

static void user_scroll(struct desk_win_s *w, struct log_s *l, int top)
{
  top = top > max_top(l) ? max_top(l) : top < 0 ? 0 : top;
  g_hwnd = (NXWINDOW)w->hwnd;
  ops_reset(g_op);
  scroll_to(l, top, INT_MAX);
  log_flush(w, l);
  set_follow(w, l, top >= max_top(l));
}

static void refilter(struct desk_win_s *w, struct log_s *l)
{
  rows_build(l);
  l->top = l->follow ? max_top(l) : 0;
  g_hwnd = (NXWINDOW)w->hwnd;
  ops_reset(g_op);
  list_draw(l);
  log_flush(w, l);
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
  log_flush(w, l);
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

/* The pointer on the list (the kit hands it over while a button pressed
 * there is held): a line is selected, a drag selects more
 */

static void list_mouse(struct desk_win_s *w, struct log_s *l, int y,
                       uint8_t buttons)
{
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(l->buttons & NX_MOUSE_LEFTBUTTON);
  int r;

  l->buttons = buttons;
  if (press && y >= l->ly && y < l->ly + l->lh)
    {
      r = l->top + (y - l->ly) / ROW_H;
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

      return;
    }

  if (!l->dragsel)
    {
      return;
    }

  if (!(buttons & NX_MOUSE_LEFTBUTTON))
    {
      l->dragsel = false;
      return;
    }

  r = y < l->ly ? l->top - 1 : l->top + (y - l->ly) / ROW_H;
  if (r >= 0 && r < l->nrow)
    {
      if (y < l->ly || y >= l->ly + l->lh)
        {
          user_scroll(w, l, y < l->ly ? l->top - 1 : l->top + 1);
        }

      select_lines(w, l, true, l->anchor, l->row[r].seq);
    }
}

static void log_event(struct kit_s *k, struct kit_widget_s *wg, int ev)
{
  struct desk_win_s *w = k->w;
  struct log_s *l = k->priv;
  int id = wg != NULL ? wg->id : 0;

  switch (ev)
    {
      case KIT_EV_CLICK:
        if (id == L_FOLLOW)
          {
            set_follow(w, l, !l->follow);
          }
        else if (id == L_CLEAR)
          {
            lines_clear(l);
            log_draw(w);
          }
        else if (id == L_COPY)
          {
            log_copy(w, l);
          }
        break;

      case KIT_EV_CHANGE:
        if (id == L_FILTER)
          {
            refilter(w, l);
          }
        else if (id == L_SCROLL)
          {
            user_scroll(w, l, wg->value);
          }
        break;

      case KIT_EV_MOUSE:
        if (id == L_LIST)
          {
            list_mouse(w, l, k->my, k->mbuttons);
          }
        break;

      default:
        break;
    }
}

static void log_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct log_s *l = w->priv;

  if (l != NULL)
    {
      kit_mouse(&l->kit, x, y, buttons);
    }
}

/* Keys: the list's (scrolling, selecting, copying) first; the rest go to
 * the kit, and so to the filter
 */

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

  nk = kit_keys(&l->kit, ch, n, keys, 16);
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
                kit_invalidate(&l->kit, L_FILTER);
                refilter(w, l);
              }
            else if (l->sel)
              {
                select_lines(w, l, false, 0, 0);
              }
            break;

          default:
            kit_press_key(&l->kit, keys[i]);
            break;
        }
    }

  kit_flush(&l->kit);
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

  if (l == NULL)
    {
      return;
    }

  if (l->msgticks > 0)
    {
      l->msgticks--;
      log_sync(l);
    }

  kit_tick(&l->kit);
}

static void log_resize(struct desk_win_s *w)
{
  struct log_s *l = w->priv;

  if (l != NULL)
    {
      kit_layout(&l->kit);
      log_draw(w);
    }
}

static void log_expose(struct desk_win_s *w, int x, int y, int cw, int ch)
{
  struct log_s *l = w->priv;

  if (l != NULL)
    {
      kit_expose(&l->kit, x, y, cw, ch);
    }
}

static void log_focus(struct desk_win_s *w)
{
  struct log_s *l = w->priv;

  if (l != NULL)
    {
      kit_activate(&l->kit);
    }
}

/* The tool bar, the list and its scroll bar, the status bar */

static void log_build(struct desk_win_s *w, struct log_s *l)
{
  struct kit_s *k = &l->kit;
  struct kit_widget_s *wg;

  kit_init(k, w, l->wg, LOG_NWG, log_event);
  k->priv = l;
  k->draw = log_paint;

  kit_col(k, KIT_BOX_WINDOW);
    kit_row(k, KIT_BOX_BAR);
      kit_button(k, L_FOLLOW, "Pause", KIT_ICON_NONE, KIT_BTN_NORMAL);
      kit_size(k, 72, 0);
      kit_tip(k, "Keep showing the newest lines");
      kit_button(k, L_CLEAR, "Clear", KIT_ICON_NONE, KIT_BTN_NORMAL);
      kit_tip(k, "Empty the list (not the kernel's log)");
      kit_button(k, L_COPY, "Copy", KIT_ICON_NONE, KIT_BTN_NORMAL);
      kit_tip(k, "The lines selected, or all shown, to the clipboard");
      kit_spacer(k, 1);
      kit_field(k, L_FILTER, &l->filter, "Filter");
      kit_size(k, FILTER_W, 0);
    kit_end(k);

    wg = kit_row(k, KIT_BOX_NONE);
    wg->gap = 0;
    kit_grow(k, 1);
      kit_custom(k, L_LIST)->flags |= KIT_F_NOFOCUS;
      kit_grow(k, 1);
      kit_scroll(k, L_SCROLL);
    kit_end(k);

    kit_row(k, KIT_BOX_STATUS)->gap = 6;
      kit_label(k, L_STATUS, l->status, KIT_TEXT_DIM);
      kit_grow(k, 1);
      kit_label(k, L_DOT, DESK_CH_BULLET, KIT_TEXT_ERROR)->flags |=
        KIT_F_HIDDEN;
      kit_label(k, L_COUNTS, l->counts, KIT_TEXT_DIM);
    kit_end(k);
  kit_end(k);

  kit_focus(k, L_FILTER);
  kit_layout(k);
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
      if (l->join)
        {
          pthread_join(l->reader, NULL);
        }
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
  w = desk_win_open(APP_LOG, "System Log", 760, 460);
  if (w == NULL)
    {
      free(l);
      free(g_op);
      g_op = NULL;
      return NULL;
    }

  w->priv = l;
  log_build(w, l);
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
          if (pthread_create(&l->reader, &attr, log_reader, l->io) != 0)
            {
              close(fd);
              sem_destroy(&l->io->go);
              pthread_mutex_destroy(&l->io->lock);
              free(l->io);
              l->io = NULL;
            }
          else if (desk_program_self())
            {
              l->join = true;
            }
          else
            {
              pthread_detach(l->reader);
            }
        }
    }

  log_draw(w);
  return w;
}

#endif /* !DESK_PROGRAM_STUB */

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

#ifdef DESK_PROGRAM_STUB
DESK_PROGRAM_APP(g_app_log, "System Log", log_icon, "syslog", true);
#else
const struct desk_app_s g_app_log =
{
  .name   = "System Log",
  .icon   = log_icon,
  .open   = log_open,
  .draw   = log_draw,
  .expose = log_expose,
  .resize = log_resize,
  .mouse  = log_mouse,
  .key    = log_key,
  .tick   = log_tick,
  .wake   = log_wake,
  .focus  = log_focus,
  .close  = log_close,
  .single = true,
};
#endif
