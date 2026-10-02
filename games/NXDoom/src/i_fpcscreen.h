/****************************************************************************
 * apps/games/NXDoom/src/i_fpcscreen.h
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Doom full screen on the FPC's GPU (i_fpcscreen.c)
 ****************************************************************************/

#ifndef __APPS_GAMES_NXDOOM_SRC_I_FPCSCREEN_H
#define __APPS_GAMES_NXDOOM_SRC_I_FPCSCREEN_H

#include <stdbool.h>
#include <stdint.h>

struct nx_key_s;

bool fpcscreen_open(void);
void fpcscreen_frame(const uint8_t *pixels, const uint8_t *rgbpal);
bool fpcscreen_key(struct nx_key_s *key);
bool fpcscreen_active(void);
void fpcscreen_close(void);

#endif /* __APPS_GAMES_NXDOOM_SRC_I_FPCSCREEN_H */
