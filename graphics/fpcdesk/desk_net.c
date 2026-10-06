/****************************************************************************
 * apps/graphics/fpcdesk/desk_net.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The network coprocessor as the desktop sees it (netcopd's control
 * socket, netutils/netcop.h): a thread holds a control socket subscribed to
 * the link's, the WiFi's and the BLE devices' events, and hands what comes
 * to the main thread (EV_NET), which keeps g_net (the top bar's WiFi
 * indicator, Settings) and calls back whoever asked (desk_net_request()).
 *
 * No netcopd, or no coprocessor on the link, is a normal state: g_net says
 * so and the top bar shows no indicator.
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "desk.h"

#ifdef CONFIG_NETUTILS_NETCOP

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define TAG_STATE      1               /* netcopd's own requests */
#define TAG_WIFI       2
#define TAG_BLE        3
#define TAG_FIRST      16              /* desk_net_request()'s */
#define NPENDING       16

#define BAR_W          3               /* The top bar's indicator */
#define BAR_GAP        2
#define IND_W          (4 * BAR_W + 3 * BAR_GAP)

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct netmsg_s
{
  struct netmsg_s *next;
  struct ncx_msg   m;
  size_t           n;
  uint8_t          body[];
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

struct desk_net_s g_net;

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static struct netmsg_s *g_qhead;
static struct netmsg_s *g_qtail;
static volatile int g_fd = -1;
static uint16_t g_tag = TAG_FIRST;
static int g_indx = -1;                /* Where the indicator is drawn */

static struct
{
  uint16_t    tag;
  desk_net_cb cb;
  void       *arg;
} g_pending[NPENDING];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void push(const struct ncx_msg *m, const uint8_t *body, size_t n)
{
  struct netmsg_s *q = malloc(sizeof(*q) + n);
  bool first;

  if (q == NULL)
    {
      return;
    }

  q->next = NULL;
  q->m    = *m;
  q->n    = n;
  memcpy(q->body, body, n);
  pthread_mutex_lock(&g_lock);
  first = g_qhead == NULL;
  if (g_qtail != NULL)
    {
      g_qtail->next = q;
    }
  else
    {
      g_qhead = q;
    }

  g_qtail = q;
  pthread_mutex_unlock(&g_lock);
  if (first)
    {
      desk_post(EV_NET);
    }
}

/* The thread: a control socket (again, if netcopd is not there yet), what
 * comes on it to the main thread
 */

static void *net_thread(void *arg)
{
  struct ncx_msg m;
  uint8_t *body;
  ssize_t n;
  int fd;

  body = malloc(NCX_MAXMSG);
  if (body == NULL)
    {
      return NULL;
    }

  for (; ; )
    {
      fd = ncx_open();
      if (fd < 0)
        {
          sleep(10);                   /* No netcopd (yet) */
          continue;
        }

      ncx_send(fd, 0, NCX_OP_SUBSCRIBE, "\x07\0\0\0", 4);
      ncx_send(fd, TAG_STATE, NCX_OP_STATE, NULL, 0);
      g_fd = fd;
      memset(&m, 0, sizeof(m));
      m.kind = NCX_MSG_EVENT;
      m.code = 0;                      /* "netcopd is there" */
      push(&m, body, 0);

      while ((n = ncx_recv(fd, &m, body, NCX_MAXMSG)) >= 0 ||
             errno == EINTR || errno == EAGAIN)
        {
          if (n >= 0)
            {
              push(&m, body, n);
            }
        }

      g_fd = -1;
      close(fd);
      sleep(1);
    }

  return NULL;
}

static void ble_update(const struct nc_ble_dev *d)
{
  int i;

  for (i = 0; i < g_net.nble; i++)
    {
      if (memcmp(g_net.ble[i].addr, d->addr, 6) == 0)
        {
          break;
        }
    }

  if (i == g_net.nble)
    {
      if (i == DESK_NET_NBLE)
        {
          return;
        }

      g_net.nble++;
    }

  g_net.ble[i] = *d;
  g_net.blegen++;
}

static void ble_list(const uint8_t *p, size_t n)
{
  struct nc_ble_dev d;
  struct nc_rd rd;
  int cnt;

  nc_rd_init(&rd, p, n);
  for (cnt = nc_rd_u8(&rd); cnt > 0 && !rd.err; cnt--)
    {
      if (nc_dec_ble_dev(&rd, &d) == 0)
        {
          ble_update(&d);
        }
    }
}

static void wifi_set(const uint8_t *p, size_t n)
{
  struct nc_rd rd;

  nc_rd_init(&rd, p, n);
  g_net.have_wifi = nc_dec_wifi_status(&rd, &g_net.wifi) == 0;
}

static void link_set(const uint8_t *p, size_t n)
{
  struct ncx_state st;
  bool was = g_net.link == NCX_LINK_READY;

  memset(&st, 0, sizeof(st));
  memcpy(&st, p, n < sizeof(st) ? n : sizeof(st));
  g_net.link = st.link;
  g_net.caps = st.caps;
  if (st.link != NCX_LINK_READY)
    {
      g_net.have_wifi = false;
      g_net.nble      = 0;
      g_net.blegen++;
    }
  else if (!was && g_fd >= 0)
    {
      ncx_send(g_fd, TAG_WIFI, NCX_OP_WIFI, NULL, 0);
      ncx_send(g_fd, TAG_BLE, NCX_OP_BLE, NULL, 0);
    }
}

static void handle(struct netmsg_s *q)
{
  struct nc_rd rd;
  int i;

  if (q->m.kind == NCX_MSG_EVENT)
    {
      switch (q->m.code)
        {
          case 0:
            g_net.daemon = true;
            break;

          case NCX_EV_LINK:
            link_set(q->body, q->n);
            break;

          case NC_EV_WIFI_STATE:
            wifi_set(q->body, q->n);
            break;

          case NC_EV_BLE_CONN:
            {
              struct nc_ble_dev d;

              nc_rd_init(&rd, q->body, q->n);
              if (nc_dec_ble_dev(&rd, &d) == 0)
                {
                  ble_update(&d);
                }
            }
            break;

          case NC_EV_BLE_BATTERY:
            {
              struct nc_ble_battery b;

              nc_rd_init(&rd, q->body, q->n);
              if (nc_dec_ble_battery(&rd, &b) == 0)
                {
                  for (i = 0; i < g_net.nble; i++)
                    {
                      if (g_net.ble[i].id == b.dev)
                        {
                          g_net.ble[i].battery = b.percent;
                          g_net.blegen++;
                        }
                    }
                }
            }
            break;

          case NC_EV_BLE_PASSKEY:
            nc_rd_init(&rd, q->body, q->n);
            g_net.pk_pending = nc_dec_ble_passkey(&rd, &g_net.pk) == 0;
            break;

          default:
            break;
        }

      return;
    }

  switch (q->m.tag)
    {
      case TAG_STATE:
        link_set(q->body, q->n);
        return;

      case TAG_WIFI:
        if (q->m.status >= 0)
          {
            wifi_set(q->body, q->n);
          }
        return;

      case TAG_BLE:
        if (q->m.kind == NCX_MSG_PART)
          {
            g_net.nble = 0;
            g_net.blegen++;
            ble_list(q->body, q->n);
          }
        return;

      default:
        break;
    }

  for (i = 0; i < NPENDING; i++)
    {
      if (g_pending[i].cb != NULL && g_pending[i].tag == q->m.tag)
        {
          desk_net_cb cb = g_pending[i].cb;
          void *arg = g_pending[i].arg;

          if (q->m.kind == NCX_MSG_FINAL)
            {
              g_pending[i].cb = NULL;
            }

          cb(arg, &q->m, q->body, q->n);

          /* What netcopd shows has changed with it */

          if (q->m.kind == NCX_MSG_FINAL &&
              (q->m.code == NC_OP_WIFI_CONNECT ||
               q->m.code == NC_OP_WIFI_STATUS) && q->n > 0)
            {
              wifi_set(q->body, q->n);
            }

          if (q->m.kind == NCX_MSG_FINAL &&
              (q->m.code == NC_OP_BLE_PAIR ||
               q->m.code == NC_OP_BLE_FORGET) && g_fd >= 0)
            {
              ncx_send(g_fd, TAG_BLE, NCX_OP_BLE, NULL, 0);
            }

          return;
        }
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void desk_net_start(void)
{
  pthread_attr_t attr;
  pthread_t thread;

  pthread_attr_init(&attr);
  pthread_attr_setstacksize(&attr, 2048);
  if (pthread_create(&thread, &attr, net_thread, NULL) == 0)
    {
      pthread_setname_np(thread, "desk net");
    }
}

/* EV_NET: what has come, taken in */

void desk_net_poll(void)
{
  struct netmsg_s *q;
  bool any = false;

  for (; ; )
    {
      pthread_mutex_lock(&g_lock);
      q = g_qhead;
      if (q != NULL)
        {
          g_qhead = q->next;
          if (g_qhead == NULL)
            {
              g_qtail = NULL;
            }
        }

      pthread_mutex_unlock(&g_lock);
      if (q == NULL)
        {
          break;
        }

      handle(q);
      free(q);
      any = true;
    }

  if (any)
    {
      desk_topbar_draw();
      desk_settings_changed(SETTINGS_WIFI);
      desk_settings_changed(SETTINGS_BLUETOOTH);
    }
}

/* A request: its answers to cb on the main thread (parts, then the final
 * one).  0, or -errno (-ENETDOWN: no netcopd).
 */

int desk_net_request(uint16_t op, const void *body, size_t n,
                     desk_net_cb cb, void *arg)
{
  int fd = g_fd;
  int i;

  if (fd < 0)
    {
      return -ENETDOWN;
    }

  for (i = 0; i < NPENDING && g_pending[i].cb != NULL; i++)
    {
    }

  if (i == NPENDING)
    {
      return -EBUSY;
    }

  if (++g_tag < TAG_FIRST)
    {
      g_tag = TAG_FIRST;
    }

  g_pending[i].tag = g_tag;
  g_pending[i].cb  = cb;
  g_pending[i].arg = arg;
  if (ncx_send(fd, g_tag, op, body, n) < 0)
    {
      g_pending[i].cb = NULL;
      return -EIO;
    }

  return 0;
}

/* Whoever asked with arg is going: no more answers for it */

void desk_net_cancel(void *arg)
{
  int i;

  for (i = 0; i < NPENDING; i++)
    {
      if (g_pending[i].arg == arg)
        {
          g_pending[i].cb = NULL;
        }
    }
}

bool desk_net_wifi_hw(void)
{
  return g_net.daemon && g_net.link == NCX_LINK_READY &&
         (g_net.caps & NC_CAP_WIFI) != 0;
}

bool desk_net_ble_hw(void)
{
  return g_net.daemon && g_net.link == NCX_LINK_READY &&
         (g_net.caps & NC_CAP_BLE_HID) != 0;
}

/* The top bar's WiFi indicator, its right edge at x: four bars, as many
 * lit as the signal is strong (all dim: not connected).  Returns where
 * what is left of it may end; nothing without WiFi hardware.
 */

int desk_net_topbar(struct desk_ops_s *o, int x, int ty)
{
  uint16_t fg = g_theme.panel_text;
  bool up = g_net.have_wifi && g_net.wifi.state == NC_WIFI_UP;
  int bars = up ? ncx_rssi_bars(g_net.wifi.rssi) : 0;
  int left = x - IND_W;
  int i;

  g_indx = -1;
  if (!desk_net_wifi_hw())
    {
      return x;
    }

  g_indx = left;
  for (i = 0; i < 4; i++)
    {
      int h = 4 + 3 * i;

      ops_rrect(o, left + i * (BAR_W + BAR_GAP), 21 - h, BAR_W, h, LUT_R3,
                1, fg, i < bars || (up && i == 0) ? 255 : THEMED(70, 60));
    }

  (void)ty;
  return left - 12;
}

/* A click on the indicator: Settings, WiFi */

bool desk_net_topbar_click(int x)
{
  if (g_indx >= 0 && x >= g_indx - 4 && x < g_indx + IND_W + 4)
    {
      desk_settings_show(SETTINGS_WIFI);
      return true;
    }

  return false;
}

#endif /* CONFIG_NETUTILS_NETCOP */
