/****************************************************************************
 * apps/graphics/fpcdesk/desk_ide.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The Python IDE: the Text Editor (desk_edit.c, with Python's colours and
 * indents) with more (struct edit_ext_s):
 *
 *   - a tool bar that starts with Run (F5), Stop, Open, Examples and
 *     Recent;
 *   - below the text, a console of two tabs: Output, where the program
 *     runs, and REPL, an interpreter of its own.  Each is MicroPython (the
 *     python builtin) as a task of its own on a pseudo-terminal, shown
 *     through the terminal's emulator (desk_vt.c): what it prints, what
 *     it asks for (input(): typed in the console), Ctrl-C;
 *   - Run saves the file (Save As first if it has no name) and runs it;
 *     Stop sends Ctrl-C (KeyboardInterrupt), then ends the task if it has
 *     not gone a second later;
 *   - when a program ends with a traceback, the editor goes to the line
 *     of the file it names last (the innermost frame in it); a click on a
 *     "File ..., line N" line of the output goes there too;
 *   - a program's windows (the fpc module) come and go beside it, as any
 *     program's (desk_client.c);
 *   - Examples: the examples the build carries (desk_pyexamples.c, made
 *     from apps/interpreters/micropython/examples), written to python/ in
 *     the home directory when one is chosen, and opened from there;
 *     Recent: the last files opened, run or saved (.python_recent in the
 *     home directory).
 *
 * The Python application's launcher tile and dock icon open one
 * (desk_python.c); its windows are APP_PYTHON's, which forwards to the
 * editor's callbacks and to desk_ide_wake().
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <nuttx/lib/builtin.h>

#include "desk.h"
#include "desk_kit.h"
#include "desk_vt.h"

#ifdef CONFIG_FPCDESK_PYTHON

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define IDE_W          720
#define IDE_H          560

#define I_RUN          (EDIT_EXT_ID + 0)
#define I_STOP         (EDIT_EXT_ID + 1)
#define I_OPEN         (EDIT_EXT_ID + 2)
#define I_EXAMPLES     (EDIT_EXT_ID + 3)
#define I_RECENT       (EDIT_EXT_ID + 4)
#define I_TABS         (EDIT_EXT_ID + 5)
#define I_CLEAR        (EDIT_EXT_ID + 6)
#define I_CONSOLE      (EDIT_EXT_ID + 7)
#define I_STATE        (EDIT_EXT_ID + 8)

#define D_OPEN         (EDIT_EXT_ID + 0)      /* Dialogs */
#define M_EXAMPLES     1                      /* Menus */
#define M_RECENT       2

#define T_OUTPUT       0
#define T_REPL         1

#define ROWS           9                      /* The console's */
#define CELL_W         9
#define CELL_H         18
#define CELL_BASE      14                     /* The baseline in a cell */
#define PAD            4
#define RING           2048
#define POLL_MS        300
#define READER_STACK   3072
#define STOP_MS        1000
#define MAX_RECENT     8
#define SCROLLBACK     400

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A python task on a pseudo-terminal; its output into a ring the main
 * thread empties.  Once quit is set, the reader owns it (and frees it).
 */

struct ide_io_s
{
  pthread_mutex_t lock;
  sem_t space;
  struct desk_win_s *w;
  int fd;                              /* The pty's master */
  pid_t pid;
  bool quit;
  bool eof;
  bool waiting;
  int head;
  int n;
  uint8_t ring[RING];
};

struct ide_s
{
  struct desk_win_s *w;
  struct vt_s vt[2];                   /* Output, REPL */
  struct ide_io_s *io[2];
  int tab;                             /* Shown */
  int view;                            /* Lines scrolled back */
  struct vt_line_s *shown[ROWS];       /* The line each row shows */
  int currow;                          /* Where the cursor was drawn, -1 */
  bool full;
  bool focused;                        /* The console has the keys */

  /* Running */

  char runpath[PATH_MAX];
  bool runpending;                     /* Run once saved */
  bool running;
  uint32_t stopms;                     /* Stop pressed: when, 0 */
  int errline;                         /* The traceback's line, 0 */
  char line[160];                      /* The output's line so far */
  int linelen;

  /* Menus */

  char recent[MAX_RECENT][PATH_MAX];
  int nrecent;
  const char *items[KIT_POP_MAX];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char * const g_tabs[2] =
{
  "Output", "REPL"
};

static const struct edit_ext_s g_ext;

/****************************************************************************
 * Private Functions: the console's tasks
 ****************************************************************************/

/* The task: python with the pty as its terminal (argv[1]: the pty, then
 * python's arguments; argv[1] becomes python's argv[0])
 */

static int ide_task(int argc, char *argv[])
{
  const struct builtin_s *b;
  int index;
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

  dup2(fd, 0);
  dup2(fd, 1);
  dup2(fd, 2);
  if (fd > 2)
    {
      close(fd);
    }

  index = builtin_isavail("python");
  b = index >= 0 ? builtin_for_index(index) : NULL;
  if (b == NULL || b->main == NULL)
    {
      printf("python is not in this system\n");
      return EXIT_FAILURE;
    }

  argv[1] = "python";
  return b->main(argc - 1, argv + 1);
}

static void *ide_reader(void *arg)
{
  struct ide_io_s *io = arg;

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

          nread = 0;                           /* It has gone */
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

/* A python task for tab t: python FILE, or the REPL (file NULL) */

static struct ide_io_s *io_start(struct ide_s *ide, const char *file)
{
  const struct builtin_s *b;
  struct ide_io_s *io;
  struct winsize ws;
  pthread_attr_t attr;
  pthread_t thread;
  char path[32];
  char *argv[4];
  int index;
  int one = 1;

  index = builtin_isavail("python");
  b = index >= 0 ? builtin_for_index(index) : NULL;
  io = b != NULL ? calloc(1, sizeof(*io)) : NULL;
  if (io == NULL)
    {
      return NULL;
    }

  pthread_mutex_init(&io->lock, NULL);
  sem_init(&io->space, 0, 0);
  io->w   = ide->w;
  io->pid = -1;
  io->fd  = posix_openpt(O_RDWR | O_NOCTTY);
  if (io->fd < 0 || unlockpt(io->fd) < 0 ||
      ptsname_r(io->fd, path, sizeof(path)) < 0)
    {
      goto fail;
    }

  fcntl(io->fd, F_SETFD, FD_CLOEXEC);
  ioctl(io->fd, FIONBIO, (unsigned long)(uintptr_t)&one);
  memset(&ws, 0, sizeof(ws));
  ws.ws_col = ide->vt[0].cols;
  ws.ws_row = ROWS;
  ioctl(io->fd, TIOCSWINSZ, (unsigned long)(uintptr_t)&ws);

  argv[0] = path;
  argv[1] = (char *)file;
  argv[2] = NULL;

  io->pid = task_create("python", b->priority, b->stacksize, ide_task,
                        argv);
  if (io->pid < 0)
    {
      io->pid = -1;
      goto fail;
    }

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, READER_STACK);
  pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
  if (pthread_create(&thread, &attr, ide_reader, io) != 0)
    {
      task_delete(io->pid);
      io->pid = -1;
      goto fail;
    }

  return io;

fail:
  if (io->fd >= 0)
    {
      close(io->fd);
    }

  sem_destroy(&io->space);
  pthread_mutex_destroy(&io->lock);
  free(io);
  return NULL;
}

/* A task's console let go of: the task is ended if it is still there */

static void io_free(struct ide_io_s *io)
{
  if (io == NULL)
    {
      return;
    }

  pthread_mutex_lock(&io->lock);
  if (io->pid > 0 && !io->eof && kill(io->pid, 0) == 0)
    {
      task_delete(io->pid);
    }

  io->quit = true;                             /* The reader frees it */
  pthread_mutex_unlock(&io->lock);
  sem_post(&io->space);
}

static void io_write(struct ide_io_s *io, const void *buf, size_t n)
{
  if (io != NULL && !io->eof)
    {
      write(io->fd, buf, n);
    }
}

/****************************************************************************
 * Private Functions: the console, drawn
 ****************************************************************************/

static uint16_t colour(uint8_t i)
{
  static const uint8_t level[6] =
  {
    0, 95, 135, 175, 215, 255
  };

  if (i < 16)
    {
      return g_theme.term_pal[i];
    }

  if (i < 232)
    {
      i -= 16;
      return RGB(level[i / 36], level[i / 6 % 6], level[i % 6]);
    }

  i = 8 + (i - 232) * 10;
  return RGB(i, i, i);
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

  *fg = f;
  *bg = b;
}

/* The editor's kit: the extension's widgets are in it */

static struct kit_s *kit_of(struct desk_win_s *w)
{
  return desk_edit_kit(w);
}

/* One row of the console into o: its cells as runs of one look */

static void row_ops(struct ide_s *ide, struct desk_ops_s *o,
                    struct kit_widget_s *wg, int row)
{
  struct vt_s *vt = &ide->vt[ide->tab];
  struct vt_line_s *l = vt_line(vt, row, ide->view);
  int x = wg->x + PAD;
  int y = wg->y + PAD + row * CELL_H;
  int cols = (wg->w - 2 * PAD) / CELL_W;
  int i = 0;

  ops_fill(o, wg->x, y, wg->w, CELL_H, C_TERM_BG);
  while (l != NULL && i < l->n && i < cols)
    {
      struct vt_cell_s c0 = l->c[i];
      char run[128];
      uint16_t fg;
      uint16_t bg;
      int j = i;
      int n = 0;

      cell_colours(&c0, &fg, &bg);
      while (j < l->n && j < cols && n < (int)sizeof(run) - 1 &&
             l->c[j].fg == c0.fg && l->c[j].bg == c0.bg &&
             l->c[j].at == c0.at)
        {
          uint8_t ch = l->c[j].ch;

          run[n++] = ch >= 0x20 && ch < 0x86 ? ch : ' ';
          j++;
        }

      run[n] = '\0';
      if (bg != C_TERM_BG)
        {
          ops_fill(o, x + i * CELL_W, y, n * CELL_W, CELL_H, bg);
        }

      if (o->n + n + 4 > NX_GPUOPS_MAX)
        {
          ops_draw((NXWINDOW)ide->w->hwnd, o);
        }

      ops_text(o, (c0.at & VT_BOLD) ? &g_desk_font_monob :
               &g_desk_font_mono, x + i * CELL_W,
               y + CELL_BASE - g_desk_font_mono.ascent, run, fg, 255);
      i = j;
    }

  /* The cursor: a block where the program reads, hollow unless the
   * console has the keys
   */

  if (ide->view == 0 && row == vt->y && vt->cursor &&
      (ide->tab == T_REPL || ide->running))
    {
      int cx = x + vt->x * CELL_W;

      if (ide->focused && ide->w->active)
        {
          ops_blend(o, cx, y + 1, CELL_W, CELL_H - 2, C_TERM_FG, 160);
        }
      else
        {
          ops_blend(o, cx, y + 1, CELL_W, 1, C_TERM_FG, 160);
          ops_blend(o, cx, y + CELL_H - 2, CELL_W, 1, C_TERM_FG, 160);
          ops_blend(o, cx, y + 1, 1, CELL_H - 2, C_TERM_FG, 160);
          ops_blend(o, cx + CELL_W - 1, y + 1, 1, CELL_H - 2, C_TERM_FG,
                    160);
        }
    }
}

/* Rows that changed (all of them: full), then what of the kit is over
 * them
 */

static void console_draw(struct ide_s *ide, bool full)
{
  struct kit_s *k = kit_of(ide->w);
  struct kit_widget_s *wg = kit_get(k, I_CONSOLE);
  struct vt_s *vt = &ide->vt[ide->tab];
  static struct desk_ops_s *o;
  int rows;
  int y0 = -1;
  int y1 = -1;
  int r;

  if (wg == NULL || wg->h <= 0 || kit_dialog_open(k))
    {
      return;
    }

  if (o == NULL)
    {
      o = malloc(sizeof(*o));
      if (o == NULL)
        {
          return;
        }

      o->n = 0;
    }

  rows = (wg->h - 2 * PAD) / CELL_H;
  rows = rows > ROWS ? ROWS : rows;
  if (full || ide->full)
    {
      ops_fill(o, wg->x, wg->y, wg->w, wg->h, C_TERM_BG);
      y0 = wg->y;
      y1 = wg->y + wg->h;
    }

  for (r = 0; r < rows; r++)
    {
      struct vt_line_s *l = vt_line(vt, r, ide->view);
      bool cursor = r == vt->y || r == ide->currow || ide->shown[r] == NULL;

      if (full || ide->full || l != ide->shown[r] ||
          (l != NULL && l->d1 > l->d0) || cursor)
        {
          if (o->n + 8 > NX_GPUOPS_MAX)
            {
              ops_draw((NXWINDOW)ide->w->hwnd, o);
            }

          row_ops(ide, o, wg, r);
          ide->shown[r] = l;
          if (r == ide->currow)
            {
              ide->currow = -1;
            }

          if (r == vt->y && ide->view == 0 && vt->cursor &&
              (ide->tab == T_REPL || ide->running))
            {
              ide->currow = r;
            }
          if (l != NULL)
            {
              l->d0 = l->d1 = 0;
            }

          if (y0 < 0 || wg->y + PAD + r * CELL_H < y0)
            {
              y0 = wg->y + PAD + r * CELL_H;
            }

          if (wg->y + PAD + (r + 1) * CELL_H > y1)
            {
              y1 = wg->y + PAD + (r + 1) * CELL_H;
            }
        }
    }

  ops_draw((NXWINDOW)ide->w->hwnd, o);
  ide->full = false;
  if (y1 > y0)
    {
      kit_touched(k, wg->x, y0, wg->w, y1 - y0);
    }
}

/****************************************************************************
 * Private Functions: the output
 ****************************************************************************/

/* A line of the Output: a traceback's frame in the file being run is
 * remembered (the last one is the innermost)
 */

static int frame_line(const char *s, const char *path)
{
  const char *f = strstr(s, "File \"");
  const char *q;
  const char *l;
  size_t n;

  if (f == NULL)
    {
      return 0;
    }

  f += 6;
  q = strchr(f, '"');
  if (q == NULL)
    {
      return 0;
    }

  n = q - f;
  if (path != NULL && (strlen(path) != n || strncmp(f, path, n) != 0))
    {
      /* Relative names end the same */

      const char *base = strrchr(path, '/');

      base = base != NULL ? base + 1 : path;
      if (strlen(base) != n || strncmp(f, base, n) != 0)
        {
          return 0;
        }
    }

  l = strstr(q, ", line ");
  return l != NULL ? atoi(l + 7) : 0;
}

static void output_bytes(struct ide_s *ide, const uint8_t *p, int n)
{
  int i;

  for (i = 0; i < n; i++)
    {
      if (p[i] == '\n')
        {
          int line;

          ide->line[ide->linelen] = '\0';
          line = frame_line(ide->line, ide->runpath);
          if (line > 0)
            {
              ide->errline = line;
            }

          ide->linelen = 0;
        }
      else if (p[i] != '\r' && ide->linelen < (int)sizeof(ide->line) - 1)
        {
          ide->line[ide->linelen++] = p[i];
        }
    }
}

static void vt_puts(struct vt_s *vt, const char *s)
{
  vt_write(vt, (const uint8_t *)s, strlen(s));
}

/****************************************************************************
 * Private Functions: the tool bar's state, recent files
 ****************************************************************************/

static void state_sync(struct ide_s *ide)
{
  struct kit_s *k = kit_of(ide->w);

  kit_enable(k, I_RUN, !ide->running);
  kit_enable(k, I_STOP, ide->running);
  kit_enable(k, I_RECENT, ide->nrecent > 0);
  kit_flush(k);
}

static const char *home_dir(void)
{
  const char *home = getenv("HOME");

#ifdef CONFIG_LIBC_HOMEDIR
  if (home == NULL)
    {
      home = CONFIG_LIBC_HOMEDIR;
    }
#endif

  return home != NULL ? home : "/tmp";
}

static void recent_path(char *buf, size_t n)
{
  snprintf(buf, n, "%s/.python_recent", home_dir());
}

static void recent_load(struct ide_s *ide)
{
  char path[PATH_MAX];
  FILE *f;

  recent_path(path, sizeof(path));
  f = fopen(path, "r");
  if (f == NULL)
    {
      return;
    }

  while (ide->nrecent < MAX_RECENT &&
         fgets(ide->recent[ide->nrecent], PATH_MAX, f) != NULL)
    {
      char *nl = strchr(ide->recent[ide->nrecent], '\n');

      if (nl != NULL)
        {
          *nl = '\0';
        }

      if (ide->recent[ide->nrecent][0] == '/')
        {
          ide->nrecent++;
        }
    }

  fclose(f);
}

/* A file opened, run or saved: first in the list, kept */

static void recent_add(struct ide_s *ide, const char *file)
{
  char path[PATH_MAX];
  FILE *f;
  int i;

  if (file == NULL || file[0] != '/')
    {
      return;
    }

  for (i = 0; i < ide->nrecent && strcmp(ide->recent[i], file) != 0; i++)
    {
    }

  if (i == 0 && ide->nrecent > 0)
    {
      return;                                  /* The first already */
    }

  if (i == ide->nrecent)
    {
      i = ide->nrecent < MAX_RECENT ? ide->nrecent++ : MAX_RECENT - 1;
    }

  memmove(ide->recent[1], ide->recent[0], i * PATH_MAX);
  strlcpy(ide->recent[0], file, PATH_MAX);

  recent_path(path, sizeof(path));
  f = fopen(path, "w");
  if (f != NULL)
    {
      for (i = 0; i < ide->nrecent; i++)
        {
          fprintf(f, "%s\n", ide->recent[i]);
        }

      fclose(f);
    }

  state_sync(ide);
}

/****************************************************************************
 * Private Functions: running
 ****************************************************************************/

static void tab_show(struct ide_s *ide, int tab)
{
  struct kit_s *k = kit_of(ide->w);

  if (tab == T_REPL && ide->io[T_REPL] == NULL)
    {
      /* The REPL starts when it is first wanted (and again after it
       * ended)
       */

      vt_puts(&ide->vt[T_REPL], "\x1b[2J\x1b[H");
      ide->io[T_REPL] = io_start(ide, NULL);
      if (ide->io[T_REPL] == NULL)
        {
          vt_puts(&ide->vt[T_REPL], "Cannot start python\r\n");
        }
    }

  ide->tab  = tab;
  ide->view = 0;
  kit_set_value(k, I_TABS, tab);
  ide->focused = true;
  kit_focus(k, I_CONSOLE);
  kit_flush(k);
  console_draw(ide, true);
}

static void run(struct ide_s *ide)
{
  struct desk_win_s *w = ide->w;
  const char *path = desk_edit_path(w);

  if (ide->running)
    {
      return;
    }

  if (path[0] == '\0' || desk_edit_modified(w))
    {
      /* Saved first (Save As if it has no name): then run */

      ide->runpending = true;
      desk_edit_save(w);
      return;
    }

  ide->runpending = false;
  strlcpy(ide->runpath, path, sizeof(ide->runpath));
  ide->errline = 0;
  ide->linelen = 0;
  io_free(ide->io[T_OUTPUT]);
  ide->io[T_OUTPUT] = NULL;

  vt_puts(&ide->vt[T_OUTPUT], "\x1b[2J\x1b[H\x1b[2m$ python ");
  vt_puts(&ide->vt[T_OUTPUT], path);
  vt_puts(&ide->vt[T_OUTPUT], "\x1b[0m\r\n");
  ide->io[T_OUTPUT] = io_start(ide, ide->runpath);
  if (ide->io[T_OUTPUT] == NULL)
    {
      vt_puts(&ide->vt[T_OUTPUT], "Cannot start python\r\n");
    }
  else
    {
      ide->running = true;
      ide->stopms  = 0;
    }

  recent_add(ide, ide->runpath);
  tab_show(ide, T_OUTPUT);
  state_sync(ide);
  desk_edit_message(w, false, "Running");
}

static void stop(struct ide_s *ide)
{
  if (ide->running && ide->io[T_OUTPUT] != NULL)
    {
      io_write(ide->io[T_OUTPUT], "\x03", 1);
      ide->stopms = desk_ms() | 1;
    }
}

/* The program has ended: the editor goes to its error's line */

static void ended(struct ide_s *ide)
{
  bool stopped = ide->stopms != 0;

  ide->running = false;
  ide->stopms  = 0;
  vt_puts(&ide->vt[T_OUTPUT], "\r\n\x1b[2m--- the program has ended ---"
          "\x1b[0m\r\n");
  state_sync(ide);
  if (ide->errline > 0)
    {
      char msg[48];

      snprintf(msg, sizeof(msg), stopped ? "Stopped at line %d" :
               "Error at line %d", ide->errline);
      desk_edit_goto(ide->w, ide->errline);
      desk_edit_message(ide->w, true, msg);
    }
  else
    {
      desk_edit_message(ide->w, false, "The program has ended");
    }
}

/* A file into this window, or into a new one if this one has changes not
 * saved
 */

static void open_file(struct ide_s *ide, const char *path)
{
  if (!desk_edit_modified(ide->w))
    {
      desk_edit_load(ide->w, path);
    }
  else
    {
      struct desk_win_s *w = desk_ide_open(path);

      if (w != NULL)
        {
          desk_win_show(w);
        }
    }

  recent_add(ide, path);
}

/* An example: written to python/ in the home directory (unless there),
 * then opened from there
 */

static void open_example(struct ide_s *ide, int i)
{
  char path[PATH_MAX];
  struct stat st;

  if (i < 0 || i >= g_desk_npyexamples)
    {
      return;
    }

  snprintf(path, sizeof(path), "%s/python", home_dir());
  mkdir(path, 0777);
  snprintf(path, sizeof(path), "%s/python/%s", home_dir(),
           g_desk_pyexamples[i].name);
  if (stat(path, &st) < 0)
    {
      FILE *f = fopen(path, "w");

      if (f == NULL)
        {
          desk_edit_message(ide->w, true, "Cannot write the example");
          return;
        }

      fputs(g_desk_pyexamples[i].text, f);
      fclose(f);
    }

  open_file(ide, path);
}

/****************************************************************************
 * Private Functions: the editor's extension
 ****************************************************************************/

static void ide_toolbar(struct desk_win_s *w, struct kit_s *k)
{
  struct ide_s *ide = desk_edit_extpriv(w);

  ide->w = w;                          /* (Before the window is drawn) */
  kit_button(k, I_RUN, "Run", KIT_ICON_NONE, KIT_BTN_DEFAULT);
  kit_tip(k, "Save and run (F5)");
  kit_button(k, I_STOP, "Stop", KIT_ICON_NONE, KIT_BTN_DANGER)->flags |=
    KIT_F_DISABLED;
  kit_tip(k, "Ctrl-C to the program");
  kit_spacer(k, 0);
  kit_size(k, 4, 0);
  kit_button(k, I_OPEN, "Open", KIT_ICON_NONE, KIT_BTN_NORMAL);
  kit_button(k, I_EXAMPLES, "Examples", KIT_ICON_CHEVRON, KIT_BTN_NORMAL);
  kit_button(k, I_RECENT, "Recent", KIT_ICON_CHEVRON, KIT_BTN_NORMAL);
  kit_spacer(k, 0);
  kit_size(k, 4, 0);
}

static void ide_pane(struct desk_win_s *w, struct kit_s *k)
{
  kit_row(k, KIT_BOX_BAR)->ph = 36;
    kit_tabs(k, I_TABS, g_tabs, 2, T_OUTPUT, KIT_TABS_SEGMENTS);
    kit_label(k, I_STATE, "", KIT_TEXT_DIM);
    kit_grow(k, 1);
    kit_button(k, I_CLEAR, "Clear", KIT_ICON_NONE, KIT_BTN_FLAT);
  kit_end(k);
  kit_custom(k, I_CONSOLE)->flags |= KIT_F_KEYS;
  kit_size(k, 0, 2 * PAD + ROWS * CELL_H);
}

/* A key for the console's task: the bytes a terminal sends */

static void console_key(struct ide_s *ide, int key)
{
  static const char * const seq[] =
  {
    "\x1b[A", "\x1b[B", "\x1b[C", "\x1b[D", "\x1b[H", "\x1b[F", "\x1b[5~",
    "\x1b[6~", "\x1b[2~", "\x1b[3~", "\x1b", NULL, NULL
  };

  struct ide_io_s *io = ide->io[ide->tab];
  int k = key & KEY_MASK;

  if (k == KEY_F5)
    {
      run(ide);
      return;
    }

  if (ide->tab == T_REPL && (io == NULL || io->eof))
    {
      /* The REPL ended: a key starts it again */

      io_free(io);
      ide->io[T_REPL] = NULL;
      tab_show(ide, T_REPL);
      return;
    }

  if (k >= KEY_UP && k <= KEY_F5)
    {
      if (seq[k - KEY_UP] != NULL)
        {
          io_write(io, seq[k - KEY_UP], strlen(seq[k - KEY_UP]));
        }
    }
  else if (k < 0x100)
    {
      uint8_t c = k == '\n' ? '\r' : k;

      io_write(io, &c, 1);
    }

  ide->view = 0;
}

/* A click on the console: on a traceback's line, the editor goes there */

static void console_click(struct ide_s *ide, struct kit_s *k,
                          struct kit_widget_s *wg)
{
  int row = (k->my - wg->y - PAD) / CELL_H;
  struct vt_line_s *l = vt_line(&ide->vt[ide->tab], row, ide->view);
  char s[160];
  int n = 0;
  int i;

  ide->focused = true;
  for (i = 0; l != NULL && i < l->n && n < (int)sizeof(s) - 1; i++)
    {
      s[n++] = l->c[i].ch;
    }

  s[n] = '\0';
  n = frame_line(s, ide->tab == T_OUTPUT ? ide->runpath : NULL);
  if (n > 0 && ide->tab == T_OUTPUT)
    {
      desk_edit_goto(ide->w, n);
    }
  else
    {
      console_draw(ide, false);
    }
}

static void open_recent(struct ide_s *ide, int i)
{
  char path[PATH_MAX];

  if (i >= 0 && i < ide->nrecent)
    {
      strlcpy(path, ide->recent[i], sizeof(path));
      open_file(ide, path);
    }
}

static void ide_event(struct desk_win_s *w, struct kit_s *k,
                      struct kit_widget_s *wg, int ev)
{
  struct ide_s *ide = desk_edit_extpriv(w);
  int id = wg != NULL ? wg->id : 0;
  int i;

  switch (ev)
    {
      case KIT_EV_CLICK:
        switch (id)
          {
            case I_RUN:
              run(ide);
              break;

            case I_STOP:
              stop(ide);
              break;

            case I_OPEN:
              kit_dialog(k, D_OPEN, KIT_DLG_OPEN, "Open", home_dir(), NULL,
                         0);
              break;

            case I_EXAMPLES:
              for (i = 0; i < g_desk_npyexamples && i < KIT_POP_MAX; i++)
                {
                  ide->items[i] = g_desk_pyexamples[i].name;
                }

              kit_menu(k, M_EXAMPLES, wg->x, wg->y + wg->h, ide->items, i,
                       0);
              break;

            case I_RECENT:
              for (i = 0; i < ide->nrecent; i++)
                {
                  ide->items[i] = ide->recent[i];
                }

              kit_menu(k, M_RECENT, wg->x, wg->y + wg->h, ide->items, i,
                       0);
              break;

            case I_CLEAR:
              vt_puts(&ide->vt[ide->tab], "\x1b[2J\x1b[H");
              if (ide->tab == T_REPL)
                {
                  io_write(ide->io[T_REPL], "\r", 1);
                }

              console_draw(ide, true);
              break;
          }
        break;

      case KIT_EV_CHANGE:
        if (id == I_TABS && wg->value != ide->tab)
          {
            tab_show(ide, wg->value);
          }
        break;

      case KIT_EV_MENU:
        if (k->key == M_EXAMPLES)
          {
            open_example(ide, k->arg);
          }
        else if (k->key == M_RECENT)
          {
            open_recent(ide, k->arg);
          }
        break;

      case KIT_EV_DIALOG:
        if (k->key == D_OPEN && k->arg == KIT_ANS_OK &&
            k->answer != NULL && k->answer[0] != '\0')
          {
            open_file(ide, k->answer);
          }
        break;

      case KIT_EV_MOUSE:
        if (id == I_CONSOLE && (k->mbuttons & NX_MOUSE_LEFTBUTTON))
          {
            console_click(ide, k, wg);
          }
        break;

      case KIT_EV_KEY:
        if (id == I_CONSOLE)
          {
            console_key(ide, k->key);
            k->key = 0;
          }
        break;

      default:
        break;
    }
}

static void ide_paint(struct desk_win_s *w, struct kit_s *k,
                      struct kit_widget_s *wg, int x0, int y0, int x1,
                      int y1)
{
  struct ide_s *ide = desk_edit_extpriv(w);

  if (wg->id == I_CONSOLE)
    {
      ide->full = true;
      console_draw(ide, true);
    }
}

/* The text's keys first: F5 runs */

static bool ide_key(struct desk_win_s *w, int key)
{
  struct ide_s *ide = desk_edit_extpriv(w);

  if ((key & KEY_MASK) == KEY_F5)
    {
      run(ide);
      return true;
    }

  if (ide->focused)
    {
      ide->focused = false;
      console_draw(ide, false);                /* Its cursor goes hollow */
    }

  return false;
}

static void ide_saved(struct desk_win_s *w)
{
  struct ide_s *ide = desk_edit_extpriv(w);

  recent_add(ide, desk_edit_path(w));
  if (ide->runpending)
    {
      run(ide);
    }
}

static void ide_close(struct desk_win_s *w)
{
  struct ide_s *ide = desk_edit_extpriv(w);

  if (ide == NULL)
    {
      return;
    }

  io_free(ide->io[T_OUTPUT]);
  io_free(ide->io[T_REPL]);
  vt_free(&ide->vt[T_OUTPUT]);
  vt_free(&ide->vt[T_REPL]);
  free(ide);
}

static void ide_newdoc(struct desk_win_s *w)
{
  struct desk_win_s *nw = desk_ide_open(NULL);

  if (nw != NULL)
    {
      desk_win_show(nw);
    }
}

static const struct edit_ext_s g_ext =
{
  .kind    = PYTHON_IDE,
  .app     = APP_PYTHON,
  .w       = IDE_W,
  .h       = IDE_H,
  .python  = true,
  .newdoc  = ide_newdoc,
  .toolbar = ide_toolbar,
  .pane    = ide_pane,
  .event   = ide_event,
  .paint   = ide_paint,
  .key     = ide_key,
  .saved   = ide_saved,
  .close   = ide_close,
};

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* A new IDE window with a file (NULL: none), not shown yet */

struct desk_win_s *desk_ide_open(const char *path)
{
  struct desk_win_s *w;
  struct ide_s *ide;
  int cols = (IDE_W - 2 * PAD) / CELL_W;

  ide = calloc(1, sizeof(*ide));
  if (ide == NULL)
    {
      return NULL;
    }

  if (vt_init(&ide->vt[T_OUTPUT], cols, ROWS, SCROLLBACK) < 0 ||
      vt_init(&ide->vt[T_REPL], cols, ROWS, SCROLLBACK) < 0)
    {
      vt_free(&ide->vt[T_OUTPUT]);
      free(ide);
      return NULL;
    }

  recent_load(ide);
  w = desk_edit_open_ext(path, &g_ext, ide);
  if (w == NULL)
    {
      vt_free(&ide->vt[T_OUTPUT]);
      vt_free(&ide->vt[T_REPL]);
      free(ide);
      return NULL;
    }

  ide->w      = w;
  ide->full   = true;
  ide->currow = -1;
  vt_puts(&ide->vt[T_OUTPUT],
          "\x1b[2mRun (F5) saves the file and runs it here.\x1b[0m\r\n");
  state_sync(ide);
  if (path != NULL)
    {
      recent_add(ide, path);
    }

  return w;
}

/* Output from the console's tasks (desk_wake()) */

void desk_ide_wake(struct desk_win_s *w)
{
  struct ide_s *ide = desk_edit_extpriv(w);
  bool draw = false;
  int t;

  if (ide == NULL)
    {
      return;
    }

  for (t = 0; t < 2; t++)
    {
      struct ide_io_s *io = ide->io[t];
      bool eof = false;

      while (io != NULL)
        {
          int head;
          int span;

          pthread_mutex_lock(&io->lock);
          head = io->head;
          span = io->n;
          eof  = io->eof && span == 0;
          pthread_mutex_unlock(&io->lock);

          span = span > RING - head ? RING - head : span;
          if (span == 0)
            {
              break;
            }

          vt_write(&ide->vt[t], io->ring + head, span);
          if (t == T_OUTPUT)
            {
              output_bytes(ide, io->ring + head, span);
            }

          draw |= t == ide->tab;

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

      if (io != NULL && ide->vt[t].nresp > 0)
        {
          io_write(io, ide->vt[t].resp, ide->vt[t].nresp);
          ide->vt[t].nresp = 0;
        }

      if (eof)
        {
          io_free(io);
          ide->io[t] = NULL;
          if (t == T_OUTPUT)
            {
              ended(ide);
            }
          else
            {
              vt_puts(&ide->vt[T_REPL], "\r\n\x1b[2m--- the REPL has "
                      "ended: a key starts it again ---\x1b[0m\r\n");
            }

          draw |= t == ide->tab;
        }
    }

  if (draw && !w->closing)
    {
      console_draw(ide, false);
    }
}

/* Once a second: a program that Stop did not end is ended */

void desk_ide_tick(struct desk_win_s *w)
{
  struct ide_s *ide = desk_edit_extpriv(w);

  if (ide != NULL && ide->running && ide->stopms != 0 &&
      desk_ms() - ide->stopms > STOP_MS && ide->io[T_OUTPUT] != NULL)
    {
      struct ide_io_s *io = ide->io[T_OUTPUT];

      pthread_mutex_lock(&io->lock);
      if (io->pid > 0 && !io->eof && kill(io->pid, 0) == 0)
        {
          task_delete(io->pid);
          vt_puts(&ide->vt[T_OUTPUT], "\r\n\x1b[2m(ended by Stop)\x1b[0m");
        }

      pthread_mutex_unlock(&io->lock);
      ide->stopms = 0;
    }
}

#endif /* CONFIG_FPCDESK_PYTHON */
