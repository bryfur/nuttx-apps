/****************************************************************************
 * apps/graphics/fpcdesk/desk.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The FPC desktop: a shell for the NX server's compositor.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_H
#define __APPS_GRAPHICS_FPCDESK_DESK_H

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <semaphore.h>

#include <nuttx/nx/nx.h>
#include <nuttx/nx/nxtk.h>
#include <nuttx/nx/nxcompose.h>

#include "desk_font.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The display: its size is the NX server's (nx_displaysize()), whatever
 * the mode.  Layout is worked out from it, never from a fixed size.
 */

#define DESK_W            (g_desk.w)
#define DESK_H            (g_desk.h)

/* RGB565 from 8-bit channels */

#define RGB(r, g, b)      ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | \
                                      ((b) >> 3)))

/* The look */

#define TOPBAR_H          28
#define TITLE_H           34
#define DOCK_ICON         48
#define DOCK_PAD          10
#define DOCK_H            (DOCK_ICON + 2 * DOCK_PAD)
#define DOCK_MARGIN       8
#define WORK_Y            (TOPBAR_H + 6)                 /* Where windows may go */
#define WORK_H            (DESK_H - WORK_Y - DOCK_H - 2 * DOCK_MARGIN)

#define C_TITLE_ACTIVE    RGB(0x38, 0x38, 0x3c)
#define C_TITLE_INACTIVE  RGB(0x28, 0x28, 0x2c)
#define C_TITLE_HILITE    RGB(0x48, 0x48, 0x4d)
#define C_TITLE_TEXT      RGB(0xec, 0xec, 0xee)
#define C_TITLE_DIM       RGB(0x8e, 0x8e, 0x93)
#define C_WINDOW_BG       RGB(0x20, 0x20, 0x24)
#define C_CARD            RGB(0x30, 0x30, 0x34)
#define C_TEXT            RGB(0xe8, 0xe8, 0xea)
#define C_TEXT_DIM        RGB(0x98, 0x98, 0x9e)
#define C_ACCENT          RGB(0x0a, 0x84, 0xff)
#define C_CLOSE           RGB(0xff, 0x5f, 0x57)
#define C_MINIMIZE        RGB(0xfe, 0xbc, 0x2e)
#define C_MAXIMIZE        RGB(0x28, 0xc8, 0x40)
#define C_LIGHT_OFF       RGB(0x48, 0x48, 0x4c)
#define C_TERM_BG         RGB(0x18, 0x18, 0x1c)
#define C_TERM_FG         RGB(0xd8, 0xd8, 0xd8)
#define C_WHITE           RGB(0xff, 0xff, 0xff)
#define C_BLACK           RGB(0x00, 0x00, 0x00)

/* Client shape tables (nx_setlut()): round corners of these radii */

#define LUT_R12           (NX_LUT_CLIENT0 + 0)
#define LUT_R6            (NX_LUT_CLIENT0 + 1)
#define LUT_GLOW          (NX_LUT_CLIENT0 + 2)   /* A wide Gaussian around a point */
#define LUT_R3            (NX_LUT_CLIENT0 + 3)
#define LUT_R20           (NX_LUT_CLIENT0 + 4)

/* Traffic lights: three discs of radius 6 in the title bar */

#define LIGHT_R           6
#define LIGHT_X0          14
#define LIGHT_DX          20
#define LIGHT_Y           (TITLE_H / 2)

/* Applications */

enum desk_app_e
{
  APP_LAUNCHER = 0,
  APP_TERMINAL,
  APP_FILES,
  APP_EDITOR,
  APP_MONITOR,
  APP_CALC,
  APP_TETRIS,
  APP_SOLITAIRE,
  APP_BREAKOUT,
  APP_SETTINGS,
  APP_ABOUT,
  APP_COUNT
};

/* Events from the NX listener thread to the desktop's */

enum desk_evtype_e
{
  EV_MOUSE = 1,
  EV_KBD,
  EV_POSITION,
  EV_BLOCKED,
  EV_REDRAW,
  EV_MODE                          /* The display mode has changed */
};

enum desk_part_e
{
  PART_CLIENT = 0,                 /* Client area of a framed window */
  PART_TITLE,                      /* Its title bar (the toolbar) */
  PART_TOPBAR,
  PART_DOCK,
  PART_LAUNCHER,
  PART_WALLPAPER,
  PART_TIP
};

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct desk_win_s;

struct desk_event_s
{
  uint8_t type;                    /* EV_* */
  uint8_t part;                    /* PART_* */
  uint8_t buttons;
  uint8_t nch;
  struct desk_win_s *win;          /* PART_CLIENT, PART_TITLE */
  struct nxgl_point_s pos;         /* Mouse: in the part */
  struct nxgl_size_s size;         /* Position: size of the part */
  struct nxgl_point_s wpos;        /* Position: where the window is */
  uint8_t ch[8];                   /* Keys */
};

/* A list of GPU operations being built */

struct desk_ops_s
{
  int n;
  struct nx_gpuop_s op[NX_GPUOPS_MAX];
};

/* An application window */

struct desk_win_s
{
  struct desk_win_s *next;
  int app;                         /* enum desk_app_e */
  NXTKWINDOW hwnd;
  char title[40];
  struct nxgl_point_s pos;         /* The frame's top-left */
  struct nxgl_size_s size;         /* Frame size */
  struct nxgl_size_s csize;        /* Client area size */
  struct nxgl_rect_s restore;      /* Before maximising */
  bool active;
  bool minimized;
  bool maximized;
  bool closing;
  int hover;                       /* Traffic light under the pointer, -1 */
  int pressed;                     /* Traffic light pressed, -1 */
  uint8_t fade;                    /* Opening: opacity steps to go */
  uint8_t fadeout;                 /* Closing or minimising: steps to go */
  bool closedone;                  /* Faded out: to be closed */
  bool minimizing;                 /* Faded out: to be hidden */
  sem_t synced;                    /* The server has caught up (nx_synch) */
  bool wakeq;                      /* desk_wake(): the app's wake is due */
  bool frames;                     /* The app's frame is due every frame */
  const char *button;              /* A title bar button's symbol, or NULL */
  bool buttonhover;
  bool buttonpressed;
  pid_t pid;                       /* A task whose exit closes the window, -1 */

  /* Application state */

  void *priv;
};

/* An application.  Client coordinates: (0, 0) is the top-left of the
 * window's client area, which is at (0, TITLE_H) in the frame (draw with
 * nx_gpuops() on (NXWINDOW)w->hwnd, frame coordinates).  Every callback
 * runs on the desktop's main thread; any may be NULL.
 */

struct desk_app_s
{
  const char *name;
  void (*icon)(struct desk_ops_s *o, int x, int y, int size, uint8_t alpha);
  struct desk_win_s *(*open)(void);              /* A new window, drawn */
  void (*draw)(struct desk_win_s *w);            /* The whole client area */
  void (*expose)(struct desk_win_s *w, int x, int y, int cw, int ch);
                                                 /* Part of it (default: draw) */
  void (*resize)(struct desk_win_s *w);          /* w->csize changed (default:
                                                  * draw) */
  void (*tick)(struct desk_win_s *w);            /* Once a second */
  void (*mouse)(struct desk_win_s *w, int x, int y, uint8_t buttons);
                                                 /* Every pointer event in the
                                                  * client area (and, while a
                                                  * button pressed there is
                                                  * held, outside it); leaving
                                                  * it: (-1, -1), no buttons */
  void (*key)(struct desk_win_s *w, const uint8_t *ch, int n);
                                                 /* Keys, while active: bytes
                                                  * and VT100 sequences */
  void (*close)(struct desk_win_s *w);           /* Going: free w->priv's
                                                  * contents (w->priv itself
                                                  * is freed after) */
  void (*wake)(struct desk_win_s *w);            /* After desk_wake(w) */
  void (*focus)(struct desk_win_s *w);           /* w->active changed */
  void (*button)(struct desk_win_s *w);          /* w->button clicked */
  void (*frame)(struct desk_win_s *w);           /* Every frame (about 60 a
                                                  * second) while w->frames
                                                  * and not minimised: games,
                                                  * animations (desk_ms() for
                                                  * the time) */
  bool single;                                   /* One window at most */
  bool (*canclose)(struct desk_win_s *w);        /* The close button: false
                                                  * keeps the window (to ask
                                                  * first; the app then calls
                                                  * desk_win_close()) */
};

/* The desktop */

struct desk_s
{
  NXHANDLE hnx;
  int w;                           /* The display's size */
  int h;
  bool composited;
  struct desk_win_s *wins;         /* Application windows */
  struct desk_win_s *active;
  NXWINDOW atlas;                  /* Hidden: the glyph atlas */
  NXWINDOW wallpaper;
  NXWINDOW topbar;
  NXWINDOW dock;
  NXWINDOW tip;
  NXWINDOW launcher;               /* NULL when closed */
  int wallstyle;                   /* Settings: which wallpaper */
  int dockx;                       /* The dock's left edge */
  int dockw;
  int dockhover;                   /* App under the pointer, -1 */
  int launcherfade;                /* Launcher opening: steps to go */
  int topbarhover;                 /* 1: over "Activities" */

  /* Dragging */

  struct desk_win_s *drag;
  struct desk_win_s *resize;
  struct nxgl_point_s grab;        /* Pointer within the frame at the press */
  struct nxgl_point_s grabpos;     /* Frame position at the press */
  struct nxgl_size_s grabsize;
  uint8_t buttons;                 /* Last buttons seen, any part */
  struct desk_win_s *clickwin;     /* Double click detection */
  struct desk_win_s *pointerwin;   /* Client area under the pointer */
  uint32_t clickms;

  /* Load */

  uint8_t cpuhist[60];             /* CPU load history, percent */
  int cpuload;
  unsigned int memused;            /* KB */
  unsigned int memtotal;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

extern struct desk_s g_desk;
extern const struct desk_app_s * const g_apps[APP_COUNT];
extern const struct desk_app_s g_app_terminal;   /* desk_term.c */
extern const struct desk_app_s g_app_files;      /* desk_files.c */
extern const struct desk_app_s g_app_editor;     /* desk_edit.c */
extern const struct desk_app_s g_app_monitor;    /* desk_monitor.c */
extern const struct desk_app_s g_app_calc;       /* desk_calc.c */
extern const struct desk_app_s g_app_tetris;     /* desk_tetris.c */
extern const struct desk_app_s g_app_solitaire;  /* desk_solitaire.c */
extern const struct desk_app_s g_app_breakout;   /* desk_breakout.c */

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* desk_draw.c: building operations */

void ops_reset(struct desk_ops_s *o);
struct nx_gpuop_s *ops_add(struct desk_ops_s *o);
void ops_fill(struct desk_ops_s *o, int x, int y, int w, int h, uint16_t c);
void ops_blend(struct desk_ops_s *o, int x, int y, int w, int h, uint16_t c,
               uint8_t alpha);
void ops_rrect(struct desk_ops_s *o, int x, int y, int w, int h, int slot,
               int r, uint16_t c, uint8_t alpha);
void ops_disc(struct desk_ops_s *o, int cx, int cy, int slot, int r,
              uint16_t c, uint8_t alpha);
void ops_vgrad(struct desk_ops_s *o, int x, int y, int w, int h,
               uint16_t c0, uint16_t c1, uint8_t alpha);
void ops_hgrad(struct desk_ops_s *o, int x, int y, int w, int h,
               uint16_t c0, uint16_t c1, uint8_t alpha);
void ops_rrect_vgrad(struct desk_ops_s *o, int x, int y, int w, int h,
                     int slot, int r, uint16_t c0, uint16_t c1,
                     uint8_t alpha);
void ops_glow(struct desk_ops_s *o, int cx, int cy, int r, int shift,
              uint16_t c, uint8_t alpha);
int  ops_text(struct desk_ops_s *o, const struct desk_font_s *f, int x,
              int y, const char *s, uint16_t c, uint8_t alpha);
int  text_width(const struct desk_font_s *f, const char *s);
int  text_height(const struct desk_font_s *f);
void ops_text_centre(struct desk_ops_s *o, const struct desk_font_s *f,
                     int cx, int y, const char *s, uint16_t c,
                     uint8_t alpha);
uint16_t colour_mix(uint16_t a, uint16_t b, int t256);

/* Any anti-aliased shape: inside(arg, x, y) says whether a point, in 1/16
 * pixel from (x, y), is in it; 4 x 4 samples a pixel, drawn as runs of
 * pixels of the same coverage (a FILL or BLEND each; the list is sent to
 * hwnd when full).  For small shapes drawn once (symbols, sprites).
 */

typedef bool (*ops_inside_t)(void *arg, int x, int y);
void ops_shape(NXWINDOW hwnd, struct desk_ops_s *o, int x, int y, int w,
               int h, uint16_t c, uint8_t alpha, ops_inside_t inside,
               void *arg);

int  desk_atlas_init(void);
void desk_luts_init(void);

/* A hidden window of w x h pixels in the GPU's memory, to draw into once
 * and use as an image source (op->src) for COPY and BLEND: sprites,
 * pre-drawn tiles.  *lost is set (on the listener thread) when the server
 * has had to take its pixels away: draw them again.
 */

NXWINDOW desk_surface_open(int w, int h, volatile bool *lost);
void desk_surface_close(NXWINDOW hwnd);

/* Send operations: into a window's pixels, or as a layer's display list */

void ops_draw(NXWINDOW hwnd, struct desk_ops_s *o);
void ops_setlist(NXWINDOW hwnd, struct desk_ops_s *o);

/* desk_main.c: windows */

struct desk_win_s *desk_win_open(int app, const char *title, int w, int h);
void desk_win_close(struct desk_win_s *w);
void desk_win_activate(struct desk_win_s *w);
void desk_win_title(struct desk_win_s *w);
void desk_win_minimize(struct desk_win_s *w);
void desk_win_restore(struct desk_win_s *w);
void desk_win_maximize(struct desk_win_s *w);
void desk_win_sync(struct desk_win_s *w);

/* From any thread: have w's application's wake callback called on the
 * main thread soon (once, however many calls come before it runs).  The
 * caller must know that w is not being destroyed.
 */

void desk_wake(struct desk_win_s *w);
struct desk_win_s *desk_app_window(int app);
void desk_launch(int app);
uint32_t desk_ms(void);

/* desk_panel.c: the top bar, the dock, the launcher, the wallpaper */

int  desk_panels_init(void);
void desk_topbar_draw(void);
void desk_dock_draw(void);
void desk_dock_mouse(int x, int y, uint8_t buttons);
void desk_topbar_mouse(int x, int y, uint8_t buttons);
void desk_launcher_toggle(void);
void desk_launcher_draw(void);
void desk_launcher_mouse(int x, int y, uint8_t buttons);
void desk_wallpaper_draw(void);
void desk_wallpaper_preview(struct desk_ops_s *o, int style, int x, int y,
                            int w, int h);
void desk_panels_tick(void);
bool desk_panels_animate(void);
void desk_panels_relayout(void);           /* The display's size changed */

/* desk_apps.c: the applications */

void desk_load_sample(void);
int  desk_prefs_load(void);                 /* The saved display mode, or -1 */
void desk_prefs_save(void);
void icon_tile(struct desk_ops_s *o, int x, int y, int s, uint16_t top,
               uint16_t bottom, uint8_t a);

/* desk_edit.c: open a file in a new editor window (NULL: a new file) */

int desk_editor_open(const char *path);

/* desk_ui.c: the clipboard, shared by the applications (a copy is kept) */

void desk_clip_set(const char *s, size_t n);
const char *desk_clip_get(size_t *n);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_H */
