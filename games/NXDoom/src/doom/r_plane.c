/****************************************************************************
 * apps/games/NXDoom/src/doom/r_plane.c
 *
 * SPDX-License-Identifer: GPLv2
 *
 * Copyright(C) 1993-1996 Id Software, Inc.
 * Copyright(C) 2005-2014 Simon Howard
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * DESCRIPTION:
 *  Here is a core component: drawing the floors and ceilings, while
 *  maintaining a per column clipping list only. Moreover, the sky areas
 *  have to be determined.
 *
 ****************************************************************************/

/****************************************************************************
 * Included Files
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <syslog.h>

#ifdef CONFIG_GAMES_NXDOOM_SRAM
#  include <nuttx/kmalloc.h>
#endif

#include "i_system.h"
#include "w_wad.h"
#include "z_zone.h"

#include "doomdef.h"
#include "doomstat.h"

#include "r_local.h"
#include "r_sky.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define MAXOPENINGS (SCREENWIDTH * CONFIG_GAMES_NXDOOM_MAXOPENINGS)

/****************************************************************************
 * Public Data
 ****************************************************************************/

planefunction_t floorfunc;
planefunction_t ceilingfunc;

/* opening */

/* Here comes the obnoxious "visplane". */

#ifdef CONFIG_GAMES_NXDOOM_SRAM
/* In the internal SRAM while the game runs (r_sram_planes()) if there is
 * room, else these in the PSRAM: every wall column writes its planes' top
 * and bottom, and the openings are the sprites' clipping
 */

static visplane_t g_visplanes[CONFIG_GAMES_NXDOOM_MAXVISPLANES];
static short g_openings[MAXOPENINGS];
static void *g_planes_sram;
static bool g_planes_tried;

visplane_t *visplanes = g_visplanes;
short *openings = g_openings;
#else
visplane_t visplanes[CONFIG_GAMES_NXDOOM_MAXVISPLANES];
short openings[MAXOPENINGS];
#endif
NXDOOM_SRAM visplane_t *lastvisplane;
NXDOOM_SRAM visplane_t *floorplane;
NXDOOM_SRAM visplane_t *ceilingplane;

NXDOOM_SRAM short *lastopening;

/* Clip values are the solid pixel bounding the range. floorclip starts out
 * SCREENHEIGHT ceilingclip starts out -1
 */

NXDOOM_SRAM short floorclip[SCREENWIDTH];
NXDOOM_SRAM short ceilingclip[SCREENWIDTH];

/* spanstart holds the start of a plane span initialized to 0 at start */

NXDOOM_SRAM int spanstart[SCREENHEIGHT];
int spanstop[SCREENHEIGHT];

/* texture mapping */

NXDOOM_SRAM lighttable_t **planezlight;
NXDOOM_SRAM fixed_t planeheight;

NXDOOM_SRAM fixed_t yslope[SCREENHEIGHT];
NXDOOM_SRAM fixed_t distscale[SCREENWIDTH];
NXDOOM_SRAM fixed_t basexscale;
NXDOOM_SRAM fixed_t baseyscale;

NXDOOM_SRAM fixed_t cachedheight[SCREENHEIGHT];
NXDOOM_SRAM fixed_t cacheddistance[SCREENHEIGHT];
NXDOOM_SRAM fixed_t cachedxstep[SCREENHEIGHT];
NXDOOM_SRAM fixed_t cachedystep[SCREENHEIGHT];

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/* Columns x1..x2 of a plane unused (0xff), as it takes them on */

static void r_plane_unused(visplane_t *pl, int x1, int x2)
{
  if (x2 >= x1)
    {
      memset(pl->top + x1, 0xff, x2 - x1 + 1);
    }
}

#ifdef CONFIG_GAMES_NXDOOM_SRAM
static void r_free_sram_planes(void)
{
  visplanes = g_visplanes;
  openings = g_openings;
  kmm_free(g_planes_sram);
  g_planes_sram = NULL;
}

/* The visplanes and the openings into the SRAM, the first frame (after
 * the screen buffer and the light tables, which matter more, have had it)
 */

static void r_sram_planes(void)
{
  size_t vsize = sizeof(g_visplanes);
  size_t size = vsize + sizeof(g_openings);

  /* (Zeroed as the arrays were: a plane's bottom is never cleared, and
   * a column it has none of in a frame, top 0xff, must not be drawn)
   */

  g_planes_tried = true;
  g_planes_sram = kmm_zalloc(size);
  if (g_planes_sram == NULL)
    {
      syslog(LOG_INFO, "nxdoom: no SRAM for the visplanes (%u bytes)\n",
             (unsigned)size);
      return;
    }

  visplanes = g_planes_sram;
  openings = (short *)((byte *)g_planes_sram + vsize);
  i_at_exit(r_free_sram_planes, true);
  syslog(LOG_INFO, "nxdoom: visplanes and openings in the SRAM (%u bytes)\n",
         (unsigned)size);
}
#endif

/* Uses global vars:
 *  planeheight
 *  ds_source
 *  basexscale
 *  baseyscale
 *  viewx
 *  viewy
 *
 * BASIC PRIMITIVE
 */

NXDOOM_FAST static void r_map_plane(int y, int x1, int x2)
{
  angle_t angle;
  fixed_t distance;
  fixed_t length;
  unsigned index;

  /* Ensure array indices are in range before access. */

  if (x2 < x1 || x1 < 0 || x2 >= viewwidth)
    {
      return;
    }

  if (y < 0)
    {
      y = 0;
    }
  else if (y >= viewheight)
    {
      y = viewheight - 1;
    }

  if (planeheight != cachedheight[y])
    {
      cachedheight[y] = planeheight;
      distance = cacheddistance[y] = fixed_mul(planeheight, yslope[y]);
      ds_xstep = cachedxstep[y] = fixed_mul(distance, basexscale);
      ds_ystep = cachedystep[y] = fixed_mul(distance, baseyscale);
    }
  else
    {
      distance = cacheddistance[y];
      ds_xstep = cachedxstep[y];
      ds_ystep = cachedystep[y];
    }

  length = fixed_mul(distance, distscale[x1]);
  angle = (viewangle + xtoviewangle[x1]) >> ANGLETOFINESHIFT;
  ds_xfrac = viewx + fixed_mul(finecosine[angle], length);
  ds_yfrac = -viewy - fixed_mul(finesine[angle], length);

  if (fixedcolormap)
    {
      ds_colormap = fixedcolormap;
    }
  else
    {
      index = distance >> LIGHTZSHIFT;

      if (index >= MAXLIGHTZ)
        {
          index = MAXLIGHTZ - 1;
        }

      ds_colormap = planezlight[index];
    }

  ds_y = y;
  ds_x1 = x1;
  ds_x2 = x2;

  /* high or low detail */

  spanfunc();
}

static inline boolean r_row_in_range(int row)
{
  return row >= 0 && row < SCREENHEIGHT;
}

NXDOOM_FAST static void r_make_spans(int x, int t1, int b1, int t2, int b2)
{
  /* Check that row is in range before indexing arrays. */

  while (t1 < t2 && t1 <= b1)
    {
      r_map_plane(t1, r_row_in_range(t1) ? spanstart[t1] : 0, x - 1);
      t1++;
    }
  while (b1 > b2 && b1 >= t1)
    {
      r_map_plane(b1, r_row_in_range(b1) ? spanstart[b1] : 0, x - 1);
      b1--;
    }

  while (t2 < t1 && t2 <= b2)
    {
      if (r_row_in_range(t2))
        {
          spanstart[t2] = x;
        }

      t2++;
    }
  while (b2 > b1 && b2 >= t2)
    {
      if (r_row_in_range(b2))
        {
          spanstart[b2] = x;
        }

      b2--;
    }
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/* r_init_planes
 * Only at game startup.
 */

void r_init_planes(void)
{
  /* Doh! */
}

/* r_clear_planes
 * At beginning of frame.
 */

void r_clear_planes(void)
{
  int i;
  angle_t angle;

#ifdef CONFIG_GAMES_NXDOOM_SRAM
  if (!g_planes_tried)
    {
      r_sram_planes();
    }
#endif

  /* opening / clipping determination */

  for (i = 0; i < viewwidth; i++)
    {
      floorclip[i] = viewheight;
      ceilingclip[i] = -1;
    }

  lastvisplane = visplanes;
  lastopening = openings;

  /* texture calculation */

  memset(cachedheight, 0, sizeof(cachedheight));

  /* left to right mapping */

  angle = (viewangle - ANG90) >> ANGLETOFINESHIFT;

  /* scale will be unit scale at SCREENWIDTH/2 distance */

  basexscale = fixed_div(finecosine[angle], centerxfrac);
  baseyscale = -fixed_div(finesine[angle], centerxfrac);
}

visplane_t *r_find_plane(fixed_t height, int picnum, int lightlevel)
{
  visplane_t *check;

  if (picnum == skyflatnum)
    {
      height = 0; /* all skys map together */
      lightlevel = 0;
    }

  for (check = visplanes; check < lastvisplane; check++)
    {
      if (height == check->height && picnum == check->picnum &&
          lightlevel == check->lightlevel)
        {
          break;
        }
    }

  if (check < lastvisplane)
    {
      return check;
    }

  if (lastvisplane - visplanes == CONFIG_GAMES_NXDOOM_MAXVISPLANES)
    {
      i_error("r_find_plane: no more visplanes");
    }

  lastvisplane++;

  check->height = height;
  check->picnum = picnum;
  check->lightlevel = lightlevel;
  check->minx = SCREENWIDTH;
  check->maxx = -1;

  /* (No columns yet: r_check_plane() marks them unused, 0xff, as the
   * plane takes them; a full-width memset per plane was 3 % of a frame)
   */

  return check;
}

visplane_t *r_check_plane(visplane_t *pl, int start, int stop)
{
  int intrl;
  int intrh;
  int unionl;
  int unionh;
  int x;

  if (start < pl->minx)
    {
      intrl = pl->minx;
      unionl = start;
    }
  else
    {
      unionl = pl->minx;
      intrl = start;
    }

  if (stop > pl->maxx)
    {
      intrh = pl->maxx;
      unionh = stop;
    }
  else
    {
      unionh = pl->maxx;
      intrh = stop;
    }

  for (x = intrl; x <= intrh; x++)
    {
      if (pl->top[x] != 0xff)
        {
          break;
        }
    }

  if (x > intrh)
    {
      /* The columns it takes on (with any gap between the old and the new
       * range) unused: only top[minx..maxx] is ever read
       */

      if (pl->minx > pl->maxx)
        {
          r_plane_unused(pl, start, stop);
        }
      else
        {
          r_plane_unused(pl, unionl, pl->minx - 1);
          r_plane_unused(pl, pl->maxx + 1, unionh);
        }

      pl->minx = unionl;
      pl->maxx = unionh;

      return pl; /* use the same one */
    }

  /* make a new visplane */

  if (lastvisplane - visplanes == CONFIG_GAMES_NXDOOM_MAXVISPLANES)
    {
      i_error("r_check_plane: no more visplanes");
    }

  lastvisplane->height = pl->height;
  lastvisplane->picnum = pl->picnum;
  lastvisplane->lightlevel = pl->lightlevel;

  pl = lastvisplane++;
  pl->minx = start;
  pl->maxx = stop;

  r_plane_unused(pl, start, stop);

  return pl;
}

/* At the end of each frame. */

NXDOOM_FAST void r_draw_planes(void)
{
  visplane_t *pl;
  int light;
  int x;
  int stop;
  int angle;
  int lumpnum;

#ifdef CONFIG_GAMES_NXDOOM_RANGECHECK
  if (ds_p - drawsegs > CONFIG_GAMES_NXDOOM_MAXDRAWSEGS)
    {
      i_error("r_draw_planes: drawsegs overflow (%td)", ds_p - drawsegs);
    }

  if (lastvisplane - visplanes > CONFIG_GAMES_NXDOOM_MAXVISPLANES)
    {
      i_error("r_draw_planes: visplane overflow (%td)",
        lastvisplane - visplanes);
    }

  if (lastopening - openings > MAXOPENINGS)
    {
      i_error("r_draw_planes: opening overflow (%td)",
        lastopening - openings);
    }
#endif

  for (pl = visplanes; pl < lastvisplane; pl++)
    {
      if (pl->minx > pl->maxx)
        {
          continue;
        }

      /* sky flat */

      if (pl->picnum == skyflatnum)
        {
          dc_iscale = pspriteiscale >> detailshift;

          /* Sky is always drawn full bright, i.e. colormaps[0] is used.
           * Because of this hack, sky is not affected by INVUL inverse
           * mapping.
           */

          dc_colormap = colormaps;
          dc_texturemid = skytexturemid;
          for (x = pl->minx; x <= pl->maxx; x++)
            {
              dc_yl = pl->top[x];
              dc_yh = pl->bottom[x];

              if (dc_yl <= dc_yh)
                {
                  angle = (viewangle + xtoviewangle[x]) >> ANGLETOSKYSHIFT;
                  dc_x = x;
                  dc_source = r_get_column(skytexture, angle);
                  colfunc();
                }
            }

          continue;
        }

      /* regular flat */

      lumpnum = firstflat + flattranslation[pl->picnum];
      ds_source = w_cache_lump_num(lumpnum, PU_STATIC);

      planeheight = abs(pl->height - viewz);
      light = (pl->lightlevel >> LIGHTSEGSHIFT) + extralight;

      if (light >= LIGHTLEVELS)
        {
          light = LIGHTLEVELS - 1;
        }

      if (light < 0)
        {
          light = 0;
        }

      planezlight = zlight[light];

      pl->top[pl->maxx + 1] = 0xff;
      pl->top[pl->minx - 1] = 0xff;

      stop = pl->maxx + 1;

      for (x = pl->minx; x <= stop; x++)
        {
          r_make_spans(x, pl->top[x - 1], pl->bottom[x - 1], pl->top[x],
                       pl->bottom[x]);
        }

      w_release_lump_num(lumpnum);
    }
}
