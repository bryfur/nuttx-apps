/****************************************************************************
 * apps/netutils/netcop/netcopd.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * netcopd: the fpc's side of the network coprocessor (docs/netcop.md,
 * section 7).  One thread owns the UART to the coprocessor (the netcop
 * link, netcop/core's nc_host) and /dev/usrsock, and polls both:
 *
 *   - usrsock requests become netcop requests and channels, so ordinary
 *     sockets work through the coprocessor (ncd_sock.c);
 *   - the coprocessor's BLE input goes to /dev/vinput (ncd_hid.c);
 *   - control sockets (socket(AF_INET, SOCK_RAW, NCX_PROTO_CTRL)) carry any
 *     other request from the tools and the Settings, and the events they
 *     subscribe to (here);
 *   - the WiFi's DNS servers go to NuttX's resolver, and once WiFi is up
 *     NuttX's NTP client sets the clock, which is handed on to the
 *     coprocessor (TIME_SET) as its fallback.
 *
 * No coprocessor is a normal, quiet state: the link keeps offering SYNC at
 * 115200 baud and nothing is logged until one answers.  When it comes the
 * link is set up, HELLO said and its state read; when it goes (or
 * restarts) every socket is aborted.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <netdb.h>
#include <nuttx/net/dns.h>
#include <nuttx/net/usrsock.h>

#ifdef CONFIG_NETUTILS_NTPCLIENT
#  include "netutils/ntpclient.h"
#endif

#include "ncd.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#ifndef CONFIG_NETCOP_DEVPATH
#  define CONFIG_NETCOP_DEVPATH "/dev/ttyS0"
#endif

#ifndef CONFIG_NETCOP_BAUD
#  define CONFIG_NETCOP_BAUD 3000000
#endif

#ifndef CONFIG_NETCOP_WINDOW
#  define CONFIG_NETCOP_WINDOW 8192
#endif

#ifndef CONFIG_NETCOP_NTP_SERVERS
#  define CONFIG_NETCOP_NTP_SERVERS "pool.ntp.org"
#endif

#ifndef CONFIG_NETCOP_HOSTNAME
#  define CONFIG_NETCOP_HOSTNAME "fpc"
#endif

/* A clock before this is not worth giving the coprocessor */

#define TIME_SANE   1735689600              /* 2025-01-01 */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct ctrl_req                              /* A control request's state */
{
  uint16_t id;
  uint16_t gen;
  uint16_t tag;
  uint16_t op;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

struct ncd_s *g_ncd;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

uint32_t ncd_now(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* The port: the UART */

static int port_write(FAR void *arg, FAR const uint8_t *p, size_t n)
{
  ssize_t k = write(g_ncd->uart, p, n);

  return k < 0 ? 0 : (int)k;
}

static uint32_t port_now(FAR void *arg)
{
  return ncd_now();
}

#ifdef CONFIG_SERIAL_TERMIOS
static speed_t baud_code(uint32_t baud)
{
  switch (baud)
    {
      case 230400:  return B230400;
      case 460800:  return B460800;
      case 921600:  return B921600;
      case 1000000: return B1000000;
      case 2000000: return B2000000;
      case 3000000: return B3000000;
      default:      return B115200;
    }
}
#endif

static void port_baud(FAR void *arg, uint32_t baud)
{
#ifdef CONFIG_SERIAL_TERMIOS
  struct termios t;

  if (tcgetattr(g_ncd->uart, &t) == 0)
    {
      cfsetispeed(&t, baud_code(baud));
      cfsetospeed(&t, baud_code(baud));
      tcsetattr(g_ncd->uart, TCSANOW, &t);
    }
#endif
}

/* The UART has sent everything.  Never while a received frame is being
 * handled: a request made from a callback polls the link, and a baud
 * switch then would clear the receive buffer under the frame being parsed
 * (nc_link.c: rx_parse() drops the frame's length after process_frame())
 */

static bool port_idle(FAR void *arg)
{
  int n = 0;

  if (g_ncd->in_input)
    {
      return false;
    }

  return ioctl(g_ncd->uart, FIONWRITE, (unsigned long)&n) < 0 || n == 0;
}

static int uart_open(void)
{
  int fd = open(CONFIG_NETCOP_DEVPATH, O_RDWR | O_NONBLOCK | O_NOCTTY);

  if (fd < 0)
    {
      return -errno;
    }

#ifdef CONFIG_SERIAL_TERMIOS
  struct termios t;

  if (tcgetattr(fd, &t) == 0)
    {
      cfmakeraw(&t);
#ifdef CONFIG_NETCOP_FLOWCONTROL
      t.c_cflag |= CRTSCTS;
#endif
      cfsetispeed(&t, B115200);
      cfsetospeed(&t, B115200);
      tcsetattr(fd, TCSANOW, &t);
    }
#endif

  return fd;
}

/* Control sockets: their queues */

static void ctrl_queue(FAR struct ncd_sock *s, uint8_t kind, uint16_t tag,
                       int32_t status, uint16_t code, FAR const void *body,
                       size_t n)
{
  FAR struct ncd_msg *m;
  struct ncx_msg h;

  /* Events give way first; answers may go to twice the limit */

  if (s->qbytes + n > (kind == NCX_MSG_EVENT ? NCD_CTRL_QMAX :
                       2 * NCD_CTRL_QMAX))
    {
      return;
    }

  m = malloc(sizeof(*m) + sizeof(h) + n);
  if (m == NULL)
    {
      return;
    }

  memset(&h, 0, sizeof(h));
  h.kind   = kind;
  h.tag    = tag;
  h.status = status;
  h.code   = code;
  memcpy(m->data, &h, sizeof(h));
  if (n > 0)
    {
      memcpy(m->data + sizeof(h), body, n);
    }

  m->len  = sizeof(h) + n;
  m->next = NULL;
  if (s->qtail != NULL)
    {
      s->qtail->next = m;
    }
  else
    {
      s->qhead = m;
    }

  s->qtail   = m;
  s->qbytes += m->len;
  if (!s->sig_recv)
    {
      s->sig_recv = true;
      ncd_us_event(s->id, USRSOCK_EVENT_RECVFROM_AVAIL);
    }
}

bool ncd_ctrl_take(FAR struct ncd_sock *s, FAR struct ncd_msg **m)
{
  if (s->qhead == NULL)
    {
      return false;
    }

  *m = s->qhead;
  s->qhead = (*m)->next;
  if (s->qhead == NULL)
    {
      s->qtail = NULL;
    }

  s->qbytes -= (*m)->len;
  return true;
}

void ncd_ctrl_free(FAR struct ncd_sock *s)
{
  FAR struct ncd_msg *m;

  while (ncd_ctrl_take(s, &m))
    {
      free(m);
    }
}

void ncd_ctrl_event(uint32_t mask, uint16_t code, FAR const void *body,
                    size_t n)
{
  int i;

  for (i = NCD_CTRL0; i < NCD_NSOCK; i++)
    {
      FAR struct ncd_sock *s = g_ncd->sock[i];

      if (s != NULL && (s->evmask & mask) != 0)
        {
          ctrl_queue(s, NCX_MSG_EVENT, 0, 0, code, body, n);
        }
    }
}

static void state_fill(FAR struct ncx_state *st)
{
  memset(st, 0, sizeof(*st));
  st->link      = g_ncd->ready ? NCX_LINK_READY :
                  nc_host_up(&g_ncd->h) ? NCX_LINK_SYNC : NCX_LINK_NONE;
  st->restarted = g_ncd->restarted;
  st->ups       = g_ncd->ups;
  st->baud      = g_ncd->h.link.baud;
  if (g_ncd->ready)
    {
      st->caps        = g_ncd->hello.caps;
      st->max_sockets = g_ncd->hello.max_sockets;
      st->max_tls     = g_ncd->hello.max_tls;
      st->max_ssh     = g_ncd->hello.max_ssh;
      st->max_ble     = g_ncd->hello.max_ble;
      memcpy(st->mac, g_ncd->hello.mac, 6);
      strlcpy(st->fw_version, g_ncd->hello.fw_version,
              sizeof(st->fw_version));
      strlcpy(st->platform, g_ncd->hello.platform, sizeof(st->platform));
    }
}

static void link_event(void)
{
  struct ncx_state st;

  state_fill(&st);
  ncd_ctrl_event(NCX_EVM_LINK, NCX_EV_LINK, &st, sizeof(st));
}

/* The bonded BLE devices, as last heard */

static void ble_update(FAR const struct nc_ble_dev *d)
{
  int i;

  for (i = 0; i < g_ncd->nble; i++)
    {
      if (memcmp(g_ncd->ble[i].addr, d->addr, 6) == 0)
        {
          break;
        }
    }

  if (i == g_ncd->nble)
    {
      if (g_ncd->nble == sizeof(g_ncd->ble) / sizeof(g_ncd->ble[0]))
        {
          return;
        }

      g_ncd->nble++;
    }

  g_ncd->ble[i] = *d;
}

static void cb_ble_list(FAR struct nc_host *h, FAR void *arg,
                        FAR const struct nc_rsp *r)
{
  struct nc_ble_dev d;
  struct nc_rd rd;
  int n;

  if (r->final)
    {
      /* Each as an event too: who subscribed may have asked before */

      for (n = 0; n < g_ncd->nble; n++)
        {
          uint8_t body[64];
          struct nc_wr w;

          nc_wr_init(&w, body, sizeof(body));
          nc_enc_ble_dev(&w, &g_ncd->ble[n]);
          ncd_ctrl_event(NCX_EVM_BLE, NC_EV_BLE_CONN, body, w.n);
        }

      return;
    }

  nc_rd_init(&rd, r->body, r->n);
  for (n = nc_rd_u8(&rd); n > 0 && !rd.err; n--)
    {
      if (nc_dec_ble_dev(&rd, &d) == 0)
        {
          ble_update(&d);
        }
    }
}

static void ble_refresh(void)
{
  g_ncd->nble = 0;
  nc_host_request(&g_ncd->h, 0, NC_OP_BLE_LIST, NULL, 0, cb_ble_list,
                  NULL);
}

/* The WiFi: DNS servers, and the clock once it is up */

static void dns_set(FAR const struct nc_wifi_status *w)
{
  struct sockaddr_in sa;

  while (dns_del_nameserver_by_index(0) >= 0)
    {
    }

  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port   = htons(53);
  if (memcmp(w->dns1, "\0\0\0\0", 4) != 0)
    {
      memcpy(&sa.sin_addr, w->dns1, 4);
      dns_add_nameserver((FAR struct sockaddr *)&sa, sizeof(sa));
    }

  if (memcmp(w->dns2, "\0\0\0\0", 4) != 0 &&
      memcmp(w->dns2, w->dns1, 4) != 0)
    {
      memcpy(&sa.sin_addr, w->dns2, 4);
      dns_add_nameserver((FAR struct sockaddr *)&sa, sizeof(sa));
    }
}

static void wifi_update(FAR const uint8_t *p, size_t n)
{
  struct nc_wifi_status w;
  struct nc_rd rd;
  bool was_up;

  nc_rd_init(&rd, p, n);
  if (nc_dec_wifi_status(&rd, &w) != 0)
    {
      return;
    }

  was_up = g_ncd->have_wifi && g_ncd->wifi.state == NC_WIFI_UP;
  if (w.state == NC_WIFI_UP &&
      (!was_up || memcmp(w.dns1, g_ncd->wifi.dns1, 4) != 0 ||
       memcmp(w.dns2, g_ncd->wifi.dns2, 4) != 0))
    {
      dns_set(&w);
    }

  if (w.state == NC_WIFI_UP && !was_up)
    {
      ncd_info("WiFi up: %s, %u.%u.%u.%u\n", w.ssid, w.ip[0], w.ip[1],
               w.ip[2], w.ip[3]);
    }
  else if (w.state != NC_WIFI_UP && was_up)
    {
      ncd_info("WiFi down (%s)\n", ncx_wifi_reason_str(w.reason));
    }

  g_ncd->wifi      = w;
  g_ncd->have_wifi = true;
  g_ncd->wifi_n    = n < sizeof(g_ncd->wifi_body) ? n :
                     sizeof(g_ncd->wifi_body);
  memcpy(g_ncd->wifi_body, p, g_ncd->wifi_n);

#ifdef CONFIG_NETUTILS_NTPCLIENT
  if (w.state == NC_WIFI_UP && !g_ncd->ntp_running)
    {
      if (ntpc_start_with_list(CONFIG_NETCOP_NTP_SERVERS) >= 0)
        {
          g_ncd->ntp_running  = true;
          g_ncd->ntp_check_ms = ncd_now() + 1000;
        }
    }
#endif
}

static void cb_wifi(FAR struct nc_host *h, FAR void *arg,
                    FAR const struct nc_rsp *r)
{
  if (r->final && r->n > 0 &&
      (r->status >= 0 || r->op == NC_OP_WIFI_CONNECT))
    {
      wifi_update(r->body, r->n);

      /* As an event too: who subscribed may have asked before */

      ncd_ctrl_event(NCX_EVM_WIFI, NC_EV_WIFI_STATE, r->body, r->n);
    }
}

/* The clock: the coprocessor's fallback (TIME_SET) */

static void time_give(void)
{
  struct timespec ts;
  uint8_t body[8];
  struct nc_wr w;

  clock_gettime(CLOCK_REALTIME, &ts);
  if (!g_ncd->ready || ts.tv_sec < TIME_SANE)
    {
      return;
    }

  nc_wr_init(&w, body, sizeof(body));
  nc_wr_u64(&w, (uint64_t)ts.tv_sec * 1000000 + ts.tv_nsec / 1000);
  nc_host_request(&g_ncd->h, 0, NC_OP_TIME_SET, body, w.n, NULL, NULL);
  g_ncd->timeset_ms = ncd_now();
}

#ifdef CONFIG_NETUTILS_NTPCLIENT
static void ntp_check(void)
{
  FAR struct ntpc_status_s *st;

  if (!g_ncd->ntp_running ||
      (int32_t)(ncd_now() - g_ncd->ntp_check_ms) < 0)
    {
      return;
    }

  g_ncd->ntp_check_ms = ncd_now() + (g_ncd->ntp_synced ? 30000 : 1000);
  st = malloc(sizeof(*st));
  if (st == NULL)
    {
      return;
    }

  if (ntpc_status(st) == 0 && st->nsamples > 0 &&
      (!g_ncd->ntp_synced || st->nsamples != g_ncd->ntp_nsamples ||
       st->samples[0].offset != g_ncd->ntp_offset))
    {
      bool first = !g_ncd->ntp_synced;

      g_ncd->ntp_synced   = true;
      g_ncd->ntp_nsamples = st->nsamples;
      g_ncd->ntp_offset   = st->samples[0].offset;     /* (2^-32 s) */
      g_ncd->ntp_last     = time(NULL);
      g_ncd->clock_valid  = true;
      if (first)
        {
          ncd_info("clock set by NTP\n");
        }

      time_give();
    }

  free(st);
}
#endif

/* After HELLO: what the coprocessor shows */

static void cb_hello(FAR struct nc_host *h, FAR void *arg,
                     FAR const struct nc_rsp *r)
{
  struct nc_rd rd;

  if (!r->final)
    {
      return;
    }

  nc_rd_init(&rd, r->body, r->n);
  if (r->status < 0 || nc_dec_hello_rsp(&rd, &g_ncd->hello) != 0)
    {
      ncd_err("HELLO: %d\n", (int)r->status);
      nc_link_reset(&g_ncd->h.link);
      return;
    }

  if (g_ncd->hello.major != NC_PROTO_MAJOR)
    {
      ncd_err("the coprocessor speaks protocol %u.%u, not %u: update its "
              "firmware\n", g_ncd->hello.major, g_ncd->hello.minor,
              NC_PROTO_MAJOR);
    }

  nc_host_set_max_chan(&g_ncd->h, g_ncd->hello.max_chan);
  g_ncd->ready = true;
  ncd_info("coprocessor %s %s up%s\n", g_ncd->hello.platform,
           g_ncd->hello.fw_version,
           g_ncd->restarted && g_ncd->ups > 1 ? " (it restarted)" : "");

  nc_host_request(&g_ncd->h, 0, NC_OP_WIFI_STATUS, NULL, 0, cb_wifi, NULL);
  ble_refresh();
  if (g_ncd->clock_valid)
    {
      time_give();
    }

  link_event();
}

static void hello(void)
{
  struct nc_hello_req hq;
  uint8_t body[48];
  struct nc_wr w;

  memset(&hq, 0, sizeof(hq));
  hq.major = NC_PROTO_MAJOR;
  hq.minor = NC_PROTO_MINOR;
  strlcpy(hq.name, CONFIG_NETCOP_HOSTNAME, sizeof(hq.name));
  nc_wr_init(&w, body, sizeof(body));
  nc_enc_hello_req(&w, &hq);
  nc_host_request(&g_ncd->h, 0, NC_OP_HELLO, body, w.n, cb_hello, NULL);
}

/* The core's callbacks */

static void on_event(FAR struct nc_host *h, uint8_t chan, uint16_t code,
                     FAR const uint8_t *p, size_t n)
{
  if (chan != 0)
    {
      ncd_sock_event(chan, code, p, n);
      return;
    }

  switch (code)
    {
      case NC_EV_WIFI_STATE:
        wifi_update(p, n);
        ncd_ctrl_event(NCX_EVM_WIFI, code, p, n);
        break;

      case NC_EV_BLE_KEY:
      case NC_EV_BLE_MOUSE:
      case NC_EV_BLE_GAMEPAD:
        ncd_hid_event(code, p, n);
        ncd_ctrl_event(NCX_EVM_INPUT, code, p, n);
        break;

      case NC_EV_BLE_CONN:
        {
          struct nc_ble_dev d;
          struct nc_rd rd;

          nc_rd_init(&rd, p, n);
          if (nc_dec_ble_dev(&rd, &d) == 0)
            {
              ble_update(&d);
            }

          ncd_hid_event(code, p, n);
          ncd_ctrl_event(NCX_EVM_BLE, code, p, n);
        }
        break;

      case NC_EV_BLE_BATTERY:
        {
          struct nc_ble_battery b;
          struct nc_rd rd;
          int i;

          nc_rd_init(&rd, p, n);
          if (nc_dec_ble_battery(&rd, &b) == 0)
            {
              for (i = 0; i < g_ncd->nble; i++)
                {
                  if (g_ncd->ble[i].id == b.dev)
                    {
                      g_ncd->ble[i].battery = b.percent;
                    }
                }
            }

          ncd_ctrl_event(NCX_EVM_BLE, code, p, n);
        }
        break;

      case NC_EV_BLE_PASSKEY:
        ncd_ctrl_event(NCX_EVM_BLE, code, p, n);
        break;

      case NC_EV_LOG:
        ncd_ctrl_event(NCX_EVM_LOG, code, p, n);
        break;

      default:
        break;
    }
}

static void on_data(FAR struct nc_host *h, uint8_t chan, uint8_t type,
                    FAR const uint8_t *p, size_t n)
{
  ncd_sock_data(chan, type, p, n);
}

static void on_credit(FAR struct nc_host *h, uint8_t chan)
{
  ncd_sock_credit(chan);
}

static void on_link(FAR struct nc_host *h, int ev, uint32_t val)
{
  switch (ev)
    {
      case NC_LINK_EV_UP:
        g_ncd->ups++;
        g_ncd->restarted = val != 0;
        g_ncd->ready     = false;
        hello();
        link_event();
        break;

      case NC_LINK_EV_DOWN:
        if (g_ncd->ready)
          {
            ncd_info("coprocessor gone\n");
          }

        g_ncd->ready     = false;
        g_ncd->have_wifi = false;
        g_ncd->nble      = 0;
        ncd_sock_link_down();
        link_event();
        break;

      case NC_LINK_EV_BAUD:
        g_ncd->baud = val;
        break;
    }
}

static const struct nc_host_ops g_ops =
{
  on_event,
  on_data,
  on_credit,
  on_link,
};

/* Control requests: answers back to the socket that asked */

static void cb_ctrl(FAR struct nc_host *h, FAR void *arg,
                    FAR const struct nc_rsp *r)
{
  FAR struct ctrl_req *c = arg;
  FAR struct ncd_sock *s = g_ncd->sock[c->id];

  if (s != NULL && s->gen == c->gen && s->kind == NCD_CTRL)
    {
      ctrl_queue(s, r->final ? NCX_MSG_FINAL : NCX_MSG_PART, c->tag,
                 r->status, r->op, r->body, r->n);
    }

  if (!r->final)
    {
      return;
    }

  /* What netcopd keeps up to date */

  switch (c->op)
    {
      case NC_OP_WIFI_CONNECT:
      case NC_OP_WIFI_STATUS:
        cb_wifi(h, NULL, r);
        break;

      case NC_OP_BLE_PAIR:
      case NC_OP_BLE_FORGET:
        if (g_ncd->ready)
          {
            ble_refresh();
          }
        break;

      default:
        break;
    }

  free(c);
}

static void ctrl_answer(FAR struct ncd_sock *s, uint16_t tag, uint16_t op,
                        int32_t status, FAR const void *body, size_t n)
{
  ctrl_queue(s, NCX_MSG_FINAL, tag, status, op, body, n);
}

int ncd_ctrl_request(FAR struct ncd_sock *s, FAR const uint8_t *p, size_t n)
{
  FAR struct ctrl_req *c;
  struct ncx_req rq;
  int ret;

  if (n < sizeof(rq))
    {
      return -EINVAL;
    }

  memcpy(&rq, p, sizeof(rq));
  p += sizeof(rq);
  n -= sizeof(rq);

  switch (rq.op)
    {
      case NCX_OP_STATE:
        {
          struct ncx_state st;

          state_fill(&st);
          ctrl_answer(s, rq.tag, rq.op, sizeof(st), &st, sizeof(st));
        }
        return 0;

      case NCX_OP_SUBSCRIBE:
        if (n >= 4)
          {
            s->evmask = p[0] | (p[1] << 8) | (p[2] << 16) |
                        ((uint32_t)p[3] << 24);
          }

        ctrl_answer(s, rq.tag, rq.op, 0, NULL, 0);
        return 0;

      case NCX_OP_WIFI:
        ctrl_answer(s, rq.tag, rq.op,
                    g_ncd->have_wifi ? g_ncd->wifi_n : -ENOENT,
                    g_ncd->wifi_body, g_ncd->have_wifi ? g_ncd->wifi_n : 0);
        return 0;

      case NCX_OP_BLE:
        {
          uint8_t body[1 + 8 * 48];
          struct nc_wr w;
          int i;

          nc_wr_init(&w, body, sizeof(body));
          nc_wr_u8(&w, g_ncd->nble);
          for (i = 0; i < g_ncd->nble; i++)
            {
              nc_enc_ble_dev(&w, &g_ncd->ble[i]);
            }

          ctrl_queue(s, NCX_MSG_PART, rq.tag, 0, rq.op, body, w.n);
          body[0] = g_ncd->nble;
          body[1] = 0;
          ctrl_answer(s, rq.tag, rq.op, 0, body, 2);
        }
        return 0;

      case NCX_OP_NTP:
        {
          struct ncx_ntp nt;

          memset(&nt, 0, sizeof(nt));
          nt.running   = g_ncd->ntp_running;
          nt.synced    = g_ncd->ntp_synced;
          nt.last      = g_ncd->ntp_last;
          nt.offset_us = (int64_t)((double)g_ncd->ntp_offset * 1e6 /
                                   4294967296.0);
          strlcpy(nt.server, CONFIG_NETCOP_NTP_SERVERS, sizeof(nt.server));
          ctrl_answer(s, rq.tag, rq.op, sizeof(nt), &nt, sizeof(nt));
        }
        return 0;

      default:
        break;
    }

  if (rq.op >= 0xff00)
    {
      ctrl_answer(s, rq.tag, rq.op, -ENOSYS, NULL, 0);
      return 0;
    }

  if (!g_ncd->ready && rq.op != NC_OP_HELLO)
    {
      ctrl_answer(s, rq.tag, rq.op, -ENOTCONN, NULL, 0);
      return 0;
    }

  c = malloc(sizeof(*c));
  if (c == NULL)
    {
      return -ENOMEM;
    }

  c->id  = s->id;
  c->gen = s->gen;
  c->tag = rq.tag;
  c->op  = rq.op;
  ret = nc_host_request(&g_ncd->h, 0, rq.op, p, n, cb_ctrl, c);
  if (ret < 0)
    {
      free(c);
      ctrl_answer(s, rq.tag, rq.op, ret == -NC_EAGAIN ? -EBUSY : ret,
                  NULL, 0);
    }

  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct nc_host_cfg cfg;
  struct nc_port port;
  struct pollfd pfd[2];
  struct timespec ts;
  int fd;

  if (g_ncd != NULL)
    {
      fprintf(stderr, "netcopd: already running\n");
      return EXIT_FAILURE;
    }

  g_ncd = calloc(1, sizeof(*g_ncd));
  if (g_ncd == NULL)
    {
      ncd_err("no memory\n");
      return EXIT_FAILURE;
    }

  g_ncd->window = CONFIG_NETCOP_WINDOW;
#ifdef CONFIG_NETCOP_CLOCK_VALID
  g_ncd->clock_valid = true;
#endif

  g_ncd->usock = open("/dev/usrsock", O_RDWR | O_NONBLOCK);
  if (g_ncd->usock < 0)
    {
      ncd_err("/dev/usrsock: %d\n", errno);
      goto errout;
    }

  fd = uart_open();
  if (fd < 0)
    {
      ncd_err("%s: %d\n", CONFIG_NETCOP_DEVPATH, fd);
      goto errout;
    }

  g_ncd->uart = fd;
  ncd_hid_init();

  clock_gettime(CLOCK_REALTIME, &ts);
  memset(&cfg, 0, sizeof(cfg));
  cfg.boot_id     = (uint32_t)(ts.tv_nsec ^ (ts.tv_sec << 12) ^
                               (uintptr_t)g_ncd) | 1;
  cfg.baud_target = CONFIG_NETCOP_BAUD;

  memset(&port, 0, sizeof(port));
  port.write    = port_write;
  port.now_ms   = port_now;
  port.set_baud = port_baud;
  port.tx_idle  = port_idle;

  nc_host_init(&g_ncd->h, &cfg, &port, &g_ops, NULL);

  pfd[0].fd = g_ncd->uart;
  pfd[1].fd = g_ncd->usock;
  pfd[1].events = POLLIN;

  for (; ; )
    {
      uint8_t buf[512];
      ssize_t n;

      nc_host_poll(&g_ncd->h);
      ncd_sock_poll();
#ifdef CONFIG_NETUTILS_NTPCLIENT
      ntp_check();
#endif

      pfd[0].events  = POLLIN;
      if (nc_link_want_write(&g_ncd->h.link))
        {
          pfd[0].events |= POLLOUT;
        }

      pfd[0].revents = 0;
      pfd[1].revents = 0;
      if (poll(pfd, 2, nc_link_next_ms(&g_ncd->h.link)) < 0)
        {
          continue;
        }

      if (pfd[0].revents & (POLLIN | POLLERR | POLLHUP))
        {
          while ((n = read(g_ncd->uart, buf, sizeof(buf))) > 0)
            {
              g_ncd->in_input = true;
              nc_host_input(&g_ncd->h, buf, n);
              g_ncd->in_input = false;
            }

          if (n < 0 && errno != EAGAIN && errno != EINTR &&
              (pfd[0].revents & POLLIN) == 0)
            {
              usleep(100000);           /* Nobody on the line */
            }
        }

      if (pfd[1].revents & POLLIN)
        {
          ncd_usrsock_request();
        }
    }

errout:
  if (g_ncd->usock >= 0)
    {
      close(g_ncd->usock);
    }

  free(g_ncd);
  g_ncd = NULL;
  return EXIT_FAILURE;
}
