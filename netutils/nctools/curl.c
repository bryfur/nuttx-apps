/****************************************************************************
 * apps/netutils/nctools/curl.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * curl (and wget, wget.c): HTTP and HTTPS on the network coprocessor, which
 * does the TLS (setsockopt(SOL_TLS), docs/netcop.md 5.7): no TLS library on
 * the fpc.
 *
 *   curl [-sSvikIL] [-X METHOD] [-H 'Name: value']... [-d DATA|@FILE]
 *        [-A AGENT] [-o FILE | -O] [--max-redirs N] [-m SECONDS] URL
 *
 *   wget [-q] [-O FILE] [--no-check-certificate] URL
 *
 * HTTP/1.1, one request per connection (Connection: close); bodies by
 * Content-Length, chunked transfer coding, or until the server closes.
 * Redirects (301, 302, 303, 307, 308) are followed with -L (wget always).
 * A progress line goes to stderr when the body goes to a file.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/socket.h>

#include "netutils/netcop.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MAXHDRS     16
#define BUFSIZE     2048
#define HDRMAX      4096

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct url_s
{
  bool tls;
  char host[NC_HOST_MAX + 1];
  int  port;
  char path[512];
};

struct curl_s
{
  bool        wget;
  bool        silent;
  bool        show_err;
  bool        verbose;
  bool        include;
  bool        head;
  bool        insecure;
  bool        location;
  bool        remote_name;
  int         max_redirs;
  int         timeout_s;
  const char *method;
  const char *data;
  size_t      data_n;
  char       *data_buf;
  const char *agent;
  const char *out;
  const char *hdr[MAXHDRS];
  int         nhdr;

  /* The response */

  int         sd;
  uint8_t     buf[BUFSIZE];
  size_t      blen;
  size_t      boff;
  int         status;
  long long   length;                  /* -1: unknown */
  bool        chunked;
  char        location_hdr[600];
  FILE       *fout;
  long long   got;
  uint32_t    t0;
  uint32_t    tprog;
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static uint32_t now_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static const char *prog(struct curl_s *c)
{
  return c->wget ? "wget" : "curl";
}

static void err(struct curl_s *c, const char *fmt, ...)
{
  va_list ap;

  if (c->silent && !c->show_err)
    {
      return;
    }

  va_start(ap, fmt);
  fprintf(stderr, "%s: ", prog(c));
  vfprintf(stderr, fmt, ap);
  fprintf(stderr, "\n");
  va_end(ap);
}

static int parse_url(const char *u, const struct url_s *base,
                     struct url_s *o)
{
  const char *p;
  const char *h;
  const char *e;
  size_t n;

  memset(o, 0, sizeof(*o));
  if (u[0] == '/' && base != NULL)
    {
      /* A path on the same server (a redirect's Location) */

      *o = *base;
      strlcpy(o->path, u, sizeof(o->path));
      return 0;
    }

  if (strncasecmp(u, "https://", 8) == 0)
    {
      o->tls  = true;
      o->port = 443;
      h = u + 8;
    }
  else if (strncasecmp(u, "http://", 7) == 0)
    {
      o->port = 80;
      h = u + 7;
    }
  else if (strstr(u, "://") != NULL)
    {
      return -EPROTONOSUPPORT;
    }
  else
    {
      o->port = 80;
      h = u;
    }

  e = h + strcspn(h, "/?#");
  p = h;
  if (*p == '[')
    {
      /* [IPv6] */

      const char *r = memchr(p, ']', e - p);

      if (r == NULL)
        {
          return -EINVAL;
        }

      n = r - p - 1;
      if (n >= sizeof(o->host))
        {
          return -EINVAL;
        }

      memcpy(o->host, p + 1, n);
      p = r + 1;
    }
  else
    {
      const char *colon = memchr(p, ':', e - p);
      const char *hend = colon != NULL ? colon : e;

      n = hend - p;
      if (n == 0 || n >= sizeof(o->host))
        {
          return -EINVAL;
        }

      memcpy(o->host, p, n);
      p = hend;
    }

  if (p < e && *p == ':')
    {
      o->port = atoi(p + 1);
      if (o->port <= 0 || o->port > 65535)
        {
          return -EINVAL;
        }
    }

  strlcpy(o->path, *e == '/' ? e : "/", sizeof(o->path));
  if (*e == '?')
    {
      snprintf(o->path, sizeof(o->path), "/%s", e);
    }

  /* No fragment */

  o->path[strcspn(o->path, "#")] = '\0';
  return 0;
}

static int tcp_connect(struct curl_s *c, const struct url_s *u)
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
  snprintf(port, sizeof(port), "%d", u->port);
  ret = getaddrinfo(u->host, port, &hints, &res);
  if (ret != 0)
    {
      err(c, "could not resolve host: %s", u->host);
      return -ENOENT;
    }

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
      err(c, "connect to %s port %d: %s", u->host, u->port,
          ret == -ENETUNREACH ? "network unreachable (no WiFi?)" :
          strerror(-ret));
      return ret;
    }

  if (c->verbose)
    {
      fprintf(stderr, "* connected to %s port %d\n", u->host, u->port);
    }

  if (u->tls)
    {
      struct nc_tls_info ti;

      ret = ncx_tls_start(sd, u->host, "http/1.1", !c->insecure, &ti);
      if (ret < 0)
        {
          err(c, "TLS with %s: %s%s%s", u->host,
              ret == -ETIME ? "the coprocessor's clock is not set" :
              ret == -EKEYEXPIRED ? "certificate expired or not yet valid" :
              ret == -EKEYREJECTED ? "certificate not trusted" :
              strerror(-ret),
              ti.verify > NC_TLSV_SKIPPED ? ": " : "",
              ti.verify > NC_TLSV_SKIPPED ? ncx_tls_verify_str(ti.verify) :
              "");
          if (ret == -EKEYREJECTED || ret == -EKEYEXPIRED)
            {
              err(c, "(-k%s to go on anyway)",
                  c->wget ? ", --no-check-certificate" : "");
            }

          close(sd);
          return ret;
        }

      if (c->verbose)
        {
          fprintf(stderr, "* TLS 1.%d, %s, certificate %s\n"
                  "*   subject %s, issuer %s\n", ti.version - 10, ti.cipher,
                  ncx_tls_verify_str(ti.verify), ti.subject, ti.issuer);
        }
    }

  return sd;
}

static int send_all(int sd, const void *p, size_t n)
{
  const uint8_t *b = p;

  while (n > 0)
    {
      ssize_t k = send(sd, b, n, 0);

      if (k < 0)
        {
          if (errno == EINTR)
            {
              continue;
            }

          return -errno;
        }

      b += k;
      n -= k;
    }

  return 0;
}

/* Buffered reading of the response */

static int fill(struct curl_s *c)
{
  struct pollfd pfd;
  ssize_t n;
  int ret;

  if (c->boff > 0)
    {
      memmove(c->buf, c->buf + c->boff, c->blen - c->boff);
      c->blen -= c->boff;
      c->boff  = 0;
    }

  if (c->blen == sizeof(c->buf))
    {
      return 1;
    }

  pfd.fd     = c->sd;
  pfd.events = POLLIN;
  ret = poll(&pfd, 1, c->timeout_s * 1000);
  if (ret == 0)
    {
      return -ETIMEDOUT;
    }

  n = recv(c->sd, c->buf + c->blen, sizeof(c->buf) - c->blen, 0);
  if (n < 0)
    {
      return -errno;
    }

  c->blen += n;
  return n;                            /* 0: the server closed */
}

/* One line (CRLF or LF), NUL-terminated in line */

static int get_line(struct curl_s *c, char *line, size_t cap)
{
  for (; ; )
    {
      uint8_t *s = c->buf + c->boff;
      uint8_t *nl = memchr(s, '\n', c->blen - c->boff);

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
          c->boff += nl - s + 1;
          return n;
        }

      int ret = fill(c);
      if (ret <= 0)
        {
          return ret == 0 ? -ECONNRESET : ret;
        }
    }
}

static void progress(struct curl_s *c, bool done)
{
  uint32_t t = now_ms();
  uint32_t dt;

  if (c->silent || c->fout == stdout || c->fout == NULL ||
      (!done && t - c->tprog < 250))
    {
      return;
    }

  c->tprog = t;
  dt = t - c->t0 > 0 ? t - c->t0 : 1;
  if (c->length > 0)
    {
      fprintf(stderr, "\r%3d%% %10lld of %lld bytes  %lu KB/s ",
              (int)(c->got * 100 / c->length), c->got, c->length,
              (unsigned long)(c->got * 1000 / 1024 / dt));
    }
  else
    {
      fprintf(stderr, "\r     %10lld bytes  %lu KB/s ", c->got,
              (unsigned long)(c->got * 1000 / 1024 / dt));
    }

  if (done)
    {
      fprintf(stderr, "\n");
    }
}

static int out_body(struct curl_s *c, const uint8_t *p, size_t n)
{
  if (c->fout != NULL && fwrite(p, 1, n, c->fout) != n)
    {
      return -EIO;
    }

  c->got += n;
  progress(c, false);
  return 0;
}

/* Copy up to n body bytes (n < 0: until the server closes) */

static int copy_body(struct curl_s *c, long long n)
{
  int ret;

  for (; ; )
    {
      size_t have = c->blen - c->boff;

      if (n >= 0 && (long long)have > n)
        {
          have = n;
        }

      if (have > 0)
        {
          ret = out_body(c, c->buf + c->boff, have);
          if (ret < 0)
            {
              return ret;
            }

          c->boff += have;
          if (n >= 0)
            {
              n -= have;
            }
        }

      if (n == 0)
        {
          return 0;
        }

      ret = fill(c);
      if (ret < 0)
        {
          return ret;
        }

      if (ret == 0)
        {
          return n < 0 ? 0 : -ECONNRESET;
        }
    }
}

static int read_chunked(struct curl_s *c)
{
  char line[64];
  long long n;
  int ret;

  for (; ; )
    {
      ret = get_line(c, line, sizeof(line));
      if (ret < 0)
        {
          return ret;
        }

      n = strtoll(line, NULL, 16);
      if (n == 0)
        {
          /* Trailers, up to the empty line */

          do
            {
              ret = get_line(c, line, sizeof(line));
            }
          while (ret > 0);

          return 0;
        }

      ret = copy_body(c, n);
      if (ret < 0)
        {
          return ret;
        }

      ret = get_line(c, line, sizeof(line));      /* The chunk's CRLF */
      if (ret < 0)
        {
          return ret;
        }
    }
}

static bool has_hdr(struct curl_s *c, const char *name)
{
  size_t n = strlen(name);
  int i;

  for (i = 0; i < c->nhdr; i++)
    {
      if (strncasecmp(c->hdr[i], name, n) == 0 && c->hdr[i][n] == ':')
        {
          return true;
        }
    }

  return false;
}

/* One request and its response; returns the status or -errno */

static int request(struct curl_s *c, const struct url_s *u,
                   const char *method, bool with_body)
{
  char *req;
  char line[600];
  size_t n;
  int ret;
  int i;
  bool hostport = u->port != (u->tls ? 443 : 80);

  c->sd = tcp_connect(c, u);
  if (c->sd < 0)
    {
      return c->sd;
    }

  req = malloc(HDRMAX);
  if (req == NULL)
    {
      close(c->sd);
      return -ENOMEM;
    }

  n = snprintf(req, HDRMAX, "%s %s HTTP/1.1\r\n", method, u->path);
  if (!has_hdr(c, "Host"))
    {
      n += snprintf(req + n, HDRMAX - n, strchr(u->host, ':') ?
                    "Host: [%s]" : "Host: %s", u->host);
      n += hostport ? snprintf(req + n, HDRMAX - n, ":%d", u->port) : 0;
      n += snprintf(req + n, HDRMAX - n, "\r\n");
    }

  if (!has_hdr(c, "User-Agent"))
    {
      n += snprintf(req + n, HDRMAX - n, "User-Agent: %s\r\n",
                    c->agent != NULL ? c->agent :
                    c->wget ? "Wget/1.21 (fpc)" : "curl/8.0 (fpc)");
    }

  if (!has_hdr(c, "Accept"))
    {
      n += snprintf(req + n, HDRMAX - n, "Accept: */*\r\n");
    }

  n += snprintf(req + n, HDRMAX - n, "Connection: close\r\n");
  if (with_body)
    {
      if (!has_hdr(c, "Content-Type"))
        {
          n += snprintf(req + n, HDRMAX - n, "Content-Type: "
                        "application/x-www-form-urlencoded\r\n");
        }

      n += snprintf(req + n, HDRMAX - n, "Content-Length: %zu\r\n",
                    c->data_n);
    }

  for (i = 0; i < c->nhdr; i++)
    {
      n += snprintf(req + n, HDRMAX - n, "%s\r\n", c->hdr[i]);
    }

  n += snprintf(req + n, HDRMAX - n, "\r\n");
  if (n >= HDRMAX)
    {
      free(req);
      close(c->sd);
      err(c, "request too long");
      return -E2BIG;
    }

  if (c->verbose)
    {
      fprintf(stderr, "> %.*s", (int)n, req);
    }

  ret = send_all(c->sd, req, n);
  free(req);
  if (ret == 0 && with_body)
    {
      ret = send_all(c->sd, c->data, c->data_n);
    }

  if (ret < 0)
    {
      err(c, "send: %s", strerror(-ret));
      close(c->sd);
      return ret;
    }

  /* The status line and the headers */

  c->blen = c->boff = 0;
  c->length = -1;
  c->chunked = false;
  c->location_hdr[0] = '\0';
  ret = get_line(c, line, sizeof(line));
  if (ret < 0 || sscanf(line, "HTTP/%*d.%*d %d", &c->status) != 1)
    {
      err(c, "no HTTP response%s%s", ret < 0 ? ": " : "",
          ret < 0 ? strerror(-ret) : "");
      close(c->sd);
      return ret < 0 ? ret : -EPROTO;
    }

  if (c->include || c->head)
    {
      printf("%s\r\n", line);
    }

  if (c->verbose)
    {
      fprintf(stderr, "< %s\n", line);
    }

  for (; ; )
    {
      ret = get_line(c, line, sizeof(line));
      if (ret < 0)
        {
          close(c->sd);
          return ret;
        }

      if (c->include || c->head)
        {
          printf("%s\r\n", line);
        }

      if (c->verbose)
        {
          fprintf(stderr, "< %s\n", line);
        }

      if (ret == 0)
        {
          break;
        }

      if (strncasecmp(line, "Content-Length:", 15) == 0)
        {
          c->length = strtoll(line + 15, NULL, 10);
        }
      else if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 &&
               strcasestr(line + 18, "chunked") != NULL)
        {
          c->chunked = true;
        }
      else if (strncasecmp(line, "Location:", 9) == 0)
        {
          const char *v = line + 9;

          while (*v == ' ')
            {
              v++;
            }

          strlcpy(c->location_hdr, v, sizeof(c->location_hdr));
        }
    }

  return c->status;
}

static int body(struct curl_s *c, const char *method)
{
  int ret;

  if (strcmp(method, "HEAD") == 0 || c->status == 204 ||
      c->status == 304 || (c->status >= 100 && c->status < 200))
    {
      return 0;
    }

  c->got = 0;
  c->t0 = c->tprog = now_ms();
  ret = c->chunked ? read_chunked(c) : copy_body(c, c->length);
  progress(c, true);
  if (c->fout != NULL)
    {
      fflush(c->fout);
    }

  return ret;
}

static bool redirect(int status)
{
  return status == 301 || status == 302 || status == 303 ||
         status == 307 || status == 308;
}

static void usage(struct curl_s *c)
{
  if (c->wget)
    {
      fprintf(stderr, "usage: wget [-q] [-O FILE] [--no-check-certificate]"
                      " URL\n");
    }
  else
    {
      fprintf(stderr,
              "usage: curl [-sSvikIL] [-X METHOD] [-H 'Name: value']...\n"
              "            [-d DATA|@FILE] [-A AGENT] [-o FILE | -O]\n"
              "            [--max-redirs N] [-m SECONDS] URL\n");
    }
}

static int load_data(struct curl_s *c, const char *arg)
{
  if (arg[0] != '@')
    {
      c->data   = arg;
      c->data_n = strlen(arg);
      return 0;
    }

  FILE *f = fopen(arg + 1, "rb");
  long n;

  if (f == NULL)
    {
      err(c, "%s: %s", arg + 1, strerror(errno));
      return -errno;
    }

  fseek(f, 0, SEEK_END);
  n = ftell(f);
  fseek(f, 0, SEEK_SET);
  free(c->data_buf);
  c->data_buf = malloc(n > 0 ? n : 1);
  if (c->data_buf == NULL || fread(c->data_buf, 1, n, f) != (size_t)n)
    {
      fclose(f);
      return -ENOMEM;
    }

  fclose(f);
  c->data   = c->data_buf;
  c->data_n = n;
  return 0;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

#ifdef CURL_AS_WGET
#  define CURL_WGET true
#else
#  define CURL_WGET false
#endif

int main(int argc, FAR char *argv[])
{
  struct curl_s *c = calloc(1, sizeof(*c));
  struct url_s u;
  struct url_s nu;
  const char *url = NULL;
  const char *method;
  bool with_body;
  int redirs = 0;
  int ret = EXIT_FAILURE;
  int i;

  if (c == NULL)
    {
      return EXIT_FAILURE;
    }

  c->wget       = CURL_WGET;
  c->location   = c->wget;
  c->max_redirs = 20;
  c->timeout_s  = 30;
  c->sd         = -1;

  for (i = 1; i < argc; i++)
    {
      const char *a = argv[i];
      const char *v = i + 1 < argc ? argv[i + 1] : NULL;

      if (a[0] != '-' || a[1] == '\0')
        {
          url = a;
          continue;
        }

      if (strcmp(a, "--max-redirs") == 0 && v != NULL)
        {
          c->max_redirs = atoi(v);
          i++;
          continue;
        }

      if (strcmp(a, "--no-check-certificate") == 0 ||
          strcmp(a, "--insecure") == 0)
        {
          c->insecure = true;
          continue;
        }

      if (strcmp(a, "--silent") == 0 || strcmp(a, "--quiet") == 0)
        {
          c->silent = true;
          continue;
        }

      /* Single letters, maybe several together (-sL) */

      for (const char *f = a + 1; *f != '\0'; f++)
        {
          bool takes = strchr(c->wget ? "O" : "XHdAom", *f) != NULL;

          if (takes && f[1] == '\0' && v == NULL)
            {
              usage(c);
              goto out;
            }

          switch (*f)
            {
              case 's':
                c->silent = true;
                break;

              case 'q':
                c->silent = true;
                break;

              case 'S':
                c->show_err = true;
                break;

              case 'v':
                c->verbose = true;
                break;

              case 'i':
                c->include = true;
                break;

              case 'I':
                c->head = true;
                break;

              case 'k':
                c->insecure = true;
                break;

              case 'L':
                c->location = true;
                break;

              case 'X':
                c->method = v;
                i++;
                goto next;

              case 'H':
                if (c->nhdr < MAXHDRS)
                  {
                    c->hdr[c->nhdr++] = v;
                  }

                i++;
                goto next;

              case 'd':
                if (load_data(c, v) < 0)
                  {
                    goto out;
                  }

                i++;
                goto next;

              case 'A':
                c->agent = v;
                i++;
                goto next;

              case 'o':
                c->out = v;
                i++;
                goto next;

              case 'O':
                if (c->wget)
                  {
                    c->out = v;
                    i++;
                    goto next;
                  }

                c->remote_name = true;
                break;

              case 'm':
                c->timeout_s = atoi(v);
                i++;
                goto next;

              default:
                usage(c);
                goto out;
            }
        }

next:
      continue;
    }

  if (url == NULL)
    {
      usage(c);
      goto out;
    }

  ret = parse_url(url, NULL, &u);
  if (ret < 0)
    {
      err(c, ret == -EPROTONOSUPPORT ? "only http:// and https://" :
          "bad URL: %s", url);
      ret = EXIT_FAILURE;
      goto out;
    }

  /* Where the body goes: stdout, -o FILE, or the URL's last part (-O,
   * wget)
   */

  if (c->out == NULL && (c->remote_name || c->wget))
    {
      const char *b = strrchr(u.path, '/');

      b = b != NULL && b[1] != '\0' ? b + 1 : "index.html";
      c->out = strdup(b);
      if (c->out != NULL)
        {
          ((char *)c->out)[strcspn(c->out, "?")] = '\0';
        }
    }

  method = c->method != NULL ? c->method :
           c->head ? "HEAD" : c->data != NULL ? "POST" : "GET";
  with_body = c->data != NULL;

  for (; ; )
    {
      int status = request(c, &u, method, with_body);

      if (status < 0)
        {
          ret = EXIT_FAILURE;
          goto out;
        }

      if (c->location && redirect(status) && c->location_hdr[0] != '\0')
        {
          close(c->sd);
          c->sd = -1;
          if (++redirs > c->max_redirs)
            {
              err(c, "too many redirects");
              ret = EXIT_FAILURE;
              goto out;
            }

          if (parse_url(c->location_hdr, &u, &nu) < 0)
            {
              err(c, "bad redirect: %s", c->location_hdr);
              ret = EXIT_FAILURE;
              goto out;
            }

          if (!c->silent && c->wget)
            {
              fprintf(stderr, "Location: %s [following]\n",
                      c->location_hdr);
            }

          u = nu;
          if (status == 303 ||
              ((status == 301 || status == 302) &&
               strcmp(method, "POST") == 0))
            {
              method = "GET";
              with_body = false;
            }

          continue;
        }

      break;
    }

  if (c->wget && c->status >= 400)
    {
      err(c, "server said %d", c->status);
      ret = EXIT_FAILURE;
      goto out;
    }

  if (c->out != NULL && strcmp(c->out, "-") != 0 && !c->head)
    {
      c->fout = fopen(c->out, "wb");
      if (c->fout == NULL)
        {
          err(c, "%s: %s", c->out, strerror(errno));
          ret = EXIT_FAILURE;
          goto out;
        }

      if (c->wget && !c->silent)
        {
          fprintf(stderr, "Saving to: %s\n", c->out);
        }
    }
  else
    {
      c->fout = c->head ? NULL : stdout;
    }

  ret = body(c, method);
  if (ret < 0)
    {
      err(c, "transfer: %s", strerror(-ret));
      ret = EXIT_FAILURE;
    }
  else
    {
      ret = EXIT_SUCCESS;
    }

  fflush(stdout);

out:
  if (c->sd >= 0)
    {
      close(c->sd);
    }

  if (c->fout != NULL && c->fout != stdout)
    {
      fclose(c->fout);
    }

  free(c->data_buf);
  free(c);
  return ret;
}
