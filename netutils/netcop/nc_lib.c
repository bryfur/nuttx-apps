/****************************************************************************
 * apps/netutils/netcop/nc_lib.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The client side of netcopd (apps/include/netutils/netcop.h): the control
 * socket, TLS on a socket, names.  Everything goes through sockets, so it
 * works from any task or loadable module.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "netutils/netcop.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static uint16_t g_tag;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int ncx_open(void)
{
  return socket(AF_INET, SOCK_RAW, NCX_PROTO_CTRL);
}

int ncx_send(int fd, uint16_t tag, uint16_t op, FAR const void *body,
             size_t n)
{
  struct ncx_req rq;
  struct iovec iov[2];
  struct msghdr msg;

  rq.tag = tag;
  rq.op  = op;
  iov[0].iov_base = &rq;
  iov[0].iov_len  = sizeof(rq);
  iov[1].iov_base = (FAR void *)body;
  iov[1].iov_len  = n;
  memset(&msg, 0, sizeof(msg));
  msg.msg_iov    = iov;
  msg.msg_iovlen = n > 0 ? 2 : 1;
  return sendmsg(fd, &msg, 0) < 0 ? -errno : 0;
}

ssize_t ncx_recv(int fd, FAR struct ncx_msg *m, FAR void *body, size_t cap)
{
  FAR uint8_t *buf;
  ssize_t n;

  /* (usrsock's recvmsg takes a single buffer) */

  buf = malloc(sizeof(*m) + cap);
  if (buf == NULL)
    {
      errno = ENOMEM;
      return -1;
    }

  n = recv(fd, buf, sizeof(*m) + cap, 0);
  if (n >= (ssize_t)sizeof(*m))
    {
      memcpy(m, buf, sizeof(*m));
      memcpy(body, buf + sizeof(*m), n - sizeof(*m));
      n -= sizeof(*m);
    }
  else if (n >= 0)
    {
      errno = EBADMSG;
      n = -1;
    }

  free(buf);
  return n;
}

int ncx_call(int fd, uint16_t op, FAR const void *body, size_t n,
             FAR struct ncx_rsp *rsp, ncx_part_cb part, FAR void *arg,
             int timeout_ms)
{
  struct ncx_msg m;
  struct pollfd pfd;
  FAR uint8_t *pbuf;
  uint32_t end;
  uint16_t tag;
  ssize_t k;
  int ret;

  if (timeout_ms <= 0)
    {
      timeout_ms = 30000;
    }

  tag = ++g_tag;
  if (tag == 0)
    {
      tag = ++g_tag;
    }

  ret = ncx_send(fd, tag, op, body, n);
  if (ret < 0)
    {
      return ret;
    }

  /* (Not static: tasks and modules share this code) */

  pbuf = malloc(NCX_MAXMSG);
  if (pbuf == NULL)
    {
      return -ENOMEM;
    }

  end = now_ms() + timeout_ms;
  for (; ; )
    {
      int32_t left = (int32_t)(end - now_ms());

      if (left <= 0)
        {
          ret = -ETIMEDOUT;
          break;
        }

      pfd.fd      = fd;
      pfd.events  = POLLIN;
      pfd.revents = 0;
      ret = poll(&pfd, 1, left);
      if (ret < 0)
        {
          if (errno == EINTR)
            {
              ret = -EINTR;
              break;
            }

          continue;
        }

      if (ret == 0)
        {
          continue;
        }

      k = ncx_recv(fd, &m, pbuf, NCX_MAXMSG);
      if (k < 0)
        {
          if (errno == EAGAIN || errno == EINTR)
            {
              continue;
            }

          ret = -errno;
          break;
        }

      if (m.tag != tag || m.kind == NCX_MSG_EVENT)
        {
          continue;
        }

      if (m.kind == NCX_MSG_PART)
        {
          if (part != NULL)
            {
              part(arg, pbuf, k);
            }

          continue;
        }

      if (rsp != NULL)
        {
          rsp->status = m.status;
          rsp->n      = k;
          memcpy(rsp->body, pbuf, k);
        }

      ret = m.status;
      break;
    }

  free(pbuf);
  return ret;
}

int ncx_call1(uint16_t op, FAR const void *body, size_t n,
              FAR struct ncx_rsp *rsp, ncx_part_cb part, FAR void *arg,
              int timeout_ms)
{
  int fd = ncx_open();
  int ret;

  if (fd < 0)
    {
      return -ENETDOWN;
    }

  ret = ncx_call(fd, op, body, n, rsp, part, arg, timeout_ms);
  close(fd);
  return ret;
}

int ncx_subscribe(int fd, uint32_t mask)
{
  uint8_t b[4];

  b[0] = (uint8_t)mask;
  b[1] = (uint8_t)(mask >> 8);
  b[2] = (uint8_t)(mask >> 16);
  b[3] = (uint8_t)(mask >> 24);
  return ncx_call(fd, NCX_OP_SUBSCRIBE, b, 4, NULL, NULL, NULL, 2000);
}

int ncx_state(FAR struct ncx_state *st)
{
  FAR struct ncx_rsp *rsp = malloc(sizeof(*rsp));
  int ret;

  memset(st, 0, sizeof(*st));
  if (rsp == NULL)
    {
      return -ENOMEM;
    }

  ret = ncx_call1(NCX_OP_STATE, NULL, 0, rsp, NULL, NULL, 2000);
  if (ret >= 0)
    {
      memcpy(st, rsp->body, rsp->n < sizeof(*st) ? rsp->n : sizeof(*st));
    }

  free(rsp);
  return ret < 0 ? ret : 0;
}

int ncx_tls_start(int sd, FAR const char *host, FAR const char *alpn,
                  bool verify, FAR struct nc_tls_info *info)
{
  uint8_t buf[sizeof(struct nc_tls_info) + 64];
  socklen_t len;
  int32_t v;
  int ret = 0;

  if (info != NULL)
    {
      memset(info, 0, sizeof(*info));
    }

  if (host != NULL && host[0] != '\0' &&
      setsockopt(sd, SOL_TLS, NC_TLS_SNI, host, strlen(host)) < 0)
    {
      return -errno;
    }

  if (alpn != NULL && alpn[0] != '\0' &&
      setsockopt(sd, SOL_TLS, NC_TLS_ALPN, alpn, strlen(alpn)) < 0)
    {
      return -errno;
    }

  v = verify ? 1 : 0;
  if (setsockopt(sd, SOL_TLS, NC_TLS_VERIFY, &v, sizeof(v)) < 0)
    {
      return -errno;
    }

  /* (A value is needed: NuttX refuses a NULL one with EFAULT) */

  v = 1;
  if (setsockopt(sd, SOL_TLS, NC_TLS_START, &v, sizeof(v)) < 0)
    {
      ret = -errno;
    }

  if (info != NULL)
    {
      len = sizeof(buf);
      if (getsockopt(sd, SOL_TLS, NC_TLS_INFO, buf, &len) == 0)
        {
          struct nc_rd rd;

          nc_rd_init(&rd, buf, len);
          nc_dec_tls_info(&rd, info);
        }
    }

  return ret;
}

FAR const char *ncx_wifi_state_str(uint8_t state)
{
  static const char *const names[] =
  {
    "idle", "connecting", "connected", "up", "retrying"
  };

  return state < sizeof(names) / sizeof(names[0]) ? names[state] : "?";
}

FAR const char *ncx_wifi_auth_str(uint8_t auth)
{
  static const char *const names[] =
  {
    "open", "WEP", "WPA", "WPA2", "WPA/WPA2", "WPA3", "WPA2/WPA3", "802.1X"
  };

  return auth < sizeof(names) / sizeof(names[0]) ? names[auth] : "?";
}

FAR const char *ncx_wifi_reason_str(uint8_t reason)
{
  static const char *const names[] =
  {
    "none", "wrong password", "network not found", "connection lost",
    "disconnected", "no address", "failed"
  };

  return reason < sizeof(names) / sizeof(names[0]) ? names[reason] : "?";
}

FAR const char *ncx_tls_verify_str(uint8_t verify)
{
  static const char *const names[] =
  {
    "verified", "not verified", "untrusted certificate",
    "certificate expired or not yet valid", "name mismatch",
    "the coprocessor's clock is not set", "verification failed"
  };

  return verify < sizeof(names) / sizeof(names[0]) ? names[verify] : "?";
}

int ncx_rssi_bars(int8_t rssi)
{
  return rssi >= -55 ? 4 : rssi >= -65 ? 3 : rssi >= -75 ? 2 :
         rssi >= -85 ? 1 : 0;
}
