/****************************************************************************
 * apps/interpreters/micropython/modagent.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The _agent module: the C half of the agent (docs/agent.md).  This file
 * has the module's table, the terminal (the line editor, keys, its size,
 * Esc as an interrupt) and text helpers (an incomplete JSON document
 * closed, HTML to text).  agent_http.c, agent_proc.c and agent_search.c
 * have the rest.
 *
 *   readline(prompt, history=None, cont="", completions=None, mask=False)
 *   getkey(wait_ms=-1)          a key's name ("a", "enter", "up", "esc",
 *                               "ctrl-c" ...), None when the time is up
 *   term_size()                 (columns, rows)
 *   isatty()
 *   interrupt_keys(on)          Esc interrupts too (KeyboardInterrupt)
 *   flush_input()               what was typed ahead, dropped
 *   home()                      $HOME, else CONFIG_LIBC_HOMEDIR
 *   json_repair(s)              s closed so that json.loads takes it
 *   html2text(s, max=65536)
 *   Http, Proc, grep, glob, fnmatch, netstate: the other files
 ****************************************************************************/

#include <nuttx/config.h>

#include <ctype.h>
#include <errno.h>
#include <poll.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "py/mphal.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/stream.h"

#include "modagent.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ESC_MS          40             /* The rest of a key's sequence */

/* Keys besides bytes */

#define K_UP            0x1001
#define K_DOWN          0x1002
#define K_RIGHT         0x1003
#define K_LEFT          0x1004
#define K_HOME          0x1005
#define K_END           0x1006
#define K_DEL           0x1007
#define K_PGUP          0x1008
#define K_PGDN          0x1009
#define K_ESC           0x100a
#define K_ALTENTER      0x100b
#define K_NONE          0x10ff         /* An unknown sequence */
#define K_ALT           0x2000         /* | the byte: Alt-x */
#define K_TIMEOUT       (-1)
#define K_EOF           (-2)

#define CTRL(c)         ((c) & 0x1f)

/****************************************************************************
 * Public Functions: helpers for the other files
 ****************************************************************************/

uint32_t agent_ms(void)
{
  struct timespec ts;

  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void agent_check_interrupt(void)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();

  if (agent_ms() - p->lastpoll >= 20)
    {
#ifdef CONFIG_ARCH_SIM
      usleep(1000);                    /* The simulator's idle thread */
#endif
      mp_nuttx_poll_input();
    }

  mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
}

int agent_wait_fd(int fd, short events, int ms)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();
  uint32_t t0 = agent_ms();
  struct pollfd pfd[2];

  for (; ; )
    {
      int slice = 100;
      int n = 1;
      int ret;

      mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
      if (ms >= 0)
        {
          int left = ms - (int)(agent_ms() - t0);

          if (left <= 0)
            {
              left = 0;
            }

          if (left < slice)
            {
              slice = left;
            }
        }

      pfd[0].fd      = fd;
      pfd[0].events  = events;
      pfd[0].revents = 0;
      if (!p->eof && p->interrupt_char >= 0)
        {
          pfd[1].fd      = STDIN_FILENO;
          pfd[1].events  = POLLIN;
          pfd[1].revents = 0;
          n = 2;
        }

      ret = poll(pfd, n, slice);
      if (ret < 0 && errno != EINTR)
        {
          return 0;
        }

      if (n == 2 && pfd[1].revents != 0)
        {
          mp_nuttx_poll_input();       /* Ctrl-C schedules the exception */
          mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
        }
      else
        {
          p->lastpoll = agent_ms();
        }

      if (ret > 0 && pfd[0].revents != 0)
        {
          return pfd[0].revents;
        }

      if (ms >= 0 && (int)(agent_ms() - t0) >= ms)
        {
          return 0;
        }
    }
}

NORETURN void agent_raise(int err, const char *fmt, ...)
{
  char msg[200];
  va_list ap;
  mp_obj_t args[2];

  va_start(ap, fmt);
  vsnprintf(msg, sizeof(msg), fmt, ap);
  va_end(ap);
  args[0] = MP_OBJ_NEW_SMALL_INT(err);
  args[1] = mp_obj_new_str_from_cstr(msg);
  nlr_raise(mp_obj_new_exception_args(&mp_type_OSError, 2, args));
}

/****************************************************************************
 * Private Functions: the terminal
 ****************************************************************************/

static void out(const char *s, size_t n)
{
  mp_hal_stdout_tx_strn(s, n);
}

/* A byte typed, or K_TIMEOUT after ms (-1: none), K_EOF */

static int rd_byte(int ms)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();
  uint32_t t0 = agent_ms();

  for (; ; )
    {
      int left = 20;

      if (p->count > 0)
        {
          int c = p->ring[p->head];

          p->head = (p->head + 1) % sizeof(p->ring);
          p->count--;
          return c;
        }

      if (p->eof)
        {
          return K_EOF;
        }

      if (ms >= 0)
        {
          left = ms - (int)(agent_ms() - t0);
          if (left <= 0)
            {
              return K_TIMEOUT;
            }

          if (left > 20)
            {
              left = 20;
            }
        }

      mp_nuttx_wfe(left);
      mp_handle_pending(MP_HANDLE_PENDING_CALLBACKS_AND_EXCEPTIONS);
    }
}

static bool input_pending(void)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();

  if (p->count == 0)
    {
      mp_nuttx_poll_input();
    }

  return p->count > 0;
}

/* A key: a byte, or K_* from an escape sequence */

static int rd_key(int ms)
{
  char par[16];
  int n = 0;
  int c = rd_byte(ms);
  int f;

  if (c != 0x1b)
    {
      return c;
    }

  c = rd_byte(ESC_MS);
  if (c == K_TIMEOUT || c == K_EOF)
    {
      return K_ESC;
    }

  if (c == '\r' || c == '\n')
    {
      return K_ALTENTER;
    }

  if (c != '[' && c != 'O')
    {
      return K_ALT | c;
    }

  /* CSI or SS3: parameters, then the final byte */

  for (; ; )
    {
      f = rd_byte(ESC_MS);
      if (f < 0)
        {
          return K_NONE;
        }

      if (f >= 0x40 && f <= 0x7e)
        {
          break;
        }

      if (n < (int)sizeof(par) - 1)
        {
          par[n++] = f;
        }
    }

  par[n] = '\0';
  switch (f)
    {
      case 'A':
        return K_UP;
      case 'B':
        return K_DOWN;
      case 'C':
        return K_RIGHT;
      case 'D':
        return K_LEFT;
      case 'H':
        return K_HOME;
      case 'F':
        return K_END;
      case '~':
        switch (atoi(par))
          {
            case 1:
            case 7:
              return K_HOME;
            case 4:
            case 8:
              return K_END;
            case 3:
              return K_DEL;
            case 5:
              return K_PGUP;
            case 6:
              return K_PGDN;
          }
        break;
    }

  return K_NONE;
}

static void term_size(int *cols, int *rows)
{
  struct winsize ws;

  *cols = 80;
  *rows = 24;
  if (ioctl(STDOUT_FILENO, TIOCGWINSZ, (unsigned long)(uintptr_t)&ws) == 0)
    {
      if (ws.ws_col >= 10)
        {
          *cols = ws.ws_col;
        }

      if (ws.ws_row >= 3)
        {
          *rows = ws.ws_row;
        }
    }
}

/****************************************************************************
 * Private Functions: the line editor
 *
 * The text is UTF-8, every character one column wide.  It is drawn from
 * the prompt on; a newline in it starts a row with the continuation
 * prompt.  Each change draws it again from its first row (the rows it
 * takes are known from the width), the cursor put where it belongs.  The
 * terminal wraps as xterm (and the desktop's terminal): a character in
 * the last column leaves the cursor there until the next.
 ****************************************************************************/

struct ed_s
{
  vstr_t      buf;
  size_t      cur;              /* Byte index of the cursor */
  const char *prompt;
  const char *cont;
  int         width;
  int         crow;             /* The cursor's row, from the first */
  bool        mask;
  mp_obj_t    history;          /* A list, or None */
  size_t      hidx;
  vstr_t      saved;            /* The line being typed, while browsing */
  bool        browsing;
};

static bool is_cont(uint8_t c)
{
  return (c & 0xc0) == 0x80;
}

/* Where a string of the display leaves (row, col) (escape sequences take
 * no room)
 */

static void lay(const char *s, size_t n, int w, int *row, int *col,
                bool mask)
{
  size_t i;

  for (i = 0; i < n; i++)
    {
      uint8_t c = s[i];

      if (c == 0x1b)
        {
          /* CSI ... final byte */

          i++;
          if (i < n && s[i] == '[')
            {
              while (i + 1 < n && !((uint8_t)s[i + 1] >= 0x40 &&
                                    (uint8_t)s[i + 1] <= 0x7e))
                {
                  i++;
                }

              i++;
            }

          continue;
        }

      if (c == '\n')
        {
          *row += 1;
          *col = 0;
          continue;
        }

      if (c < 0x20 || is_cont(c))
        {
          continue;
        }

      if (*col >= w)
        {
          *row += 1;
          *col = 0;
        }

      *col += 1;
    }
}

/* The position of the character at byte idx of the text */

static void ed_pos(struct ed_s *e, size_t idx, int *row, int *col)
{
  const char *t = e->buf.buf;
  size_t i;
  size_t start = 0;

  *row = 0;
  *col = 0;
  lay(e->prompt, strlen(e->prompt), e->width, row, col, false);
  for (i = 0; i <= idx && i <= e->buf.len; i++)
    {
      if (i == idx || i == e->buf.len || t[i] == '\n')
        {
          lay(t + start, i - start, e->width, row, col, e->mask);
          if (i == idx || i == e->buf.len)
            {
              break;
            }

          *row += 1;
          *col = 0;
          lay(e->cont, strlen(e->cont), e->width, row, col, false);
          start = i + 1;
        }
    }
}

static void ed_refresh(struct ed_s *e)
{
  vstr_t o;
  int re;
  int ce;
  int rc;
  int cc;
  int rows;
  size_t i;

  term_size(&e->width, &rows);
  vstr_init(&o, e->buf.len + 64);
  if (e->crow > 0)
    {
      vstr_printf(&o, "\x1b[%dA", e->crow);
    }

  vstr_add_str(&o, "\r\x1b[J");
  vstr_add_str(&o, e->prompt);
  for (i = 0; i < e->buf.len; i++)
    {
      char c = e->buf.buf[i];

      if (c == '\n')
        {
          vstr_add_str(&o, "\r\n");
          vstr_add_str(&o, e->cont);
        }
      else if (e->mask)
        {
          if (!is_cont(c))
            {
              vstr_add_byte(&o, '*');
            }
        }
      else if ((uint8_t)c >= 0x20 || c == '\t')
        {
          vstr_add_byte(&o, c == '\t' ? ' ' : c);
        }
    }

  ed_pos(e, e->buf.len, &re, &ce);
  if (ce >= e->width)
    {
      vstr_add_str(&o, "\r\n");
      re++;
      ce = 0;
    }

  ed_pos(e, e->cur, &rc, &cc);
  if (cc >= e->width)
    {
      rc++;
      cc = 0;
    }

  if (re > rc)
    {
      vstr_printf(&o, "\x1b[%dA", re - rc);
    }

  vstr_add_byte(&o, '\r');
  if (cc > 0)
    {
      vstr_printf(&o, "\x1b[%dC", cc);
    }

  out(o.buf, o.len);
  vstr_clear(&o);
  e->crow = rc;
}

static void ed_set(struct ed_s *e, const char *s, size_t n)
{
  vstr_reset(&e->buf);
  vstr_add_strn(&e->buf, s, n);
  e->cur = n;
}

static void ed_insert(struct ed_s *e, const char *s, size_t n)
{
  vstr_ins_strn(&e->buf, e->cur, s, n);
  e->cur += n;
}

static size_t ed_prev(struct ed_s *e, size_t i)
{
  if (i == 0)
    {
      return 0;
    }

  i--;
  while (i > 0 && is_cont(e->buf.buf[i]))
    {
      i--;
    }

  return i;
}

static size_t ed_next(struct ed_s *e, size_t i)
{
  if (i >= e->buf.len)
    {
      return e->buf.len;
    }

  i++;
  while (i < e->buf.len && is_cont(e->buf.buf[i]))
    {
      i++;
    }

  return i;
}

static bool is_word(char c)
{
  return c == '_' || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') ||
         (c >= 'A' && c <= 'Z') || (uint8_t)c >= 0x80;
}

static size_t word_left(struct ed_s *e, size_t i)
{
  while (i > 0 && !is_word(e->buf.buf[i - 1]))
    {
      i--;
    }

  while (i > 0 && is_word(e->buf.buf[i - 1]))
    {
      i--;
    }

  return i;
}

static size_t word_right(struct ed_s *e, size_t i)
{
  while (i < e->buf.len && !is_word(e->buf.buf[i]))
    {
      i++;
    }

  while (i < e->buf.len && is_word(e->buf.buf[i]))
    {
      i++;
    }

  return i;
}

static size_t line_start(struct ed_s *e, size_t i)
{
  while (i > 0 && e->buf.buf[i - 1] != '\n')
    {
      i--;
    }

  return i;
}

static size_t line_end(struct ed_s *e, size_t i)
{
  while (i < e->buf.len && e->buf.buf[i] != '\n')
    {
      i++;
    }

  return i;
}

static void ed_delete(struct ed_s *e, size_t from, size_t to)
{
  if (to > from)
    {
      vstr_cut_out_bytes(&e->buf, from, to - from);
      if (e->cur > to)
        {
          e->cur -= to - from;
        }
      else if (e->cur > from)
        {
          e->cur = from;
        }
    }
}

/* History: dir -1 older, +1 newer */

static void ed_history(struct ed_s *e, int dir)
{
  size_t len;
  mp_obj_t *items;
  size_t ni;

  if (e->history == mp_const_none)
    {
      return;
    }

  mp_obj_list_get(e->history, &len, &items);
  if (!e->browsing)
    {
      e->hidx = len;
    }

  if ((dir < 0 && e->hidx == 0) || (dir > 0 && e->hidx >= len))
    {
      return;
    }

  if (!e->browsing)
    {
      vstr_reset(&e->saved);
      vstr_add_strn(&e->saved, e->buf.buf, e->buf.len);
      e->browsing = true;
    }

  ni = e->hidx + dir;
  e->hidx = ni;
  if (ni >= len)
    {
      ed_set(e, e->saved.buf, e->saved.len);
      e->browsing = false;
    }
  else
    {
      size_t n;
      const char *s = mp_obj_str_get_data(items[ni], &n);

      ed_set(e, s, n);
    }
}

/* Tab: the text completed to what the completions starting with it have
 * in common
 */

static void ed_complete(struct ed_s *e, mp_obj_t completions)
{
  size_t len;
  mp_obj_t *items;
  const char *best = NULL;
  size_t bestn = 0;
  size_t i;

  mp_obj_list_get(completions, &len, &items);
  for (i = 0; i < len; i++)
    {
      size_t n;
      const char *s = mp_obj_str_get_data(items[i], &n);

      if (n < e->buf.len || memcmp(s, e->buf.buf, e->buf.len) != 0)
        {
          continue;
        }

      if (best == NULL)
        {
          best = s;
          bestn = n;
        }
      else
        {
          size_t k = 0;

          while (k < bestn && k < n && best[k] == s[k])
            {
              k++;
            }

          bestn = k;
        }
    }

  if (best != NULL && bestn > e->buf.len)
    {
      ed_set(e, best, bestn);
    }
}

/* The editor's loop; returns the text, or raises */

static mp_obj_t ed_run(struct ed_s *e, mp_obj_t completions)
{
  ed_refresh(e);
  for (; ; )
    {
      int k = rd_key(-1);
      bool redraw = true;

      if (k != K_EOF && k != K_ESC && k != '\r' && k != '\n')
        {
          e->browsing = e->browsing && (k == K_UP || k == K_DOWN);
        }

      switch (k)
        {
          case K_EOF:
            if (e->buf.len == 0)
              {
                out("\r\n", 2);
                mp_raise_type(&mp_type_EOFError);
              }

            k = '\r';

            /* Fall through: what was typed is the line */

          case '\r':
          case '\n':
            if (k != K_EOF && input_pending())
              {
                /* More came with it: a paste, whose lines are one text
                 * (a terminal without bracketed paste)
                 */

                ed_insert(e, "\n", 1);
                break;
              }

            if (e->buf.len > 0 && e->buf.buf[e->buf.len - 1] == '\\' &&
                e->cur == e->buf.len && k != K_EOF)
              {
                /* A backslash at the end: another line */

                e->buf.buf[e->buf.len - 1] = '\n';
                break;
              }

            e->cur = e->buf.len;
            ed_refresh(e);
            out("\r\n", 2);
            return mp_obj_new_str(e->buf.buf, e->buf.len);

          case K_ALTENTER:
          case CTRL('j') | K_ALT:
            ed_insert(e, "\n", 1);
            break;

          case CTRL('c'):
            if (e->buf.len == 0)
              {
                out("^C\r\n", 4);
                mp_raise_type(&mp_type_KeyboardInterrupt);
              }

            ed_set(e, "", 0);
            break;

          case CTRL('d'):
            if (e->buf.len == 0)
              {
                out("\r\n", 2);
                mp_raise_type(&mp_type_EOFError);
              }

            /* Fall through: delete the character under the cursor */

          case K_DEL:
            ed_delete(e, e->cur, ed_next(e, e->cur));
            break;

          case 0x7f:
          case CTRL('h'):
            ed_delete(e, ed_prev(e, e->cur), e->cur);
            break;

          case CTRL('a'):
          case K_HOME:
            e->cur = line_start(e, e->cur);
            break;

          case CTRL('e'):
          case K_END:
            e->cur = line_end(e, e->cur);
            break;

          case CTRL('b'):
          case K_LEFT:
            e->cur = ed_prev(e, e->cur);
            break;

          case CTRL('f'):
          case K_RIGHT:
            e->cur = ed_next(e, e->cur);
            break;

          case K_ALT | 'b':
            e->cur = word_left(e, e->cur);
            break;

          case K_ALT | 'f':
            e->cur = word_right(e, e->cur);
            break;

          case CTRL('k'):
            ed_delete(e, e->cur, line_end(e, e->cur));
            break;

          case CTRL('u'):
            ed_delete(e, line_start(e, e->cur), e->cur);
            break;

          case CTRL('w'):
          case K_ALT | 0x7f:
            ed_delete(e, word_left(e, e->cur), e->cur);
            break;

          case K_ALT | 'd':
            ed_delete(e, e->cur, word_right(e, e->cur));
            break;

          case CTRL('l'):
            out("\x1b[H\x1b[2J", 7);
            e->crow = 0;
            break;

          case CTRL('p'):
          case K_UP:
            {
              size_t ls = line_start(e, e->cur);

              if (ls > 0)
                {
                  /* Up a row of the text */

                  size_t col = e->cur - ls;
                  size_t ps = line_start(e, ls - 1);

                  e->cur = ps + col < ls - 1 ? ps + col : ls - 1;
                }
              else
                {
                  ed_history(e, -1);
                }
            }
            break;

          case CTRL('n'):
          case K_DOWN:
            {
              size_t le = line_end(e, e->cur);

              if (le < e->buf.len)
                {
                  size_t col = e->cur - line_start(e, e->cur);
                  size_t ne = line_end(e, le + 1);

                  e->cur = le + 1 + col < ne ? le + 1 + col : ne;
                }
              else
                {
                  ed_history(e, 1);
                }
            }
            break;

          case '\t':
            if (completions != mp_const_none && e->buf.len > 0 &&
                e->cur == e->buf.len)
              {
                ed_complete(e, completions);
              }
            else
              {
                ed_insert(e, "    ", 4);
              }
            break;

          case K_ESC:
          case K_NONE:
          case K_TIMEOUT:
            redraw = false;
            break;

          default:
            if (k >= 0x20 && k < 0x100)
              {
                char c = k;

                ed_insert(e, &c, 1);
              }
            else
              {
                redraw = false;
              }
            break;
        }

      /* Drawn when nothing more is waiting (a paste: drawn once) */

      if (redraw && !input_pending())
        {
          ed_refresh(e);
        }
    }
}

/****************************************************************************
 * Private Functions: the module's functions
 ****************************************************************************/

/* readline(prompt, history=None, cont="", completions=None, mask=False) */

static mp_obj_t agent_readline(size_t n_args, const mp_obj_t *pos,
                               mp_map_t *kw)
{
  enum
  {
    A_PROMPT, A_HISTORY, A_CONT, A_COMPLETIONS, A_MASK, A_TEXT
  };

  static const mp_arg_t allowed[] =
  {
    { MP_QSTR_prompt,      MP_ARG_OBJ, {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_)} },
    { MP_QSTR_history,     MP_ARG_OBJ, {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_cont,        MP_ARG_KW_ONLY | MP_ARG_OBJ,
                           {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_)} },
    { MP_QSTR_completions, MP_ARG_KW_ONLY | MP_ARG_OBJ,
                           {.u_rom_obj = MP_ROM_NONE} },
    { MP_QSTR_mask,        MP_ARG_KW_ONLY | MP_ARG_BOOL, {.u_bool = false} },
    { MP_QSTR_text,        MP_ARG_KW_ONLY | MP_ARG_OBJ,
                           {.u_rom_obj = MP_ROM_QSTR(MP_QSTR_)} },
  };

  struct mp_nuttx_port_s *p = mp_nuttx_port();
  mp_arg_val_t a[MP_ARRAY_SIZE(allowed)];
  struct ed_s e;
  nlr_buf_t nlr;
  mp_obj_t ret;
  int ic = p->interrupt_char;
  int ie = p->interrupt_esc;
  size_t n;
  const char *text;
  int rows;

  mp_arg_parse_all(n_args, pos, kw, MP_ARRAY_SIZE(allowed), allowed, a);
  memset(&e, 0, sizeof(e));
  e.prompt  = mp_obj_str_get_str(a[A_PROMPT].u_obj);
  e.cont    = mp_obj_str_get_str(a[A_CONT].u_obj);
  e.history = a[A_HISTORY].u_obj;
  e.mask    = a[A_MASK].u_bool;
  term_size(&e.width, &rows);
  if (e.history != mp_const_none &&
      !mp_obj_is_type(e.history, &mp_type_list))
    {
      mp_raise_TypeError(MP_ERROR_TEXT("history: a list"));
    }

  if (a[A_COMPLETIONS].u_obj != mp_const_none &&
      !mp_obj_is_type(a[A_COMPLETIONS].u_obj, &mp_type_list))
    {
      mp_raise_TypeError(MP_ERROR_TEXT("completions: a list"));
    }

  text = mp_obj_str_get_data(a[A_TEXT].u_obj, &n);
  vstr_init(&e.buf, 64 + n);
  vstr_init(&e.saved, 16);
  ed_set(&e, text, n);

  /* Ctrl-C is a key while editing */

  p->interrupt_char = -1;
  p->interrupt_esc  = -1;
  if (nlr_push(&nlr) == 0)
    {
      ret = ed_run(&e, a[A_COMPLETIONS].u_obj);
      nlr_pop();
    }
  else
    {
      p->interrupt_char = ic;
      p->interrupt_esc  = ie;
      nlr_jump(nlr.ret_val);
    }

  p->interrupt_char = ic;
  p->interrupt_esc  = ie;
  vstr_clear(&e.buf);
  vstr_clear(&e.saved);
  return ret;
}

static MP_DEFINE_CONST_FUN_OBJ_KW(agent_readline_obj, 0, agent_readline);

static const char *key_name(int k, char *tmp)
{
  static const char *const names[] =
  {
    "up", "down", "right", "left", "home", "end", "delete", "pgup",
    "pgdn", "esc", "alt-enter"
  };

  if (k >= K_UP && k <= K_ALTENTER)
    {
      return names[k - K_UP];
    }

  switch (k)
    {
      case '\r':
      case '\n':
        return "enter";
      case '\t':
        return "tab";
      case 0x7f:
      case CTRL('h'):
        return "backspace";
      case K_EOF:
        return "eof";
    }

  if (k & K_ALT)
    {
      snprintf(tmp, 8, "alt-%c", (char)(k & 0xff));
      return tmp;
    }

  if (k >= 0 && k < 0x20)
    {
      snprintf(tmp, 8, "ctrl-%c", (char)(k + 'a' - 1));
      return tmp;
    }

  return "unknown";
}

/* getkey(wait_ms=-1): a key's name, or None */

static mp_obj_t agent_getkey(size_t n_args, const mp_obj_t *args)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();
  int ms = n_args > 0 && args[0] != mp_const_none ?
           mp_obj_get_int(args[0]) : -1;
  int ic = p->interrupt_char;
  int ie = p->interrupt_esc;
  nlr_buf_t nlr;
  char tmp[8];
  char u[4];
  int k;
  int n;
  int i;

  p->interrupt_char = -1;
  p->interrupt_esc  = -1;
  if (nlr_push(&nlr) == 0)
    {
      k = rd_key(ms);
      nlr_pop();
    }
  else
    {
      p->interrupt_char = ic;
      p->interrupt_esc  = ie;
      nlr_jump(nlr.ret_val);
    }

  p->interrupt_char = ic;
  p->interrupt_esc  = ie;
  if (k == K_TIMEOUT)
    {
      return mp_const_none;
    }

  if (k >= 0x20 && k < 0x7f)
    {
      u[0] = k;
      return mp_obj_new_str(u, 1);
    }

  if (k >= 0xc0 && k < 0x100)
    {
      /* A UTF-8 character: its other bytes */

      n = k >= 0xf0 ? 4 : k >= 0xe0 ? 3 : 2;
      u[0] = k;
      for (i = 1; i < n; i++)
        {
          int c = rd_byte(ESC_MS);

          if (c < 0)
            {
              break;
            }

          u[i] = c;
        }

      return mp_obj_new_str(u, i);
    }

  return mp_obj_new_str_from_cstr(key_name(k, tmp));
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(agent_getkey_obj, 0, 1,
                                           agent_getkey);

static mp_obj_t agent_term_size(void)
{
  int cols;
  int rows;
  mp_obj_t t[2];

  term_size(&cols, &rows);
  t[0] = MP_OBJ_NEW_SMALL_INT(cols);
  t[1] = MP_OBJ_NEW_SMALL_INT(rows);
  return mp_obj_new_tuple(2, t);
}

static MP_DEFINE_CONST_FUN_OBJ_0(agent_term_size_obj, agent_term_size);

static mp_obj_t agent_isatty(void)
{
  return mp_obj_new_bool(isatty(STDIN_FILENO) && isatty(STDOUT_FILENO));
}

static MP_DEFINE_CONST_FUN_OBJ_0(agent_isatty_obj, agent_isatty);

static mp_obj_t agent_interrupt_keys(mp_obj_t on)
{
  mp_nuttx_port()->interrupt_esc = mp_obj_is_true(on) ? 1 : -1;
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_1(agent_interrupt_keys_obj,
                                 agent_interrupt_keys);

/* The home directory: $HOME, else NuttX's (CONFIG_LIBC_HOMEDIR) */

static mp_obj_t agent_home(void)
{
  const char *home = getenv("HOME");

#ifdef CONFIG_LIBC_HOMEDIR
  if (home == NULL || home[0] == '\0')
    {
      home = CONFIG_LIBC_HOMEDIR;
    }
#endif

  return mp_obj_new_str_from_cstr(home != NULL ? home : "/");
}

static MP_DEFINE_CONST_FUN_OBJ_0(agent_home_obj, agent_home);

/* The input typed ahead, dropped (after an interrupt) */

static mp_obj_t agent_flush_input(void)
{
  struct mp_nuttx_port_s *p = mp_nuttx_port();

  mp_nuttx_poll_input();
  p->count = 0;
  return mp_const_none;
}

static MP_DEFINE_CONST_FUN_OBJ_0(agent_flush_input_obj, agent_flush_input);

/****************************************************************************
 * Private Functions: JSON closed
 *
 * A JSON document cut short (a tool's input as it streams) made into one
 * that parses: an open string closed, a dangling key given null, a
 * half-written number or literal dropped, containers closed.
 ****************************************************************************/

static mp_obj_t agent_json_repair(mp_obj_t s_in)
{
  size_t n;
  const char *s = mp_obj_str_get_data(s_in, &n);
  char stack[64];
  uint8_t ostate[64];                /* Objects: 0 key, 1 colon, 2 value,
                                      * 3 after a value */
  int depth = 0;
  bool instr = false;
  bool esc = false;
  size_t keep = 0;                   /* Bytes of s kept */
  size_t strstart = 0;
  vstr_t o;
  size_t i;

  for (i = 0; i < n; i++)
    {
      char c = s[i];

      if (instr)
        {
          if (esc)
            {
              esc = false;
            }
          else if (c == '\\')
            {
              esc = true;
            }
          else if (c == '"')
            {
              instr = false;
              if (depth > 0 && stack[depth - 1] == '{')
                {
                  ostate[depth - 1] = ostate[depth - 1] == 0 ? 1 : 3;
                }

              keep = i + 1;
            }

          continue;
        }

      switch (c)
        {
          case '"':
            instr = true;
            strstart = i;
            break;

          case '{':
          case '[':
            if (depth < (int)sizeof(stack))
              {
                stack[depth] = c;
                ostate[depth] = 0;
                depth++;
              }

            keep = i + 1;
            break;

          case '}':
          case ']':
            if (depth > 0)
              {
                depth--;
                if (depth > 0 && stack[depth - 1] == '{')
                  {
                    ostate[depth - 1] = 3;
                  }
              }

            keep = i + 1;
            break;

          case ':':
            if (depth > 0 && stack[depth - 1] == '{')
              {
                ostate[depth - 1] = 2;
              }

            keep = i + 1;
            break;

          case ',':
            if (depth > 0 && stack[depth - 1] == '{')
              {
                ostate[depth - 1] = 0;
              }

            keep = i;                  /* Not the comma: nothing follows */
            break;

          case ' ':
          case '\t':
          case '\r':
          case '\n':
            break;

          default:
            {
              /* A number or a literal: kept if it is complete (what ends
               * it follows, or it is a whole literal)
               */

              size_t j = i;

              while (j < n && strchr(",]} \t\r\n", s[j]) == NULL)
                {
                  j++;
                }

              if (j < n || (j - i == 4 && (memcmp(s + i, "true", 4) == 0 ||
                                           memcmp(s + i, "null", 4) == 0))
                  || (j - i == 5 && memcmp(s + i, "false", 5) == 0))
                {
                  keep = j;
                  if (depth > 0 && stack[depth - 1] == '{')
                    {
                      ostate[depth - 1] = 3;
                    }
                }

              i = j - 1;
            }
            break;
        }
    }

  vstr_init(&o, n + depth + 8);
  if (instr)
    {
      /* An open string: closed, without a half escape */

      size_t e = n;
      size_t k = strstart + 1;
      bool bs = false;
      size_t last = k;

      for (; k < n; k++)
        {
          if (bs)
            {
              bs = false;
              if (s[k] == 'u' && k + 4 >= n)
                {
                  break;              /* \uXX cut short */
                }

              last = k + 1;
            }
          else if (s[k] == '\\')
            {
              bs = true;
            }
          else
            {
              last = k + 1;
            }
        }

      e = last;
      vstr_add_strn(&o, s, e);
      vstr_add_byte(&o, '"');
      if (depth > 0 && stack[depth - 1] == '{')
        {
          ostate[depth - 1] = ostate[depth - 1] == 0 ? 1 : 3;
        }
    }
  else
    {
      vstr_add_strn(&o, s, keep);
    }

  while (depth > 0)
    {
      depth--;
      if (stack[depth] == '{')
        {
          if (ostate[depth] == 1)
            {
              vstr_add_str(&o, ":null");
            }
          else if (ostate[depth] == 2)
            {
              vstr_add_str(&o, "null");
            }

          vstr_add_byte(&o, '}');
        }
      else
        {
          vstr_add_byte(&o, ']');
        }
    }

  return mp_obj_new_str_from_vstr(&o);
}

static MP_DEFINE_CONST_FUN_OBJ_1(agent_json_repair_obj, agent_json_repair);

/****************************************************************************
 * Private Functions: HTML to text
 ****************************************************************************/

struct h2t_s
{
  vstr_t o;
  size_t max;
  int    nl;                    /* Newlines at the end of o */
  bool   space;                 /* A space is due */
  int    pre;
  char   href[256];
};

static void h2t_put(struct h2t_s *h, const char *s, size_t n)
{
  if (h->o.len >= h->max)
    {
      return;
    }

  if (h->space && h->nl == 0 && h->o.len > 0)
    {
      vstr_add_byte(&h->o, ' ');
    }

  h->space = false;
  if (h->o.len + n > h->max)
    {
      n = h->max - h->o.len;
    }

  vstr_add_strn(&h->o, s, n);
  h->nl = 0;
}

static void h2t_nl(struct h2t_s *h, int want)
{
  h->space = false;
  if (h->o.len == 0)
    {
      return;
    }

  while (h->nl < want && h->o.len < h->max)
    {
      vstr_add_byte(&h->o, '\n');
      h->nl++;
    }
}

static const struct
{
  const char *name;
  const char *text;
} g_entities[] =
{
  { "amp", "&" }, { "lt", "<" }, { "gt", ">" }, { "quot", "\"" },
  { "apos", "'" }, { "nbsp", " " }, { "mdash", "--" }, { "ndash", "-" },
  { "hellip", "..." }, { "copy", "(c)" }, { "reg", "(R)" },
  { "rsquo", "'" }, { "lsquo", "'" }, { "rdquo", "\"" }, { "ldquo", "\"" },
  { "middot", "*" }, { "bull", "*" }, { "times", "x" }, { "laquo", "<<" },
  { "raquo", ">>" }, { "trade", "(TM)" }, { "deg", " deg" },
};

/* An entity at s (after the &): its text into buf, its length in *used */

static size_t h2t_entity(const char *s, size_t n, size_t *used, char *buf)
{
  size_t k = 0;
  size_t i;

  while (k < n && k < 10 && s[k] != ';')
    {
      k++;
    }

  if (k >= n || s[k] != ';' || k == 0)
    {
      return 0;
    }

  *used = k + 1;
  if (s[0] == '#')
    {
      unsigned long v = (s[1] == 'x' || s[1] == 'X') ?
                        strtoul(s + 2, NULL, 16) : strtoul(s + 1, NULL, 10);
      vstr_t t;
      size_t len;

      if (v == 0 || v > 0x10ffff)
        {
          return 0;
        }

      vstr_init_fixed_buf(&t, 5, buf);
      vstr_add_char(&t, v == 160 ? ' ' : v);
      len = t.len;
      return len;
    }

  for (i = 0; i < MP_ARRAY_SIZE(g_entities); i++)
    {
      if (strlen(g_entities[i].name) == k &&
          memcmp(g_entities[i].name, s, k) == 0)
        {
          strcpy(buf, g_entities[i].text);
          return strlen(buf);
        }
    }

  return 0;
}

static bool tag_is(const char *t, size_t n, const char *name)
{
  size_t k = strlen(name);

  return n == k && strncasecmp(t, name, k) == 0;
}

static mp_obj_t agent_html2text(size_t n_args, const mp_obj_t *args)
{
  static const char *const blocks[] =
  {
    "p", "div", "br", "tr", "table", "section", "article", "header",
    "footer", "ul", "ol", "dl", "dt", "dd", "blockquote", "hr", "main",
    "nav", "aside", "form", "figure", "figcaption", "title", NULL
  };

  size_t n;
  const char *s = mp_obj_str_get_data(args[0], &n);
  struct h2t_s h;
  size_t i = 0;
  int skip = 0;                   /* In script, style ... */
  bool title = false;

  memset(&h, 0, sizeof(h));
  h.max = n_args > 1 ? mp_obj_get_int(args[1]) : 65536;
  vstr_init(&h.o, n / 3 + 16);
  while (i < n && h.o.len < h.max)
    {
      char c = s[i];

      if (c == '<')
        {
          size_t j = i + 1;
          bool close = false;
          const char *t;
          size_t tn;

          if (n - i >= 4 && memcmp(s + i, "<!--", 4) == 0)
            {
              const char *e = strstr(s + i + 4, "-->");

              i = e != NULL ? (size_t)(e - s) + 3 : n;
              continue;
            }

          if (j < n && s[j] == '/')
            {
              close = true;
              j++;
            }

          t = s + j;
          while (j < n && (isalnum((unsigned char)s[j]) || s[j] == '!'))
            {
              j++;
            }

          tn = s + j - t;

          /* The attributes, to the end of the tag: an a's href kept */

          {
            size_t a = j;
            bool q1 = false;
            bool q2 = false;

            while (j < n && (q1 || q2 || s[j] != '>'))
              {
                if (s[j] == '"' && !q1)
                  {
                    q2 = !q2;
                  }
                else if (s[j] == '\'' && !q2)
                  {
                    q1 = !q1;
                  }

                j++;
              }

            if (!close && tag_is(t, tn, "a"))
              {
                const char *hr = NULL;
                size_t k;

                h.href[0] = '\0';
                for (k = a; k + 5 < j; k++)
                  {
                    if (strncasecmp(s + k, "href=", 5) == 0)
                      {
                        hr = s + k + 5;
                        break;
                      }
                  }

                if (hr != NULL)
                  {
                    char q = *hr == '"' || *hr == '\'' ? *hr++ : 0;
                    size_t m = 0;

                    while (hr < s + j && m < sizeof(h.href) - 1 &&
                           (q ? *hr != q : (*hr != ' ' && *hr != '>')))
                      {
                        h.href[m++] = *hr++;
                      }

                    h.href[m] = '\0';
                    if (strncmp(h.href, "http", 4) != 0 &&
                        h.href[0] != '/')
                      {
                        h.href[0] = '\0';
                      }
                  }
              }
          }

          i = j < n ? j + 1 : n;
          if (tag_is(t, tn, "script") || tag_is(t, tn, "style") ||
              tag_is(t, tn, "noscript") || tag_is(t, tn, "svg") ||
              tag_is(t, tn, "template"))
            {
              skip += close ? (skip > 0 ? -1 : 0) : 1;
              continue;
            }

          if (skip > 0)
            {
              continue;
            }

          if (tag_is(t, tn, "title"))
            {
              title = !close;
              if (title)
                {
                  h2t_put(&h, "Title:", 6);
                  h.space = true;
                }
            }

          if (tag_is(t, tn, "pre"))
            {
              h.pre += close ? (h.pre > 0 ? -1 : 0) : 1;
              h2t_nl(&h, 2);
              continue;
            }

          if (tn == 2 && (t[0] == 'h' || t[0] == 'H') && t[1] >= '1' &&
              t[1] <= '6')
            {
              h2t_nl(&h, 2);
              if (!close)
                {
                  static const char hashes[] = "######";

                  h2t_put(&h, hashes, t[1] - '0');
                  h.space = true;
                }

              continue;
            }

          if (tag_is(t, tn, "li"))
            {
              if (!close)
                {
                  h2t_nl(&h, 1);
                  h2t_put(&h, "-", 1);
                  h.space = true;
                }

              continue;
            }

          if (tag_is(t, tn, "td") || tag_is(t, tn, "th"))
            {
              if (!close)
                {
                  h2t_put(&h, " |", h.nl > 0 ? 1 : 2);
                  h.space = true;
                }

              continue;
            }

          if (tag_is(t, tn, "a") && close && h.href[0] != '\0')
            {
              h2t_put(&h, " (", 2);
              h2t_put(&h, h.href, strlen(h.href));
              h2t_put(&h, ")", 1);
              h.href[0] = '\0';
              continue;
            }

          for (int b = 0; blocks[b] != NULL; b++)
            {
              if (tag_is(t, tn, blocks[b]))
                {
                  h2t_nl(&h, tag_is(t, tn, "br") || tag_is(t, tn, "tr") ||
                         tag_is(t, tn, "title") ? 1 : 2);
                  break;
                }
            }

          continue;
        }

      if (skip > 0)
        {
          i++;
          continue;
        }

      if (c == '&')
        {
          char buf[8];
          size_t used;
          size_t k = h2t_entity(s + i + 1, n - i - 1, &used, buf);

          if (k > 0)
            {
              if (buf[0] == ' ' && k == 1 && !h.pre)
                {
                  h.space = true;
                }
              else
                {
                  h2t_put(&h, buf, k);
                }

              i += used + 1;
              continue;
            }
        }

      if (h.pre)
        {
          if (c == '\n')
            {
              vstr_add_byte(&h.o, '\n');
              h.nl++;
            }
          else if (c != '\r')
            {
              h.space = false;
              h2t_put(&h, &c, 1);
            }

          i++;
          continue;
        }

      if (c == ' ' || c == '\t' || c == '\n' || c == '\r')
        {
          h.space = true;
          i++;
          continue;
        }

      /* A run of text at once */

      {
        size_t j = i;

        while (j < n && s[j] != '<' && s[j] != '&' && s[j] != ' ' &&
               s[j] != '\t' && s[j] != '\n' && s[j] != '\r')
          {
            j++;
          }

        h2t_put(&h, s + i, j - i);
        i = j;
      }
    }

  /* Trailing newlines off */

  while (h.o.len > 0 && h.o.buf[h.o.len - 1] == '\n')
    {
      h.o.len--;
    }

  /* Cut at a character's start */

  while (h.o.len > 0 && h.o.len < n &&
         is_cont(h.o.buf[h.o.len - 1]) && h.o.len >= h.max)
    {
      h.o.len--;
    }

  return mp_obj_new_str_from_vstr(&h.o);
}

static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(agent_html2text_obj, 1, 2,
                                           agent_html2text);

/****************************************************************************
 * The module
 ****************************************************************************/

static const mp_rom_map_elem_t agent_globals[] =
{
  { MP_ROM_QSTR(MP_QSTR___name__),       MP_ROM_QSTR(MP_QSTR__agent) },
  { MP_ROM_QSTR(MP_QSTR_Http),           MP_ROM_PTR(&agent_http_type) },
  { MP_ROM_QSTR(MP_QSTR_Proc),           MP_ROM_PTR(&agent_proc_type) },
  { MP_ROM_QSTR(MP_QSTR_grep),           MP_ROM_PTR(&agent_grep_obj) },
  { MP_ROM_QSTR(MP_QSTR_glob),           MP_ROM_PTR(&agent_glob_obj) },
  { MP_ROM_QSTR(MP_QSTR_fnmatch),        MP_ROM_PTR(&agent_fnmatch_obj) },
  { MP_ROM_QSTR(MP_QSTR_netstate),       MP_ROM_PTR(&agent_netstate_obj) },
  { MP_ROM_QSTR(MP_QSTR_readline),       MP_ROM_PTR(&agent_readline_obj) },
  { MP_ROM_QSTR(MP_QSTR_getkey),         MP_ROM_PTR(&agent_getkey_obj) },
  { MP_ROM_QSTR(MP_QSTR_term_size),      MP_ROM_PTR(&agent_term_size_obj) },
  { MP_ROM_QSTR(MP_QSTR_isatty),         MP_ROM_PTR(&agent_isatty_obj) },
  { MP_ROM_QSTR(MP_QSTR_interrupt_keys),
    MP_ROM_PTR(&agent_interrupt_keys_obj) },
  { MP_ROM_QSTR(MP_QSTR_home),           MP_ROM_PTR(&agent_home_obj) },
  { MP_ROM_QSTR(MP_QSTR_flush_input),    MP_ROM_PTR(&agent_flush_input_obj) },
  { MP_ROM_QSTR(MP_QSTR_json_repair),    MP_ROM_PTR(&agent_json_repair_obj) },
  { MP_ROM_QSTR(MP_QSTR_html2text),      MP_ROM_PTR(&agent_html2text_obj) },
};

static MP_DEFINE_CONST_DICT(agent_globals_dict, agent_globals);

const mp_obj_module_t mp_module_agent =
{
  .base    = { &mp_type_module },
  .globals = (mp_obj_dict_t *)&agent_globals_dict,
};

MP_REGISTER_MODULE(MP_QSTR__agent, mp_module_agent);
