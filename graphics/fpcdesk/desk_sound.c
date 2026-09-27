/****************************************************************************
 * apps/graphics/fpcdesk/desk_sound.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The desktop's sound (desk_sound.h):
 *
 *   - system sounds, made here (short sine partials with a quick attack,
 *     an exponential decay and a glide), played by a thread of their own
 *     through a channel of the mixer named "Alerts" (whose level is the
 *     alert volume): written, not waited for; the channel is let go after
 *     a few seconds of quiet;
 *   - the mixer's state: a thread waits on /dev/mixer (poll) and tells the
 *     main thread (EV_SOUND) when anything has changed, a volume key, a
 *     stream starting or ending, so the speaker, its popover and Settings
 *     follow;
 *   - the volumes, and the settings kept across boots (the master volume,
 *     mute, the application levels, interface sounds on or off).
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <nuttx/audio/audio.h>

#include "desk_sound.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SND_RATE        48000
#define SND_QUEUE       8
#define SND_IDLE_S      3                /* Let the channel go after this */
#define SND_CHUNK       512
#define SINE_N          1024

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A partial: a sine gliding from f0 to f1 Hz, starting at ms, lasting len
 * ms, of peak amp (of 32767), decaying with time constant tau ms, with a
 * second harmonic h2 percent as loud
 */

struct snd_note_s
{
  uint16_t f0;
  uint16_t f1;
  uint16_t at;
  uint16_t len;
  uint16_t amp;
  uint16_t tau;
  uint8_t  h2;
};

struct snd_def_s
{
  uint8_t n;
  struct snd_note_s note[3];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct snd_def_s g_sounds[SND_COUNT] =
{
  /* SND_OPEN: a soft rising tick */

  { 1, { { 700, 1150, 0, 50, 6000, 14, 20 } } },

  /* SND_CLOSE: falling */

  { 1, { { 1100, 640, 0, 50, 5500, 14, 20 } } },

  /* SND_MINIMIZE: falling, longer, softer */

  { 1, { { 900, 480, 0, 80, 4500, 22, 10 } } },

  /* SND_ALERT: two bell notes down a fourth (D5, A4) */

  {
    2,
    {
      { 587, 587, 0, 220, 12000, 70, 30 },
      { 440, 440, 110, 420, 13000, 110, 30 },
    }
  },

  /* SND_NOTIFY: a rising arpeggio (C6, E6, G6) */

  {
    3,
    {
      { 1047, 1047, 0, 360, 7000, 110, 15 },
      { 1319, 1319, 70, 360, 7000, 110, 15 },
      { 1568, 1568, 140, 420, 7500, 130, 15 },
    }
  },

  /* SND_VOLUME: a short pop, at the new volume */

  { 1, { { 880, 880, 0, 70, 12000, 18, 25 } } },
};

static struct
{
  bool     up;
  int      fd;                           /* /dev/mixer, the main thread's */
  struct fpcmix_info_s info;
  bool     have;                         /* info is valid */
  uint16_t master;                       /* What the desktop last set */
  bool     mute;
  bool     effects;                      /* Interface sounds */
  bool     dirty;                        /* Settings to save */

  /* The settings file's, until the mixer has them */

  int      pmaster;
  int      pmute;
  int      nplevels;
  struct fpcmix_level_s plevels[FPCMIX_LEVELS_MAX];

  /* The player thread's queue */

  pthread_mutex_t lock;
  sem_t    count;
  int      head;
  int      n;
  uint8_t  q[SND_QUEUE];

  /* The watcher: posted and not yet seen by the main thread */

  volatile bool posted;
  float    sine[SINE_N + 1];
} g_snd =
{
  .fd      = -1,
  .effects = true,
  .pmaster = -1,
  .pmute   = -1,
};

/****************************************************************************
 * Private Functions: making sounds
 ****************************************************************************/

/* sin(2 pi i / SINE_N), without libm: a polynomial on [-pi/2, pi/2] */

static void sine_init(void)
{
  const double pi = 3.14159265358979323846;
  int i;

  for (i = 0; i <= SINE_N; i++)
    {
      double x = 2 * pi * i / SINE_N;
      double x2;
      int k = (int)(x / pi + 0.5);

      x -= k * pi;
      x2 = x * x;
      x = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72))));
      g_snd.sine[i] = (float)((k & 1) ? -x : x);
    }
}

static float sine(uint32_t phase)
{
  uint32_t i = phase >> (32 - 10);
  float f = (float)(phase & ((1u << 22) - 1)) / (float)(1u << 22);

  return g_snd.sine[i] + (g_snd.sine[i + 1] - g_snd.sine[i]) * f;
}

/* n samples (from sample t0) of a sound, 48 kHz mono */

static void snd_render(const struct snd_def_s *d, int t0, int16_t *out,
                       int n)
{
  int i;
  int k;

  for (i = 0; i < n; i++)
    {
      int t = t0 + i;
      float v = 0;

      for (k = 0; k < d->n; k++)
        {
          const struct snd_note_s *nt = &d->note[k];
          int s0 = nt->at * (SND_RATE / 1000);
          int len = nt->len * (SND_RATE / 1000);
          int u = t - s0;
          float env;
          float f;
          float ph;
          float ph2;

          if (u < 0 || u >= len)
            {
              continue;
            }

          /* 2 ms attack, the decay, 6 ms of release at the end.  The
           * decay is e^-(u / tau) as (1 + x/16)^16, x = -u / tau
           */

          env = u < 96 ? u / 96.0f : 1.0f;
          {
            float x = 1.0f - (float)u / (nt->tau * (SND_RATE / 1000)) / 16;
            float e = x > 0 ? x : 0;

            e *= e;
            e *= e;
            e *= e;
            e *= e;
            env *= e;
          }

          if (len - u < 288)
            {
              env *= (len - u) / 288.0f;
            }

          /* The phase of a glide: the integral of a linear frequency */

          f  = nt->f0 + (nt->f1 - nt->f0) * (float)u / (2 * len);
          ph = f * u / SND_RATE;
          ph -= (int)ph;
          ph2 = 2 * ph;
          ph2 -= (int)ph2;
          v += nt->amp * env * (sine((uint32_t)(ph * 4294967040.0f)) +
                                nt->h2 / 100.0f *
                                sine((uint32_t)(ph2 * 4294967040.0f)));
        }

      v = v > 32767 ? 32767 : v < -32768 ? -32768 : v;
      out[i] = (int16_t)v;
    }
}

static int snd_length(const struct snd_def_s *d)
{
  int len = 0;
  int k;

  for (k = 0; k < d->n; k++)
    {
      int end = (d->note[k].at + d->note[k].len) * (SND_RATE / 1000);

      len = end > len ? end : len;
    }

  return len;
}

/* A free channel of the mixer for the system sounds: 48 kHz mono */

static int snd_open(void)
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
          ioctl(fd, MIXIOC_SETNAME, (unsigned long)DESK_SOUND_ALERTS) < 0)
        {
          close(fd);
          continue;
        }

      memset(&desc, 0, sizeof(desc));
      desc.caps.ac_len            = sizeof(struct audio_caps_s);
      desc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
      desc.caps.ac_subtype        = AUDIO_FMT_PCM;
      desc.caps.ac_channels       = 1;
      desc.caps.ac_controls.hw[0] = SND_RATE;
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

/* The player: sounds from the queue into the channel, which it keeps
 * while sounds come and lets go after SND_IDLE_S of quiet (what was
 * written plays out after the close)
 */

static void *snd_player(void *arg)
{
  int16_t buf[SND_CHUNK];
  int fd = -1;

  for (; ; )
    {
      const struct snd_def_s *d;
      struct timespec ts;
      int len;
      int t;
      int snd;

      clock_gettime(CLOCK_REALTIME, &ts);
      ts.tv_sec += SND_IDLE_S;
      if (sem_timedwait(&g_snd.count, &ts) < 0)
        {
          if (errno == ETIMEDOUT && fd >= 0)
            {
              close(fd);
              fd = -1;
            }

          continue;
        }

      pthread_mutex_lock(&g_snd.lock);
      snd = g_snd.q[g_snd.head];
      g_snd.head = (g_snd.head + 1) % SND_QUEUE;
      g_snd.n--;
      pthread_mutex_unlock(&g_snd.lock);

      if (fd < 0)
        {
          fd = snd_open();
          if (fd < 0)
            {
              continue;                    /* No free channel: not heard */
            }
        }

      d   = &g_sounds[snd];
      len = snd_length(d);
      for (t = 0; t < len; t += SND_CHUNK)
        {
          int n = len - t < SND_CHUNK ? len - t : SND_CHUNK;

          snd_render(d, t, buf, n);
          if (write(fd, buf, n * 2) < 0)
            {
              close(fd);
              fd = -1;
              break;
            }
        }
    }

  return NULL;
}

/* The watcher: the main thread hears (EV_SOUND) of every change of the
 * mixer's state; one event at a time, however many changes come before
 * it is seen
 */

static void *snd_watcher(void *arg)
{
  struct fpcmix_info_s *info = malloc(sizeof(*info));
  struct pollfd pfd;

  pfd.fd     = open(FPCMIX_DEVPATH, O_RDONLY | O_CLOEXEC);
  pfd.events = POLLIN;
  if (pfd.fd < 0 || info == NULL)
    {
      free(info);
      return NULL;
    }

  for (; ; )
    {
      if (poll(&pfd, 1, -1) <= 0)
        {
          continue;
        }

      /* Read it: that is what makes this opener's poll quiet again */

      ioctl(pfd.fd, MIXIOC_GETINFO, (unsigned long)info);
      if (!g_snd.posted)
        {
          g_snd.posted = true;
          desk_post(EV_SOUND);
        }

      usleep(20000);                       /* A drag: at most 50 a second */
    }

  return NULL;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int desk_sound_init(void)
{
  pthread_attr_t attr;
  pthread_t thread;
  int i;

  g_snd.fd = open(FPCMIX_DEVPATH, O_RDONLY | O_CLOEXEC);
  if (g_snd.fd < 0)
    {
      return -ENODEV;
    }

  sine_init();
  pthread_mutex_init(&g_snd.lock, NULL);
  sem_init(&g_snd.count, 0, 0);

  /* What the settings file said */

  if (g_snd.pmaster >= 0)
    {
      ioctl(g_snd.fd, MIXIOC_SETMASTER, g_snd.pmaster);
    }

  if (g_snd.pmute >= 0)
    {
      ioctl(g_snd.fd, MIXIOC_SETMUTE, g_snd.pmute);
    }

  for (i = 0; i < g_snd.nplevels; i++)
    {
      ioctl(g_snd.fd, MIXIOC_SETLEVEL, (unsigned long)&g_snd.plevels[i]);
    }

  desk_sound_refresh();
  g_snd.master = g_snd.info.master;
  g_snd.mute   = g_snd.info.mute;
  g_snd.up     = true;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 3072);
  pthread_create(&thread, &attr, snd_player, NULL);
  pthread_setname_np(thread, "desk sounds");
  pthread_attr_setstacksize(&attr, 2048);
  pthread_create(&thread, &attr, snd_watcher, NULL);
  pthread_setname_np(thread, "desk mixer");
  return 0;
}

void desk_sound_play(int snd)
{
  if (!g_snd.up || snd < 0 || snd >= SND_COUNT ||
      (!g_snd.effects && snd <= SND_MINIMIZE))
    {
      return;
    }

  pthread_mutex_lock(&g_snd.lock);
  if (g_snd.n < SND_QUEUE)
    {
      g_snd.q[(g_snd.head + g_snd.n) % SND_QUEUE] = (uint8_t)snd;
      g_snd.n++;
      sem_post(&g_snd.count);
    }

  pthread_mutex_unlock(&g_snd.lock);
}

const struct fpcmix_info_s *desk_sound_info(void)
{
  return g_snd.have ? &g_snd.info : NULL;
}

const struct fpcmix_info_s *desk_sound_refresh(void)
{
  g_snd.have = g_snd.fd >= 0 &&
               ioctl(g_snd.fd, MIXIOC_GETINFO,
                     (unsigned long)&g_snd.info) >= 0;
  return desk_sound_info();
}

/* EV_SOUND: what has changed, shown; a volume key's change (not the
 * desktop's own) shows the popover for a moment, and is saved
 */

void desk_sound_changed(void)
{
  bool bykey;

  g_snd.posted = false;
  if (desk_sound_refresh() == NULL)
    {
      return;
    }

  bykey = g_snd.info.master != g_snd.master ||
          g_snd.info.mute != g_snd.mute;
  if (bykey)
    {
      g_snd.master = g_snd.info.master;
      g_snd.mute   = g_snd.info.mute;
      desk_prefs_save();
    }

  desk_volume_changed(bykey);
  desk_soundpage_changed();
}

void desk_sound_master(int volume)
{
  g_snd.master = volume;
  g_snd.mute   = false;
  ioctl(g_snd.fd, MIXIOC_SETMASTER, volume);
  if (g_snd.info.mute)
    {
      ioctl(g_snd.fd, MIXIOC_SETMUTE, 0);
    }

  desk_sound_refresh();
}

void desk_sound_mute(int how)
{
  g_snd.mute = how == 2 ? !g_snd.info.mute : how != 0;
  ioctl(g_snd.fd, MIXIOC_SETMUTE, g_snd.mute ? 1 : 0);
  desk_sound_refresh();
}

void desk_sound_level(const char *name, int level)
{
  struct fpcmix_level_s l;

  memset(&l, 0, sizeof(l));
  strlcpy(l.name, name, sizeof(l.name));
  l.level = level;
  ioctl(g_snd.fd, MIXIOC_SETLEVEL, (unsigned long)&l);
  desk_sound_refresh();
}

int desk_sound_levels(struct fpcmix_levels_s *ls)
{
  ls->n = 0;
  return g_snd.fd >= 0 ? ioctl(g_snd.fd, MIXIOC_GETLEVELS,
                               (unsigned long)ls) : -ENODEV;
}

int desk_sound_getlevel(const char *name)
{
  struct fpcmix_levels_s ls;
  int i;

  if (desk_sound_levels(&ls) >= 0)
    {
      for (i = 0; i < ls.n; i++)
        {
          if (strcmp(ls.l[i].name, name) == 0)
            {
              return ls.l[i].level;
            }
        }
    }

  return FPCMIX_VOLUME_MAX;
}

bool desk_sound_effects(void)
{
  return g_snd.effects;
}

void desk_sound_set_effects(bool on)
{
  g_snd.effects = on;
}

/* Settings: "sound master N", "sound mute 0|1", "sound effects 0|1",
 * "sound level NAME N"
 */

bool desk_sound_prefs_line(const char *line)
{
  char name[FPCMIX_NAME_MAX];
  int n;

  if (strncmp(line, "sound ", 6) != 0)
    {
      return false;
    }

  if (sscanf(line, "sound master %d", &n) == 1)
    {
      g_snd.pmaster = n;
    }
  else if (sscanf(line, "sound mute %d", &n) == 1)
    {
      g_snd.pmute = n != 0;
    }
  else if (sscanf(line, "sound effects %d", &n) == 1)
    {
      g_snd.effects = n != 0;
    }
  else if (sscanf(line, "sound level %15s %d", name, &n) == 2 &&
           g_snd.nplevels < FPCMIX_LEVELS_MAX)
    {
      strlcpy(g_snd.plevels[g_snd.nplevels].name, name, FPCMIX_NAME_MAX);
      g_snd.plevels[g_snd.nplevels++].level = n;
    }

  return true;
}

void desk_sound_prefs_write(FILE *f)
{
  struct fpcmix_levels_s ls;
  int i;

  if (!g_snd.up)
    {
      /* Not started yet: what the file said, as it was */

      if (g_snd.pmaster >= 0)
        {
          fprintf(f, "sound master %d\n", g_snd.pmaster);
        }

      if (g_snd.pmute >= 0)
        {
          fprintf(f, "sound mute %d\n", g_snd.pmute);
        }

      fprintf(f, "sound effects %d\n", g_snd.effects);
      for (i = 0; i < g_snd.nplevels; i++)
        {
          fprintf(f, "sound level %s %d\n", g_snd.plevels[i].name,
                  g_snd.plevels[i].level);
        }

      return;
    }

  desk_sound_refresh();
  fprintf(f, "sound master %d\nsound mute %d\nsound effects %d\n",
          g_snd.info.master, g_snd.info.mute, g_snd.effects);
  if (desk_sound_levels(&ls) >= 0)
    {
      for (i = 0; i < ls.n; i++)
        {
          /* Names with spaces do not fit the format: not kept */

          if (strchr(ls.l[i].name, ' ') == NULL &&
              ls.l[i].level != FPCMIX_VOLUME_MAX)
            {
              fprintf(f, "sound level %s %d\n", ls.l[i].name,
                      ls.l[i].level);
            }
        }
    }
}
