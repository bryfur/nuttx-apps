/****************************************************************************
 * apps/netutils/nctools/ntpdate.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ntpdate: the clock and NTP.
 *
 *   ntpdate                  what the NTP client netcopd started (once WiFi
 *                            was up) has done, and the clock
 *   ntpdate [-q] SERVER[:PORT]
 *                            ask a server once (SNTP); set the clock to it
 *                            unless -q
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include "netutils/netcop.h"

#define NTP_EPOCH 2208988800u

static void put_time(void)
{
  time_t t = time(NULL);
  struct tm tm;
  char buf[64];

  localtime_r(&t, &tm);
  strftime(buf, sizeof(buf), "%a %b %e %H:%M:%S %Z %Y", &tm);
  printf("the clock: %s\n", buf);
}

static int status(void)
{
  struct ncx_rsp *rsp = malloc(sizeof(*rsp));
  struct ncx_ntp nt;
  int ret;

  if (rsp == NULL)
    {
      return EXIT_FAILURE;
    }

  ret = ncx_call1(NCX_OP_NTP, NULL, 0, rsp, NULL, NULL, 3000);
  if (ret < 0)
    {
      fprintf(stderr, "ntpdate: no netcopd\n");
      free(rsp);
      put_time();
      return EXIT_FAILURE;
    }

  memset(&nt, 0, sizeof(nt));
  memcpy(&nt, rsp->body, rsp->n < sizeof(nt) ? rsp->n : sizeof(nt));
  free(rsp);
  if (!nt.running)
    {
      printf("NTP: not started (WiFi not up yet); servers %s\n", nt.server);
    }
  else if (!nt.synced)
    {
      printf("NTP: asking %s, not synced yet\n", nt.server);
    }
  else
    {
      time_t t = (time_t)nt.last;
      struct tm tm;
      char buf[40];

      gmtime_r(&t, &tm);
      strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &tm);
      printf("NTP: synced with %s at %s (offset %lld ms)\n", nt.server, buf,
             (long long)(nt.offset_us / 1000));
    }

  put_time();
  return EXIT_SUCCESS;
}

static uint32_t get32(const uint8_t *p)
{
  return ((uint32_t)p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3];
}

static int query(const char *server, bool set)
{
  struct addrinfo hints;
  struct addrinfo *res;
  struct pollfd pfd;
  struct timespec t1;
  struct timespec t4;
  uint8_t pkt[48];
  char host[NC_HOST_MAX + 1];
  const char *port = "123";
  char *colon;
  int64_t offset;
  int64_t tx;
  int sd;
  int ret;

  strlcpy(host, server, sizeof(host));
  colon = strchr(host, ':');
  if (colon != NULL)
    {
      *colon = '\0';
      port = colon + 1;
    }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_INET;
  hints.ai_socktype = SOCK_DGRAM;
  if (getaddrinfo(host, port, &hints, &res) != 0)
    {
      fprintf(stderr, "ntpdate: cannot resolve %s\n", host);
      return EXIT_FAILURE;
    }

  sd = socket(AF_INET, SOCK_DGRAM, 0);
  if (sd < 0)
    {
      fprintf(stderr, "ntpdate: socket: %s\n", strerror(errno));
      freeaddrinfo(res);
      return EXIT_FAILURE;
    }

  memset(pkt, 0, sizeof(pkt));
  pkt[0] = 0x23;                      /* Version 4, client */
  clock_gettime(CLOCK_REALTIME, &t1);
  ret = sendto(sd, pkt, sizeof(pkt), 0, res->ai_addr, res->ai_addrlen);
  freeaddrinfo(res);
  pfd.fd = sd;
  pfd.events = POLLIN;
  if (ret < 0 || poll(&pfd, 1, 5000) <= 0 ||
      recv(sd, pkt, sizeof(pkt), 0) < 48)
    {
      fprintf(stderr, "ntpdate: no answer from %s\n", server);
      close(sd);
      return EXIT_FAILURE;
    }

  clock_gettime(CLOCK_REALTIME, &t4);
  close(sd);

  /* The server's transmit time (ignoring the round trip's asymmetry) */

  tx = (int64_t)(get32(pkt + 40) - NTP_EPOCH) * 1000000 +
       (((int64_t)get32(pkt + 44) * 1000000) >> 32);
  offset = tx - ((int64_t)t1.tv_sec * 1000000 + t1.tv_nsec / 1000 +
                 (int64_t)t4.tv_sec * 1000000 + t4.tv_nsec / 1000) / 2;
  printf("server %s, stratum %u, offset %+.3f s\n", server, pkt[1],
         offset / 1e6);
  if (set)
    {
      struct timespec ts;

      ts.tv_sec  = tx / 1000000;
      ts.tv_nsec = (tx % 1000000) * 1000;
      if (clock_settime(CLOCK_REALTIME, &ts) < 0)
        {
          fprintf(stderr, "ntpdate: clock_settime: %s\n", strerror(errno));
          return EXIT_FAILURE;
        }
    }

  put_time();
  return EXIT_SUCCESS;
}

int main(int argc, FAR char *argv[])
{
  bool set = true;
  int i = 1;

  if (i < argc && strcmp(argv[i], "-q") == 0)
    {
      set = false;
      i++;
    }

  if (i >= argc)
    {
      return status();
    }

  return query(argv[i], set);
}
