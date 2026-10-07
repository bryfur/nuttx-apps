/****************************************************************************
 * apps/graphics/fpcgame/fg_audio_engine.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * What TRI's sound engine (fg_audio_engine.c, TRI's firmware/vdp2/audio.c)
 * needs from its surroundings, on the FPC: the Pico SDK's few names, and
 * VDP2's VRAM map, where its samples are (fg_audio.c: the game SDK's own
 * samples, or the TRI games' VDP2 VRAM).  Its interface is TRI's audio.h.
 ****************************************************************************/

#ifndef __APPS_GRAPHICS_FPCGAME_FG_AUDIO_ENGINE_H
#define __APPS_GRAPHICS_FPCGAME_FG_AUDIO_ENGINE_H

#include <stdbool.h>
#include <stdint.h>

/* TRI's "in RAM": the engine's render loops (__not_in_flash_func) and the
 * tables and voices they read every frame (FG_AUDIO_SRAM) in the internal
 * SRAM while the game runs (the board's module loader puts .time_critical*
 * and .sram* sections there), not in the PSRAM behind the XIP cache the
 * game's own code and data thrash.  The simulator: where they are.
 */

#if defined(__arm__)
#  define __not_in_flash_func(f) \
     __attribute__((section(".time_critical.fgaudio"))) f
#  define FG_AUDIO_SRAM __attribute__((section(".sram.fgaudio")))
#else
#  define __not_in_flash_func(f) f
#  define FG_AUDIO_SRAM
#endif

uint32_t time_us_32(void);

typedef struct vdp2_s vdp2_t;             /* (only ever NULL here) */

uint8_t *vdp2_vram_range(vdp2_t *v, uint32_t off, uint32_t len);
const uint8_t *vdp2_vram_base(const vdp2_t *v, uint32_t off);

typedef struct
{
  uint32_t active_voices;
  uint32_t render_us_max;
  uint32_t render_us_sum, render_calls, frames_rendered;
  uint32_t bad_cmds;
} vdp2_audio_stats_t;

void vdp2_audio_init(vdp2_t *vdp);
bool vdp2_audio_exec(uint32_t op, uint32_t imm, const uint32_t *payload,
                     uint32_t nwords);
void vdp2_audio_render(int16_t *lr, int frames);
const vdp2_audio_stats_t *vdp2_audio_stats(void);
void vdp2_audio_stats_reset(void);
void vdp2_audio_reset(void);

#endif /* __APPS_GRAPHICS_FPCGAME_FG_AUDIO_ENGINE_H */
