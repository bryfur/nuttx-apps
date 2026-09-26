/****************************************************************************
 * apps/graphics/fpcdesk/desk_ui.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Small pieces shared by the document applications (Files, Text Editor):
 * clipping lists of operations, buttons, a one-line text field, and the
 * keyboard's bytes decoded into keys.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_UI_H
#define __APPS_GRAPHICS_FPCDESK_DESK_UI_H

#include "desk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Keys: bytes (0-255), then these, with modifiers */

#define KEY_UP          0x100
#define KEY_DOWN        0x101
#define KEY_RIGHT       0x102
#define KEY_LEFT        0x103
#define KEY_HOME        0x104
#define KEY_END         0x105
#define KEY_PGUP        0x106
#define KEY_PGDN        0x107
#define KEY_INS         0x108
#define KEY_DEL         0x109
#define KEY_ESC         0x10a
#define KEY_F2          0x10b
#define KEY_F5          0x10c
#define KEY_MASK        0x0fff
#define KEY_SHIFT       0x1000
#define KEY_CTRL        0x2000

#define KEY_CTL(c)      ((c) & 0x1f)             /* Ctrl-letter: its byte */

/* Button looks */

#define UI_NORMAL       0
#define UI_HOVER        1
#define UI_PRESSED      2
#define UI_DEFAULT      3                        /* Accent */
#define UI_DANGER       4                        /* Red */
#define UI_DISABLED     5

#define UI_FIELD_MAX    256

#define C_UI_FIELD      RGB(0x16, 0x16, 0x1a)
#define C_UI_BUTTON     RGB(0x3a, 0x3a, 0x40)
#define C_UI_HOVER      RGB(0x48, 0x48, 0x50)
#define C_UI_PRESSED    RGB(0x2a, 0x2a, 0x30)
#define C_UI_DANGER     RGB(0xe0, 0x3c, 0x34)
#define C_UI_ERROR      RGB(0xff, 0x7b, 0x72)
#define C_UI_BAR        RGB(0x2a, 0x2a, 0x2e)    /* Tool and status bars */
#define C_UI_LINE       RGB(0x14, 0x14, 0x16)    /* Hairlines between */

/****************************************************************************
 * Public Types
 ****************************************************************************/

/* Decoding the keyboard's bytes: VT100 sequences may come in pieces */

struct ui_keys_s
{
  uint8_t n;                                     /* Bytes of a sequence */
  uint8_t seq[8];
};

/* Areas that react to the pointer: buttons and such, by group (a group
 * is redrawn as a whole when one of its areas changes look)
 */

#define UI_ZONES_MAX    48

struct ui_zone_s
{
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  uint8_t id;
  uint8_t group;
  bool enabled;
};

struct ui_zones_s
{
  struct ui_zone_s z[UI_ZONES_MAX];
  int n;
  int hot;                                       /* Under the pointer, 0 */
  int pressed;                                   /* Pressed, 0 */
};

/* A one-line text field */

struct ui_field_s
{
  char text[UI_FIELD_MAX];
  int len;
  int cur;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* Clip operations from index from onwards to [x0, x1) x [y0, y1) */

void ui_clip(struct desk_ops_s *o, int from, int x0, int y0, int x1,
             int y1);

/* Send the operations if fewer than need slots are left */

void ui_flush(NXWINDOW hwnd, struct desk_ops_s *o, int need);

/* s, or as much of it as fits in maxw followed by an ellipsis, into buf;
 * returns its width
 */

int  ui_fit(const struct desk_font_s *f, const char *s, int maxw, char *buf,
            int size);

/* A button of text (or none: then the caller draws a symbol on it) */

void ui_button(struct desk_ops_s *o, int x, int y, int w, int h,
               const char *label, int look);
int  ui_button_width(const char *label);

/* A small triangle: dir 0 up, 1 down, 2 left, 3 right; (cx, cy) its middle */

void ui_triangle(struct desk_ops_s *o, int cx, int cy, int size, int dir,
                 uint16_t c, uint8_t alpha);

/* Keys: decode n bytes into at most max keys, returns how many */

int  ui_keys(struct ui_keys_s *k, const uint8_t *ch, int n, int *keys,
             int max);

/* Zones: ids are not 0 */

void ui_zones_clear(struct ui_zones_s *zs, int group);
void ui_zone_add(struct ui_zones_s *zs, int group, int id, int x, int y,
                 int w, int h, bool enabled);
struct ui_zone_s *ui_zone_at(struct ui_zones_s *zs, int x, int y);
int  ui_zone_group(struct ui_zones_s *zs, int id);
int  ui_look(struct ui_zones_s *zs, int id, bool enabled);

/* Text fields */

void ui_field_set(struct ui_field_s *f, const char *s);
bool ui_field_key(struct ui_field_s *f, int key);
void ui_field_draw(struct desk_ops_s *o, struct ui_field_s *f, int x, int y,
                   int w, int h, bool focus);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_UI_H */
