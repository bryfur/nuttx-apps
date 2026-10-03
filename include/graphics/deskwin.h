/****************************************************************************
 * apps/include/graphics/deskwin.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Licensed to the Apache Software Foundation (ASF) under one or more
 * contributor license agreements.  See the NOTICE file distributed with
 * this work for additional information regarding copyright ownership.  The
 * ASF licenses this file to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance with the
 * License.  You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS, WITHOUT
 * WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.  See the
 * License for the specific language governing permissions and limitations
 * under the License.
 *
 ****************************************************************************/

#ifndef __APPS_INCLUDE_GRAPHICS_DESKWIN_H
#define __APPS_INCLUDE_GRAPHICS_DESKWIN_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* Desktop windows for other tasks (e.g. MicroPython's fpc module).  The
 * program never talks to the NX server: it queues drawing commands and
 * widget changes that the desktop's main thread carries out at
 * deskwin_flush(), and reads events with deskwin_event().  A slow or
 * crashed program can't hold the desktop up; its windows close when its
 * task ends.
 *
 * Coordinates are the client area's, colours RGB565.  The desktop replays
 * what was drawn since the last deskwin_clear() when the window's pixels
 * are lost; past DESKWIN_KEEP bytes it sends DESKWIN_EV_EXPOSE instead.
 * Widgets are the desktop's kit (docs/uikit.md), ids 1 to DESKWIN_WIDGETS.
 *
 * Only the task that opened the window may use it (deskwin_text_width()
 * and _height(): any task).
 */

/* Event types */

#define DESKWIN_EV_MOUSE      1        /* x, y, buttons (x -1: it left) */
#define DESKWIN_EV_KEY        2        /* key, ch, mods, kind: a press or
                                        * a repeat (DESKWIN_KEY_REPEAT) */
#define DESKWIN_EV_KEYUP      3        /* key, mods: a release */
#define DESKWIN_EV_RESIZE     4        /* w, h: the client area's size */
#define DESKWIN_EV_CLOSE      5        /* Closed (its button, the dock):
                                        * the window has gone */
#define DESKWIN_EV_EXPOSE     6        /* x, y, w, h: draw it again */
#define DESKWIN_EV_FRAME      7        /* value: ms (deskwin_frames()) */
#define DESKWIN_EV_TICK       8        /* Once a second */
#define DESKWIN_EV_FOCUS      9        /* value: 1 active, 0 not */
#define DESKWIN_EV_WIDGET     10       /* id, kind, value */
#define DESKWIN_EV_WAKE       11       /* deskwin_wake() (the client's) */
#define DESKWIN_EV_BUTTON     12       /* The title bar's button
                                        * (deskwin_button()) */

/* DESKWIN_EV_WIDGET kinds */

#define DESKWIN_CLICK         1        /* A button */
#define DESKWIN_CHANGE        2        /* A value (a slider dragged, text
                                        * typed, a row selected ...) */
#define DESKWIN_ACTIVATE      3        /* Enter in a field, a row double
                                        * clicked, a slider let go */

#define DESKWIN_KEY_REPEAT    1        /* DESKWIN_EV_KEY kind */

/* Mouse buttons */

#define DESKWIN_LEFT          0x01
#define DESKWIN_CENTRE        0x02
#define DESKWIN_RIGHT         0x04

/* Fonts: the desktop's */

#define DESKWIN_FONT_BODY     0        /* Regular 13 */
#define DESKWIN_FONT_BOLD     1        /* Medium 13 */
#define DESKWIN_FONT_SMALL    2        /* Regular 11 */
#define DESKWIN_FONT_HEADING  3        /* Medium 20 */
#define DESKWIN_FONT_DISPLAY  4        /* Light 44 */
#define DESKWIN_FONT_MONO     5        /* IBM Plex Mono 15 */
#define DESKWIN_FONT_MONOBOLD 6
#define DESKWIN_FONTS         7

/* Widget types */

#define DESKWIN_BUTTON        1        /* text; style: 0 normal, 1 default,
                                        * 2 danger, 3 flat, 4 toggle */
#define DESKWIN_LABEL         2        /* text; style: DESKWIN_FONT_*, +8
                                        * dim, +16 centred, +32 right */
#define DESKWIN_FIELD         3        /* text (placeholder: items) */
#define DESKWIN_SLIDER        4        /* value, min, max */
#define DESKWIN_CHECK         5        /* text, value */
#define DESKWIN_SWITCH        6        /* value */
#define DESKWIN_PROGRESS      7        /* value, max */
#define DESKWIN_LIST          8        /* items; value: the selected row */
#define DESKWIN_DROPDOWN      9        /* items; value: the item chosen */

/* deskwin_widget_set(): what */

#define DESKWIN_SET_TEXT      1
#define DESKWIN_SET_VALUE     2
#define DESKWIN_SET_ITEMS     3        /* Lines, "\n" between */
#define DESKWIN_SET_ENABLED   4        /* value */
#define DESKWIN_SET_VISIBLE   5        /* value */
#define DESKWIN_SET_RANGE     6        /* value: min, max: the text "min max" */
#define DESKWIN_SET_FOCUS     7

#define DESKWIN_WIDGETS       64       /* Ids 1 to this */
#define DESKWIN_IMAGES        16       /* Images a window at once */
#define DESKWIN_KEEP          (48 * 1024)

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct deskwin_s;

struct deskwin_event_s
{
  uint8_t  type;                       /* DESKWIN_EV_* */
  uint8_t  buttons;                    /* MOUSE: held (DESKWIN_LEFT ...) */
  uint8_t  mods;                       /* KEY: nxkeys.h NX_KEY_SHIFT ... */
  uint8_t  id;                         /* WIDGET: which */
  uint8_t  kind;                       /* WIDGET: DESKWIN_CLICK ...;
                                        * KEY: DESKWIN_KEY_REPEAT */
  uint8_t  key;                        /* KEY: the key (nxkeys.h: the
                                        * character, or NX_KEY_SPECIAL()) */
  uint8_t  ch;                         /* KEY: what it types, or 0 */
  int16_t  x;
  int16_t  y;
  int16_t  w;
  int16_t  h;
  int32_t  value;
};

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

#ifdef __cplusplus
#define EXTERN extern "C"
extern "C"
{
#else
#define EXTERN extern
#endif

/* A window with a client area of w x h (made to fit the display), shown
 * and active: NULL if there is no desktop (errno ENODEV) or no memory.
 */

FAR struct deskwin_s *deskwin_open(FAR const char *title, int w, int h);

/* The same, the window one of an application of the desktop's (its enum
 * desk_app_e: its name and icon in the dock), for an application built as
 * a program of its own (graphics/fpcdesk/desk_program.c); -1: none
 */

FAR struct deskwin_s *deskwin_open_app(int app, FAR const char *title,
                                       int w, int h);

/* Close it (if the user has not) and free d */

void deskwin_close(FAR struct deskwin_s *d);

/* Not closed by the user or the desktop */

bool deskwin_alive(FAR struct deskwin_s *d);

/* The client area's size now */

void deskwin_size(FAR struct deskwin_s *d, FAR int *w, FAR int *h);

/* The next event: 1, or 0 if none came in timeout ms (0: do not wait,
 * -1: wait until one comes); -EPIPE once the window has gone and its
 * events have been read.
 */

int deskwin_event(FAR struct deskwin_s *d, FAR struct deskwin_event_s *ev,
                  int timeout);

/* Drawing: queued until deskwin_flush() (or a full queue).  alpha 255 is
 * opaque; radius rounds the corners.
 */

void deskwin_fill(FAR struct deskwin_s *d, int x, int y, int w, int h,
                  uint16_t c);
void deskwin_rect(FAR struct deskwin_s *d, int x, int y, int w, int h,
                  uint16_t c, int alpha, int radius);
void deskwin_gradient(FAR struct deskwin_s *d, int x, int y, int w, int h,
                      uint16_t c0, uint16_t c1, int alpha, bool vertical);
void deskwin_line(FAR struct deskwin_s *d, int x0, int y0, int x1, int y1,
                  uint16_t c, int alpha);
void deskwin_text(FAR struct deskwin_s *d, int x, int y, int font,
                  uint16_t c, int alpha, FAR const char *s);
void deskwin_image(FAR struct deskwin_s *d, int img, int sx, int sy,
                   int w, int h, int dx, int dy, int alpha);
void deskwin_copy(FAR struct deskwin_s *d, int sx, int sy, int w, int h,
                  int dx, int dy);
void deskwin_clip(FAR struct deskwin_s *d, int x, int y, int w, int h);
void deskwin_clear(FAR struct deskwin_s *d, uint16_t c);

/* The desktop's own drawing operations (n struct nx_gpuop_s,
 * nuttx/nx/nxcompose.h), in frame coordinates (the client area is at y
 * TITLE_H): what an application of the desktop's draws, run as a program
 * (desk_program.c).  A source other than the font atlas or the window's
 * own images is not drawn.
 */

void deskwin_ops(FAR struct deskwin_s *d, FAR const void *ops, int n);

/* Wake the client's deskwin_event() (DESKWIN_EV_WAKE): from another of
 * its threads, which has something for it
 */

void deskwin_wake(FAR struct deskwin_s *d);

/* The same into a surface of the window's (deskwin_surface_open()) */

void deskwin_ops_to(FAR struct deskwin_s *d, FAR void *target,
                    FAR const void *ops, int n);

/* A hidden surface of w x h in the GPU's memory for the window's program
 * to draw into (deskwin_ops_to()) and copy from (an operation's source):
 * NULL if there is no room.  *lost is set when the desktop had to let its
 * pixels go (draw it again).  Closed with the window at the latest.
 */

FAR void *deskwin_surface_open(FAR struct deskwin_s *d, int w, int h,
                               FAR volatile bool *lost);
void deskwin_surface_close(FAR struct deskwin_s *d, FAR void *surface);

/* Hand the queue to the desktop: 0, -ETIMEDOUT if it had no room for
 * timeout ms (-1: wait), -EPIPE if the window has gone.
 */

int deskwin_flush(FAR struct deskwin_s *d, int timeout);

/* The window */

void deskwin_title(FAR struct deskwin_s *d, FAR const char *title);

/* The window to the front (restored if minimised), active */

void deskwin_raise(FAR struct deskwin_s *d);

/* A button in the title bar, beside the title (a label of a few
 * characters, NULL: none): DESKWIN_EV_BUTTON when it is clicked
 */

void deskwin_button(FAR struct deskwin_s *d, FAR const char *label);
void deskwin_frames(FAR struct deskwin_s *d, bool on);

/* Load an image file (BMP, PPM, PGM, PBM) into the GPU's memory, scaled
 * to fit maxw x maxh (0: no limit).  Returns its number (< DESKWIN_IMAGES)
 * or a negated errno, with the reason in err.  Waits for the desktop.
 */

int deskwin_image_load(FAR struct deskwin_s *d, FAR const char *path,
                       int maxw, int maxh, FAR int *w, FAR int *h,
                       FAR char *err, int errlen);
void deskwin_image_free(FAR struct deskwin_s *d, int img);

/* Widgets: (re)made at (x, y), w x h (0: natural size); items: list and
 * drop-down rows ("\n" between), or a field's placeholder
 */

void deskwin_widget(FAR struct deskwin_s *d, int id, int type, int x, int y,
                    int w, int h, FAR const char *text,
                    FAR const char *items, int value, int min, int max,
                    int style);
void deskwin_widget_set(FAR struct deskwin_s *d, int id, int what,
                        int value, FAR const char *text);
void deskwin_widget_move(FAR struct deskwin_s *d, int id, int x, int y,
                         int w, int h);
void deskwin_widget_remove(FAR struct deskwin_s *d, int id);

/* Its value and text now: false if there is no such widget */

bool deskwin_widget_get(FAR struct deskwin_s *d, int id, FAR int *value,
                        FAR char *text, int textlen);

/* Its natural size, as the kit lays it out (0 x 0 if unknown yet) */

void deskwin_widget_size(FAR struct deskwin_s *d, int id, FAR int *w,
                         FAR int *h);

/* Text: its width in a font, the font's line height */

int deskwin_text_width(int font, FAR const char *s);
int deskwin_text_height(int font);

/* The theme's colour of a role ("window_bg", "text", "accent" ...): -1 if
 * there is none of that name
 */

int deskwin_colour(FAR const char *role);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __APPS_INCLUDE_GRAPHICS_DESKWIN_H */
