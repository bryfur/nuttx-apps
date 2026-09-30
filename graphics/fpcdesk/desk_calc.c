/****************************************************************************
 * apps/graphics/fpcdesk/desk_calc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Calculator: a display (the expression so far above, the number below)
 * and a keypad of round buttons, in the style of GNOME's and macOS's.
 * + - x / with the usual precedence (2 + 3 x 4 = 14), %, the sign, the
 * decimal point, clear (C clears the entry, AC everything), backspace;
 * = again repeats the last operation.  Numbers are doubles shown with at
 * most 15 significant digits (0.1 + 0.2 is 0.3), grouped in thousands,
 * in a smaller font when they do not fit; errors are said ("Cannot divide
 * by zero").
 *
 * Keys: digits, . and ,, + - * x /, %, = or Enter, Backspace, Escape (all
 * clear), Delete (the entry), n (the sign), Ctrl-C / Ctrl-V (the number,
 * through the desktop's clipboard).  A key lights its button for a moment
 * (the desktop's frames, while it is lit).
 *
 * Drawing: everything the window shows remembers what it last drew (each
 * button its look and label, the display its texts), and only what
 * changed is drawn again.
 ****************************************************************************/

#include <nuttx/config.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk.h"
#include "desk_ui.h"
#include "desk_num.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define CALC_W          320
#define CALC_H          464
#define CALC_PAD        14
#define CALC_GAP        10
#define CALC_DISP_H     112             /* The display, above the keypad */
#define CALC_ROWS       5
#define CALC_COLS       4
#define CALC_NKEYS      (CALC_ROWS * CALC_COLS)
#define CALC_DIGITS     16              /* The most digits of an entry */
#define CALC_FLASH_MS   110             /* A key's button lights this long */

#define C_KEY_DIGIT     RGB(0x3a, 0x3a, 0x40)
#define C_KEY_FUNC      RGB(0x5a, 0x5a, 0x62)
#define C_KEY_OP        RGB(0xff, 0x9f, 0x0a)
#define C_KEY_OPTEXT    RGB(0xff, 0x9f, 0x0a)
#define C_CALC_ERROR    RGB(0xff, 0x7b, 0x72)

/* The operations */

#define OP_NONE         0
#define OP_ADD          1
#define OP_SUB          2
#define OP_MUL          3
#define OP_DIV          4

/* The keys */

enum calc_key_e
{
  K_0 = 0, K_1, K_2, K_3, K_4, K_5, K_6, K_7, K_8, K_9,
  K_POINT, K_ADD, K_SUB, K_MUL, K_DIV, K_EQ, K_PCT, K_SIGN, K_CLEAR,
  K_BACK, K_CE, K_ALLCLEAR
};

/* Button kinds: their colours */

#define KIND_DIGIT      0
#define KIND_FUNC       1
#define KIND_OP         2

/* In a label: the division sign, drawn (the fonts have none) */

#define CH_DIVIDE       '\x01'
#define S_DIVIDE        "\x01"

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A key of the pad, row by row */

struct calc_keydef_s
{
  uint8_t key;                         /* enum calc_key_e */
  uint8_t kind;                        /* KIND_* */
  const char *label;                   /* NULL: drawn (backspace) */
};

/* What a button looks like: redrawn when it changes */

struct calc_look_s
{
  uint8_t look;                        /* UI_NORMAL, UI_HOVER, UI_PRESSED */
  bool lit;                            /* The pending operator */
  const char *label;
};

struct calc_s
{
  /* The arithmetic: sum sumop (prod prodop (entry)) */

  char entry[CALC_DIGITS + 4];         /* Being typed: digits, '.', '-' */
  bool typing;                         /* entry is the operand */
  bool operand;                        /* An operand since the operator */
  double value;                        /* Shown when not typing */
  double sum;
  int sumop;
  double prod;
  int prodop;
  int lastop;                          /* = again: this ... */
  double lastarg;                      /* ... with this */
  bool done;                           /* Just after =: a digit starts over */
  char expr[96];                       /* The expression line */
  const char *error;                   /* Shown instead of the number */

  /* The pad */

  struct ui_zones_s zones;
  struct ui_keys_s keys;
  int flash;                           /* A key's button lit, 0 */
  uint32_t flashms;
  int cw;                              /* The layout: the client size */
  int ch;
  int kx0;                             /* The keypad: first key, key size */
  int ky0;
  int kw;
  int kh;

  /* What is drawn */

  struct calc_look_s drawn[CALC_NKEYS];
  bool drawnvalid;
  char drawnexpr[96];
  char drawnnum[48];
  const struct desk_font_s *drawnfont;

  struct desk_ops_s o;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const struct calc_keydef_s g_calc_pad[CALC_NKEYS] =
{
  { K_CLEAR, KIND_FUNC,  "AC" },
  { K_BACK,  KIND_FUNC,  NULL },
  { K_PCT,   KIND_FUNC,  "%" },
  { K_DIV,   KIND_OP,    S_DIVIDE },
  { K_7,     KIND_DIGIT, "7" },
  { K_8,     KIND_DIGIT, "8" },
  { K_9,     KIND_DIGIT, "9" },
  { K_MUL,   KIND_OP,    DESK_CH_TIMES },
  { K_4,     KIND_DIGIT, "4" },
  { K_5,     KIND_DIGIT, "5" },
  { K_6,     KIND_DIGIT, "6" },
  { K_SUB,   KIND_OP,    DESK_CH_MINUS },
  { K_1,     KIND_DIGIT, "1" },
  { K_2,     KIND_DIGIT, "2" },
  { K_3,     KIND_DIGIT, "3" },
  { K_ADD,   KIND_OP,    "+" },
  { K_SIGN,  KIND_FUNC,  "+/" DESK_CH_MINUS },
  { K_0,     KIND_DIGIT, "0" },
  { K_POINT, KIND_DIGIT, "." },
  { K_EQ,    KIND_OP,    "=" },
};

static const char * const g_calc_opname[] =
{
  "", "+", DESK_CH_MINUS, DESK_CH_TIMES, S_DIVIDE
};

/****************************************************************************
 * Private Functions: numbers (desk_num.c: by hand, the C library may have
 * no floating point in printf() and strtod())
 ****************************************************************************/

#define calc_group(s, out, size)       num_group(s, out, size)
#define calc_plain(v, prec, out, size) num_plain(v, prec, out, size)
#define calc_format(v, prec, out, size) num_format(v, prec, out, size)
#define calc_parse(s, v)               num_parse(s, v)

static bool calc_apply(struct calc_s *c, double a, int op, double b,
                       double *r)
{
  switch (op)
    {
      case OP_ADD:
        *r = a + b;
        break;

      case OP_SUB:
        *r = a - b;
        break;

      case OP_MUL:
        *r = a * b;
        break;

      case OP_DIV:
        if (b == 0.0)
          {
            c->error = "Cannot divide by zero";
            return false;
          }

        *r = a / b;
        break;

      default:
        *r = b;
        break;
    }

  if (!isfinite(*r))
    {
      c->error = "Overflow";
      return false;
    }

  /* Rounding noise near zero (0.3 - 0.1 - 0.2) */

  if (fabs(*r) < 1e-15 * (fabs(a) + fabs(b)))
    {
      *r = 0.0;
    }

  return true;
}

/****************************************************************************
 * Private Functions: the calculator
 ****************************************************************************/

static void calc_reset(struct calc_s *c)
{
  c->entry[0] = '\0';
  c->typing   = false;
  c->operand  = false;
  c->value    = 0.0;
  c->sumop    = OP_NONE;
  c->prodop   = OP_NONE;
  c->lastop   = OP_NONE;
  c->done     = false;
  c->expr[0]  = '\0';
  c->error    = NULL;
}

static double calc_operand(struct calc_s *c)
{
  double v = 0.0;

  if (!c->typing)
    {
      return c->value;
    }

  calc_parse(c->entry, &v);
  return v;
}

static void calc_expr_add(struct calc_s *c, const char *s)
{
  if (c->expr[0] != '\0')
    {
      strlcat(c->expr, " ", sizeof(c->expr));
    }

  strlcat(c->expr, s, sizeof(c->expr));
}

/* The operand as the expression line shows it */

static void calc_expr_operand(struct calc_s *c, double v)
{
  char buf[48];

  calc_format(v, 15, buf, sizeof(buf));
  calc_expr_add(c, buf);
}

/* Fold the pending operations into v: the products, then (all) the sums */

static bool calc_fold(struct calc_s *c, double *v, bool sums)
{
  if (c->prodop != OP_NONE)
    {
      if (!calc_apply(c, c->prod, c->prodop, *v, v))
        {
          return false;
        }

      c->prodop = OP_NONE;
    }

  if (sums && c->sumop != OP_NONE)
    {
      if (!calc_apply(c, c->sum, c->sumop, *v, v))
        {
          return false;
        }

      c->sumop = OP_NONE;
    }

  return true;
}

static void calc_error(struct calc_s *c)
{
  const char *e = c->error;

  calc_reset(c);
  c->error = e;
}

static void calc_digit(struct calc_s *c, int d)
{
  size_t n;
  int digits = 0;
  int i;

  if (c->error != NULL || c->done)
    {
      calc_reset(c);
    }

  if (!c->typing)
    {
      c->entry[0] = '\0';
      c->typing   = true;
    }

  n = strlen(c->entry);
  for (i = 0; c->entry[i] != '\0'; i++)
    {
      digits += c->entry[i] >= '0' && c->entry[i] <= '9';
    }

  if (d < 0)
    {
      /* The point */

      if (strchr(c->entry, '.') != NULL)
        {
          return;
        }

      if (n == 0 || (n == 1 && c->entry[0] == '-'))
        {
          strlcat(c->entry, "0", sizeof(c->entry));
        }

      strlcat(c->entry, ".", sizeof(c->entry));
    }
  else if (digits < CALC_DIGITS)
    {
      char s[2] =
        {
          (char)('0' + d), '\0'
        };

      /* No leading zeros */

      if (strcmp(c->entry, "0") == 0 || strcmp(c->entry, "-0") == 0)
        {
          c->entry[n - 1] = '\0';
        }

      strlcat(c->entry, s, sizeof(c->entry));
    }

  c->operand = true;
}

static void calc_operator(struct calc_s *c, int op)
{
  double v;

  if (c->error != NULL)
    {
      calc_reset(c);
    }

  /* A second operator in a row replaces the first */

  if (!c->operand && (c->sumop != OP_NONE || c->prodop != OP_NONE))
    {
      char *sp = strrchr(c->expr, ' ');

      if (sp != NULL)
        {
          *sp = '\0';
        }
      else
        {
          c->expr[0] = '\0';
        }

      if (c->prodop != OP_NONE && (op == OP_ADD || op == OP_SUB))
        {
          c->value  = c->prod;
          c->prodop = OP_NONE;
          if (c->sumop != OP_NONE)
            {
              calc_fold(c, &c->value, true);
            }

          c->sum   = c->value;
          c->sumop = op;
        }
      else if (c->prodop != OP_NONE)
        {
          c->prodop = op;
        }
      else if (op == OP_ADD || op == OP_SUB)
        {
          c->sumop = op;
        }
      else
        {
          /* a + b + then x: (a + b) x */

          c->prod   = c->value;
          c->prodop = op;
          c->sumop  = OP_NONE;
        }

      calc_expr_add(c, g_calc_opname[op]);
      return;
    }

  if (c->done)
    {
      c->expr[0] = '\0';
      c->done    = false;
    }

  v = calc_operand(c);
  calc_expr_operand(c, v);
  calc_expr_add(c, g_calc_opname[op]);

  if (op == OP_ADD || op == OP_SUB)
    {
      if (!calc_fold(c, &v, true))
        {
          calc_error(c);
          return;
        }

      c->sum   = v;
      c->sumop = op;
    }
  else
    {
      if (!calc_fold(c, &v, false))
        {
          calc_error(c);
          return;
        }

      c->prod   = v;
      c->prodop = op;
    }

  c->value   = v;
  c->typing  = false;
  c->operand = false;
}

static void calc_equals(struct calc_s *c)
{
  double v;

  if (c->error != NULL)
    {
      calc_reset(c);
      return;
    }

  v = calc_operand(c);
  if (c->sumop == OP_NONE && c->prodop == OP_NONE)
    {
      /* Again: the last operation once more */

      if (c->lastop == OP_NONE)
        {
          return;
        }

      c->expr[0] = '\0';
      calc_expr_operand(c, v);
      calc_expr_add(c, g_calc_opname[c->lastop]);
      calc_expr_operand(c, c->lastarg);
      if (!calc_apply(c, v, c->lastop, c->lastarg, &v))
        {
          calc_error(c);
          return;
        }
    }
  else
    {
      /* The last operation, for = again: the innermost pending one */

      c->lastop  = c->prodop != OP_NONE ? c->prodop : c->sumop;
      c->lastarg = v;
      if (c->prodop != OP_NONE && c->sumop != OP_NONE)
        {
          c->lastop = OP_NONE;           /* Mixed: nothing to repeat */
        }

      if (!c->operand)
        {
          /* 5 + = : 5 + 5 */

          v = c->value;
          c->lastarg = v;
        }

      calc_expr_operand(c, v);
      if (!calc_fold(c, &v, true))
        {
          calc_error(c);
          return;
        }
    }

  calc_expr_add(c, "=");
  c->value   = v;
  c->typing  = false;
  c->operand = true;
  c->done    = true;
}

static void calc_percent(struct calc_s *c)
{
  double v;

  if (c->error != NULL)
    {
      return;
    }

  /* a + b% is a + a * b / 100; otherwise b / 100 */

  v = calc_operand(c);
  if (c->sumop != OP_NONE && c->prodop == OP_NONE)
    {
      v = c->sum * v / 100.0;
    }
  else
    {
      v = v / 100.0;
    }

  c->value   = v;
  c->typing  = false;
  c->operand = true;
  c->done    = false;
}

static void calc_sign(struct calc_s *c)
{
  if (c->error != NULL)
    {
      return;
    }

  if (c->typing)
    {
      if (c->entry[0] == '-')
        {
          memmove(c->entry, c->entry + 1, strlen(c->entry));
        }
      else if (strlen(c->entry) + 1 < sizeof(c->entry))
        {
          memmove(c->entry + 1, c->entry, strlen(c->entry) + 1);
          c->entry[0] = '-';
        }
    }
  else
    {
      c->value   = -c->value;
      c->operand = true;
      if (c->done)
        {
          c->expr[0] = '\0';
          c->done    = false;
        }
    }
}

static void calc_back(struct calc_s *c)
{
  size_t n;

  if (c->error != NULL)
    {
      calc_reset(c);
      return;
    }

  if (!c->typing)
    {
      return;
    }

  n = strlen(c->entry);
  if (n > 0)
    {
      c->entry[n - 1] = '\0';
    }

  if (c->entry[0] == '\0' || strcmp(c->entry, "-") == 0)
    {
      strlcpy(c->entry, "0", sizeof(c->entry));
    }
}

static void calc_press(struct calc_s *c, int key)
{
  switch (key)
    {
      case K_POINT:
        calc_digit(c, -1);
        break;

      case K_ADD:
        calc_operator(c, OP_ADD);
        break;

      case K_SUB:
        calc_operator(c, OP_SUB);
        break;

      case K_MUL:
        calc_operator(c, OP_MUL);
        break;

      case K_DIV:
        calc_operator(c, OP_DIV);
        break;

      case K_EQ:
        calc_equals(c);
        break;

      case K_PCT:
        calc_percent(c);
        break;

      case K_SIGN:
        calc_sign(c);
        break;

      case K_BACK:
        calc_back(c);
        break;

      case K_CLEAR:

        /* C: the entry; AC (nothing being typed): everything */

        if (c->typing && c->operand && c->error == NULL && !c->done)
          {
            strlcpy(c->entry, "0", sizeof(c->entry));
            c->operand = false;
          }
        else
          {
            calc_reset(c);
          }
        break;

      case K_CE:
        if (c->typing)
          {
            strlcpy(c->entry, "0", sizeof(c->entry));
          }
        break;

      case K_ALLCLEAR:
        calc_reset(c);
        break;

      default:
        if (key >= K_0 && key <= K_9)
          {
            calc_digit(c, key - K_0);
          }
        break;
    }
}

/* The number shown, grouped, in the largest font it fits in */

static const struct desk_font_s *calc_shown(struct calc_s *c, int room,
                                            char *out, size_t size)
{
  static const struct desk_font_s * const fonts[2] =
  {
    &g_desk_font_big, &g_desk_font_title
  };

  int f;
  int prec;

  if (c->error != NULL)
    {
      strlcpy(out, c->error, size);
      return text_width(&g_desk_font_title, out) <= room ?
             &g_desk_font_title : &g_desk_font_ui;
    }

  if (c->typing)
    {
      calc_group(c->entry, out, size);
      return text_width(fonts[0], out) <= room ? fonts[0] : fonts[1];
    }

  for (f = 0; f < 2; f++)
    {
      for (prec = 15; prec >= (f == 0 ? 10 : 1); prec--)
        {
          calc_format(c->value, prec, out, size);
          if (text_width(fonts[f], out) <= room)
            {
              return fonts[f];
            }
        }
    }

  return fonts[1];
}

/****************************************************************************
 * Private Functions: drawing helpers (the keypad, symbols)
 ****************************************************************************/

/* The division sign: a bar and two round dots, the size of the font's
 * plus
 */

static bool calc_dot_inside(void *arg, int x, int y)
{
  int r = *(int *)arg;                 /* In 1/16 pixel */

  x -= r;
  y -= r;
  return x * x + y * y <= r * r;
}

static int calc_divide(NXWINDOW hwnd, struct desk_ops_s *o,
                       const struct desk_font_s *f, int x, int y,
                       uint16_t col)
{
  const struct desk_glyph_s *g = &f->glyphs['+' - DESK_FONT_FIRST];
  int bar = f->size >= 30 ? 3 : 2;
  int cy = y + f->ascent - g->by + g->h / 2;
  int w = g->w;
  int dot = f->size >= 30 ? 5 : f->size >= 18 ? 4 : 3;
  int r = dot * 8;
  int gap = w / 4;

  x += g->bx;
  ops_fill(o, x, cy - bar / 2, w, bar, col);
  ops_shape(hwnd, o, x + (w - dot) / 2, cy - bar / 2 - gap - dot, dot, dot,
            col, 255, calc_dot_inside, &r);
  ops_shape(hwnd, o, x + (w - dot) / 2, cy - bar / 2 + bar + gap, dot, dot,
            col, 255, calc_dot_inside, &r);
  return g->adv;
}

/* Text with the division sign */

static int calc_text_width(const struct desk_font_s *f, const char *s)
{
  int w = 0;

  for (; *s != '\0'; s++)
    {
      char one[2] =
        {
          *s == CH_DIVIDE ? '+' : *s, '\0'
        };

      w += text_width(f, one);
    }

  return w;
}

static void calc_text(NXWINDOW hwnd, struct desk_ops_s *o,
                      const struct desk_font_s *f, int x, int y,
                      const char *s, uint16_t col)
{
  for (; *s != '\0'; s++)
    {
      char one[2] =
        {
          *s, '\0'
        };

      if (*s == CH_DIVIDE)
        {
          x += calc_divide(hwnd, o, f, x, y, col);
        }
      else
        {
          x += ops_text(o, f, x, y, one, col, 255);
        }
    }
}

/* The backspace symbol: an outlined tag pointing left with a cross in it */

struct calc_backsym_s
{
  int w;                               /* In 1/16 pixel */
  int h;
};

static bool calc_back_in(struct calc_backsym_s *b, int x, int y, int inset)
{
  int tip = b->h / 2;                  /* The point's length */
  int cy = b->h / 2;
  int dy = y > cy ? y - cy : cy - y;

  if (x < inset * 2 || x >= b->w - inset || y < inset || y >= b->h - inset)
    {
      return false;
    }

  if (x < tip)
    {
      /* The point: 45 degrees, the inset along the diagonal */

      return dy + inset * 3 / 2 <= x;
    }

  return true;
}

static bool calc_back_inside(void *arg, int x, int y)
{
  struct calc_backsym_s *b = arg;
  int stroke = 26;                     /* 1.6 pixels */
  int cx;
  int cy;
  int u;
  int v;
  int r;

  if (!calc_back_in(b, x, y, 0))
    {
      return false;
    }

  if (!calc_back_in(b, x, y, stroke))
    {
      return true;
    }

  /* The cross, in the body */

  cx = (b->h / 2 + b->w) / 2 + 8;
  cy = b->h / 2;
  r  = b->h / 5;
  u  = x - cx;
  v  = y - cy;
  if (u < -r || u > r || v < -r || v > r)
    {
      return false;
    }

  return (u - v < 0 ? v - u : u - v) <= 23 || (u + v < 0 ? -u - v : u + v)
         <= 23;
}

static void keypad_symbol_back(NXWINDOW hwnd, struct desk_ops_s *o, int cx,
                               int cy, uint16_t col)
{
  struct calc_backsym_s b;

  b.w = 26 * 16;
  b.h = 18 * 16;
  ops_shape(hwnd, o, cx - 13, cy - 9, 26, 18, col, 255,
            calc_back_inside, &b);
}

/* A button of the keypad: a round rectangle of its kind's colour, lighter
 * under the pointer and lighter still pressed; the pending operator
 * inverted (white, the label orange)
 */

static void keypad_button(NXWINDOW hwnd, struct desk_ops_s *o, int x,
                          int y, int w, int h, int kind, int look, bool lit,
                          const char *label)
{
  const struct desk_font_s *f = &g_desk_font_title;
  uint16_t bg;
  uint16_t fg = C_WHITE;

  switch (kind)
    {
      case KIND_OP:
        bg = C_KEY_OP;
        break;

      case KIND_FUNC:
        bg = C_KEY_FUNC;
        break;

      default:
        bg = C_KEY_DIGIT;
        break;
    }

  if (lit)
    {
      bg = C_WHITE;
      fg = C_KEY_OPTEXT;
    }

  if (look == UI_HOVER)
    {
      bg = colour_mix(bg, C_WHITE, 36);
    }
  else if (look == UI_PRESSED)
    {
      bg = colour_mix(bg, C_WHITE, 90);
    }

  ops_fill(o, x, y, w, h, C_WINDOW_BG);
  ops_rrect(o, x, y, w, h, LUT_R12, 12, bg, 255);

  /* A hairline of light along the top */

  ops_blend(o, x + 12, y, w - 24, 1, C_WHITE, 40);

  if (label == NULL)
    {
      keypad_symbol_back(hwnd, o, x + w / 2, y + h / 2, fg);
    }
  else
    {
      calc_text(hwnd, o, f, x + (w - calc_text_width(f, label)) / 2,
                y + (h - text_height(f)) / 2, label, fg);
    }
}

/****************************************************************************
 * Private Functions: the window
 ****************************************************************************/

static void calc_layout(struct desk_win_s *w)
{
  struct calc_s *c = w->priv;
  int i;

  c->cw  = w->csize.w;
  c->ch  = w->csize.h;
  c->kx0 = CALC_PAD;
  c->ky0 = CALC_DISP_H;
  c->kw  = (c->cw - 2 * CALC_PAD - (CALC_COLS - 1) * CALC_GAP) / CALC_COLS;
  c->kh  = (c->ch - CALC_DISP_H - CALC_PAD - (CALC_ROWS - 1) * CALC_GAP) /
           CALC_ROWS;
  c->kh  = c->kh < 24 ? 24 : c->kh;

  ui_zones_clear(&c->zones, 0);
  for (i = 0; i < CALC_NKEYS; i++)
    {
      ui_zone_add(&c->zones, 0, i + 1,
                  c->kx0 + (i % CALC_COLS) * (c->kw + CALC_GAP),
                  c->ky0 + (i / CALC_COLS) * (c->kh + CALC_GAP), c->kw,
                  c->kh, true);
    }

  c->drawnvalid = false;
}

/* The look each button should have now */

static void calc_looks(struct calc_s *c, struct calc_look_s *looks)
{
  int pending = c->operand ? OP_NONE :
                c->prodop != OP_NONE ? c->prodop : c->sumop;
  int i;

  for (i = 0; i < CALC_NKEYS; i++)
    {
      const struct calc_keydef_s *k = &g_calc_pad[i];

      looks[i].look  = c->flash == i + 1 ? UI_PRESSED :
                       ui_look(&c->zones, i + 1, true);
      looks[i].label = k->label;
      looks[i].lit   = (k->key == K_ADD && pending == OP_ADD) ||
                       (k->key == K_SUB && pending == OP_SUB) ||
                       (k->key == K_MUL && pending == OP_MUL) ||
                       (k->key == K_DIV && pending == OP_DIV);
      if (k->key == K_CLEAR)
        {
          looks[i].label = c->typing && c->operand && c->error == NULL &&
                           !c->done ?
                           "C" : "AC";
        }
    }
}

/* Draw what has changed (everything, if nothing is drawn yet) */

static void calc_update(struct desk_win_s *w)
{
  struct calc_s *c = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  struct desk_ops_s *o = &c->o;
  struct calc_look_s looks[CALC_NKEYS];
  const struct desk_font_s *f;
  char num[48];
  int room = c->cw - 2 * CALC_PAD - 8;
  int i;

  ops_reset(o);
  if (!c->drawnvalid)
    {
      ops_fill(o, 0, TITLE_H, c->cw, c->ch, C_WINDOW_BG);
      c->drawnexpr[0] = '\1';
      c->drawnnum[0]  = '\1';
    }

  /* The display: the expression line, the number */

  f = calc_shown(c, room, num, sizeof(num));
  if (strcmp(c->expr, c->drawnexpr) != 0)
    {
      const struct desk_font_s *ef = &g_desk_font_ui;
      const char *e = c->expr;
      int ey = TITLE_H + 16;
      int start;

      /* Too long: its end, after an ellipsis */

      while (calc_text_width(ef, e) > room - 12 && *e != '\0')
        {
          e++;
        }

      ops_fill(o, CALC_PAD, ey, c->cw - 2 * CALC_PAD, text_height(ef),
               C_WINDOW_BG);
      start = o->n;
      if (e != c->expr)
        {
          ops_text(o, ef, CALC_PAD + 4, ey, DESK_CH_ELLIPSIS, C_TEXT_DIM,
                   255);
        }

      calc_text(hwnd, o, ef, c->cw - CALC_PAD - 4 -
                calc_text_width(ef, e), ey, e, C_TEXT_DIM);
      ui_clip(o, start, CALC_PAD, ey, c->cw - CALC_PAD,
              ey + text_height(ef));
      strlcpy(c->drawnexpr, c->expr, sizeof(c->drawnexpr));
    }

  if (strcmp(num, c->drawnnum) != 0 || f != c->drawnfont)
    {
      int ny = TITLE_H + CALC_DISP_H - 16 - g_desk_font_big.ascent -
               g_desk_font_big.descent;
      int nh = text_height(&g_desk_font_big);

      ops_fill(o, CALC_PAD, ny, c->cw - 2 * CALC_PAD, nh, C_WINDOW_BG);

      /* Smaller fonts sit on the big one's baseline */

      ny += g_desk_font_big.ascent - f->ascent;
      calc_text(hwnd, o, f, c->cw - CALC_PAD - 4 -
                calc_text_width(f, num), ny, num,
                c->error != NULL ? C_CALC_ERROR : C_TEXT);
      strlcpy(c->drawnnum, num, sizeof(c->drawnnum));
      c->drawnfont = f;
    }

  /* The buttons whose look changed */

  calc_looks(c, looks);
  for (i = 0; i < CALC_NKEYS; i++)
    {
      struct ui_zone_s *z = &c->zones.z[i];

      if (c->drawnvalid && looks[i].look == c->drawn[i].look &&
          looks[i].lit == c->drawn[i].lit &&
          looks[i].label == c->drawn[i].label)
        {
          continue;
        }

      ui_flush(hwnd, o, 64);
      keypad_button(hwnd, o, z->x, TITLE_H + z->y, z->w, z->h,
                    g_calc_pad[i].kind, looks[i].look, looks[i].lit,
                    looks[i].label);
      c->drawn[i] = looks[i];
    }

  c->drawnvalid = true;
  ops_draw(hwnd, o);
}

static void calc_draw(struct desk_win_s *w)
{
  struct calc_s *c = w->priv;

  if (c->cw != w->csize.w || c->ch != w->csize.h)
    {
      calc_layout(w);
    }

  c->drawnvalid = false;
  calc_update(w);
}

static void calc_resize(struct desk_win_s *w)
{
  calc_layout(w);
  calc_update(w);
}

/* A key lights its button for a moment */

static void calc_flash(struct desk_win_s *w, int key)
{
  struct calc_s *c = w->priv;
  int i;

  for (i = 0; i < CALC_NKEYS; i++)
    {
      if (g_calc_pad[i].key == key)
        {
          c->flash   = i + 1;
          c->flashms = desk_ms();
          w->frames  = true;
          return;
        }
    }
}

static void calc_frame(struct desk_win_s *w)
{
  struct calc_s *c = w->priv;

  if (c->flash != 0 && desk_ms() - c->flashms >= CALC_FLASH_MS)
    {
      c->flash  = 0;
      w->frames = false;
      calc_update(w);
    }
}

static void calc_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct calc_s *c = w->priv;
  struct ui_zone_s *z = ui_zone_at(&c->zones, x, y);
  bool down = (buttons & NX_MOUSE_LEFTBUTTON) != 0;
  int hot = z != NULL ? z->id : 0;

  c->zones.hot = hot;
  if (down && c->zones.pressed == 0)
    {
      c->zones.pressed = hot != 0 ? hot : -1;
    }
  else if (!down && c->zones.pressed != 0)
    {
      if (c->zones.pressed == hot && hot > 0)
        {
          calc_press(c, g_calc_pad[hot - 1].key);
        }

      c->zones.pressed = 0;
    }

  calc_update(w);
}

static void calc_copy(struct calc_s *c)
{
  char buf[48];

  if (c->error != NULL)
    {
      return;
    }

  if (c->typing)
    {
      strlcpy(buf, c->entry, sizeof(buf));
    }
  else
    {
      calc_plain(c->value, 15, buf, sizeof(buf));
    }

  desk_clip_set(buf, strlen(buf));
}

static void calc_paste(struct calc_s *c)
{
  size_t n;
  const char *s = desk_clip_get(&n);
  char buf[32];
  double v;

  if (s == NULL || n == 0 || n >= sizeof(buf))
    {
      return;
    }

  memcpy(buf, s, n);
  buf[n] = '\0';
  if (calc_parse(buf, &v) == 0 || !isfinite(v))
    {
      return;
    }

  if (c->error != NULL || c->done)
    {
      calc_reset(c);
    }

  c->value   = v;
  c->typing  = false;
  c->operand = true;
}

static void calc_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct calc_s *c = w->priv;
  int keys[16];
  int nk = ui_keys(&c->keys, ch, n, keys, 16);
  int i;

  for (i = 0; i < nk; i++)
    {
      int k = -1;

      switch (keys[i])
        {
          case '+':
            k = K_ADD;
            break;

          case '-':
            k = K_SUB;
            break;

          case '*':
          case 'x':
          case 'X':
            k = K_MUL;
            break;

          case '/':
            k = K_DIV;
            break;

          case '=':
          case '\r':
          case '\n':
            k = K_EQ;
            break;

          case '%':
            k = K_PCT;
            break;

          case '.':
          case ',':
            k = K_POINT;
            break;

          case 'n':
          case 'N':
            k = K_SIGN;
            break;

          case 0x7f:
          case 0x08:
            k = K_BACK;
            break;

          case KEY_ESC:
            k = K_ALLCLEAR;
            break;

          case KEY_DEL:
            k = K_CE;
            break;

          case KEY_CTL('c'):
            calc_copy(c);
            break;

          case KEY_CTL('v'):
            calc_paste(c);
            break;

          default:
            if (keys[i] >= '0' && keys[i] <= '9')
              {
                k = K_0 + keys[i] - '0';
              }
            break;
        }

      if (k >= 0)
        {
          calc_press(c, k);
          calc_flash(w, k == K_ALLCLEAR || k == K_CE ? K_CLEAR : k);
        }
    }

  calc_update(w);
}

static void calc_close(struct desk_win_s *w)
{
  w->frames = false;
}

/* The icon: an orange tile with a keypad of four keys, one of them the
 * operators' colour
 */

static void calc_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  int k = s / 5;
  int gap = s / 12;
  int x0 = x + (s - 2 * k - gap) / 2;
  int y0 = y + s / 2 - k / 4;
  int i;

  icon_tile(o, x, y, s, RGB(0x5c, 0x5c, 0x66), RGB(0x2c, 0x2c, 0x32), a);

  /* The display */

  ops_rrect(o, x + s / 5, y + s / 6, s * 3 / 5, s / 5, LUT_R3, 3,
            RGB(0xd8, 0xf0, 0xd8), (uint8_t)(a * 220 / 255));
  for (i = 0; i < 4; i++)
    {
      ops_rrect(o, x0 + (i % 2) * (k + gap), y0 + (i / 2) * (k + gap), k, k,
                LUT_R3, 3, i == 3 ? C_KEY_OP : RGB(0xe8, 0xe8, 0xee), a);
    }
}

static struct desk_win_s *calc_open(void)
{
  struct desk_win_s *w;
  struct calc_s *c;

  c = calloc(1, sizeof(*c));
  if (c == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_CALC, "Calculator", CALC_W, CALC_H);
  if (w == NULL)
    {
      free(c);
      return NULL;
    }

  w->priv = c;
  calc_reset(c);
  calc_layout(w);
  calc_update(w);
  return w;
}

/****************************************************************************
 * Public Data
 ****************************************************************************/

const struct desk_app_s g_app_calc =
{
  .name   = "Calculator",
  .icon   = calc_icon,
  .open   = calc_open,
  .draw   = calc_draw,
  .resize = calc_resize,
  .mouse  = calc_mouse,
  .key    = calc_key,
  .close  = calc_close,
  .frame  = calc_frame,
  .single = true,
};
