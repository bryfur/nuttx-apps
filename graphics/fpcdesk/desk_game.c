/****************************************************************************
 * apps/graphics/fpcdesk/desk_game.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Games in windows (apps/include/graphics/deskgame.h).  A game is a task
 * of its own that renders frames of 8-bit pixels with a palette; the
 * desktop gives it a window and does the rest:
 *
 *   - Frames: the game copies each into the session (deskgame_frame())
 *     and wakes the main thread, which compares it with the one shown,
 *     band by band (BAND rows), and sends only the rows and columns that
 *     changed.  They are scaled to the window by a whole number s (320 x
 *     200 is 640 x 400 in a new window, 960 x 600 maximised at 1280 x
 *     800): across on the CPU, where it costs nothing (a palette lookup
 *     gives the pixel s times over), and down on the GPU: the rows are
 *     PUT once into a hidden staging window, then each is COPYed s times
 *     into the window.  A full 640 x 400 frame is 256 KB on the link and
 *     400 COPYs (6 KB of commands), not the 512 KB of a PUT of all of it
 *     (CONFIG_FPCDESK_GAME_CPUSCALE does that, for comparison).
 *   - Input: key events (nxkeys.h) and the mouse over the window go to a
 *     queue the game reads (deskgame_input()).
 *   - Pause: while the window is not the active one (or is minimised) the
 *     game waits in deskgame_wait().  Closing the window tells it to quit.
 *   - Before the first frame the window shows what the game says
 *     (deskgame_status()), and if it ends with an error, the error.
 *
 * The session is shared by the window and the game's task and freed by
 * whichever lets go of it last.
 *
 * Doom (games/NXDoom, CONFIG_GAMES_NXDOOM_DESKTOP) is the game: the window
 * looks for an IWAD in the home directory (and CONFIG_FPCDESK_DOOM_WADDIRS)
 * and starts the nxdoom builtin with it, or says what to do if there is
 * none (and starts it as soon as there is one).
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <sched.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#include <nuttx/lib/builtin.h>
#include <nuttx/nx/nxbe.h>

#include <graphics/deskgame.h>

#include "desk.h"

#ifdef CONFIG_GAMES_NXDOOM_DESKTOP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GAME_MAGIC        0x44474d45           /* "EMGD" */
#define GAME_QLEN         64                   /* Input events waiting */
#define BAND              16                   /* Rows compared, sent */
#define GAME_W            320                  /* Doom's frame */
#define GAME_H            200

#define C_GAME_BG         RGB(0x00, 0x00, 0x00)
#define C_GAME_TEXT       RGB(0xd8, 0xd8, 0xdc)
#define C_GAME_DIM        RGB(0x8e, 0x8e, 0x93)
#define C_GAME_RED        RGB(0xd8, 0x30, 0x20)

#ifndef CONFIG_FPCDESK_DOOM_PRIORITY
#  define CONFIG_FPCDESK_DOOM_PRIORITY  90
#endif

#ifndef CONFIG_FPCDESK_DOOM_STACKSIZE
#  define CONFIG_FPCDESK_DOOM_STACKSIZE 8192
#endif

#ifndef CONFIG_FPCDESK_DOOM_WADDIRS
#  define CONFIG_FPCDESK_DOOM_WADDIRS   ""
#endif

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct deskgame_s
{
  uint32_t magic;
  pthread_mutex_t lock;
  sem_t resume;                    /* deskgame_wait(): active, or quit */
  int refs;                        /* The window, the task */
  struct desk_win_s *win;          /* NULL once the window has gone */
  pid_t pid;                       /* The game, -1: none */

  /* What the game says (under lock) */

  int fw;                          /* Frame size, 0: not attached */
  int fh;
  uint8_t *pending;                /* The latest frame */
  uint8_t pendpal[768];
  bool newframe;
  bool detached;                   /* The game has ended */
  bool quit;                       /* The window has been closed */
  bool paused;                     /* The window is not active */
  char status[96];
  char error[160];
  bool newstatus;

  /* Input for the game (under lock) */

  struct deskgame_input_s q[GAME_QLEN];
  int qhead;
  int qn;

  /* Drawing: the main thread's */

  uint8_t *work;                   /* The frame being drawn */
  uint8_t *shown;                  /* The frame the window shows */
  uint8_t workpal[768];
  uint8_t shownpal[768];
  uint32_t pal32[256];             /* RGB565, twice (scaled across) */
  uint16_t *band;                  /* Rows being converted */
  size_t bandsize;
  NXWINDOW stage;                  /* Hidden: rows before scaling down */
  int stagew;
  int stageh;
  int scale;                       /* Layout: s, where the frame is */
  int ox;
  int oy;
  bool haveframe;                  /* A frame has been drawn */
  bool drawnmsg;                   /* The window shows a message */

  /* Pointer over the window */

  int16_t mx;
  int16_t my;
  bool mvalid;
  uint8_t buttons;

  /* Statistics */

  uint32_t frames;
  uint64_t puts;                   /* Pixels sent */
  uint64_t copies;                 /* COPY operations */
};

struct game_win_s
{
  struct deskgame_s *g;
  char wad[128];                   /* The IWAD, "" if none */
  char dir[96];                    /* Where it is (the config goes there) */
  char nowad[128];                 /* Where to put one */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

/* Operations being built: from the heap, not the SRAM's .bss, as the game
 * is not always there (taken with the first window, kept)
 */

static struct desk_ops_s *g_ops;

static const char * const g_iwads[] =
{
  "doom.wad", "doom2.wad", "plutonia.wad", "tnt.wad", "doom1.wad",
  "freedoom1.wad", "freedoom2.wad", "freedm.wad"
};

/****************************************************************************
 * Private Functions: the session
 ****************************************************************************/

/* Let go of the session: the last one frees it (lock held; unlocks) */

static void game_unref(struct deskgame_s *g)
{
  bool last = --g->refs == 0;

  pthread_mutex_unlock(&g->lock);
  if (!last)
    {
      return;
    }

  syslog(LOG_INFO, "desk: game: %" PRIu32 " frames, %" PRIu32
         " KB and %" PRIu32 " COPYs a frame\n", g->frames,
         (uint32_t)(g->frames ? g->puts * 2 / 1024 / g->frames : 0),
         (uint32_t)(g->frames ? g->copies / g->frames : 0));

  g->magic = 0;
  sem_destroy(&g->resume);
  pthread_mutex_destroy(&g->lock);
  free(g->pending);
  free(g->work);
  free(g->shown);
  free(g->band);
  free(g);
}

static void game_push(struct deskgame_s *g,
                      const struct deskgame_input_s *in)
{
  pthread_mutex_lock(&g->lock);
  if (g->qn < GAME_QLEN && !g->detached)
    {
      g->q[(g->qhead + g->qn) % GAME_QLEN] = *in;
      g->qn++;
    }

  pthread_mutex_unlock(&g->lock);
}

/* Paused unless the window is the active one */

static void game_setpause(struct deskgame_s *g, bool paused)
{
  pthread_mutex_lock(&g->lock);
  if (g->paused != paused)
    {
      g->paused = paused;
      if (!paused)
        {
          sem_post(&g->resume);
        }
    }

  pthread_mutex_unlock(&g->lock);
}

/****************************************************************************
 * Public Functions: the game's side (deskgame.h), on the game's task
 ****************************************************************************/

FAR struct deskgame_s *deskgame_attach(int argc, FAR char *argv[],
                                       int width, int height)
{
  struct deskgame_s *g = NULL;
  size_t size = (size_t)width * height;
  int i;

  for (i = 1; i + 1 < argc; i++)
    {
      if (strcmp(argv[i], DESKGAME_ARG) == 0)
        {
          g = (struct deskgame_s *)(uintptr_t)strtoul(argv[i + 1], NULL,
                                                      16);
          break;
        }
    }

  if (g == NULL || g->magic != GAME_MAGIC || width <= 0 || height <= 0)
    {
      return NULL;
    }

  pthread_mutex_lock(&g->lock);
  if (g->fw == 0)
    {
      g->pending = calloc(1, size);
      g->work    = calloc(1, size);
      g->shown   = calloc(1, size);
      if (g->pending == NULL || g->work == NULL || g->shown == NULL)
        {
          pthread_mutex_unlock(&g->lock);
          return NULL;
        }

      g->fw = width;
      g->fh = height;
    }

  pthread_mutex_unlock(&g->lock);
  return g;
}

void deskgame_frame(FAR struct deskgame_s *g, FAR const uint8_t *pixels,
                    FAR const uint8_t *palette)
{
  pthread_mutex_lock(&g->lock);
  memcpy(g->pending, pixels, (size_t)g->fw * g->fh);
  if (palette != NULL)
    {
      memcpy(g->pendpal, palette, sizeof(g->pendpal));
    }

  g->newframe = true;

  /* The window cannot go while the lock is held (doom_close()) */

  if (g->win != NULL)
    {
      desk_wake(g->win);
    }

  pthread_mutex_unlock(&g->lock);
}

bool deskgame_input(FAR struct deskgame_s *g,
                    FAR struct deskgame_input_s *in)
{
  bool have;

  pthread_mutex_lock(&g->lock);
  have = g->qn > 0;
  if (have)
    {
      *in      = g->q[g->qhead];
      g->qhead = (g->qhead + 1) % GAME_QLEN;
      g->qn--;
    }

  pthread_mutex_unlock(&g->lock);
  return have;
}

int deskgame_wait(FAR struct deskgame_s *g)
{
  uint32_t start = 0;
  bool waited = false;
  int ret;

  pthread_mutex_lock(&g->lock);
  while (g->paused && !g->quit)
    {
      if (!waited)
        {
          start  = desk_ms();
          waited = true;
        }

      pthread_mutex_unlock(&g->lock);
      while (sem_wait(&g->resume) < 0 && errno == EINTR)
        {
        }

      pthread_mutex_lock(&g->lock);
    }

  ret = g->quit ? -1 : waited ? (int)(desk_ms() - start) : 0;

  /* Posts from pauses that were over before they were waited for */

  while (sem_trywait(&g->resume) == 0)
    {
    }

  pthread_mutex_unlock(&g->lock);
  return ret;
}

void deskgame_status(FAR struct deskgame_s *g, FAR const char *text)
{
  pthread_mutex_lock(&g->lock);
  strlcpy(g->status, text, sizeof(g->status));
  g->newstatus = true;
  if (g->win != NULL)
    {
      desk_wake(g->win);
    }

  pthread_mutex_unlock(&g->lock);
}

void deskgame_detach(FAR struct deskgame_s *g, FAR const char *error)
{
  pthread_mutex_lock(&g->lock);
  if (g->detached)
    {
      pthread_mutex_unlock(&g->lock);
      return;
    }

  if (error != NULL)
    {
      strlcpy(g->error, error, sizeof(g->error));
    }

  g->detached = true;
  g->qn       = 0;
  if (g->win != NULL)
    {
      desk_wake(g->win);
    }

  game_unref(g);
}

/****************************************************************************
 * Private Functions: drawing (the main thread)
 ****************************************************************************/

static void game_ops_flush(struct desk_win_s *w)
{
  ops_draw((NXWINDOW)w->hwnd, g_ops);
}

/* A part of the window around the frame (none if the frame fills it) */

static void game_fill(int x, int y, int w, int h)
{
  if (w > 0 && h > 0)
    {
      ops_fill(g_ops, x, y, w, h, C_GAME_BG);
    }
}

/* A message in the window, centred: a title and lines of text, the long
 * ones wrapped at spaces
 */

static int game_text_wrapped(const char *s, int cx, int y, int maxw)
{
  char line[160];
  const char *p = s;

  while (*p != '\0')
    {
      int n = 0;
      int fit = 0;

      /* The longest run of words that fits */

      while (p[n] != '\0')
        {
          int e = n;

          while (p[e] == ' ')
            {
              e++;
            }

          while (p[e] != '\0' && p[e] != ' ')
            {
              e++;
            }

          if (e >= (int)sizeof(line))
            {
              break;
            }

          memcpy(line, p, e);
          line[e] = '\0';
          if (fit > 0 && text_width(&g_desk_font_ui, line) > maxw)
            {
              break;
            }

          fit = e;
          n   = e;
        }

      if (fit == 0)
        {
          fit = strlen(p) < sizeof(line) - 1 ? strlen(p) : sizeof(line) - 1;
        }

      memcpy(line, p, fit);
      line[fit] = '\0';
      ops_text_centre(g_ops, &g_desk_font_ui, cx, y, line, C_GAME_TEXT, 255);
      y += text_height(&g_desk_font_ui) + 6;
      p += fit;
      while (*p == ' ')
        {
          p++;
        }
    }

  return y;
}

static void game_message(struct desk_win_s *w, const char *title,
                         uint16_t tcolour, const char * const *lines,
                         int nlines, const char *dim)
{
  int cw = w->csize.w;
  int y = TITLE_H + w->csize.h / 2 - 60;
  int i;

  ops_reset(g_ops);
  ops_fill(g_ops, 0, TITLE_H, cw, w->csize.h, C_GAME_BG);
  ops_text_centre(g_ops, &g_desk_font_title, cw / 2, y, title, tcolour, 255);
  y += text_height(&g_desk_font_title) + 16;
  game_ops_flush(w);

  for (i = 0; i < nlines; i++)
    {
      y = game_text_wrapped(lines[i], cw / 2, y, cw - 60);
      game_ops_flush(w);
    }

  if (dim != NULL)
    {
      ops_text_centre(g_ops, &g_desk_font_small, cw / 2, y + 10, dim,
                      C_GAME_DIM, 255);
      game_ops_flush(w);
    }
}

/* What the window shows before the first frame, or after an error */

static void game_draw_message(struct desk_win_s *w)
{
  struct game_win_s *gw = w->priv;
  struct deskgame_s *g = gw->g;
  const char *lines[3];
  char status[96];
  char error[160];

  pthread_mutex_lock(&g->lock);
  strlcpy(status, g->status, sizeof(status));
  strlcpy(error, g->error, sizeof(error));
  pthread_mutex_unlock(&g->lock);

  g->drawnmsg = true;
  if (error[0] != '\0')
    {
      lines[0] = error;
      game_message(w, "Doom stopped", C_GAME_RED, lines, 1,
                   "Close the window, and open Doom again to restart");
    }
  else if (gw->wad[0] == '\0')
    {
      lines[0] = "Doom needs the game's data, an IWAD file: doom1.wad (the "
                 "shareware episode), doom.wad or doom2.wad, or Freedoom's "
                 "freedoom1.wad or freedoom2.wad (free, "
                 "freedoom.github.io).";
      lines[1] = gw->nowad;
      game_message(w, "No game data", C_GAME_TEXT, lines, 2,
                   "Doom starts as soon as the file is there");
    }
  else
    {
      lines[0] = status[0] != '\0' ? status : "Starting";
      game_message(w, "Doom", C_GAME_RED, lines, 1, gw->wad);
    }
}

/* Where the frame goes: the largest whole scale that fits, centred */

static bool game_layout(struct desk_win_s *w, struct deskgame_s *g)
{
  int s = w->csize.w / g->fw;
  int ox;
  int oy;

  if (w->csize.h / g->fh < s)
    {
      s = w->csize.h / g->fh;
    }

  if (s < 1)
    {
      s = 1;
    }

  ox = (w->csize.w - g->fw * s) / 2;
  oy = (w->csize.h - g->fh * s) / 2;
  if (ox < 0)
    {
      ox = 0;
    }

  if (oy < 0)
    {
      oy = 0;
    }

  if (s == g->scale && ox == g->ox && oy == g->oy)
    {
      return false;
    }

  g->scale = s;
  g->ox    = ox;
  g->oy    = oy;
  return true;
}

/* The buffers and the staging window for scale s.  The staging window
 * takes the desktop's own callbacks (as a part of no consequence): it may
 * be the one window of the desktop's told of a new display mode
 */

extern const struct nx_callback_s g_desk_partcb;

static int game_buffers(struct deskgame_s *g)
{
  int s = g->scale;
  size_t need;

#ifdef CONFIG_FPCDESK_GAME_CPUSCALE
  need = (size_t)g->fw * s * BAND * s * sizeof(uint16_t);
#else
  need = (size_t)g->fw * s * BAND * sizeof(uint16_t);
#endif

  if (g->bandsize < need)
    {
      free(g->band);
      g->band     = malloc(need);
      g->bandsize = g->band != NULL ? need : 0;
      if (g->band == NULL)
        {
          return -ENOMEM;
        }
    }

#ifndef CONFIG_FPCDESK_GAME_CPUSCALE
  if (s > 1 && (g->stage == NULL || g->stagew != g->fw * s ||
                g->stageh != g->fh))
    {
      struct nxgl_size_s size;
      struct nxgl_point_s pos;

      if (g->stage == NULL)
        {
          g->stage = nx_openwindow(g_desk.hnx, NXBE_WINDOW_RAMBACKED |
                                   NXBE_WINDOW_HIDDEN, &g_desk_partcb,
                                   (void *)(uintptr_t)PART_WALLPAPER);
          if (g->stage == NULL)
            {
              return -ENOMEM;
            }
        }

      size.w = g->fw * s;
      size.h = g->fh;
      pos.x  = 0;
      pos.y  = 0;
      nx_setsize(g->stage, &size);
      nx_setposition(g->stage, &pos);
      g->stagew = size.w;
      g->stageh = size.h;
    }
#endif

  return 0;
}

static void game_palette(struct deskgame_s *g, const uint8_t *pal)
{
  int i;

  for (i = 0; i < 256; i++)
    {
      uint32_t c = RGB(pal[3 * i], pal[3 * i + 1], pal[3 * i + 2]);

      g->pal32[i] = c | c << 16;
    }
}

/* Rows r0 to r1 - 1, columns x0 to x1 - 1 of src, scaled across by s,
 * into g->band (rows of (x1 - x0) * s pixels)
 */

static void game_convert(struct deskgame_s *g, const uint8_t *src, int r0,
                         int r1, int x0, int x1, int rowrepeat)
{
  const uint32_t *pal = g->pal32;
  uint16_t *out = g->band;
  int s = g->scale;
  int n = x1 - x0;
  int r;
  int k;

  for (r = r0; r < r1; r++)
    {
      const uint8_t *p = src + (size_t)r * g->fw + x0;
      uint16_t *row = out;
      int i;

      if (s == 1)
        {
          for (i = 0; i < n; i++)
            {
              row[i] = (uint16_t)pal[p[i]];
            }
        }
      else if (s == 2 && ((uintptr_t)row & 3) == 0)
        {
          uint32_t *d = (uint32_t *)row;

          for (i = 0; i < n; i++)
            {
              d[i] = pal[p[i]];
            }
        }
      else
        {
          uint16_t *d = row;

          for (i = 0; i < n; i++)
            {
              uint16_t c = (uint16_t)pal[p[i]];

              for (k = 0; k < s; k++)
                {
                  *d++ = c;
                }
            }
        }

      out += n * s;

      /* Scaled down on the CPU too: the row again */

      for (k = 1; k < rowrepeat; k++)
        {
          memcpy(out, row, n * s * sizeof(uint16_t));
          out += n * s;
        }
    }
}

/* Which columns of rows r0 to r1 - 1 differ between a and b: [*x0, *x1),
 * false if none
 */

static bool game_diff(struct deskgame_s *g, const uint8_t *a,
                      const uint8_t *b, int r0, int r1, int *x0, int *x1)
{
  int lo = g->fw;
  int hi = 0;
  int r;

  for (r = r0; r < r1; r++)
    {
      const uint8_t *pa = a + (size_t)r * g->fw;
      const uint8_t *pb = b + (size_t)r * g->fw;
      int i;
      int j;

      if (memcmp(pa, pb, g->fw) == 0)
        {
          continue;
        }

      for (i = 0; i < lo && pa[i] == pb[i]; i++)
        {
        }

      for (j = g->fw; j > hi && pa[j - 1] == pb[j - 1]; j--)
        {
        }

      lo = i < lo ? i : lo;
      hi = j > hi ? j : hi;
    }

  *x0 = lo;
  *x1 = hi;
  return lo < hi;
}

static void game_bitmap(NXWINDOW hwnd, int x, int y, int w, int h,
                        const uint16_t *pix)
{
  struct nxgl_rect_s dest;
  struct nxgl_point_s origin;
  const void *src[1];

  dest.pt1.x = x;
  dest.pt1.y = y;
  dest.pt2.x = x + w - 1;
  dest.pt2.y = y + h - 1;
  origin     = dest.pt1;
  src[0]     = pix;
  nx_bitmap(hwnd, &dest, src, &origin, w * sizeof(uint16_t));
}

/* Draw the frame src (its palette pal) into the window: the rows and
 * columns that differ from the one shown, or all of it
 */

static void game_blit(struct desk_win_s *w, struct deskgame_s *g,
                      const uint8_t *src, const uint8_t *pal, bool full)
{
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  bool borders;
  int s;
  int fx;
  int fy;
  int r0;

  /* All of it, around it too, after a message or at a new place; all of
   * the frame after a new palette
   */

  borders = game_layout(w, g) || g->drawnmsg || !g->haveframe || full;
  full    = borders;
  if (memcmp(pal, g->shownpal, sizeof(g->shownpal)) != 0 || full)
    {
      game_palette(g, pal);
      memcpy(g->shownpal, pal, sizeof(g->shownpal));
      full = true;
    }

  if (game_buffers(g) < 0)
    {
      return;
    }

  s  = g->scale;
  fx = g->ox;
  fy = TITLE_H + g->oy;

  /* The borders around the frame */

  if (borders)
    {
      int fw = g->fw * s;
      int fh = g->fh * s;

      ops_reset(g_ops);
      game_fill(0, TITLE_H, w->csize.w, g->oy);
      game_fill(0, fy + fh, w->csize.w, w->csize.h - g->oy - fh);
      game_fill(0, fy, fx, fh);
      game_fill(fx + fw, fy, w->csize.w - fx - fw, fh);
      game_ops_flush(w);
      g->drawnmsg = false;
    }

  ops_reset(g_ops);
  for (r0 = 0; r0 < g->fh; r0 += BAND)
    {
      int r1 = r0 + BAND > g->fh ? g->fh : r0 + BAND;
      int x0 = 0;
      int x1 = g->fw;
      int n;

      if (!full && !game_diff(g, src, g->shown, r0, r1, &x0, &x1))
        {
          continue;
        }

      n = (x1 - x0) * s;

#ifdef CONFIG_FPCDESK_GAME_CPUSCALE
      /* Scaled both ways on the CPU, one PUT */

      game_convert(g, src, r0, r1, x0, x1, s);
      game_bitmap(hwnd, fx + x0 * s, fy + r0 * s, n, (r1 - r0) * s,
                  g->band);
      g->puts += n * (r1 - r0) * s;
#else
      game_convert(g, src, r0, r1, x0, x1, 1);
      g->puts += n * (r1 - r0);
      if (s == 1)
        {
          game_bitmap(hwnd, fx + x0, fy + r0, n, r1 - r0, g->band);
        }
      else
        {
          int r;
          int k;

          /* The rows into the staging window, then each s times down */

          game_bitmap(g->stage, x0 * s, r0, n, r1 - r0, g->band);
          for (r = r0; r < r1; r++)
            {
              for (k = 0; k < s; k++)
                {
                  struct nx_gpuop_s *op;

                  if (g_ops->n == NX_GPUOPS_MAX)
                    {
                      game_ops_flush(w);
                    }

                  op     = ops_add(g_ops);
                  op->op = NX_GPUOP_COPY;
                  op->x  = fx + x0 * s;
                  op->y  = fy + r * s + k;
                  op->w  = n;
                  op->h  = 1;
                  op->src = g->stage;
                  op->sx = x0 * s;
                  op->sy = r;
                  g->copies++;
                }
            }
        }
#endif

      if (src != g->shown)
        {
          int r;

          for (r = r0; r < r1; r++)
            {
              memcpy(g->shown + (size_t)r * g->fw + x0,
                     src + (size_t)r * g->fw + x0, x1 - x0);
            }
        }
    }

  game_ops_flush(w);
  g->haveframe = true;
}

/****************************************************************************
 * Private Functions: Doom
 ****************************************************************************/

/* An IWAD in dir: the first of g_iwads there, whatever the case */

static bool doom_find_in(const char *dir, char *path, size_t len)
{
  char found[sizeof(g_iwads) / sizeof(g_iwads[0])][16];
  struct dirent *de;
  DIR *d;
  size_t i;
  bool any = false;

  d = opendir(dir);
  if (d == NULL)
    {
      return false;
    }

  memset(found, 0, sizeof(found));
  while ((de = readdir(d)) != NULL)
    {
      for (i = 0; i < sizeof(g_iwads) / sizeof(g_iwads[0]); i++)
        {
          if (strcasecmp(de->d_name, g_iwads[i]) == 0)
            {
              strlcpy(found[i], de->d_name, sizeof(found[i]));
              any = true;
            }
        }
    }

  closedir(d);

  for (i = 0; any && i < sizeof(g_iwads) / sizeof(g_iwads[0]); i++)
    {
      if (found[i][0] != '\0')
        {
          snprintf(path, len, "%s%s%s", dir,
                   dir[strlen(dir) - 1] == '/' ? "" : "/", found[i]);
          return true;
        }
    }

  return false;
}

/* Look in the home directory, then CONFIG_FPCDESK_DOOM_WADDIRS */

static bool doom_find_wad(struct game_win_s *gw)
{
  char dirs[sizeof(CONFIG_FPCDESK_DOOM_WADDIRS) + 64];
  const char *home = getenv("HOME");
  char *save;
  char *dir;

  if (home == NULL || home[0] == '\0')
    {
      home = CONFIG_LIBC_HOMEDIR;
    }

  snprintf(dirs, sizeof(dirs), "%s %s %s", home, CONFIG_LIBC_HOMEDIR,
           CONFIG_FPCDESK_DOOM_WADDIRS);

  /* Where to put one: home, unless that is the root (there is none) and
   * there are other places
   */

  if (strcmp(home, "/") == 0 && CONFIG_FPCDESK_DOOM_WADDIRS[0] != '\0')
    {
      char first[sizeof(CONFIG_FPCDESK_DOOM_WADDIRS)];

      strlcpy(first, CONFIG_FPCDESK_DOOM_WADDIRS, sizeof(first));
      snprintf(gw->nowad, sizeof(gw->nowad), "Put one in %s.",
               strtok_r(first, " ", &save));
    }
  else
    {
      snprintf(gw->nowad, sizeof(gw->nowad), "Put one in %s.", home);
    }

  for (dir = strtok_r(dirs, " ", &save); dir != NULL;
       dir = strtok_r(NULL, " ", &save))
    {
      if (doom_find_in(dir, gw->wad, sizeof(gw->wad)))
        {
          strlcpy(gw->dir, gw->wad, sizeof(gw->dir));
          *strrchr(gw->dir, '/') = '\0';
          if (gw->dir[0] == '\0')
            {
              strlcpy(gw->dir, "/", sizeof(gw->dir));
            }

          return true;
        }
    }

  gw->wad[0] = '\0';
  return false;
}

/* Start the game: the nxdoom builtin, a task of its own */

static int doom_start(struct desk_win_s *w)
{
  struct game_win_s *gw = w->priv;
  struct deskgame_s *g = gw->g;
  const struct builtin_s *b;
  char session[20];
  char config[128];
  char *argv[8];
  int index;
  pid_t pid;

  index = builtin_isavail("nxdoom");
  b     = index >= 0 ? builtin_for_index(index) : NULL;
  if (b == NULL)
    {
      strlcpy(g->error, "The nxdoom program is not in this system",
              sizeof(g->error));
      return -ENOENT;
    }

  /* The configuration and the saved games beside the WAD */

  snprintf(config, sizeof(config), "%s%s.doom/", gw->dir,
           gw->dir[strlen(gw->dir) - 1] == '/' ? "" : "/");
  snprintf(session, sizeof(session), "%lx", (unsigned long)(uintptr_t)g);

  argv[0] = "-iwad";
  argv[1] = gw->wad;
  argv[2] = "-configdir";
  argv[3] = config;
  argv[4] = DESKGAME_ARG;
  argv[5] = session;
  argv[6] = NULL;

  pthread_mutex_lock(&g->lock);
  g->refs++;                                  /* The task's */
  pthread_mutex_unlock(&g->lock);

  pid = task_create("Doom", CONFIG_FPCDESK_DOOM_PRIORITY,
                    CONFIG_FPCDESK_DOOM_STACKSIZE, b->main, argv);
  if (pid < 0)
    {
      int err = errno;

      pthread_mutex_lock(&g->lock);
      snprintf(g->error, sizeof(g->error), "Cannot start Doom: %d", err);
      game_unref(g);
      return -err;
    }

  g->pid = pid;
  syslog(LOG_INFO, "desk: Doom (%d): %s\n", pid, gw->wad);
  return 0;
}

/****************************************************************************
 * Private Functions: the application
 ****************************************************************************/

static void doom_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  const struct desk_font_s *f = s >= 64 ? &g_desk_font_title :
                                          &g_desk_font_uib;

  icon_tile(o, x, y, s, RGB(0x9a, 0x1c, 0x10), RGB(0x2c, 0x06, 0x04), a);
  ops_rrect(o, x + s / 8, y + s * 5 / 8, s * 3 / 4, s / 4, LUT_R6, 6,
            RGB(0x10, 0x02, 0x02), (uint8_t)(a * 120 / 255));
  ops_text_centre(o, f, x + s / 2, y + s / 2 - text_height(f) / 2 - 2,
                  "DOOM", RGB(0xff, 0xc8, 0x40), a);
}

static void doom_draw(struct desk_win_s *w)
{
  struct game_win_s *gw = w->priv;
  struct deskgame_s *g = gw->g;
  bool error;

  pthread_mutex_lock(&g->lock);
  error = g->error[0] != '\0';
  pthread_mutex_unlock(&g->lock);

  if (g->haveframe && !error)
    {
      game_blit(w, g, g->shown, g->shownpal, true);
    }
  else
    {
      game_draw_message(w);
    }
}

/* The game has something: a frame, a status, its end */

static void doom_wake(struct desk_win_s *w)
{
  struct game_win_s *gw = w->priv;
  struct deskgame_s *g = gw->g;
  bool frame;
  bool status;
  bool detached;
  bool error;
  uint8_t *t;

  pthread_mutex_lock(&g->lock);
  frame = g->newframe;
  if (frame)
    {
      t          = g->work;
      g->work    = g->pending;
      g->pending = t;
      memcpy(g->workpal, g->pendpal, sizeof(g->workpal));
      g->newframe = false;
    }

  status       = g->newstatus;
  g->newstatus = false;
  detached     = g->detached;
  error        = g->error[0] != '\0';
  pthread_mutex_unlock(&g->lock);

  if (detached && !error)
    {
      desk_win_close(w);                        /* The game has quit */
      return;
    }

  if (error)
    {
      g->haveframe = false;
      game_draw_message(w);
      return;
    }

  if (frame)
    {
      game_blit(w, g, g->work, g->workpal, false);
      g->frames++;
    }
  else if (status && !g->haveframe)
    {
      game_draw_message(w);
    }
}

static void doom_tick(struct desk_win_s *w)
{
  struct game_win_s *gw = w->priv;
  struct deskgame_s *g = gw->g;

  /* No game data yet: look again, start as soon as there is */

  if (g->pid < 0 && gw->wad[0] == '\0' && g->error[0] == '\0')
    {
      if (doom_find_wad(gw))
        {
          doom_start(w);
          game_draw_message(w);
        }

      return;
    }

  /* A game that went without a word (ended from the System Monitor) */

  if (g->pid >= 0 && kill(g->pid, 0) < 0)
    {
      pid_t pid = g->pid;

      g->pid = -1;
      pthread_mutex_lock(&g->lock);
      if (!g->detached)
        {
          snprintf(g->error, sizeof(g->error), "The game (task %d) ended",
                   pid);
          g->detached = true;
          game_unref(g);
          g->haveframe = false;
          game_draw_message(w);
        }
      else
        {
          pthread_mutex_unlock(&g->lock);
        }
    }
}

static void doom_keyev(struct desk_win_s *w, const struct nx_key_s *k)
{
  struct game_win_s *gw = w->priv;
  struct deskgame_input_s in;

  memset(&in, 0, sizeof(in));
  in.type = DESKGAME_KEY;
  in.key  = *k;
  game_push(gw->g, &in);
}

/* The pointer over the frame: how far it moved, in the frame's pixels */

static void doom_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct game_win_s *gw = w->priv;
  struct deskgame_s *g = gw->g;
  struct deskgame_input_s in;
  int s = g->scale > 0 ? g->scale : 1;

  if (x < 0)
    {
      g->mvalid = false;                      /* Left the window */
      buttons   = 0;
    }

  memset(&in, 0, sizeof(in));
  in.type    = DESKGAME_MOUSE;
  in.buttons = buttons;
  if (g->mvalid && x >= 0)
    {
      in.dx = (x - g->mx) / s;
      in.dy = (y - g->my) / s;
    }

  if (in.dx != 0 || in.dy != 0 || x < 0 || !g->mvalid)
    {
      g->mx     = x;
      g->my     = y;
      g->mvalid = x >= 0;
    }

  if (in.dx != 0 || in.dy != 0 || buttons != g->buttons)
    {
      g->buttons = buttons;
      game_push(g, &in);
    }
}

static void doom_focus(struct desk_win_s *w)
{
  struct game_win_s *gw = w->priv;
  struct deskgame_input_s in;

  if (!w->active)
    {
      memset(&in, 0, sizeof(in));
      in.type = DESKGAME_FOCUS;
      game_push(gw->g, &in);
      gw->g->buttons = 0;
      gw->g->mvalid  = false;
    }

  game_setpause(gw->g, !w->active);
}

/* The window is going: the game is told to quit (it ends on its own) */

static void doom_close(struct desk_win_s *w)
{
  struct game_win_s *gw = w->priv;
  struct deskgame_s *g = gw->g;

  /* The staging window is the desktop's: it goes with the window */

  if (g->stage != NULL)
    {
      nx_closewindow(g->stage);
      g->stage = NULL;
    }

  pthread_mutex_lock(&g->lock);
  g->win  = NULL;
  g->quit = true;
  sem_post(&g->resume);
  game_unref(g);
}

static struct desk_win_s *doom_open(void)
{
  struct game_win_s *gw;
  struct deskgame_s *g;
  struct desk_win_s *w;

  if (g_ops == NULL)
    {
      g_ops = malloc(sizeof(*g_ops));
      if (g_ops == NULL)
        {
          return NULL;
        }

      g_ops->n = 0;
    }

  gw = calloc(1, sizeof(*gw));
  g  = calloc(1, sizeof(*g));
  if (gw == NULL || g == NULL)
    {
      free(gw);
      free(g);
      return NULL;
    }

  g->magic  = GAME_MAGIC;
  g->refs   = 1;                               /* The window's */
  g->pid    = -1;
  g->paused = false;
  pthread_mutex_init(&g->lock, NULL);
  sem_init(&g->resume, 0, 0);
  gw->g = g;

  w = desk_win_open(APP_DOOM, "Doom", GAME_W * 2, GAME_H * 2);
  if (w == NULL)
    {
      pthread_mutex_destroy(&g->lock);
      sem_destroy(&g->resume);
      free(gw);
      free(g);
      return NULL;
    }

  w->priv = gw;
  g->win  = w;

  if (doom_find_wad(gw))
    {
      doom_start(w);
    }

  game_draw_message(w);
  return w;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_doom =
{
  .name   = "Doom",
  .icon   = doom_icon,
  .open   = doom_open,
  .draw   = doom_draw,
  .tick   = doom_tick,
  .mouse  = doom_mouse,
  .keyev  = doom_keyev,
  .close  = doom_close,
  .wake   = doom_wake,
  .focus  = doom_focus,
  .single = true,
};

#endif /* CONFIG_GAMES_NXDOOM_DESKTOP */
