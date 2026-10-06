/****************************************************************************
 * apps/games/NXDoom/src/i_fpcsound.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * DESCRIPTION:
 *  Doom's sound effects through the FPC's audio mixer
 *  (nuttx/audio/fpc_mixer.h): its 8-bit lumps mixed here into 11025 Hz
 *  stereo 16-bit and written to a mixer channel named "Doom".  A thread
 *  keeps the channel about 60 ms ahead of what the mixer has played, so a
 *  sound starts soon after the game asks for it.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/fpc_mixer.h>

#include "doomtype.h"
#include "i_sound.h"
#include "w_wad.h"
#include "z_zone.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define FPC_RATE        11025
#define FPC_NCHAN       16              /* Doom asks for up to 8 (snd_channels) */
#define FPC_AHEAD       662             /* Frames kept written ahead: 60 ms */
#define FPC_CHUNK       256
#define FPC_TICK_US     10000

/****************************************************************************
 * Private Types
 ****************************************************************************/

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

/****************************************************************************
 * Private Function Prototypes
 ****************************************************************************/

static boolean i_fpc_init_sound(gamemission_t mission);
static void i_fpc_shutdown_sound(void);
static int i_fpc_get_sfx_lumpnum(sfxinfo_t *sfx);
static void i_fpc_update_sound(void);
static void i_fpc_update_sound_params(int channel, int vol, int sep);
static int i_fpc_start_sound(sfxinfo_t *sfxinfo, int channel, int vol,
                             int sep, int pitch);
static void i_fpc_stop_sound(int channel);
static boolean i_fpc_sound_is_playing(int channel);

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const snddevice_t g_fpc_devices[] =
{
  SNDDEVICE_SB,
  SNDDEVICE_PAS,
  SNDDEVICE_GUS,
  SNDDEVICE_WAVEBLASTER,
  SNDDEVICE_SOUNDCANVAS,
  SNDDEVICE_AWE32,
};

static struct
{
  pthread_mutex_t lock;
  pthread_t thread;
  bool      running;
  volatile bool quit;
  int       fd;                          /* The channel */
  int       mixfd;                       /* /dev/mixer */
  int       pcm;                         /* Which /dev/audio/pcmN */
  uint32_t  written;                     /* Frames written */
  uint32_t  f0;                          /* The channel's count at start */
  struct fpc_voice_s voice[FPC_NCHAN];
  struct fpcmix_info_s info;
  int16_t   buf[FPC_CHUNK * 2];
} g_fpc;

/****************************************************************************
 * Public Data
 ****************************************************************************/

const sound_module_t sound_fpc_module =
{
  .sound_devices       = g_fpc_devices,
  .num_sound_devices   = sizeof(g_fpc_devices) / sizeof(g_fpc_devices[0]),
  .init                = i_fpc_init_sound,
  .shutdown            = i_fpc_shutdown_sound,
  .get_sfx_lumpnum     = i_fpc_get_sfx_lumpnum,
  .update              = i_fpc_update_sound,
  .update_sound_params = i_fpc_update_sound_params,
  .start_sound         = i_fpc_start_sound,
  .stop_sound          = i_fpc_stop_sound,
  .sound_is_playing    = i_fpc_sound_is_playing,
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* A free channel of the mixer, named "Doom": 11025 Hz stereo 16-bit */

static int fpc_open(void)
{
  struct audio_caps_desc_s desc;
  char path[32];
  int fd;
  int i;

  for (i = 0; i < FPCMIX_NCHAN_MAX; i++)
    {
      snprintf(path, sizeof(path), FPCMIX_PCMPATH, i);
      fd = open(path, O_RDWR | O_CLOEXEC);
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
          ioctl(fd, MIXIOC_SETNAME, (unsigned long)"Doom") < 0)
        {
          close(fd);
          continue;
        }

      memset(&desc, 0, sizeof(desc));
      desc.caps.ac_len            = sizeof(struct audio_caps_s);
      desc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
      desc.caps.ac_subtype        = AUDIO_FMT_PCM;
      desc.caps.ac_channels       = 2;
      desc.caps.ac_controls.hw[0] = FPC_RATE;
      desc.caps.ac_controls.b[2]  = 16;
      if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&desc) < 0)
        {
          close(fd);
          return -1;
        }

      g_fpc.pcm = i;
      return fd;
    }

  return -1;
}

/* The frames the mixer has played of the channel */

static uint32_t fpc_played(void)
{
  if (ioctl(g_fpc.mixfd, MIXIOC_GETINFO, (unsigned long)&g_fpc.info) < 0)
    {
      return g_fpc.written + g_fpc.f0;     /* As if all: write on */
    }

  return g_fpc.info.chan[g_fpc.pcm].frames;
}

/* n frames of every voice, mixed */

static void fpc_mix(int n)
{
  int16_t *out = g_fpc.buf;
  int i;
  int k;

  memset(out, 0, n * 4);
  pthread_mutex_lock(&g_fpc.lock);
  for (k = 0; k < FPC_NCHAN; k++)
    {
      struct fpc_voice_s *v = &g_fpc.voice[k];

      for (i = 0; v->on && i < n; i++)
        {
          uint32_t at = v->pos >> 16;
          int s;
          int l;
          int r;

          if (at >= v->len)
            {
              v->on = false;
              break;
            }

          s = ((int)v->data[at] - 128) << 8;
          l = out[2 * i] + s * v->left / 254;
          r = out[2 * i + 1] + s * v->right / 254;
          out[2 * i]     = l > 32767 ? 32767 : l < -32768 ? -32768 : l;
          out[2 * i + 1] = r > 32767 ? 32767 : r < -32768 ? -32768 : r;
          v->pos += v->step;
        }
    }

  pthread_mutex_unlock(&g_fpc.lock);
}

/* Keeps the channel FPC_AHEAD frames ahead of the mixer, writing silence
 * when nothing sounds: one continuous stream
 */

static void *fpc_thread(void *arg)
{
  while (!g_fpc.quit)
    {
      int32_t ahead = (int32_t)(g_fpc.written - (fpc_played() - g_fpc.f0));

      while (ahead < FPC_AHEAD && !g_fpc.quit)
        {
          int n = FPC_AHEAD - ahead > FPC_CHUNK ? FPC_CHUNK :
                  FPC_AHEAD - ahead;

          fpc_mix(n);
          if (write(g_fpc.fd, g_fpc.buf, n * 4) != n * 4)
            {
              break;
            }

          g_fpc.written += n;
          ahead += n;
        }

      usleep(FPC_TICK_US);
    }

  return NULL;
}

static boolean i_fpc_init_sound(gamemission_t mission)
{
  memset(&g_fpc, 0, sizeof(g_fpc));
  g_fpc.mixfd = open(FPCMIX_DEVPATH, O_RDONLY | O_CLOEXEC);
  if (g_fpc.mixfd < 0)
    {
      return false;
    }

  g_fpc.fd = fpc_open();
  if (g_fpc.fd < 0)
    {
      close(g_fpc.mixfd);
      return false;
    }

  pthread_mutex_init(&g_fpc.lock, NULL);
  g_fpc.f0      = fpc_played();
  g_fpc.running = pthread_create(&g_fpc.thread, NULL, fpc_thread, NULL) ==
                  0;
  if (!g_fpc.running)
    {
      close(g_fpc.fd);
      close(g_fpc.mixfd);
      return false;
    }

  pthread_setname_np(g_fpc.thread, "doom sound");
  printf("i_init_sound: FPC mixer, %d Hz stereo (/dev/audio/pcm%d)\n",
         FPC_RATE, g_fpc.pcm);
  return true;
}

static void i_fpc_shutdown_sound(void)
{
  if (g_fpc.running)
    {
      g_fpc.quit = true;
      ioctl(g_fpc.fd, MIXIOC_FLUSH, 0);          /* Wakes a waiting write */
      pthread_join(g_fpc.thread, NULL);
      close(g_fpc.fd);
      close(g_fpc.mixfd);
      pthread_mutex_destroy(&g_fpc.lock);
      g_fpc.running = false;
    }
}

static int i_fpc_get_sfx_lumpnum(sfxinfo_t *sfx)
{
  char namebuf[16];

  if (sfx->link != NULL)
    {
      sfx = sfx->link;
    }

  snprintf(namebuf, sizeof(namebuf), "ds%s", sfx->name);
  return w_check_num_for_name(namebuf);
}

static void i_fpc_update_sound(void)
{
}

/* Doom's volume 0..127 and separation 0..254 (128 the middle) as the two
 * sides' gains
 */

static void fpc_gains(struct fpc_voice_s *v, int vol, int sep)
{
  v->left  = (254 - sep) * vol / 127;
  v->right = sep * vol / 127;
  v->left  = v->left > 254 ? 254 : v->left;
  v->right = v->right > 254 ? 254 : v->right;
}

static void i_fpc_update_sound_params(int channel, int vol, int sep)
{
  if (channel >= 0 && channel < FPC_NCHAN)
    {
      pthread_mutex_lock(&g_fpc.lock);
      fpc_gains(&g_fpc.voice[channel], vol, sep);
      pthread_mutex_unlock(&g_fpc.lock);
    }
}

/* A sound lump (DMX): format 3, the rate, the length, then the samples
 * with 16 bytes of padding either end
 */

static int i_fpc_start_sound(sfxinfo_t *sfxinfo, int channel, int vol,
                             int sep, int pitch)
{
  struct fpc_voice_s *v;
  const uint8_t *data;
  uint32_t rate;
  uint32_t len;
  int lump;
  int size;

  if (channel < 0 || channel >= FPC_NCHAN || sfxinfo->lumpnum < 0)
    {
      return -1;
    }

  lump = sfxinfo->lumpnum;
  size = w_lump_length(lump);
  data = w_cache_lump_num(lump, PU_STATIC);      /* Other channels may share it */
  if (size < 8 || data[0] != 3 || data[1] != 0)
    {
      return -1;
    }

  rate = data[2] | data[3] << 8;
  len  = data[4] | data[5] << 8 | data[6] << 16 | (uint32_t)data[7] << 24;
  if (len > (uint32_t)size - 8 || len <= 48 || rate == 0)
    {
      return -1;
    }

  v = &g_fpc.voice[channel];
  pthread_mutex_lock(&g_fpc.lock);
  v->data = data + 8 + 16;
  v->len  = len - 32;
  v->pos  = 0;
  v->step = (uint32_t)(((uint64_t)rate << 16) / FPC_RATE);
  fpc_gains(v, vol, sep);
  v->on   = true;
  pthread_mutex_unlock(&g_fpc.lock);
  return channel;
}

static void i_fpc_stop_sound(int channel)
{
  if (channel >= 0 && channel < FPC_NCHAN)
    {
      pthread_mutex_lock(&g_fpc.lock);
      g_fpc.voice[channel].on = false;
      pthread_mutex_unlock(&g_fpc.lock);
    }
}

static boolean i_fpc_sound_is_playing(int channel)
{
  return channel >= 0 && channel < FPC_NCHAN && g_fpc.voice[channel].on;
}
