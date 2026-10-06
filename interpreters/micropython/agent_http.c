/****************************************************************************
 * apps/interpreters/micropython/agent_http.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * _agent.Http: an HTTP/1.1 client connection over the network
 * coprocessor (docs/network.md), TLS done by the coprocessor
 * (ncx_tls_start), as curl does (apps/netutils/nctools/curl.c).  Kept
 * alive between requests when the server allows (a TLS handshake on the
 * coprocessor takes a while); the response read as it comes, as bytes or
 * as server-sent events.
 *
 *   c = Http(host, port, tls=False, verify=True, timeout=120)
 *   c.request(method, path, headers="", body=None) -> status
 *   c.header(name) -> value or None; c.headers() -> the header block
 *   c.read(n=4096, wait_ms=-1) -> bytes (b"" at the end, None when
 *                                 wait_ms passed with nothing)
 *   c.sse(wait_ms=-1) -> (event, data); None at the end; False when
 *                        wait_ms passed with nothing
 *   c.reusable          the response is read and the server keeps the
 *                       connection
 *   c.close()
 *   netstate() -> "ready", "sync", "none" (no coprocessor), "nodaemon"
 *
 * Waiting (for the server, its data) watches the terminal: Ctrl-C (and
 * Esc while asked for) raises KeyboardInterrupt.  Nothing for timeout
 * seconds raises OSError(ETIMEDOUT).
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include "py/objstr.h"
#include "py/runtime.h"

#include "netutils/netcop.h"

#include "modagent.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BUFSIZE        4096
#define HDRMAX         8192            /* The response's headers */

/* The body, as the response says */

#define BODY_NONE      0
#define BODY_LENGTH    1
#define BODY_CHUNKED   2
#define BODY_CLOSE     3

/* Chunked: where we are */

#define CH_SIZE        0
#define CH_DATA        1
#define CH_CRLF        2
#define CH_TRAILER     3

/****************************************************************************
 * Private Types
 ****************************************************************************/

typedef struct
{
  mp_obj_base_t base;
  int        sd;
  int        timeout_ms;
  char       host[NC_HOST_MAX + 1];
  int        port;
  bool       tls;

  /* The response */

  int        status;
  mp_obj_t   headers;           /* str */
  int        mode;              /* BODY_* */
  int        chstate;           /* CH_* */
  long long  remain;            /* Of the body or the chunk */
  bool       done;              /* The body read */
  bool       keepalive;
  bool       eof;               /* The server closed */
  bool       reused;            /* Not the connection's first request */

  /* Server-sent events */

  vstr_t     line;
  vstr_t     data;
  bool       hasdata;
  char       event[32];

  size_t     blen;
  size_t     boff;
  uint8_t    buf[BUFSIZE];
} http_obj_t;

/****************************************************************************
 * Private Functions: the connection
 ****************************************************************************/

static void http_shut(http_obj_t *o)
{
  if (o->sd >= 0)
    {
      close(o->sd);
      o->sd = -1;
    }
}

static http_obj_t *http_self(mp_obj_t self)
{
  http_obj_t *o = MP_OBJ_TO_PTR(self);

  if (o->sd < 0)
    {
      agent_raise(EBADF, "connection closed");
    }

  return o;
}

/* "no network" or why: from the coprocessor's state */

static bool no_network(void)
{
  struct ncx_state st;

  return ncx_state(&st) < 0 || st.link != NCX_LINK_READY;
}

static void http_connect(http_obj_t *o, bool verify)
{
  struct addrinfo hints;
  struct addrinfo *res;
  struct addrinfo *ai;
  char port[8];
  int ret;
  int sd = -1;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family   = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(port, sizeof(port), "%d", o->port);
  ret = getaddrinfo(o->host, port, &hints, &res);
  if (ret != 0)
    {
      if (no_network())
        {
          agent_raise(ENETUNREACH, "no network (no network coprocessor)");
        }

      agent_raise(ENOENT, "could not resolve host: %s", o->host);
    }

  ret = -ENETUNREACH;
  for (ai = res; ai != NULL; ai = ai->ai_next)
    {
      sd = socket(ai->ai_family, SOCK_STREAM, 0);
      if (sd < 0)
        {
          ret = -errno;
          continue;
        }

      if (connect(sd, ai->ai_addr, ai->ai_addrlen) == 0)
        {
          break;
        }

      ret = -errno;
      close(sd);
      sd = -1;
    }

  freeaddrinfo(res);
  if (sd < 0)
    {
      if (ret == -ENETUNREACH && no_network())
        {
          agent_raise(ENETUNREACH, "no network (no network coprocessor)");
        }

      agent_raise(-ret, "connect to %s port %d: %s", o->host, o->port,
                  ret == -ENETUNREACH ? "network unreachable (no WiFi?)" :
                  strerror(-ret));
    }

  /* Not for the programs NSH commands start (agent_proc.c) */

  fcntl(sd, F_SETFD, FD_CLOEXEC);
  if (o->tls)
    {
      struct nc_tls_info ti;

      memset(&ti, 0, sizeof(ti));
      ret = ncx_tls_start(sd, o->host, "http/1.1", verify, &ti);
      if (ret < 0)
        {
          close(sd);
          agent_raise(-ret, "TLS with %s: %s%s%s", o->host,
                      ret == -ETIME ? "the coprocessor's clock is not set" :
                      ret == -EKEYEXPIRED ?
                      "certificate expired or not yet valid" :
                      ret == -EKEYREJECTED ? "certificate not trusted" :
                      strerror(-ret),
                      ti.verify > NC_TLSV_SKIPPED ? ": " : "",
                      ti.verify > NC_TLSV_SKIPPED ?
                      ncx_tls_verify_str(ti.verify) : "");
        }
    }

  o->sd = sd;
}

static void send_all(http_obj_t *o, const void *p, size_t n)
{
  const uint8_t *b = p;

  while (n > 0)
    {
      size_t k = n > 2048 ? 2048 : n;
      ssize_t r;

      if (agent_wait_fd(o->sd, POLLOUT, o->timeout_ms) == 0)
        {
          http_shut(o);
          agent_raise(ETIMEDOUT, "send: timed out");
        }

      r = send(o->sd, b, k, 0);
      if (r < 0)
        {
          int err = errno;

          if (err == EINTR || err == EAGAIN)
            {
              continue;
            }

          http_shut(o);
          agent_raise(err, "send: %s", strerror(err));
        }

      b += r;
      n -= r;
    }
}

/* More of the response into the buffer: > 0 bytes, 0 the server closed,
 * -1 nothing within wait_ms (-1: up to the timeout, then OSError)
 */

static int fill(http_obj_t *o, int wait_ms)
{
  ssize_t n;
  int ms = wait_ms >= 0 && wait_ms < o->timeout_ms ? wait_ms :
           o->timeout_ms;

  if (o->boff > 0)
    {
      memmove(o->buf, o->buf + o->boff, o->blen - o->boff);
      o->blen -= o->boff;
      o->boff  = 0;
    }

  if (o->blen == sizeof(o->buf))
    {
      return 1;
    }

  if (o->eof)
    {
      return 0;
    }

  if (agent_wait_fd(o->sd, POLLIN, ms) == 0)
    {
      if (wait_ms >= 0 && wait_ms < o->timeout_ms)
        {
          return -1;
        }

      http_shut(o);
      agent_raise(ETIMEDOUT, "no answer from %s in %d s", o->host,
                  o->timeout_ms / 1000);
    }

  n = recv(o->sd, o->buf + o->blen, sizeof(o->buf) - o->blen, 0);
  if (n < 0)
    {
      int err = errno;

      if (err == EINTR || err == EAGAIN)
        {
          return -1;
        }

      http_shut(o);
      agent_raise(err, "receive: %s", strerror(err));
    }

  if (n == 0)
    {
      o->eof = true;
    }

  o->blen += n;
  return n;
}

/* A line of the head (CRLF or LF) */

static size_t get_line(http_obj_t *o, char *line, size_t cap)
{
  for (; ; )
    {
      uint8_t *s = o->buf + o->boff;
      uint8_t *nl = memchr(s, '\n', o->blen - o->boff);

      if (nl != NULL)
        {
          size_t n = nl - s;

          if (n > 0 && s[n - 1] == '\r')
            {
              n--;
            }

          if (n >= cap)
            {
              n = cap - 1;
            }

          memcpy(line, s, n);
          line[n] = '\0';
          o->boff += nl - s + 1;
          return n;
        }

      if (o->blen - o->boff == sizeof(o->buf))
        {
          o->boff = o->blen;            /* A line too long: dropped */
        }

      if (fill(o, -1) == 0)
        {
          http_shut(o);
          agent_raise(ECONNRESET, "the server closed the connection");
        }
    }
}

/* The body's bytes available now, contiguous, not taken (*p); 0 at the
 * end; -1 when wait_ms passed with nothing
 */

static int body_avail(http_obj_t *o, int wait_ms, const uint8_t **p)
{
  char line[80];

  for (; ; )
    {
      size_t have = o->blen - o->boff;
      int r;

      if (o->done)
        {
          return 0;
        }

      if (o->mode == BODY_CHUNKED && o->chstate != CH_DATA)
        {
          /* A chunk's size, the CRLF after its data, the trailers: whole
           * lines
           */

          if (memchr(o->buf + o->boff, '\n', have) == NULL)
            {
              r = fill(o, wait_ms);
              if (r < 0)
                {
                  return -1;
                }

              if (r == 0)
                {
                  http_shut(o);
                  agent_raise(ECONNRESET, "the response was cut short");
                }

              continue;
            }

          get_line(o, line, sizeof(line));
          if (o->chstate == CH_SIZE)
            {
              o->remain = strtoll(line, NULL, 16);
              o->chstate = o->remain > 0 ? CH_DATA : CH_TRAILER;
            }
          else if (o->chstate == CH_CRLF)
            {
              o->chstate = CH_SIZE;
            }
          else if (line[0] == '\0')
            {
              o->done = true;          /* The end of the trailers */
            }

          continue;
        }

      if ((o->mode == BODY_LENGTH || o->mode == BODY_CHUNKED) &&
          o->remain == 0)
        {
          if (o->mode == BODY_CHUNKED)
            {
              o->chstate = CH_CRLF;
              continue;
            }

          o->done = true;
          return 0;
        }

      if (have > 0)
        {
          if (o->mode != BODY_CLOSE && (long long)have > o->remain)
            {
              have = o->remain;
            }

          *p = o->buf + o->boff;
          return have;
        }

      r = fill(o, wait_ms);
      if (r < 0)
        {
          return -1;
        }

      if (r == 0)
        {
          if (o->mode == BODY_CLOSE)
            {
              o->done = true;
              return 0;
            }

          http_shut(o);
          agent_raise(ECONNRESET, "the response was cut short");
        }
    }
}

static void body_take(http_obj_t *o, size_t n)
{
  o->boff += n;
  if (o->mode != BODY_CLOSE)
    {
      o->remain -= n;
    }
}

/****************************************************************************
 * Private Functions: the type
 ****************************************************************************/

/* Http(host, port, tls=False, verify=True, timeout=120) */

static mp_obj_t http_make_new(const mp_obj_type_t *type, size_t n_args,
                              size_t n_kw, const mp_obj_t *args)
{
  enum
  {
    A_HOST, A_PORT, A_TLS, A_VERIFY, A_TIMEOUT
  };

  static const mp_arg_t allowed[] =
  {
    { MP_QSTR_host,    MP_ARG_REQUIRED | MP_ARG_OBJ, {.u_obj = MP_OBJ_NULL} },
    { MP_QSTR_port,    MP_ARG_REQUIRED | MP_ARG_INT, {.u_int = 0} },
    { MP_QSTR_tls,     MP_ARG_BOOL, {.u_bool = false} },
    { MP_QSTR_verify,  MP_ARG_BOOL, {.u_bool = true} },
    { MP_QSTR_timeout, MP_ARG_INT,  {.u_int = 120} },
  };

  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
  mp_map_t kw;
  http_obj_t *o;
  const char *host;

  mp_map_init_fixed_table(&kw, n_kw, args + n_args);
  mp_arg_parse_all(n_args, args, &kw, MP_ARRAY_SIZE(allowed), allowed, a);
  host = mp_obj_str_get_str(a[A_HOST].u_obj);
  if (strlen(host) > NC_HOST_MAX || a[A_PORT].u_int <= 0 ||
      a[A_PORT].u_int > 65535)
    {
      mp_raise_ValueError(MP_ERROR_TEXT("bad host or port"));
    }

  o = mp_obj_malloc_with_finaliser(http_obj_t, type);
  memset((char *)o + sizeof(o->base), 0, sizeof(*o) - sizeof(o->base));
  o->sd         = -1;
  o->headers    = mp_const_none;
  o->timeout_ms = (a[A_TIMEOUT].u_int > 0 ? a[A_TIMEOUT].u_int : 120) *
                  1000;
  strlcpy(o->host, host, sizeof(o->host));
  o->port = a[A_PORT].u_int;
  o->tls  = a[A_TLS].u_bool;
  o->done = true;
  vstr_init(&o->line, 128);
  vstr_init(&o->data, 256);
  http_connect(o, a[A_VERIFY].u_bool);
  return MP_OBJ_FROM_PTR(o);
}

/* request(method, path, headers="", body=None) -> status */

static mp_obj_t http_request(size_t n_args, const mp_obj_t *args)
{
  http_obj_t *o = http_self(args[0]);
  const char *method = mp_obj_str_get_str(args[1]);
  const char *path = mp_obj_str_get_str(args[2]);
  size_t hn = 0;
  const char *hdrs = n_args > 3 ? mp_obj_str_get_data(args[3], &hn) : "";
  mp_buffer_info_t body;
  bool with_body = n_args > 4 && args[4] != mp_const_none;
  vstr_t req;
  vstr_t head;
  char line[512];
  bool http10;
  bool hostport = o->port != (o->tls ? 443 : 80);

  if (!o->done)
    {
      agent_raise(EBUSY, "the last response is not read");
    }

  /* A kept connection the server has since closed: said at once, so the
   * caller connects again
   */

  if (o->reused || o->eof)
    {
      struct pollfd pfd;

      pfd.fd      = o->sd;
      pfd.events  = POLLIN;
      pfd.revents = 0;
      /* Readable while idle: closed (or talking out of turn) */

      if (o->eof || o->blen > o->boff || poll(&pfd, 1, 0) > 0)
        {
          http_shut(o);
          agent_raise(ECONNRESET, "the kept connection was closed");
        }
    }

  if (with_body)
    {
      mp_get_buffer_raise(args[4], &body, MP_BUFFER_READ);
    }

  vstr_init(&req, 256 + hn);
  vstr_printf(&req, "%s %s HTTP/1.1\r\nHost: %s", method, path, o->host);
  if (hostport)
    {
      vstr_printf(&req, ":%d", o->port);
    }

  vstr_add_str(&req, "\r\n");
  vstr_add_strn(&req, hdrs, hn);
  if (hn > 0 && hdrs[hn - 1] != '\n')
    {
      vstr_add_str(&req, "\r\n");
    }

  if (with_body)
    {
      vstr_printf(&req, "Content-Length: %u\r\n", (unsigned)body.len);
    }

  vstr_add_str(&req, "\r\n");
  send_all(o, req.buf, req.len);
  vstr_clear(&req);
  if (with_body && body.len > 0)
    {
      send_all(o, body.buf, body.len);
    }

  /* The status line, then the headers */

  o->blen = o->boff = 0;
  o->status    = 0;
  o->mode      = BODY_CLOSE;
  o->chstate   = CH_SIZE;
  o->remain    = 0;
  o->done      = false;
  o->keepalive = true;
  o->hasdata   = false;
  o->event[0]  = '\0';
  vstr_reset(&o->line);
  vstr_reset(&o->data);

  do
    {
      get_line(o, line, sizeof(line));
      if (sscanf(line, "HTTP/%*d.%*d %d", &o->status) != 1)
        {
          http_shut(o);
          agent_raise(EPROTO, "not an HTTP response from %s", o->host);
        }

      http10 = strncmp(line, "HTTP/1.0", 8) == 0;
      vstr_init(&head, 256);
      for (; ; )
        {
          size_t n = get_line(o, line, sizeof(line));

          if (n == 0)
            {
              break;
            }

          if (head.len + n < HDRMAX)
            {
              vstr_add_strn(&head, line, n);
              vstr_add_byte(&head, '\n');
            }

          if (strncasecmp(line, "Content-Length:", 15) == 0)
            {
              o->mode   = BODY_LENGTH;
              o->remain = strtoll(line + 15, NULL, 10);
            }
          else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
                   strcasestr(line + 18, "chunked") != NULL)
            {
              o->mode = BODY_CHUNKED;
            }
          else if (strncasecmp(line, "Connection:", 11) == 0 &&
                   strcasestr(line + 11, "close") != NULL)
            {
              o->keepalive = false;
            }
        }
    }
  while (o->status == 100);           /* 100 Continue: the real one next */

  if (http10)
    {
      o->keepalive = false;
    }

  if (o->mode == BODY_CLOSE)
    {
      o->keepalive = false;
    }

  if (strcmp(method, "HEAD") == 0 || o->status == 204 || o->status == 304)
    {
      o->mode = BODY_NONE;
      o->done = true;
    }

  o->headers = mp_obj_new_str_from_vstr(&head);
  o->reused  = true;
  return MP_OBJ_NEW_SMALL_INT(o->status);
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(http_request_obj, 3, 5,
                                           http_request);

static mp_obj_t http_header(mp_obj_t self, mp_obj_t name_in)
{
  http_obj_t *o = MP_OBJ_TO_PTR(self);
  size_t nn;
  size_t hn;
  const char *name = mp_obj_str_get_data(name_in, &nn);
  const char *h;
  const char *end;

  if (o->headers == mp_const_none)
    {
      return mp_const_none;
    }

  h   = mp_obj_str_get_data(o->headers, &hn);
  end = h + hn;
  while (h < end)
    {
      const char *e = memchr(h, '\n', end - h);

      if (e == NULL)
        {
          e = end;
        }

      if ((size_t)(e - h) > nn && h[nn] == ':' &&
          strncasecmp(h, name, nn) == 0)
        {
          const char *v = h + nn + 1;

          while (v < e && *v == ' ')
            {
              v++;
            }

          return mp_obj_new_str(v, e - v);
        }

      h = e + 1;
    }

  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_2(http_header_obj, http_header);

static mp_obj_t http_headers(mp_obj_t self)
{
  http_obj_t *o = MP_OBJ_TO_PTR(self);

  return o->headers;
}

static MP_DEFINE_CONST_FUN_OBJ_1(http_headers_obj, http_headers);

/* read(n=4096, wait_ms=-1) */

static mp_obj_t http_read(size_t n_args, const mp_obj_t *args)
{
  http_obj_t *o = MP_OBJ_TO_PTR(args[0]);
  size_t want = n_args > 1 ? mp_obj_get_int(args[1]) : 4096;
  int wait_ms = n_args > 2 && args[2] != mp_const_none ?
                mp_obj_get_int(args[2]) : -1;
  const uint8_t *p;
  int n;

  if (o->done)
    {
      return mp_const_empty_bytes;
    }

  http_self(args[0]);
  n = body_avail(o, wait_ms, &p);
  if (n < 0)
    {
      return mp_const_none;
    }

  if ((size_t)n > want)
    {
      n = want;
    }

  if (n == 0)
    {
      return mp_const_empty_bytes;
    }

  mp_obj_t b = mp_obj_new_bytes(p, n);
  body_take(o, n);
  return b;
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(http_read_obj, 1, 3, http_read);

/* A line of the event stream: its fields (event, data); true when a blank
 * line ends an event that has data
 */

static bool sse_line(http_obj_t *o)
{
  char *l = o->line.buf;
  size_t n = o->line.len;
  const char *v;
  size_t fn;
  size_t vn;

  if (n > 0 && l[n - 1] == '\r')
    {
      n--;
    }

  if (n == 0)
    {
      return o->hasdata || o->event[0] != '\0';
    }

  if (l[0] == ':')
    {
      return false;                    /* A comment (keep-alive) */
    }

  v = memchr(l, ':', n);
  fn = v != NULL ? (size_t)(v - l) : n;
  if (v != NULL)
    {
      v++;
      if (v < l + n && *v == ' ')
        {
          v++;
        }

      vn = l + n - v;
    }
  else
    {
      v  = "";
      vn = 0;
    }

  if (fn == 5 && memcmp(l, "event", 5) == 0)
    {
      if (vn >= sizeof(o->event))
        {
          vn = sizeof(o->event) - 1;
        }

      memcpy(o->event, v, vn);
      o->event[vn] = '\0';
    }
  else if (fn == 4 && memcmp(l, "data", 4) == 0)
    {
      if (o->hasdata)
        {
          vstr_add_byte(&o->data, '\n');
        }

      vstr_add_strn(&o->data, v, vn);
      o->hasdata = true;
    }

  return false;
}

static mp_obj_t sse_take(http_obj_t *o)
{
  mp_obj_t t[2];

  t[0] = mp_obj_new_str_from_cstr(o->event[0] != '\0' ? o->event :
                                  "message");
  t[1] = mp_obj_new_str(o->data.buf, o->data.len);
  o->event[0] = '\0';
  o->hasdata  = false;
  vstr_reset(&o->data);
  return mp_obj_new_tuple(2, t);
}

/* sse(wait_ms=-1) */

static mp_obj_t http_sse(size_t n_args, const mp_obj_t *args)
{
  http_obj_t *o = MP_OBJ_TO_PTR(args[0]);
  int wait_ms = n_args > 1 && args[1] != mp_const_none ?
                mp_obj_get_int(args[1]) : -1;

  for (; ; )
    {
      const uint8_t *p;
      const uint8_t *nl;
      int n;

      n = o->done ? 0 : (http_self(args[0]), body_avail(o, wait_ms, &p));
      if (n < 0)
        {
          return mp_const_false;
        }

      if (n == 0)
        {
          /* The end: an event without its blank line still counts */

          if (o->line.len > 0)
            {
              sse_line(o);
              vstr_reset(&o->line);
            }

          if (o->hasdata)
            {
              return sse_take(o);
            }

          return mp_const_none;
        }

      nl = memchr(p, '\n', n);
      if (nl == NULL)
        {
          vstr_add_strn(&o->line, (const char *)p, n);
          body_take(o, n);
          continue;
        }

      vstr_add_strn(&o->line, (const char *)p, nl - p);
      body_take(o, nl - p + 1);
      if (sse_line(o))
        {
          vstr_reset(&o->line);
          return sse_take(o);
        }

      vstr_reset(&o->line);
    }
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(http_sse_obj, 1, 2, http_sse);

static mp_obj_t http_close(mp_obj_t self)
{
  http_shut(MP_OBJ_TO_PTR(self));
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(http_close_obj, http_close);

static void http_attr(mp_obj_t self, qstr attr, mp_obj_t *dest)
{
  http_obj_t *o = MP_OBJ_TO_PTR(self);

  if (dest[0] != MP_OBJ_NULL)
    {
      return;
    }

  switch (attr)
    {
      case MP_QSTR_reusable:
        dest[0] = mp_obj_new_bool(o->sd >= 0 && o->done && o->keepalive &&
                                  !o->eof);
        return;

      case MP_QSTR_status:
        dest[0] = MP_OBJ_NEW_SMALL_INT(o->status);
        return;

      case MP_QSTR_closed:
        dest[0] = mp_obj_new_bool(o->sd < 0);
        return;

      case MP_QSTR_host:
        dest[0] = mp_obj_new_str_from_cstr(o->host);
        return;

      default:
        dest[1] = MP_OBJ_SENTINEL;     /* The methods */
        return;
    }
}

static const mp_rom_map_elem_t http_locals[] =
{
  { MP_ROM_QSTR(MP_QSTR_request), MP_ROM_PTR(&http_request_obj) },
  { MP_ROM_QSTR(MP_QSTR_header),  MP_ROM_PTR(&http_header_obj) },
  { MP_ROM_QSTR(MP_QSTR_headers), MP_ROM_PTR(&http_headers_obj) },
  { MP_ROM_QSTR(MP_QSTR_read),    MP_ROM_PTR(&http_read_obj) },
  { MP_ROM_QSTR(MP_QSTR_sse),     MP_ROM_PTR(&http_sse_obj) },
  { MP_ROM_QSTR(MP_QSTR_close),   MP_ROM_PTR(&http_close_obj) },
  { MP_ROM_QSTR(MP_QSTR___del__), MP_ROM_PTR(&http_close_obj) },
};

static MP_DEFINE_CONST_DICT(http_locals_dict, http_locals);

MP_DEFINE_CONST_OBJ_TYPE(
  agent_http_type, MP_QSTR_Http, MP_TYPE_FLAG_NONE,
  make_new, http_make_new,
  attr, http_attr,
  locals_dict, &http_locals_dict);

/****************************************************************************
 * The coprocessor's state
 ****************************************************************************/

static mp_obj_t agent_netstate(void)
{
  struct ncx_state st;

  if (ncx_state(&st) < 0)
    {
      return MP_OBJ_NEW_QSTR(MP_QSTR_nodaemon);
    }

  return MP_OBJ_NEW_QSTR(st.link == NCX_LINK_READY ? MP_QSTR_ready :
                         st.link == NCX_LINK_SYNC ? MP_QSTR_sync :
                         MP_QSTR_none);
}

MP_DEFINE_CONST_FUN_OBJ_0(agent_netstate_obj, agent_netstate);
