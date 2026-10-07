/****************************************************************************
 * apps/graphics/fpcgame/fg_audio.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The game SDK's sound: TRI's engine (fg_audio_engine.c: sixteen voices,
 * PCM samples and two-operator FM, 48 kHz stereo), fed TRI's sound
 * commands (TRI_OP_AUDIO_*: the calls below make them, a TRI game makes
 * its own), rendered by a thread of its own into a channel of the
 * desktop's mixer (drivers/audio/fpc_audio.c, /dev/audio/pcmN: it blocks
 * on write() while the channel's FIFO is full, which paces it), and a
 * sequencer for music, run once a frame from the game's loop.
 *
 * The engine finds a sample's data through VDP2's VRAM map
 * (vdp2_vram_range(), _base()): here an offset names one of the game's
 * samples (fg_sample(): its id + 1 in bits 31:24, the byte in 23:0), or,
 * for a TRI game, a place in its VDP2 VRAM (fg_audio_vram()).  The engine
 * runs under a lock: commands from the game's task, rendering from the
 * thread.
 ****************************************************************************/

#include "fg_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/fpc_mixer.h>

#include "fg_audio_engine.h"
#include "fpc_gpu.h"
#include "tri/tri_isa.h"

#define FG_BLOCK        256               /* Frames rendered at a time (5.3 ms) */
#define FG_AUDIO_STACK  4096

/* The sound thread's stack in the SRAM with the engine (fg_audio_engine.h:
 * its mix buffer and spills, every frame), not in the PSRAM
 */

#if defined(__arm__)
static uint64_t g_au_stack[FG_AUDIO_STACK / 8] FG_AUDIO_SRAM;
#endif

static struct
{
  pthread_mutex_t lock;
  pthread_t thread;
  bool running;
  bool stop;
  int fd;
  FAR const int8_t *data[FG_SAMPLES];     /* fg_sample()'s */
  uint32_t len[FG_SAMPLES];
  FAR const uint8_t *(*xbase)(uint32_t off);   /* fg_audio_vram()'s */
  bool (*xrange)(uint32_t off, uint32_t len);

  /* The sequencer */

  FAR const struct fg_song_s *song;
  int pos;
  uint32_t frame;
  uint16_t off_at[FG_VOICES];
} g_au =
{
  .lock = PTHREAD_MUTEX_INITIALIZER,
  .fd   = -1,
};

/****************************************************************************
 * The engine's surroundings
 ****************************************************************************/

uint32_t time_us_32(void)
{
  return fpc_gpu_game_us();             /* (the audio thread's: not fg_now_us()) */
}

uint8_t *vdp2_vram_range(vdp2_t *v, uint32_t off, uint32_t len)
{
  uint32_t id;
  uint32_t at;

  if (g_au.xrange != NULL)
    {
      return g_au.xrange(off, len) ? (uint8_t *)g_au.xbase(off) : NULL;
    }

  id = (off >> 24) - 1;
  at = off & 0xffffff;
  if (id >= FG_SAMPLES || g_au.data[id] == NULL || at + len > g_au.len[id])
    {
      return NULL;
    }

  return (uint8_t *)(g_au.data[id] + at);
}

const uint8_t *vdp2_vram_base(const vdp2_t *v, uint32_t off)
{
  if (g_au.xbase != NULL)
    {
      return g_au.xbase(off);
    }

  return (const uint8_t *)g_au.data[(off >> 24) - 1] + (off & 0xffffff);
}

void fg_audio_vram(FAR const uint8_t *(*base)(uint32_t off),
                   bool (*range)(uint32_t off, uint32_t len))
{
  pthread_mutex_lock(&g_au.lock);
  g_au.xbase  = base;
  g_au.xrange = range;
  pthread_mutex_unlock(&g_au.lock);
}

/****************************************************************************
 * Commands
 ****************************************************************************/

void fg_audio_cmd(uint32_t op, uint32_t imm, FAR const uint32_t *p,
                  uint32_t nw)
{
  pthread_mutex_lock(&g_au.lock);
  vdp2_audio_exec(op, imm, p, nw);
  pthread_mutex_unlock(&g_au.lock);
}

static uint32_t vol2(int l, int r)
{
  l = l < 0 ? 0 : l > 255 ? 255 : l;
  r = r < 0 ? 0 : r > 255 ? 255 : r;
  return (uint32_t)l | (uint32_t)r << 8;
}

int fg_sample(int id, FAR const int8_t *data, uint32_t samples,
              uint32_t loop_start, uint32_t rate_hz)
{
  uint32_t p[4];

  if (id < 0 || id >= FG_SAMPLES || data == NULL || samples == 0 ||
      samples > 0xffffff)
    {
      return -EINVAL;
    }

  pthread_mutex_lock(&g_au.lock);
  g_au.data[id] = data;
  g_au.len[id]  = samples;
  pthread_mutex_unlock(&g_au.lock);
  p[0] = (uint32_t)(id + 1) << 24;
  p[1] = samples;
  p[2] = loop_start;
  p[3] = rate_hz;
  fg_audio_cmd(TRI_OP_AUDIO_SAMPLE_DEF, id, p, 4);
  return OK;
}

void fg_sfx_play(int voice, int sample, uint32_t pitch16, int vol_l, int vol_r)
{
  uint32_t p[2] = { pitch16, vol2(vol_l, vol_r) };

  if (voice >= 0 && voice < FG_VOICES && sample >= 0 && sample < FG_SAMPLES)
    {
      fg_audio_cmd(TRI_OP_AUDIO_PLAY, voice | sample << 4, p, 2);
    }
}

void fg_sfx_stop(int voice)
{
  if (voice >= 0 && voice < FG_VOICES)
    {
      fg_audio_cmd(TRI_OP_AUDIO_STOP, voice, NULL, 0);
    }
}

void fg_voice_volume(int voice, int vol_l, int vol_r)
{
  uint32_t p = vol2(vol_l, vol_r);

  if (voice >= 0 && voice < FG_VOICES)
    {
      fg_audio_cmd(TRI_OP_AUDIO_VOL, voice, &p, 1);
    }
}

void fg_master_volume(int vol_l, int vol_r)
{
  uint32_t p = vol2(vol_l, vol_r);

  fg_audio_cmd(TRI_OP_AUDIO_VOL, 0xffff, &p, 1);
}

void fg_patch(int id, FAR const struct fg_patch_s *pt)
{
  uint32_t p[5];

  if (id < 0 || id >= FG_PATCHES)
    {
      return;
    }

  p[0] = (uint32_t)(pt->alg & 1) | (uint32_t)(pt->feedback & 7) << 4;
  p[1] = TRI_FM_OP_W0(pt->op[0].mult, pt->op[0].detune, pt->op[0].wave,
                      pt->op[0].level);
  p[2] = TRI_FM_OP_W1(pt->op[0].attack, pt->op[0].decay, pt->op[0].sustain,
                      pt->op[0].release);
  p[3] = TRI_FM_OP_W0(pt->op[1].mult, pt->op[1].detune, pt->op[1].wave,
                      pt->op[1].level);
  p[4] = TRI_FM_OP_W1(pt->op[1].attack, pt->op[1].decay, pt->op[1].sustain,
                      pt->op[1].release);
  fg_audio_cmd(TRI_OP_AUDIO_FM_PATCH, id, p, 5);
}

void fg_fm_on(int voice, int patch, uint32_t freq16, int vol_l, int vol_r)
{
  uint32_t p[2] = { freq16, vol2(vol_l, vol_r) };

  if (voice >= 0 && voice < FG_VOICES && patch >= 0 && patch < FG_PATCHES)
    {
      fg_audio_cmd(TRI_OP_AUDIO_FM_ON, voice | patch << 4, p, 2);
    }
}

void fg_fm_off(int voice)
{
  if (voice >= 0 && voice < FG_VOICES)
    {
      fg_audio_cmd(TRI_OP_AUDIO_FM_OFF, voice, NULL, 0);
    }
}

void fg_fm_freq(int voice, uint32_t freq16)
{
  if (voice >= 0 && voice < FG_VOICES)
    {
      fg_audio_cmd(TRI_OP_AUDIO_FM_FREQ, voice, &freq16, 1);
    }
}

/* MIDI note -> Hz, 16.16 (octave 4 tabulated, as TRI's) */

uint32_t fg_note_freq(int n)
{
  static const uint32_t octave4[12] =
  {
    17146147, 18165760, 19245984, 20390656, 21603136, 22887808,
    24248832, 25690624, 27218176, 28836608, 30551552, 32368384
  };

  int oct;
  uint32_t f;

  n = n < 0 ? 0 : n > 119 ? 119 : n;
  oct = n / 12;
  f = octave4[n % 12];
  return oct >= 5 ? f << (oct - 5) : f >> (5 - oct);
}

/****************************************************************************
 * The sequencer (TRI's tri_music_*)
 ****************************************************************************/

static void song_release(void)
{
  int v;

  for (v = 0; v < FG_VOICES; v++)
    {
      if (g_au.off_at[v])
        {
          fg_fm_off(v);
          g_au.off_at[v] = 0;
        }
    }
}

void fg_music_play(FAR const struct fg_song_s *song)
{
  if (song == NULL || song->notes == NULL || song->count <= 0)
    {
      return;
    }

  song_release();
  g_au.song  = song;
  g_au.pos   = 0;
  g_au.frame = 0;
}

void fg_music_stop(void)
{
  song_release();
  g_au.song = NULL;
}

bool fg_music_playing(void)
{
  return g_au.song != NULL;
}

void fg_audio_stop_all(void)
{
  int v;

  fg_music_stop();
  for (v = 0; v < FG_VOICES; v++)
    {
      fg_sfx_stop(v);
    }
}

void fg_audio_frame(void)
{
  FAR const struct fg_song_s *s = g_au.song;
  int v;

  if (s == NULL)
    {
      return;
    }

  while (g_au.pos < s->count && s->notes[g_au.pos].frame <= g_au.frame)
    {
      FAR const struct fg_note_s *n = &s->notes[g_au.pos++];

      fg_fm_on(n->voice & 15, n->patch, fg_note_freq(n->note), n->vel,
               n->vel);
      g_au.off_at[n->voice & 15] = n->len ?
                                   (uint16_t)(g_au.frame + n->len) : 0;
    }

  for (v = 0; v < FG_VOICES; v++)
    {
      if (g_au.off_at[v] && g_au.off_at[v] <= g_au.frame)
        {
          fg_fm_off(v);
          g_au.off_at[v] = 0;
        }
    }

  g_au.frame++;
  if (g_au.frame >= s->length_frames)
    {
      if (s->loop)
        {
          g_au.frame = 0;
          g_au.pos   = 0;
        }
      else if (g_au.pos >= s->count)
        {
          bool any = false;

          for (v = 0; v < FG_VOICES; v++)
            {
              any |= g_au.off_at[v] != 0;
            }

          if (!any)
            {
              g_au.song = NULL;
            }
        }
    }
}

/****************************************************************************
 * The output: a mixer channel, 48 kHz stereo 16-bit
 ****************************************************************************/

static int chan_open(void)
{
  struct audio_caps_desc_s desc;
  char path[32];
  int fd;
  int i;

  for (i = 0; i < FPCMIX_NCHAN_MAX; i++)
    {
      snprintf(path, sizeof(path), FPCMIX_PCMPATH, i);
      fd = open(path, O_RDWR);
      if (fd < 0)
        {
          if (errno == ENOENT)
            {
              break;
            }

          continue;
        }

      memset(&desc, 0, sizeof(desc));
      desc.caps.ac_len     = sizeof(struct audio_caps_s);
      desc.caps.ac_type    = AUDIO_TYPE_QUERY;
      desc.caps.ac_subtype = AUDIO_TYPE_QUERY;
      if (ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)&desc.caps) < 0 ||
          (desc.caps.ac_controls.b[0] & AUDIO_TYPE_OUTPUT) == 0 ||
          ioctl(fd, AUDIOIOC_RESERVE, 0) < 0 ||
          ioctl(fd, MIXIOC_SETNAME,
                (unsigned long)(g_fg.game && g_fg.game->name ?
                                g_fg.game->name : "game")) < 0)
        {
          close(fd);
          continue;
        }

      memset(&desc, 0, sizeof(desc));
      desc.caps.ac_len            = sizeof(struct audio_caps_s);
      desc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
      desc.caps.ac_subtype        = AUDIO_FMT_PCM;
      desc.caps.ac_channels       = 2;
      desc.caps.ac_controls.hw[0] = TRI_AUDIO_RATE & 0xffff;
      desc.caps.ac_controls.b[3]  = TRI_AUDIO_RATE >> 16;
      desc.caps.ac_controls.b[2]  = 16;
      if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&desc) < 0)
        {
          close(fd);
          return -1;
        }

      return fd;
    }

  return -1;
}

static FAR void *audio_thread(FAR void *arg)
{
  static int16_t buf[FG_BLOCK * 2] FG_AUDIO_SRAM;

  while (!g_au.stop)
    {
      FAR const uint8_t *p = (FAR const uint8_t *)buf;
      size_t n = sizeof(buf);

      pthread_mutex_lock(&g_au.lock);
      vdp2_audio_render(buf, FG_BLOCK);
      pthread_mutex_unlock(&g_au.lock);

      while (n > 0 && !g_au.stop)
        {
          ssize_t ret = write(g_au.fd, p, n);

          if (ret <= 0)
            {
              if (ret < 0 && errno == EINTR)
                {
                  continue;
                }

              g_au.stop = true;
              break;
            }

          p += ret;
          n -= ret;
        }
    }

  return NULL;
}

int fg_audio_start(void)
{
  pthread_attr_t attr;
  struct sched_param sp;

  memset(g_au.data, 0, sizeof(g_au.data));
  memset(g_au.off_at, 0, sizeof(g_au.off_at));
  g_au.xbase  = NULL;
  g_au.xrange = NULL;
  g_au.song   = NULL;
  vdp2_audio_init(NULL);

  g_au.fd = chan_open();
  if (g_au.fd < 0)
    {
      syslog(LOG_WARNING, "fpcgame: no sound (no free mixer channel)\n");
      return -ENODEV;
    }

  g_au.stop = false;
  pthread_attr_init(&attr);
#if defined(__arm__)
  pthread_attr_setstack(&attr, g_au_stack, sizeof(g_au_stack));
#else
  pthread_attr_setstacksize(&attr, FG_AUDIO_STACK);
#endif
  sp.sched_priority = 150;                /* Above the game, below the mixer */
  pthread_attr_setschedparam(&attr, &sp);
  if (pthread_create(&g_au.thread, &attr, audio_thread, NULL) != 0)
    {
      close(g_au.fd);
      g_au.fd = -1;
      return -ENOMEM;
    }

  pthread_setname_np(g_au.thread, "game sound");
  g_au.running = true;
  return OK;
}

void fg_audio_stop(void)
{
  if (g_au.running)
    {
      g_au.stop = true;
      pthread_join(g_au.thread, NULL);
      g_au.running = false;
    }

  if (g_au.fd >= 0)
    {
      close(g_au.fd);                     /* (what is written plays out) */
      g_au.fd = -1;
    }

  g_au.song = NULL;
}
