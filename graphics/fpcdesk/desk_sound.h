/****************************************************************************
 * apps/graphics/fpcdesk/desk_sound.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The desktop's sound (CONFIG_FPCDESK_SOUND): its system sounds, the
 * mixer's state and volumes (/dev/mixer, nuttx/audio/fpc_mixer.h), the
 * settings kept across boots; and what shows them:
 *
 *   desk_volume.c     the speaker in the top bar and its popover
 *   desk_settings.c   the Sound page of Settings (SETTINGS_SOUND)
 *   desk_music.c      the Music application
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCDESK_DESK_SOUND_H
#define __APPS_GRAPHICS_FPCDESK_DESK_SOUND_H

#include <stdio.h>

#include <nuttx/audio/fpc_mixer.h>
#include <nuttx/nx/nxkeys.h>

#include "desk.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

/* The name the system sounds play under: their level is the alert volume */

#define DESK_SOUND_ALERTS    "Alerts"

/* System sounds */

#define SND_OPEN             0          /* A window opens */
#define SND_CLOSE            1          /* ... closes */
#define SND_MINIMIZE         2          /* ... goes to the dock */
#define SND_ALERT            3          /* A question, an error */
#define SND_NOTIFY           4          /* Something has happened */
#define SND_VOLUME           5          /* The volume was set: how loud */
#define SND_ALARM            6          /* An alarm rings (a chime) */
#define SND_TIMER            7          /* The timer has ended */
#define SND_COUNT            8

/****************************************************************************
 * Public Function Prototypes
 ****************************************************************************/

/* desk_sound.c.  All on the desktop's main thread, except where said. */

int  desk_sound_init(void);                  /* After the panels */
void desk_sound_play(int snd);               /* Any thread */
void desk_sound_changed(void);               /* EV_SOUND: the mixer's state
                                              * has changed */
bool desk_sound_key(const struct nx_key_s *key);  /* A volume key: true */

/* The mixer's state as last read (desk_sound_refresh() reads it again);
 * NULL without a mixer
 */

const struct fpcmix_info_s *desk_sound_info(void);
const struct fpcmix_info_s *desk_sound_refresh(void);

void desk_sound_master(int volume);          /* 0..1000 */
void desk_sound_mute(int how);               /* 0, 1, 2: toggle */
void desk_sound_level(const char *name, int level);
int  desk_sound_getlevel(const char *name);  /* 1000 if never set */
int  desk_sound_levels(struct fpcmix_levels_s *ls);

/* Interface sounds (window open, close): on or off */

bool desk_sound_effects(void);
void desk_sound_set_effects(bool on);

/* Settings: a line of the settings file (true if it was ours), and ours
 * into it
 */

void desk_sound_prefs_line(const char *line);
void desk_sound_prefs_write(FILE *f);

/* desk_volume.c: the top bar's speaker and its popover.  Hooks:
 *   desk_topbar_draw():   right = desk_volume_topbar(o, right, ty) - gap;
 *   desk_topbar_mouse():  desk_volume_topbar_mouse(x, y, b) (true: the
 *                         pointer is on the speaker, not the rest)
 *   on_mouse():           desk_volume_mouse_any(part, buttons) first, and
 *                         case PART_SOUND: desk_volume_popover_mouse();
 *   animate():            more |= desk_volume_animate();
 *   desk_panels_relayout(): desk_volume_relayout() (it closes; the theme
 *                         or the display has changed)
 *   the mixer changed:    desk_volume_changed()   (from desk_sound_changed)
 */

int  desk_volume_topbar(struct desk_ops_s *o, int right, int ty);
bool desk_volume_topbar_mouse(int x, int y, uint8_t buttons);
void desk_volume_popover_mouse(int x, int y, uint8_t buttons);
void desk_volume_mouse_any(int part, uint8_t buttons);
bool desk_volume_animate(void);
void desk_volume_changed(bool bykey);
void desk_volume_relayout(void);

#endif /* __APPS_GRAPHICS_FPCDESK_DESK_SOUND_H */
