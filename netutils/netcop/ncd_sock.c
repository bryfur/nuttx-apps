/****************************************************************************
 * apps/netutils/netcop/ncd_sock.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * netcopd: /dev/usrsock requests as netcop requests and channels, and
 * channel data and events back as usrsock events (docs/netcop.md, 7.2 and
 * 7.3).
 *
 * A socket's usrsock id is its channel (1..255); control sockets, which are
 * netcopd's own, have ids from 256.  The kernel hands the daemon one request
 * at a time and waits for its ack before the next, so anything that has to
 * wait for the coprocessor is acked "in progress" at once (which frees the
 * device for the other sockets) and completed later with the same xid, as
 * arch/sim's usrsock does for connect.
 *
 * The flags the kernel keeps per socket (data to read, room to send) are
 * cleared by the kernel after each recvfrom / sendto whose ack does not set
 * them again, and set by events: sig_recv / sig_send track what the kernel
 * believes, so that an event is sent only when it would change something.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include <nuttx/net/usrsock.h>

#include "ncd.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define SOCK_ARG(s)  ((FAR void *)(uintptr_t)(((uint32_t)(s)->gen << 16) | \
                                              (s)->id))

#define NCD_NOREQ    0xff

#ifndef SOCK_TYPE_MASK
#  define SOCK_TYPE_MASK 0x0f
#endif

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static FAR struct ncd_sock *sock_of(FAR void *arg)
{
  uint32_t v = (uint32_t)(uintptr_t)arg;
  FAR struct ncd_sock *s;

  if ((v & 0xffff) >= NCD_NSOCK)
    {
      return NULL;
    }

  s = g_ncd->sock[v & 0xffff];
  return s != NULL && s->gen == (v >> 16) ? s : NULL;
}

static FAR struct ncd_sock *sock_get(int id)
{
  return id > 0 && id < NCD_NSOCK ? g_ncd->sock[id] : NULL;
}

/* usrsock messages */

static void us_write(FAR const void *p, size_t n)
{
  FAR const uint8_t *b = p;

  if (write(g_ncd->usock, p, n) < 0)
    {
      ncd_err("usrsock write: %d (msg %d flags %d events %x, %02x%02x%02x%02x"
              " %02x%02x%02x%02x, %u bytes)\n", errno, b[0], b[1],
              b[2] | (b[3] << 8), b[4], b[5], b[6], b[7], b[8], b[9], b[10],
              b[11], (unsigned)n);
    }
}

static void us_ack(uint32_t xid, int32_t result, uint16_t events,
                   bool inprogress)
{
  struct usrsock_message_req_ack_s ack;

  memset(&ack, 0, sizeof(ack));
  ack.head.msgid  = USRSOCK_MESSAGE_RESPONSE_ACK;
  ack.head.flags  = inprogress ? USRSOCK_MESSAGE_FLAG_REQ_IN_PROGRESS : 0;
  ack.head.events = events;
  ack.xid         = xid;
  ack.result      = inprogress ? -EINPROGRESS : result;
  us_write(&ack, sizeof(ack));
}

/* A data ack: result (>= 0: the bytes of data), the value (an address, an
 * option) of vlen bytes kept of vfull, then the data
 */

static void us_dack(uint32_t xid, int32_t result, uint16_t events,
                    FAR const void *val, uint16_t vlen, uint16_t vfull,
                    FAR const void *data, size_t dlen)
{
  FAR struct usrsock_message_datareq_ack_s *ack =
    (FAR struct usrsock_message_datareq_ack_s *)g_ncd->out;
  FAR uint8_t *p = (FAR uint8_t *)(ack + 1);

  memset(ack, 0, sizeof(*ack));
  ack->reqack.head.msgid  = USRSOCK_MESSAGE_RESPONSE_DATA_ACK;
  ack->reqack.head.events = events;
  ack->reqack.xid         = xid;
  ack->reqack.result      = result;
  if (result < 0)
    {
      vlen = vfull = 0;
      dlen = 0;
    }

  if (vlen > vfull)
    {
      vlen = vfull;
    }

  if (sizeof(*ack) + vlen + dlen > sizeof(g_ncd->out))
    {
      dlen = sizeof(g_ncd->out) - sizeof(*ack) - vlen;
      ack->reqack.result = dlen;
    }

  ack->valuelen          = vlen;
  ack->valuelen_nontrunc = vfull;
  if (vlen > 0)
    {
      memcpy(p, val, vlen);
    }

  if (dlen > 0 && data != p + vlen)
    {
      memmove(p + vlen, data, dlen);
    }

  us_write(ack, sizeof(*ack) + vlen + dlen);
}

void ncd_us_event(uint16_t id, uint16_t events)
{
  struct usrsock_message_socket_event_s ev;

  memset(&ev, 0, sizeof(ev));
  ev.head.msgid  = USRSOCK_MESSAGE_SOCKET_EVENT;
  ev.head.flags  = USRSOCK_MESSAGE_FLAG_EVENT;
  ev.head.events = events;
  ev.usockid     = id;
  us_write(&ev, sizeof(ev));
}

/* Hold a request in progress; complete it */

static void pend(FAR struct ncd_sock *s, uint32_t xid, uint8_t req,
                 uint16_t max)
{
  s->pend_xid = xid;
  s->pend_req = req;
  s->pend_max = max;
  us_ack(xid, 0, 0, true);
}

static void pend_done(FAR struct ncd_sock *s, int32_t result,
                      uint16_t events)
{
  if (s->pend_xid != 0)
    {
      uint32_t xid = s->pend_xid;

      s->pend_xid = 0;
      s->pend_req = NCD_NOREQ;
      us_ack(xid, result, events, false);
    }
}

/* Addresses: struct sockaddr <-> the protocol's (port as a number) */

static int sa_to_nc(FAR const void *p, size_t len, FAR struct nc_addr *a)
{
  FAR const struct sockaddr *sa = p;

  memset(a, 0, sizeof(*a));
  if (len >= sizeof(struct sockaddr_in) && sa->sa_family == AF_INET)
    {
      FAR const struct sockaddr_in *in = p;

      a->family = NC_ADDR_IP4;
      a->port   = ntohs(in->sin_port);
      memcpy(a->ip, &in->sin_addr, 4);
      return 0;
    }

#ifdef CONFIG_NET_IPv6
  if (len >= sizeof(struct sockaddr_in6) && sa->sa_family == AF_INET6)
    {
      FAR const struct sockaddr_in6 *in6 = p;

      a->family = NC_ADDR_IP6;
      a->port   = ntohs(in6->sin6_port);
      memcpy(a->ip, &in6->sin6_addr, 16);
      return 0;
    }
#endif

  return -EAFNOSUPPORT;
}

static uint16_t nc_to_sa(FAR const struct nc_addr *a,
                         FAR struct sockaddr_storage *ss)
{
  memset(ss, 0, sizeof(*ss));
  if (a->family == NC_ADDR_IP4)
    {
      FAR struct sockaddr_in *in = (FAR struct sockaddr_in *)ss;

      in->sin_family = AF_INET;
      in->sin_port   = htons(a->port);
      memcpy(&in->sin_addr, a->ip, 4);
      return sizeof(*in);
    }

#ifdef CONFIG_NET_IPv6
  if (a->family == NC_ADDR_IP6)
    {
      FAR struct sockaddr_in6 *in6 = (FAR struct sockaddr_in6 *)ss;

      in6->sin6_family = AF_INET6;
      in6->sin6_port   = htons(a->port);
      memcpy(&in6->sin6_addr, a->ip, 16);
      return sizeof(*in6);
    }
#endif

  return 0;
}

static size_t icmp_raw_in(FAR struct ncd_sock *s,
                          FAR const struct nc_addr *from,
                          FAR uint8_t *buf, size_t n);
static void ssh_connect(FAR struct ncd_sock *s, uint32_t xid);

/* Sockets */

static int chan_alloc(void)
{
  int max = g_ncd->ready ? g_ncd->h.max_chan : 32;
  int i;

  for (i = 1; i <= max && i < NCD_NCHAN; i++)
    {
      if (g_ncd->sock[i] == NULL && !g_ncd->h.chan[i].open)
        {
          return i;
        }
    }

  return -ENFILE;
}

static FAR struct ncd_sock *sock_new(int id, uint8_t kind)
{
  FAR struct ncd_sock *s = calloc(1, sizeof(*s));

  if (s == NULL)
    {
      return NULL;
    }

  s->id       = id;
  s->gen      = ++g_ncd->gen;
  s->kind     = kind;
  s->pend_req = NCD_NOREQ;
  if (kind != NCD_CTRL)
    {
      s->cap = g_ncd->window + (kind == NCD_DGRAM ? 2048 : 0);
      s->rx  = malloc(s->cap);
      if (s->rx == NULL)
        {
          free(s);
          return NULL;
        }
    }

  g_ncd->sock[id] = s;
  return s;
}

static void sock_free(FAR struct ncd_sock *s)
{
  g_ncd->sock[s->id] = NULL;
  if (s->kind == NCD_CTRL)
    {
      ncd_ctrl_free(s);
    }

  free(s->rx);
  free(s->ssh_req);
  free(s->hostkey);
  free(s->info);
  free(s->exitb);
  free(s);
}

/* The receive ring */

static void rx_put(FAR struct ncd_sock *s, FAR const void *p, size_t n)
{
  FAR const uint8_t *b = p;
  uint32_t tail = (s->head + s->len) % s->cap;
  size_t k;

  while (n > 0)
    {
      k = s->cap - tail < n ? s->cap - tail : n;
      memcpy(s->rx + tail, b, k);
      tail = (tail + k) % s->cap;
      s->len += k;
      b += k;
      n -= k;
    }
}

static void rx_get(FAR struct ncd_sock *s, FAR void *p, size_t n)
{
  FAR uint8_t *b = p;
  size_t k;

  while (n > 0)
    {
      k = s->cap - s->head < n ? s->cap - s->head : n;
      if (b != NULL)
        {
          memcpy(b, s->rx + s->head, k);
          b += k;
        }

      s->head = (s->head + k) % s->cap;
      s->len -= k;
      n -= k;
    }
}

static uint16_t rx_peek16(FAR struct ncd_sock *s)
{
  uint8_t b[2];

  b[0] = s->rx[s->head];
  b[1] = s->rx[(s->head + 1) % s->cap];
  return b[0] | (b[1] << 8);
}

/* What the kernel should believe now */

static uint16_t recv_events(FAR struct ncd_sock *s)
{
  s->sig_recv = s->len > 0 || s->eof || s->err != 0 ||
                s->accept_pending > 0;
  return s->sig_recv ? USRSOCK_EVENT_RECVFROM_AVAIL : 0;
}

static bool can_send(FAR struct ncd_sock *s)
{
  return s->open && nc_host_tx_credit(&g_ncd->h, s->id) > 0 &&
         nc_link_room(&g_ncd->h.link) > 0;
}

static uint16_t send_events(FAR struct ncd_sock *s)
{
  s->sig_send = s->kind == NCD_CTRL || can_send(s);
  return s->sig_send ? USRSOCK_EVENT_SENDTO_READY : 0;
}

/* Callbacks of the requests a socket's usrsock request became */

static void cb_status(FAR struct nc_host *h, FAR void *arg,
                      FAR const struct nc_rsp *r)
{
  FAR struct ncd_sock *s = sock_of(arg);

  if (r->final && s != NULL)
    {
      pend_done(s, r->status, 0);
    }
}

static void cb_open(FAR struct nc_host *h, FAR void *arg,
                    FAR const struct nc_rsp *r)
{
  FAR struct ncd_sock *s = sock_of(arg);

  if (!r->final || s == NULL)
    {
      return;
    }

  if (r->status < 0)
    {
      pend_done(s, r->status, 0);
      sock_free(s);
      return;
    }

  s->open = true;
  pend_done(s, s->id, send_events(s));
}

static void cb_connect(FAR struct nc_host *h, FAR void *arg,
                       FAR const struct nc_rsp *r)
{
  FAR struct ncd_sock *s = sock_of(arg);

  if (!r->final || s == NULL)
    {
      return;
    }

  if (r->status >= 0)
    {
      s->have_peer = true;
    }

  pend_done(s, r->status, 0);
  if (r->status >= 0 && can_send(s))
    {
      s->sig_send = true;
      ncd_us_event(s->id, USRSOCK_EVENT_SENDTO_READY);
    }
}

static void cb_getopt(FAR struct nc_host *h, FAR void *arg,
                      FAR const struct nc_rsp *r)
{
  FAR struct ncd_sock *s = sock_of(arg);
  uint32_t xid;

  if (!r->final || s == NULL || s->pend_xid == 0)
    {
      return;
    }

  xid = s->pend_xid;
  s->pend_xid = 0;
  s->pend_req = NCD_NOREQ;
  us_dack(xid, r->status < 0 ? r->status : 0, 0, r->body,
          r->n < s->pend_max ? r->n : s->pend_max, r->n, NULL, 0);
}

static void cb_getname(FAR struct nc_host *h, FAR void *arg,
                       FAR const struct nc_rsp *r)
{
  FAR struct ncd_sock *s = sock_of(arg);
  struct sockaddr_storage ss;
  struct nc_addr a;
  struct nc_rd rd;
  uint16_t alen = 0;
  uint32_t xid;

  if (!r->final || s == NULL || s->pend_xid == 0)
    {
      return;
    }

  if (r->status >= 0)
    {
      nc_rd_init(&rd, r->body, r->n);
      nc_rd_addr(&rd, &a);
      alen = rd.err ? 0 : nc_to_sa(&a, &ss);
    }

  xid = s->pend_xid;
  s->pend_xid = 0;
  s->pend_req = NCD_NOREQ;
  us_dack(xid, r->status < 0 ? r->status : 0, 0, &ss,
          alen < s->pend_max ? alen : s->pend_max, alen, NULL, 0);
}

static void cb_accept(FAR struct nc_host *h, FAR void *arg,
                      FAR const struct nc_rsp *r)
{
  FAR struct ncd_sock *s = sock_of(arg);
  FAR struct ncd_sock *n;
  struct sockaddr_storage ss;
  struct nc_rd rd;
  uint16_t alen;
  int16_t newid;
  uint32_t xid;

  if (!r->final || s == NULL || s->pend_xid == 0)
    {
      return;
    }

  n = sock_get(s->pend_new);
  xid = s->pend_xid;
  s->pend_xid = 0;
  s->pend_req = NCD_NOREQ;
  if (r->status < 0 || n == NULL)
    {
      if (n != NULL)
        {
          sock_free(n);
        }

      if (r->status == -EAGAIN)
        {
          s->accept_pending = 0;
        }

      us_dack(xid, r->status < 0 ? r->status : -ENOMEM, recv_events(s),
              NULL, 0, 0, NULL, 0);
      return;
    }

  nc_rd_init(&rd, r->body, r->n);
  nc_rd_u32(&rd);
  nc_rd_addr(&rd, &n->peer);
  n->have_peer = !rd.err;
  n->open      = true;
  alen = n->have_peer ? nc_to_sa(&n->peer, &ss) : 0;
  if (s->accept_pending > 0)
    {
      s->accept_pending--;
    }

  newid = n->id;
  us_dack(xid, sizeof(newid), recv_events(s), &ss,
          alen < s->pend_max ? alen : s->pend_max, alen, &newid,
          sizeof(newid));
  n->sig_send = true;
  ncd_us_event(n->id, USRSOCK_EVENT_SENDTO_READY);
}

static void cb_ssh_open(FAR struct nc_host *h, FAR void *arg,
                        FAR const struct nc_rsp *r)
{
  FAR struct ncd_sock *s = sock_of(arg);

  if (!r->final || s == NULL)
    {
      return;
    }

  s->ssh_opening = false;
  if (r->status >= 0)
    {
      s->open = true;
      free(s->info);
      s->info = malloc(r->n);
      if (s->info != NULL)
        {
          memcpy(s->info, r->body, r->n);
          s->info_n = r->n;
        }
    }
  else
    {
      s->err = -r->status;
    }

  pend_done(s, r->status, 0);
  if (r->status >= 0 && can_send(s))
    {
      s->sig_send = true;
      ncd_us_event(s->id, USRSOCK_EVENT_SENDTO_READY);
    }
}

/* Send a request for a socket; on failure answer the usrsock request */

static int sock_request(FAR struct ncd_sock *s, uint32_t xid, uint8_t req,
                        uint16_t op, FAR const void *body, size_t n,
                        nc_rsp_cb cb, uint16_t max)
{
  int ret = nc_host_request(&g_ncd->h, s->id, op, body, n, cb,
                            SOCK_ARG(s));

  if (ret < 0)
    {
      return ret == -NC_ENOTCONN ? -ECONNRESET :
             ret == -NC_EAGAIN ? -EBUSY : ret;
    }

  pend(s, xid, req, max);
  return 0;
}

/****************************************************************************
 * usrsock requests
 ****************************************************************************/

static void req_socket(FAR const struct usrsock_request_socket_s *req)
{
  FAR struct ncd_sock *s;
  struct nc_sock_open so;
  uint8_t body[16];
  struct nc_wr w;
  int type = req->type & SOCK_TYPE_MASK;
  int proto = req->protocol;
  bool icmp_raw;
  int id;
  int i;
  int ret;

  /* netcopd's own control socket: there whether the cop is or not */

  if (proto == NCX_PROTO_CTRL)
    {
      if (type != SOCK_RAW || req->domain != AF_INET)
        {
          us_ack(req->head.xid, -EPROTONOSUPPORT, 0, false);
          return;
        }

      for (i = NCD_CTRL0; i < NCD_NSOCK && g_ncd->sock[i] != NULL; i++)
        {
        }

      s = i < NCD_NSOCK ? sock_new(i, NCD_CTRL) : NULL;
      if (s == NULL)
        {
          us_ack(req->head.xid, -ENFILE, 0, false);
          return;
        }

      s->sig_send = true;
      us_ack(req->head.xid, s->id, USRSOCK_EVENT_SENDTO_READY, false);
      ncd_us_event(s->id, USRSOCK_EVENT_SENDTO_READY);
      return;
    }

  if (req->domain != AF_INET &&
      (req->domain != AF_INET6 ||
       (g_ncd->hello.caps & NC_CAP_IPV6) == 0))
    {
      us_ack(req->head.xid, -EAFNOSUPPORT, 0, false);
      return;
    }

  if (!g_ncd->ready)
    {
      /* Not -ENETDOWN: that would send the kernel to its own stack */

      us_ack(req->head.xid, -ENETUNREACH, 0, false);
      return;
    }

  id = chan_alloc();
  if (id < 0)
    {
      us_ack(req->head.xid, -ENFILE, 0, false);
      return;
    }

  if (proto == NCX_PROTO_SSH)
    {
      if (type != SOCK_STREAM || (g_ncd->hello.caps & NC_CAP_SSH) == 0)
        {
          us_ack(req->head.xid, -EPROTONOSUPPORT, 0, false);
          return;
        }

      s = sock_new(id, NCD_SSH);
      us_ack(req->head.xid, s != NULL ? s->id : -ENOMEM, 0, false);
      return;
    }

  /* Stock ping's raw ICMP socket: the cop's ICMP datagram socket */

  icmp_raw = type == SOCK_RAW && proto == IPPROTO_ICMP;
  if (type != SOCK_STREAM && type != SOCK_DGRAM && !icmp_raw)
    {
      us_ack(req->head.xid, -EPROTONOSUPPORT, 0, false);
      return;
    }

  s = sock_new(id, type == SOCK_STREAM ? NCD_TCP : NCD_DGRAM);
  if (s == NULL)
    {
      us_ack(req->head.xid, -ENOMEM, 0, false);
      return;
    }

  s->icmp_raw = icmp_raw;
  s->family = req->domain;
  memset(&so, 0, sizeof(so));
  so.domain = req->domain == AF_INET6 ? NC_AF_INET6 : NC_AF_INET;
  so.type   = type == SOCK_STREAM ? NC_SOCK_STREAM : NC_SOCK_DGRAM;
  so.proto  = proto != 0 ? proto :
              type == SOCK_STREAM ? NC_IPPROTO_TCP : NC_IPPROTO_UDP;
  so.window = g_ncd->window;
  nc_wr_init(&w, body, sizeof(body));
  nc_enc_sock_open(&w, &so);

  nc_host_chan_open(&g_ncd->h, id, g_ncd->window);
  ret = sock_request(s, req->head.xid, USRSOCK_REQUEST_SOCKET,
                     NC_OP_SOCK_OPEN, body, w.n, cb_open, 0);
  if (ret < 0)
    {
      nc_host_chan_free(&g_ncd->h, id);
      sock_free(s);
      us_ack(req->head.xid, ret, 0, false);
    }
}

static void cb_closed(FAR struct nc_host *h, FAR void *arg,
                      FAR const struct nc_rsp *r)
{
  FAR struct ncd_sock *s = sock_of(arg);

  if (r->final && s != NULL && s->closing)
    {
      sock_free(s);
    }
}

static void req_close(FAR const struct usrsock_request_close_s *req)
{
  FAR struct ncd_sock *s = sock_get(req->usockid);

  us_ack(req->head.xid, 0, 0, false);
  if (s == NULL)
    {
      return;
    }

  /* The channel number stays taken until the cop has closed it */

  if (s->kind != NCD_CTRL && g_ncd->h.chan[s->id].open && !s->aborted &&
      nc_host_request(&g_ncd->h, s->id, NC_OP_CHAN_CLOSE, NULL, 0,
                      cb_closed, SOCK_ARG(s)) >= 0)
    {
      s->closing  = true;
      s->pend_xid = 0;
      s->pend_req = NCD_NOREQ;
      free(s->rx);
      s->rx = NULL;
      s->len = 0;
      return;
    }

  if (s->kind != NCD_CTRL && g_ncd->h.chan[s->id].open)
    {
      nc_host_chan_free(&g_ncd->h, s->id);
    }

  sock_free(s);
}

static FAR struct ncd_sock *sock_usable(int id, uint32_t xid)
{
  FAR struct ncd_sock *s = sock_get(id);

  if (s == NULL || s->closing)
    {
      us_ack(xid, -EBADF, 0, false);
      return NULL;
    }

  if (s->aborted)
    {
      us_ack(xid, -ECONNRESET, 0, false);
      return NULL;
    }

  return s;
}

static void req_connect(FAR const struct usrsock_request_connect_s *req,
                        size_t len)
{
  FAR struct ncd_sock *s = sock_usable(req->usockid, req->head.xid);
  struct nc_sock_connect sc;
  uint8_t body[32];
  struct nc_wr w;
  int ret;

  if (s == NULL)
    {
      return;
    }

  if (s->kind == NCD_SSH)
    {
      ssh_connect(s, req->head.xid);
      return;
    }

  if (s->kind == NCD_CTRL)
    {
      us_ack(req->head.xid, -EOPNOTSUPP, 0, false);
      return;
    }

  memset(&sc, 0, sizeof(sc));
  ret = sa_to_nc(req + 1, len - sizeof(*req) < req->addrlen ?
                 len - sizeof(*req) : req->addrlen, &sc.addr);
  if (ret < 0)
    {
      us_ack(req->head.xid, ret, 0, false);
      return;
    }

  s->peer = sc.addr;
  nc_wr_init(&w, body, sizeof(body));
  nc_enc_sock_connect(&w, &sc);
  ret = sock_request(s, req->head.xid, USRSOCK_REQUEST_CONNECT,
                     NC_OP_SOCK_CONNECT, body, w.n, cb_connect, 0);
  if (ret < 0)
    {
      us_ack(req->head.xid, ret, 0, false);
    }
}

static void req_sendto(FAR const struct usrsock_request_sendto_s *rq,
                       size_t len)
{
  struct usrsock_request_sendto_s rqh = *rq;     /* g_ncd->req is reused */
  FAR const struct usrsock_request_sendto_s *req = &rqh;
  FAR struct ncd_sock *s = sock_usable(req->usockid, req->head.xid);
  FAR const uint8_t *data;
  struct nc_addr to;
  size_t have;
  size_t sent;
  size_t left;
  int ret;

  if (s == NULL)
    {
      return;
    }

  if (len < sizeof(*req) + req->addrlen)
    {
      us_ack(req->head.xid, -EINVAL, 0, false);
      return;
    }

  data = (FAR const uint8_t *)(rq + 1) + req->addrlen;
  have = len - sizeof(*req) - req->addrlen;     /* Of buflen, read so far */

  if (s->kind == NCD_CTRL)
    {
      if (have < req->buflen)
        {
          us_ack(req->head.xid, -EMSGSIZE, send_events(s), false);
          return;
        }

      ret = ncd_ctrl_request(s, data, req->buflen);
      us_ack(req->head.xid, ret < 0 ? ret : (int)req->buflen,
             send_events(s), false);
      return;
    }

  if (!s->open)
    {
      us_ack(req->head.xid, s->err ? -s->err : -ENOTCONN, 0, false);
      return;
    }

  if (s->err != 0)
    {
      us_ack(req->head.xid, -s->err, 0, false);
      return;
    }

  if (s->kind == NCD_DGRAM)
    {
      memset(&to, 0, sizeof(to));
      if (req->addrlen > 0)
        {
          ret = sa_to_nc(rq + 1, req->addrlen, &to);
          if (ret < 0)
            {
              us_ack(req->head.xid, ret, send_events(s), false);
              return;
            }
        }

      if (have < req->buflen)
        {
          us_ack(req->head.xid, -EMSGSIZE, send_events(s), false);
          return;
        }

      if (s->icmp_raw && req->buflen >= 8)
        {
          s->icmp_id = data[4] | (data[5] << 8);       /* As sent */
        }

      ret = nc_host_send_dgram(&g_ncd->h, s->id, &to, data, req->buflen);
      if (ret == 0)
        {
          us_ack(req->head.xid, -EAGAIN, 0, false);
          s->sig_send = false;
          return;
        }

      us_ack(req->head.xid, ret < 0 ? -EMSGSIZE : (int)req->buflen,
             send_events(s), false);
      return;
    }

  /* A stream: as much as the credit and the link take now, the rest of
   * the request read from the device as it goes
   */

  sent = 0;
  left = req->buflen;
  while (left > 0)
    {
      size_t k;
      size_t took;

      if (have == 0)
        {
          ssize_t r;

          k = nc_host_tx_credit(&g_ncd->h, s->id);
          k = k < left ? k : left;
          k = k < NC_MTU_MAX ? k : NC_MTU_MAX;
          if (k == 0 || nc_link_room(&g_ncd->h.link) == 0)
            {
              break;
            }

          r = read(g_ncd->usock, g_ncd->req, k);
          if (r <= 0)
            {
              break;
            }

          data = g_ncd->req;
          have = r;
        }

      k = have;
      took = nc_host_send(&g_ncd->h, s->id, data, k);
      if (took == 0)
        {
          break;
        }

      sent += took;
      left -= took;
      data += took;
      have -= took;
      if (took < k)
        {
          break;
        }
    }

  if (sent == 0)
    {
      us_ack(req->head.xid, -EAGAIN, 0, false);
      s->sig_send = false;
      return;
    }

  us_ack(req->head.xid, sent, send_events(s), false);
}

static void req_recvfrom(FAR const struct usrsock_request_recvfrom_s *rq)
{
  struct usrsock_request_recvfrom_s rqh = *rq;   /* g_ncd->req is reused */
  FAR const struct usrsock_request_recvfrom_s *req = &rqh;
  FAR struct ncd_sock *s = sock_get(req->usockid);
  FAR uint8_t *buf = g_ncd->req;
  struct sockaddr_storage ss;
  uint16_t alen = 0;
  size_t n;

  if (s == NULL || s->closing)
    {
      us_dack(req->head.xid, -EBADF, 0, NULL, 0, 0, NULL, 0);
      return;
    }

  if (s->kind == NCD_CTRL)
    {
      FAR struct ncd_msg *m;

      if (!ncd_ctrl_take(s, &m))
        {
          s->sig_recv = false;
          us_dack(req->head.xid, -EAGAIN, 0, NULL, 0, 0, NULL, 0);
          return;
        }

      n = m->len < req->max_buflen ? m->len : req->max_buflen;
      memcpy(buf, m->data, n);
      free(m);
      s->sig_recv = s->qhead != NULL;
      us_dack(req->head.xid, n,
              s->sig_recv ? USRSOCK_EVENT_RECVFROM_AVAIL : 0,
              NULL, 0, 0, buf, n);
      return;
    }

  if (s->len == 0)
    {
      int32_t ret = s->aborted ? -ECONNRESET :
                    s->err ? -s->err :
                    s->eof ? 0 : -EAGAIN;

      if (s->err != 0)
        {
          s->err = 0;
          s->eof = true;
        }

      if (ret == -EAGAIN)
        {
          s->sig_recv = false;
          us_dack(req->head.xid, ret, 0, NULL, 0, 0, NULL, 0);
        }
      else
        {
          us_dack(req->head.xid, ret, recv_events(s), NULL, 0, 0, NULL, 0);
        }

      return;
    }

  if (s->kind == NCD_DGRAM)
    {
      struct nc_addr from;
      struct nc_rd rd;
      uint8_t hdr[NC_ADDR_MAXLEN];
      uint16_t rec = rx_peek16(s);
      size_t hn;

      rx_get(s, NULL, 2);
      hn = rec < sizeof(hdr) ? rec : sizeof(hdr);
      rx_get(s, hdr, hn);
      nc_rd_init(&rd, hdr, hn);
      nc_rd_addr(&rd, &from);
      if (rd.err)
        {
          rx_get(s, NULL, rec - hn);
          nc_host_consumed(&g_ncd->h, s->id, rec);
          us_dack(req->head.xid, -EAGAIN, recv_events(s), NULL, 0, 0,
                  NULL, 0);
          return;
        }

      /* The datagram: what fits, the rest dropped */

      n = rec - rd.off;
      memcpy(buf, hdr + rd.off, hn - rd.off);
      rx_get(s, buf + (hn - rd.off), n - (hn - rd.off));
      nc_host_consumed(&g_ncd->h, s->id, rec);
      alen = nc_to_sa(&from, &ss);
      if (s->icmp_raw)
        {
          n = icmp_raw_in(s, &from, buf, n);
        }
      if (n > req->max_buflen)
        {
          n = req->max_buflen;
        }
    }
  else
    {
      /* No more than the answer carries: us_dack() would cut what does
       * not fit, and it is taken from the ring here (a program asking for
       * more than about 2 KB at once lost bytes)
       */

      n = s->len < req->max_buflen ? s->len : req->max_buflen;
      if (n > sizeof(g_ncd->out) -
              sizeof(struct usrsock_message_datareq_ack_s) - sizeof(ss))
        {
          n = sizeof(g_ncd->out) -
              sizeof(struct usrsock_message_datareq_ack_s) - sizeof(ss);
        }

      rx_get(s, buf, n);
      nc_host_consumed(&g_ncd->h, s->id, n);
      if (s->have_peer)
        {
          alen = nc_to_sa(&s->peer, &ss);
        }
    }

  us_dack(req->head.xid, n, recv_events(s), &ss,
          alen < req->max_addrlen ? alen : req->max_addrlen, alen, buf, n);
}

/* A raw ICMP socket's datagram: an IPv4 header in front (raw sockets have
 * one), and the identifier the program used (a Linux ping socket puts its
 * own)
 */

static uint16_t csum(FAR const uint8_t *p, size_t n)
{
  uint32_t sum = 0;
  size_t i;

  for (i = 0; i + 1 < n; i += 2)
    {
      sum += (p[i] << 8) | p[i + 1];
    }

  if (n & 1)
    {
      sum += p[n - 1] << 8;
    }

  while (sum >> 16)
    {
      sum = (sum & 0xffff) + (sum >> 16);
    }

  return ~sum & 0xffff;
}

static size_t icmp_raw_in(FAR struct ncd_sock *s,
                          FAR const struct nc_addr *from,
                          FAR uint8_t *buf, size_t n)
{
  uint16_t c;

  if (n + 20 > sizeof(g_ncd->req))
    {
      n = sizeof(g_ncd->req) - 20;
    }

  memmove(buf + 20, buf, n);
  if (n >= 8)
    {
      buf[24] = (uint8_t)s->icmp_id;
      buf[25] = (uint8_t)(s->icmp_id >> 8);
      buf[22] = buf[23] = 0;
      c = csum(buf + 20, n);
      buf[22] = c >> 8;
      buf[23] = c & 0xff;
    }

  memset(buf, 0, 20);
  buf[0] = 0x45;
  buf[2] = (n + 20) >> 8;
  buf[3] = (n + 20) & 0xff;
  buf[8] = 64;                           /* TTL */
  buf[9] = IPPROTO_ICMP;
  memcpy(buf + 12, from->ip, 4);
  if (g_ncd->have_wifi)
    {
      memcpy(buf + 16, g_ncd->wifi.ip, 4);
    }

  c = csum(buf, 20);
  buf[10] = c >> 8;
  buf[11] = c & 0xff;
  return n + 20;
}

/* SSH sockets: connect() opens the session (SSH_OPEN, with what
 * NCX_SSH_OPEN gave), and once it runs marks the socket connected for the
 * kernel (a connect() after an ENOKEY and the host key's decision)
 */

static void ssh_connect(FAR struct ncd_sock *s, uint32_t xid)
{
  FAR uint8_t *body = g_ncd->tmp;
  size_t n = s->ssh_req_n;
  int ret;

  if (s->open)
    {
      us_ack(xid, 0, 0, false);
      return;
    }

  if (s->ssh_opening)
    {
      us_ack(xid, -EALREADY, 0, false);
      return;
    }

  if (s->ssh_req == NULL || n > sizeof(g_ncd->tmp))
    {
      us_ack(xid, s->err ? -s->err : -EDESTADDRREQ, 0, false);
      return;
    }

  /* The window is the daemon's (u32 at 8) */

  memcpy(body, s->ssh_req, n);
  body[8]  = (uint8_t)g_ncd->window;
  body[9]  = (uint8_t)(g_ncd->window >> 8);
  body[10] = (uint8_t)(g_ncd->window >> 16);
  body[11] = (uint8_t)(g_ncd->window >> 24);
  free(s->hostkey);
  s->hostkey   = NULL;
  s->hostkey_n = 0;
  s->err       = 0;
  nc_host_chan_open(&g_ncd->h, s->id, g_ncd->window);
  ret = sock_request(s, xid, USRSOCK_REQUEST_CONNECT, NC_OP_SSH_OPEN, body,
                     n, cb_ssh_open, 0);
  if (ret < 0)
    {
      nc_host_chan_free(&g_ncd->h, s->id);
      us_ack(xid, ret, 0, false);
      return;
    }

  s->ssh_opening = true;
}

/* SSH sockets: netcopd's options */

static void ssh_setopt(FAR struct ncd_sock *s, uint32_t xid, int option,
                       FAR const uint8_t *val, size_t n)
{
  FAR uint8_t *body = g_ncd->tmp;
  int ret;

  switch (option)
    {
      case NCX_SSH_OPEN:
        if (s->open || s->ssh_opening || n < 12)
          {
            us_ack(xid, -EALREADY, 0, false);
            return;
          }

        /* Kept for connect(), which opens the session */

        free(s->ssh_req);
        s->ssh_req = malloc(n);
        if (s->ssh_req == NULL)
          {
            us_ack(xid, -ENOMEM, 0, false);
            return;
          }

        memcpy(s->ssh_req, val, n);
        s->ssh_req_n = n;
        us_ack(xid, 0, 0, false);
        return;

      case NCX_SSH_HOSTKEY:
        if (!s->ssh_opening)
          {
            us_ack(xid, s->err ? -s->err : -EINVAL, 0, false);
            return;
          }

        if (n < 1 ||
            nc_host_request(&g_ncd->h, s->id, NC_OP_SSH_HOSTKEY_REPLY,
                            val, 1, NULL, NULL) < 0)
          {
            us_ack(xid, -EINVAL, 0, false);
            return;
          }

        pend(s, xid, USRSOCK_REQUEST_SETSOCKOPT, 0);
        return;

      case NCX_SSH_RESIZE:
        if (!s->open || n < 4)
          {
            us_ack(xid, -EINVAL, 0, false);
            return;
          }

        nc_host_request(&g_ncd->h, s->id, NC_OP_SSH_RESIZE, val, 4, NULL,
                        NULL);
        us_ack(xid, 0, 0, false);
        return;

      default:
        us_ack(xid, -ENOPROTOOPT, 0, false);
        return;
    }
}

static void req_setsockopt(FAR const struct usrsock_request_setsockopt_s *req,
                           size_t len)
{
  FAR struct ncd_sock *s = sock_usable(req->usockid, req->head.xid);
  FAR const uint8_t *val = (FAR const uint8_t *)(req + 1);
  FAR uint8_t *body = g_ncd->tmp;
  size_t n = req->valuelen;
  struct nc_wr w;
  int ret;

  if (s == NULL)
    {
      return;
    }

  if (len < sizeof(*req) + n)
    {
      n = len - sizeof(*req);
    }

  if (s->kind == NCD_CTRL)
    {
      us_ack(req->head.xid, -ENOPROTOOPT, 0, false);
      return;
    }

  if (req->level == SOL_NCX)
    {
      if (s->kind != NCD_SSH)
        {
          us_ack(req->head.xid, -ENOPROTOOPT, 0, false);
          return;
        }

      ssh_setopt(s, req->head.xid, req->option, val, n);
      return;
    }

  if (!s->open)
    {
      us_ack(req->head.xid, -ENOTCONN, 0, false);
      return;
    }

  if (s->icmp_raw && req->level == IPPROTO_ICMP)
    {
      us_ack(req->head.xid, 0, 0, false);   /* ICMP_FILTER: replies only */
      return;
    }

  nc_wr_init(&w, body, sizeof(g_ncd->tmp));
  nc_wr_u16(&w, (uint16_t)req->level);
  nc_wr_u16(&w, (uint16_t)req->option);
  nc_wr_bytes(&w, val, n);
  if (w.err)
    {
      us_ack(req->head.xid, -EINVAL, 0, false);
      return;
    }

  ret = sock_request(s, req->head.xid, USRSOCK_REQUEST_SETSOCKOPT,
                     NC_OP_SOCK_SETOPT, body, w.n, cb_status, 0);
  if (ret < 0)
    {
      us_ack(req->head.xid, ret, 0, false);
    }
}

static void req_getsockopt(FAR const struct usrsock_request_getsockopt_s *req)
{
  FAR struct ncd_sock *s = sock_get(req->usockid);
  FAR const void *val = NULL;
  uint8_t body[8];
  struct nc_wr w;
  size_t n = 0;
  int32_t e;
  int ret;

  if (s == NULL || s->closing || s->kind == NCD_CTRL)
    {
      us_dack(req->head.xid, s == NULL ? -EBADF : -ENOPROTOOPT, 0,
              NULL, 0, 0, NULL, 0);
      return;
    }

  if (req->level == SOL_NCX)
    {
      switch (req->option)
        {
          case NCX_SSH_HOSTKEY:
            val = s->hostkey;
            n   = s->hostkey_n;
            break;

          case NCX_SSH_INFO:
            val = s->info;
            n   = s->info_n;
            break;

          case NCX_SSH_EXIT:
            val = s->exitb;
            n   = s->exit_n;
            break;

          default:
            us_dack(req->head.xid, -ENOPROTOOPT, 0, NULL, 0, 0, NULL, 0);
            return;
        }

      if (val == NULL)
        {
          us_dack(req->head.xid, -ENODATA, 0, NULL, 0, 0, NULL, 0);
          return;
        }

      us_dack(req->head.xid, 0, 0, val,
              n < req->max_valuelen ? n : req->max_valuelen, n, NULL, 0);
      return;
    }

  if (req->level == SOL_SOCKET && req->option == SO_ERROR &&
      (s->err != 0 || s->aborted || !s->open))
    {
      e = s->aborted ? ECONNRESET : s->err;
      s->err = 0;
      us_dack(req->head.xid, 0, 0, &e,
              req->max_valuelen < sizeof(e) ? req->max_valuelen :
              sizeof(e), sizeof(e), NULL, 0);
      return;
    }

  if (s->aborted || !s->open)
    {
      us_dack(req->head.xid, s->aborted ? -ECONNRESET : -ENOTCONN, 0,
              NULL, 0, 0, NULL, 0);
      return;
    }

  nc_wr_init(&w, body, sizeof(body));
  nc_wr_u16(&w, (uint16_t)req->level);
  nc_wr_u16(&w, (uint16_t)req->option);
  nc_wr_u16(&w, req->max_valuelen);
  ret = sock_request(s, req->head.xid, USRSOCK_REQUEST_GETSOCKOPT,
                     NC_OP_SOCK_GETOPT, body, w.n, cb_getopt,
                     req->max_valuelen);
  if (ret < 0)
    {
      us_dack(req->head.xid, ret, 0, NULL, 0, 0, NULL, 0);
    }
}

static void req_getname(FAR const struct usrsock_request_getsockname_s *req,
                        uint8_t which)
{
  FAR struct ncd_sock *s = sock_get(req->usockid);
  int ret;

  if (s == NULL || s->closing || s->aborted || !s->open ||
      s->kind == NCD_CTRL || s->kind == NCD_SSH)
    {
      us_dack(req->head.xid, s == NULL ? -EBADF :
              s->aborted ? -ECONNRESET : -EOPNOTSUPP, 0,
              NULL, 0, 0, NULL, 0);
      return;
    }

  ret = sock_request(s, req->head.xid, USRSOCK_REQUEST_GETSOCKNAME,
                     NC_OP_SOCK_GETNAME, &which, 1, cb_getname,
                     req->max_addrlen);
  if (ret < 0)
    {
      us_dack(req->head.xid, ret, 0, NULL, 0, 0, NULL, 0);
    }
}

static void req_bind(FAR const struct usrsock_request_bind_s *req,
                     size_t len)
{
  FAR struct ncd_sock *s = sock_usable(req->usockid, req->head.xid);
  struct nc_addr a;
  uint8_t body[NC_ADDR_MAXLEN];
  struct nc_wr w;
  int ret;

  if (s == NULL)
    {
      return;
    }

  ret = s->kind == NCD_TCP || s->kind == NCD_DGRAM ?
        sa_to_nc(req + 1, len - sizeof(*req), &a) : -EOPNOTSUPP;
  if (ret == 0)
    {
      nc_wr_init(&w, body, sizeof(body));
      nc_wr_addr(&w, &a);
      ret = sock_request(s, req->head.xid, USRSOCK_REQUEST_BIND,
                         NC_OP_SOCK_BIND, body, w.n, cb_status, 0);
    }

  if (ret < 0)
    {
      us_ack(req->head.xid, ret, 0, false);
    }
}

static void req_listen(FAR const struct usrsock_request_listen_s *req)
{
  FAR struct ncd_sock *s = sock_usable(req->usockid, req->head.xid);
  uint8_t body[2];
  int ret;

  if (s == NULL)
    {
      return;
    }

  if (s->kind != NCD_TCP)
    {
      us_ack(req->head.xid, -EOPNOTSUPP, 0, false);
      return;
    }

  body[0] = (uint8_t)req->backlog;
  body[1] = (uint8_t)(req->backlog >> 8);
  s->listening = true;
  ret = sock_request(s, req->head.xid, USRSOCK_REQUEST_LISTEN,
                     NC_OP_SOCK_LISTEN, body, 2, cb_status, 0);
  if (ret < 0)
    {
      us_ack(req->head.xid, ret, 0, false);
    }
}

static void req_accept(FAR const struct usrsock_request_accept_s *req)
{
  FAR struct ncd_sock *s = sock_get(req->usockid);
  FAR struct ncd_sock *n;
  struct nc_sock_accept sa;
  uint8_t body[16];
  struct nc_wr w;
  int id;
  int ret;

  if (s == NULL || s->closing || s->aborted || !s->listening)
    {
      us_dack(req->head.xid, s == NULL ? -EBADF :
              s->aborted ? -ECONNRESET : -EINVAL, 0, NULL, 0, 0, NULL, 0);
      return;
    }

  id = chan_alloc();
  n = id > 0 ? sock_new(id, NCD_TCP) : NULL;
  if (n == NULL)
    {
      us_dack(req->head.xid, -ENFILE, 0, NULL, 0, 0, NULL, 0);
      return;
    }

  n->family = s->family;
  memset(&sa, 0, sizeof(sa));
  sa.new_chan = id;
  sa.window   = g_ncd->window;
  nc_wr_init(&w, body, sizeof(body));
  nc_enc_sock_accept(&w, &sa);
  nc_host_chan_open(&g_ncd->h, id, g_ncd->window);
  s->pend_new = id;
  ret = sock_request(s, req->head.xid, USRSOCK_REQUEST_ACCEPT,
                     NC_OP_SOCK_ACCEPT, body, w.n, cb_accept,
                     req->max_addrlen);
  if (ret < 0)
    {
      nc_host_chan_free(&g_ncd->h, id);
      sock_free(n);
      us_dack(req->head.xid, ret, 0, NULL, 0, 0, NULL, 0);
    }
}

static void req_ioctl(FAR const struct usrsock_request_ioctl_s *req,
                      size_t len)
{
  FAR struct ncd_sock *s = sock_get(req->usockid);
  int val;

  if (s == NULL || req->cmd != FIONREAD)
    {
      us_dack(req->head.xid, s == NULL ? -EBADF : -ENOTTY, 0,
              NULL, 0, 0, NULL, 0);
      return;
    }

  if (s->kind == NCD_CTRL)
    {
      val = s->qhead != NULL ? s->qhead->len : 0;
    }
  else if (s->kind == NCD_DGRAM && s->len >= 2)
    {
      val = rx_peek16(s);     /* (with its address) */
    }
  else
    {
      val = s->len;
    }

  us_dack(req->head.xid, 0, 0, &val,
          req->arglen < sizeof(val) ? req->arglen : sizeof(val),
          sizeof(val), NULL, 0);
}

static void req_shutdown(FAR const struct usrsock_request_shutdown_s *req)
{
  FAR struct ncd_sock *s = sock_usable(req->usockid, req->head.xid);
  uint8_t how;
  int ret;

  if (s == NULL)
    {
      return;
    }

  if (s->kind == NCD_CTRL || !s->open)
    {
      us_ack(req->head.xid, -ENOTCONN, 0, false);
      return;
    }

  how = req->how == SHUT_RD ? NC_SHUT_RD :
        req->how == SHUT_WR ? NC_SHUT_WR : NC_SHUT_RDWR;
  ret = sock_request(s, req->head.xid, USRSOCK_REQUEST_SHUTDOWN,
                     NC_OP_CHAN_SHUTDOWN, &how, 1, cb_status, 0);
  if (ret < 0)
    {
      us_ack(req->head.xid, ret, 0, false);
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* A request waits on /dev/usrsock */

void ncd_usrsock_request(void)
{
  FAR struct usrsock_request_common_s *c =
    (FAR struct usrsock_request_common_s *)g_ncd->req;
  ssize_t n;

  n = read(g_ncd->usock, g_ncd->req, sizeof(g_ncd->req));
  if (n < (ssize_t)sizeof(*c))
    {
      return;
    }

#ifdef CONFIG_NETCOP_DEBUG
  syslog(LOG_DEBUG, "netcop: usrsock req %u xid %u id %d (%d bytes)\n",
         (unsigned)c->reqid, (unsigned)c->xid,
         *(FAR int16_t *)(g_ncd->req + sizeof(*c)), (int)n);
#endif

#define REQ(type) ((FAR const struct type *)g_ncd->req)
#define NEED(type) if ((size_t)n < sizeof(struct type)) break

  switch (c->reqid)
    {
      case USRSOCK_REQUEST_SOCKET:
        NEED(usrsock_request_socket_s);
        req_socket(REQ(usrsock_request_socket_s));
        return;

      case USRSOCK_REQUEST_CLOSE:
        NEED(usrsock_request_close_s);
        req_close(REQ(usrsock_request_close_s));
        return;

      case USRSOCK_REQUEST_CONNECT:
        NEED(usrsock_request_connect_s);
        req_connect(REQ(usrsock_request_connect_s), n);
        return;

      case USRSOCK_REQUEST_SENDTO:
        NEED(usrsock_request_sendto_s);
        req_sendto(REQ(usrsock_request_sendto_s), n);
        return;

      case USRSOCK_REQUEST_RECVFROM:
        NEED(usrsock_request_recvfrom_s);
        req_recvfrom(REQ(usrsock_request_recvfrom_s));
        return;

      case USRSOCK_REQUEST_SETSOCKOPT:
        NEED(usrsock_request_setsockopt_s);
        req_setsockopt(REQ(usrsock_request_setsockopt_s), n);
        return;

      case USRSOCK_REQUEST_GETSOCKOPT:
        NEED(usrsock_request_getsockopt_s);
        req_getsockopt(REQ(usrsock_request_getsockopt_s));
        return;

      case USRSOCK_REQUEST_GETSOCKNAME:
        NEED(usrsock_request_getsockname_s);
        req_getname(REQ(usrsock_request_getsockname_s), 0);
        return;

      case USRSOCK_REQUEST_GETPEERNAME:
        NEED(usrsock_request_getpeername_s);
        req_getname(REQ(usrsock_request_getsockname_s), 1);
        return;

      case USRSOCK_REQUEST_BIND:
        NEED(usrsock_request_bind_s);
        req_bind(REQ(usrsock_request_bind_s), n);
        return;

      case USRSOCK_REQUEST_LISTEN:
        NEED(usrsock_request_listen_s);
        req_listen(REQ(usrsock_request_listen_s));
        return;

      case USRSOCK_REQUEST_ACCEPT:
        NEED(usrsock_request_accept_s);
        req_accept(REQ(usrsock_request_accept_s));
        return;

      case USRSOCK_REQUEST_IOCTL:
        NEED(usrsock_request_ioctl_s);
        req_ioctl(REQ(usrsock_request_ioctl_s), n);
        return;

      case USRSOCK_REQUEST_SHUTDOWN:
        NEED(usrsock_request_shutdown_s);
        req_shutdown(REQ(usrsock_request_shutdown_s));
        return;

      default:
        break;
    }

  /* Unknown or short: answer, or the kernel waits for ever */

  us_ack(c->xid, -EINVAL, 0, false);
}

/* DATA / DATA_EXT / DGRAM from the cop */

void ncd_sock_data(uint8_t chan, uint8_t type, FAR const uint8_t *p,
                   size_t n)
{
  FAR struct ncd_sock *s = g_ncd->sock[chan];
  size_t need = n + (type == NC_T_DGRAM ? 2 : 0);

  if (s == NULL || s->closing || s->rx == NULL || s->kind == NCD_CTRL)
    {
      nc_host_consumed(&g_ncd->h, chan, n);
      return;
    }

  if (s->len + need > s->cap)
    {
      ncd_err("chan %u: receive overflow (%u + %u > %u)\n", chan,
              (unsigned)s->len, (unsigned)need, (unsigned)s->cap);
      nc_host_consumed(&g_ncd->h, chan, n);
      return;
    }

  if (type == NC_T_DGRAM)
    {
      uint8_t b[2];

      b[0] = (uint8_t)n;
      b[1] = (uint8_t)(n >> 8);
      rx_put(s, b, 2);
    }

  rx_put(s, p, n);
  if (!s->sig_recv)
    {
      s->sig_recv = true;
      ncd_us_event(s->id, USRSOCK_EVENT_RECVFROM_AVAIL);
    }
}

/* More transmit credit */

void ncd_sock_credit(uint8_t chan)
{
  FAR struct ncd_sock *s = g_ncd->sock[chan];

  if (s != NULL && !s->closing && !s->sig_send && s->open &&
      s->pend_req != USRSOCK_REQUEST_CONNECT && can_send(s))
    {
      s->sig_send = true;
      ncd_us_event(s->id, USRSOCK_EVENT_SENDTO_READY);
    }
}

/* Channel events */

void ncd_sock_event(uint8_t chan, uint16_t code, FAR const uint8_t *p,
                    size_t n)
{
  FAR struct ncd_sock *s = g_ncd->sock[chan];
  struct nc_rd rd;

  if (s == NULL || s->closing)
    {
      return;
    }

  switch (code)
    {
      case NC_EV_CHAN_EOF:
        s->eof = true;
        s->sig_recv = true;
        ncd_us_event(s->id, USRSOCK_EVENT_REMOTE_CLOSED |
                            USRSOCK_EVENT_RECVFROM_AVAIL);
        break;

      case NC_EV_CHAN_ERROR:
        nc_rd_init(&rd, p, n);
        s->err = -(int32_t)nc_rd_u32(&rd);
        if (s->err <= 0)
          {
            s->err = ECONNRESET;
          }

        s->sig_recv = true;
        ncd_us_event(s->id, USRSOCK_EVENT_REMOTE_CLOSED |
                            USRSOCK_EVENT_RECVFROM_AVAIL);
        break;

      case NC_EV_CHAN_ACCEPT:
        nc_rd_init(&rd, p, n);
        s->accept_pending = nc_rd_u16(&rd);
        if (s->accept_pending == 0)
          {
            s->accept_pending = 1;
          }

        if (!s->sig_recv)
          {
            s->sig_recv = true;
            ncd_us_event(s->id, USRSOCK_EVENT_RECVFROM_AVAIL);
          }
        break;

      case NC_EV_SSH_HOSTKEY:
        free(s->hostkey);
        s->hostkey = malloc(n);
        if (s->hostkey != NULL)
          {
            memcpy(s->hostkey, p, n);
            s->hostkey_n = n;
          }

        /* The program decides (NCX_SSH_HOSTKEY) */

        pend_done(s, -ENOKEY, 0);
        break;

      case NC_EV_SSH_EXIT:
        free(s->exitb);
        s->exitb = malloc(n > 0 ? n : 1);
        if (s->exitb != NULL)
          {
            memcpy(s->exitb, p, n);
            s->exit_n = n;
          }
        break;

      default:
        break;
    }
}

/* The link went down: every socket is gone */

void ncd_sock_link_down(void)
{
  int i;

  for (i = 1; i < NCD_NCHAN; i++)
    {
      FAR struct ncd_sock *s = g_ncd->sock[i];

      if (s == NULL)
        {
          continue;
        }

      if (s->closing)
        {
          sock_free(s);
          continue;
        }

      s->aborted     = true;
      s->open        = false;
      s->ssh_opening = false;
      s->pend_xid    = 0;
      s->pend_req    = NCD_NOREQ;
      ncd_us_event(s->id, USRSOCK_EVENT_ABORT);
    }
}

/* After the link has moved: sockets that were refused for want of room on
 * the link may send again
 */

void ncd_sock_poll(void)
{
  int i;

  if (nc_link_room(&g_ncd->h.link) == 0)
    {
      return;
    }

  for (i = 1; i < NCD_NCHAN; i++)
    {
      FAR struct ncd_sock *s = g_ncd->sock[i];

      if (s != NULL && !s->sig_send && !s->closing && s->open &&
          s->pend_req != USRSOCK_REQUEST_CONNECT && can_send(s))
        {
          s->sig_send = true;
          ncd_us_event(s->id, USRSOCK_EVENT_SENDTO_READY);
        }
    }
}
