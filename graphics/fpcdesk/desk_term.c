/****************************************************************************
 * apps/graphics/fpcdesk/desk_term.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Terminal: NSH in an NxTerm window (the NxTerm driver draws the text).
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/boardctl.h>

#include <nuttx/nx/nxfonts.h>

#include "nshlib/nshlib.h"
#include "desk.h"

#define TERM_COLS     80
#define TERM_ROWS     24
#define TERM_MARGIN   8               /* Below the last row: the round corners */

static struct desk_ops_s g_o;

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

  printf("FPC - NuttX shell\n\n");
  return nsh_consolemain(0, NULL);
}

static struct desk_win_s *term_open(void)
{
  static bool nshinit;
  static int minor;
  struct boardioc_nxterm_create_s create;
  const struct nx_font_s *fs;
  struct desk_win_s *w;
  NXHANDLE hfont;
  char path[24];
  char *argv[2];
  int cw = 8;
  int ch = 13;

  hfont = nxf_getfonthandle(NXFONT_DEFAULT);
  if (hfont != NULL && (fs = nxf_getfontset(hfont)) != NULL)
    {
      cw = fs->mxwidth;
      ch = fs->mxheight + CONFIG_NXTERM_LINESEPARATION;
    }

  w = desk_win_open(APP_TERMINAL, "Terminal", TERM_COLS * cw + 12,
                    TERM_ROWS * ch + TERM_MARGIN);
  if (w == NULL)
    {
      return NULL;
    }

  ops_reset(&g_o);
  ops_fill(&g_o, 0, TITLE_H, w->csize.w, w->csize.h, C_TERM_BG);
  ops_draw((NXWINDOW)w->hwnd, &g_o);
  desk_win_sync(w);

  if (!nshinit)
    {
      nsh_initialize();
      nshinit = true;
    }

  memset(&create, 0, sizeof(create));
  create.hwnd           = w->hwnd;
  create.wndo.wcolor[0] = C_TERM_BG;
  create.wndo.fcolor[0] = C_TERM_FG;
  create.wndo.wsize     = w->csize;
  create.wndo.fontid    = NXFONT_DEFAULT;
  create.type           = BOARDIOC_XTERM_FRAMED;
  create.minor          = minor;
  if (boardctl(BOARDIOC_NXTERM, (uintptr_t)&create) < 0)
    {
      fprintf(stderr, "desk: no terminal: %d\n", errno);
      return w;
    }

  w->nxterm = create.nxterm;
  w->minor  = minor++;

  snprintf(path, sizeof(path), "/dev/nxterm%d", w->minor);
  argv[0] = path;
  argv[1] = NULL;
  w->pid  = task_create("Terminal", CONFIG_FPCDESK_TERM_PRIORITY,
                        CONFIG_FPCDESK_TERM_STACKSIZE, term_task, argv);
  return w;
}

static void term_close(struct desk_win_s *w)
{
  char path[24];

  if (w->pid >= 0)
    {
      task_delete(w->pid);
      w->pid = -1;
    }

  if (w->nxterm != NULL)
    {
      snprintf(path, sizeof(path), "/dev/nxterm%d", w->minor);
      unlink(path);
      w->nxterm = NULL;
    }
}


/* Keys go to the NxTerm */

static void term_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct boardioc_nxterm_ioctl_s io;
  struct nxtermioc_kbdin_s kbd;

  if (w->nxterm == NULL)
    {
      return;
    }

  kbd.handle = w->nxterm;
  kbd.buffer = ch;
  kbd.buflen = n;
  io.cmd     = NXTERMIOC_NXTERM_KBDIN;
  io.arg     = (uintptr_t)&kbd;
  boardctl(BOARDIOC_NXTERM_IOCTL, (uintptr_t)&io);
}

/* Part of the client area to draw again (new territory): the background,
 * then the NxTerm's text there
 */

static void term_expose(struct desk_win_s *w, int x, int y, int cw, int ch)
{
  struct boardioc_nxterm_ioctl_s io;
  struct nxtermioc_redraw_s redraw;

  ops_reset(&g_o);
  ops_fill(&g_o, x, TITLE_H + y, cw, ch, C_TERM_BG);
  ops_draw((NXWINDOW)w->hwnd, &g_o);

  if (w->nxterm == NULL)
    {
      return;
    }

  redraw.handle     = w->nxterm;
  redraw.rect.pt1.x = x;
  redraw.rect.pt1.y = y;
  redraw.rect.pt2.x = x + cw - 1;
  redraw.rect.pt2.y = y + ch - 1;
  redraw.more       = false;
  io.cmd            = NXTERMIOC_NXTERM_REDRAW;
  io.arg            = (uintptr_t)&redraw;
  boardctl(BOARDIOC_NXTERM_IOCTL, (uintptr_t)&io);
}

/* The client area has a new size */

static void term_resize(struct desk_win_s *w)
{
  struct boardioc_nxterm_ioctl_s io;
  struct nxtermioc_resize_s resize;

  if (w->nxterm != NULL)
    {
      resize.handle = w->nxterm;
      resize.size   = w->csize;
      io.cmd        = NXTERMIOC_NXTERM_RESIZE;
      io.arg        = (uintptr_t)&resize;
      boardctl(BOARDIOC_NXTERM_IOCTL, (uintptr_t)&io);
    }

  term_expose(w, 0, 0, w->csize.w, w->csize.h);
}

const struct desk_app_s g_app_terminal =
{
  .name   = "Terminal",
  .icon   = term_icon,
  .open   = term_open,
  .key    = term_key,
  .expose = term_expose,
  .resize = term_resize,
  .close  = term_close,
};
