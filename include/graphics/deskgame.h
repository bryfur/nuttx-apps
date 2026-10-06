/****************************************************************************
 * apps/include/graphics/deskgame.h
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

#ifndef __APPS_INCLUDE_GRAPHICS_DESKGAME_H
#define __APPS_INCLUDE_GRAPHICS_DESKGAME_H

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdbool.h>
#include <stdint.h>

#include <nuttx/nx/nxkeys.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* A game in a window of the FPC desktop (apps/graphics/fpcdesk).  The
 * desktop owns the window and gets the pixels to the GPU; the game is a
 * task it starts with DESKGAME_ARG and the session's address in argv.
 * The game attaches, hands over 8-bit frames with a 256-colour palette
 * (the desktop scales them by whole numbers and sends only what changed),
 * calls deskgame_wait() once a tic (held while the window is inactive,
 * -1 when it was closed), reads keys and mouse with deskgame_input()
 * (DESKGAME_FOCUS: release held keys), and detaches when it ends.
 */

#define DESKGAME_ARG          "-deskgame"

/* deskgame_input_s type */

#define DESKGAME_KEY          1        /* key */
#define DESKGAME_MOUSE        2        /* dx, dy, buttons */
#define DESKGAME_FOCUS        3        /* The window is not active now */

/* deskgame_input_s buttons (NX's) */

#define DESKGAME_LEFT         0x01
#define DESKGAME_CENTRE       0x02
#define DESKGAME_RIGHT        0x04

/****************************************************************************
 * Public Types
 ****************************************************************************/

struct deskgame_s;

struct deskgame_input_s
{
  uint8_t type;                        /* DESKGAME_* */
  uint8_t buttons;                     /* DESKGAME_MOUSE: held now */
  int16_t dx;                          /* DESKGAME_MOUSE: moved (pixels of */
  int16_t dy;                          /* the frame) since the last one */
  struct nx_key_s key;                 /* DESKGAME_KEY */
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

/****************************************************************************
 * Name: deskgame_attach
 *
 * Description:
 *   The session the desktop started this task for (DESKGAME_ARG in its
 *   arguments), with frames of width x height pixels; NULL if it was not
 *   started by the desktop.
 *
 ****************************************************************************/

FAR struct deskgame_s *deskgame_attach(int argc, FAR char *argv[],
                                       int width, int height);

/****************************************************************************
 * Name: deskgame_frame
 *
 * Description:
 *   A frame of width x height palette indices and the palette (256 x RGB,
 *   NULL: unchanged).  Both are copied; the desktop draws the latest.
 *
 ****************************************************************************/

void deskgame_frame(FAR struct deskgame_s *g, FAR const uint8_t *pixels,
                    FAR const uint8_t *palette);

/****************************************************************************
 * Name: deskgame_input
 *
 * Description:
 *   The next input event: false if there is none.
 *
 ****************************************************************************/

bool deskgame_input(FAR struct deskgame_s *g,
                    FAR struct deskgame_input_s *in);

/****************************************************************************
 * Name: deskgame_wait
 *
 * Description:
 *   Wait while the window is not active or is minimised.
 *
 * Returned Value:
 *   How long it waited, ms (0: not at all); -1: the window has been
 *   closed, quit.
 *
 ****************************************************************************/

int deskgame_wait(FAR struct deskgame_s *g);

/****************************************************************************
 * Name: deskgame_status
 *
 * Description:
 *   A line of text for the window, shown until the first frame.
 *
 ****************************************************************************/

void deskgame_status(FAR struct deskgame_s *g, FAR const char *text);

/****************************************************************************
 * Name: deskgame_detach
 *
 * Description:
 *   The game has ended: error is shown in the window, NULL closes it.
 *
 ****************************************************************************/

void deskgame_detach(FAR struct deskgame_s *g, FAR const char *error);

#undef EXTERN
#ifdef __cplusplus
}
#endif

#endif /* __APPS_INCLUDE_GRAPHICS_DESKGAME_H */
