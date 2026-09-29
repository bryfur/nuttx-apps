/****************************************************************************
 * apps/graphics/fpcdesk/desk_chess_pieces.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Chess's pieces, drawn on the CPU once for each size into coverage masks
 * (desk_chess.h), which the GPU then blends in the pieces' colours.
 *
 * Each piece is a few parts (a base, a body, a collar, a head ...), each
 * a signed distance function: circles, rounded boxes, trapezoids,
 * ellipses and polygons (the knight's head, the bishop's mitre and the
 * pawn's body are cubic Bezier curves made into polygons).  A distance
 * gives exact anti-aliasing (the coverage of a pixel is how far inside
 * the edge its centre is) and outlines for free (a band inside the edge).
 * The parts are painted one over the other, as on paper, into four
 * weights a pixel: the body's colour, its shading (from the left, lit, to
 * the right), the outline where it meets the background, and the lines
 * inside (a part's outline over another part, the knight's eye, the
 * bishop's slit).  White and black pieces use the same masks in different
 * colours: black ones have their inner lines light.
 *
 * The weights become four masks to blend one after the other: mask k is
 * weight k over the weights of it and everything under it (the
 * background's included), so that the blends give exactly the painted
 * mixture.
 ****************************************************************************/

#include <math.h>
#include <string.h>

#include "desk_chess.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define R_FILL          0
#define R_SHADE         1
#define R_INK           2
#define R_DETAIL        3

#define MAXPTS          96

/****************************************************************************
 * Private Types
 ****************************************************************************/

/* A pixel being painted: units are 1/100 of the square */

struct paint_s
{
  float x;                               /* The pixel's centre */
  float y;
  float px;                              /* A pixel, in units */
  float line;                            /* The outlines' width */
  float w[4];                            /* Weights by role */
  float a;                               /* All of them */
};

struct poly_s
{
  int   n;
  float x0;                              /* Its bounding box */
  float y0;
  float x1;
  float y1;
  float pt[MAXPTS][2];
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct poly_s g_knight;
static struct poly_s g_mitre;
static struct poly_s g_pawn;
static struct poly_s g_crown;
static bool g_made;

/****************************************************************************
 * Private Functions: shapes
 ****************************************************************************/

static inline float clampf(float v, float a, float b)
{
  return v < a ? a : v > b ? b : v;
}

static inline float minf(float a, float b)
{
  return a < b ? a : b;
}

static inline float maxf(float a, float b)
{
  return a > b ? a : b;
}

static float sd_circle(float x, float y, float cx, float cy, float r)
{
  return sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)) - r;
}

static float sd_rbox(float x, float y, float cx, float cy, float hw,
                     float hh, float r)
{
  float qx = fabsf(x - cx) - hw + r;
  float qy = fabsf(y - cy) - hh + r;
  float ox = maxf(qx, 0.0f);
  float oy = maxf(qy, 0.0f);

  return sqrtf(ox * ox + oy * oy) + minf(maxf(qx, qy), 0.0f) - r;
}

/* An ellipse: close to the true distance near its edge, which is all that
 * matters here
 */

static float sd_ellipse(float x, float y, float cx, float cy, float rx,
                        float ry)
{
  float dx = x - cx;
  float dy = y - cy;
  float k0 = sqrtf((dx / rx) * (dx / rx) + (dy / ry) * (dy / ry));
  float k1 = sqrtf((dx / (rx * rx)) * (dx / (rx * rx)) +
                   (dy / (ry * ry)) * (dy / (ry * ry)));

  return k1 > 0.0f ? k0 * (k0 - 1.0f) / k1 : -minf(rx, ry);
}

/* A trapezoid centred on x = cx, from y0 (half width w0) down to y1 (w1) */

static float sd_trap(float x, float y, float cx, float y0, float y1,
                     float w0, float w1)
{
  float he = (y1 - y0) / 2.0f;
  float px = fabsf(x - cx);
  float py = (y0 + y1) / 2.0f - y;       /* Up: towards y0 */
  float r1 = w1;                         /* At py = -he */
  float r2 = w0;                         /* At py = +he */
  float k2x = r2 - r1;
  float k2y = 2.0f * he;
  float cax = px - minf(px, py < 0.0f ? r1 : r2);
  float cay = fabsf(py) - he;
  float t = clampf(((r2 - px) * k2x + (he - py) * k2y) /
                   (k2x * k2x + k2y * k2y), 0.0f, 1.0f);
  float cbx = px - r2 + k2x * t;
  float cby = py - he + k2y * t;
  float s = (cbx < 0.0f && cay < 0.0f) ? -1.0f : 1.0f;

  return s * sqrtf(minf(cax * cax + cay * cay, cbx * cbx + cby * cby));
}

static float sd_segment(float x, float y, float ax, float ay, float bx,
                        float by)
{
  float px = x - ax;
  float py = y - ay;
  float ex = bx - ax;
  float ey = by - ay;
  float t = clampf((px * ex + py * ey) / (ex * ex + ey * ey), 0.0f, 1.0f);
  float dx = px - ex * t;
  float dy = py - ey * t;

  return sqrtf(dx * dx + dy * dy);
}

static float sd_poly(const struct poly_s *pl, float x, float y)
{
  float d;
  float s = 1.0f;
  int i;
  int j;

  /* Far from its box: the distance to the box is enough */

  d = maxf(maxf(pl->x0 - x, x - pl->x1), maxf(pl->y0 - y, y - pl->y1));
  if (d > 6.0f)
    {
      return d;
    }

  d = 1e9f;
  for (i = 0, j = pl->n - 1; i < pl->n; j = i, i++)
    {
      float vix = pl->pt[i][0];
      float viy = pl->pt[i][1];
      float ex = pl->pt[j][0] - vix;
      float ey = pl->pt[j][1] - viy;
      float wx = x - vix;
      float wy = y - viy;
      float t = clampf((wx * ex + wy * ey) / (ex * ex + ey * ey), 0.0f,
                       1.0f);
      float bx = wx - ex * t;
      float by = wy - ey * t;
      bool c1 = y >= viy;
      bool c2 = y < pl->pt[j][1];
      bool c3 = ex * wy > ey * wx;

      d = minf(d, bx * bx + by * by);
      if ((c1 && c2 && c3) || (!c1 && !c2 && !c3))
        {
          s = -s;
        }
    }

  return s * sqrtf(d);
}

/* Smooth union */

static float smin(float a, float b, float k)
{
  float h = clampf(0.5f + 0.5f * (b - a) / k, 0.0f, 1.0f);

  return b + (a - b) * h - k * h * (1.0f - h);
}

/****************************************************************************
 * Private Functions: polygons from curves
 ****************************************************************************/

static void poly_start(struct poly_s *pl, float x, float y)
{
  pl->n = 0;
  pl->pt[pl->n][0] = x;
  pl->pt[pl->n][1] = y;
  pl->n++;
}

static void poly_line(struct poly_s *pl, float x, float y)
{
  if (pl->n < MAXPTS)
    {
      pl->pt[pl->n][0] = x;
      pl->pt[pl->n][1] = y;
      pl->n++;
    }
}

/* A cubic from the last point, in steps */

static void poly_curve(struct poly_s *pl, float x1, float y1, float x2,
                       float y2, float x3, float y3, int steps)
{
  float x0 = pl->pt[pl->n - 1][0];
  float y0 = pl->pt[pl->n - 1][1];
  int i;

  for (i = 1; i <= steps; i++)
    {
      float t = (float)i / steps;
      float u = 1.0f - t;

      poly_line(pl, u * u * u * x0 + 3 * u * u * t * x1 + 3 * u * t * t * x2 +
                t * t * t * x3,
                u * u * u * y0 + 3 * u * u * t * y1 + 3 * u * t * t * y2 +
                t * t * t * y3);
    }
}

/* The other half of a shape symmetric about x = 50: the points so far
 * again, mirrored, in reverse
 */

static void poly_mirror(struct poly_s *pl)
{
  int n = pl->n;
  int i;

  for (i = n - 1; i >= 0; i--)
    {
      if (pl->pt[i][0] != 50.0f || (i != 0 && i != n - 1))
        {
          poly_line(pl, 100.0f - pl->pt[i][0], pl->pt[i][1]);
        }
    }
}

static void poly_done(struct poly_s *pl)
{
  int i;

  pl->x0 = pl->x1 = pl->pt[0][0];
  pl->y0 = pl->y1 = pl->pt[0][1];
  for (i = 1; i < pl->n; i++)
    {
      pl->x0 = minf(pl->x0, pl->pt[i][0]);
      pl->x1 = maxf(pl->x1, pl->pt[i][0]);
      pl->y0 = minf(pl->y0, pl->pt[i][1]);
      pl->y1 = maxf(pl->y1, pl->pt[i][1]);
    }
}

static void shapes_make(void)
{
  struct poly_s *pl;

  /* The knight, facing left */

  pl = &g_knight;
  poly_start(pl, 73.0f, 80.0f);
  poly_curve(pl, 78.0f, 62.0f, 80.0f, 38.0f, 68.0f, 24.0f, 10);
  poly_curve(pl, 65.0f, 20.0f, 62.0f, 17.0f, 58.5f, 15.5f, 4);
  poly_line(pl, 57.5f, 6.0f);
  poly_curve(pl, 55.0f, 7.0f, 51.5f, 10.0f, 49.5f, 13.0f, 3);
  poly_curve(pl, 41.0f, 14.5f, 33.0f, 21.0f, 27.0f, 30.0f, 8);
  poly_curve(pl, 22.0f, 37.0f, 17.0f, 43.0f, 16.5f, 48.5f, 6);
  poly_curve(pl, 16.0f, 54.0f, 21.0f, 57.0f, 26.5f, 55.5f, 6);
  poly_curve(pl, 31.0f, 54.0f, 36.0f, 50.0f, 42.5f, 48.0f, 5);
  poly_curve(pl, 41.0f, 57.0f, 32.0f, 66.0f, 30.0f, 80.0f, 8);
  poly_done(pl);

  /* The bishop's mitre: the right half, then mirrored */

  pl = &g_mitre;
  poly_start(pl, 50.0f, 23.0f);
  poly_curve(pl, 57.0f, 28.0f, 67.0f, 39.0f, 66.5f, 51.0f, 10);
  poly_curve(pl, 66.0f, 60.0f, 60.0f, 65.0f, 55.0f, 66.0f, 6);
  poly_line(pl, 50.0f, 66.0f);
  poly_mirror(pl);
  poly_done(pl);

  /* The pawn's body: from the collar down to the base, flared */

  pl = &g_pawn;
  poly_start(pl, 50.0f, 50.0f);
  poly_line(pl, 57.0f, 50.0f);
  poly_curve(pl, 58.0f, 62.0f, 66.0f, 71.0f, 70.0f, 81.0f, 10);
  poly_line(pl, 50.0f, 81.0f);
  poly_mirror(pl);
  poly_done(pl);

  /* The queen's crown: five points */

  pl = &g_crown;
  poly_start(pl, 50.0f, 24.0f);
  poly_line(pl, 55.0f, 50.0f);
  poly_line(pl, 63.5f, 28.0f);
  poly_line(pl, 66.0f, 52.0f);
  poly_line(pl, 81.5f, 33.0f);
  poly_line(pl, 72.0f, 74.0f);
  poly_line(pl, 50.0f, 74.0f);
  poly_mirror(pl);
  poly_done(pl);
}

/****************************************************************************
 * Private Functions: painting
 ****************************************************************************/

static inline float cover(const struct paint_s *p, float d)
{
  return clampf(0.5f - d / p->px, 0.0f, 1.0f);
}

/* Over: a layer of coverage a whose weights are src */

static void over(struct paint_s *p, float a, const float *src)
{
  int k;

  for (k = 0; k < 4; k++)
    {
      p->w[k] = src[k] + p->w[k] * (1.0f - a);
    }

  p->a = a + p->a * (1.0f - a);
}

/* The light: from the left.  How much of the shade a point of a part
 * gets, across the part (cx its middle, hw its half width)
 */

static float shading(const struct paint_s *p, float cx, float hw)
{
  float t = (p->x - cx) / hw;

  return clampf(0.15f + t * 0.5f, 0.0f, 0.75f);
}

/* A part: its body, shaded, and its outline, inside its edge.  Where the
 * outline falls on parts already painted it is an inner line.
 */

static void part(struct paint_s *p, float d, float cx, float hw)
{
  float src[4];
  float cov = cover(p, d);
  float in;
  float ring;
  float sh;

  if (cov <= 0.0f)
    {
      return;
    }

  in   = cover(p, d + p->line);
  ring = cov - in;
  sh   = shading(p, cx, hw);
  src[R_FILL]   = in * (1.0f - sh);
  src[R_SHADE]  = in * sh;
  src[R_INK]    = ring * (1.0f - p->a);
  src[R_DETAIL] = ring * p->a;
  over(p, cov, src);
}

/* A line inside (d: the distance from its middle), cut to a part (clip) */

static void detail(struct paint_s *p, float d, float width, float clip)
{
  float src[4];
  float cov = minf(cover(p, d - width / 2.0f), cover(p, clip + p->line));

  if (cov <= 0.0f)
    {
      return;
    }

  src[R_FILL]   = 0.0f;
  src[R_SHADE]  = 0.0f;
  src[R_INK]    = 0.0f;
  src[R_DETAIL] = cov;
  over(p, cov, src);
}

static void base(struct paint_s *p, float hw)
{
  part(p, sd_rbox(p->x, p->y, 50.0f, 84.5f, hw, 5.0f, 2.5f), 50.0f, hw);
}

static void piece(struct paint_s *p, int t)
{
  float x = p->x;
  float y = p->y;
  float d;

  switch (t)
    {
      case CH_PAWN:
        part(p, sd_poly(&g_pawn, x, y), 50.0f, 20.0f);
        base(p, 23.0f);
        part(p, sd_ellipse(x, y, 50.0f, 50.5f, 14.5f, 4.3f), 50.0f, 14.0f);
        part(p, sd_circle(x, y, 50.0f, 35.0f, 12.5f), 50.0f, 12.5f);
        break;

      case CH_KNIGHT:
        d = sd_poly(&g_knight, x, y);
        part(p, d, 52.0f, 26.0f);
        detail(p, sd_ellipse(x, y, 37.5f, 28.0f, 2.8f, 2.2f), 0.0f, d);
        detail(p, sd_circle(x, y, 21.0f, 47.0f, 1.5f), 0.0f, d);

        /* The mane: along the back of the neck */

        detail(p, fminf(sd_segment(x, y, 60.0f, 19.0f, 68.5f, 30.0f),
                        fminf(sd_segment(x, y, 68.5f, 30.0f, 71.5f, 46.0f),
                              sd_segment(x, y, 71.5f, 46.0f, 69.5f,
                                         66.0f))),
               p->line * 0.9f, d);
        base(p, 27.0f);
        break;

      case CH_BISHOP:
        part(p, sd_trap(x, y, 50.0f, 68.0f, 81.0f, 9.0f, 17.0f), 50.0f,
             15.0f);
        base(p, 25.0f);
        d = sd_poly(&g_mitre, x, y);
        part(p, d, 50.0f, 17.0f);
        part(p, sd_rbox(x, y, 50.0f, 68.5f, 15.5f, 3.4f, 1.7f), 50.0f,
             15.5f);
        part(p, sd_circle(x, y, 50.0f, 18.0f, 5.2f), 50.0f, 5.2f);

        /* The slit */

        detail(p, sd_segment(x, y, 52.5f, 35.0f, 60.5f, 47.0f),
               p->line * 1.1f, d);
        break;

      case CH_ROOK:
        part(p, sd_trap(x, y, 50.0f, 38.0f, 76.0f, 14.5f, 17.5f), 50.0f,
             16.0f);
        d = maxf(sd_rbox(x, y, 50.0f, 26.5f, 21.0f, 9.0f, 1.5f),
                 -minf(sd_rbox(x, y, 38.5f, 18.5f, 3.4f, 6.0f, 0.8f),
                       sd_rbox(x, y, 61.5f, 18.5f, 3.4f, 6.0f, 0.8f)));
        part(p, d, 50.0f, 21.0f);
        part(p, sd_rbox(x, y, 50.0f, 38.5f, 18.5f, 3.2f, 1.5f), 50.0f,
             18.5f);
        part(p, sd_rbox(x, y, 50.0f, 77.0f, 22.5f, 3.6f, 1.6f), 50.0f,
             22.5f);
        base(p, 27.0f);
        break;

      case CH_QUEEN:
        d = sd_poly(&g_crown, x, y) - 0.8f;
        part(p, d, 50.0f, 24.0f);
        part(p, sd_circle(x, y, 18.0f, 31.5f, 4.8f), 18.0f, 4.8f);
        part(p, sd_circle(x, y, 36.0f, 25.5f, 4.8f), 36.0f, 4.8f);
        part(p, sd_circle(x, y, 50.0f, 21.0f, 5.2f), 50.0f, 5.2f);
        part(p, sd_circle(x, y, 64.0f, 25.5f, 4.8f), 64.0f, 4.8f);
        part(p, sd_circle(x, y, 82.0f, 31.5f, 4.8f), 82.0f, 4.8f);
        detail(p, fabsf(sd_circle(x, y, 50.0f, 124.0f, 58.0f)),
               p->line * 0.9f, d);
        part(p, sd_rbox(x, y, 50.0f, 76.5f, 23.0f, 3.6f, 1.8f), 50.0f,
             23.0f);
        base(p, 27.0f);
        break;

      default:                           /* King */
        d = smin(smin(sd_circle(x, y, 35.0f, 55.0f, 14.5f),
                      sd_circle(x, y, 65.0f, 55.0f, 14.5f), 3.0f),
                 sd_trap(x, y, 50.0f, 52.0f, 74.0f, 16.0f, 22.0f), 3.0f);
        part(p, d, 50.0f, 28.0f);
        part(p, sd_rbox(x, y, 50.0f, 45.0f, 8.0f, 11.0f, 4.0f), 50.0f,
             8.0f);
        part(p, minf(sd_rbox(x, y, 50.0f, 22.0f, 3.2f, 11.0f, 1.0f),
                     sd_rbox(x, y, 50.0f, 18.0f, 9.0f, 3.2f, 1.0f)),
             50.0f, 9.0f);
        detail(p, fabsf(sd_circle(x, y, 50.0f, 126.0f, 60.0f)),
               p->line * 0.9f, d);
        part(p, sd_rbox(x, y, 50.0f, 76.5f, 23.0f, 3.6f, 1.8f), 50.0f,
             23.0f);
        base(p, 27.0f);
        break;
    }
}

static inline uint8_t byte(float v)
{
  return (uint8_t)(clampf(v, 0.0f, 1.0f) * 255.0f + 0.5f);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

void ch_piece_masks(int t, int size, int weight, uint8_t *out)
{
  int n = size * size;
  int i;
  int j;

  if (!g_made)
    {
      shapes_make();
      g_made = true;
    }

  for (j = 0; j < size; j++)
    {
      for (i = 0; i < size; i++)
        {
          struct paint_s p;
          float below;
          int k;

          memset(&p, 0, sizeof(p));
          p.px   = 100.0f / size;
          p.x    = (i + 0.5f) * p.px;
          p.y    = (j + 0.5f) * p.px;
          p.line = p.px * (weight ? 1.15f : clampf(size * 0.03f, 1.25f,
                                                    2.6f));
          piece(&p, t);

          /* Blend k: its weight over it and all under it */

          below = 1.0f - p.a;
          for (k = 0; k < CH_MASKS; k++)
            {
              float all = below + p.w[k];

              out[k * n + j * size + i] = all > 1e-4f ?
                                          byte(p.w[k] / all) : 0;
              below = all;
            }
        }
    }
}

void ch_mark_mask(int kind, int size, uint8_t *out)
{
  float c = size / 2.0f;
  int i;
  int j;

  for (j = 0; j < size; j++)
    {
      for (i = 0; i < size; i++)
        {
          float dx = i + 0.5f - c;
          float dy = j + 0.5f - c;
          float r = sqrtf(dx * dx + dy * dy);
          float v;

          switch (kind)
            {
              case CH_MARK_DOT:
                v = clampf(size * 0.16f - r + 0.5f, 0.0f, 1.0f);
                break;

              case CH_MARK_RING:
                v = minf(clampf(c - r + 0.5f, 0.0f, 1.0f),
                         clampf(r - (c - size * 0.085f) + 0.5f, 0.0f, 1.0f));
                break;

              default:                   /* A glow, strongest inside */
                v = clampf(1.0f - r / (c * 1.05f), 0.0f, 1.0f);
                v = v * v * (3.0f - 2.0f * v);
                break;
            }

          out[j * size + i] = byte(v);
        }
    }
}
