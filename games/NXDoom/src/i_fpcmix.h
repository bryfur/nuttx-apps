/****************************************************************************
 * apps/games/NXDoom/src/i_fpcmix.h
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DESCRIPTION:
 *  The inner loop of Doom's sound effects (i_fpcsound.c): a voice's 8-bit
 *  samples, at its pitch and its two sides' gains, added to stereo 16-bit
 *  frames with saturation.  On the RP2350 the frame is one word and the
 *  Cortex-M33's QADD16 adds both sides saturating in one instruction;
 *  elsewhere (the simulator) the same in C, bit for bit (tests/dsp checks
 *  both against the loop this replaced).  A header of its own so that the
 *  test builds this very function.
 *
 ****************************************************************************/

#ifndef __APPS_GAMES_NXDOOM_SRC_I_FPCMIX_H
#define __APPS_GAMES_NXDOOM_SRC_I_FPCMIX_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __ARM_FEATURE_DSP
#  include <arm_acle.h>
#endif

struct fpc_voice_s
{
  const uint8_t *data;                  /* 8-bit unsigned samples */
  uint32_t len;                         /* ... how many */
  uint32_t pos;                         /* 16.16 */
  uint32_t step;                        /* 16.16: its rate / FPC_RATE */
  int      left;                        /* Gains, 0..254 */
  int      right;
  bool     on;
};

/* n frames of the voice added to out (left in the low half); false (and
 * where it stopped kept) if it ends first
 */

static inline bool fpc_mix_voice(struct fpc_voice_s *v, uint32_t *out,
                                 int n)
{
  const uint8_t *data = v->data;
  uint32_t len = v->len;
  uint32_t pos = v->pos;
  uint32_t step = v->step;
  int left = v->left;
  int right = v->right;
  int i;

  for (i = 0; i < n; i++)
    {
      uint32_t at = pos >> 16;
      int s;
      int l;
      int r;

      if (at >= len)
        {
          v->pos = pos;
          return false;
        }

      /* Each side's sample in 16 bits (s * 254 / 254 at most), added
       * saturating
       */

      s = ((int)data[at] - 128) << 8;
      l = s * left / 254;
      r = s * right / 254;
#ifdef __ARM_FEATURE_DSP
      __asm__("pkhbt %0, %0, %1, lsl #16" : "+r"(l) : "r"(r));
      out[i] = (uint32_t)__qadd16((int16x2_t)out[i], (int16x2_t)l);
#else
      l += (int16_t)out[i];
      r += (int16_t)(out[i] >> 16);
      l  = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
      r  = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
      out[i] = (uint16_t)l | (uint32_t)(uint16_t)r << 16;
#endif
      pos += step;
    }

  v->pos = pos;
  return true;
}

#endif /* __APPS_GAMES_NXDOOM_SRC_I_FPCMIX_H */
