/****************************************************************************
 * apps/graphics/fpcdesk/desk_kit_surface.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The widget kit's surfaces (desk_kit.h): pictures kept in the GPU's
 * memory to draw from (sprites, tiles, previews, images), each a hidden
 * window, which NX gives a surface of the GPU's space.  Drawing one is a
 * COPY or a BLEND from there: nothing crosses the link but the command.
 * The server can take the pixels away (no room after a display mode
 * change): the surface is then marked lost and its owner's wake function
 * is called (desk_wake()), to draw them again.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <string.h>
#include <time.h>

#include <nuttx/nx/nxbe.h>

#include "desk_kit.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void surface_redraw(NXWINDOW hwnd, const struct nxgl_rect_s *rect,
                           bool more, void *arg)
{
  struct kit_surface_s *s = arg;

  /* After it was made, the server asks for its pixels only when it has
   * lost them
   */

  if (s->armed)
    {
      s->lost = true;
      if (s->owner != NULL)
        {
          desk_wake(s->owner);
        }
    }
}

static void surface_event(NXWINDOW hwnd, enum nx_event_e event, void *arg1,
                          void *arg2)
{
  struct kit_surface_s *s = arg2 != NULL ? arg2 : arg1;

  if (event == NXEVENT_SYNCHED || event == NXEVENT_BLOCKED)
    {
      sem_post(&s->sem);
    }
}

static const struct nx_callback_s g_surfacecb =
{
  .redraw = surface_redraw,
  .event  = surface_event,
};

static void surface_wait(struct kit_surface_s *s)
{
  struct timespec ts;

  clock_gettime(CLOCK_REALTIME, &ts);
  ts.tv_sec += 2;
  while (sem_timedwait(&s->sem, &ts) < 0 && errno == EINTR)
    {
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int kit_surface_open(struct kit_surface_s *s, int w, int h,
                     struct desk_win_s *owner)
{
  struct nxgl_size_s size;
  struct nxgl_point_s pos;

  memset(s, 0, sizeof(*s));

  /* In a program of its own: one of the desktop's for its window (the
   * desktop sets lost; ops_draw() sends what is drawn into it)
   */

  if (desk_program_self())
    {
      s->hwnd = desk_program_surface_open(w, h, &s->lost);
      if (s->hwnd == NULL)
        {
          return -ENOMEM;
        }

      s->w     = w;
      s->h     = h;
      s->owner = owner;
      s->gpu   = true;
      s->armed = true;
      return 0;
    }

  sem_init(&s->sem, 0, 0);
  s->hwnd = nx_openwindow(g_desk.hnx,
                          NXBE_WINDOW_RAMBACKED | NXBE_WINDOW_HIDDEN,
                          &g_surfacecb, s);
  if (s->hwnd == NULL)
    {
      sem_destroy(&s->sem);
      return -ENOMEM;
    }

  size.w = w;
  size.h = h;
  pos.x  = 0;
  pos.y  = 0;
  nx_setsize(s->hwnd, &size);
  nx_setposition(s->hwnd, &pos);

  /* The redraws of the new territory come before the handshake's end */

  if (nx_synch(s->hwnd, s) == 0)
    {
      surface_wait(s);
    }

  s->w     = w;
  s->h     = h;
  s->owner = owner;
  s->gpu   = nx_surface(s->hwnd) > 0;
  s->armed = true;
  return s->gpu ? 0 : 1;
}

void kit_surface_close(struct kit_surface_s *s)
{
  if (s->hwnd == NULL)
    {
      return;
    }

  if (desk_program_surf(s->hwnd))
    {
      desk_program_surface_close(s->hwnd);
      s->hwnd = NULL;
      return;
    }

  /* Nothing more from the server for it (a redraw), then it goes */

  s->armed = false;
  if (nx_block(s->hwnd, s) == 0)
    {
      surface_wait(s);
    }

  nx_closewindow(s->hwnd);
  sem_destroy(&s->sem);
  s->hwnd = NULL;
}

void kit_surface_draw(struct kit_surface_s *s, struct desk_ops_s *o)
{
  if (s->hwnd != NULL && desk_program_surf(s->hwnd))
    {
      ops_draw(s->hwnd, o);                 /* (A program's) */
    }
  else if (s->hwnd != NULL && o->n > 0)
    {
      nx_gpuops(s->hwnd, o->op, o->n);
    }

  o->n = 0;
  s->lost = false;
}

void kit_surface_copy(struct desk_ops_s *o, struct kit_surface_s *s,
                      int sx, int sy, int w, int h, int dx, int dy)
{
  struct nx_gpuop_s *op = ops_add(o);

  op->op = NX_GPUOP_COPY;
  op->x  = dx;
  op->y  = dy;
  op->w  = w;
  op->h  = h;
  op->src = s->hwnd;
  op->sx = sx;
  op->sy = sy;
}

void kit_surface_blend(struct desk_ops_s *o, struct kit_surface_s *s,
                       int sx, int sy, int w, int h, int dx, int dy,
                       uint8_t mode, uint16_t colour, uint8_t alpha)
{
  struct nx_gpuop_s *op = ops_add(o);

  op->op     = NX_GPUOP_BLEND;
  op->x      = dx;
  op->y      = dy;
  op->w      = w;
  op->h      = h;
  op->src    = s->hwnd;
  op->sx     = sx;
  op->sy     = sy;
  op->mode   = mode;
  op->colour = colour;
  op->alpha  = alpha;
}
