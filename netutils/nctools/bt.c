/****************************************************************************
 * apps/netutils/nctools/bt.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * bt: the network coprocessor's BLE keyboards, mice and game pads
 * (docs/netcop.md 5.9).
 *
 *   bt                     the paired devices
 *   bt scan [SECONDS]      HID devices nearby
 *   bt pair ADDR           pair one (a passkey to type or confirm is
 *                          shown or asked for)
 *   bt forget ADDR|all     unpair
 *   bt disconnect ADDR
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "netutils/netcop.h"

static const char *kind_str(uint8_t k)
{
  return k & NC_BLE_KIND_KEYBOARD ? "keyboard" :
         k & NC_BLE_KIND_MOUSE ? "mouse" :
         k & NC_BLE_KIND_GAMEPAD ? "gamepad" : "other";
}

static char *mac_str(const uint8_t *a, char *buf)
{
  sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3],
          a[4], a[5]);
  return buf;
}

static int parse_mac(const char *s, uint8_t *a)
{
  unsigned int v[6];

  if (strcmp(s, "all") == 0)
    {
      memset(a, 0, 6);
      return 0;
    }

  if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4],
             &v[5]) != 6)
    {
      return -EINVAL;
    }

  for (int i = 0; i < 6; i++)
    {
      a[i] = v[i];
    }

  return 0;
}

static int fail(int ret, const char *what)
{
  fprintf(stderr, "bt: %s\n",
          ret == -ENOTCONN || ret == -ENETDOWN || ret == -EAFNOSUPPORT ?
          "No Bluetooth hardware (no network coprocessor)" :
          ret == -EACCES ? "pairing refused (wrong passkey?)" :
          ret == -ENOENT ? "device not found" :
          ret == -ETIMEDOUT ? "timed out" :
          ret == -EOPNOTSUPP ? "not a keyboard, mouse or game pad" :
          strerror(-ret));
  (void)what;
  return EXIT_FAILURE;
}

static void dev_print(const struct nc_ble_dev *d)
{
  char m[20];

  printf("%s  %-8s  %-12s  battery %s%d%s  %s\n", mac_str(d->addr, m),
         kind_str(d->kind),
         d->state == NC_BLE_ST_CONNECTED ? "connected" :
         d->state == NC_BLE_ST_CONNECTING ? "connecting" : "disconnected",
         d->battery == 0xff ? "?" : "", d->battery == 0xff ? 0 : d->battery,
         d->battery == 0xff ? "" : "%", d->name);
}

static void list_part(void *arg, const uint8_t *p, size_t n)
{
  struct nc_ble_dev d;
  struct nc_rd rd;
  int cnt;

  nc_rd_init(&rd, p, n);
  for (cnt = nc_rd_u8(&rd); cnt > 0; cnt--)
    {
      if (nc_dec_ble_dev(&rd, &d) != 0)
        {
          break;
        }

      dev_print(&d);
      (*(int *)arg)++;
    }
}

static void scan_part(void *arg, const uint8_t *p, size_t n)
{
  struct nc_ble_adv a;
  struct nc_rd rd;
  char m[20];
  int cnt;

  nc_rd_init(&rd, p, n);
  for (cnt = nc_rd_u8(&rd); cnt > 0; cnt--)
    {
      if (nc_dec_ble_adv(&rd, &a) != 0)
        {
          break;
        }

      printf("%s  %-8s %4d dBm  %s%s\n", mac_str(a.addr, m),
             kind_str(a.kind), a.rssi, a.name,
             a.flags & NC_BLE_DEV_BONDED ? "  (paired)" : "");
      fflush(stdout);
      (*(int *)arg)++;
    }
}

static int pair(const uint8_t *addr)
{
  struct nc_ble_pair pr;
  struct ncx_msg m;
  uint8_t *body;
  struct nc_wr w;
  struct nc_rd rd;
  int fd;
  int ret;

  fd = ncx_open();
  if (fd < 0)
    {
      return fail(-ENETDOWN, "pair");
    }

  body = malloc(NCX_MAXMSG);
  if (body == NULL)
    {
      close(fd);
      return EXIT_FAILURE;
    }

  ncx_subscribe(fd, NCX_EVM_BLE);
  memset(&pr, 0, sizeof(pr));
  memcpy(pr.addr, addr, 6);
  pr.timeout_s = 30;
  nc_wr_init(&w, body, 32);
  nc_enc_ble_pair(&w, &pr);
  ret = ncx_send(fd, 1, NC_OP_BLE_PAIR, body, w.n);
  printf("pairing...\n");
  while (ret == 0)
    {
      struct pollfd pfd;
      ssize_t n;

      pfd.fd = fd;
      pfd.events = POLLIN;
      if (poll(&pfd, 1, 40000) <= 0)
        {
          ret = -ETIMEDOUT;
          break;
        }

      n = ncx_recv(fd, &m, body, NCX_MAXMSG);
      if (n < 0)
        {
          ret = -errno;
          break;
        }

      if (m.kind == NCX_MSG_EVENT && m.code == NC_EV_BLE_PASSKEY)
        {
          struct nc_ble_passkey pk;
          uint8_t rb[16];
          char line[16];

          nc_rd_init(&rd, body, n);
          if (nc_dec_ble_passkey(&rd, &pk) != 0 ||
              memcmp(pk.addr, addr, 6) != 0)
            {
              continue;
            }

          if (pk.action == NC_BLE_PK_DISPLAY)
            {
              printf("type %06lu on the keyboard, then Enter\n",
                     (unsigned long)pk.passkey);
              fflush(stdout);
              continue;
            }

          if (pk.action == NC_BLE_PK_CONFIRM)
            {
              printf("does the device show %06lu (yes/no)? ",
                     (unsigned long)pk.passkey);
            }
          else
            {
              printf("the passkey the device shows: ");
            }

          fflush(stdout);
          if (fgets(line, sizeof(line), stdin) == NULL)
            {
              line[0] = '\0';
            }

          if (pk.action == NC_BLE_PK_CONFIRM)
            {
              pk.action = strncmp(line, "yes", 3) == 0;   /* Accept? */
            }
          else
            {
              pk.action  = 1;
              pk.passkey = strtoul(line, NULL, 10);
            }

          nc_wr_init(&w, rb, sizeof(rb));
          nc_enc_ble_passkey(&w, &pk);
          ncx_send(fd, 2, NC_OP_BLE_PASSKEY_REPLY, rb, w.n);
          continue;
        }

      if (m.kind == NCX_MSG_FINAL && m.tag == 1)
        {
          struct nc_ble_dev d;

          ret = m.status;
          nc_rd_init(&rd, body, n);
          if (ret >= 0 && nc_dec_ble_dev(&rd, &d) == 0)
            {
              printf("paired: ");
              dev_print(&d);
            }

          break;
        }
    }

  free(body);
  close(fd);
  return ret < 0 ? fail(ret, "pair") : EXIT_SUCCESS;
}

int main(int argc, FAR char *argv[])
{
  const char *cmd = argc > 1 ? argv[1] : "list";
  uint8_t addr[6];
  int n = 0;
  int ret;

  if (strcmp(cmd, "list") == 0)
    {
      ret = ncx_call1(NC_OP_BLE_LIST, NULL, 0, NULL, list_part, &n, 5000);
      if (ret < 0)
        {
          return fail(ret, "list");
        }

      if (n == 0)
        {
          printf("no paired devices\n");
        }

      return EXIT_SUCCESS;
    }

  if (strcmp(cmd, "scan") == 0)
    {
      struct nc_ble_scan sc;
      uint8_t body[8];
      struct nc_wr w;

      memset(&sc, 0, sizeof(sc));
      sc.duration_ms = (argc > 2 ? atoi(argv[2]) : 5) * 1000;
      sc.flags = NC_BLE_SCAN_HID_ONLY;
      nc_wr_init(&w, body, sizeof(body));
      nc_enc_ble_scan(&w, &sc);
      ret = ncx_call1(NC_OP_BLE_SCAN, body, w.n, NULL, scan_part, &n,
                      sc.duration_ms + 10000);
      if (ret < 0)
        {
          return fail(ret, "scan");
        }

      if (n == 0)
        {
          printf("nothing found\n");
        }

      return EXIT_SUCCESS;
    }

  if (argc < 3 || parse_mac(argv[2], addr) < 0)
    {
      fprintf(stderr, "usage: bt [list | scan [SECONDS] | pair ADDR |\n"
                      "           forget ADDR|all | disconnect ADDR]\n");
      return EXIT_FAILURE;
    }

  if (strcmp(cmd, "pair") == 0)
    {
      return pair(addr);
    }

  ret = ncx_call1(strcmp(cmd, "forget") == 0 ? NC_OP_BLE_FORGET :
                  NC_OP_BLE_DISCONNECT, addr, 6, NULL, NULL, NULL, 10000);
  return ret < 0 ? fail(ret, cmd) : EXIT_SUCCESS;
}
