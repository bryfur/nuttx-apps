/****************************************************************************
 * apps/netutils/nctools/ssh.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * ssh: SSH sessions offloaded to the network coprocessor (docs/netcop.md
 * 5.8): it does the key exchange, the encryption, the host key check and
 * the authentication; here the session is a socket (netcopd's
 * NCX_PROTO_SSH) carrying the plain terminal stream.
 *
 *   ssh [-p PORT] [-i] [-y] [-t|-T] [-l USER] [USER@]HOST [COMMAND...]
 *   ssh -K [-r]           the coprocessor's public key (-r: a new one)
 *   ssh -k                the known hosts
 *   ssh -F HOST [PORT]    forget a known host
 *
 * Authentication: the coprocessor's key first; if the server refuses it,
 * a password is asked for (-i: the key only).  An unknown host key is
 * shown and asked about (yes saves it, once does not; -y: yes); a changed
 * one is refused unless accepted.  Interactive sessions put the terminal in
 * raw mode and follow its size (the desktop's Terminal tells its pty).
 * "~." at the start of a line ends the session.
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <netinet/in.h>
#include <sys/ioctl.h>
#include <sys/socket.h>

#include "netutils/netcop.h"

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct termios g_saved;
static bool g_raw;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void tty_raw(void)
{
  struct termios t;

  if (tcgetattr(STDIN_FILENO, &t) < 0)
    {
      return;
    }

  g_saved = t;
  t.c_iflag &= ~(ICRNL | INLCR | IGNCR | IXON);
  t.c_oflag &= ~OPOST;
  t.c_lflag &= ~(ECHO | ICANON | ISIG | IEXTEN);
  if (tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0)
    {
      g_raw = true;
    }
}

static void tty_restore(void)
{
  if (g_raw)
    {
      tcsetattr(STDIN_FILENO, TCSANOW, &g_saved);
      g_raw = false;
    }
}

static void win_size(uint16_t *cols, uint16_t *rows)
{
  struct winsize ws;

  *cols = 80;
  *rows = 24;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, (unsigned long)&ws) == 0 &&
      ws.ws_col > 0 && ws.ws_row > 0)
    {
      *cols = ws.ws_col;
      *rows = ws.ws_row;
    }
}

/* A line from the terminal, echoed or not */

static int ask(const char *prompt, char *buf, size_t cap, bool echo)
{
  struct termios t;
  struct termios o;
  bool set = false;
  size_t n = 0;
  char ch;

  fputs(prompt, stdout);
  fflush(stdout);
  if (!echo && tcgetattr(STDIN_FILENO, &o) == 0)
    {
      t = o;
      t.c_lflag &= ~ECHO;
      set = tcsetattr(STDIN_FILENO, TCSANOW, &t) == 0;
    }

  while (read(STDIN_FILENO, &ch, 1) == 1)
    {
      if (ch == '\r' || ch == '\n')
        {
          break;
        }

      if ((ch == 0x7f || ch == '\b') && n > 0)
        {
          n--;
          continue;
        }

      if (ch == 0x03)
        {
          n = 0;
          break;
        }

      if (n + 1 < cap)
        {
          buf[n++] = ch;
        }
    }

  buf[n] = '\0';
  if (set)
    {
      tcsetattr(STDIN_FILENO, TCSANOW, &o);
      fputs("\n", stdout);
    }

  return n;
}

static const char *ssh_err(int err)
{
  switch (err)
    {
      case EKEYREJECTED:
        return "host key rejected";
      case EACCES:
        return "permission denied";
      case ECONNREFUSED:
        return "connection refused";
      case ETIMEDOUT:
        return "timed out";
      case ENOENT:
        return "could not resolve the host name";
      case ENETDOWN:
        return "the network is down (no WiFi?)";
      case ENETUNREACH:
        return "no network (no coprocessor?)";
      case EPROTONOSUPPORT:
        return "the coprocessor does not do SSH";
      case EPROTO:
        return "SSH protocol error";
      default:
        return strerror(err);
    }
}

/* The server's key is unknown or changed: ask */

static int hostkey_decide(int sd, bool yes)
{
  struct nc_ssh_hostkey hk;
  uint8_t buf[sizeof(hk) + 64];
  socklen_t len = sizeof(buf);
  struct nc_rd rd;
  char fp[64];
  char ans[16];

  if (getsockopt(sd, SOL_NCX, NCX_SSH_HOSTKEY, buf, &len) < 0)
    {
      return NC_SSH_HK_REJECT;
    }

  nc_rd_init(&rd, buf, len);
  if (nc_dec_ssh_hostkey(&rd, &hk) != 0)
    {
      return NC_SSH_HK_REJECT;
    }

  nc_fp_str(hk.fp, fp, sizeof(fp));
  if (hk.status == NC_SSH_HK_CHANGED)
    {
      char old[64];

      nc_fp_str(hk.old_fp, old, sizeof(old));
      printf("@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
             "@    WARNING: REMOTE HOST IDENTIFICATION HAS CHANGED!     @\n"
             "@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@\n"
             "Someone could be eavesdropping on you right now, or the\n"
             "host key has just been changed.\n"
             "The %s key of %s is now\n  %s\nwas\n  %s\n", hk.key_type,
             hk.host, fp, old);
      if (yes)
        {
          printf("Refused (-y does not accept a changed key).\n");
          return NC_SSH_HK_REJECT;
        }

      ask("Accept the new key and save it (yes/no)? ", ans, sizeof(ans),
          true);
      return strcmp(ans, "yes") == 0 ? NC_SSH_HK_ACCEPT_SAVE :
             NC_SSH_HK_REJECT;
    }

  printf("The authenticity of host '%s' (port %u) can't be established.\n"
         "%s key fingerprint is %s.\n", hk.host, hk.port, hk.key_type, fp);
  if (yes)
    {
      printf("Warning: permanently added '%s' to the known hosts.\n",
             hk.host);
      return NC_SSH_HK_ACCEPT_SAVE;
    }

  for (; ; )
    {
      ask("Are you sure you want to continue connecting (yes/no/once)? ",
          ans, sizeof(ans), true);
      if (strcmp(ans, "yes") == 0)
        {
          return NC_SSH_HK_ACCEPT_SAVE;
        }

      if (strcmp(ans, "once") == 0)
        {
          return NC_SSH_HK_ACCEPT_ONCE;
        }

      if (strcmp(ans, "no") == 0 || ans[0] == '\0')
        {
          return NC_SSH_HK_REJECT;
        }
    }
}

/* Open a session: the socket, or -errno */

static int session(struct nc_ssh_open *o, bool yes)
{
  uint8_t *body = malloc(NC_MTU_MAX);
  struct sockaddr_in sa;
  struct nc_wr w;
  int sd;
  int ret;

  if (body == NULL)
    {
      return -ENOMEM;
    }

  sd = socket(AF_INET, SOCK_STREAM, NCX_PROTO_SSH);
  if (sd < 0)
    {
      ret = -errno;
      free(body);
      return ret == -EAFNOSUPPORT || ret == -ENETDOWN ? -ENETUNREACH : ret;
    }

  nc_wr_init(&w, body, NC_MTU_MAX);
  nc_enc_ssh_open(&w, o);
  ret = setsockopt(sd, SOL_NCX, NCX_SSH_OPEN, body, w.n) < 0 ? -errno : 0;
  free(body);

  /* connect() opens the session (the address is the body's) */

  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port   = htons(o->port);
  if (ret == 0)
    {
      ret = connect(sd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ?
            -errno : 0;
    }

  if (ret == -ENOKEY)
    {
      while (ret == -ENOKEY)
        {
          uint8_t b = (uint8_t)hostkey_decide(sd, yes);

          ret = setsockopt(sd, SOL_NCX, NCX_SSH_HOSTKEY, &b, 1) < 0 ?
                -errno : 0;
        }

      if (ret == 0)
        {
          ret = connect(sd, (struct sockaddr *)&sa, sizeof(sa)) < 0 ?
                -errno : 0;
        }
    }

  if (ret < 0)
    {
      close(sd);
      return ret;
    }

  return sd;
}

static int exit_status(int sd)
{
  struct nc_ssh_exit ex;
  uint8_t buf[64];
  socklen_t len = sizeof(buf);
  struct nc_rd rd;

  if (getsockopt(sd, SOL_NCX, NCX_SSH_EXIT, buf, &len) < 0)
    {
      return -1;
    }

  nc_rd_init(&rd, buf, len);
  if (nc_dec_ssh_exit(&rd, &ex) != 0)
    {
      return -1;
    }

  if (ex.status < 0 && ex.signal[0] != '\0')
    {
      fprintf(stderr, "ssh: remote killed by SIG%s\n", ex.signal);
    }

  return ex.status;
}

/* The session: the terminal both ways */

static int run(int sd, bool interactive)
{
  uint8_t buf[512];
  struct pollfd pfd[2];
  uint16_t cols;
  uint16_t rows;
  bool bol = true;
  bool tilde = false;
  bool in_open = true;
  int ret = 0;

  win_size(&cols, &rows);
  if (interactive)
    {
      tty_raw();
    }

  for (; ; )
    {
      ssize_t n;

      pfd[0].fd      = sd;
      pfd[0].events  = POLLIN;
      pfd[0].revents = 0;
      pfd[1].fd      = in_open ? STDIN_FILENO : -1;
      pfd[1].events  = POLLIN;
      pfd[1].revents = 0;
      if (poll(pfd, 2, interactive ? 500 : -1) < 0 && errno != EINTR)
        {
          ret = -errno;
          break;
        }

      if (interactive)
        {
          uint16_t c;
          uint16_t r;

          win_size(&c, &r);
          if (c != cols || r != rows)
            {
              uint8_t b[4] =
              {
                (uint8_t)c, (uint8_t)(c >> 8), (uint8_t)r, (uint8_t)(r >> 8)
              };

              cols = c;
              rows = r;
              setsockopt(sd, SOL_NCX, NCX_SSH_RESIZE, b, 4);
            }
        }

      if (pfd[0].revents & (POLLIN | POLLHUP | POLLERR))
        {
          n = recv(sd, buf, sizeof(buf), 0);
          if (n <= 0)
            {
              ret = n < 0 && errno != ECONNRESET ? -errno : 0;
              break;
            }

          write(STDOUT_FILENO, buf, n);
        }

      if (pfd[1].revents & (POLLIN | POLLHUP))
        {
          size_t i;
          size_t k = 0;

          n = read(STDIN_FILENO, buf, sizeof(buf));
          if (n <= 0)
            {
              /* The end of input: EOF to the remote side */

              shutdown(sd, SHUT_WR);
              in_open = false;
              continue;
            }

          /* "~." at the start of a line: the end */

          for (i = 0; i < (size_t)n; i++)
            {
              uint8_t ch = buf[i];

              if (interactive && tilde)
                {
                  tilde = false;
                  if (ch == '.')
                    {
                      tty_restore();
                      fprintf(stderr, "\nConnection closed.\n");
                      return 255;
                    }

                  if (ch != '~')
                    {
                      buf[k++] = '~';
                    }
                }
              else if (interactive && bol && ch == '~')
                {
                  tilde = true;
                  continue;
                }

              bol = ch == '\r' || ch == '\n';
              buf[k++] = ch;
            }

          for (i = 0; i < k; )
            {
              ssize_t s = send(sd, buf + i, k - i, 0);

              if (s < 0)
                {
                  break;
                }

              i += s;
            }
        }
    }

  tty_restore();
  if (ret < 0)
    {
      fprintf(stderr, "ssh: %s\n", ssh_err(-ret));
      return 255;
    }

  ret = exit_status(sd);
  return ret < 0 ? 255 : ret;
}

static int show_key(bool regen)
{
  struct ncx_rsp *rsp = malloc(sizeof(*rsp));
  struct nc_ssh_key k;
  struct nc_rd rd;
  uint8_t flags = regen ? NC_SSH_KEY_REGEN : 0;
  char fp[64];
  int ret;

  if (rsp == NULL)
    {
      return EXIT_FAILURE;
    }

  ret = ncx_call1(NC_OP_SSH_KEY, &flags, 1, rsp, NULL, NULL, 10000);
  nc_rd_init(&rd, rsp->body, rsp->n);
  if (ret < 0 || nc_dec_ssh_key(&rd, &k) != 0)
    {
      fprintf(stderr, "ssh: %s\n", ret == -ENOTCONN || ret == -ENETDOWN ?
              "no network coprocessor" : ssh_err(ret < 0 ? -ret : EPROTO));
      free(rsp);
      return EXIT_FAILURE;
    }

  printf("%s\n", k.line);
  printf("%s fingerprint %s\n", k.type, nc_fp_str(k.fp, fp, sizeof(fp)));
  printf("(add the line above to ~/.ssh/authorized_keys on the servers)\n");
  free(rsp);
  return EXIT_SUCCESS;
}

static void known_part(void *arg, const uint8_t *p, size_t n)
{
  struct nc_ssh_known k;
  struct nc_rd rd;
  char fp[64];
  int cnt;

  nc_rd_init(&rd, p, n);
  for (cnt = nc_rd_u8(&rd); cnt > 0; cnt--)
    {
      if (nc_dec_ssh_known(&rd, &k) != 0)
        {
          break;
        }

      printf("%s:%u %s %s\n", k.host, k.port, k.type,
             nc_fp_str(k.fp, fp, sizeof(fp)));
      (*(int *)arg)++;
    }
}

static void usage(void)
{
  fprintf(stderr,
          "usage: ssh [-p PORT] [-i] [-y] [-t|-T] [-l USER] [USER@]HOST "
          "[COMMAND...]\n"
          "       ssh -K [-r]          the coprocessor's public key\n"
          "       ssh -k               the known hosts\n"
          "       ssh -F HOST [PORT]   forget a known host\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  struct nc_ssh_open *o;
  const char *user = NULL;
  const char *dest = NULL;
  char prompt[NC_HOST_MAX + 80];
  bool key_only = false;
  bool yes = false;
  int pty = -1;
  int port = 22;
  int cmd0 = 0;
  int sd;
  int ret;
  int i;

  for (i = 1; i < argc && argv[i][0] == '-'; i++)
    {
      const char *a = argv[i];

      if (strcmp(a, "-K") == 0)
        {
          return show_key(i + 1 < argc && strcmp(argv[i + 1], "-r") == 0);
        }
      else if (strcmp(a, "-k") == 0)
        {
          int n = 0;

          ret = ncx_call1(NC_OP_SSH_KNOWN_LIST, NULL, 0, NULL, known_part,
                          &n, 5000);
          if (ret < 0)
            {
              fprintf(stderr, "ssh: %s\n", ssh_err(-ret));
              return EXIT_FAILURE;
            }

          if (n == 0)
            {
              printf("no known hosts\n");
            }

          return EXIT_SUCCESS;
        }
      else if (strcmp(a, "-F") == 0 && i + 1 < argc)
        {
          uint8_t b[NC_HOST_MAX + 4];
          struct nc_wr w;

          nc_wr_init(&w, b, sizeof(b));
          nc_wr_u16(&w, i + 2 < argc ? atoi(argv[i + 2]) : 0);
          nc_wr_str8(&w, argv[i + 1]);
          ret = ncx_call1(NC_OP_SSH_KNOWN_FORGET, b, w.n, NULL, NULL, NULL,
                          5000);
          if (ret < 0)
            {
              fprintf(stderr, "ssh: %s\n", ret == -ENOENT ? "not known" :
                      ssh_err(-ret));
              return EXIT_FAILURE;
            }

          return EXIT_SUCCESS;
        }
      else if (strcmp(a, "-p") == 0 && i + 1 < argc)
        {
          port = atoi(argv[++i]);
        }
      else if (strcmp(a, "-l") == 0 && i + 1 < argc)
        {
          user = argv[++i];
        }
      else if (strcmp(a, "-i") == 0)
        {
          key_only = true;
        }
      else if (strcmp(a, "-y") == 0)
        {
          yes = true;
        }
      else if (strcmp(a, "-t") == 0)
        {
          pty = 1;
        }
      else if (strcmp(a, "-T") == 0)
        {
          pty = 0;
        }
      else
        {
          usage();
          return EXIT_FAILURE;
        }
    }

  if (i >= argc)
    {
      usage();
      return EXIT_FAILURE;
    }

  dest = argv[i++];
  cmd0 = i;

  o = calloc(1, sizeof(*o));
  if (o == NULL)
    {
      return EXIT_FAILURE;
    }

  if (strchr(dest, '@') != NULL)
    {
      size_t n = strchr(dest, '@') - dest;

      snprintf(o->user, sizeof(o->user), "%.*s", (int)n, dest);
      strlcpy(o->host, dest + n + 1, sizeof(o->host));
    }
  else
    {
      strlcpy(o->host, dest, sizeof(o->host));
      strlcpy(o->user, user != NULL ? user :
              getenv("USER") != NULL ? getenv("USER") : "root",
              sizeof(o->user));
    }

  if (user != NULL)
    {
      strlcpy(o->user, user, sizeof(o->user));
    }

  for (i = cmd0; i < argc; i++)
    {
      if (i > cmd0)
        {
          strlcat(o->command, " ", sizeof(o->command));
        }

      strlcat(o->command, argv[i], sizeof(o->command));
    }

  if (pty < 0)
    {
      pty = o->command[0] == '\0' && isatty(STDIN_FILENO);
    }

  o->auth      = NC_SSH_AUTH_KEY;
  o->flags     = (pty ? NC_SSH_F_PTY : 0) |
                 (o->command[0] != '\0' ? NC_SSH_F_EXEC : 0);
  o->port      = port;
  o->timeout_s = 30;
  strlcpy(o->term, "xterm-256color", sizeof(o->term));
  win_size(&o->cols, &o->rows);

  sd = session(o, yes);
  if (sd == -EACCES && !key_only)
    {
      /* The server would not take the coprocessor's key: a password */

      snprintf(prompt, sizeof(prompt), "%s@%s's password: ", o->user,
               o->host);
      if (ask(prompt, o->password, sizeof(o->password), false) > 0)
        {
          o->auth = NC_SSH_AUTH_PASSWORD;
          sd = session(o, yes);
        }
    }

  memset(o->password, 0, sizeof(o->password));
  free(o);
  if (sd < 0)
    {
      fprintf(stderr, "ssh: %s port %d: %s\n", dest, port, ssh_err(-sd));
      return 255;
    }

  ret = run(sd, pty != 0);
  close(sd);
  return ret;
}
