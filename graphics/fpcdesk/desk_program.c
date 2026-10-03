/****************************************************************************
 * apps/graphics/fpcdesk/desk_program.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Applications as programs of their own.  An application of the desktop's
 * (struct desk_app_s) holds memory only while it runs when it is a program
 * on the PATH (a loadable module in /mnt/sd0/bin): the desktop keeps only
 * its name and icon (desk_app_s.program, DESK_PROGRAM_APP()) and starts the
 * program when it is launched.
 *
 * The program is the application's own code with a main() that calls
 * desk_program_main().  That opens its window as the desktop would (the
 * application's open()), then reads the window's events (deskwin.h) and
 * calls the application's callbacks with them as the desktop does, until
 * the last of its windows has gone.  The application does not know: its
 * desk_win_open() comes here (desk_program_self()), and what it draws with
 * ops_draw() on its window goes to the desktop as drawing operations
 * (deskwin_ops()), the window's handle marked (desk_program_hwnd()).  The
 * desktop's library (the drawing, the fonts, the theme, the kit) is the
 * firmware's, shared, in the flash.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>

#include <nuttx/nx/nxkeys.h>

#include <graphics/deskwin.h>

#include "desk.h"

#ifdef CONFIG_FPCDESK_CLIENTS

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define PROGS        8                    /* Programs running at once */
#define POLL_MS      20                   /* More than one window: each
                                           * waited on this long in turn */

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A program's window: the application's view of it, and the deskwin
 * session behind it
 */

struct pwin_s
{
  struct desk_win_s w;                    /* (First: the handle points
                                           * here, marked) */
  struct deskwin_s *d;
  struct pwin_s *next;
  bool frames;                            /* deskwin_frames() as it is */
  bool closing;                           /* desk_win_close() by the app */
};

/* A surface of the program's: made by the desktop for its window's
 * session (deskwin_surface_open()), the desktop's handle behind it
 */

struct psurf_s
{
  struct deskwin_s *d;
  FAR void *real;
};

struct prog_s
{
  pid_t pid;
  int app;
  const struct desk_app_s *a;
  struct pwin_s *wins;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_plock = PTHREAD_MUTEX_INITIALIZER;
static struct prog_s *g_progs[PROGS];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static struct prog_s *prog_self(void)
{
  pid_t pid = getpid();
  struct prog_s *p = NULL;
  int i;

  pthread_mutex_lock(&g_plock);
  for (i = 0; i < PROGS && p == NULL; i++)
    {
      if (g_progs[i] != NULL && g_progs[i]->pid == pid)
        {
          p = g_progs[i];
        }
    }

  pthread_mutex_unlock(&g_plock);
  return p;
}

static bool prog_register(struct prog_s *p, bool add)
{
  bool ok = false;
  int i;

  pthread_mutex_lock(&g_plock);
  for (i = 0; i < PROGS && !ok; i++)
    {
      if (add ? g_progs[i] == NULL : g_progs[i] == p)
        {
          g_progs[i] = add ? p : NULL;
          ok = true;
        }
    }

  pthread_mutex_unlock(&g_plock);
  return ok;
}

static struct pwin_s *pwin_of(NXWINDOW hwnd)
{
  return (struct pwin_s *)((uintptr_t)hwnd & ~(uintptr_t)3);
}

static struct psurf_s *psurf_of(NXWINDOW hwnd)
{
  return (struct psurf_s *)((uintptr_t)hwnd & ~(uintptr_t)3);
}

/* A window gone (closed by the user, the desktop, or the application):
 * the application's close(), then its memory
 */

static void pwin_free(struct prog_s *p, struct pwin_s *pw)
{
  struct pwin_s **pp;

  if (p->a->close != NULL)
    {
      p->a->close(&pw->w);
    }

  free(pw->w.priv);
  for (pp = &p->wins; *pp != NULL; pp = &(*pp)->next)
    {
      if (*pp == pw)
        {
          *pp = pw->next;
          break;
        }
    }

  deskwin_close(pw->d);
  free(pw);
}

/* All of it drawn again (the application's draw, else its expose) */

static void pwin_redraw(struct prog_s *p, struct pwin_s *pw)
{
  struct desk_win_s *w = &pw->w;

  if (p->a->draw != NULL)
    {
      p->a->draw(w);
    }
  else if (p->a->expose != NULL)
    {
      p->a->expose(w, 0, 0, w->csize.w, w->csize.h);
    }
}

/* One of the window's events, to the application as the desktop would
 * hand it over
 */

static void pwin_event(struct prog_s *p, struct pwin_s *pw,
                       const struct deskwin_event_s *ev)
{
  const struct desk_app_s *a = p->a;
  struct desk_win_s *w = &pw->w;

  switch (ev->type)
    {
      case DESKWIN_EV_MOUSE:
        if (a->mouse != NULL)
          {
            a->mouse(w, ev->x, ev->y, ev->buttons);
          }
        break;

      case DESKWIN_EV_KEY:
      case DESKWIN_EV_KEYUP:
        {
          struct nx_key_s k;

          k.flags = (ev->mods & NX_KEY_MODS) |
                    (ev->type == DESKWIN_EV_KEYUP ? NX_KEY_RELEASE : 0) |
                    ((ev->kind & DESKWIN_KEY_REPEAT) != 0 ?
                     NX_KEY_REPEAT : 0);
          k.code  = ev->key;
          k.ch    = ev->type == DESKWIN_EV_KEYUP ? 0 : ev->ch;
          if (a->keyev != NULL)
            {
              a->keyev(w, &k);
            }
          else if (a->key != NULL && ev->type == DESKWIN_EV_KEY)
            {
              uint8_t bytes[NX_KEY_MAXBYTES];
              int n = nx_key_bytes(&k, bytes);

              if (n > 0)
                {
                  a->key(w, bytes, n);
                }
            }
        }
        break;

      case DESKWIN_EV_RESIZE:
        w->csize.w = ev->w;
        w->csize.h = ev->h;
        w->size.w  = ev->w;
        w->size.h  = ev->h + TITLE_H;
        if (a->resize != NULL)
          {
            a->resize(w);
          }
        else
          {
            pwin_redraw(p, pw);
          }
        break;

      case DESKWIN_EV_EXPOSE:
        if (a->expose != NULL)
          {
            a->expose(w, ev->x, ev->y, ev->w, ev->h);
          }
        else if (a->draw != NULL)
          {
            a->draw(w);
          }
        break;

      case DESKWIN_EV_FRAME:
        if (a->frame != NULL)
          {
            a->frame(w);
          }
        break;

      case DESKWIN_EV_TICK:
        if (a->tick != NULL)
          {
            a->tick(w);
          }
        break;

      case DESKWIN_EV_FOCUS:
        w->active = ev->value != 0;
        if (a->focus != NULL)
          {
            a->focus(w);
          }
        break;

      default:
        break;
    }
}

/* After the application has had an event: what it changed of its window
 * (its frames, a wake asked for), and what it drew, to the desktop
 */

static void pwin_after(struct prog_s *p, struct pwin_s *pw)
{
  struct desk_win_s *w = &pw->w;

  if (w->wakeq)
    {
      w->wakeq = false;
      if (p->a->wake != NULL)
        {
          p->a->wake(w);
        }
    }

  if (w->frames != pw->frames)
    {
      pw->frames = w->frames;
      deskwin_frames(pw->d, w->frames);
    }

  deskwin_flush(pw->d, -1);
}

/****************************************************************************
 * Public Functions: the desktop's
 ****************************************************************************/

/* Start an application's program from the PATH: its pid, or a negated
 * errno (the program is not there: the launcher has nothing to show)
 */

int desk_program_start(int app)
{
  const struct desk_app_s *a = g_apps[app];
  FAR char *argv[2];
  pid_t pid;
  int ret;

  argv[0] = (FAR char *)a->program;
  argv[1] = NULL;
  ret = posix_spawnp(&pid, a->program, NULL, NULL, argv, NULL);
  if (ret != 0)
    {
      syslog(LOG_WARNING, "desk: %s: cannot start %s: %d\n", a->name,
             a->program, ret);
      return -ret;
    }

  syslog(LOG_INFO, "desk: %s (%d): %s\n", a->name, (int)pid, a->program);
  return pid;
}

/****************************************************************************
 * Public Functions: the program's
 ****************************************************************************/

/* Running as an application's program (the calling task's) */

bool desk_program_self(void)
{
  return prog_self() != NULL;
}

/* The application's desk_win_open(): a deskwin window of the desktop's,
 * belonging to the application (its name and icon in the dock)
 */

struct desk_win_s *desk_program_win_open(int app, const char *title, int w,
                                         int h)
{
  struct prog_s *p = prog_self();
  struct pwin_s *pw;
  int cw;
  int ch;

  if (p == NULL)
    {
      return NULL;
    }

  pw = calloc(1, sizeof(*pw));
  if (pw == NULL)
    {
      return NULL;
    }

  pw->d = deskwin_open_app(app, title, w, h);
  if (pw->d == NULL)
    {
      free(pw);
      return NULL;
    }

  deskwin_size(pw->d, &cw, &ch);
  pw->w.app       = app;
  pw->w.hwnd      = (NXTKWINDOW)((uintptr_t)pw | 1);
  pw->w.csize.w   = cw;
  pw->w.csize.h   = ch;
  pw->w.size.w    = cw;
  pw->w.size.h    = ch + TITLE_H;
  pw->w.active    = true;
  pw->w.hover     = -1;
  pw->w.pressed   = -1;
  pw->w.pid       = getpid();
  strlcpy(pw->w.title, title != NULL ? title : "", sizeof(pw->w.title));
  pw->next        = p->wins;
  p->wins         = pw;
  return &pw->w;
}

/* The application's ops_draw() on its window: its operations to the
 * desktop (handed over at the end of the event, pwin_after())
 */

void desk_program_ops(NXWINDOW hwnd, struct desk_ops_s *o)
{
  int i;

  /* A source that is one of its surfaces: the desktop's handle */

  for (i = 0; i < o->n; i++)
    {
      if (desk_program_surf(o->op[i].src))
        {
          o->op[i].src = psurf_of(o->op[i].src)->real;
        }
    }

  if (o->n > 0 && desk_program_surf(hwnd))
    {
      struct psurf_s *ps = psurf_of(hwnd);

      deskwin_ops_to(ps->d, ps->real, o->op, o->n);
    }
  else if (o->n > 0)
    {
      deskwin_ops(pwin_of(hwnd)->d, o->op, o->n);
    }

  o->n = 0;
}

/* The application's desk_surface_open(): one of the desktop's for the
 * session of its (first) window: NULL before it has one
 */

NXWINDOW desk_program_surface_open(int w, int h, volatile bool *lost)
{
  struct prog_s *p = prog_self();
  struct psurf_s *ps;

  if (p == NULL || p->wins == NULL)
    {
      return NULL;
    }

  ps = malloc(sizeof(*ps));
  if (ps == NULL)
    {
      return NULL;
    }

  ps->d    = p->wins->d;
  ps->real = deskwin_surface_open(ps->d, w, h, lost);
  if (ps->real == NULL)
    {
      free(ps);
      return NULL;
    }

  return (NXWINDOW)((uintptr_t)ps | 3);
}

void desk_program_surface_close(NXWINDOW hwnd)
{
  struct psurf_s *ps = psurf_of(hwnd);

  deskwin_surface_close(ps->d, ps->real);
  free(ps);
}

/* The application's desk_win_close(): gone once the event is done */

void desk_program_win_close(struct desk_win_s *w)
{
  pwin_of((NXWINDOW)w->hwnd)->closing = true;
}

/* The program's main(): the application's window, its events, until it
 * has none left
 */

int desk_program_main(int app, const struct desk_app_s *a, int argc,
                      char *argv[])
{
  struct prog_s prog;
  struct desk_win_s *w;

  (void)argc;
  (void)argv;
  memset(&prog, 0, sizeof(prog));
  prog.pid = getpid();
  prog.app = app;
  prog.a   = a;
  if (a == NULL || a->open == NULL || !prog_register(&prog, true))
    {
      fprintf(stderr, "%s: not an application, or too many running\n",
              argv[0]);
      return EXIT_FAILURE;
    }

  w = a->open();
  if (w == NULL || prog.wins == NULL)
    {
      fprintf(stderr, "%s: no window (is the desktop running?)\n",
              argv[0]);
      prog_register(&prog, false);
      return EXIT_FAILURE;
    }

  pwin_redraw(&prog, prog.wins);
  pwin_after(&prog, prog.wins);

  while (prog.wins != NULL)
    {
      struct pwin_s *pw;
      struct pwin_s *next;
      int timeout = prog.wins->next == NULL ? -1 : POLL_MS;

      for (pw = prog.wins; pw != NULL; pw = next)
        {
          struct deskwin_event_s ev;
          int ret;

          next = pw->next;
          ret  = deskwin_event(pw->d, &ev, timeout);
          if (ret < 0 || (ret > 0 && ev.type == DESKWIN_EV_CLOSE))
            {
              pwin_free(&prog, pw);             /* (Gone) */
              continue;
            }

          while (ret > 0 && !pw->closing)
            {
              pwin_event(&prog, pw, &ev);
              ret = deskwin_event(pw->d, &ev, 0);
              if (ret > 0 && ev.type == DESKWIN_EV_CLOSE)
                {
                  pw->closing = true;
                }
            }

          if (pw->closing)
            {
              pwin_free(&prog, pw);
              continue;
            }

          pwin_after(&prog, pw);
        }
    }

  prog_register(&prog, false);
  return EXIT_SUCCESS;
}

#endif /* CONFIG_FPCDESK_CLIENTS */
