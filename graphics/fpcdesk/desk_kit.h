/****************************************************************************
 * apps/graphics/fpcdesk/desk_kit.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The widget kit: the controls of the desktop's applications, laid out,
 * drawn and driven in one place, in the theme's colours
 * (docs/uikit.md).  Colours are the theme's roles (g_theme,
 * desk_theme.h), read as each widget is drawn: when the theme changes, a
 * window's kit_expose() of all of it (the desktop's default) draws it in
 * the new one.
 *
 * An application keeps a struct kit_s and an array of widgets (in its
 * window's private state) and builds its window's controls once, as a
 * tree of boxes (rows and columns) and controls:
 *
 *   kit_init(&a->kit, w, a->wg, NWG, on_event);
 *   kit_col(&a->kit, KIT_BOX_WINDOW);
 *     kit_row(&a->kit, KIT_BOX_BAR);
 *       kit_button(&a->kit, B_SAVE, "Save", KIT_ICON_NONE, KIT_BTN_NORMAL);
 *     kit_end(&a->kit);
 *     kit_list(&a->kit, L_FILES, &a->list, KIT_LIST_TABLE);
 *     kit_grow(&a->kit, 1);
 *   kit_end(&a->kit);
 *   kit_layout(&a->kit);
 *
 * then hands it its window's events (kit_mouse(), kit_key(), kit_tick(),
 * kit_expose(), kit_resize()) and hears back through its event function.
 * Changing a widget (kit_set_*()) marks it; kit_flush() draws what is
 * marked, each widget cut to its own rectangle, batched into as few
 * nx_gpuops() calls as it takes (one, usually).  The kit's own events
 * flush when they are done.
 *
 * Everything is in frame coordinates, as the window is drawn: the client
 * area starts at (0, TITLE_H).  Widgets are small and in the
 * application's array; nothing is allocated while drawing or handling
 * events (a dialog allocates its widgets when it opens).
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_KIT_H
#define __APPS_GRAPHICS_FPCDESK_DESK_KIT_H

#include "desk.h"
#include "desk_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Metrics: every size the kit draws with (those the desktop shares are
 * desk_theme.h's).  Round shapes come from the client shape tables, so
 * the radii are theirs (LUT_R3, R6, R12).
 */

#define KIT_CONTROL_H     UI_CONTROL_H     /* Buttons, fields, drop-downs */
#define KIT_BAR_H         44               /* Tool bars */
#define KIT_STATUS_H      28               /* Status bars */
#define KIT_ROW_H         28               /* Rows of lists */
#define KIT_CARD_ROW_H    44               /* Rows of a card of settings */
#define KIT_STEP          24               /* A stepper's buttons */
#define KIT_STEPPER_W     96
#define KIT_SWATCH_R      12               /* Colour swatches */
#define KIT_SWATCH_DX     30
#define KIT_SIDE_ROW_H    30               /* Rows of sidebars */
#define KIT_HEAD_H        28               /* A list's column names */
#define KIT_TAB_H         30               /* Tab strips */
#define KIT_TAB_MAXW      240
#define KIT_MENU_ROW_H    26               /* Rows of menus */
#define KIT_SB_W          12               /* Scroll bars */
#define KIT_THUMB_MIN     24
#define KIT_PAD           UI_PAD           /* Inside cards, bars, dialogs */
#define KIT_GAP           UI_GAP           /* Between controls */
#define KIT_ICON          16               /* Icons in buttons and rows */
#define KIT_ICON_BTN_W    32               /* A button of an icon */
#define KIT_BTN_PAD       12               /* Around a button's label */
#define KIT_CHECK         18               /* Check boxes */
#define KIT_RADIO         16               /* Radio buttons */
#define KIT_SWITCH_W      48
#define KIT_SWITCH_H      28
#define KIT_SLIDER_H      24               /* A slider's knob */
#define KIT_PROGRESS_H    6
#define KIT_FIELD_W       160              /* A text field's natural width */
#define KIT_RING          3                /* Focus rings: outside controls */
#define KIT_DBLCLICK_MS   400
#define KIT_TIP_MS        600              /* Hover before a tooltip */

#define KIT_R_SMALL       UI_RADIUS_SMALL  /* Radii, with their tables */
#define KIT_LUT_SMALL     LUT_R3
#define KIT_R_CONTROL     UI_RADIUS_CONTROL
#define KIT_LUT_CONTROL   LUT_R6
#define KIT_R_CARD        UI_RADIUS_CARD
#define KIT_LUT_CARD      LUT_R12

/* Fonts by role */

#define KIT_FONT          FONT_BODY        /* Regular 13 */
#define KIT_FONT_BOLD     FONT_BODY_BOLD   /* Medium 13 */
#define KIT_FONT_SMALL    FONT_SMALL       /* Regular 11 */
#define KIT_FONT_HEADING  FONT_HEADING     /* Medium 20 */
#define KIT_FONT_DISPLAY  FONT_DISPLAY     /* Light 44 */

/* Widget types */

#define KIT_BOX           0                /* A row or a column of widgets */
#define KIT_LABEL         1
#define KIT_BUTTON        2
#define KIT_CHECK_BOX     3
#define KIT_RADIO_BTN     4
#define KIT_SWITCH        5
#define KIT_SLIDER        6
#define KIT_PROGRESS      7
#define KIT_FIELD         8                /* One line of text */
#define KIT_DROPDOWN      9
#define KIT_LIST          10               /* Rows (and columns), scrolled */
#define KIT_TABS          11               /* Tabs, segmented controls */
#define KIT_SCROLL        12               /* A scroll bar of its own */
#define KIT_CRUMBS        13               /* A path as buttons */
#define KIT_SEPARATOR     14
#define KIT_SPACER        15
#define KIT_CUSTOM        16               /* Drawn by the application */
#define KIT_STEPPER       17               /* [-] value [+] */
#define KIT_SWATCHES      18               /* Colours to choose from */
#define KIT_IMAGE         19               /* A surface's picture */

/* Flags */

#define KIT_F_DISABLED    0x0001
#define KIT_F_HIDDEN      0x0002
#define KIT_F_ROW         0x0004           /* Box: a row (else a column) */
#define KIT_F_NOFOCUS     0x0008           /* Not in the Tab order */
#define KIT_F_CLOSABLE    0x0010           /* Tabs: each with a close button */
#define KIT_F_KEYS        0x0020           /* Custom: takes every key (Tab
                                            * too) while focused */
#define KIT_F_HOT         0x0040           /* Drawn as if under the pointer, */
#define KIT_F_DOWN        0x0080           /* ... pressed, */
#define KIT_F_RING        0x0100           /* ... focused (galleries) */
#define KIT_F_LINES       0x0200           /* Box: hairlines between its
                                            * rows (a card of settings) */
#define KIT_F_SELECTED    0x0400           /* Field: all of its text is
                                            * selected (typing replaces it) */
#define KIT_F_SCROLL      0x0800           /* Column: what does not fit
                                            * scrolls (a scroll bar at its
                                            * right; value: the offset) */
#define KIT_F_DIRTY       0x1000           /* The kit's: to be drawn */
#define KIT_F_RELAYOUT    0x2000           /* Box: to be laid out again */

/* Box looks (style) */

#define KIT_BOX_NONE      0                /* Its parent's ground */
#define KIT_BOX_WINDOW    1                /* The window's background */
#define KIT_BOX_BAR       2                /* A tool bar: a hairline below */
#define KIT_BOX_STATUS    3                /* A status bar: a hairline above */
#define KIT_BOX_SIDEBAR   4                /* A hairline at its right */
#define KIT_BOX_CARD      5                /* A round card */
#define KIT_BOX_GROUP     6                /* A card with a title above it */
#define KIT_BOX_DIALOG    7                /* A card with an edge */

/* Label looks: a font, a colour, an alignment, or'ed.  A label's text
 * may have a second line after a '\n': a caption, small and dim.
 */

#define KIT_TEXT_BODY     0x00
#define KIT_TEXT_BOLD     0x01
#define KIT_TEXT_SMALL    0x02
#define KIT_TEXT_HEADING  0x03
#define KIT_TEXT_DISPLAY  0x04             /* The big one: a clock */
#define KIT_TEXT_FONT     0x07
#define KIT_TEXT_DIM      0x08
#define KIT_TEXT_ERROR    0x10
#define KIT_TEXT_CENTRE   0x20
#define KIT_TEXT_RIGHT    0x40

/* Button looks */

#define KIT_BTN_NORMAL    0
#define KIT_BTN_DEFAULT   1                /* The accent: Enter presses it */
#define KIT_BTN_DANGER    2                /* Red: delete, discard */
#define KIT_BTN_FLAT      3                /* No background until hovered */
#define KIT_BTN_TOGGLE    4                /* Stays in when value is set */
#define KIT_BTN_CUSTOM    5                /* Drawn by the kit's draw
                                            * function (kit_look()): tiles,
                                            * previews, rows of a list of
                                            * choices */

/* Tabs looks */

#define KIT_TABS_STRIP    0                /* Below a title bar (Terminal) */
#define KIT_TABS_SEGMENTS 1                /* A segmented control */

/* List looks */

#define KIT_LIST_TABLE    0                /* Column names, a scroll bar */
#define KIT_LIST_PLAIN    1                /* A scroll bar */
#define KIT_LIST_SIDEBAR  2                /* Neither: places, pages */

/* Scroll bar looks */

#define KIT_SCROLL_VERT   0                /* Up and down (kit_scroll()) */
#define KIT_SCROLL_HORIZ  1                /* Across: set its style */

/* Columns */

#define KIT_COL_RIGHT     0x01             /* Right-aligned */
#define KIT_COL_DIM       0x02             /* Secondary text */

/* Cross-axis alignment in a box (align) */

#define KIT_A_AUTO        0                /* Boxes, lists, custom widgets,
                                            * separators, spacers, scroll
                                            * bars and tab strips fill; in
                                            * a column so do labels,
                                            * fields, sliders, progress
                                            * bars, crumbs and switches;
                                            * the rest keep their size,
                                            * centred in a row, at the
                                            * start of a column */
#define KIT_A_START       1
#define KIT_A_CENTRE      2
#define KIT_A_END         3
#define KIT_A_FILL        4

/* Icons */

#define KIT_ICON_NONE     0
#define KIT_ICON_BACK     1
#define KIT_ICON_FORWARD  2
#define KIT_ICON_UP       3
#define KIT_ICON_DOWN     4
#define KIT_ICON_REFRESH  5
#define KIT_ICON_CLOSE    6
#define KIT_ICON_PLUS     7
#define KIT_ICON_MINUS    8
#define KIT_ICON_CHECK    9
#define KIT_ICON_CHEVRON  10               /* Down: menus */
#define KIT_ICON_MENU     11               /* Three lines */
#define KIT_ICON_FOLDER   12
#define KIT_ICON_FILE     13
#define KIT_ICON_HOME     14
#define KIT_ICON_INFO     15
#define KIT_ICON_WARNING  16
#define KIT_ICON_CUSTOM   255              /* The kit's draw function */

/* Looks of controls (kit_paint_*()) */

#define KIT_LOOK_NORMAL   0
#define KIT_LOOK_HOT      1
#define KIT_LOOK_DOWN     2
#define KIT_LOOK_DISABLED 3
#define KIT_LOOK_MASK     3
#define KIT_LOOK_FOCUS    4                /* Or'ed: a focus ring */

/* Events (the application's event function, k->arg says more) */

#define KIT_EV_CLICK      1                /* A button; crumbs: arg = which */
#define KIT_EV_CHANGE     2                /* A value: check, radio, switch,
                                            * slider, drop-down, tabs,
                                            * swatches, a field's text, a
                                            * list's selection, a scroll
                                            * bar; a stepper: k->arg -1 or
                                            * 1 (the application sets its
                                            * text) */
#define KIT_EV_ACTIVATE   3                /* A list's row double clicked or
                                            * Enter; Enter in a field; a
                                            * slider let go (its value is
                                            * final) */
#define KIT_EV_SORT       4                /* A list's column: its sortcol
                                            * and sortdesc are set */
#define KIT_EV_CLOSE      5                /* A tab's close button: arg */
#define KIT_EV_MENU       6                /* A menu's item: arg (-1: none) */
#define KIT_EV_DIALOG     7                /* A dialog's answer: arg, below */
#define KIT_EV_MOUSE      8                /* The pointer on (or held by) a
                                            * custom widget, or the right
                                            * button on any: k->mx, my,
                                            * mbuttons */
#define KIT_EV_KEY        9                /* A key nothing took: k->key
                                            * (wg: the focused widget or
                                            * NULL); set k->key to 0 if
                                            * the application took it */

/* Dialogs */

#define KIT_DLG_MESSAGE   0                /* Text, one button */
#define KIT_DLG_CONFIRM   1                /* Text, two or three buttons */
#define KIT_DLG_PROMPT    2                /* Text and a field */
#define KIT_DLG_OPEN      3                /* A file to open */
#define KIT_DLG_SAVE      4                /* A file to save as */
#define KIT_DLG_DANGER    0x80             /* Or'ed: the default button is
                                            * red */

#define KIT_ANS_OK        0                /* The default button, Enter */
#define KIT_ANS_CANCEL    1                /* The second, Escape */
#define KIT_ANS_ALT       2                /* The third, at the left */

#define KIT_NONE          0xff             /* No widget */
#define KIT_DEPTH         8                /* Boxes in boxes */
#define KIT_MAX_SHORTCUTS 8
#define KIT_POP_MAX       24               /* Items of a menu */

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct kit_s;
struct kit_widget_s;

typedef void (*kit_event_t)(struct kit_s *k, struct kit_widget_s *wg,
                            int ev);

/* A custom widget, a custom button, or an icon of the application's: draw
 * it, cut to [x0, x1) x [y0, y1): into kit_ops() (cut for it), or into a
 * list of the application's (the kit has sent its own first)
 */

typedef void (*kit_draw_t)(struct kit_s *k, struct kit_widget_s *wg,
                           int x0, int y0, int x1, int y1);

struct kit_rect_s
{
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
};

/* A widget.  value, min and max are the control's: a check box, radio
 * button, switch or toggle is on when value is not 0; a slider's and a
 * progress bar's value is in [min, max]; tabs', a drop-down's and
 * crumbs' value is the item chosen, of nitems in items (data); a scroll
 * bar's value is its position in [0, max - min], min being the page (what
 * shows) and max all there is.
 */

struct kit_widget_s
{
  uint8_t  type;                   /* KIT_BOX, ... */
  uint8_t  id;                     /* The application's, 0: none */
  uint8_t  style;                  /* KIT_BOX_*, KIT_TEXT_*, KIT_BTN_*, ... */
  uint8_t  end;                    /* Past its last descendant */
  uint16_t flags;                  /* KIT_F_* */
  uint8_t  grow;                   /* Its share of the room left in its
                                    * box, 0: its own size */
  uint8_t  align;                  /* KIT_A_* across its box */
  uint8_t  padx;                   /* Box: around its contents */
  uint8_t  pady;
  uint8_t  gap;                    /* Box: between them */
  uint8_t  icon;                   /* KIT_ICON_* */
  uint8_t  nitems;
  int16_t  x;                      /* Where it is (frame coordinates) */
  int16_t  y;
  int16_t  w;
  int16_t  h;
  int16_t  pw;                     /* The size it wants, 0: its natural */
  int16_t  ph;
  int32_t  value;
  int32_t  min;
  int32_t  max;
  const char *text;                /* Label, placeholder, box's title */
  const char *tip;                 /* Its tooltip, or NULL */
  void    *data;                   /* List: struct kit_list_s; field:
                                    * struct ui_field_s; tabs, drop-down,
                                    * crumbs: const char * const *items */
};

/* A picture in the GPU's memory (desk_kit_surface.c): a hidden window,
 * drawn from with COPY and BLEND.  When the server takes its pixels away
 * (no room after a display mode change), lost is set and owner's wake is
 * called: draw them again (kit_surface_draw()), and the widgets showing
 * it (kit_invalidate()).
 */

struct kit_surface_s
{
  NXWINDOW hwnd;
  int16_t  w;
  int16_t  h;
  bool     gpu;                    /* In the GPU's memory (else PSRAM: slow
                                    * to draw from) */
  bool     armed;                  /* Made: a redraw now means lost */
  volatile bool lost;              /* Its pixels are gone */
  struct desk_win_s *owner;        /* Woken when they are */
  sem_t    sem;
};

/* A list's columns */

struct kit_col_s
{
  const char *name;                /* In the header */
  int16_t  w;                      /* Its width, 0: the room left */
  int16_t  hide;                   /* Left out when the list is narrower */
  uint8_t  flags;                  /* KIT_COL_* */
};

/* A list: its rows are the application's, asked for as they are drawn */

struct kit_list_s
{
  int      n;                      /* Rows */
  int      sel;                    /* Selected, -1: none */
  int      top;                    /* Scrolled, pixels */
  uint8_t  rowh;                   /* 0: KIT_ROW_H (sidebars: _SIDE_ROW_H) */
  uint8_t  ncols;                  /* 0: one column */
  int8_t   sortcol;                /* Sorted by, -1: not sortable */
  bool     sortdesc;
  const struct kit_col_s *cols;
  const char *empty;               /* Shown when there are no rows */

  /* Row row's text in column col (buf is there to format it in) */

  const char *(*text)(struct kit_s *k, struct kit_widget_s *wg, int row,
                      int col, char *buf, int size);

  /* Its icon, KIT_ICON x KIT_ICON at (x, y), or NULL: none */

  void (*icon)(struct kit_s *k, struct kit_widget_s *wg,
               struct desk_ops_s *o, int row, int x, int y, uint16_t fg,
               uint16_t bg);

  /* The kit's */

  int      hover;
  int16_t  dirty[4];               /* Rows to draw again */
  uint8_t  ndirty;
  bool     alldirty;
  bool     headdirty;
  bool     sbdirty;
  bool     thumbdrag;
  int16_t  grab;
};

/* A menu or a drop-down's list, over the window */

struct kit_popup_s
{
  bool     open;
  uint8_t  owner;                  /* A drop-down, or KIT_NONE: a menu */
  uint8_t  n;
  int8_t   hover;
  int8_t   sel;                    /* Marked (a drop-down's value) */
  bool     armed;                  /* Released once: a click chooses */
  int      id;                     /* A menu's id (KIT_EV_MENU) */
  uint32_t disabled;               /* Items not enabled, a bit each */
  struct kit_rect_s r;
  const char *items[KIT_POP_MAX];  /* "-": a separator */
};

struct kit_s
{
  struct desk_win_s *w;
  struct kit_widget_s *wg;
  uint8_t  n;
  uint8_t  max;
  uint8_t  depth;                  /* Building: the boxes being filled */
  uint8_t  stack[KIT_DEPTH];
  uint8_t  hot;                    /* Under the pointer, KIT_NONE */
  uint8_t  down;                   /* Pressed (it has the pointer) */
  uint8_t  focus;                  /* Has the keys */
  int16_t  hotpart;                /* Which part of it: a tab, a row ... */
  int16_t  downpart;
  bool     ring;                   /* Show the focus: keys moved it */
  bool     caret;                  /* A field's caret: its blink */
  bool     tipshown;
  bool     scrolls;                /* It has a scrolling box */
  uint8_t  buttons;
  int16_t  px;                     /* The pointer */
  int16_t  py;
  int16_t  grab;                   /* Pointer within a thumb or knob */
  uint32_t hotms;                  /* When hot last changed */
  uint32_t clickms;                /* Double clicks */
  int16_t  clickpart;
  uint8_t  clickwg;
  struct kit_rect_s area;          /* What the kit lays out */
  struct kit_rect_s damage;        /* To draw again (w == 0: nothing) */
  struct kit_rect_s drawn;         /* Drawn since the last flush */
  struct kit_rect_s tiprect;
  struct ui_keys_s keys;
  struct kit_popup_s pop;
  struct kit_s *dlg;               /* A dialog over the window, or NULL */
  struct kit_s *parent;            /* A dialog's window's kit */
  void    *dlgstate;               /* The dialog's own (desk_kit_dialog.c) */

  struct
  {
    int16_t key;
    uint8_t id;
  } sc[KIT_MAX_SHORTCUTS];         /* Keyboard shortcuts */

  kit_event_t event;
  kit_draw_t draw;                 /* Custom widgets and icons */
  void    *priv;                   /* The application's */

  /* What an event says besides its widget */

  int      arg;
  int      key;                    /* KIT_EV_KEY */
  int16_t  mx;                     /* KIT_EV_MOUSE */
  int16_t  my;
  uint8_t  mbuttons;
  const char *answer;              /* KIT_EV_DIALOG: its field, or the
                                    * path chosen */
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Setting up: wg is the application's array of max widgets; event hears
 * what happens (may be NULL)
 */

void kit_init(struct kit_s *k, struct desk_win_s *w,
              struct kit_widget_s *wg, int max, kit_event_t event);

/* Building, in order: a box's widgets follow it until kit_end().  Each
 * returns its widget (so its fields can be set), or a spare if the array
 * is full.  The first widget should be a box: the root.
 */

struct kit_widget_s *kit_row(struct kit_s *k, int style);
struct kit_widget_s *kit_col(struct kit_s *k, int style);
void kit_end(struct kit_s *k);
struct kit_widget_s *kit_add(struct kit_s *k, int type, int id,
                             const char *text);
struct kit_widget_s *kit_label(struct kit_s *k, int id, const char *text,
                               int style);
struct kit_widget_s *kit_button(struct kit_s *k, int id, const char *text,
                                int icon, int style);
struct kit_widget_s *kit_check(struct kit_s *k, int id, const char *text,
                               bool on);
struct kit_widget_s *kit_radio(struct kit_s *k, int id, const char *text,
                               bool on);
struct kit_widget_s *kit_switch(struct kit_s *k, int id, bool on);
struct kit_widget_s *kit_slider(struct kit_s *k, int id, int value,
                                int min, int max);
struct kit_widget_s *kit_progress(struct kit_s *k, int id, int value,
                                  int max);
struct kit_widget_s *kit_field(struct kit_s *k, int id,
                               struct ui_field_s *f,
                               const char *placeholder);
struct kit_widget_s *kit_dropdown(struct kit_s *k, int id,
                                  const char * const *items, int n,
                                  int value);
struct kit_widget_s *kit_list(struct kit_s *k, int id,
                              struct kit_list_s *l, int style);
struct kit_widget_s *kit_tabs(struct kit_s *k, int id,
                              const char * const *items, int n, int value,
                              int style);
struct kit_widget_s *kit_scroll(struct kit_s *k, int id);
struct kit_widget_s *kit_crumbs(struct kit_s *k, int id,
                                const char * const *items, int n);
struct kit_widget_s *kit_separator(struct kit_s *k);
struct kit_widget_s *kit_spacer(struct kit_s *k, int grow);
struct kit_widget_s *kit_custom(struct kit_s *k, int id);
struct kit_widget_s *kit_stepper(struct kit_s *k, int id,
                                 const char *value);
struct kit_widget_s *kit_swatches(struct kit_s *k, int id,
                                  const uint16_t *colours, int n,
                                  int value);
struct kit_widget_s *kit_image(struct kit_s *k, int id,
                               struct kit_surface_s *s);

/* The last widget added: grow, size wanted, tooltip */

void kit_grow(struct kit_s *k, int grow);
void kit_size(struct kit_s *k, int w, int h);
void kit_tip(struct kit_s *k, const char *tip);

/* A card of rows of settings (a column, hairlines between its rows) */

struct kit_widget_s *kit_card(struct kit_s *k);

/* A row of a card of settings (KIT_BOX_CARD with KIT_F_LINES): the label
 * (and its caption after a '\n') at the left; then kit_end() after the
 * control(s) at the right
 */

struct kit_widget_s *kit_card_row(struct kit_s *k, int id,
                                  const char *label);

/* For a draw function: the kit's list of operations, with room for n
 * more.  What goes into it is cut to what the function was given.
 */

struct desk_ops_s *kit_ops(struct kit_s *k, int n);

/* How a widget is to look now (KIT_LOOK_*, with KIT_LOOK_FOCUS): for the
 * application drawing a custom one
 */

int  kit_look(struct kit_s *k, struct kit_widget_s *wg);

/* A key (ui_keys()'s, e.g. KEY_CTL('s')) that clicks widget id */

void kit_shortcut(struct kit_s *k, int key, int id);

/* Finding widgets: by id (the first of that id), NULL if none */

struct kit_widget_s *kit_get(struct kit_s *k, int id);

/* Changing them: each marks what has to be drawn again */

void kit_set_text(struct kit_s *k, int id, const char *text);
void kit_set_value(struct kit_s *k, int id, int value);
void kit_set_items(struct kit_s *k, int id, const char * const *items,
                   int n);
void kit_enable(struct kit_s *k, int id, bool enabled);
void kit_show(struct kit_s *k, int id, bool shown);
void kit_set_style(struct kit_s *k, int id, int style);
void kit_scroll_set(struct kit_s *k, int id, int pos, int page, int total);
void kit_box_scroll(struct kit_s *k, int id, int offset);  /* A scrolling
                                                            * box's */
void kit_focus(struct kit_s *k, int id);
void kit_select_all(struct kit_s *k, int id);      /* A field's text */
void kit_invalidate(struct kit_s *k, int id);      /* Draw it again */
void kit_damage(struct kit_s *k, int x, int y, int w, int h);

/* Lists */

void kit_list_changed(struct kit_s *k, int id);    /* Rows, sort: all */
void kit_list_row(struct kit_s *k, int id, int row);
void kit_list_select(struct kit_s *k, int id, int row);
void kit_list_show(struct kit_s *k, int id, int row);  /* Scroll to it */
void kit_list_scroll(struct kit_s *k, int id, int top);
int  kit_list_visible(struct kit_s *k, int id);    /* Whole rows shown */
int  kit_list_at(struct kit_s *k, int id, int x, int y);
                                                   /* The row at a point
                                                    * (frame coordinates),
                                                    * or -1 */

/* Laying out: all of the client area, or the rectangle given (a kit for
 * part of a window); then everything is drawn at the next flush
 */

void kit_layout(struct kit_s *k);
void kit_place(struct kit_s *k, int x, int y, int w, int h);

/* The size the widgets would like (the root's natural size) */

void kit_natural(struct kit_s *k, int *w, int *h);

/* Drawing what is marked; kit_redraw(): everything */

void kit_flush(struct kit_s *k);
void kit_redraw(struct kit_s *k);

/* The application drew over [x, y, w, h) itself: menus, tooltips and
 * dialogs over it are drawn again
 */

void kit_touched(struct kit_s *k, int x, int y, int w, int h);

/* The window's events.  kit_mouse() takes client coordinates as the
 * application's mouse function has them, and returns true if the
 * pointer was on (or held by) the kit's widgets.  kit_key() returns true
 * if every key was taken (the rest went to the event function as
 * KIT_EV_KEY).
 */

bool kit_mouse(struct kit_s *k, int x, int y, uint8_t buttons);
bool kit_key(struct kit_s *k, const uint8_t *ch, int n);

/* Keys one at a time, for an application that looks at them first:
 * kit_keys() decodes the bytes (as ui_keys()), kit_press_key() gives the
 * kit one key (true if it took it), then kit_flush()
 */

int  kit_keys(struct kit_s *k, const uint8_t *ch, int n, int *keys,
              int max);
bool kit_press_key(struct kit_s *k, int key);
void kit_tick(struct kit_s *k);                    /* Once a second */
void kit_expose(struct kit_s *k, int x, int y, int w, int h);
void kit_resize(struct kit_s *k);                  /* w->csize changed */
void kit_activate(struct kit_s *k);                /* w->active changed */

/* Menus: items ("-" a separator), at (x, y) in frame coordinates, kept
 * in the window; KIT_EV_MENU with k->arg the item chosen (wg NULL,
 * k->key the menu's id)
 */

void kit_menu(struct kit_s *k, int id, int x, int y,
              const char * const *items, int n, uint32_t disabled);
void kit_menu_close(struct kit_s *k);

/* Dialogs, over the window (desk_kit_dialog.c).  buttons: the default
 * one (Enter), then the one Escape presses, then one at the left, or
 * NULL for OK.  text: for a prompt the field's first text; for OPEN and
 * SAVE the folder (or file) to start in.  The answer comes as
 * KIT_EV_DIALOG, wg NULL, k->key the dialog's id, k->arg KIT_ANS_*,
 * k->answer the text or path.
 */

void kit_dialog(struct kit_s *k, int id, int kind, const char *title,
                const char *text, const char * const *buttons, int n);
void kit_dialog_close(struct kit_s *k);
bool kit_dialog_open(struct kit_s *k);
void kit_dialog_reap(void);                        /* The kit's: frees
                                                    * closed dialogs when
                                                    * it is done with an
                                                    * event (never from
                                                    * an event function) */

/* Surfaces (desk_kit_surface.c).  kit_surface_open() returns 0 (in the
 * GPU's memory), 1 (in memory: slow) or a negated errno.  Draw into one
 * with a list of operations sent by kit_surface_draw(); draw from one
 * into a window's list with kit_surface_copy() or _blend() (mode: an
 * NX_BLEND_SRC_* image source, with its modifiers).
 */

int  kit_surface_open(struct kit_surface_s *s, int w, int h,
                      struct desk_win_s *owner);
void kit_surface_close(struct kit_surface_s *s);
void kit_surface_draw(struct kit_surface_s *s, struct desk_ops_s *o);
void kit_surface_copy(struct desk_ops_s *o, struct kit_surface_s *s,
                      int sx, int sy, int w, int h, int dx, int dy);
void kit_surface_blend(struct desk_ops_s *o, struct kit_surface_s *s,
                       int sx, int sy, int w, int h, int dx, int dy,
                       uint8_t mode, uint16_t colour, uint8_t alpha);

/* Painting controls where the caller wants them (desk_kit_paint.c): the
 * kit's widgets are drawn with these, and anything that must look like
 * them can use them too.  look: KIT_LOOK_*.
 */

void kit_paint_button(struct desk_ops_s *o, int x, int y, int w, int h,
                      const char *label, int icon, int style, int look,
                      bool on);
void kit_paint_check(struct desk_ops_s *o, int x, int y, bool on, int look);
void kit_paint_radio(struct desk_ops_s *o, int x, int y, bool on, int look);
void kit_paint_switch(struct desk_ops_s *o, int x, int y, bool on,
                      int look);
void kit_paint_slider(struct desk_ops_s *o, int x, int y, int w, int v,
                      int min, int max, int look);
int  kit_slider_value(int x, int w, int min, int max, int px);
void kit_paint_progress(struct desk_ops_s *o, int x, int y, int w, int v,
                        int min, int max);
void kit_paint_field(struct desk_ops_s *o, struct ui_field_s *f, int x,
                     int y, int w, int h, const char *placeholder,
                     bool focus, bool caret, bool disabled, bool selected);
int  kit_field_pos(struct ui_field_s *f, int x, int w, int px);
void kit_paint_dropdown(struct desk_ops_s *o, int x, int y, int w, int h,
                        const char *label, int look);
void kit_paint_ring(struct desk_ops_s *o, int x, int y, int w, int h,
                    int r, uint16_t ground);
void kit_paint_ring_round(struct desk_ops_s *o, int x, int y, int d,
                          uint16_t ground);
void kit_paint_icon(struct desk_ops_s *o, int icon, int x, int y,
                    uint16_t c, uint16_t bg);
void kit_paint_thumb(struct desk_ops_s *o, int x, int y, int h, bool hot,
                     bool drag);
void kit_paint_thumb_across(struct desk_ops_s *o, int x, int y, int w,
                            bool hot, bool drag);
void kit_paint_tooltip(struct desk_ops_s *o, int x, int y, int w, int h,
                       const char *text);
void kit_paint_stepper(struct desk_ops_s *o, int x, int y, int w,
                       const char *value, int hot);
int  kit_stepper_at(int x, int w, int px);
void kit_paint_swatch(struct desk_ops_s *o, int cx, int cy, uint16_t c,
                      bool sel, bool hot);
void kit_paint_segments(struct desk_ops_s *o, int x, int y, int w, int h,
                        const char * const *labels, int n, int sel,
                        int hot, bool pressed, bool disabled);
int  kit_segment_at(int x, int w, int n, int px);
void kit_paint_card(struct desk_ops_s *o, int x, int y, int w, int h);
void kit_paint_card_lines(struct desk_ops_s *o, int x, int y, int w);
void kit_paint_row_label(struct desk_ops_s *o, int x, int y, int h,
                         const char *label, const char *caption);
void kit_paint_sidebar_row(struct desk_ops_s *o, int x, int y, int w,
                           int h, const char *label, uint16_t tile,
                           bool sel, bool hot);
void kit_paint_meter(struct desk_ops_s *o, int x, int y, int w, int h,
                     int peak);
int  kit_paint_speaker(struct desk_ops_s *o, int x, int y, int s, int waves,
                       bool muted, uint16_t c, uint8_t alpha);
int  kit_speaker_waves(int volume);
int  kit_tooltip_size(const char *text, int *h);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_KIT_H */
