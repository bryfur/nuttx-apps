/****************************************************************************
 * apps/graphics/fpcdesk/desk_breakout.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Breakout: a paddle that follows the mouse (or the arrow keys), a ball,
 * and rows of translucent bricks.  A brick scores more the higher its
 * row, and breaks into fragments that fall and fade.  The ball speeds up
 * a little at every hit of the paddle; where it hits the paddle sets the
 * angle it leaves at.  Three balls; a cleared wall brings the next level,
 * in another pattern and faster.
 *
 * Click or Space launches the ball; p or Escape pauses (as does another
 * window becoming active); Enter or F2 starts a new game.
 *
 * Drawing: the game runs on the desktop's frames (w->frames) while the
 * ball flies or fragments fall, about 60 a second.  The window remembers
 * where it drew each thing (the ball, the paddle, each fragment, each
 * brick, the message), and each frame draws again only the rectangles
 * where something moved, came or went: the background (a gradient, the
 * same wherever it is cut), the bricks, the paddle, fragments and the
 * ball in them, clipped.  A brick is one COPY from a hidden surface that
 * holds one brick of each row, drawn once with its gradient over the
 * background it will be on.  A frame of play is about 10 to 30 operations.
 *
 * A program of its own (/mnt/sd0/bin/breakout, apps/graphics/deskapps):
 * built into the desktop (DESK_PROGRAM_STUB) this file is only the icon
 * and the name the dock shows (desk_program.c).
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "desk.h"
#include "desk_ui.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define BRK_W           560
#define BRK_H           540
#define BRK_HUD_H       34
#define BRK_COLS        10
#define BRK_ROWS        6
#define BRK_BH          18              /* A brick's height */
#define BRK_GAP         4
#define BRK_SIDE        20
#define BRK_TOP         48              /* The wall, in the field */
#define BRK_PADDLE_W    90
#define BRK_PADDLE_H    12
#define BRK_PADDLE_DY   36              /* From the field's bottom */
#define BRK_BALL_R      6
#define BRK_SPEED0      300.0f          /* Pixels a second, at level 1 */
#define BRK_SPEED_MAX   760.0f
#define BRK_LIVES       3
#define BRK_NPART       96
#define BRK_PART_MS     700
#define BRK_NDIRTY      40

#define C_BRK_BG0       RGB(0x1c, 0x1e, 0x36)
#define C_BRK_BG1       RGB(0x06, 0x06, 0x0e)

/* States */

#define ST_SERVE        0               /* The ball on the paddle */
#define ST_PLAY         1
#define ST_PAUSED       2
#define ST_OVER         3

/* The rows' colours (the icon's too) */

static const uint16_t g_brk_colour[BRK_ROWS] =
{
  RGB(0xff, 0x4a, 0x5a), RGB(0xff, 0x94, 0x2c), RGB(0xff, 0xd6, 0x2e),
  RGB(0x4c, 0xd9, 0x64), RGB(0x2c, 0xc4, 0xf0), RGB(0x7a, 0x6c, 0xff)
};

#ifndef DESK_PROGRAM_STUB

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct brk_rect_s
{
  int x;
  int y;
  int w;
  int h;
};

struct brk_part_s
{
  float x;                              /* In the field */
  float y;
  float vx;
  float vy;
  uint16_t colour;
  uint16_t age;                         /* ms; >= BRK_PART_MS: gone */
  uint8_t size;
};

struct breakout_s
{
  /* The game */

  bool brick[BRK_ROWS][BRK_COLS];
  int nbricks;
  float bx;                             /* The ball, in the field */
  float by;
  float vx;                             /* Pixels a second */
  float vy;
  float speed;
  float px;                             /* The paddle's centre */
  int state;
  int prevstate;
  unsigned int score;
  int lives;
  int level;
  uint32_t last;
  uint32_t seed;
  struct brk_part_s part[BRK_NPART];

  /* The layout: the field, frame coordinates */

  int cw;
  int ch;
  int fx;
  int fy;
  int fw;
  int fh;
  int bw;                               /* A brick's width */

  /* What is drawn */

  bool valid;
  struct brk_rect_s drawnball;
  struct brk_rect_s drawnpaddle;
  struct brk_rect_s drawnmsg;
  const char *drawntext;
  struct brk_rect_s drawnpart[BRK_NPART];
  bool drawnbrick[BRK_ROWS][BRK_COLS];
  unsigned int drawnscore;
  int drawnlives;
  int drawnlevel;
  struct brk_rect_s dirty[BRK_NDIRTY];
  int ndirty;

  struct ui_keys_s keys;
  uint8_t buttons;
  struct desk_ops_s o;
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static NXWINDOW g_brk_sprite;           /* One brick of each row */
static int g_brk_spritew;               /* The brick width drawn in it */
static volatile bool g_brk_lost;

/****************************************************************************
 * Private Functions: the game
 ****************************************************************************/

static uint32_t brk_rand(struct breakout_s *b)
{
  b->seed = b->seed * 1103515245 + 12345;
  return b->seed >> 16;
}

static float brk_sqrt(float v)
{
  float r = v > 1.0f ? v : 1.0f;
  int i;

  for (i = 0; i < 20; i++)
    {
      r = 0.5f * (r + v / r);
    }

  return r;
}

static int brk_brick_x(struct breakout_s *b, int col)
{
  return (b->fw - (BRK_COLS * b->bw + (BRK_COLS - 1) * BRK_GAP)) / 2 +
         col * (b->bw + BRK_GAP);
}

static int brk_brick_y(int row)
{
  return BRK_TOP + row * (BRK_BH + BRK_GAP);
}

static float brk_paddle_y(struct breakout_s *b)
{
  return b->fh - BRK_PADDLE_DY;
}

/* A wall: level 1 full, then a pyramid, a checkerboard, stripes, ... */

static void brk_wall(struct breakout_s *b)
{
  int r;
  int c;

  b->nbricks = 0;
  for (r = 0; r < BRK_ROWS; r++)
    {
      for (c = 0; c < BRK_COLS; c++)
        {
          bool on;

          switch ((b->level - 1) % 4)
            {
              case 1:
                on = c >= BRK_ROWS - 1 - r && c <= BRK_COLS - BRK_ROWS + r;
                break;

              case 2:
                on = (r + c) % 2 == 0 || r == BRK_ROWS - 1;
                break;

              case 3:
                on = c != 2 && c != 7;
                break;

              default:
                on = true;
                break;
            }

          b->brick[r][c] = on;
          b->nbricks += on;
        }
    }
}

static void brk_serve(struct breakout_s *b)
{
  b->state = ST_SERVE;
  b->speed = BRK_SPEED0 + 40.0f * (b->level - 1);
  b->speed = b->speed > BRK_SPEED_MAX ? BRK_SPEED_MAX : b->speed;
}

static void brk_new(struct breakout_s *b)
{
  b->score = 0;
  b->lives = BRK_LIVES;
  b->level = 1;
  b->px    = b->fw / 2;
  brk_wall(b);
  brk_serve(b);
}

static void brk_launch(struct breakout_s *b)
{
  float t = ((int)(brk_rand(b) % 101) - 50) / 100.0f;

  b->vx    = b->speed * t * 0.6f;
  b->vy    = -brk_sqrt(b->speed * b->speed - b->vx * b->vx);
  b->state = ST_PLAY;
  b->last  = desk_ms();
}

/* A brick breaks: points, fragments */

static void brk_break(struct breakout_s *b, int r, int c)
{
  int x = brk_brick_x(b, c);
  int y = brk_brick_y(r);
  int n = 0;
  int i;

  b->brick[r][c] = false;
  b->nbricks--;
  b->score += (BRK_ROWS - r) * 10;

  for (i = 0; i < BRK_NPART && n < 12; i++)
    {
      struct brk_part_s *p = &b->part[i];

      if (p->age < BRK_PART_MS && p->size != 0)
        {
          continue;
        }

      p->x      = x + brk_rand(b) % b->bw;
      p->y      = y + brk_rand(b) % BRK_BH;
      p->vx     = ((int)(brk_rand(b) % 241) - 120);
      p->vy     = -((int)(brk_rand(b) % 160)) + 20;
      p->colour = g_brk_colour[r];
      p->age    = brk_rand(b) % 150;
      p->size   = 3 + brk_rand(b) % 4;
      n++;
    }
}

/* The ball against the bricks: the first it overlaps, bounced off the
 * side it came from
 */

static void brk_hit_bricks(struct breakout_s *b, float ox, float oy)
{
  int r;
  int c;

  for (r = 0; r < BRK_ROWS; r++)
    {
      for (c = 0; c < BRK_COLS; c++)
        {
          float x0;
          float y0;

          if (!b->brick[r][c])
            {
              continue;
            }

          x0 = brk_brick_x(b, c);
          y0 = brk_brick_y(r);
          if (b->bx + BRK_BALL_R <= x0 || b->bx - BRK_BALL_R >= x0 + b->bw ||
              b->by + BRK_BALL_R <= y0 || b->by - BRK_BALL_R >= y0 + BRK_BH)
            {
              continue;
            }

          if (ox + BRK_BALL_R <= x0 || ox - BRK_BALL_R >= x0 + b->bw)
            {
              b->vx = -b->vx;
              b->bx = ox;
            }
          else
            {
              b->vy = -b->vy;
              b->by = oy;
            }

          brk_break(b, r, c);
          return;
        }
    }
}

/* Time passes: the ball flies (in steps of a few pixels), the fragments
 * fall
 */

static void brk_step(struct breakout_s *b, int dt)
{
  float py = brk_paddle_y(b);
  float s = dt / 1000.0f;
  int steps;
  int i;

  for (i = 0; i < BRK_NPART; i++)
    {
      struct brk_part_s *p = &b->part[i];

      if (p->size == 0 || p->age >= BRK_PART_MS)
        {
          continue;
        }

      p->age += dt;
      p->x   += p->vx * s;
      p->y   += p->vy * s;
      p->vy  += 900.0f * s;
      if (p->age >= BRK_PART_MS || p->y > b->fh)
        {
          p->age = BRK_PART_MS;
        }
    }

  if (b->state == ST_SERVE)
    {
      b->bx = b->px;
      b->by = py - BRK_BALL_R - 1;
      return;
    }

  if (b->state != ST_PLAY)
    {
      return;
    }

  steps = (int)(b->speed * s / 3.0f) + 1;
  for (i = 0; i < steps; i++)
    {
      float ox = b->bx;
      float oy = b->by;

      b->bx += b->vx * s / steps;
      b->by += b->vy * s / steps;

      /* The walls */

      if (b->bx < BRK_BALL_R)
        {
          b->bx = BRK_BALL_R;
          b->vx = b->vx < 0 ? -b->vx : b->vx;
        }
      else if (b->bx > b->fw - BRK_BALL_R)
        {
          b->bx = b->fw - BRK_BALL_R;
          b->vx = b->vx > 0 ? -b->vx : b->vx;
        }

      if (b->by < BRK_BALL_R)
        {
          b->by = BRK_BALL_R;
          b->vy = b->vy < 0 ? -b->vy : b->vy;
        }

      /* The paddle: where it is hit sets the angle */

      if (b->vy > 0 && b->by + BRK_BALL_R >= py && oy + BRK_BALL_R <= py + 2
          && b->bx >= b->px - BRK_PADDLE_W / 2 - BRK_BALL_R &&
          b->bx <= b->px + BRK_PADDLE_W / 2 + BRK_BALL_R)
        {
          float t = (b->bx - b->px) / (BRK_PADDLE_W / 2);

          t = t < -1.0f ? -1.0f : t > 1.0f ? 1.0f : t;
          b->speed *= 1.025f;
          b->speed  = b->speed > BRK_SPEED_MAX ? BRK_SPEED_MAX : b->speed;
          b->vx     = b->speed * t * 0.82f;
          b->vy     = -brk_sqrt(b->speed * b->speed - b->vx * b->vx);
          b->by     = py - BRK_BALL_R;
        }

      brk_hit_bricks(b, ox, oy);

      /* Past the paddle: a ball lost */

      if (b->by > b->fh + BRK_BALL_R)
        {
          b->lives--;
          if (b->lives <= 0)
            {
              b->state = ST_OVER;
            }
          else
            {
              brk_serve(b);
            }

          return;
        }

      if (b->nbricks == 0)
        {
          b->level++;
          b->score += 500;
          brk_wall(b);
          brk_serve(b);
          return;
        }
    }
}

/****************************************************************************
 * Private Functions: drawing helpers
 ****************************************************************************/

/* The background: a gradient down the field.  origin: the field's top in
 * the surface drawn into (it is the same wherever it is cut)
 */

static void bg_draw(struct desk_ops_s *o, int x, int y, int w, int h,
                    int origin, int fh)
{
  struct nx_gpuop_s *op;

  ops_fill(o, x, y, w, h, C_BRK_BG0);
  op = ops_add(o);
  op->op     = NX_GPUOP_BLEND;
  op->x      = x;
  op->y      = y;
  op->w      = w;
  op->h      = h;
  op->colour = C_BRK_BG1;
  op->mode   = NX_BLEND_SRC_COLOUR | NX_BLEND_DITHER | NX_BLEND_MOD_VRAMP;
  op->p[0]   = origin;
  op->p[1]   = fh > 1 ? (255 * 256) / (fh - 1) : 0;
  op->p[2]   = 0;
}

/* A brick: translucent, lighter at the top, a line of light along it */

static void brick_draw(struct desk_ops_s *o, int x, int y, int w, int h,
                       uint16_t c)
{
  ops_rrect_vgrad(o, x, y, w, h, LUT_R3, 3, colour_mix(c, C_WHITE, 80),
                  colour_mix(c, C_BLACK, 50), 200);
  ops_blend(o, x + 3, y + 1, w - 6, 1, C_WHITE, 120);
  ops_blend(o, x + 3, y + h - 1, w - 6, 1, C_BLACK, 70);
}

static void paddle_draw(struct desk_ops_s *o, int x, int y)
{
  ops_rrect(o, x, y + 3, BRK_PADDLE_W, BRK_PADDLE_H, LUT_R6, 6, C_BLACK,
            90);
  ops_rrect_vgrad(o, x, y, BRK_PADDLE_W, BRK_PADDLE_H, LUT_R6, 6,
                  RGB(0xf0, 0xf2, 0xff), RGB(0x80, 0x88, 0xb0), 255);
  ops_blend(o, x + 6, y + 1, BRK_PADDLE_W - 12, 1, C_WHITE, 160);
}

static void ball_draw(struct desk_ops_s *o, int cx, int cy)
{
  ops_disc(o, cx, cy, LUT_R6, BRK_BALL_R, C_WHITE, 255);
  ops_disc(o, cx + 2, cy + 2, LUT_R3, 3, RGB(0x90, 0x98, 0xc0), 90);
  ops_disc(o, cx - 2, cy - 2, LUT_R3, 3, C_WHITE, 255);
}

/* The message panel: a word and a hint */

static void msg_draw(struct desk_ops_s *o, const struct brk_rect_s *r,
                     const char *text)
{
  const char *hint = strchr(text, '\n');
  char title[40];
  size_t n = hint != NULL ? (size_t)(hint - text) : strlen(text);

  n = n < sizeof(title) - 1 ? n : sizeof(title) - 1;
  memcpy(title, text, n);
  title[n] = '\0';
  ops_rrect(o, r->x, r->y, r->w, r->h, LUT_R12, 12, RGB(0x24, 0x24, 0x2e),
            225);
  ops_text_centre(o, &g_desk_font_title, r->x + r->w / 2, r->y + 16, title,
                  C_TEXT, 255);
  if (hint != NULL)
    {
      ops_text_centre(o, &g_desk_font_ui, r->x + r->w / 2, r->y + 50,
                      hint + 1, C_TEXT_DIM, 255);
    }
}

/* The surface: one brick of each row over the background it will be on */

static void sprite_make(struct breakout_s *b)
{
  struct desk_ops_s *o = &b->o;
  int r;

  if (g_brk_sprite == NULL)
    {
      g_brk_sprite = desk_surface_open(128, BRK_ROWS * BRK_BH, &g_brk_lost);
      if (g_brk_sprite == NULL)
        {
          return;
        }
    }

  g_brk_lost = false;
  ops_reset(o);
  for (r = 0; r < BRK_ROWS; r++)
    {
      int sy = r * BRK_BH;

      /* Row r of the sprite shows field y brk_brick_y(r) */

      bg_draw(o, 0, sy, b->bw, BRK_BH, sy - brk_brick_y(r), b->fh);
      brick_draw(o, 0, sy, b->bw, BRK_BH, g_brk_colour[r]);
    }

  ops_draw(g_brk_sprite, o);
  g_brk_spritew = b->bw;
}

/****************************************************************************
 * Private Functions: the window
 ****************************************************************************/

static void brk_layout(struct desk_win_s *w)
{
  struct breakout_s *b = w->priv;
  float oldfw = b->fw;

  b->cw = w->csize.w;
  b->ch = w->csize.h;
  b->fx = 0;
  b->fy = TITLE_H + BRK_HUD_H;
  b->fw = b->cw;
  b->fh = b->ch - BRK_HUD_H;
  b->bw = (b->fw - 2 * BRK_SIDE - (BRK_COLS - 1) * BRK_GAP) / BRK_COLS;
  b->bw = b->bw > 120 ? 120 : b->bw < 16 ? 16 : b->bw;
  if (oldfw > 0)
    {
      b->px = b->px * b->fw / oldfw;
      b->bx = b->bx * b->fw / oldfw;
    }

  b->valid = false;
  if (g_brk_spritew != b->bw || g_brk_sprite == NULL)
    {
      sprite_make(b);
    }
}

static struct brk_rect_s brk_ball_rect(struct breakout_s *b)
{
  struct brk_rect_s r;

  r.x = b->fx + (int)b->bx - BRK_BALL_R - 1;
  r.y = b->fy + (int)b->by - BRK_BALL_R - 1;
  r.w = 2 * BRK_BALL_R + 3;
  r.h = 2 * BRK_BALL_R + 3;
  return r;
}

static struct brk_rect_s brk_paddle_rect(struct breakout_s *b)
{
  struct brk_rect_s r;

  r.x = b->fx + (int)b->px - BRK_PADDLE_W / 2;
  r.y = b->fy + (int)brk_paddle_y(b);
  r.w = BRK_PADDLE_W;
  r.h = BRK_PADDLE_H + 4;
  return r;
}

static struct brk_rect_s brk_part_rect(struct breakout_s *b,
                                       struct brk_part_s *p)
{
  struct brk_rect_s r;

  r.x = b->fx + (int)p->x;
  r.y = b->fy + (int)p->y;
  r.w = p->size;
  r.h = p->size;
  return r;
}

/* The message the field shows now, or NULL */

static const char *brk_message(struct breakout_s *b)
{
  switch (b->state)
    {
      case ST_SERVE:
        return b->score == 0 && b->lives == BRK_LIVES && b->level == 1 ?
               "Breakout\nClick or press Space to launch" :
               "Ready\nClick or press Space to launch";

      case ST_PAUSED:
        return "Paused\nClick or press P to go on";

      case ST_OVER:
        return "Game Over\nClick or press Enter to play again";

      default:
        return NULL;
    }
}

static bool rect_meet(const struct brk_rect_s *a, const struct brk_rect_s *c)
{
  return a->w > 0 && c->w > 0 && a->x < c->x + c->w && c->x < a->x + a->w &&
         a->y < c->y + c->h && c->y < a->y + a->h;
}

/* Something to draw again: merged with a rectangle it meets */

static void brk_dirty(struct breakout_s *b, struct brk_rect_s r)
{
  int i;

  if (r.w <= 0 || r.h <= 0)
    {
      return;
    }

  for (i = 0; i < b->ndirty; i++)
    {
      struct brk_rect_s *d = &b->dirty[i];
      struct brk_rect_s grow = *d;

      grow.x -= 4;
      grow.y -= 4;
      grow.w += 8;
      grow.h += 8;
      if (rect_meet(&grow, &r))
        {
          int x1 = d->x + d->w > r.x + r.w ? d->x + d->w : r.x + r.w;
          int y1 = d->y + d->h > r.y + r.h ? d->y + d->h : r.y + r.h;

          d->x = d->x < r.x ? d->x : r.x;
          d->y = d->y < r.y ? d->y : r.y;
          d->w = x1 - d->x;
          d->h = y1 - d->y;
          return;
        }
    }

  if (b->ndirty == BRK_NDIRTY)
    {
      /* Too many: the first grows to take this one too */

      struct brk_rect_s *d = &b->dirty[0];
      int x1 = d->x + d->w > r.x + r.w ? d->x + d->w : r.x + r.w;
      int y1 = d->y + d->h > r.y + r.h ? d->y + d->h : r.y + r.h;

      d->x = d->x < r.x ? d->x : r.x;
      d->y = d->y < r.y ? d->y : r.y;
      d->w = x1 - d->x;
      d->h = y1 - d->y;
      return;
    }

  b->dirty[b->ndirty++] = r;
}

static void brk_clip(NXWINDOW hwnd, struct desk_ops_s *o, int from,
                     const struct brk_rect_s *r)
{
  ui_clip(o, from, r->x, r->y, r->x + r->w, r->y + r->h);
  ui_flush(hwnd, o, 24);
}

/* The field within r: everything in it, clipped */

static void brk_paint(struct desk_win_s *w, struct brk_rect_s r)
{
  struct breakout_s *b = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  struct desk_ops_s *o = &b->o;
  struct brk_rect_s e;
  const char *msg = brk_message(b);
  int from;
  int row;
  int col;
  int i;

  /* Within the field */

  if (r.x < b->fx)
    {
      r.w -= b->fx - r.x;
      r.x  = b->fx;
    }

  if (r.y < b->fy)
    {
      r.h -= b->fy - r.y;
      r.y  = b->fy;
    }

  r.w = r.x + r.w > b->fx + b->fw ? b->fx + b->fw - r.x : r.w;
  r.h = r.y + r.h > b->fy + b->fh ? b->fy + b->fh - r.y : r.h;
  if (r.w <= 0 || r.h <= 0)
    {
      return;
    }

  ui_flush(hwnd, o, 24);
  from = o->n;
  bg_draw(o, r.x, r.y, r.w, r.h, b->fy, b->fh);
  brk_clip(hwnd, o, from, &r);

  for (row = 0; row < BRK_ROWS; row++)
    {
      for (col = 0; col < BRK_COLS; col++)
        {
          struct nx_gpuop_s *op;

          e.x = b->fx + brk_brick_x(b, col);
          e.y = b->fy + brk_brick_y(row);
          e.w = b->bw;
          e.h = BRK_BH;
          if (!b->brick[row][col] || !rect_meet(&e, &r))
            {
              continue;
            }

          from = o->n;
          op = ops_add(o);
          op->op  = NX_GPUOP_COPY;
          op->x   = e.x;
          op->y   = e.y;
          op->w   = e.w;
          op->h   = e.h;
          op->src = g_brk_sprite;
          op->sx  = 0;
          op->sy  = row * BRK_BH;
          brk_clip(hwnd, o, from, &r);
        }
    }

  e = brk_paddle_rect(b);
  if (b->state != ST_OVER && rect_meet(&e, &r))
    {
      from = o->n;
      paddle_draw(o, e.x, e.y);
      brk_clip(hwnd, o, from, &r);
    }

  for (i = 0; i < BRK_NPART; i++)
    {
      struct brk_part_s *p = &b->part[i];

      if (p->size == 0 || p->age >= BRK_PART_MS)
        {
          continue;
        }

      e = brk_part_rect(b, p);
      if (rect_meet(&e, &r))
        {
          from = o->n;
          ops_rrect(o, e.x, e.y, e.w, e.h, LUT_R3, e.w / 2, p->colour,
                    (uint8_t)(230 - p->age * 230 / BRK_PART_MS));
          brk_clip(hwnd, o, from, &r);
        }
    }

  e = brk_ball_rect(b);
  if (b->state != ST_OVER && rect_meet(&e, &r))
    {
      from = o->n;
      ball_draw(o, b->fx + (int)b->bx, b->fy + (int)b->by);
      brk_clip(hwnd, o, from, &r);
    }

  if (msg != NULL && rect_meet(&b->drawnmsg, &r))
    {
      from = o->n;
      msg_draw(o, &b->drawnmsg, msg);
      brk_clip(hwnd, o, from, &r);
    }
}

/* The strip at the top: the score, the level, the balls left */

static void hud_draw(struct breakout_s *b, struct desk_ops_s *o)
{
  char buf[32];
  int i;
  int ty = TITLE_H + (BRK_HUD_H - text_height(&g_desk_font_uib)) / 2;

  ops_fill(o, 0, TITLE_H, b->cw, BRK_HUD_H, C_UI_BAR);
  ops_fill(o, 0, TITLE_H + BRK_HUD_H - 1, b->cw, 1, C_UI_LINE);
  snprintf(buf, sizeof(buf), "Score  %u", b->score);
  ops_text(o, &g_desk_font_uib, 14, ty, buf, C_TEXT, 255);
  snprintf(buf, sizeof(buf), "Level %d", b->level);
  ops_text_centre(o, &g_desk_font_uib, b->cw / 2, ty, buf, C_TEXT_DIM,
                  255);
  for (i = 0; i < BRK_LIVES; i++)
    {
      ops_disc(o, b->cw - 20 - i * 18, TITLE_H + BRK_HUD_H / 2, LUT_R6, 6,
               i < b->lives ? C_WHITE : C_UI_HOVER, 255);
    }

  b->drawnscore = b->score;
  b->drawnlives = b->lives;
  b->drawnlevel = b->level;
}

/* Draw what has changed since the last time */

static void brk_render(struct desk_win_s *w)
{
  struct breakout_s *b = w->priv;
  NXWINDOW hwnd = (NXWINDOW)w->hwnd;
  struct desk_ops_s *o = &b->o;
  struct brk_rect_s r;
  const char *msg;
  int row;
  int col;
  int i;

  if (g_brk_lost || g_brk_spritew != b->bw)
    {
      sprite_make(b);
      b->valid = false;
    }

  ops_reset(o);
  b->ndirty = 0;
  msg = brk_message(b);

  if (!b->valid)
    {
      r.x = b->fx;
      r.y = b->fy;
      r.w = b->fw;
      r.h = b->fh;
      brk_dirty(b, r);
      hud_draw(b, o);
    }

  /* The ball, the paddle: where they were and are */

  r = brk_ball_rect(b);
  if (!b->valid || memcmp(&r, &b->drawnball, sizeof(r)) != 0)
    {
      brk_dirty(b, b->drawnball);
      brk_dirty(b, r);
      b->drawnball = r;
    }

  r = brk_paddle_rect(b);
  if (!b->valid || memcmp(&r, &b->drawnpaddle, sizeof(r)) != 0)
    {
      brk_dirty(b, b->drawnpaddle);
      brk_dirty(b, r);
      b->drawnpaddle = r;
    }

  /* Fragments: all of them, they move all the time */

  for (i = 0; i < BRK_NPART; i++)
    {
      struct brk_part_s *p = &b->part[i];

      brk_dirty(b, b->drawnpart[i]);
      memset(&b->drawnpart[i], 0, sizeof(b->drawnpart[i]));
      if (p->size != 0 && p->age < BRK_PART_MS)
        {
          b->drawnpart[i] = brk_part_rect(b, p);
          brk_dirty(b, b->drawnpart[i]);
        }
    }

  /* Bricks that went or came */

  for (row = 0; row < BRK_ROWS; row++)
    {
      for (col = 0; col < BRK_COLS; col++)
        {
          if (b->valid && b->brick[row][col] == b->drawnbrick[row][col])
            {
              continue;
            }

          r.x = b->fx + brk_brick_x(b, col);
          r.y = b->fy + brk_brick_y(row);
          r.w = b->bw;
          r.h = BRK_BH;
          brk_dirty(b, r);
          b->drawnbrick[row][col] = b->brick[row][col];
        }
    }

  /* The message */

  if (msg != b->drawntext || !b->valid)
    {
      brk_dirty(b, b->drawnmsg);
      memset(&b->drawnmsg, 0, sizeof(b->drawnmsg));
      if (msg != NULL)
        {
          b->drawnmsg.w = 320;
          b->drawnmsg.h = 80;
          b->drawnmsg.x = b->fx + (b->fw - 320) / 2;
          b->drawnmsg.y = b->fy + brk_brick_y(BRK_ROWS) + 60;
          brk_dirty(b, b->drawnmsg);
        }

      b->drawntext = msg;
    }

  for (i = 0; i < b->ndirty; i++)
    {
      brk_paint(w, b->dirty[i]);
    }

  if (b->valid && (b->drawnscore != b->score || b->drawnlives != b->lives ||
                   b->drawnlevel != b->level))
    {
      ui_flush(hwnd, o, 64);
      hud_draw(b, o);
    }

  b->valid = true;
  ops_draw(hwnd, o);
}

static bool brk_parts_live(struct breakout_s *b)
{
  int i;

  for (i = 0; i < BRK_NPART; i++)
    {
      if (b->part[i].size != 0 && b->part[i].age < BRK_PART_MS)
        {
          return true;
        }
    }

  return false;
}

static void brk_frames(struct desk_win_s *w)
{
  struct breakout_s *b = w->priv;

  w->frames = b->state == ST_PLAY ||
              (b->state != ST_PAUSED && brk_parts_live(b));
}

static void brk_frame(struct desk_win_s *w)
{
  struct breakout_s *b = w->priv;
  uint32_t now = desk_ms();
  int dt = now - b->last;

  b->last = now;
  brk_step(b, dt > 50 ? 50 : dt);
  brk_render(w);
  brk_frames(w);
}

/* Something happened outside the frames: catch up, draw, and ask for
 * frames if the game now moves
 */

static void brk_kick(struct desk_win_s *w)
{
  struct breakout_s *b = w->priv;

  if (!w->frames)
    {
      b->last = desk_ms();
      brk_step(b, 0);
      brk_render(w);
    }

  brk_frames(w);
}

static void brk_pause(struct desk_win_s *w, bool pause)
{
  struct breakout_s *b = w->priv;

  if (pause && (b->state == ST_PLAY || b->state == ST_SERVE))
    {
      b->prevstate = b->state;
      b->state     = ST_PAUSED;
    }
  else if (!pause && b->state == ST_PAUSED)
    {
      b->state = b->prevstate;
      b->last  = desk_ms();
    }

  w->frames = false;
  brk_kick(w);
}

/* The paddle to x (in the field), within it */

static void brk_paddle_to(struct breakout_s *b, float x)
{
  float half = BRK_PADDLE_W / 2;

  b->px = x < half ? half : x > b->fw - half ? b->fw - half : x;
}

static void brk_press(struct desk_win_s *w)
{
  struct breakout_s *b = w->priv;

  switch (b->state)
    {
      case ST_SERVE:
        brk_launch(b);
        break;

      case ST_PAUSED:
        brk_pause(w, false);
        return;

      case ST_OVER:
        brk_new(b);
        break;

      default:
        break;
    }

  brk_kick(w);
}

static void brk_mouse(struct desk_win_s *w, int x, int y, uint8_t buttons)
{
  struct breakout_s *b = w->priv;
  bool press = (buttons & NX_MOUSE_LEFTBUTTON) &&
               !(b->buttons & NX_MOUSE_LEFTBUTTON);

  b->buttons = buttons;
  if (x < 0 || y < 0)
    {
      return;                           /* Left the window */
    }

  if (b->state == ST_PLAY || b->state == ST_SERVE)
    {
      brk_paddle_to(b, x - b->fx);
    }

  if (press)
    {
      brk_press(w);
    }
  else
    {
      brk_kick(w);
    }
}

static void brk_key(struct desk_win_s *w, const uint8_t *ch, int n)
{
  struct breakout_s *b = w->priv;
  int keys[16];
  int nk = ui_keys(&b->keys, ch, n, keys, 16);
  int i;

  for (i = 0; i < nk; i++)
    {
      switch (keys[i] & ~KEY_SHIFT)
        {
          case KEY_LEFT:
            if (b->state == ST_PLAY || b->state == ST_SERVE)
              {
                brk_paddle_to(b, b->px - 30);
              }
            break;

          case KEY_RIGHT:
            if (b->state == ST_PLAY || b->state == ST_SERVE)
              {
                brk_paddle_to(b, b->px + 30);
              }
            break;

          case ' ':
            if (b->state != ST_PLAY)
              {
                brk_press(w);
              }
            break;

          case 'p':
          case 'P':
          case KEY_ESC:
            brk_pause(w, b->state != ST_PAUSED);
            break;

          case '\r':
          case '\n':
          case KEY_F2:
            if (b->state == ST_OVER || keys[i] == KEY_F2)
              {
                brk_new(b);
              }
            else if (b->state == ST_PAUSED)
              {
                brk_pause(w, false);
              }
            break;

          default:
            break;
        }
    }

  brk_kick(w);
}

static void brk_focus(struct desk_win_s *w)
{
  if (!w->active)
    {
      brk_pause(w, true);
    }
}

static void brk_draw(struct desk_win_s *w)
{
  struct breakout_s *b = w->priv;

  if (b->cw != w->csize.w || b->ch != w->csize.h)
    {
      brk_layout(w);
    }

  b->valid = false;
  brk_render(w);
}

static void brk_resize(struct desk_win_s *w)
{
  brk_layout(w);
  brk_step(w->priv, 0);
  brk_render(w);
}

static void brk_close(struct desk_win_s *w)
{
  w->frames = false;
  desk_surface_close(g_brk_sprite);
  g_brk_sprite  = NULL;
  g_brk_spritew = 0;
}

#endif /* !DESK_PROGRAM_STUB */

/* The icon: bricks, a ball and a paddle */

static void brk_icon(struct desk_ops_s *o, int x, int y, int s, uint8_t a)
{
  int bw = s / 5;
  int bh = s / 12;
  int r;
  int c;

  icon_tile(o, x, y, s, RGB(0x30, 0x34, 0x5c), RGB(0x10, 0x10, 0x24), a);
  for (r = 0; r < 3; r++)
    {
      for (c = 0; c < 4; c++)
        {
          ops_rrect(o, x + s / 8 + c * (bw + 2), y + s / 6 + r * (bh + 2),
                    bw, bh, LUT_R3, bh / 2 < 3 ? bh / 2 : 3,
                    g_brk_colour[r * 2], (uint8_t)(a * 220 / 255));
        }
    }

  ops_disc(o, x + s * 3 / 5, y + s * 3 / 5, LUT_R3, 3, C_WHITE, a);
  ops_rrect(o, x + s / 3, y + s * 4 / 5, s / 3, s / 14 < 3 ? 3 : s / 14,
            LUT_R3, 1, C_WHITE, a);
}

#ifndef DESK_PROGRAM_STUB
static struct desk_win_s *brk_open(void)
{
  struct desk_win_s *w;
  struct breakout_s *b;

  b = calloc(1, sizeof(*b));
  if (b == NULL)
    {
      return NULL;
    }

  w = desk_win_open(APP_BREAKOUT, "Breakout", BRK_W, BRK_H);
  if (w == NULL)
    {
      free(b);
      return NULL;
    }

  w->priv = b;
  b->seed = desk_seed() * 2654435761u;
  brk_layout(w);
  brk_new(b);
  brk_step(b, 0);
  brk_render(w);
  return w;
}

#endif /* !DESK_PROGRAM_STUB */

/****************************************************************************
 * Public Data
 ****************************************************************************/

#ifdef DESK_PROGRAM_STUB
DESK_PROGRAM_APP(g_app_breakout, "Breakout", brk_icon, "breakout", true);
#else
const struct desk_app_s g_app_breakout =
{
  .name   = "Breakout",
  .icon   = brk_icon,
  .open   = brk_open,
  .draw   = brk_draw,
  .resize = brk_resize,
  .mouse  = brk_mouse,
  .key    = brk_key,
  .close  = brk_close,
  .frame  = brk_frame,
  .focus  = brk_focus,
  .single = true,
};
#endif
