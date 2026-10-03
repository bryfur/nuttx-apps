/****************************************************************************
 * apps/graphics/fpcdesk/desk_client.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Windows for other tasks (apps/include/graphics/deskwin.h): a program
 * started anywhere (MicroPython's fpc module) opens a window, draws into it
 * and puts the widget kit's controls in it, without ever talking to the NX
 * server.
 *
 * The session: shared by the program's task (the client) and the window
 * (the desktop's main thread), freed by whichever lets go of it last.
 *
 *   - The client queues commands in a batch of its own (no lock), and
 *     deskwin_flush() moves the batch into the session's pending buffer
 *     (under the lock) and wakes the window (desk_wake()).  The window's
 *     wake takes all that is pending and carries it out: a full buffer
 *     makes the client wait for room, never the desktop.
 *   - Events go the other way, into a queue the client reads
 *     (deskwin_event()), pointer moves merged, frames and ticks at most
 *     one waiting.
 *   - The lock is a robust mutex: a client that dies holding it (deleted)
 *     leaves it to the desktop, which lets go of the session.  The window
 *     watches the client's task (w->pid): when it has gone, the window
 *     closes.  Everything the client sends is checked (ids, images, fonts,
 *     lengths), so a program in trouble cannot take the desktop down.
 *
 * The window: a kit (desk_kit.h) of one free box (KIT_F_FREE: its widgets
 * are where the program puts them), a custom widget over all of the client
 * area (the canvas: behind the others, it has the pointer where they are
 * not), then the program's widgets.  Drawing goes into the window's
 * pixels, cut to the canvas less the widgets (so it never covers them) and
 * to the program's clip, and is kept (DESKWIN_KEEP bytes of commands since
 * the last deskwin_clear()), so that when the canvas has to be drawn again
 * (an expose, a theme change, a parked window, a menu gone) the window
 * does it itself.  If more was drawn than it keeps, the program is asked
 * (DESKWIN_EV_EXPOSE).
 *
 * Their application is Python (APP_PYTHON): the windows of the programs,
 * and the launcher's Python (a terminal running python).
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <inttypes.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/param.h>

#include <nuttx/input/kbd_codec.h>
#include <nuttx/nx/nxbe.h>

#include <graphics/deskwin.h>

#include "desk.h"
#include "desk_image.h"
#include "desk_kit.h"

#ifdef CONFIG_FPCDESK_CLIENTS

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SESSION_MAGIC     0x4457494e           /* "DWIN" */
#define QLEN              64                   /* Events waiting */
#define BATCH             4096                 /* The client's batch */
#define PENDING           (16 * 1024)          /* Handed over, not yet done */
#define OPEN_MS           5000                 /* Waiting for a window */
#define MAX_TEXT          255                  /* A widget's text */
#define MAX_ITEMS         4096                 /* A list's items, bytes */
#define MAX_PIECES        64                   /* The canvas less widgets */
#define DESKWIN_SURFACES  8                    /* A program's at once */
#define CANVAS_ID         250                  /* The kit's id of it */
#define NWG               (DESKWIN_WIDGETS + 4)
#define MIN_W             120
#define MIN_H             60

/* Commands: a header, then 32-bit arguments, then a string or two */

#define CMD_FILL          1                    /* x y w h c */
#define CMD_RECT          2                    /* x y w h c alpha r */
#define CMD_GRAD          3                    /* x y w h c0 c1 alpha vert */
#define CMD_LINE          4                    /* x0 y0 x1 y1 c alpha */
#define CMD_TEXT          5                    /* x y font c alpha, s */
#define CMD_IMAGE         6                    /* img sx sy w h dx dy alpha */
#define CMD_COPY          7                    /* sx sy w h dx dy */
#define CMD_CLIP          8                    /* x y w h (w <= 0: none) */
#define CMD_CLEAR         9                    /* c */
#define CMD_TITLE         10                   /* s */
#define CMD_FRAMES        11                   /* on */
#define CMD_WIDGET        12                   /* id type x y w h value min
                                                * max style, text, items */
#define CMD_WSET          13                   /* id what value, s */
#define CMD_WMOVE         14                   /* id x y w h */
#define CMD_WDEL          15                   /* id */
#define CMD_IMGFREE       16                   /* img */
#define CMD_CLOSE         17
#define CMD_SURFFREE      19                   /* surface */
#define CMD_RAISE         20                   /* (Restored, active) */
#define CMD_BUTTON        21                   /* s: the title bar's button
                                                * ("": none) */
#define CMD_OPS           18                   /* n target, n nx_gpuop_s:
                                                * the desktop's own drawing
                                                * operations, frame
                                                * coordinates, into the
                                                * window (target 0) or a
                                                * surface of the program's
                                                * (a program built from a
                                                * desktop application,
                                                * desk_program.c) */

#define CMD_HDR           4
#define CMD_MAXARGS       10

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct cmd_s
{
  uint8_t  op;
  uint8_t  nargs;
  uint16_t len;                        /* All of it, a multiple of 4 */
};

/* A widget as the program has it (the main thread's) */

struct cwidget_s
{
  bool     used;
  bool     enabled;
  bool     visible;
  uint8_t  type;                       /* DESKWIN_* */
  uint8_t  style;
  int16_t  x;                          /* As asked (client coordinates) */
  int16_t  y;
  int16_t  w;                          /* 0: natural */
  int16_t  h;
  int32_t  value;
  int32_t  min;
  int32_t  max;
  char    *text;                       /* Label, button, placeholder */
  char    *items;                      /* List, drop-down: the lines */
  const char **itemv;
  int      nitems;
  struct ui_field_s *field;
  struct kit_list_s *list;
};

/* The session */

struct deskwin_s
{
  uint32_t magic;
  struct deskwin_s *next;              /* All of them (g_sessions) */
  pthread_mutex_t lock;
  sem_t    evsem;                      /* Events for the client */
  sem_t    space;                      /* Pending has been taken */
  sem_t    reply;                      /* Open, a sync, an image */
  int      refs;                       /* The client, the desktop */
  pid_t    pid;                        /* The client */
  int      app;                        /* Its window's application, or -1 */

  /* Under the lock */

  struct desk_win_s *win;              /* NULL until open, and once gone */
  bool     opened;                     /* Open done (win, or failed) */
  bool     gone;                       /* The window has gone */
  bool     clientgone;                 /* deskwin_close(), or it died */
  bool     waitsync;                   /* The client waits for done */
  char     title[40];
  int16_t  reqw;
  int16_t  reqh;
  int16_t  cw;                         /* The client area now */
  int16_t  ch;
  uint8_t *pend;
  size_t   npend;
  uint32_t seq;                        /* Batches handed over */
  uint32_t done;                       /* ... carried out */
  struct deskwin_event_s q[QLEN];
  int      qhead;
  int      qn;
  bool     frameq;                     /* A frame is waiting */
  bool     tickq;
  bool     wakeq;                      /* deskwin_wake(): the client's own */

  struct
  {
    uint8_t  type;
    int16_t  nw;                       /* Its natural size */
    int16_t  nh;
    int32_t  value;
    char    *text;
  } mirror[DESKWIN_WIDGETS + 1];

  /* An image to load (under the lock; the client waits on reply) */

  bool     imgreq;
  char     imgpath[128];
  int16_t  imgmaxw;
  int16_t  imgmaxh;
  int      imgret;
  int16_t  imgw;
  int16_t  imgh;
  char     imgerr[80];

  /* A surface to make (under the lock; the client waits on reply) */

  bool     surfreq;
  int16_t  surfw;
  int16_t  surfh;
  volatile bool *surflost;             /* The client's: set when lost */
  NXWINDOW surfret;

  /* The client's own (its task only) */

  uint8_t  batch[BATCH];
  size_t   nbatch;
};

/* The window's state (w->priv) */

struct client_win_s
{
  uint32_t kind;                       /* PYTHON_CLIENT (desk_python) */
  struct deskwin_s *s;
  struct desk_win_s *w;
  struct kit_s kit;
  struct kit_widget_s wg[NWG];
  struct cwidget_s cw[DESKWIN_WIDGETS + 1];
  bool     rebuild;                    /* Widgets added or removed */

  /* Drawing */

  uint8_t *take;                       /* Taken from pending */
  size_t   takecap;
  struct kit_rect_s clip;              /* The program's (frame), w 0: none */
  uint8_t *keep;                       /* What was drawn since a clear */
  size_t   nkeep;
  bool     keepok;                     /* ... all of it */

  /* Images */

  struct desk_image_s *img[DESKWIN_IMAGES];
  char    *imgpath[DESKWIN_IMAGES];
  int16_t  imgmaxw[DESKWIN_IMAGES];
  int16_t  imgmaxh[DESKWIN_IMAGES];

  /* Surfaces the program draws into and copies from (desk_surface_open()
   * for it): the desktop's, its lost flags passed on to the program's
   */

  char button[8];                      /* Its title bar's button's label */
  NXWINDOW surf[DESKWIN_SURFACES];
  volatile bool *surfclient[DESKWIN_SURFACES];
  volatile bool surflost[DESKWIN_SURFACES];

  /* The key being handed to the kit (its KIT_EV_KEY is that key) */

  struct nx_key_s key;
  bool     havekey;
};

struct emit_s
{
  NXWINDOW hwnd;
  const struct kit_rect_s *pieces;
  int      npieces;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_slock = PTHREAD_MUTEX_INITIALIZER;
static struct deskwin_s *g_sessions;
static bool g_ready;

/* The main thread's lists: what is being made, what goes to the server
 * (from the heap: PSRAM on the board)
 */

static struct desk_ops_s *g_tmp;
static struct desk_ops_s *g_out;

static const struct desk_font_s * const g_fonts[DESKWIN_FONTS] =
{
  &g_desk_font_ui, &g_desk_font_uib, &g_desk_font_small,
  &g_desk_font_title, &g_desk_font_big, &g_desk_font_mono,
  &g_desk_font_monob
};

/****************************************************************************
 * Private Functions: the session
 ****************************************************************************/

static void s_lock(struct deskwin_s *s)
{
  int ret = pthread_mutex_lock(&s->lock);

  if (ret == EOWNERDEAD)
    {
      /* The client died holding it */

      pthread_mutex_consistent(&s->lock);
      s->clientgone = true;
      s->npend = 0;
    }
}

static void s_unlock(struct deskwin_s *s)
{
  pthread_mutex_unlock(&s->lock);
}

static void s_free(struct deskwin_s *s)
{
  struct deskwin_s **pp;
  int i;

  pthread_mutex_lock(&g_slock);
  for (pp = &g_sessions; *pp != NULL; pp = &(*pp)->next)
    {
      if (*pp == s)
        {
          *pp = s->next;
          break;
        }
    }

  pthread_mutex_unlock(&g_slock);

  for (i = 0; i <= DESKWIN_WIDGETS; i++)
    {
      free(s->mirror[i].text);
    }

  s->magic = 0;
  sem_destroy(&s->evsem);
  sem_destroy(&s->space);
  sem_destroy(&s->reply);
  pthread_mutex_destroy(&s->lock);
  free(s->pend);
  free(s);
}

/* Let go of it (lock held; unlocks): the last one frees it */

static void s_unref(struct deskwin_s *s)
{
  bool last = --s->refs == 0;

  s_unlock(s);
  if (last)
    {
      s_free(s);
    }
}

/* An event for the client (lock held) */

static void s_push(struct deskwin_s *s, const struct deskwin_event_s *ev)
{
  if (s->clientgone)
    {
      return;
    }

  if ((ev->type == DESKWIN_EV_MOUSE || ev->type == DESKWIN_EV_EXPOSE) &&
      s->qn > 0)
    {
      struct deskwin_event_s *last = &s->q[(s->qhead + s->qn - 1) % QLEN];

      if (last->type == DESKWIN_EV_MOUSE && ev->type == DESKWIN_EV_MOUSE &&
          last->buttons == ev->buttons)
        {
          *last = *ev;
          return;
        }

      if (last->type == DESKWIN_EV_EXPOSE && ev->type == DESKWIN_EV_EXPOSE)
        {
          /* One rectangle: both */

          int x0 = MIN(last->x, ev->x);
          int y0 = MIN(last->y, ev->y);
          int x1 = MAX(last->x + last->w, ev->x + ev->w);
          int y1 = MAX(last->y + last->h, ev->y + ev->h);

          last->x = x0;
          last->y = y0;
          last->w = x1 - x0;
          last->h = y1 - y0;
          return;
        }
    }

  if (s->qn == QLEN)
    {
      return;                                   /* Full: dropped */
    }

  s->q[(s->qhead + s->qn) % QLEN] = *ev;
  s->qn++;
  sem_post(&s->evsem);
}

static void push(struct deskwin_s *s, const struct deskwin_event_s *ev)
{
  s_lock(s);
  s_push(s, ev);
  s_unlock(s);
}

static void push_simple(struct deskwin_s *s, int type, int value)
{
  struct deskwin_event_s ev;

  memset(&ev, 0, sizeof(ev));
  ev.type  = type;
  ev.value = value;
  push(s, &ev);
}

/* The mirror of a widget (lock held) */

static void mirror_text(struct deskwin_s *s, int id, const char *text)
{
  free(s->mirror[id].text);
  s->mirror[id].text = text != NULL ? strdup(text) : NULL;
}

/****************************************************************************
 * Private Functions: geometry
 ****************************************************************************/

static struct kit_rect_s rect(int x, int y, int w, int h)
{
  struct kit_rect_s r;

  r.x = x;
  r.y = y;
  r.w = w < 0 ? 0 : w;
  r.h = h < 0 ? 0 : h;
  return r;
}

static bool r_and(struct kit_rect_s *a, const struct kit_rect_s *b)
{
  int x0 = MAX(a->x, b->x);
  int y0 = MAX(a->y, b->y);
  int x1 = MIN(a->x + a->w, b->x + b->w);
  int y1 = MIN(a->y + a->h, b->y + b->h);

  *a = rect(x0, y0, x1 - x0, y1 - y0);
  return a->w > 0 && a->h > 0;
}

/* area less holes: rectangles into out (at most max); -1 if more */

static int pieces(const struct kit_rect_s *area,
                  const struct kit_rect_s *holes, int nholes,
                  struct kit_rect_s *out, int max)
{
  struct kit_rect_s tmp[MAX_PIECES];
  int n = 0;
  int i;
  int j;

  if (area->w <= 0 || area->h <= 0)
    {
      return 0;
    }

  out[n++] = *area;
  for (i = 0; i < nholes; i++)
    {
      const struct kit_rect_s *h = &holes[i];
      int m = 0;

      for (j = 0; j < n; j++)
        {
          struct kit_rect_s p = out[j];
          int px1 = p.x + p.w;
          int py1 = p.y + p.h;
          int hx1 = h->x + h->w;
          int hy1 = h->y + h->h;
          int top;
          int bot;

          if (h->x >= px1 || hx1 <= p.x || h->y >= py1 || hy1 <= p.y)
            {
              if (m == MAX_PIECES)
                {
                  return -1;
                }

              tmp[m++] = p;
              continue;
            }

          if (m + 4 > MAX_PIECES)
            {
              return -1;
            }

          top = MAX(p.y, h->y);
          bot = MIN(py1, hy1);
          if (h->y > p.y)
            {
              tmp[m++] = rect(p.x, p.y, p.w, h->y - p.y);
            }

          if (hy1 < py1)
            {
              tmp[m++] = rect(p.x, hy1, p.w, py1 - hy1);
            }

          if (h->x > p.x)
            {
              tmp[m++] = rect(p.x, top, h->x - p.x, bot - top);
            }

          if (hx1 < px1)
            {
              tmp[m++] = rect(hx1, top, px1 - hx1, bot - top);
            }
        }

      if (m > max)
        {
          return -1;
        }

      memcpy(out, tmp, m * sizeof(tmp[0]));
      n = m;
    }

  return n;
}

/****************************************************************************
 * Private Functions: drawing (the main thread)
 ****************************************************************************/

/* g_tmp's operations, cut to each piece, into g_out (sent as it fills) */

static void emit(struct emit_s *e)
{
  int i;
  int p;

  for (i = 0; i < g_tmp->n; i++)
    {
      for (p = 0; p < e->npieces; p++)
        {
          const struct kit_rect_s *r = &e->pieces[p];
          struct nx_gpuop_s op = g_tmp->op[i];
          int d;

          if ((d = r->x - op.x) > 0)
            {
              op.x += d;
              op.w -= d;
              op.sx += d;
            }

          if ((d = r->y - op.y) > 0)
            {
              op.y += d;
              op.h -= d;
              op.sy += d;
            }

          if (op.x + op.w > r->x + r->w)
            {
              op.w = r->x + r->w - op.x;
            }

          if (op.y + op.h > r->y + r->h)
            {
              op.h = r->y + r->h - op.y;
            }

          if (op.w <= 0 || op.h <= 0)
            {
              continue;
            }

          if (g_out->n == NX_GPUOPS_MAX)
            {
              ops_draw(e->hwnd, g_out);
            }

          g_out->op[g_out->n++] = op;
        }
    }

  g_tmp->n = 0;
}

/* Round shapes of any radius: a disc's table in LUT_APP (the shift makes
 * large ones from a table of at most 30)
 */

static void lut_disc(uint8_t lut[NX_BLEND_LUT_N][NX_BLEND_LUT_N], int r)
{
  int dx;
  int dy;
  int i;
  int j;

  for (dy = 0; dy < NX_BLEND_LUT_N; dy++)
    {
      for (dx = 0; dx < NX_BLEND_LUT_N; dx++)
        {
          int n = 0;

          for (j = 0; j < 8; j++)
            {
              for (i = 0; i < 8; i++)
                {
                  int u = -dx * 16 + i * 2 + 1;
                  int v = -dy * 16 + j * 2 + 1;

                  u = u < 0 ? -u : 0;
                  v = v < 0 ? -v : 0;
                  n += u * u + v * v <= r * r * 256;
                }
            }

          lut[dy][dx] = (uint8_t)((n * 255 + 32) / 64);
        }
    }
}

/* A run of a row at a coverage (a sixteenth a sample) */

static void run(struct emit_s *e, int x, int y, int w, uint16_t c,
                int alpha, int cov)
{
  int a = alpha * cov / 16;

  if (w <= 0 || a <= 0)
    {
      return;
    }

  if (a >= 255)
    {
      ops_fill(g_tmp, x, y, w, 1, c);
    }
  else
    {
      ops_blend(g_tmp, x, y, w, 1, c, a);
    }

  if (g_tmp->n >= NX_GPUOPS_MAX - 2)
    {
      emit(e);
    }
}

/* Round rectangles whose radius the shape tables cannot take: the corners
 * on the CPU (4 x 4 samples a pixel), as runs of pixels of equal coverage;
 * the rest FILLs
 */

static void rrect_cpu(struct emit_s *e, int x, int y, int w, int h,
                      uint16_t c, int alpha, int r)
{
  int r8 = r * 8;
  int row;

  if (h > 2 * r)
    {
      if (alpha >= 255)
        {
          ops_fill(g_tmp, x, y + r, w, h - 2 * r, c);
        }
      else
        {
          ops_blend(g_tmp, x, y + r, w, h - 2 * r, c, alpha);
        }
    }

  for (row = 0; row < r; row++)
    {
      int start = 0;
      int prev = -1;
      int px;

      for (px = 0; px <= r; px++)
        {
          int cov = 16;
          int i;
          int j;

          if (px < r)
            {
              cov = 0;
              for (j = 0; j < 4; j++)
                {
                  for (i = 0; i < 4; i++)
                    {
                      int dx = r8 - (px * 8 + 2 * i + 1);
                      int dy = r8 - (row * 8 + 2 * j + 1);

                      cov += dx * dx + dy * dy <= r8 * r8;
                    }
                }
            }

          if (cov != prev || px == r)
            {
              if (prev > 0)
                {
                  int n = px - start;

                  /* The left corner, the right one (mirrored); top and
                   * bottom rows alike
                   */

                  run(e, x + start, y + row, n, c, alpha, prev);
                  run(e, x + w - start - n, y + row, n, c, alpha, prev);
                  run(e, x + start, y + h - 1 - row, n, c, alpha, prev);
                  run(e, x + w - start - n, y + h - 1 - row, n, c, alpha,
                      prev);
                }

              start = px;
              prev  = cov;
            }
        }

      /* Between the corners */

      if (w > 2 * r)
        {
          run(e, x + r, y + row, w - 2 * r, c, alpha, 16);
          run(e, x + r, y + h - 1 - row, w - 2 * r, c, alpha, 16);
        }
    }

  emit(e);
}

static void draw_rrect(struct emit_s *e, int x, int y, int w, int h,
                       uint16_t c, int alpha, int r)
{
  int slot;

  r = MIN(r, MIN(w, h) / 2);
  if (r <= 0)
    {
      if (alpha >= 255)
        {
          ops_fill(g_tmp, x, y, w, h, c);
        }
      else
        {
          ops_blend(g_tmp, x, y, w, h, c, alpha);
        }

      emit(e);
      return;
    }

  if (r > NX_BLEND_LUT_N - 2)
    {
      rrect_cpu(e, x, y, w, h, c, alpha, r);
      return;
    }

  if (r == 3 || r == 6 || r == 12)
    {
      slot = r == 3 ? LUT_R3 : r == 6 ? LUT_R6 : LUT_R12;
    }
  else
    {
      /* The table goes first: what was made with the last one is sent */

      slot = LUT_APP;
      if (g_out->n > 0)
        {
          ops_draw(e->hwnd, g_out);
        }

      desk_lut_app(0x50590000 | r, lut_disc, r);
    }

  ops_rrect(g_tmp, x, y, w, h, slot, r, c, alpha);
  emit(e);
}

/* A line: runs of pixels in a row (or a column), each one FILL or BLEND */

static void draw_line(struct emit_s *e, int x0, int y0, int x1, int y1,
                      uint16_t c, int alpha)
{
  int dx = abs(x1 - x0);
  int dy = abs(y1 - y0);
  int sx = x0 < x1 ? 1 : -1;
  int sy = y0 < y1 ? 1 : -1;
  int err = dx - dy;
  int rx = x0;
  int ry = y0;
  int n = 0;

  for (; ; )
    {
      int e2 = 2 * err;
      bool last = x0 == x1 && y0 == y1;
      bool newrun;
      int nx = x0;
      int ny = y0;

      if (!last)
        {
          if (e2 > -dy)
            {
              err -= dy;
              nx += sx;
            }

          if (e2 < dx)
            {
              err += dx;
              ny += sy;
            }
        }

      n++;
      newrun = last || (dx >= dy ? ny != y0 : nx != x0);
      if (newrun)
        {
          int x = dx >= dy ? MIN(rx, x0) : x0;
          int y = dx >= dy ? y0 : MIN(ry, y0);
          int w = dx >= dy ? n : 1;
          int h = dx >= dy ? 1 : n;

          if (alpha >= 255)
            {
              ops_fill(g_tmp, x, y, w, h, c);
            }
          else
            {
              ops_blend(g_tmp, x, y, w, h, c, alpha);
            }

          if (g_tmp->n >= NX_GPUOPS_MAX - 1)
            {
              emit(e);
            }

          n  = 0;
          rx = nx;
          ry = ny;
        }

      if (last)
        {
          break;
        }

      x0 = nx;
      y0 = ny;
    }

  emit(e);
}

static void draw_text(struct emit_s *e, int x, int y, int font, uint16_t c,
                      int alpha, const char *s)
{
  const struct desk_font_s *f = g_fonts[font];
  char chunk[129];

  while (*s != '\0')
    {
      size_t n = strlen(s);

      n = n > 128 ? 128 : n;
      memcpy(chunk, s, n);
      chunk[n] = '\0';
      x += ops_text(g_tmp, f, x, y, chunk, c, alpha);
      emit(e);
      s += n;
    }
}

static int arg(const struct cmd_s *cmd, int i)
{
  return i < cmd->nargs ? ((const int32_t *)(cmd + 1))[i] : 0;
}

static const char *cmd_str(const struct cmd_s *cmd, int which)
{
  const char *s = (const char *)(cmd + 1) + cmd->nargs * 4;
  const char *end = (const char *)cmd + cmd->len;

  while (which-- > 0 && s < end)
    {
      s += strnlen(s, end - s) + 1;
    }

  return s < end && memchr(s, '\0', end - s) != NULL ? s : "";
}

/* A source an operation of the program's may read: none (a fill, or the
 * window itself), the desktop's font atlas, an image of its own
 */

static bool client_src_ok(struct client_win_s *c, FAR void *src)
{
  int i;

  if (src == NULL || src == g_desk.atlas)
    {
      return true;
    }

  for (i = 0; i < DESKWIN_IMAGES; i++)
    {
      if (c->img[i] != NULL && src == c->img[i]->hwnd)
        {
          return true;
        }
    }

  for (i = 0; i < DESKWIN_SURFACES; i++)
    {
      if (c->surf[i] != NULL && src == c->surf[i])
        {
          return true;
        }
    }

  return false;
}

/* One of the program's surfaces, or -1 */

static int client_surface(struct client_win_s *c, uintptr_t h)
{
  int i;

  for (i = 0; h != 0 && i < DESKWIN_SURFACES; i++)
    {
      if ((uintptr_t)c->surf[i] == h)
        {
          return i;
        }
    }

  return -1;
}

/* One drawing command, frame coordinates made from the client's */

static void draw_cmd(struct client_win_s *c, struct emit_s *e,
                     const struct cmd_s *cmd)
{
  int y0 = TITLE_H;
  int a4;

  switch (cmd->op)
    {
      case CMD_FILL:
        ops_fill(g_tmp, arg(cmd, 0), arg(cmd, 1) + y0, arg(cmd, 2),
                 arg(cmd, 3), arg(cmd, 4));
        emit(e);
        break;

      case CMD_CLEAR:
        ops_fill(g_tmp, 0, y0, 4096, 4096, arg(cmd, 0));
        emit(e);
        break;

      case CMD_RECT:
        draw_rrect(e, arg(cmd, 0), arg(cmd, 1) + y0, arg(cmd, 2),
                   arg(cmd, 3), arg(cmd, 4), arg(cmd, 5) & 255,
                   MAX(0, arg(cmd, 6)));
        break;

      case CMD_GRAD:
        a4 = arg(cmd, 6) & 255;
        if (arg(cmd, 7))
          {
            ops_vgrad(g_tmp, arg(cmd, 0), arg(cmd, 1) + y0, arg(cmd, 2),
                      arg(cmd, 3), arg(cmd, 4), arg(cmd, 5), a4);
          }
        else
          {
            ops_hgrad(g_tmp, arg(cmd, 0), arg(cmd, 1) + y0, arg(cmd, 2),
                      arg(cmd, 3), arg(cmd, 4), arg(cmd, 5), a4);
          }

        emit(e);
        break;

      case CMD_LINE:
        draw_line(e, arg(cmd, 0), arg(cmd, 1) + y0, arg(cmd, 2),
                  arg(cmd, 3) + y0, arg(cmd, 4), arg(cmd, 5) & 255);
        break;

      case CMD_TEXT:
        if (arg(cmd, 2) >= 0 && arg(cmd, 2) < DESKWIN_FONTS)
          {
            draw_text(e, arg(cmd, 0), arg(cmd, 1) + y0, arg(cmd, 2),
                      arg(cmd, 3), arg(cmd, 4) & 255, cmd_str(cmd, 0));
          }
        break;

      case CMD_IMAGE:
        {
          int i = arg(cmd, 0);
          struct desk_image_s *img;
          int w;
          int h;

          if (i < 0 || i >= DESKWIN_IMAGES || c->img[i] == NULL)
            {
              break;
            }

          img = c->img[i];
          w   = arg(cmd, 3) < 0 ? img->w : arg(cmd, 3);
          h   = arg(cmd, 4) < 0 ? img->h : arg(cmd, 4);
          if ((arg(cmd, 7) & 255) >= 255 && arg(cmd, 7) >= 0)
            {
              img_copy(g_tmp, img, arg(cmd, 1), arg(cmd, 2), w, h,
                       arg(cmd, 5), arg(cmd, 6) + y0);
            }
          else
            {
              img_blend(g_tmp, img, arg(cmd, 1), arg(cmd, 2), w, h,
                        arg(cmd, 5), arg(cmd, 6) + y0, arg(cmd, 7) & 255);
            }

          emit(e);
        }
        break;

      case CMD_OPS:
        {
          const struct nx_gpuop_s *op = (const struct nx_gpuop_s *)
                                        ((const int32_t *)(cmd + 1) +
                                         cmd->nargs);
          int n = arg(cmd, 0);
          int i;

          if (n < 0 || cmd->nargs * 4 + CMD_HDR + n * sizeof(*op) >
              cmd->len)
            {
              break;
            }

          /* Into one of its surfaces: all of it there, as it is */

          if (arg(cmd, 1) != 0)
            {
              int si = client_surface(c, (uint32_t)arg(cmd, 1));

              if (si < 0)
                {
                  break;
                }

              ops_draw(e->hwnd, g_out);          /* (What was before) */
              g_tmp->n = 0;
              for (i = 0; i < n; i++)
                {
                  if ((op[i].op == NX_GPUOP_FILL ||
                       op[i].op == NX_GPUOP_BLEND ||
                       op[i].op == NX_GPUOP_COPY) &&
                      client_src_ok(c, op[i].src))
                    {
                      if (g_tmp->n == NX_GPUOPS_MAX)
                        {
                          ops_draw(c->surf[si], g_tmp);
                        }

                      g_tmp->op[g_tmp->n++] = op[i];
                    }
                }

              ops_draw(c->surf[si], g_tmp);
              break;
            }

          /* Each one checked: an operation the desktop makes, from a
           * surface the program may use (none, the font atlas, its images)
           */

          for (i = 0; i < n; i++)
            {
              if ((op[i].op != NX_GPUOP_FILL && op[i].op != NX_GPUOP_BLEND &&
                   op[i].op != NX_GPUOP_COPY) || !client_src_ok(c, op[i].src))
                {
                  continue;
                }

              if (g_tmp->n == NX_GPUOPS_MAX)
                {
                  emit(e);
                }

              g_tmp->op[g_tmp->n++] = op[i];
            }

          emit(e);
        }
        break;

      case CMD_COPY:
        {
          struct nx_gpuop_s *op = ops_add(g_tmp);

          op->op = NX_GPUOP_COPY;
          op->sx = arg(cmd, 0);
          op->sy = arg(cmd, 1) + y0;
          op->w  = arg(cmd, 2);
          op->h  = arg(cmd, 3);
          op->x  = arg(cmd, 4);
          op->y  = arg(cmd, 5) + y0;
          emit(e);
        }
        break;

      default:
        break;
    }
}

/* Where drawing goes: the canvas (and within [area)), the program's clip,
 * less the widgets shown
 */

static int where(struct client_win_s *c, const struct kit_rect_s *area,
                 struct kit_rect_s *out)
{
  struct kit_rect_s holes[DESKWIN_WIDGETS];
  struct kit_rect_s r = rect(0, TITLE_H, c->w->csize.w,
                             c->w->csize.h);
  int nholes = 0;
  int n;
  int i;

  if (area != NULL && !r_and(&r, area))
    {
      return 0;
    }

  if (c->clip.w > 0 && !r_and(&r, &c->clip))
    {
      return 0;
    }

  for (i = 1; i <= DESKWIN_WIDGETS; i++)
    {
      struct kit_widget_s *wg;

      if (!c->cw[i].used || !c->cw[i].visible)
        {
          continue;
        }

      wg = kit_get(&c->kit, i);
      if (wg != NULL && wg->w > 0 && wg->h > 0)
        {
          holes[nholes++] = rect(wg->x, wg->y, wg->w, wg->h);
        }
    }

  n = pieces(&r, holes, nholes, out, MAX_PIECES);
  if (n < 0)
    {
      /* Too many pieces: all of it, the widgets drawn again after */

      out[0] = r;
      n = 1;
      for (i = 1; i <= DESKWIN_WIDGETS; i++)
        {
          if (c->cw[i].used)
            {
              kit_invalidate(&c->kit, i);
            }
        }
    }

  return n;
}

/* Keep a drawing command for drawing the canvas again */

static void keep(struct client_win_s *c, const struct cmd_s *cmd)
{
  if (cmd->op == CMD_CLEAR)
    {
      c->nkeep  = 0;
      c->keepok = true;
    }
  else if (cmd->op == CMD_COPY)
    {
      c->keepok = false;                 /* Depends on what was there */
    }
  else if (cmd->op == CMD_OPS && arg(cmd, 1) != 0)
    {
      return;                            /* (A surface's, not the canvas) */
    }
  else if (cmd->op == CMD_OPS)
    {
      const struct nx_gpuop_s *op = (const struct nx_gpuop_s *)
                                    ((const int32_t *)(cmd + 1) +
                                     cmd->nargs);
      int i;

      for (i = 0; i < arg(cmd, 0) && c->keepok; i++)
        {
          if (op[i].op == NX_GPUOP_COPY && op[i].src == NULL)
            {
              c->keepok = false;         /* (The same) */
            }
        }
    }

  if (!c->keepok)
    {
      return;
    }

  if (c->keep == NULL)
    {
      c->keep = malloc(DESKWIN_KEEP);
      if (c->keep == NULL)
        {
          c->keepok = false;
          return;
        }
    }

  if (c->nkeep + cmd->len > DESKWIN_KEEP)
    {
      c->keepok = false;
      return;
    }

  memcpy(c->keep + c->nkeep, cmd, cmd->len);
  c->nkeep += cmd->len;
}

/* What is kept, again, into [area) (frame coordinates) */

static void replay(struct client_win_s *c, const struct kit_rect_s *area)
{
  struct desk_win_s *w = c->w;
  struct kit_rect_s piece[MAX_PIECES];
  struct kit_rect_s saved = c->clip;
  struct emit_s e;
  size_t off = 0;

  e.hwnd    = (NXWINDOW)w->hwnd;
  e.pieces  = piece;
  c->clip.w = 0;
  e.npieces = where(c, area, piece);

  ops_fill(g_tmp, area->x, area->y, area->w, area->h, g_theme.window_bg);
  emit(&e);

  while (c->keepok && off + CMD_HDR <= c->nkeep)
    {
      const struct cmd_s *cmd = (const struct cmd_s *)(c->keep + off);

      if (cmd->op == CMD_CLIP)
        {
          c->clip = arg(cmd, 2) > 0 ?
                    rect(arg(cmd, 0), arg(cmd, 1) + TITLE_H, arg(cmd, 2),
                         arg(cmd, 3)) : rect(0, 0, 0, 0);
          e.npieces = where(c, area, piece);
        }
      else
        {
          draw_cmd(c, &e, cmd);
        }

      off += cmd->len;
    }

  ops_draw(e.hwnd, g_out);
  c->clip = saved;

  if (!c->keepok)
    {
      struct deskwin_event_s ev;

      memset(&ev, 0, sizeof(ev));
      ev.type = DESKWIN_EV_EXPOSE;
      ev.x    = area->x;
      ev.y    = area->y - TITLE_H;
      ev.w    = area->w;
      ev.h    = area->h;
      push(c->s, &ev);
    }
}

/****************************************************************************
 * Private Functions: the widgets (the main thread)
 ****************************************************************************/

static const char *list_text(struct kit_s *k, struct kit_widget_s *wg,
                             int row, int col, char *buf, int size)
{
  struct client_win_s *c = k->priv;
  struct cwidget_s *cw = &c->cw[wg->id <= DESKWIN_WIDGETS ? wg->id : 0];

  return row >= 0 && row < cw->nitems ? cw->itemv[row] : "";
}

/* A list's or drop-down's lines, from "a\nb\nc" */

static void set_items(struct cwidget_s *cw, const char *items)
{
  int n = 1;
  char *p;

  free(cw->items);
  free(cw->itemv);
  cw->items  = NULL;
  cw->itemv  = NULL;
  cw->nitems = 0;
  if (items == NULL || items[0] == '\0')
    {
      return;
    }

  cw->items = strndup(items, MAX_ITEMS);
  if (cw->items == NULL)
    {
      return;
    }

  for (p = cw->items; *p != '\0'; p++)
    {
      n += *p == '\n';
    }

  cw->itemv = malloc(n * sizeof(char *));
  if (cw->itemv == NULL)
    {
      return;
    }

  for (p = cw->items; p != NULL; )
    {
      char *nl = strchr(p, '\n');

      cw->itemv[cw->nitems++] = p;
      if (nl != NULL)
        {
          *nl = '\0';
          p = nl + 1;
        }
      else
        {
          p = NULL;
        }
    }
}

static void cw_free(struct cwidget_s *cw)
{
  free(cw->text);
  free(cw->items);
  free(cw->itemv);
  free(cw->field);
  free(cw->list);
  memset(cw, 0, sizeof(*cw));
}

static int label_style(int style)
{
  static const uint8_t fonts[] =
  {
    KIT_TEXT_BODY, KIT_TEXT_BOLD, KIT_TEXT_SMALL, KIT_TEXT_HEADING,
    KIT_TEXT_DISPLAY, KIT_TEXT_BODY, KIT_TEXT_BOLD, KIT_TEXT_BODY
  };

  return fonts[style & 7] | (style & 8 ? KIT_TEXT_DIM : 0) |
         (style & 16 ? KIT_TEXT_CENTRE : 0) |
         (style & 32 ? KIT_TEXT_RIGHT : 0);
}

/* Place the canvas and the widgets: the kit lays out nothing (a free box) */

static void place(struct client_win_s *c)
{
  struct desk_win_s *w = c->w;
  struct kit_widget_s *wg;
  int i;

  wg = kit_get(&c->kit, CANVAS_ID);
  if (wg != NULL)
    {
      wg->x = 0;
      wg->y = TITLE_H;
      wg->w = w->csize.w;
      wg->h = w->csize.h;
    }

  s_lock(c->s);
  for (i = 1; i <= DESKWIN_WIDGETS; i++)
    {
      struct cwidget_s *cw = &c->cw[i];
      int nw;
      int nh;

      wg = cw->used ? kit_get(&c->kit, i) : NULL;
      if (wg == NULL)
        {
          continue;
        }

      kit_natural_of(&c->kit, wg, &nw, &nh);
      if (cw->type == DESKWIN_FIELD || cw->type == DESKWIN_SLIDER ||
          cw->type == DESKWIN_PROGRESS)
        {
          nw = MAX(nw, KIT_FIELD_W);
        }
      else if (cw->type == DESKWIN_LIST)
        {
          nw = MAX(nw, 160);
          nh = MAX(nh, 5 * KIT_ROW_H);
        }

      wg->x = cw->x;
      wg->y = cw->y + TITLE_H;
      wg->w = cw->w > 0 ? cw->w : nw;
      wg->h = cw->h > 0 ? cw->h : nh;
      c->s->mirror[i].nw = nw;
      c->s->mirror[i].nh = nh;
    }

  s_unlock(c->s);
}

static void client_kit_event(struct kit_s *k, struct kit_widget_s *wg,
                             int ev);
static void client_kit_draw(struct kit_s *k, struct kit_widget_s *wg,
                            int x0, int y0, int x1, int y1);

/* The kit made again (widgets added or removed): all of it drawn */

static void build(struct client_win_s *c)
{
  struct desk_win_s *w = c->w;
  struct kit_s *k = &c->kit;
  int i;

  kit_init(k, w, c->wg, NWG, client_kit_event);
  k->priv = c;
  k->draw = client_kit_draw;
  kit_col(k, KIT_BOX_NONE)->flags |= KIT_F_FREE;
  kit_custom(k, CANVAS_ID)->flags |= KIT_F_NOFOCUS;
  for (i = 1; i <= DESKWIN_WIDGETS; i++)
    {
      struct cwidget_s *cw = &c->cw[i];
      struct kit_widget_s *wg = NULL;
      const char *t = cw->text != NULL ? cw->text : "";

      if (!cw->used)
        {
          continue;
        }

      switch (cw->type)
        {
          case DESKWIN_BUTTON:
            wg = kit_button(k, i, t, KIT_ICON_NONE,
                            cw->style <= KIT_BTN_TOGGLE ? cw->style : 0);
            wg->value = cw->value;
            break;

          case DESKWIN_LABEL:
            wg = kit_label(k, i, t, label_style(cw->style));
            break;

          case DESKWIN_FIELD:
            wg = kit_field(k, i, cw->field, cw->text);
            break;

          case DESKWIN_SLIDER:
            wg = kit_slider(k, i, cw->value, cw->min, cw->max);
            break;

          case DESKWIN_CHECK:
            wg = kit_check(k, i, t, cw->value != 0);
            break;

          case DESKWIN_SWITCH:
            wg = kit_switch(k, i, cw->value != 0);
            break;

          case DESKWIN_PROGRESS:
            wg = kit_progress(k, i, cw->value, cw->max);
            break;

          case DESKWIN_LIST:
            cw->list->n    = cw->nitems;
            cw->list->sel  = cw->value < cw->nitems ? cw->value : -1;
            cw->list->text = list_text;
            wg = kit_list(k, i, cw->list, KIT_LIST_PLAIN);
            break;

          case DESKWIN_DROPDOWN:
            wg = kit_dropdown(k, i, cw->itemv, cw->nitems,
                              cw->value < cw->nitems ? cw->value : 0);
            break;

          default:
            break;
        }

      if (wg != NULL)
        {
          if (!cw->enabled)
            {
              wg->flags |= KIT_F_DISABLED;
            }

          if (!cw->visible)
            {
              wg->flags |= KIT_F_HIDDEN;
            }
        }
    }

  kit_end(k);
  kit_layout(k);
  place(c);
  kit_damage(k, 0, TITLE_H, w->csize.w, w->csize.h);
  c->rebuild = false;
}

static void widget_cmd(struct client_win_s *c, const struct cmd_s *cmd)
{
  struct kit_s *k = &c->kit;
  int id = arg(cmd, 0);
  struct cwidget_s *cw;
  const char *s;

  if (id < 1 || id > DESKWIN_WIDGETS)
    {
      return;
    }

  cw = &c->cw[id];
  switch (cmd->op)
    {
      case CMD_WIDGET:
        {
          int type = arg(cmd, 1);

          if (type < DESKWIN_BUTTON || type > DESKWIN_DROPDOWN)
            {
              return;
            }

          if (cw->used)
            {
              struct kit_widget_s *wg = kit_get(k, id);

              if (wg != NULL)
                {
                  kit_damage(k, wg->x, wg->y, wg->w, wg->h);
                }
            }

          cw_free(cw);
          cw->used    = true;
          cw->enabled = true;
          cw->visible = true;
          cw->type    = type;
          cw->x       = arg(cmd, 2);
          cw->y       = arg(cmd, 3);
          cw->w       = MAX(0, arg(cmd, 4));
          cw->h       = MAX(0, arg(cmd, 5));
          cw->value   = arg(cmd, 6);
          cw->min     = arg(cmd, 7);
          cw->max     = arg(cmd, 8);
          cw->style   = arg(cmd, 9);
          s           = cmd_str(cmd, 0);
          if (type == DESKWIN_FIELD)
            {
              /* A field's text; its placeholder is the "items" */

              cw->field = calloc(1, sizeof(*cw->field));
              cw->text  = strndup(cmd_str(cmd, 1), MAX_TEXT);
              if (cw->field == NULL)
                {
                  cw_free(cw);
                  return;
                }

              ui_field_set(cw->field, s);
            }
          else
            {
              cw->text = strndup(s, MAX_TEXT);
              set_items(cw, cmd_str(cmd, 1));
            }

          if (type == DESKWIN_LIST)
            {
              cw->list = calloc(1, sizeof(*cw->list));
              if (cw->list == NULL)
                {
                  cw_free(cw);
                  return;
                }

              cw->list->sortcol = -1;
            }

          s_lock(c->s);
          c->s->mirror[id].type  = type;
          c->s->mirror[id].value = cw->value;
          mirror_text(c->s, id, type == DESKWIN_FIELD ? cw->field->text :
                                s);
          s_unlock(c->s);
          c->rebuild = true;
        }
        break;

      case CMD_WDEL:
        if (cw->used)
          {
            struct kit_widget_s *wg = kit_get(k, id);

            if (wg != NULL)
              {
                kit_damage(k, wg->x, wg->y, wg->w, wg->h);
              }

            cw_free(cw);
            s_lock(c->s);
            c->s->mirror[id].type = 0;
            mirror_text(c->s, id, NULL);
            s_unlock(c->s);
            c->rebuild = true;
          }
        break;

      case CMD_WMOVE:
        if (cw->used)
          {
            struct kit_widget_s *wg = kit_get(k, id);

            if (wg != NULL)
              {
                kit_damage(k, wg->x, wg->y, wg->w, wg->h);
              }

            cw->x = arg(cmd, 1);
            cw->y = arg(cmd, 2);
            cw->w = MAX(0, arg(cmd, 3));
            cw->h = MAX(0, arg(cmd, 4));
            if (!c->rebuild)
              {
                place(c);
                if (wg != NULL)
                  {
                    kit_damage(k, wg->x, wg->y, wg->w, wg->h);
                  }
              }
          }
        break;

      case CMD_WSET:
        if (!cw->used)
          {
            break;
          }

        s = cmd_str(cmd, 0);
        switch (arg(cmd, 1))
          {
            case DESKWIN_SET_TEXT:
              if (cw->type == DESKWIN_FIELD)
                {
                  ui_field_set(cw->field, s);
                  kit_invalidate(k, id);
                  s_lock(c->s);
                  mirror_text(c->s, id, cw->field->text);
                  s_unlock(c->s);
                }
              else
                {
                  char *old = cw->text;

                  cw->text = strndup(s, MAX_TEXT);
                  if (!c->rebuild)
                    {
                      kit_set_text(k, id, cw->text != NULL ? cw->text : "");
                    }

                  free(old);
                  s_lock(c->s);
                  mirror_text(c->s, id, cw->text);
                  s_unlock(c->s);
                }
              break;

            case DESKWIN_SET_VALUE:
              cw->value = arg(cmd, 2);
              if (!c->rebuild)
                {
                  if (cw->type == DESKWIN_LIST)
                    {
                      kit_list_select(k, id, cw->value < cw->nitems ?
                                      cw->value : -1);
                    }
                  else
                    {
                      kit_set_value(k, id, cw->value);
                    }
                }

              s_lock(c->s);
              c->s->mirror[id].value = cw->value;
              s_unlock(c->s);
              break;

            case DESKWIN_SET_ITEMS:
              if (cw->type == DESKWIN_LIST || cw->type == DESKWIN_DROPDOWN)
                {
                  set_items(cw, s);
                  c->rebuild = true;
                }
              break;

            case DESKWIN_SET_ENABLED:
              cw->enabled = arg(cmd, 2) != 0;
              if (!c->rebuild)
                {
                  kit_enable(k, id, cw->enabled);
                }
              break;

            case DESKWIN_SET_VISIBLE:
              cw->visible = arg(cmd, 2) != 0;
              if (!c->rebuild)
                {
                  struct kit_widget_s *wg = kit_get(k, id);

                  kit_show(k, id, cw->visible);
                  if (wg != NULL)
                    {
                      kit_damage(k, wg->x, wg->y, wg->w, wg->h);
                    }
                }
              break;

            case DESKWIN_SET_RANGE:
              {
                struct kit_widget_s *wg = kit_get(k, id);

                sscanf(s, "%" SCNd32 " %" SCNd32, &cw->min, &cw->max);
                if (wg != NULL && !c->rebuild)
                  {
                    wg->min = cw->min;
                    wg->max = cw->max;
                    kit_invalidate(k, id);
                  }
              }
              break;

            case DESKWIN_SET_FOCUS:
              if (!c->rebuild)
                {
                  kit_focus(k, id);
                }
              break;

            default:
              break;
          }
        break;

      default:
        break;
    }
}

/* The kit's events: the program's */

static void client_kit_event(struct kit_s *k, struct kit_widget_s *wg,
                             int ev)
{
  struct client_win_s *c = k->priv;
  struct deskwin_s *s = c->s;
  struct deskwin_event_s e;
  struct cwidget_s *cw;
  int id;

  memset(&e, 0, sizeof(e));
  if (ev == KIT_EV_KEY)
    {
      if (c->havekey)
        {
          e.type = DESKWIN_EV_KEY;
          e.key  = c->key.code;
          e.ch   = c->key.ch;
          e.mods = c->key.flags & NX_KEY_MODS;
          e.kind = c->key.flags & NX_KEY_REPEAT ? DESKWIN_KEY_REPEAT : 0;
        }
      else
        {
          /* Bytes (deskctl k): the kit's key, as a key event */

          static const uint8_t special[] =
          {
            KEYCODE_UP, KEYCODE_DOWN, KEYCODE_RIGHT, KEYCODE_LEFT,
            KEYCODE_HOME, KEYCODE_END, KEYCODE_PAGEUP, KEYCODE_PAGEDOWN,
            KEYCODE_INSERT, KEYCODE_FWDDEL, 0, KEYCODE_F2, KEYCODE_F5
          };

          int key = k->key & KEY_MASK;

          e.type = DESKWIN_EV_KEY;
          if (key == KEY_ESC)
            {
              e.key = NX_KEY_ESCAPE;
              e.ch  = 0x1b;
            }
          else if (key >= KEY_UP && key <= KEY_F5)
            {
              e.key = NX_KEY_SPECIAL(special[key - KEY_UP]);
            }
          else if (key < 0x100)
            {
              e.ch  = key;
              e.key = nx_key_code(key, false);
            }

          e.mods = (k->key & KEY_SHIFT ? NX_KEY_SHIFT : 0) |
                   (k->key & KEY_CTRL ? NX_KEY_CTRL : 0);
        }

      push(s, &e);
      k->key = 0;
      return;
    }

  if (wg == NULL)
    {
      return;
    }

  id = wg->id;
  if (id == CANVAS_ID)
    {
      if (ev == KIT_EV_MOUSE)
        {
          e.type    = DESKWIN_EV_MOUSE;
          e.x       = k->mx;
          e.y       = k->my - TITLE_H;
          e.buttons = k->mbuttons;
          push(s, &e);
        }

      return;
    }

  if (id < 1 || id > DESKWIN_WIDGETS || !c->cw[id].used)
    {
      return;
    }

  cw = &c->cw[id];
  switch (ev)
    {
      case KIT_EV_CLICK:
        e.kind = DESKWIN_CLICK;
        break;

      case KIT_EV_CHANGE:
        e.kind = DESKWIN_CHANGE;
        break;

      case KIT_EV_ACTIVATE:
        e.kind = DESKWIN_ACTIVATE;
        break;

      default:
        return;
    }

  cw->value = cw->type == DESKWIN_LIST ? cw->list->sel : wg->value;
  e.type  = DESKWIN_EV_WIDGET;
  e.id    = id;
  e.value = cw->value;

  s_lock(s);
  s->mirror[id].value = cw->value;
  if (cw->type == DESKWIN_FIELD)
    {
      mirror_text(s, id, cw->field->text);
    }

  s_push(s, &e);
  s_unlock(s);
}

/* The kit draws the canvas: what is kept, again */

static void client_kit_draw(struct kit_s *k, struct kit_widget_s *wg,
                            int x0, int y0, int x1, int y1)
{
  struct client_win_s *c = k->priv;
  struct kit_rect_s r = rect(x0, y0, x1 - x0, y1 - y0);

  if (wg->id == CANVAS_ID && r.w > 0 && r.h > 0)
    {
      replay(c, &r);
    }
}

/****************************************************************************
 * Private Functions: carrying out the program's commands
 ****************************************************************************/

static void image_request(struct client_win_s *c)
{
  struct deskwin_s *s = c->s;
  char path[128];
  char err[80];
  int maxw;
  int maxh;
  int i;
  struct desk_image_s *img = NULL;

  s_lock(s);
  if (!s->imgreq)
    {
      s_unlock(s);
      return;
    }

  strlcpy(path, s->imgpath, sizeof(path));
  maxw = s->imgmaxw > 0 ? s->imgmaxw : IMG_SPACE;
  maxh = s->imgmaxh > 0 ? s->imgmaxh : IMG_SPACE;
  s_unlock(s);

  err[0] = '\0';
  for (i = 0; i < DESKWIN_IMAGES && c->img[i] != NULL; i++)
    {
    }

  if (i < DESKWIN_IMAGES)
    {
      img = img_load(path, maxw, maxh, c->w, err, sizeof(err));
    }
  else
    {
      strlcpy(err, "Too many images", sizeof(err));
    }

  if (img != NULL)
    {
      c->img[i]     = img;
      c->imgpath[i] = strdup(path);
      c->imgmaxw[i] = maxw;
      c->imgmaxh[i] = maxh;
    }

  s_lock(s);
  s->imgreq = false;
  s->imgret = img != NULL ? i : i < DESKWIN_IMAGES ? -EINVAL : -ENOSPC;
  s->imgw   = img != NULL ? img->w : 0;
  s->imgh   = img != NULL ? img->h : 0;
  strlcpy(s->imgerr, err, sizeof(s->imgerr));
  sem_post(&s->reply);
  s_unlock(s);
}

/* A surface for the program (deskwin_surface_open()) */

static void surface_request(struct client_win_s *c)
{
  struct deskwin_s *s = c->s;
  NXWINDOW hwnd = NULL;
  int w;
  int h;
  int i;

  s_lock(s);
  if (!s->surfreq)
    {
      s_unlock(s);
      return;
    }

  w = s->surfw;
  h = s->surfh;
  s_unlock(s);

  for (i = 0; i < DESKWIN_SURFACES && c->surf[i] != NULL; i++)
    {
    }

  if (i < DESKWIN_SURFACES && w > 0 && h > 0)
    {
      c->surflost[i] = false;
      hwnd = desk_surface_open(w, h, &c->surflost[i]);
    }

  s_lock(s);
  if (hwnd != NULL)
    {
      c->surf[i]       = hwnd;
      c->surfclient[i] = s->surflost;
    }

  s->surfreq = false;
  s->surfret = hwnd;
  sem_post(&s->reply);
  s_unlock(s);
}

static void surface_free(struct client_win_s *c, int i)
{
  if (i >= 0 && i < DESKWIN_SURFACES && c->surf[i] != NULL)
    {
      desk_surface_close(c->surf[i]);
      c->surf[i]       = NULL;
      c->surfclient[i] = NULL;
    }
}

/* A surface that lost its pixels: the program's flag set (its own, in the
 * same memory: it draws the surface again when it sees it)
 */

static void surfaces_lost(struct client_win_s *c)
{
  int i;

  for (i = 0; i < DESKWIN_SURFACES; i++)
    {
      if (c->surf[i] != NULL && c->surflost[i])
        {
          c->surflost[i] = false;
          s_lock(c->s);
          if (!c->s->clientgone && c->surfclient[i] != NULL)
            {
              *c->surfclient[i] = true;
            }

          s_unlock(c->s);
        }
    }
}

static void image_free(struct client_win_s *c, int i)
{
  if (i >= 0 && i < DESKWIN_IMAGES && c->img[i] != NULL)
    {
      img_free(c->img[i]);
      free(c->imgpath[i]);
      c->img[i]     = NULL;
      c->imgpath[i] = NULL;
    }
}

/* Images whose pixels the server took away: loaded again */

static void images_lost(struct client_win_s *c)
{
  bool any = false;
  int i;

  for (i = 0; i < DESKWIN_IMAGES; i++)
    {
      if (c->img[i] != NULL && c->img[i]->lost)
        {
          char err[80];
          struct desk_image_s *img = img_load(c->imgpath[i], c->imgmaxw[i],
                                              c->imgmaxh[i], c->w, err,
                                              sizeof(err));

          img_free(c->img[i]);
          c->img[i] = img;
          any = true;
        }
    }

  if (any)
    {
      kit_damage(&c->kit, 0, TITLE_H, c->w->csize.w,
                 c->w->csize.h);
    }
}

static void run_cmds(struct client_win_s *c, const uint8_t *buf, size_t n)
{
  struct desk_win_s *w = c->w;
  struct kit_rect_s piece[MAX_PIECES];
  struct emit_s e;
  size_t off = 0;
  bool placed = true;

  e.hwnd    = (NXWINDOW)w->hwnd;
  e.pieces  = piece;
  e.npieces = where(c, NULL, piece);

  while (off + CMD_HDR <= n)
    {
      const struct cmd_s *cmd = (const struct cmd_s *)(buf + off);

      if (cmd->len < CMD_HDR || cmd->len > n - off ||
          cmd->nargs > CMD_MAXARGS || cmd->nargs * 4 + CMD_HDR > cmd->len)
        {
          syslog(LOG_WARNING, "desk: client %d: a bad command\n",
                 (int)c->s->pid);
          break;
        }

      switch (cmd->op)
        {
          case CMD_FILL:
          case CMD_RECT:
          case CMD_GRAD:
          case CMD_LINE:
          case CMD_TEXT:
          case CMD_IMAGE:
          case CMD_COPY:
          case CMD_CLEAR:
          case CMD_OPS:
            if (!placed)
              {
                /* Widgets changed: they are where they are now */

                if (c->rebuild)
                  {
                    build(c);
                  }

                kit_flush(&c->kit);
                e.npieces = where(c, NULL, piece);
                placed = true;
              }

            draw_cmd(c, &e, cmd);
            keep(c, cmd);
            break;

          case CMD_CLIP:
            ops_draw(e.hwnd, g_out);
            c->clip = arg(cmd, 2) > 0 ?
                      rect(arg(cmd, 0), arg(cmd, 1) + TITLE_H, arg(cmd, 2),
                           arg(cmd, 3)) : rect(0, 0, 0, 0);
            e.npieces = where(c, NULL, piece);
            keep(c, cmd);
            break;

          case CMD_TITLE:
            snprintf(w->title, sizeof(w->title), "%s", cmd_str(cmd, 0));
            desk_win_title(w);
            break;

          case CMD_FRAMES:
            w->frames = arg(cmd, 0) != 0;
            break;

          case CMD_RAISE:
            ops_draw(e.hwnd, g_out);
            desk_win_restore(w);
            break;

          case CMD_BUTTON:
            strlcpy(c->button, cmd_str(cmd, 0), sizeof(c->button));
            w->button = c->button[0] != '\0' ? c->button : NULL;
            desk_win_title(w);
            break;

          case CMD_WIDGET:
          case CMD_WSET:
          case CMD_WMOVE:
          case CMD_WDEL:
            ops_draw(e.hwnd, g_out);
            widget_cmd(c, cmd);
            placed = false;
            break;

          case CMD_IMGFREE:
            ops_draw(e.hwnd, g_out);
            image_free(c, arg(cmd, 0));
            break;

          case CMD_SURFFREE:
            ops_draw(e.hwnd, g_out);
            surface_free(c, client_surface(c, (uint32_t)arg(cmd, 0)));
            break;

          case CMD_CLOSE:
            ops_draw(e.hwnd, g_out);
            desk_win_close(w);
            return;

          default:
            break;
        }

      off += cmd->len;
    }

  ops_draw(e.hwnd, g_out);
  if (c->rebuild)
    {
      build(c);
    }

  kit_flush(&c->kit);
}

/****************************************************************************
 * Private Functions: the window's callbacks (the main thread)
 ****************************************************************************/

static void client_wake(struct desk_win_s *w)
{
  struct client_win_s *c = w->priv;
  struct deskwin_s *s = c->s;
  uint8_t *t;
  size_t n;
  uint32_t seq;

  images_lost(c);
  image_request(c);
  surfaces_lost(c);
  surface_request(c);

  s_lock(s);
  if (s->clientgone && s->npend == 0)
    {
      s_unlock(s);
      desk_win_close(w);
      return;
    }

  /* Take what is pending: the client may fill it again meanwhile */

  t        = s->pend;
  n        = s->npend;
  seq      = s->seq;
  s->pend  = c->take;
  s->npend = 0;
  c->take  = t;
  sem_post(&s->space);
  s_unlock(s);

  if (n > 0 && t != NULL)
    {
      run_cmds(c, t, n);
    }

  if (w->closing)
    {
      return;
    }

  s_lock(s);
  s->done = seq;
  if (s->waitsync)
    {
      s->waitsync = false;
      sem_post(&s->reply);
    }

  s_unlock(s);
}

static void client_mouse(struct desk_win_s *w, int x, int y,
                         uint8_t buttons)
{
  struct client_win_s *c = w->priv;

  if (x < 0)
    {
      struct deskwin_event_s ev;

      memset(&ev, 0, sizeof(ev));
      ev.type = DESKWIN_EV_MOUSE;
      ev.x    = -1;
      ev.y    = -1;
      push(c->s, &ev);
    }

  kit_mouse(&c->kit, x, y, buttons);
}

static void client_keyev(struct desk_win_s *w, const struct nx_key_s *key)
{
  struct client_win_s *c = w->priv;
  uint8_t bytes[NX_KEY_MAXBYTES];
  int n;

  if (key->flags & NX_KEY_RELEASE)
    {
      struct deskwin_event_s ev;

      memset(&ev, 0, sizeof(ev));
      ev.type = DESKWIN_EV_KEYUP;
      ev.key  = key->code;
      ev.mods = key->flags & NX_KEY_MODS;
      push(c->s, &ev);
      return;
    }

  /* A press: to the kit (a field, a list), and what it does not take to
   * the program
   */

  n = nx_key_bytes(key, bytes);
  c->key     = *key;
  c->havekey = true;
  if (n > 0)
    {
      kit_key(&c->kit, bytes, n);
    }
  else
    {
      struct deskwin_event_s ev;

      memset(&ev, 0, sizeof(ev));
      ev.type = DESKWIN_EV_KEY;
      ev.key  = key->code;
      ev.mods = key->flags & NX_KEY_MODS;
      push(c->s, &ev);
    }

  c->havekey = false;
}

static void client_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct client_win_s *c = w->priv;

  c->havekey = false;
  kit_key(&c->kit, ch, n);
}

static void client_expose(struct desk_win_s *w, int x, int y, int cw,
                          int ch)
{
  struct client_win_s *c = w->priv;

  kit_expose(&c->kit, x, y, cw, ch);
}

static void client_resize(struct desk_win_s *w)
{
  struct client_win_s *c = w->priv;
  struct deskwin_event_s ev;

  kit_layout(&c->kit);
  place(c);
  kit_resize(&c->kit);

  memset(&ev, 0, sizeof(ev));
  ev.type = DESKWIN_EV_RESIZE;
  ev.w    = w->csize.w;
  ev.h    = w->csize.h;
  s_lock(c->s);
  c->s->cw = w->csize.w;
  c->s->ch = w->csize.h;
  s_push(c->s, &ev);
  s_unlock(c->s);
}

static void client_tick(struct desk_win_s *w)
{
  struct client_win_s *c = w->priv;
  struct deskwin_s *s = c->s;

  kit_tick(&c->kit);
  s_lock(s);
  if (!s->tickq)
    {
      struct deskwin_event_s ev;

      memset(&ev, 0, sizeof(ev));
      ev.type  = DESKWIN_EV_TICK;
      s->tickq = true;
      s_push(s, &ev);
    }

  s_unlock(s);
}

static void client_frame(struct desk_win_s *w)
{
  struct client_win_s *c = w->priv;
  struct deskwin_s *s = c->s;

  s_lock(s);
  if (!s->frameq)
    {
      struct deskwin_event_s ev;

      memset(&ev, 0, sizeof(ev));
      ev.type   = DESKWIN_EV_FRAME;
      ev.value  = (int32_t)desk_ms();
      s->frameq = true;
      s_push(s, &ev);
    }

  s_unlock(s);
}

static void client_button(struct desk_win_s *w)
{
  struct client_win_s *c = w->priv;

  push_simple(c->s, DESKWIN_EV_BUTTON, 0);
}

static void client_focus(struct desk_win_s *w)
{
  struct client_win_s *c = w->priv;

  kit_activate(&c->kit);
  push_simple(c->s, DESKWIN_EV_FOCUS, w->active);
}

static void client_close(struct desk_win_s *w)
{
  struct client_win_s *c = w->priv;
  struct desk_win_s *o;
  struct deskwin_s *s;
  int i;

  if (c == NULL)
    {
      return;
    }

  s = c->s;
  for (i = 0; i < DESKWIN_IMAGES; i++)
    {
      image_free(c, i);
    }

  for (i = 0; i <= DESKWIN_WIDGETS; i++)
    {
      cw_free(&c->cw[i]);
    }

  for (i = 0; i < DESKWIN_SURFACES; i++)
    {
      surface_free(c, i);
    }

  free(c->keep);
  free(c->take);
  c->keep = NULL;
  c->take = NULL;

  /* The client learns that it has gone; a dead one lets go too */

  s_lock(s);
  s->win  = NULL;
  s->gone = true;
  if (!s->clientgone && kill(s->pid, 0) < 0)
    {
      s->clientgone = true;
    }

  if (!s->clientgone)
    {
      struct deskwin_event_s ev;

      memset(&ev, 0, sizeof(ev));
      ev.type = DESKWIN_EV_CLOSE;
      s_push(s, &ev);
    }

  sem_post(&s->space);
  sem_post(&s->reply);
  sem_post(&s->evsem);
  if (s->clientgone && s->refs > 1)
    {
      s->refs--;                           /* The client's */
    }

  s_unref(s);

  /* The last program's window gone: the drawing buffers too (18 KB; made
   * again by the next one, client_open())
   */

  for (o = g_desk.wins; o != NULL; o = o->next)
    {
      if (o != w && o->client && !o->closedone)
        {
          return;
        }
    }

  free(g_tmp);
  free(g_out);
  g_tmp = NULL;
  g_out = NULL;
}

/* A new window for a session (EV_CLIENT) */

static void client_open(struct deskwin_s *s)
{
  struct client_win_s *c;
  struct desk_win_s *w = NULL;

  if (g_tmp == NULL)
    {
      g_tmp = malloc(sizeof(*g_tmp));
      g_out = malloc(sizeof(*g_out));
      if (g_tmp == NULL || g_out == NULL)
        {
          free(g_tmp);
          free(g_out);
          g_tmp = NULL;
          g_out = NULL;
        }
      else
        {
          g_tmp->n = 0;
          g_out->n = 0;
        }
    }

  c = g_tmp != NULL ? calloc(1, sizeof(*c)) : NULL;
  if (c != NULL)
    {
      c->take = malloc(PENDING);
      if (c->take != NULL)
        {
          w = desk_win_open(s->app > 0 && s->app < APP_COUNT ?
                            s->app : APP_PYTHON, s->title,
                            MAX(s->reqw, MIN_W), MAX(s->reqh, MIN_H));
        }
    }

  s_lock(s);
  if (w == NULL || s->clientgone)
    {
      if (c != NULL)
        {
          free(c->take);
          free(c);
        }

      if (w != NULL)
        {
          desk_win_close(w);
        }

      s->opened = true;
      sem_post(&s->reply);
      s_unref(s);                          /* The desktop's */
      return;
    }

  c->kind   = PYTHON_CLIENT;
  c->s      = s;
  c->w      = w;
  c->keepok = true;
  w->priv   = c;
  w->pid    = s->pid;
  w->client = true;
  s->win    = w;
  s->cw     = w->csize.w;
  s->ch     = w->csize.h;
  s->opened = true;
  s_unlock(s);

  desk_win_sync(w);
  build(c);
  kit_flush(&c->kit);
  desk_win_show(w);

  s_lock(s);
  sem_post(&s->reply);
  s_unlock(s);
}

/****************************************************************************
 * Private Functions: the client's side
 ****************************************************************************/

static int timed_wait(sem_t *sem, int timeout)
{
  struct timespec ts;
  int ret;

  if (timeout < 0)
    {
      while ((ret = sem_wait(sem)) < 0 && errno == EINTR)
        {
        }

      return ret;
    }

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec  += timeout / 1000;
  ts.tv_nsec += (long)(timeout % 1000) * 1000000;
  if (ts.tv_nsec >= 1000000000)
    {
      ts.tv_sec++;
      ts.tv_nsec -= 1000000000;
    }

  while ((ret = sem_timedwait(sem, &ts)) < 0 && errno == EINTR)
    {
    }

  return ret;
}

/* A command into the client's batch (handed over when full) */

static void put(struct deskwin_s *d, int op, const int32_t *args, int nargs,
                const char *s0, const char *s1)
{
  size_t l0 = s0 != NULL ? strnlen(s0, MAX_ITEMS) + 1 : 0;
  size_t l1 = s1 != NULL ? strnlen(s1, MAX_ITEMS) + 1 : 0;
  size_t len = (CMD_HDR + nargs * 4 + l0 + l1 + 3) & ~(size_t)3;
  struct cmd_s *cmd;
  char *p;

  if (d == NULL || len > BATCH)
    {
      return;
    }

  if (d->nbatch + len > BATCH)
    {
      deskwin_flush(d, -1);
    }

  cmd = (struct cmd_s *)(d->batch + d->nbatch);
  cmd->op    = op;
  cmd->nargs = nargs;
  cmd->len   = len;
  memcpy(cmd + 1, args, nargs * 4);
  p = (char *)(cmd + 1) + nargs * 4;
  if (s0 != NULL)
    {
      memcpy(p, s0, l0 - 1);
      p[l0 - 1] = '\0';
      p += l0;
    }

  if (s1 != NULL)
    {
      memcpy(p, s1, l1 - 1);
      p[l1 - 1] = '\0';
      p += l1;
    }

  memset(p, 0, (char *)cmd + len - p);
  d->nbatch += len;
}

/* Sessions whose window has gone and whose client died without letting
 * go (deleted): freed
 */

static void reap(void)
{
  for (; ; )
    {
      struct deskwin_s *s;

      pthread_mutex_lock(&g_slock);
      for (s = g_sessions; s != NULL; s = s->next)
        {
          if (s->gone && !s->clientgone && s->pid != getpid() &&
              kill(s->pid, 0) < 0)
            {
              break;
            }
        }

      pthread_mutex_unlock(&g_slock);
      if (s == NULL)
        {
          return;
        }

      s_lock(s);
      s->clientgone = true;
      s_unref(s);
    }
}

/* Hand over what is queued and wait until the desktop has done it */

static int sync_wait(struct deskwin_s *d)
{
  uint32_t seq;
  int ret = deskwin_flush(d, -1);

  if (ret < 0)
    {
      return ret;
    }

  s_lock(d);
  seq = d->seq;
  while ((int32_t)(d->done - seq) < 0 && !d->gone)
    {
      d->waitsync = true;
      if (d->win != NULL)
        {
          desk_wake(d->win);
        }

      s_unlock(d);
      timed_wait(&d->reply, 1000);
      s_lock(d);
    }

  ret = d->gone ? -EPIPE : 0;
  s_unlock(d);
  return ret;
}

/****************************************************************************
 * Public Functions: the desktop's
 ****************************************************************************/

/* The desktop is up: clients may come */

void desk_clients_init(void)
{
  g_ready = true;
}

/* EV_CLIENT: sessions waiting for a window */

void desk_clients_poll(void)
{
  for (; ; )
    {
      struct deskwin_s *s;

      pthread_mutex_lock(&g_slock);
      for (s = g_sessions; s != NULL; s = s->next)
        {
          if (!s->opened && !s->gone)
            {
              break;
            }
        }

      pthread_mutex_unlock(&g_slock);
      if (s == NULL)
        {
          return;
        }

      client_open(s);
    }
}

const struct desk_app_s g_app_client =
{
  .name   = "Python",
  .expose = client_expose,
  .resize = client_resize,
  .tick   = client_tick,
  .mouse  = client_mouse,
  .key    = client_key,
  .keyev  = client_keyev,
  .close  = client_close,
  .wake   = client_wake,
  .focus  = client_focus,
  .button = client_button,
  .frame  = client_frame,
};

/****************************************************************************
 * Public Functions: the client's (deskwin.h)
 ****************************************************************************/

FAR struct deskwin_s *deskwin_open(FAR const char *title, int w, int h)
{
  return deskwin_open_app(-1, title, w, h);
}

FAR struct deskwin_s *deskwin_open_app(int app, FAR const char *title,
                                       int w, int h)
{
  pthread_mutexattr_t attr;
  struct deskwin_s *s;
  int i;

  /* A desktop still starting: a moment */

  for (i = 0; i < 50 && !g_ready && g_desk.hnx != NULL; i++)
    {
      usleep(100 * 1000);
    }

  if (!g_ready || g_desk.hnx == NULL)
    {
      errno = ENODEV;
      return NULL;
    }

  reap();

  s = calloc(1, sizeof(*s));
  if (s == NULL)
    {
      errno = ENOMEM;
      return NULL;
    }

  s->pend = malloc(PENDING);
  if (s->pend == NULL)
    {
      free(s);
      errno = ENOMEM;
      return NULL;
    }

  s->magic = SESSION_MAGIC;
  s->refs  = 2;                        /* The client, the desktop */
  s->pid   = getpid();
  s->app   = app;
  s->reqw  = MIN(MAX(w, 1), 4096);
  s->reqh  = MIN(MAX(h, 1), 4096);
  strlcpy(s->title, title != NULL ? title : "Python", sizeof(s->title));
  pthread_mutexattr_init(&attr);
  pthread_mutexattr_setrobust(&attr, PTHREAD_MUTEX_ROBUST);
  pthread_mutex_init(&s->lock, &attr);
  pthread_mutexattr_destroy(&attr);
  sem_init(&s->evsem, 0, 0);
  sem_init(&s->space, 0, 0);
  sem_init(&s->reply, 0, 0);

  pthread_mutex_lock(&g_slock);
  s->next = g_sessions;
  g_sessions = s;
  pthread_mutex_unlock(&g_slock);

  desk_post(EV_CLIENT);

  s_lock(s);
  while (!s->opened)
    {
      s_unlock(s);
      if (timed_wait(&s->reply, OPEN_MS) < 0)
        {
          s_lock(s);
          if (!s->opened)
            {
              /* No answer: the desktop closes it when it comes to it */

              s->clientgone = true;
              s_unref(s);
              errno = ETIMEDOUT;
              return NULL;
            }

          break;
        }

      s_lock(s);
    }

  if (s->win == NULL)
    {
      s->clientgone = true;
      s_unref(s);
      errno = ENOMEM;
      return NULL;
    }

  s_unlock(s);
  return s;
}

void deskwin_close(FAR struct deskwin_s *d)
{
  if (d == NULL || d->magic != SESSION_MAGIC)
    {
      return;
    }

  d->nbatch = 0;
  s_lock(d);
  d->clientgone = true;
  d->qn = 0;
  if (d->win != NULL)
    {
      desk_wake(d->win);                  /* It closes */
    }

  s_unref(d);
}

bool deskwin_alive(FAR struct deskwin_s *d)
{
  bool alive;

  s_lock(d);
  alive = !d->gone;
  s_unlock(d);
  return alive;
}

void deskwin_size(FAR struct deskwin_s *d, FAR int *w, FAR int *h)
{
  s_lock(d);
  *w = d->cw;
  *h = d->ch;
  s_unlock(d);
}

int deskwin_event(FAR struct deskwin_s *d, FAR struct deskwin_event_s *ev,
                  int timeout)
{
  for (; ; )
    {
      s_lock(d);
      if (d->wakeq)
        {
          d->wakeq = false;
          s_unlock(d);
          memset(ev, 0, sizeof(*ev));
          ev->type = DESKWIN_EV_WAKE;
          return 1;
        }

      if (d->qn > 0)
        {
          *ev = d->q[d->qhead];
          d->qhead = (d->qhead + 1) % QLEN;
          d->qn--;
          if (ev->type == DESKWIN_EV_FRAME)
            {
              d->frameq = false;
            }
          else if (ev->type == DESKWIN_EV_TICK)
            {
              d->tickq = false;
            }

          s_unlock(d);
          return 1;
        }

      if (d->gone)
        {
          s_unlock(d);
          return -EPIPE;
        }

      s_unlock(d);
      if (timeout == 0 || timed_wait(&d->evsem, timeout) < 0)
        {
          return 0;
        }
    }
}

/* Its own thread or another of the client's: deskwin_event() returns
 * DESKWIN_EV_WAKE (once for any number before it)
 */

void deskwin_wake(FAR struct deskwin_s *d)
{
  bool post;

  s_lock(d);
  post     = !d->wakeq;
  d->wakeq = true;
  s_unlock(d);
  if (post)
    {
      sem_post(&d->evsem);
    }
}

int deskwin_flush(FAR struct deskwin_s *d, int timeout)
{
  if (d->nbatch == 0)
    {
      return deskwin_alive(d) ? 0 : -EPIPE;
    }

  s_lock(d);
  while (!d->gone && d->npend + d->nbatch > PENDING)
    {
      if (d->win != NULL)
        {
          desk_wake(d->win);
        }

      s_unlock(d);
      if (timed_wait(&d->space, timeout) < 0 && timeout >= 0)
        {
          return -ETIMEDOUT;
        }

      s_lock(d);
    }

  if (d->gone)
    {
      d->nbatch = 0;
      s_unlock(d);
      return -EPIPE;
    }

  memcpy(d->pend + d->npend, d->batch, d->nbatch);
  d->npend += d->nbatch;
  d->nbatch = 0;
  d->seq++;
  if (d->win != NULL)
    {
      desk_wake(d->win);
    }

  s_unlock(d);
  return 0;
}

/* A surface of the desktop's for the program: hidden, w x h, drawn into
 * with deskwin_ops_to() and copied from by its operations (its pointer is
 * their source); *lost is set when it has lost its pixels.  Waits.
 */

FAR void *deskwin_surface_open(FAR struct deskwin_s *d, int w, int h,
                               FAR volatile bool *lost)
{
  NXWINDOW hwnd;

  if (deskwin_flush(d, -1) < 0)
    {
      return NULL;
    }

  s_lock(d);
  d->surfw    = MIN(MAX(w, 1), 2048);
  d->surfh    = MIN(MAX(h, 1), 2048);
  d->surflost = lost;
  d->surfret  = NULL;
  d->surfreq  = true;
  while (d->surfreq && !d->gone)
    {
      if (d->win != NULL)
        {
          desk_wake(d->win);
        }

      s_unlock(d);
      timed_wait(&d->reply, 1000);
      s_lock(d);
    }

  hwnd = d->gone ? NULL : d->surfret;
  s_unlock(d);
  return hwnd;
}

void deskwin_surface_close(FAR struct deskwin_s *d, FAR void *surface)
{
  int32_t a[1] =
    {
      (int32_t)(uintptr_t)surface
    };

  put(d, CMD_SURFFREE, a, 1, NULL, NULL);
}

/* The desktop's own drawing operations (struct nx_gpuop_s, frame
 * coordinates), as many commands as they take: into the window, or one of
 * the program's surfaces
 */

void deskwin_ops(FAR struct deskwin_s *d, FAR const void *ops, int n)
{
  deskwin_ops_to(d, NULL, ops, n);
}

void deskwin_ops_to(FAR struct deskwin_s *d, FAR void *target,
                    FAR const void *ops, int n)
{
  const struct nx_gpuop_s *op = ops;
  const int per = (BATCH - CMD_HDR - 8) / sizeof(*op);

  while (d != NULL && n > 0)
    {
      int k = n < per ? n : per;
      size_t len = (CMD_HDR + 8 + k * sizeof(*op) + 3) & ~(size_t)3;
      struct cmd_s *cmd;

      if (d->nbatch + len > BATCH)
        {
          deskwin_flush(d, -1);
        }

      cmd = (struct cmd_s *)(d->batch + d->nbatch);
      cmd->op    = CMD_OPS;
      cmd->nargs = 2;
      cmd->len   = len;
      ((int32_t *)(cmd + 1))[0] = k;
      ((int32_t *)(cmd + 1))[1] = (int32_t)(uintptr_t)target;
      memcpy((int32_t *)(cmd + 1) + 2, op, k * sizeof(*op));
      d->nbatch += len;
      op += k;
      n  -= k;
    }
}

void deskwin_fill(FAR struct deskwin_s *d, int x, int y, int w, int h,
                  uint16_t c)
{
  int32_t a[5] =
    {
      x, y, w, h, c
    };

  put(d, CMD_FILL, a, 5, NULL, NULL);
}

void deskwin_rect(FAR struct deskwin_s *d, int x, int y, int w, int h,
                  uint16_t c, int alpha, int radius)
{
  int32_t a[7] =
    {
      x, y, w, h, c, alpha, radius
    };

  put(d, CMD_RECT, a, 7, NULL, NULL);
}

void deskwin_gradient(FAR struct deskwin_s *d, int x, int y, int w, int h,
                      uint16_t c0, uint16_t c1, int alpha, bool vertical)
{
  int32_t a[8] =
    {
      x, y, w, h, c0, c1, alpha, vertical
    };

  put(d, CMD_GRAD, a, 8, NULL, NULL);
}

void deskwin_line(FAR struct deskwin_s *d, int x0, int y0, int x1, int y1,
                  uint16_t c, int alpha)
{
  int32_t a[6] =
    {
      x0, y0, x1, y1, c, alpha
    };

  put(d, CMD_LINE, a, 6, NULL, NULL);
}

void deskwin_text(FAR struct deskwin_s *d, int x, int y, int font,
                  uint16_t c, int alpha, FAR const char *s)
{
  int32_t a[5] =
    {
      x, y, font, c, alpha
    };

  put(d, CMD_TEXT, a, 5, s, NULL);
}

void deskwin_image(FAR struct deskwin_s *d, int img, int sx, int sy,
                   int w, int h, int dx, int dy, int alpha)
{
  int32_t a[8] =
    {
      img, sx, sy, w, h, dx, dy, alpha
    };

  put(d, CMD_IMAGE, a, 8, NULL, NULL);
}

void deskwin_copy(FAR struct deskwin_s *d, int sx, int sy, int w, int h,
                  int dx, int dy)
{
  int32_t a[6] =
    {
      sx, sy, w, h, dx, dy
    };

  put(d, CMD_COPY, a, 6, NULL, NULL);
}

void deskwin_clip(FAR struct deskwin_s *d, int x, int y, int w, int h)
{
  int32_t a[4] =
    {
      x, y, w, h
    };

  put(d, CMD_CLIP, a, 4, NULL, NULL);
}

void deskwin_clear(FAR struct deskwin_s *d, uint16_t c)
{
  int32_t a[1] =
    {
      c
    };

  put(d, CMD_CLEAR, a, 1, NULL, NULL);
}

void deskwin_title(FAR struct deskwin_s *d, FAR const char *title)
{
  put(d, CMD_TITLE, NULL, 0, title, NULL);
}

void deskwin_button(FAR struct deskwin_s *d, FAR const char *label)
{
  put(d, CMD_BUTTON, NULL, 0, label != NULL ? label : "", NULL);
}

void deskwin_raise(FAR struct deskwin_s *d)
{
  put(d, CMD_RAISE, NULL, 0, NULL, NULL);
}

void deskwin_frames(FAR struct deskwin_s *d, bool on)
{
  int32_t a[1] =
    {
      on
    };

  put(d, CMD_FRAMES, a, 1, NULL, NULL);
}

int deskwin_image_load(FAR struct deskwin_s *d, FAR const char *path,
                       int maxw, int maxh, FAR int *w, FAR int *h,
                       FAR char *err, int errlen)
{
  int ret = deskwin_flush(d, -1);

  if (ret < 0)
    {
      return ret;
    }

  s_lock(d);
  strlcpy(d->imgpath, path, sizeof(d->imgpath));
  d->imgmaxw = MIN(MAX(maxw, 0), IMG_SPACE);
  d->imgmaxh = MIN(MAX(maxh, 0), IMG_SPACE);
  d->imgreq  = true;
  while (d->imgreq && !d->gone)
    {
      if (d->win != NULL)
        {
          desk_wake(d->win);
        }

      s_unlock(d);
      timed_wait(&d->reply, 1000);
      s_lock(d);
    }

  ret = d->gone ? -EPIPE : d->imgret;
  if (ret >= 0)
    {
      *w = d->imgw;
      *h = d->imgh;
    }

  if (err != NULL && errlen > 0)
    {
      strlcpy(err, d->gone ? "The window has gone" : d->imgerr, errlen);
    }

  s_unlock(d);
  return ret;
}

void deskwin_image_free(FAR struct deskwin_s *d, int img)
{
  int32_t a[1] =
    {
      img
    };

  put(d, CMD_IMGFREE, a, 1, NULL, NULL);
}

void deskwin_widget(FAR struct deskwin_s *d, int id, int type, int x, int y,
                    int w, int h, FAR const char *text,
                    FAR const char *items, int value, int min, int max,
                    int style)
{
  int32_t a[10] =
    {
      id, type, x, y, w, h, value, min, max, style
    };

  put(d, CMD_WIDGET, a, 10, text != NULL ? text : "",
      items != NULL ? items : "");
  if (id >= 1 && id <= DESKWIN_WIDGETS)
    {
      /* Known at once (the desktop sets it again when it has made it) */

      s_lock(d);
      d->mirror[id].type  = type;
      d->mirror[id].value = value;
      mirror_text(d, id, text);
      s_unlock(d);
    }
}

void deskwin_widget_set(FAR struct deskwin_s *d, int id, int what,
                        int value, FAR const char *text)
{
  int32_t a[3] =
    {
      id, what, value
    };

  put(d, CMD_WSET, a, 3, text != NULL ? text : "", NULL);
  if (id >= 1 && id <= DESKWIN_WIDGETS)
    {
      s_lock(d);
      if (what == DESKWIN_SET_VALUE)
        {
          d->mirror[id].value = value;
        }
      else if (what == DESKWIN_SET_TEXT)
        {
          mirror_text(d, id, text);
        }

      s_unlock(d);
    }
}

void deskwin_widget_move(FAR struct deskwin_s *d, int id, int x, int y,
                         int w, int h)
{
  int32_t a[5] =
    {
      id, x, y, w, h
    };

  put(d, CMD_WMOVE, a, 5, NULL, NULL);
}

void deskwin_widget_remove(FAR struct deskwin_s *d, int id)
{
  int32_t a[1] =
    {
      id
    };

  put(d, CMD_WDEL, a, 1, NULL, NULL);
  if (id >= 1 && id <= DESKWIN_WIDGETS)
    {
      s_lock(d);
      d->mirror[id].type = 0;
      mirror_text(d, id, NULL);
      s_unlock(d);
    }
}

bool deskwin_widget_get(FAR struct deskwin_s *d, int id, FAR int *value,
                        FAR char *text, int textlen)
{
  bool ok;

  if (id < 1 || id > DESKWIN_WIDGETS)
    {
      return false;
    }

  s_lock(d);
  ok = d->mirror[id].type != 0;
  if (ok)
    {
      if (value != NULL)
        {
          *value = d->mirror[id].value;
        }

      if (text != NULL && textlen > 0)
        {
          strlcpy(text, d->mirror[id].text != NULL ? d->mirror[id].text :
                  "", textlen);
        }
    }

  s_unlock(d);
  return ok;
}

void deskwin_widget_size(FAR struct deskwin_s *d, int id, FAR int *w,
                         FAR int *h)
{
  *w = 0;
  *h = 0;
  if (id < 1 || id > DESKWIN_WIDGETS || sync_wait(d) < 0)
    {
      return;
    }

  s_lock(d);
  *w = d->mirror[id].nw;
  *h = d->mirror[id].nh;
  s_unlock(d);
}

int deskwin_text_width(int font, FAR const char *s)
{
  return font >= 0 && font < DESKWIN_FONTS ? text_width(g_fonts[font], s) :
         0;
}

int deskwin_text_height(int font)
{
  return font >= 0 && font < DESKWIN_FONTS ? text_height(g_fonts[font]) :
         0;
}

int deskwin_colour(FAR const char *role)
{
  static const struct
  {
    const char *name;
    size_t off;
  }

  roles[] =
  {
#define ROLE(n) { #n, offsetof(struct desk_theme_s, n) }
    ROLE(desktop), ROLE(window_bg), ROLE(card), ROLE(sidebar), ROLE(bar),
    ROLE(field), ROLE(separator), ROLE(text), ROLE(text_dim),
    ROLE(text_faint), ROLE(accent), ROLE(accent_text), ROLE(selection),
    ROLE(focus), ROLE(control), ROLE(control_hover),
    ROLE(control_pressed), ROLE(control_text), ROLE(destructive),
    ROLE(error), ROLE(warning), ROLE(success), ROLE(hilite), ROLE(shade),
    ROLE(thumb), ROLE(term_bg), ROLE(term_fg)
#undef ROLE
  };

  size_t i;

  for (i = 0; i < sizeof(roles) / sizeof(roles[0]); i++)
    {
      if (strcmp(roles[i].name, role) == 0)
        {
          return *(const uint16_t *)((const char *)&g_theme +
                                     roles[i].off);
        }
    }

  return -1;
}

#endif /* CONFIG_FPCDESK_CLIENTS */
