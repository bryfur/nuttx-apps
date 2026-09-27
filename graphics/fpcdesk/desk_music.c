/****************************************************************************
 * apps/graphics/fpcdesk/desk_music.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Music: plays the WAV files (and MP3 files, with AUDIOUTILS_LIBMAD) of a
 * folder (CONFIG_FPCDESK_MUSIC_DIR, else the home directory and its Music
 * folder), one after the other.  At the top what is playing, its format,
 * where it is (click the bar to go there); the buttons (previous,
 * play / pause, next, stop) and the application's level; below, the files.
 * Click selects, a double click (or Enter) plays; Space pauses, the
 * arrows move through the list, Left / Right go back or on 10 seconds.
 *
 * A thread of its own plays: it reads (and decodes) the file and write()s
 * the samples into a channel of the mixer named "Music" (any rate, mono
 * or stereo: the mixer converts), which blocks while the channel's FIFO is
 * full; pausing and going to a place are MIXIOC_PAUSE and MIXIOC_FLUSH on
 * the channel.  Where it is comes from the mixer (the frames the channel
 * has played).  The thread wakes the window four times a second while it
 * plays, and when a file ends (the next one starts).
 ****************************************************************************/

#include <nuttx/config.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>

#include <nuttx/audio/audio.h>

#ifdef CONFIG_AUDIOUTILS_LIBMAD
#  ifndef FPM_DEFAULT
#    define FPM_DEFAULT                  /* As libmad is built */
#  endif
#  include <mad.h>
#endif

#include "desk_sound.h"
#include "desk_ui.h"
#include "desk_theme.h"
#include "desk_widgets.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MUSIC_NAME      "Music"          /* The channel's name, its level's */
#define MUSIC_W         560
#define MUSIC_H         440
#define TRACKS_MAX      200
#define HEAD_H          112              /* Now playing */
#define CTRL_H          56               /* The buttons */
#define LIST_Y          (HEAD_H + CTRL_H)
#define ROW_H           26
#define PAD             20
#define BTN             36               /* A transport button */
#define CHUNK           4096

#define KIND_WAV        0
#define KIND_MP3        1

/* Requests to the player */

#define REQ_NONE        0
#define REQ_PLAY        1                /* reqtrack from reqms */
#define REQ_STOP        2
#define REQ_QUIT        3

/* The player's state */

#define ST_IDLE         0
#define ST_PLAYING      1
#define ST_PAUSED       2

/* Buttons (zones) */

#define ID_PREV         1
#define ID_PLAY         2
#define ID_NEXT         3
#define ID_STOP         4

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A slider of desk_widgets' (0..MS_MAX) and what the pointer does with
 * it; y in client coordinates
 */

#define MS_MAX          1000

struct mslider_s
{
  int16_t  x;
  int16_t  y;
  int16_t  w;
  uint16_t value;
  bool     hot;
  bool     drag;
};

struct track_s
{
  char     name[48];
  uint32_t ms;                           /* Its length, 0: not known */
  uint8_t  kind;
};

struct music_s
{
  struct desk_win_s *win;
  char     dir[96];
  int      ntracks;
  struct track_s *tracks;
  int      sel;                          /* Selected in the list */
  int      top;                          /* First row shown */
  uint32_t clickms;
  int      clickrow;
  uint8_t  last;
  struct ui_zones_s zones;
  struct mslider_s volume;
  bool     seeking;                      /* The progress bar is held */
  uint32_t seekms;

  /* The player, and what the window and it share (lock) */

  pthread_t thread;
  bool     running;
  pthread_mutex_t lock;
  sem_t    wake;
  int      req;
  int      reqtrack;
  uint32_t reqms;
  int      fd;                           /* The channel while playing */
  int      pcm;                          /* Which /dev/audio/pcmN */
  int      state;                        /* ST_* */
  int      track;                        /* Playing (or last played) */
  uint32_t rate;
  uint8_t  channels;
  uint8_t  bits;
  uint32_t startms;                      /* Where the channel's count began */
  uint32_t startframes;                  /* ... its frames then */
  uint32_t lenms;
  bool     ended;                        /* A file has played to its end */
  char     err[64];
  uint32_t wakems;                       /* The window last woken */
  int      mixfd;                        /* The player's /dev/mixer */
  struct fpcmix_info_s *mix;             /* ... and what it read */
  uint32_t f0;                           /* The channel's frames at start */
  uint32_t written;                      /* Frames written since */
  int      shownstate;                   /* What the list shows */
  int      showntrack;
  uint32_t decodeus;                     /* Decoding: CPU time, audio time */
  uint32_t audioms;

  struct desk_ops_s ops;
};

/****************************************************************************
 * Private Functions: files
 ****************************************************************************/

static uint32_t le32(const uint8_t *p)
{
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

/* A WAV file's format; the file left at its samples.  Returns the bytes of
 * samples, 0 if it is not one we can play.
 */

static uint32_t wav_open(FILE *f, uint32_t *rate, uint8_t *nch,
                         uint8_t *bits)
{
  uint8_t b[16];
  uint32_t size;

  if (fread(b, 1, 12, f) != 12 || memcmp(b, "RIFF", 4) != 0 ||
      memcmp(b + 8, "WAVE", 4) != 0)
    {
      return 0;
    }

  *rate = 0;
  while (fread(b, 1, 8, f) == 8)
    {
      size = le32(b + 4);
      if (memcmp(b, "data", 4) == 0)
        {
          return *rate != 0 ? size : 0;
        }

      if (memcmp(b, "fmt ", 4) == 0 && size >= 16)
        {
          if (fread(b, 1, 16, f) != 16 || (b[0] | b[1] << 8) != 1)
            {
              return 0;                  /* Not PCM */
            }

          *nch  = b[2];
          *rate = le32(b + 4);
          *bits = b[14];
          size -= 16;
        }

      fseek(f, size + (size & 1), SEEK_CUR);
    }

  return 0;
}

static int kind_of(const char *name)
{
  const char *dot = strrchr(name, '.');

  if (dot != NULL && strcasecmp(dot, ".wav") == 0)
    {
      return KIND_WAV;
    }

#ifdef CONFIG_AUDIOUTILS_LIBMAD
  if (dot != NULL && strcasecmp(dot, ".mp3") == 0)
    {
      return KIND_MP3;
    }
#endif

  return -1;
}

static int track_cmp(const void *a, const void *b)
{
  return strcasecmp(((const struct track_s *)a)->name,
                    ((const struct track_s *)b)->name);
}

/* The folder: CONFIG_FPCDESK_MUSIC_DIR, else home/Music if there is one,
 * else home
 */

static void music_dir(struct music_s *m)
{
  const char *home = getenv("HOME");
  struct stat st;

  if (CONFIG_FPCDESK_MUSIC_DIR[0] != '\0')
    {
      strlcpy(m->dir, CONFIG_FPCDESK_MUSIC_DIR, sizeof(m->dir));
      return;
    }

  home = home != NULL ? home : CONFIG_LIBC_HOMEDIR;
  snprintf(m->dir, sizeof(m->dir), "%s%sMusic", home,
           home[strlen(home) - 1] == '/' ? "" : "/");
  if (stat(m->dir, &st) < 0 || !S_ISDIR(st.st_mode))
    {
      strlcpy(m->dir, home, sizeof(m->dir));
    }
}

static void music_scan(struct music_s *m)
{
  struct dirent *de;
  char path[160];
  DIR *d;

  m->ntracks = 0;
  if (m->tracks == NULL)
    {
      m->tracks = calloc(TRACKS_MAX, sizeof(struct track_s));
      if (m->tracks == NULL)
        {
          return;
        }
    }

  d = opendir(m->dir);
  if (d == NULL)
    {
      return;
    }

  while ((de = readdir(d)) != NULL && m->ntracks < TRACKS_MAX)
    {
      struct track_s *t = &m->tracks[m->ntracks];
      int kind = kind_of(de->d_name);

      if (kind < 0 || de->d_name[0] == '.')
        {
          continue;
        }

      strlcpy(t->name, de->d_name, sizeof(t->name));
      t->kind = kind;
      t->ms   = 0;
      if (kind == KIND_WAV)
        {
          uint32_t rate;
          uint8_t nch = 0;
          uint8_t bits = 0;
          uint32_t bytes;
          FILE *f;

          snprintf(path, sizeof(path), "%s/%s", m->dir, de->d_name);
          f = fopen(path, "rb");
          if (f == NULL)
            {
              continue;
            }

          bytes = wav_open(f, &rate, &nch, &bits);
          fclose(f);
          if (bytes == 0 || nch == 0 || bits == 0)
            {
              continue;
            }

          t->ms = (uint32_t)((uint64_t)bytes * 1000 /
                             ((uint64_t)rate * nch * bits / 8));
        }

      m->ntracks++;
    }

  closedir(d);
  qsort(m->tracks, m->ntracks, sizeof(struct track_s), track_cmp);
}

/****************************************************************************
 * Private Functions: the player (its own thread)
 ****************************************************************************/

/* A free channel of the mixer, named "Music", in this format */

static int chan_open(struct music_s *m, uint32_t rate, int nch, int bits)
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
          ioctl(fd, MIXIOC_SETNAME, (unsigned long)MUSIC_NAME) < 0)
        {
          close(fd);
          continue;
        }

      memset(&desc, 0, sizeof(desc));
      desc.caps.ac_len            = sizeof(struct audio_caps_s);
      desc.caps.ac_type           = AUDIO_TYPE_OUTPUT;
      desc.caps.ac_subtype        = AUDIO_FMT_PCM;
      desc.caps.ac_channels       = nch;
      desc.caps.ac_controls.hw[0] = rate & 0xffff;
      desc.caps.ac_controls.b[3]  = rate >> 16;
      desc.caps.ac_controls.b[2]  = bits;
      if (ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&desc) < 0)
        {
          close(fd);
          snprintf(m->err, sizeof(m->err), "%lu Hz, %d channels, %d bits: "
                   "not a format the mixer takes", (unsigned long)rate, nch,
                   bits);
          return -1;
        }

      m->pcm = i;
      return fd;
    }

  snprintf(m->err, sizeof(m->err), "No free sound channel");
  return -1;
}

/* The channel's frames played now (the mixer's count): as the window has
 * it, and as the player asks for it (its own descriptor)
 */

static uint32_t chan_frames(struct music_s *m)
{
  const struct fpcmix_info_s *info = desk_sound_info();

  return info != NULL && m->pcm >= 0 && m->pcm < FPCMIX_NCHAN_MAX ?
         info->chan[m->pcm].frames : 0;
}

static uint32_t player_frames(struct music_s *m)
{
  if (m->mixfd < 0 || m->pcm < 0 || m->pcm >= FPCMIX_NCHAN_MAX ||
      ioctl(m->mixfd, MIXIOC_GETINFO, (unsigned long)m->mix) < 0)
    {
      return 0;
    }

  return m->mix->chan[m->pcm].frames;
}

static bool player_req(struct music_s *m);

/* Played to the end: wait until the channel has played all that was
 * written (the next file then follows it, not over it)
 */

static void player_drain(struct music_s *m)
{
  int i;

  for (i = 0; i < 200 && !player_req(m); i++)
    {
      if ((int32_t)(player_frames(m) - m->f0 - m->written) >= -4)
        {
          break;
        }

      usleep(20000);
    }
}

/* A new request stops what is being played: true if there is one */

static bool player_req(struct music_s *m)
{
  bool req;

  pthread_mutex_lock(&m->lock);
  req = m->req != REQ_NONE;
  pthread_mutex_unlock(&m->lock);
  return req;
}

static int player_write(struct music_s *m, const void *buf, size_t n)
{
  const uint8_t *p = buf;
  uint32_t now = desk_ms();

  /* The window shows where it is, four times a second */

  if (now - m->wakems >= 250)
    {
      m->wakems = now;
      desk_wake(m->win);
    }

  while (n > 0)
    {
      ssize_t ret;

      if (player_req(m))
        {
          return -EINTR;
        }

      ret = write(m->fd, p, n);
      if (ret < 0)
        {
          return errno == EINTR ? -EINTR : -errno;
        }

      if (ret == 0)
        {
          return -EINTR;                 /* Flushed meanwhile */
        }

      p += ret;
      n -= ret;
      m->written += ret / (m->channels * m->bits / 8);
    }

  return 0;
}

static void player_started(struct music_s *m, uint32_t ms)
{
  uint32_t f = player_frames(m);

  pthread_mutex_lock(&m->lock);
  m->state       = ST_PLAYING;
  m->startms     = ms;
  m->startframes = f;
  m->f0          = f;
  m->written     = 0;
  pthread_mutex_unlock(&m->lock);
  desk_wake(m->win);
}

/* A WAV file from ms on */

static int play_wav(struct music_s *m, FILE *f, uint32_t ms)
{
  uint8_t *buf = malloc(CHUNK);
  uint32_t rate;
  uint32_t left;
  uint32_t skip;
  uint8_t nch = 0;
  uint8_t bits = 0;
  int bpf;
  int ret = 0;

  left = buf != NULL ? wav_open(f, &rate, &nch, &bits) : 0;
  if (left == 0 || nch == 0 || bits == 0)
    {
      snprintf(m->err, sizeof(m->err), "Not a PCM WAV file");
      free(buf);
      return -EINVAL;
    }

  bpf  = nch * bits / 8;
  skip = (uint32_t)((uint64_t)ms * rate / 1000) * bpf;
  skip = skip > left ? left : skip;
  fseek(f, skip, SEEK_CUR);
  left -= skip;

  m->rate     = rate;
  m->channels = nch;
  m->bits     = bits;
  m->lenms    = (uint32_t)((uint64_t)(left + skip) * 1000 / rate / bpf);
  m->fd       = chan_open(m, rate, nch, bits);
  if (m->fd < 0)
    {
      free(buf);
      return -EBUSY;
    }

  player_started(m, ms);
  while (left > 0)
    {
      size_t n = fread(buf, 1, left < CHUNK ? left : CHUNK, f);

      n -= n % bpf;
      if (n == 0)
        {
          break;
        }

      ret = player_write(m, buf, n);
      if (ret < 0)
        {
          break;
        }

      left -= n;
    }

  free(buf);
  return ret;
}

#ifdef CONFIG_AUDIOUTILS_LIBMAD
/* An MP3 file from ms on (by its size: right for a constant bit rate),
 * decoded by libmad into 16-bit samples
 */

static int16_t mad_s16(mad_fixed_t s)
{
  s += 1L << (MAD_F_FRACBITS - 16);
  s = s >= MAD_F_ONE ? MAD_F_ONE - 1 : s < -MAD_F_ONE ? -MAD_F_ONE : s;
  return (int16_t)(s >> (MAD_F_FRACBITS + 1 - 16));
}

struct mp3_s
{
  struct mad_stream stream;
  struct mad_frame frame;
  struct mad_synth synth;
  uint8_t in[CHUNK + MAD_BUFFER_GUARD];
  int16_t out[1152 * 2];
};

static int play_mp3(struct music_s *m, FILE *f, uint32_t ms)
{
  struct mp3_s *d = malloc(sizeof(struct mp3_s));
  uint32_t id3 = 0;
  long size;
  bool first = true;
  bool eof = false;
  int ret = 0;
  struct timespec t0;
  struct timespec t1;

  if (d == NULL)
    {
      return -ENOMEM;
    }

  /* An ID3v2 tag first: past it */

  fseek(f, 0, SEEK_END);
  size = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (fread(d->in, 1, 10, f) == 10 && memcmp(d->in, "ID3", 3) == 0)
    {
      id3 = 10 + ((d->in[6] & 0x7f) << 21 | (d->in[7] & 0x7f) << 14 |
                  (d->in[8] & 0x7f) << 7 | (d->in[9] & 0x7f));
    }

  mad_stream_init(&d->stream);
  mad_frame_init(&d->frame);
  mad_synth_init(&d->synth);
  fseek(f, id3, SEEK_SET);
  m->fd = -1;

  for (; ; )
    {
      int n;
      int i;

      /* More input: what is left of the buffer, then the file */

      if (d->stream.buffer == NULL || d->stream.error == MAD_ERROR_BUFLEN)
        {
          size_t keep = 0;
          size_t got;

          if (eof)
            {
              break;
            }

          if (d->stream.next_frame != NULL)
            {
              keep = d->stream.bufend - d->stream.next_frame;
              memmove(d->in, d->stream.next_frame, keep);
            }

          got = fread(d->in + keep, 1, CHUNK - keep, f);
          if (got < CHUNK - keep)
            {
              eof = true;
              memset(d->in + keep + got, 0, MAD_BUFFER_GUARD);
              got += MAD_BUFFER_GUARD;
            }

          mad_stream_buffer(&d->stream, d->in, keep + got);
          d->stream.error = MAD_ERROR_NONE;
        }

      clock_gettime(CLOCK_MONOTONIC, &t0);
      if (mad_frame_decode(&d->frame, &d->stream) < 0)
        {
          if (MAD_RECOVERABLE(d->stream.error) ||
              d->stream.error == MAD_ERROR_BUFLEN)
            {
              continue;
            }

          break;
        }

      mad_synth_frame(&d->synth, &d->frame);
      n = d->synth.pcm.length;
      for (i = 0; i < n; i++)
        {
          d->out[i * d->synth.pcm.channels] =
            mad_s16(d->synth.pcm.samples[0][i]);
          if (d->synth.pcm.channels == 2)
            {
              d->out[i * 2 + 1] = mad_s16(d->synth.pcm.samples[1][i]);
            }
        }

      clock_gettime(CLOCK_MONOTONIC, &t1);
      m->decodeus += (t1.tv_sec - t0.tv_sec) * 1000000 +
                     (t1.tv_nsec - t0.tv_nsec) / 1000;
      m->audioms  += n * 1000 / d->synth.pcm.samplerate;

      /* The first frame gives the format, the bit rate (the length), and
       * where to go for ms
       */

      if (first)
        {
          first = false;
          m->rate     = d->synth.pcm.samplerate;
          m->channels = d->synth.pcm.channels;
          m->bits     = 16;
          m->lenms    = d->frame.header.bitrate > 0 ?
                        (uint32_t)((uint64_t)(size - id3) * 8000 /
                                   d->frame.header.bitrate) : 0;
          m->tracks[m->track].ms = m->lenms;
          m->fd = chan_open(m, m->rate, m->channels, 16);
          if (m->fd < 0)
            {
              ret = -EBUSY;
              break;
            }

          if (ms > 0 && m->lenms > 0)
            {
              fseek(f, id3 + (long)((uint64_t)(size - id3) * ms /
                                    m->lenms), SEEK_SET);
              mad_stream_finish(&d->stream);
              mad_stream_init(&d->stream);
              player_started(m, ms);
              continue;
            }

          player_started(m, ms);
        }

      ret = player_write(m, d->out, n * d->synth.pcm.channels * 2);
      if (ret < 0)
        {
          break;
        }
    }

  mad_synth_finish(&d->synth);
  mad_frame_finish(&d->frame);
  mad_stream_finish(&d->stream);
  free(d);
  if (m->audioms > 0)
    {
      syslog(LOG_INFO, "music: MP3 decoding took %lu ms of CPU for %lu ms "
             "of sound (%lu.%lu %%)\n", (unsigned long)m->decodeus / 1000,
             (unsigned long)m->audioms,
             (unsigned long)(m->decodeus / 10 / m->audioms),
             (unsigned long)(m->decodeus / m->audioms % 10));
    }

  return ret;
}
#endif

/* Play a track from ms on; returns when it has ended, or a request has
 * come
 */

static void player_play(struct music_s *m, int track, uint32_t ms)
{
  char path[160];
  int ret = -EINVAL;
  FILE *f;

  snprintf(path, sizeof(path), "%s/%s", m->dir, m->tracks[track].name);
  m->err[0]   = '\0';
  m->track    = track;
  m->decodeus = 0;
  m->audioms  = 0;
  f = fopen(path, "rb");
  if (f == NULL)
    {
      snprintf(m->err, sizeof(m->err), "Cannot open it: %d", errno);
    }
  else
    {
#ifdef CONFIG_AUDIOUTILS_LIBMAD
      if (m->tracks[track].kind == KIND_MP3)
        {
          ret = play_mp3(m, f, ms);
        }
      else
#endif
        {
          ret = play_wav(m, f, ms);
        }

      fclose(f);
    }

  /* Stopped or gone elsewhere: what is written goes now; played to the
   * end: it plays out after the close
   */

  if (m->fd >= 0)
    {
      if (ret == -EINTR)
        {
          ioctl(m->fd, MIXIOC_FLUSH, 0);
        }
      else if (ret == 0)
        {
          player_drain(m);
        }

      close(m->fd);
      m->fd = -1;
    }

  pthread_mutex_lock(&m->lock);
  m->state = ST_IDLE;
  m->ended = ret == 0;
  pthread_mutex_unlock(&m->lock);
  desk_wake(m->win);
}

static void *player_thread(void *arg)
{
  struct music_s *m = arg;

  m->mixfd = open(FPCMIX_DEVPATH, O_RDONLY | O_CLOEXEC);
  m->mix   = malloc(sizeof(struct fpcmix_info_s));
  if (m->mix == NULL && m->mixfd >= 0)
    {
      close(m->mixfd);
      m->mixfd = -1;
    }

  for (; ; )
    {
      int req;
      int track;
      uint32_t ms;

      pthread_mutex_lock(&m->lock);
      while (m->req == REQ_NONE)
        {
          pthread_mutex_unlock(&m->lock);
          sem_wait(&m->wake);
          pthread_mutex_lock(&m->lock);
        }

      req    = m->req;
      track  = m->reqtrack;
      ms     = m->reqms;
      m->req = REQ_NONE;
      pthread_mutex_unlock(&m->lock);

      if (req == REQ_QUIT)
        {
          break;
        }

      if (req == REQ_PLAY && track >= 0 && track < m->ntracks)
        {
          player_play(m, track, ms);
        }
    }

  if (m->mixfd >= 0)
    {
      close(m->mixfd);
    }

  free(m->mix);
  return NULL;
}

/* From the window: a request, and a flush of what is being written so the
 * player notices at once
 */

static void music_request(struct music_s *m, int req, int track,
                          uint32_t ms)
{
  pthread_mutex_lock(&m->lock);
  m->req      = req;
  m->reqtrack = track;
  m->reqms    = ms;
  if (m->fd >= 0)
    {
      ioctl(m->fd, MIXIOC_FLUSH, 0);
    }

  pthread_mutex_unlock(&m->lock);
  sem_post(&m->wake);
}

static void music_pause(struct music_s *m)
{
  pthread_mutex_lock(&m->lock);
  if (m->fd >= 0 && (m->state == ST_PLAYING || m->state == ST_PAUSED))
    {
      m->state = m->state == ST_PLAYING ? ST_PAUSED : ST_PLAYING;
      ioctl(m->fd, MIXIOC_PAUSE, m->state == ST_PAUSED);
    }

  pthread_mutex_unlock(&m->lock);
}

/* Where the player is, ms */

static uint32_t music_pos(struct music_s *m)
{
  uint32_t frames;

  if (m->state == ST_IDLE || m->rate == 0)
    {
      return 0;
    }

  frames = chan_frames(m) - m->startframes;
  return m->startms + (uint32_t)((uint64_t)frames * 1000 / m->rate);
}

/****************************************************************************
 * Private Functions: the window
 ****************************************************************************/

static bool mslider_over(const struct mslider_s *s, int x, int y)
{
  return x >= s->x && x < s->x + s->w && y >= s->y &&
         y < s->y + WG_SLIDER_H;
}

static bool mslider_mouse(struct mslider_s *s, int x, int y,
                          uint8_t buttons, uint8_t last)
{
  bool down = (buttons & NX_MOUSE_LEFTBUTTON) != 0;
  int v = s->value;

  s->hot = mslider_over(s, x, y);
  if (down && !(last & NX_MOUSE_LEFTBUTTON) && s->hot)
    {
      s->drag = true;
    }
  else if (!down)
    {
      s->drag = false;
    }

  if (s->drag)
    {
      s->value = wg_slider_value(s->x, s->w, 0, MS_MAX, x);
    }

  return s->value != v;
}

static void mslider_draw(struct desk_ops_s *o, const struct mslider_s *s)
{
  wg_slider(o, s->x, s->y + TITLE_H, s->w, s->value, 0, MS_MAX,
            s->hot || s->drag);
}

static void fmt_time(char *buf, size_t len, uint32_t ms)
{
  snprintf(buf, len, "%lu:%02lu", (unsigned long)(ms / 60000),
           (unsigned long)(ms / 1000 % 60));
}

static int list_rows(struct desk_win_s *w)
{
  int n = (w->csize.h - LIST_Y - 8) / ROW_H;

  return n > 0 ? n : 1;
}

/* The progress bar: where it is, as a slider (0..1000 of the length) */

static void progress_geom(struct desk_win_s *w, struct mslider_s *s)
{
  s->x = PAD + 44;
  s->y = 72;
  s->w = w->csize.w - 2 * PAD - 88;
}

static void draw_head(struct music_s *m)
{
  struct desk_win_s *w = m->win;
  struct desk_ops_s *o = &m->ops;
  struct mslider_s bar;
  const char *title;
  char buf[128];
  char fit[64];
  uint32_t pos = m->seeking ? m->seekms : music_pos(m);
  int y = TITLE_H;

  ops_fill(o, 0, y, w->csize.w, HEAD_H, C_CARD);

  title = m->track >= 0 && m->track < m->ntracks ?
          m->tracks[m->track].name : "Nothing playing";
  ui_fit(&g_desk_font_title, title, w->csize.w - 2 * PAD, fit, sizeof(fit));
  ops_text(o, &g_desk_font_title, PAD, y + 14, fit, C_TEXT, 255);

  if (m->err[0] != '\0')
    {
      snprintf(buf, sizeof(buf), "%s", m->err);
    }
  else if (m->state != ST_IDLE)
    {
      snprintf(buf, sizeof(buf), "%s  %lu.%lu kHz %s%s",
               m->tracks[m->track].kind == KIND_MP3 ? "MP3" : "WAV",
               (unsigned long)((m->rate + 50) / 1000),
               (unsigned long)((m->rate + 50) / 100 % 10),
               m->channels == 1 ? "mono" : "stereo",
               m->state == ST_PAUSED ? ", paused" : "");
    }
  else
    {
      snprintf(buf, sizeof(buf), "%d file%s in %s", m->ntracks,
               m->ntracks == 1 ? "" : "s", m->dir);
    }

  ui_fit(&g_desk_font_ui, buf, w->csize.w - 2 * PAD, fit, sizeof(fit));
  ops_text(o, &g_desk_font_ui, PAD, y + 44,
           fit, m->err[0] ? C_UI_ERROR : C_TEXT_DIM, 255);

  /* Where it is */

  memset(&bar, 0, sizeof(bar));
  progress_geom(w, &bar);
  bar.value = m->lenms > 0 ? (uint32_t)((uint64_t)(pos > m->lenms ?
              m->lenms : pos) * MS_MAX / m->lenms) : 0;
  bar.drag  = m->seeking;
  mslider_draw(o, &bar);
  fmt_time(buf, sizeof(buf), m->state != ST_IDLE || m->seeking ? pos : 0);
  ops_text(o, &g_desk_font_small, PAD, y + 76, buf, C_TEXT_DIM, 255);
  fmt_time(buf, sizeof(buf), m->lenms);
  ops_text(o, &g_desk_font_small, w->csize.w - PAD -
           text_width(&g_desk_font_small, buf), y + 76, buf, C_TEXT_DIM,
           255);
}

static void draw_button(struct music_s *m, int id, int cx, int cy)
{
  struct desk_ops_s *o = &m->ops;
  int look = ui_look(&m->zones, id, true);
  uint16_t c = C_TEXT;
  int r = id == ID_PLAY ? 22 : 12;

  if (id == ID_PLAY)
    {
      ops_rrect(o, cx - 22, cy - 12, 44, 24, LUT_R12, 12, C_ACCENT,
                look == UI_PRESSED ? 180 : look == UI_HOVER ? 230 : 255);
      c = C_ACCENT_TEXT;
    }
  else if (look != UI_NORMAL)
    {
      ops_disc(o, cx, cy, LUT_R12, 12, C_TEXT,
               look == UI_PRESSED ? 50 : 25);
    }

  switch (id)
    {
      case ID_PLAY:
        if (m->state == ST_PLAYING)
          {
            ops_rrect(o, cx - 7, cy - 8, 5, 16, LUT_R3, 2, c, 255);
            ops_rrect(o, cx + 2, cy - 8, 5, 16, LUT_R3, 2, c, 255);
          }
        else
          {
            ui_triangle(o, cx + 2, cy, 9, 3, c, 255);
          }
        break;

      case ID_PREV:
        ops_blend(o, cx - 7, cy - 6, 2, 12, c, 255);
        ui_triangle(o, cx, cy, 7, 2, c, 255);
        break;

      case ID_NEXT:
        ui_triangle(o, cx - 1, cy, 7, 3, c, 255);
        ops_blend(o, cx + 5, cy - 6, 2, 12, c, 255);
        break;

      default:
        ops_rrect(o, cx - 6, cy - 6, 12, 12, LUT_R3, 2, c, 255);
        break;
    }

  ui_zone_add(&m->zones, 1, id, cx - r, cy - TITLE_H - r, 2 * r, 2 * r,
              true);
}

static void draw_controls(struct music_s *m)
{
  struct desk_win_s *w = m->win;
  struct desk_ops_s *o = &m->ops;
  const struct fpcmix_info_s *info = desk_sound_info();
  int y = TITLE_H + HEAD_H;
  int cy = y + CTRL_H / 2;
  int cx = PAD + 20;

  ops_fill(o, 0, y, w->csize.w, CTRL_H, C_WINDOW_BG);
  ops_fill(o, 0, y + CTRL_H - 1, w->csize.w, 1, g_theme.separator);
  ui_zones_clear(&m->zones, 1);
  draw_button(m, ID_PREV, cx, cy);
  draw_button(m, ID_PLAY, cx + 44, cy);
  draw_button(m, ID_NEXT, cx + 88, cy);
  draw_button(m, ID_STOP, cx + 124, cy);

  /* Its level (the application's, "Music") */

  m->volume.x = w->csize.w - PAD - 150;
  m->volume.y = cy - TITLE_H - WG_SLIDER_H / 2;
  m->volume.w = 150;
  if (!m->volume.drag)
    {
      m->volume.value = desk_sound_getlevel(MUSIC_NAME);
    }

  wg_speaker(o, m->volume.x - 34, cy - 8, 16,
             wg_speaker_waves(m->volume.value),
             info != NULL && info->mute, C_TEXT_DIM, 255);
  mslider_draw(o, &m->volume);
}

static void draw_list(struct music_s *m)
{
  struct desk_win_s *w = m->win;
  struct desk_ops_s *o = &m->ops;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  int rows = list_rows(w);
  char buf[16];
  char fit[64];
  int y = TITLE_H + LIST_Y;
  int i;

  m->shownstate = m->state;
  m->showntrack = m->track;
  ops_fill(o, 0, y, w->csize.w, w->csize.h - LIST_Y, C_WINDOW_BG);
  if (m->ntracks == 0)
    {
      ops_text(o, &g_desk_font_ui, PAD, y + 16,
               "No WAV"
#ifdef CONFIG_AUDIOUTILS_LIBMAD
               " or MP3"
#endif
               " files here", C_TEXT_DIM, 255);
    }

  for (i = m->top; i < m->ntracks && i < m->top + rows; i++)
    {
      int ry = y + 4 + (i - m->top) * ROW_H;
      bool playing = i == m->track && m->state != ST_IDLE;
      uint16_t c = i == m->sel ? C_ACCENT_TEXT : C_TEXT;

      if (i == m->sel)
        {
          ops_rrect(o, 8, ry, w->csize.w - 16, ROW_H, LUT_R6, 6, C_ACCENT,
                    200);
        }

      if (playing)
        {
          ui_triangle(o, PAD + 2, ry + ROW_H / 2, 5, 3,
                      i == m->sel ? C_ACCENT_TEXT : C_ACCENT, 255);
        }

      ui_fit(&g_desk_font_ui, m->tracks[i].name, w->csize.w - 2 * PAD - 80,
             fit, sizeof(fit));
      ops_text(o, playing ? &g_desk_font_uib : &g_desk_font_ui, PAD + 16,
               ry + (ROW_H - text_height(&g_desk_font_ui)) / 2, fit, c,
               255);
      if (m->tracks[i].ms > 0)
        {
          fmt_time(buf, sizeof(buf), m->tracks[i].ms);
          ops_text(o, &g_desk_font_ui, w->csize.w - PAD -
                   text_width(&g_desk_font_ui, buf), ry + (ROW_H -
                   text_height(&g_desk_font_ui)) / 2, buf,
                   i == m->sel ? C_ACCENT_TEXT : C_TEXT_DIM, 255);
        }

      ui_flush(hwnd, o, 64);
    }
}

static void music_draw(struct desk_win_s *w)
{
  struct music_s *m = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;

  ops_reset(&m->ops);
  draw_head(m);
  ops_draw(hwnd, &m->ops);
  draw_controls(m);
  ops_draw(hwnd, &m->ops);
  draw_list(m);
  ops_draw(hwnd, &m->ops);
}

static void music_redraw_head(struct music_s *m)
{
  ops_reset(&m->ops);
  draw_head(m);
  draw_controls(m);
  ops_draw((NXWINDOW)m->win->hwnd, &m->ops);
}

static void music_play(struct music_s *m, int track, uint32_t ms)
{
  if (track < 0 || track >= m->ntracks)
    {
      return;
    }

  m->track = track;
  m->sel   = track;
  m->err[0] = '\0';
  music_request(m, REQ_PLAY, track, ms);
}

static void music_show(struct music_s *m, int i)
{
  int rows = list_rows(m->win);

  m->sel = i < 0 ? 0 : i >= m->ntracks ? m->ntracks - 1 : i;
  if (m->sel < m->top)
    {
      m->top = m->sel;
    }
  else if (m->sel >= m->top + rows)
    {
      m->top = m->sel - rows + 1;
    }
}

static void music_button(struct music_s *m, int id)
{
  switch (id)
    {
      case ID_PLAY:
        if (m->state == ST_IDLE)
          {
            music_play(m, m->sel >= 0 ? m->sel : 0, 0);
          }
        else
          {
            music_pause(m);
          }
        break;

      case ID_PREV:
        music_play(m, music_pos(m) > 3000 || m->track <= 0 ? m->track :
                   m->track - 1, 0);
        break;

      case ID_NEXT:
        music_play(m, m->track + 1, 0);
        break;

      case ID_STOP:
        music_request(m, REQ_STOP, 0, 0);
        break;
    }
}

static void music_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct music_s *m = w->priv;
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(m->last & NX_MOUSE_LEFTBUTTON);
  bool release = !(buttons & NX_MOUSE_LEFTBUTTON) &&
                 (m->last & NX_MOUSE_LEFTBUTTON);
  uint8_t last = m->last;
  struct mslider_s bar;
  struct ui_zone_s *z;
  int hot;
  bool drag;

  m->last = buttons;

  /* The buttons */

  z   = ui_zone_at(&m->zones, x, y);
  hot = z != NULL ? z->id : 0;
  if (hot != m->zones.hot || (release && m->zones.pressed != 0))
    {
      int pressed = m->zones.pressed;

      m->zones.hot = hot;
      if (release)
        {
          m->zones.pressed = 0;
          if (pressed != 0 && pressed == hot)
            {
              music_button(m, hot);
            }
        }

      music_redraw_head(m);
    }

  if (press && hot != 0)
    {
      m->zones.pressed = hot;
      music_redraw_head(m);
      return;
    }

  /* The level */

  drag = m->volume.drag;
  if (mslider_mouse(&m->volume, x, y, buttons, last))
    {
      desk_sound_level(MUSIC_NAME, m->volume.value);
      music_redraw_head(m);
    }
  else if (drag != m->volume.drag)
    {
      desk_prefs_save();
      music_redraw_head(m);
    }

  /* The progress bar: held, it shows where it will go; let go, it goes */

  memset(&bar, 0, sizeof(bar));
  progress_geom(w, &bar);
  if (m->lenms > 0 && m->track >= 0 &&
      ((press && mslider_over(&bar, x, y)) || m->seeking))
    {
      int v;

      m->seeking = (buttons & NX_MOUSE_LEFTBUTTON) != 0;
      v = (x - bar.x) * MS_MAX / (bar.w > 0 ? bar.w : 1);
      v = v < 0 ? 0 : v > MS_MAX ? MS_MAX : v;
      m->seekms = (uint32_t)((uint64_t)m->lenms * v / MS_MAX);
      if (!m->seeking)
        {
          music_play(m, m->track, m->seekms);
        }

      music_redraw_head(m);
      return;
    }

  /* The list: click selects, a double click plays */

  if (press && y >= LIST_Y + 4)
    {
      int row = m->top + (y - LIST_Y - 4) / ROW_H;
      uint32_t now = desk_ms();

      if (row < m->ntracks)
        {
          if (row == m->clickrow && now - m->clickms < 400)
            {
              music_play(m, row, 0);
            }

          m->sel      = row;
          m->clickrow = row;
          m->clickms  = now;
          music_draw(w);
        }
    }
}

static void music_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  static struct ui_keys_s k;
  struct music_s *m = w->priv;
  int keys[8];
  int nk = ui_keys(&k, ch, n, keys, 8);
  int i;

  for (i = 0; i < nk; i++)
    {
      switch (keys[i] & KEY_MASK)
        {
          case ' ':
            music_button(m, ID_PLAY);
            break;

          case '\r':
          case '\n':
            music_play(m, m->sel, 0);
            break;

          case KEY_UP:
            music_show(m, m->sel - 1);
            break;

          case KEY_DOWN:
            music_show(m, m->sel + 1);
            break;

          case KEY_LEFT:
          case KEY_RIGHT:
            if (m->state != ST_IDLE)
              {
                int32_t pos = (int32_t)music_pos(m) +
                              ((keys[i] & KEY_MASK) == KEY_LEFT ? -10000 :
                                                                  10000);

                music_play(m, m->track, pos < 0 ? 0 : pos);
              }
            break;

          default:
            continue;
        }
    }

  if (nk > 0)
    {
      music_draw(w);
    }
}

/* The player's news: where it is (four times a second while it plays), a
 * file that has ended: the next
 */

static void music_wake(struct desk_win_s *w)
{
  struct music_s *m = w->priv;
  bool ended;

  pthread_mutex_lock(&m->lock);
  ended    = m->ended;
  m->ended = false;
  pthread_mutex_unlock(&m->lock);

  desk_sound_refresh();
  if (ended && m->track + 1 < m->ntracks)
    {
      music_play(m, m->track + 1, 0);
      music_draw(w);
      return;
    }

  music_redraw_head(m);
  if (ended || m->state != m->shownstate || m->track != m->showntrack)
    {
      draw_list(m);
      ops_draw((NXWINDOW)w->hwnd, &m->ops);
    }
}

static void music_tick(struct desk_win_s *w)
{
  struct music_s *m = w->priv;

  if (m->state == ST_PLAYING)
    {
      desk_sound_refresh();
      music_redraw_head(m);
    }
}

static struct desk_win_s *music_open(void)
{
  pthread_attr_t attr;
  struct desk_win_s *w;
  struct music_s *m;

  m = calloc(1, sizeof(*m));
  if (m == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_MUSIC, "Music", MUSIC_W, MUSIC_H);
  if (w == NULL)
    {
      free(m);
      return NULL;
    }

  w->priv     = m;
  m->win      = w;
  m->fd       = -1;
  m->pcm      = -1;
  m->track    = -1;
  m->clickrow = -1;
  m->mixfd    = -1;
  pthread_mutex_init(&m->lock, NULL);
  sem_init(&m->wake, 0, 0);
  music_dir(m);
  music_scan(m);

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 4096);
  m->running = pthread_create(&m->thread, &attr, player_thread, m) == 0;
  if (m->running)
    {
      pthread_setname_np(m->thread, "music");
    }

  music_draw(w);
  return w;
}

static void music_close(struct desk_win_s *w)
{
  struct music_s *m = w->priv;

  if (m->running)
    {
      music_request(m, REQ_QUIT, 0, 0);
      pthread_join(m->thread, NULL);
    }

  sem_destroy(&m->wake);
  pthread_mutex_destroy(&m->lock);
  free(m->tracks);
}

/* A note (two heads on stems, beamed) on a pink tile */

static void music_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  icon_tile(o, x, y, s, RGB(0xff, 0x5e, 0x7a), RGB(0xd8, 0x1b, 0x60), a);
  ops_disc(o, x + s * 36 / 100, y + s * 70 / 100, LUT_R6, s / 9, C_WHITE,
           a);
  ops_disc(o, x + s * 66 / 100, y + s * 62 / 100, LUT_R6, s / 9, C_WHITE,
           a);
  ops_blend(o, x + s * 36 / 100 + s / 9 - 2, y + s * 28 / 100, 3,
            s * 42 / 100, C_WHITE, a);
  ops_blend(o, x + s * 66 / 100 + s / 9 - 2, y + s * 20 / 100, 3,
            s * 42 / 100, C_WHITE, a);
  ops_blend(o, x + s * 36 / 100 + s / 9 - 2, y + s * 20 / 100,
            s * 30 / 100 + 3, s / 10, C_WHITE, a);
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_music =
{
  .name   = "Music",
  .icon   = music_icon,
  .open   = music_open,
  .draw   = music_draw,
  .tick   = music_tick,
  .mouse  = music_mouse,
  .key    = music_key,
  .close  = music_close,
  .wake   = music_wake,
  .single = true,
};
