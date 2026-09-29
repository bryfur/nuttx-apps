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
#include <time.h>
#include <semaphore.h>

#include <nuttx/nx/nx.h>
#include <nuttx/nx/nxtk.h>
#include <nuttx/nx/nxcompose.h>
#include <nuttx/nx/nxkeys.h>

#include "desk_font.h"
#include "desk_theme.h"

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

/* The look: colours by role come from the theme (desk_theme.h); these are
 * the older names for them
 */

#define TOPBAR_H          28
#define TITLE_H           34

#define C_TITLE_ACTIVE    (g_theme.title_active)
#define C_TITLE_INACTIVE  (g_theme.title_inactive)
#define C_TITLE_HILITE    (g_theme.title_hilite)
#define C_TITLE_TEXT      (g_theme.title_text)
#define C_TITLE_DIM       (g_theme.title_dim)
#define C_WINDOW_BG       (g_theme.window_bg)
#define C_CARD            (g_theme.card)
#define C_TEXT            (g_theme.text)
#define C_TEXT_DIM        (g_theme.text_dim)
#define C_TEXT_FAINT      (g_theme.text_faint)
#define C_ACCENT          (g_theme.accent)
#define C_ACCENT_TEXT     (g_theme.accent_text)
#define C_SELECTION       (g_theme.selection)
#define C_HILITE          (g_theme.hilite)
#define C_SHADE           (g_theme.shade)
#define C_THUMB           (g_theme.thumb)
#define C_LIGHT_OFF       (g_theme.light_off)
#define C_TERM_BG         (g_theme.term_bg)
#define C_TERM_FG         (g_theme.term_fg)

/* Fixed colours: the traffic lights, white and black */

#define C_CLOSE           RGB(0xff, 0x5f, 0x57)
#define C_MINIMIZE        RGB(0xfe, 0xbc, 0x2e)
#define C_MAXIMIZE        RGB(0x28, 0xc8, 0x40)
#define C_WHITE           RGB(0xff, 0xff, 0xff)
#define C_BLACK           RGB(0x00, 0x00, 0x00)

/* The dock: its icons' size is a setting (40 to 64), and where it is */

#define DOCK_ICON         (g_prefs.dockicon)
#define DOCK_ICON_MIN     40
#define DOCK_ICON_MAX     64
#define DOCK_PAD          (DOCK_ICON / 5)
#define DOCK_H            (DOCK_ICON + 2 * DOCK_PAD)     /* Its thickness */
#define DOCK_MARGIN       8

#define DOCK_BOTTOM       0
#define DOCK_LEFT         1
#define DOCK_RIGHT        2

/* Where windows may go: the display less the top bar and the dock (unless
 * it hides itself); desk_layout() works it out
 */

#define WORK_X            (g_desk.work.pt1.x)
#define WORK_Y            (g_desk.work.pt1.y)
#define WORK_W            (g_desk.work.pt2.x - g_desk.work.pt1.x + 1)
#define WORK_H            (g_desk.work.pt2.y - g_desk.work.pt1.y + 1)

/* Window shadows (Settings) */

#define SHADOW_OFF        0
#define SHADOW_LIGHT      1
#define SHADOW_NORMAL     2
#define SHADOW_STRONG     3

/* Client shape tables (nx_setlut()): round corners of these radii */

#define LUT_R12           (NX_LUT_CLIENT0 + 0)
#define LUT_R6            (NX_LUT_CLIENT0 + 1)
#define LUT_GLOW          (NX_LUT_CLIENT0 + 2)   /* A wide Gaussian around a point */
#define LUT_R3            (NX_LUT_CLIENT0 + 3)

/* The applications' table: loaded by an application for what it draws into
 * its own windows, never for a display list (the compositor would replay
 * that with whatever table is loaded then).  desk_lut_app() loads it.
 */

#define LUT_APP           (NX_LUT_CLIENT0 + 4)

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
  APP_VIEWER,
  APP_PAINT,
  APP_MONITOR,
  APP_CALC,
  APP_TETRIS,
  APP_SOLITAIRE,
  APP_BREAKOUT,
#ifdef CONFIG_GAMES_NXDOOM_DESKTOP
  APP_DOOM,
#endif
  APP_SETTINGS,
  APP_ABOUT,
  APP_LOG,
#ifdef CONFIG_FPCDESK_WIDGETS
  APP_WIDGETS,
#endif
#ifdef CONFIG_FPCDESK_MUSIC
  APP_MUSIC,                       /* (Last: the icons' places in their
                                    * window, and so their dither, stay
                                    * as they were) */
#endif
#ifdef CONFIG_FPCDESK_PYTHON
  APP_PYTHON,                      /* Python: programs' windows (deskwin.h) */
#endif
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
  EV_MODE,                         /* The display mode has changed */
  EV_SOUND,                        /* The mixer's state has changed */
  EV_CLIENT                        /* A program wants a window (deskwin.h) */
};

enum desk_part_e
{
  PART_CLIENT = 0,                 /* Client area of a framed window */
  PART_TITLE,                      /* Its title bar (the toolbar) */
  PART_TOPBAR,
  PART_DOCK,
  PART_LAUNCHER,
  PART_WALLPAPER,
  PART_TIP,
  PART_MENU,                       /* A pop-up menu */
  PART_EDGE,                       /* Where the hidden dock comes out */
  PART_SOUND                       /* The volume popover */
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

/* A layer window's display list longer than one desk_ops_s (up to
 * NX_DLIST_MAX): operations are built in a desk_ops_s and moved into it
 */

struct desk_list_s
{
  struct nx_gpuop_s *op;
  int n;
  int cap;
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
  void (*keyev)(struct desk_win_s *w, const struct nx_key_s *k);
                                                 /* Key events, while active:
                                                  * each press, repeat and
                                                  * release, with the
                                                  * modifiers (nxkeys.h).  An
                                                  * app that has this gets
                                                  * its keys here, not as
                                                  * bytes (key still gets
                                                  * plain bytes, deskctl k) */
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
  void (*theme)(struct desk_win_s *w);           /* The theme changed: all
                                                  * of it again (default:
                                                  * expose all of the client
                                                  * area, else draw) */
  bool single;                                   /* One window at most */
  bool nopark;                                   /* Its window must keep its
                                                  * pixels when not seen (the
                                                  * compositor may otherwise
                                                  * drop them and ask for all
                                                  * of it again: expose) */
  bool (*canclose)(struct desk_win_s *w);        /* The close button: false
                                                  * keeps the window (to ask
                                                  * first; the app then calls
                                                  * desk_win_close()) */
};

/* The settings remembered across boots (desk_prefs_load(), _save()) */

struct desk_prefs_s
{
  uint8_t theme;                   /* THEME_* */
  uint8_t accent;                  /* ACCENT_* */
  uint8_t shadow;                  /* SHADOW_* */
  bool border;                     /* Windows' 1-pixel edge */
  bool clock24;                    /* The top bar's clock: 24-hour */
  bool clockdate;                  /* ... with the date */
  bool clocksecs;                  /* ... with seconds */
  uint8_t dockicon;                /* DOCK_ICON_MIN to _MAX */
  uint8_t dockpos;                 /* DOCK_BOTTOM, _LEFT, _RIGHT */
  bool dockhide;                   /* The dock hides until the pointer
                                    * touches its edge */
  uint16_t repdelay;               /* Keys repeat after (ms), 0: never */
  uint16_t reprate;                /* ... every (ms) */
  uint16_t ptrspeed;               /* Pointer speed, percent */
  int16_t tzoff;                   /* Time zone: minutes east of UTC */
  uint8_t npinned;                 /* The dock's favourites, in order */
  uint8_t pinned[APP_COUNT];
};

/* The desktop */

struct desk_s
{
  NXHANDLE hnx;
  int w;                           /* The display's size */
  int h;
  bool composited;
  struct desk_win_s *wins;         /* Application windows, top first */
  struct desk_win_s *active;
  NXWINDOW atlas;                  /* Hidden: the glyph atlas */
  NXWINDOW wallpaper;
  NXWINDOW topbar;
  NXWINDOW dock;
  NXWINDOW tip;
  NXWINDOW launcher;               /* NULL when closed */
  int wallstyle;                   /* Settings: which wallpaper */
  NXWINDOW menu;                   /* A pop-up menu, NULL */
  NXWINDOW edge;                   /* The hidden dock's edge, NULL */
  struct nxgl_rect_s work;         /* Where windows may go */
  int dockx;                       /* The dock's top-left, shown */
  int docky;
  int dockw;                       /* Its size */
  int dockh;
  int dockhover;                   /* Item under the pointer, -1 */
  int dockout;                     /* Autohide: 0 hidden .. DOCK_STEPS out */
  bool dockwant;                   /* ... it should be out */
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
extern struct desk_prefs_s g_prefs;
extern const struct desk_app_s * const g_apps[APP_COUNT];
extern const struct desk_app_s g_app_terminal;   /* desk_term.c */
extern const struct desk_app_s g_app_files;      /* desk_files.c */
extern const struct desk_app_s g_app_editor;     /* desk_edit.c */
extern const struct desk_app_s g_app_monitor;    /* desk_monitor.c */
extern const struct desk_app_s g_app_calc;       /* desk_calc.c */
extern const struct desk_app_s g_app_tetris;     /* desk_tetris.c */
extern const struct desk_app_s g_app_solitaire;  /* desk_solitaire.c */
extern const struct desk_app_s g_app_breakout;   /* desk_breakout.c */
extern const struct desk_app_s g_app_settings;   /* desk_settings.c */
extern const struct desk_app_s g_app_log;        /* desk_log.c */
#ifdef CONFIG_FPCDESK_MUSIC
extern const struct desk_app_s g_app_music;      /* desk_music.c */
#endif
#ifdef CONFIG_GAMES_NXDOOM_DESKTOP
extern const struct desk_app_s g_app_doom;       /* desk_game.c */
#endif
#ifdef CONFIG_FPCDESK_PYTHON
extern const struct desk_app_s g_app_python;     /* desk_python.c */
#endif

extern const struct desk_app_s g_app_viewer;     /* desk_viewer.c */
extern const struct desk_app_s g_app_paint;      /* desk_paint.c */
extern const struct desk_app_s g_app_widgets;    /* desk_kitdemo.c */

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

/* A disc d pixels across at (x, y), from the glyph atlas like a character
 * (one BLEND): false if the atlas has none of that size (6, 8, 14, 16,
 * 18, 20, 22)
 */

bool ops_disc_atlas(struct desk_ops_s *o, int x, int y, int d, uint16_t c,
                    uint8_t alpha);

int  desk_atlas_init(void);
void desk_luts_init(void);

/* A hidden window of w x h pixels in the GPU's memory, to draw into once
 * and use as an image source (op->src) for COPY and BLEND: sprites,
 * pre-drawn tiles.  *lost is set (on the listener thread) when the server
 * has had to take its pixels away: draw them again.
 */

NXWINDOW desk_surface_open(int w, int h, volatile bool *lost);
void desk_surface_close(NXWINDOW hwnd);

/* LUT_APP: make (if key is not what it holds) and load it; key 0 is none */

typedef void (*desk_lutmake_t)(uint8_t lut[NX_BLEND_LUT_N][NX_BLEND_LUT_N],
                               int arg);
void desk_lut_app(uint32_t key, desk_lutmake_t make, int arg);

/* Send operations: into a window's pixels, or as a layer's display list */

void ops_draw(NXWINDOW hwnd, struct desk_ops_s *o);
void ops_setlist(NXWINDOW hwnd, struct desk_ops_s *o);

/* Long display lists: make room for need more operations in o (moving
 * what it has into l), and set l, with what is left in o, as hwnd's list
 */

void list_room(struct desk_list_s *l, struct desk_ops_s *o, int need);
void list_set(NXWINDOW hwnd, struct desk_list_s *l, struct desk_ops_s *o);

/* desk_main.c: windows */

struct desk_win_s *desk_win_open(int app, const char *title, int w, int h);
void desk_win_close(struct desk_win_s *w);
void desk_win_activate(struct desk_win_s *w);
void desk_win_title(struct desk_win_s *w);
void desk_win_minimize(struct desk_win_s *w);
void desk_win_restore(struct desk_win_s *w);
void desk_win_maximize(struct desk_win_s *w);
void desk_win_sync(struct desk_win_s *w);
void desk_win_show(struct desk_win_s *w);  /* A window just opened (and
                                            * drawn): it fades in, active */

/* From any thread: have w's application's wake callback called on the
 * main thread soon (once, however many calls come before it runs).  The
 * caller must know that w is not being destroyed.
 */

void desk_wake(struct desk_win_s *w);

/* From any thread: an event (EV_*) for the main thread */

void desk_post(uint8_t type);
struct desk_win_s *desk_app_window(int app);
void desk_launch(int app);
void desk_app_quit(int app);                /* Close all of its windows */
uint32_t desk_ms(void);

/* The theme or the look of windows has changed (g_theme, g_prefs): draw
 * everything again
 */

void desk_theme_changed(void);

/* The work area, the dock and the windows after a change of the display's
 * size or of the dock's settings
 */

void desk_relayout(void);

/* The time as the desktop shows it: UTC plus g_prefs.tzoff */

void desk_localtime(struct tm *tm);

/* The wall clock the desktop shows.  Tests (deskctl freeze) stop it at a
 * fixed time and stop the text cursors blinking (they stay on), so that
 * screenshots do not depend on when they are taken.
 */

time_t desk_time(void);
bool desk_frozen(void);
void desk_freeze(time_t t);                /* 0: the clock runs again */

/* A seed for the games: fixed while the clock is frozen (the same deal,
 * the same pieces), else desk_ms()
 */

uint32_t desk_seed(void);

/* desk_panel.c: the top bar, the dock, the launcher, the wallpaper */

void desk_layout(void);                     /* g_desk.work, the dock */
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
void desk_panels_mouse(int part, int x, int y, uint8_t buttons);
                                           /* Any pointer event (autohide) */
bool desk_launcher_key(const uint8_t *ch, int n);
bool desk_dock_pinned(int app);
bool desk_dock_where(int app, int *x, int *y);   /* deskctl dock */
void desk_dock_pin(int app, bool pin);

/* Pop-up menus: items (a label of "-" is a separator) with ids; pick is
 * called with the id of the one chosen.  At (x, y) on the display, moved
 * to stay on it.  Any press elsewhere, or Escape, closes it.
 */

struct desk_menu_item_s
{
  const char *label;
  int id;
  bool enabled;
};

void desk_menu_show(int x, int y, const struct desk_menu_item_s *items,
                    int n, void (*pick)(int id, void *arg), void *arg);
void desk_menu_close(void);
void desk_menu_mouse(int x, int y, uint8_t buttons);

/* desk_wallimg.c: the wallpaper from an image file (BC1, a v6 GPU) */

#define WALL_IMAGE        3                /* g_desk.wallstyle: the image */

bool desk_wallimg_available(void);
int  desk_wallimg_show(void);
void desk_wallimg_hide(void);
void desk_wallimg_tick(void);
bool desk_wallimg_preview(struct desk_ops_s *o, int x, int y, int w, int h);
extern const struct nx_callback_s g_desk_partcb;

/* desk_apps.c: the applications */

void desk_load_sample(void);
int  desk_prefs_load(void);                 /* The saved display mode, or -1 */
void desk_prefs_save(void);
void desk_input_apply(void);                /* Key repeat, pointer speed:
                                             * to the desktop session */
void desk_about_draw(NXWINDOW hwnd, int x, int y, int w);
                                            /* About this computer */
void icon_tile(struct desk_ops_s *o, int x, int y, int s, uint16_t top,
               uint16_t bottom, uint8_t a);

/* desk_settings.c: Settings, at one of its pages */

#define SETTINGS_APPEARANCE 0
#define SETTINGS_DISPLAY    1
#define SETTINGS_DOCK       2
#define SETTINGS_INPUT      3               /* Mouse & Keyboard */
#define SETTINGS_DATETIME   4
#ifdef CONFIG_FPCDESK_SOUND
#  define SETTINGS_SOUND    5
#  define SETTINGS_ABOUT    6
#  define SETTINGS_PAGES    7
#else
#  define SETTINGS_ABOUT    5
#  define SETTINGS_PAGES    6
#endif

void desk_settings_show(int page);
void desk_settings_changed(int page);       /* What the page shows has
                                             * changed: drawn again if it
                                             * is showing */

/* desk_term.c: a terminal whose tab runs a program (a builtin) instead of
 * NSH; the window closes when it ends
 */

struct desk_win_s *desk_term_run(const char *program);

/* desk_client.c: other tasks' windows (deskwin.h); desk_python.c: their
 * application, Python.  The first word of such a window's w->priv says
 * which kind of Python window it is.
 */

#define PYTHON_CLIENT     0x50594331               /* A program's (deskwin) */
#define PYTHON_IDE        0x50594944               /* The IDE's */

void desk_clients_init(void);
void desk_clients_poll(void);                      /* EV_CLIENT */
extern const struct desk_app_s g_app_client;

/* desk_edit.c: open a file in a new editor window (NULL: a new file) */

int desk_editor_open(const char *path);

/* desk_viewer.c: show an image; desk_paint.c: edit one (NULL: a new one) */

int desk_viewer_open(const char *path);
int desk_paint_open(const char *path);

/* desk_ui.c: the clipboard, shared by the applications (a copy is kept) */

void desk_clip_set(const char *s, size_t n);
const char *desk_clip_get(size_t *n);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_H */
