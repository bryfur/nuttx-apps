/****************************************************************************
 * apps/graphics/fpcdesk/desk_vt.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * A terminal emulator's screen: the VT100 / xterm subset that NSH, its
 * readline and the usual tools emit, turned into lines of cells, with a
 * scrollback.  No drawing here (desk_term.c draws): every change marks
 * the columns of its line that changed.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_VT_H
#define __APPS_GRAPHICS_FPCDESK_DESK_VT_H

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Cell attributes */

#define VT_BOLD        0x01
#define VT_UNDER       0x02
#define VT_REVERSE     0x04
#define VT_DIM         0x08
#define VT_HIDDEN      0x10
#define VT_FGDEF       0x20            /* fg is the default colour */
#define VT_BGDEF       0x40            /* bg is the default colour */

/* Characters: 32-126 ASCII, 127-133 the fonts' extras (DESK_CH_*), then
 * shapes the terminal draws itself
 */

#define VT_CH_BOX      0x90            /* | 1 left, 2 right, 4 up, 8 down */
#define VT_CH_BLOCK    0xa0            /* Full block */
#define VT_CH_SHADE    0xa1            /* Medium shade */
#define VT_CH_LOWER    0xa2            /* Lower half block */
#define VT_CH_UPPER    0xa3            /* Upper half block */

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct vt_cell_s
{
  uint8_t ch;
  uint8_t fg;                          /* Palette index (0-255) */
  uint8_t bg;
  uint8_t at;                          /* VT_* */
};

/* A line: its cells (as many as it had columns when it was last written;
 * the rest are blank), and the columns changed since it was last drawn
 */

struct vt_line_s
{
  uint16_t n;                          /* Cells */
  uint16_t d0;                         /* Changed: columns d0 to d1 - 1 */
  uint16_t d1;
  uint8_t wrapped;                     /* Continues on the next line */
  struct vt_cell_s c[];
};

struct vt_save_s
{
  int x;
  int y;
  struct vt_cell_s pen;
  bool origin;
  uint8_t g[2];
  uint8_t gl;
};

struct vt_s
{
  int cols;
  int rows;
  struct vt_line_s **scr;              /* The screen shown: main or alt */
  struct vt_line_s **main;
  struct vt_line_s **alt;              /* The alternate screen, or NULL */

  /* Scrollback: a ring of lines that scrolled off the top of the main
   * screen, oldest first (sb[(sbhead + i) % sbcap]); the ring grows as it
   * fills, up to sbmax lines, then the oldest are reused
   */

  struct vt_line_s **sb;
  int sbmax;
  int sbcap;
  int sbn;
  int sbhead;

  /* The cursor and the pen */

  int x;
  int y;
  bool wrapnext;                       /* The last column was written */
  int top;                             /* Scroll region: rows top-bot */
  int bot;
  struct vt_cell_s pen;
  struct vt_save_s saved;
  bool origin;
  bool autowrap;
  bool insert;
  bool cursor;                         /* Visible (DECTCEM) */
  bool appcursor;                      /* DECCKM: arrows send ESC O x */
  uint8_t g[2];                        /* G0, G1: 0 ASCII, 1 DEC graphics */
  uint8_t gl;
  uint8_t last;                        /* The last character (REP) */

  /* The parser */

  uint8_t state;
  uint8_t priv;                        /* CSI private marker: ? > = */
  uint8_t inter;                       /* Intermediate byte */
  uint8_t npar;
  int par[16];
  uint8_t nosc;
  char osc[64];
  uint32_t ucs;                        /* UTF-8 being decoded */
  uint8_t ulen;

  /* What the owner must look at */

  char title[40];                      /* OSC 0 / 2 */
  bool titled;                         /* ... changed */
  uint32_t epoch;                      /* Lines were freed or replaced */
  uint8_t resp[48];                    /* Replies to send (DSR, DA) */
  uint8_t nresp;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

int  vt_init(struct vt_s *vt, int cols, int rows, int sbmax);
void vt_free(struct vt_s *vt);
void vt_write(struct vt_s *vt, const uint8_t *buf, int n);
int  vt_resize(struct vt_s *vt, int cols, int rows);

/* The line shown at row with the view scrolled back by view lines */

struct vt_line_s *vt_line(struct vt_s *vt, int row, int view);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_VT_H */
