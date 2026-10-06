/****************************************************************************
 * apps/netutils/nctools/nc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * nc: netcat over the network coprocessor.
 *
 *   nc [-u] [-t] HOST PORT     connect; stdin to it, it to stdout (-u UDP;
 *                              -t TLS, the coprocessor doing it)
 *   nc -l [-u] PORT            listen for one connection (or datagrams)
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include "netutils/netcop.h"

static int pump(int sd, bool udp)
{
  struct sockaddr_storage peer;
  socklen_t plen = 0;
  struct pollfd pfd[2];
  uint8_t buf[512];
  bool in_open = true;

  for (; ; )
    {
      ssize_t n;

      pfd[0].fd = sd;
      pfd[0].events = POLLIN;
      pfd[1].fd = in_open ? STDIN_FILENO : -1;
      pfd[1].events = POLLIN;
      if (poll(pfd, 2, -1) < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return EXIT_FAILURE;
        }

      if (pfd[0].revents)
        {
          socklen_t l = sizeof(peer);

          n = udp ? recvfrom(sd, buf, sizeof(buf), 0,
                             (struct sockaddr *)&peer, &l) :
                    recv(sd, buf, sizeof(buf), 0);
          if (n <= 0)
            {
              return n < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
            }

          if (udp)
            {
              plen = l;
            }

          write(STDOUT_FILENO, buf, n);
        }

      if (pfd[1].revents)
        {
          n = read(STDIN_FILENO, buf, sizeof(buf));
          if (n <= 0)
            {
              if (!udp)
                {
                  shutdown(sd, SHUT_WR);
                }

              in_open = false;
              continue;
            }

          if (udp && plen > 0)
            {
              sendto(sd, buf, n, 0, (struct sockaddr *)&peer, plen);
            }
          else
            {
              send(sd, buf, n, 0);
            }
        }
    }
}

int main(int argc, FAR char *argv[])
{
  bool udp = false;
  bool listen_ = false;
  bool tls = false;
  int i = 1;
  int sd;

  for (; i < argc && argv[i][0] == '-'; i++)
    {
      udp |= strchr(argv[i], 'u') != NULL;
      listen_ |= strchr(argv[i], 'l') != NULL;
      tls |= strchr(argv[i], 't') != NULL;
    }

  if (listen_ ? i + 1 != argc : i + 2 != argc)
    {
      fprintf(stderr, "usage: nc [-u] [-t] HOST PORT | nc -l [-u] PORT\n");
      return EXIT_FAILURE;
    }

  if (listen_)
    {
      struct sockaddr_in sa;
      int ls = socket(AF_INET, udp ? SOCK_DGRAM : SOCK_STREAM, 0);

      memset(&sa, 0, sizeof(sa));
      sa.sin_family = AF_INET;
      sa.sin_port = htons(atoi(argv[i]));
      if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof(sa)) < 0 ||
          (!udp && listen(ls, 1) < 0))
        {
          fprintf(stderr, "nc: %s\n", strerror(errno));
          return EXIT_FAILURE;
        }

      if (udp)
        {
          return pump(ls, true);
        }

      sd = accept(ls, NULL, NULL);
      close(ls);
      if (sd < 0)
        {
          fprintf(stderr, "nc: accept: %s\n", strerror(errno));
          return EXIT_FAILURE;
        }
    }
  else
    {
      struct addrinfo hints;
      struct addrinfo *res;

      memset(&hints, 0, sizeof(hints));
      hints.ai_family = AF_INET;
      hints.ai_socktype = udp ? SOCK_DGRAM : SOCK_STREAM;
      if (getaddrinfo(argv[i], argv[i + 1], &hints, &res) != 0)
        {
          fprintf(stderr, "nc: cannot resolve %s\n", argv[i]);
          return EXIT_FAILURE;
        }

      sd = socket(AF_INET, hints.ai_socktype, 0);
      if (sd < 0 || connect(sd, res->ai_addr, res->ai_addrlen) < 0)
        {
          fprintf(stderr, "nc: %s port %s: %s\n", argv[i], argv[i + 1],
                  strerror(errno));
          freeaddrinfo(res);
          return EXIT_FAILURE;
        }

      freeaddrinfo(res);
      if (tls && !udp)
        {
          int ret = ncx_tls_start(sd, argv[i], NULL, true, NULL);

          if (ret < 0)
            {
              fprintf(stderr, "nc: TLS: %s\n", strerror(-ret));
              close(sd);
              return EXIT_FAILURE;
            }
        }
    }

  i = pump(sd, udp);
  close(sd);
  return i;
}
