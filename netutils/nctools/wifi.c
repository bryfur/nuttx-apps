/****************************************************************************
 * apps/netutils/nctools/wifi.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * wifi: the network coprocessor's WiFi from NSH (docs/netcop.md 5.2).
 *
 *   wifi                     the status (as ifconfig)
 *   wifi scan                the networks in range
 *   wifi connect SSID [PASS] join one (saved on success); no password:
 *                            the saved one
 *   wifi disconnect [-s]     leave (-s: stay off until the next connect)
 *   wifi saved               the saved networks
 *   wifi forget SSID|-a      forget one, or all
 *   wifi auto on|off         auto-connect
 *   wifi hostname [NAME]
 *   wifi info                the coprocessor: firmware, link, time
 *
 * ifconfig (ifconfig.c includes this file) prints the status the way
 * NuttX's ifconfig would for an interface "wlan0".
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>

#include "netutils/netcop.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static char *mac_str(const uint8_t *a, char *buf)
{
  sprintf(buf, "%02x:%02x:%02x:%02x:%02x:%02x", a[0], a[1], a[2], a[3],
          a[4], a[5]);
  return buf;
}

static char *ip_str(const uint8_t *a, char *buf)
{
  sprintf(buf, "%u.%u.%u.%u", a[0], a[1], a[2], a[3]);
  return buf;
}

/* No netcopd, no coprocessor: say so plainly */

static int no_hardware(int ret)
{
  if (ret == -ENETDOWN || ret == -EAFNOSUPPORT || ret == -ENOTCONN)
    {
      fprintf(stderr, "No WiFi hardware (no network coprocessor)\n");
    }
  else
    {
      fprintf(stderr, "wifi: %s\n", strerror(-ret));
    }

  return EXIT_FAILURE;
}

static int get_status(struct nc_wifi_status *w)
{
  struct ncx_rsp *rsp = malloc(sizeof(*rsp));
  struct nc_rd rd;
  int ret;

  if (rsp == NULL)
    {
      return -ENOMEM;
    }

  ret = ncx_call1(NC_OP_WIFI_STATUS, NULL, 0, rsp, NULL, NULL, 5000);
  if (ret >= 0)
    {
      nc_rd_init(&rd, rsp->body, rsp->n);
      ret = nc_dec_wifi_status(&rd, w);
    }

  free(rsp);
  return ret;
}

static void print_status(const struct nc_wifi_status *w)
{
  char a[20];
  char b[20];

  printf("state: %s", ncx_wifi_state_str(w->state));
  if (w->reason != NC_WIFI_R_NONE && w->state != NC_WIFI_UP)
    {
      printf(" (%s)", ncx_wifi_reason_str(w->reason));
    }

  printf("%s\n", w->flags & NC_WIFI_ST_EMULATED ? " [emulated]" : "");
  if (w->ssid[0] != '\0')
    {
      printf("ssid: %s  %s  signal %d dBm (%d/4)  channel %u\n", w->ssid,
             ncx_wifi_auth_str(w->auth), w->rssi, ncx_rssi_bars(w->rssi),
             w->channel);
    }

  if (w->state == NC_WIFI_UP)
    {
      printf("ip: %s  netmask %s\n", ip_str(w->ip, a), ip_str(w->mask, b));
      printf("gateway: %s  dns %s", ip_str(w->gw, a), ip_str(w->dns1, b));
      if (memcmp(w->dns2, "\0\0\0\0", 4) != 0)
        {
          printf(" %s", ip_str(w->dns2, a));
        }

      printf("\n");
    }

  printf("mac: %s  hostname %s  auto-connect %s\n", mac_str(w->mac, a),
         w->hostname, w->flags & NC_WIFI_ST_AUTOCONNECT ? "on" : "off");
}

static int cmd_status(bool as_ifconfig)
{
  struct nc_wifi_status w;
  char a[20];
  char b[20];
  int ret = get_status(&w);

  if (ret < 0)
    {
      return no_hardware(ret);
    }

  if (!as_ifconfig)
    {
      print_status(&w);
      return EXIT_SUCCESS;
    }

  printf("wlan0\tLink encap:Ethernet HWaddr %s at %s\n", mac_str(w.mac, a),
         w.state == NC_WIFI_UP ? "UP" : "DOWN");
  if (w.state == NC_WIFI_UP)
    {
      printf("\tinet addr:%s DRaddr:%s ", ip_str(w.ip, a), ip_str(w.gw, b));
      printf("Mask:%s\n", ip_str(w.mask, a));
      printf("\tESSID:%s Signal:%d dBm DNS:%s\n", w.ssid, w.rssi,
             ip_str(w.dns1, a));
    }
  else if (w.ssid[0] != '\0')
    {
      printf("\tESSID:%s (%s)\n", w.ssid, ncx_wifi_state_str(w.state));
    }

  return EXIT_SUCCESS;
}

static void scan_part(void *arg, const uint8_t *p, size_t n)
{
  static const char *const bars[5] =
  {
    "    ", "#   ", "##  ", "### ", "####"
  };

  struct nc_wifi_ap ap;
  struct nc_rd rd;
  int cnt;

  nc_rd_init(&rd, p, n);
  for (cnt = nc_rd_u8(&rd); cnt > 0; cnt--)
    {
      if (nc_dec_wifi_ap(&rd, &ap) != 0)
        {
          break;
        }

      printf("%c %-24s %4d dBm %s  ch %3u  %-9s%s\n",
             ap.flags & NC_WIFI_AP_CURRENT ? '*' : ' ', ap.ssid, ap.rssi,
             bars[ncx_rssi_bars(ap.rssi)], ap.channel,
             ncx_wifi_auth_str(ap.auth),
             ap.flags & NC_WIFI_AP_SAVED ? " saved" : "");
      (*(int *)arg)++;
    }
}

static int cmd_scan(void)
{
  struct nc_wifi_scan sc;
  uint8_t body[4];
  struct nc_wr w;
  int n = 0;
  int ret;

  memset(&sc, 0, sizeof(sc));
  nc_wr_init(&w, body, sizeof(body));
  nc_enc_wifi_scan(&w, &sc);
  ret = ncx_call1(NC_OP_WIFI_SCAN, body, w.n, NULL, scan_part, &n, 15000);
  if (ret < 0)
    {
      return no_hardware(ret);
    }

  if (n == 0)
    {
      printf("no networks found\n");
    }

  return EXIT_SUCCESS;
}

static int cmd_connect(const char *ssid, const char *pass)
{
  struct nc_wifi_connect c;
  struct nc_wifi_status st;
  struct ncx_rsp *rsp;
  uint8_t body[128];
  struct nc_wr w;
  struct nc_rd rd;
  int ret;

  memset(&c, 0, sizeof(c));
  c.flags     = NC_WIFI_CONN_SAVE;
  c.timeout_s = 20;
  strlcpy(c.ssid, ssid, sizeof(c.ssid));
  strlcpy(c.password, pass != NULL ? pass : "", sizeof(c.password));
  nc_wr_init(&w, body, sizeof(body));
  nc_enc_wifi_connect(&w, &c);

  rsp = malloc(sizeof(*rsp));
  if (rsp == NULL)
    {
      return EXIT_FAILURE;
    }

  printf("connecting to %s...\n", ssid);
  ret = ncx_call1(NC_OP_WIFI_CONNECT, body, w.n, rsp, NULL, NULL, 30000);
  if (ret == -ENOTCONN || ret == -ENETDOWN || ret == -EAFNOSUPPORT)
    {
      free(rsp);
      return no_hardware(ret);
    }

  if (ret < 0)
    {
      printf("connect failed: %s\n",
             ret == -EACCES ? "wrong password" :
             ret == -ENOENT ? "network not found" :
             ret == -ETIMEDOUT ? "timed out" : strerror(-ret));
    }

  nc_rd_init(&rd, rsp->body, rsp->n);
  if (rsp->n > 0 && nc_dec_wifi_status(&rd, &st) == 0)
    {
      print_status(&st);
    }

  free(rsp);
  return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}

static int simple(uint16_t op, const void *body, size_t n, const char *what)
{
  int ret = ncx_call1(op, body, n, NULL, NULL, NULL, 10000);

  if (ret < 0)
    {
      if (ret == -ENOTCONN || ret == -ENETDOWN || ret == -EAFNOSUPPORT)
        {
          return no_hardware(ret);
        }

      printf("%s: %s\n", what, ret == -ENOENT ? "not found" :
             strerror(-ret));
      return EXIT_FAILURE;
    }

  return EXIT_SUCCESS;
}

static void saved_part(void *arg, const uint8_t *p, size_t n)
{
  struct nc_wifi_saved sv;
  struct nc_rd rd;
  int cnt;

  nc_rd_init(&rd, p, n);
  for (cnt = nc_rd_u8(&rd); cnt > 0; cnt--)
    {
      if (nc_dec_wifi_saved(&rd, &sv) != 0)
        {
          break;
        }

      printf("%c %-32s %s\n", sv.flags & NC_WIFI_AP_CURRENT ? '*' : ' ',
             sv.ssid, ncx_wifi_auth_str(sv.auth));
      (*(int *)arg)++;
    }
}

static int cmd_config(int mask, int autoconnect, const char *hostname)
{
  struct nc_wifi_config c;
  struct ncx_rsp *rsp = malloc(sizeof(*rsp));
  uint8_t body[48];
  struct nc_wr w;
  struct nc_rd rd;
  int ret;

  if (rsp == NULL)
    {
      return EXIT_FAILURE;
    }

  memset(&c, 0, sizeof(c));
  c.mask        = mask;
  c.autoconnect = autoconnect;
  strlcpy(c.hostname, hostname != NULL ? hostname : "", sizeof(c.hostname));
  nc_wr_init(&w, body, sizeof(body));
  nc_enc_wifi_config(&w, &c);
  ret = ncx_call1(NC_OP_WIFI_CONFIG, body, w.n, rsp, NULL, NULL, 5000);
  if (ret < 0)
    {
      free(rsp);
      return no_hardware(ret);
    }

  nc_rd_init(&rd, rsp->body, rsp->n);
  if (nc_dec_wifi_config(&rd, &c) == 0)
    {
      printf("auto-connect %s  hostname %s\n", c.autoconnect ? "on" : "off",
             c.hostname);
    }

  free(rsp);
  return EXIT_SUCCESS;
}

static int cmd_info(void)
{
  struct ncx_state st;
  struct nc_time_status ts;
  struct ncx_rsp *rsp;
  struct nc_rd rd;
  char m[20];
  int ret = ncx_state(&st);

  if (ret < 0)
    {
      return no_hardware(ret);
    }

  if (st.link != NCX_LINK_READY)
    {
      printf("coprocessor: %s\n", st.link == NCX_LINK_NONE ?
             "none (no answer on the link)" : "starting");
      return EXIT_SUCCESS;
    }

  printf("coprocessor: %s, firmware %s%s\n", st.platform, st.fw_version,
         st.caps & NC_CAP_EMULATED ? " (emulated radio)" : "");
  printf("link: %lu baud, up %lu time%s%s\n", (unsigned long)st.baud,
         (unsigned long)st.ups, st.ups == 1 ? "" : "s",
         st.restarted ? ", the coprocessor restarted" : "");
  printf("mac: %s  sockets %u  tls %u  ssh %u  ble %u\n",
         mac_str(st.mac, m), st.max_sockets, st.max_tls, st.max_ssh,
         st.max_ble);

  rsp = malloc(sizeof(*rsp));
  if (rsp != NULL &&
      ncx_call1(NC_OP_TIME_STATUS, NULL, 0, rsp, NULL, NULL, 3000) >= 0)
    {
      nc_rd_init(&rd, rsp->body, rsp->n);
      if (nc_dec_time_status(&rd, &ts) == 0)
        {
          time_t t = (time_t)(ts.unix_us / 1000000);

          printf("its clock: %s", ts.source == NC_TIME_NONE ? "not set\n" :
                 ctime(&t));
          if (ts.source != NC_TIME_NONE)
            {
              printf("  from %s\n", ts.source == NC_TIME_SNTP ? "SNTP" :
                     "the fpc");
            }
        }
    }

  free(rsp);
  return EXIT_SUCCESS;
}

static void usage(void)
{
  fprintf(stderr,
          "usage: wifi [status | scan | connect SSID [PASSWORD] |\n"
          "             disconnect [-s] | saved | forget SSID|-a |\n"
          "             auto on|off | hostname [NAME] | info]\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  const char *cmd = argc > 1 ? argv[1] : "status";

#ifdef WIFI_AS_IFCONFIG
  if (argc <= 1 || strcmp(argv[1], "wlan0") == 0)
    {
      return cmd_status(true);
    }

  fprintf(stderr, "ifconfig: only wlan0 (the network coprocessor's "
                  "WiFi: see wifi)\n");
  return EXIT_FAILURE;
#endif

  if (strcmp(cmd, "status") == 0)
    {
      return cmd_status(false);
    }
  else if (strcmp(cmd, "scan") == 0)
    {
      return cmd_scan();
    }
  else if (strcmp(cmd, "connect") == 0 && argc >= 3)
    {
      return cmd_connect(argv[2], argc > 3 ? argv[3] : NULL);
    }
  else if (strcmp(cmd, "disconnect") == 0)
    {
      uint8_t flags = argc > 2 && strcmp(argv[2], "-s") == 0 ?
                      NC_WIFI_DISC_STAY : 0;

      return simple(NC_OP_WIFI_DISCONNECT, &flags, 1, "disconnect");
    }
  else if (strcmp(cmd, "saved") == 0)
    {
      int n = 0;
      int ret = ncx_call1(NC_OP_WIFI_SAVED, NULL, 0, NULL, saved_part, &n,
                          5000);

      if (ret < 0)
        {
          return no_hardware(ret);
        }

      if (n == 0)
        {
          printf("no saved networks\n");
        }

      return EXIT_SUCCESS;
    }
  else if (strcmp(cmd, "forget") == 0 && argc >= 3)
    {
      uint8_t body[NC_SSID_MAX + 2];
      struct nc_wr w;

      nc_wr_init(&w, body, sizeof(body));
      nc_wr_str8(&w, strcmp(argv[2], "-a") == 0 ? "" : argv[2]);
      return simple(NC_OP_WIFI_FORGET, body, w.n, "forget");
    }
  else if (strcmp(cmd, "auto") == 0 && argc >= 3)
    {
      return cmd_config(NC_WIFI_CFG_AUTOCONNECT,
                        strcmp(argv[2], "on") == 0, NULL);
    }
  else if (strcmp(cmd, "hostname") == 0)
    {
      return cmd_config(argc > 2 ? NC_WIFI_CFG_HOSTNAME : 0, 0,
                        argc > 2 ? argv[2] : NULL);
    }
  else if (strcmp(cmd, "info") == 0)
    {
      return cmd_info();
    }

  usage();
  return EXIT_FAILURE;
}
