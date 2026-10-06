/****************************************************************************
 * apps/netutils/nctools/host.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * host NAME: its addresses (NuttX's resolver, over the network
 * coprocessor, the WiFi's DNS servers)
 ****************************************************************************/

#include <nuttx/config.h>

#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

int main(int argc, FAR char *argv[])
{
  struct addrinfo hints;
  struct addrinfo *res;
  struct addrinfo *ai;
  char buf[INET6_ADDRSTRLEN];
  int ret;

  if (argc != 2)
    {
      fprintf(stderr, "usage: host NAME\n");
      return EXIT_FAILURE;
    }

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  ret = getaddrinfo(argv[1], NULL, &hints, &res);
  if (ret != 0)
    {
      fprintf(stderr, "Host %s not found: %s\n", argv[1], gai_strerror(ret));
      return EXIT_FAILURE;
    }

  for (ai = res; ai != NULL; ai = ai->ai_next)
    {
      const void *a = ai->ai_family == AF_INET ?
        (const void *)&((struct sockaddr_in *)ai->ai_addr)->sin_addr :
        (const void *)&((struct sockaddr_in6 *)ai->ai_addr)->sin6_addr;

      printf("%s has %saddress %s\n", argv[1],
             ai->ai_family == AF_INET ? "" : "IPv6 ",
             inet_ntop(ai->ai_family, a, buf, sizeof(buf)));
    }

  freeaddrinfo(res);
  return EXIT_SUCCESS;
}
