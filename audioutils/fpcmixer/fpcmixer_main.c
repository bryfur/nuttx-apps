/****************************************************************************
 * apps/audioutils/fpcmixer/fpcmixer_main.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * mixer: the FPC's audio mixer (nuttx/audio/fpc_mixer.h) from the shell,
 * and test sounds through it.
 *
 *   mixer                        what is playing, the volumes
 *   mixer master [N|+N|-N]       the master volume, percent
 *   mixer mute [on|off|toggle]
 *   mixer level NAME N           an application's level, percent
 *   mixer tone FREQ MS [RATE [CH [BITS [AMP [NAME [VOL]]]]]]
 *                                a sine through a free channel; defaults
 *                                48000 Hz, 2 ch, 16 bits, peak 10000,
 *                                "tone", volume 100 %
 *   mixer wav FILE [NAME]        a WAV file through a free channel
 *   mixer watch [N]              a line each time the state changes
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <nuttx/audio/audio.h>
#include <nuttx/audio/fpc_mixer.h>

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* sin(x) without libm: reduced to [-pi/2, pi/2], then a polynomial */

static double msin(double x)
{
  const double pi = 3.14159265358979323846;
  double x2;
  long k;

  k = (long)(x / pi + (x >= 0 ? 0.5 : -0.5));
  x -= k * pi;
  x2 = x * x;
  x = x * (1 - x2 / 6 * (1 - x2 / 20 * (1 - x2 / 42 * (1 - x2 / 72 *
          (1 - x2 / 110)))));
  return (k & 1) ? -x : x;
}

/* A free channel, reserved and configured: its descriptor, or -1 */

static int chan_open(unsigned int rate, int nch, int bits,
                     const char *name, int vol)
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

      /* Skip one in use; the name claims it (another mixer may race) */

      memset(&desc, 0, sizeof(desc));
      desc.caps.ac_len     = sizeof(struct audio_caps_s);
      desc.caps.ac_type    = AUDIO_TYPE_QUERY;
      desc.caps.ac_subtype = AUDIO_TYPE_QUERY;
      if (ioctl(fd, AUDIOIOC_GETCAPS, (unsigned long)&desc.caps) < 0 ||
          (desc.caps.ac_controls.b[0] & AUDIO_TYPE_OUTPUT) == 0 ||
          ioctl(fd, AUDIOIOC_RESERVE, 0) < 0 ||
          ioctl(fd, MIXIOC_SETNAME, (unsigned long)name) < 0)
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
          fprintf(stderr, "mixer: %s: format not taken: %d\n", path, errno);
          close(fd);
          return -1;
        }

      if (vol >= 0)
        {
          memset(&desc, 0, sizeof(desc));
          desc.caps.ac_len            = sizeof(struct audio_caps_s);
          desc.caps.ac_type           = AUDIO_TYPE_FEATURE;
          desc.caps.ac_format.hw      = AUDIO_FU_VOLUME;
          desc.caps.ac_controls.hw[0] = vol;
          ioctl(fd, AUDIOIOC_CONFIGURE, (unsigned long)&desc);
        }

      return fd;
    }

  fprintf(stderr, "mixer: no free channel\n");
  return -1;
}

static int write_all(int fd, const uint8_t *p, size_t n)
{
  while (n > 0)
    {
      ssize_t ret = write(fd, p, n);

      if (ret <= 0)
        {
          if (ret < 0 && errno == EINTR)
            {
              continue;
            }

          fprintf(stderr, "mixer: write: %d\n", ret < 0 ? errno : 0);
          return -1;
        }

      p += ret;
      n -= ret;
    }

  return 0;
}

static int cmd_tone(int argc, char **argv)
{
  uint8_t buf[1024];
  double f = atof(argv[2]);
  long ms = atol(argv[3]);
  unsigned int rate = argc > 4 ? atoi(argv[4]) : 48000;
  int nch = argc > 5 ? atoi(argv[5]) : 2;
  int bits = argc > 6 ? atoi(argv[6]) : 16;
  int amp = argc > 7 ? atoi(argv[7]) : 10000;
  const char *name = argc > 8 ? argv[8] : "tone";
  int vol = argc > 9 ? atoi(argv[9]) * 10 : 1000;
  int bpf = nch * bits / 8;
  long frames = (long)rate * ms / 1000;
  long i = 0;
  static int16_t sine[1024];
  uint64_t step;
  int n0;
  int fd;

  /* One period in a table, stepped by a phase: msin() per frame can't
   * keep up with 48 kHz (no double-precision FPU)
   */

  for (n0 = 0; n0 < 1024; n0++)
    {
      double v = 32767 * msin(2 * 3.14159265358979323846 * n0 / 1024);
      sine[n0] = (int16_t)(v < 0 ? v - 0.5 : v + 0.5);
    }

  step = (uint64_t)(f / rate * 4294967296.0);

  fd = chan_open(rate, nch, bits, name, vol);
  if (fd < 0)
    {
      return EXIT_FAILURE;
    }

  while (i < frames)
    {
      size_t n = 0;

      for (; i < frames && n + bpf <= sizeof(buf); i++)
        {
          uint32_t ph = (uint32_t)(i * step);
          int32_t a0 = sine[ph >> 22];
          int32_t a1 = sine[((ph >> 22) + 1) & 1023];
          int32_t s = (int32_t)((a0 + (((a1 - a0) * (int32_t)((ph >> 6) & 0xffff))
                                  >> 16)) * amp) >> 15;
          int c;

          for (c = 0; c < nch; c++)
            {
              switch (bits)
                {
                  case 8:
                    buf[n++] = (uint8_t)(128 + (s >> 8));
                    break;

                  case 24:
                    buf[n++] = 0;
                    buf[n++] = (uint8_t)s;
                    buf[n++] = (uint8_t)(s >> 8);
                    break;

                  case 32:
                    buf[n++] = 0;
                    buf[n++] = 0;
                    buf[n++] = (uint8_t)s;
                    buf[n++] = (uint8_t)(s >> 8);
                    break;

                  default:
                    buf[n++] = (uint8_t)s;
                    buf[n++] = (uint8_t)(s >> 8);
                    break;
                }
            }
        }

      if (write_all(fd, buf, n) < 0)
        {
          break;
        }
    }

  close(fd);                            /* What is written plays out */
  return EXIT_SUCCESS;
}

static uint32_t le32(const uint8_t *p)
{
  return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static int cmd_wav(int argc, char **argv)
{
  uint8_t buf[1024];
  unsigned int rate = 0;
  int nch = 0;
  int bits = 0;
  uint32_t left = 0;
  int fd = -1;
  FILE *f;

  f = fopen(argv[2], "rb");
  if (f == NULL)
    {
      fprintf(stderr, "mixer: %s: %d\n", argv[2], errno);
      return EXIT_FAILURE;
    }

  if (fread(buf, 1, 12, f) != 12 || memcmp(buf, "RIFF", 4) != 0 ||
      memcmp(buf + 8, "WAVE", 4) != 0)
    {
      fprintf(stderr, "mixer: %s: not a WAV file\n", argv[2]);
      fclose(f);
      return EXIT_FAILURE;
    }

  while (fread(buf, 1, 8, f) == 8)
    {
      uint32_t size = le32(buf + 4);

      if (memcmp(buf, "fmt ", 4) == 0 && size >= 16 && size <= 64)
        {
          if (fread(buf, 1, size, f) != size)
            {
              break;
            }

          nch  = buf[2] | buf[3] << 8;
          rate = le32(buf + 4);
          bits = buf[14] | buf[15] << 8;
        }
      else if (memcmp(buf, "data", 4) == 0)
        {
          left = size;
          break;
        }
      else
        {
          fseek(f, size + (size & 1), SEEK_CUR);
        }
    }

  if (nch == 0 || left == 0)
    {
      fprintf(stderr, "mixer: %s: no PCM in it\n", argv[2]);
      fclose(f);
      return EXIT_FAILURE;
    }

  fd = chan_open(rate, nch, bits, argc > 3 ? argv[3] : "wav", -1);
  while (fd >= 0 && left > 0)
    {
      size_t n = fread(buf, 1, left < sizeof(buf) ? left : sizeof(buf), f);

      if (n == 0 || write_all(fd, buf, n) < 0)
        {
          break;
        }

      left -= n;
    }

  if (fd >= 0)
    {
      close(fd);
    }

  fclose(f);
  return fd >= 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

static void watch_line(const struct fpcmix_info_s *info)
{
  int i;

  printf("master %d%%%s latency %u ms xruns %lu:", (info->master + 5) / 10,
         info->mute ? " muted" : "", info->latency,
         (unsigned long)info->xruns);
  for (i = 0; i < info->nchan; i++)
    {
      const struct fpcmix_chan_s *c = &info->chan[i];

      if (c->state != FPCMIX_IDLE)
        {
          printf(" [pcm%d %s %d %lu Hz]", i, c->name, c->state,
                 (unsigned long)c->rate);
        }
    }

  printf("\n");
}

static int cmd_watch(int fd, int count)
{
  struct fpcmix_info_s *info = malloc(sizeof(*info));
  struct pollfd pfd;

  if (info == NULL)
    {
      return EXIT_FAILURE;
    }

  pfd.fd     = fd;
  pfd.events = POLLIN;
  while (count-- > 0)
    {
      if (poll(&pfd, 1, -1) < 0 && errno != EINTR)
        {
          break;
        }

      if (pfd.revents & POLLIN)
        {
          ioctl(fd, MIXIOC_GETINFO, (unsigned long)info);
          watch_line(info);
        }
    }

  free(info);
  return EXIT_SUCCESS;
}

static void usage(void)
{
  fprintf(stderr,
          "mixer                        what is playing, the volumes\n"
          "mixer master [N|+N|-N]       master volume, percent\n"
          "mixer mute [on|off|toggle]\n"
          "mixer level NAME N           an application's level, percent\n"
          "mixer tone FREQ MS [RATE [CH [BITS [AMP [NAME [VOL]]]]]]\n"
          "mixer wav FILE [NAME]\n"
          "mixer watch [N]\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  char buf[256];
  int ret = EXIT_SUCCESS;
  ssize_t n;
  int fd;

  if (argc > 1 && strcmp(argv[1], "tone") == 0 && argc >= 4)
    {
      return cmd_tone(argc, argv);
    }

  if (argc > 1 && strcmp(argv[1], "wav") == 0 && argc >= 3)
    {
      return cmd_wav(argc, argv);
    }

  fd = open(FPCMIX_DEVPATH, O_RDONLY);
  if (fd < 0)
    {
      fprintf(stderr, "mixer: %s: %d\n", FPCMIX_DEVPATH, errno);
      return EXIT_FAILURE;
    }

  if (argc == 1)
    {
      while ((n = read(fd, buf, sizeof(buf))) > 0)
        {
          fwrite(buf, 1, n, stdout);
        }
    }
  else if (strcmp(argv[1], "master") == 0 && argc >= 3)
    {
      int v = atoi(argv[2]) * 10;

      ret = ioctl(fd, argv[2][0] == '+' || argv[2][0] == '-' ?
                  MIXIOC_STEPMASTER : MIXIOC_SETMASTER, v);
    }
  else if (strcmp(argv[1], "mute") == 0)
    {
      int how = 2;

      if (argc >= 3)
        {
          how = strcmp(argv[2], "on") == 0 ? 1 :
                strcmp(argv[2], "off") == 0 ? 0 : 2;
        }

      ret = ioctl(fd, MIXIOC_SETMUTE, how);
    }
  else if (strcmp(argv[1], "level") == 0 && argc >= 4)
    {
      struct fpcmix_level_s l;

      memset(&l, 0, sizeof(l));
      strlcpy(l.name, argv[2], sizeof(l.name));
      l.level = atoi(argv[3]) * 10;
      ret = ioctl(fd, MIXIOC_SETLEVEL, (unsigned long)&l);
    }
  else if (strcmp(argv[1], "watch") == 0)
    {
      ret = cmd_watch(fd, argc >= 3 ? atoi(argv[2]) : 1000000);
    }
  else
    {
      usage();
      ret = EXIT_FAILURE;
    }

  if (ret < 0)
    {
      fprintf(stderr, "mixer: %d\n", errno);
      ret = EXIT_FAILURE;
    }

  close(fd);
  return ret;
}
