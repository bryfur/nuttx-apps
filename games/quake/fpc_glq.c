/****************************************************************************
 * apps/games/quake/fpc_glq.c
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Quake's view drawn by the 3D GPU through fpcgl (-gl; docs/gpu3d.md),
 * the software renderer's decisions kept where they matter:
 *
 *   - The world: the BSP walked front to back through the leaves the PVS
 *     marks (R_MarkLeaves), nodes culled by the view frustum, surfaces by
 *     the side of their plane; each a polygon with the depth test.
 *   - Its surfaces drawn as GLQuake draws them, in passes: the texture's
 *     mip (D_MipLevelForScale's; every mip uploaded at the map's load,
 *     through the palette, resampled to powers of two to repeat), then
 *     the surface's lightmap blended on (twice the product: Quake's
 *     overbright), then its fullbright texels (224 and up) keyed over.
 *     The lightmap: the light grid as R_BuildLightMap makes it (the
 *     styles, the dynamic lights) interpolated as the software renderer
 *     does, 4 texels to a light block (2, 1 for the far mips), the
 *     colormap's brightness per level, dithered; kept in an atlas and
 *     made again when its lighting changes (as D_CacheSurface decides).
 *   - Water and the sky: cut into small polygons (as GLQuake does), their
 *     textures repeated, warped (the vertices' texture coordinates as the
 *     software renderer's turbulence) or scrolled with the view's
 *     direction, the sky's keyed front layer over its back one.
 *   - Models: their frames' triangles lit per vertex as R_AliasSetupLighting
 *     does, the light level through the colormap's brightness (the factors
 *     BUILD uses); the gun drawn in front (a depth range of its own).
 *     Brush models as the world, sprites as quads facing the view, the
 *     particles (which the game still moves) as small quads.
 *   - The status bar, the console, the menus: drawn in the 8-bit frame as
 *     before, on a view cleared to 255, and put over the picture keyed
 *     (fpc_quake.c); the damage, bonus and powerup tints as one blend.
 ****************************************************************************/

#include "quakedef.h"
#include "r_local.h"
#include "d_local.h"

#include <syslog.h>

#include "fpcgl.h"
#include "fpc_gpu.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define ATLAS_W      1024                 /* The lightmap atlas (the most) */
#define ATLAS_H      1024
#define SHELVES      256
#define MAXTEX       512                  /* A map's textures */
#define MAXSKINS     96                   /* Models' skins kept as textures */
#define MAXSPRITES   64                   /* Sprite frames kept as textures */
#define MAXPARTS     2048                 /* Particles in a frame */
#define CACHES       4096                 /* Lightmaps in the atlas */

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct gcache_s                           /* A surface's lightmap in the atlas */
{
  msurface_t *surf;
  int16_t x;
  int16_t y;
  int16_t w;
  int16_t h;
  int lightadj[MAXLIGHTMAPS];
  qboolean dlight;
  uint8_t mip;
};

struct shelf_s
{
  int16_t y;
  int16_t h;
  int16_t x;
};

/****************************************************************************
 * Public Data
 ****************************************************************************/

qboolean fpcq_gl;                         /* -gl: the 3D GPU draws */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct
{
  qboolean on;
  int16_t ax;                             /* The atlas */
  int16_t ay;
  int16_t aw;
  int16_t ah;
  int nshelf;
  int top;
  struct shelf_s shelf[SHELVES];
  int ncache;
  struct gcache_s cache[CACHES];

  /* The map's textures: each mip, its fullbright texels (0: none), water
   * and the sky
   */

  model_t *world;
  int ntex;
  GLuint mtex[MAXTEX][MIPLEVELS];
  GLuint fbtex[MAXTEX][MIPLEVELS];
  GLuint gltex[MAXTEX];
  GLuint skyfront;                        /* The sky's front layer (keyed) */

  /* Models' skins and sprite frames as textures */

  struct
  {
    const void *key;
    GLuint t;
    int w;
    int h;
  } skin[MAXSKINS], sprite[MAXSPRITES];
  int nskin;
  int nsprite;

  /* The palette (the textures', 255 the key), the colormap's brightness
   * per light level (the lightmaps' and the models' shades)
   */

  uint16_t pal[256];
  uint8_t factor[64];
  qboolean palok;

  /* This frame's particles */

  int npart;
  struct
  {
    vec3_t org;
    uint16_t colour;
  } part[MAXPARTS];

  /* The view frustum's planes (culling) */

  mplane_t frustum[4];
  uint32_t surfs, lightmaps, flushes, frames;
} g_gl;

/* (fpc_quake.c's) */

const unsigned short *fpcq_base_lut(void);
void fpcq_tint(uint16_t *colour, int *alpha);

/* The software renderer's own (r_surf.c, r_alias.c, r_sprite.c, r_main.c,
 * mathlib.c)
 */

#define LIGHT_MIN    5                    /* (r_alias.c's) */

extern unsigned blocklights[18 * 18];
extern float r_avertexnormals[162][3];
extern cvar_t r_drawviewmodel;
void R_BuildLightMap(void);
mspriteframe_t *R_GetSpriteframe(msprite_t *psprite);
void RotatePointAroundVector(vec3_t dst, const vec3_t dir,
                             const vec3_t point, float degrees);

/****************************************************************************
 * The atlas
 ****************************************************************************/

static qboolean atlas_alloc(int w, int h, int16_t *x, int16_t *y)
{
  int i;

  w = (w + 1) & ~1;
  for (i = 0; i < g_gl.nshelf; i++)
    {
      struct shelf_s *s = &g_gl.shelf[i];

      if (s->h >= h && s->h <= h + h / 2 + 2 && s->x + w <= g_gl.aw)
        {
          *x = g_gl.ax + s->x;
          *y = g_gl.ay + s->y;
          s->x += w;
          return true;
        }
    }

  if (g_gl.nshelf < SHELVES && g_gl.top + h <= g_gl.ah)
    {
      struct shelf_s *s = &g_gl.shelf[g_gl.nshelf++];

      s->y = g_gl.top;
      s->h = h;
      s->x = w;
      g_gl.top += h;
      *x = g_gl.ax;
      *y = g_gl.ay + s->y;
      return true;
    }

  return false;
}

/* Empty: every surface's block forgotten (built again as used) */

static void atlas_flush(void)
{
  int i;

  for (i = 0; i < g_gl.ncache; i++)
    {
      struct gcache_s *c = &g_gl.cache[i];

      if (c->surf != NULL &&
          c->surf->cachespots[c->mip] == (struct surfcache_s *)c)
        {
          c->surf->cachespots[c->mip] = NULL;
        }
    }

  g_gl.ncache = 0;
  g_gl.nshelf = 0;
  g_gl.top = 0;
  g_gl.flushes++;
}

/****************************************************************************
 * Palette, light factors
 ****************************************************************************/

/* The colormap's brightness per light level (BUILD's factors and the
 * models' shades): its row's colours against the palette's, 1.7
 */

static void make_factors(void)
{
  const uint8_t *pal = host_basepal;
  int level;

  for (level = 0; level < 64; level++)
    {
      const uint8_t *row = host_colormap + level * 256;
      int32_t num = 0;
      int32_t den = 0;
      int32_t f;
      int i;

      for (i = 0; i < 224; i++)
        {
          num += pal[3 * row[i]] + pal[3 * row[i] + 1] + pal[3 * row[i] + 2];
          den += pal[3 * i] + pal[3 * i + 1] + pal[3 * i + 2];
        }

      f = den > 0 ? (num * 128 + den / 2) / den : 128;
      g_gl.factor[level] = (uint8_t)(f > 255 ? 255 : f);
    }
}

static void table_check(void)
{
  const unsigned short *lut = fpcq_base_lut();

  if (lut == NULL)
    {
      return;
    }

  if (!g_gl.palok || memcmp(lut, g_gl.pal, sizeof(g_gl.pal)) != 0)
    {
      uint16_t tpal[256];

      memcpy(g_gl.pal, lut, sizeof(g_gl.pal));
      memcpy(tpal, g_gl.pal, sizeof(tpal));
      tpal[255] = FGL_KEY;                /* (the textures': 255 the key) */
      fglColorTable(tpal);
      g_gl.palok = true;
    }
}

/****************************************************************************
 * Init, a new map
 ****************************************************************************/

void fpcq_gl_init(void)
{
  g_gl.on = true;
  make_factors();
}

static void free_map(void)
{
  /* Everything fpcgl holds is the game's: given back at once */

  fglReset();
  memset(g_gl.mtex, 0, sizeof(g_gl.mtex));
  memset(g_gl.fbtex, 0, sizeof(g_gl.fbtex));
  memset(g_gl.gltex, 0, sizeof(g_gl.gltex));
  g_gl.skyfront = 0;
  g_gl.ntex = 0;
  g_gl.nskin = 0;
  g_gl.nsprite = 0;
  g_gl.aw = 0;
  g_gl.palok = false;                     /* (sent again with the textures) */
}

/* A w x h 8-bit image as a texture through the palette, resampled to
 * powers of two (repeating; the nearest texel), fullbright: only its
 * texels 224 and up (the others 255: the key), 0 if it has none
 */

static GLuint mip_texture(const uint8_t *src, int w, int h, qboolean fullbright)
{
  static uint8_t img[256 * 256];
  int w2 = 1;
  int h2 = 1;
  qboolean any = false;
  GLuint t;
  int x;
  int y;

  while (w2 < w)
    {
      w2 <<= 1;
    }

  while (h2 < h)
    {
      h2 <<= 1;
    }

  if (w2 * h2 > (int)sizeof(img))
    {
      return 0;
    }

  for (y = 0; y < h2; y++)
    {
      for (x = 0; x < w2; x++)
        {
          uint8_t p = src[(y * h / h2) * w + x * w / w2];

          if (fullbright)
            {
              any |= p >= 224 && p != 255;
              p = p >= 224 ? p : 255;
            }

          img[y * w2 + x] = p;
        }
    }

  if (fullbright && !any)
    {
      return 0;
    }

  glGenTextures(1, &t);
  glBindTexture(GL_TEXTURE_2D, t);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_COLOR_INDEX, w2, h2, 0, GL_COLOR_INDEX,
               GL_UNSIGNED_BYTE, img);
  return t;
}

/* R_NewMap: the map's textures into the GPU (every mip of the surfaces'
 * with their fullbright texels, water and the sky), then the lightmaps'
 * atlas in what is left
 */

void fpcq_gl_newmap(void)
{
  model_t *m = cl.worldmodel;
  int i;
  int k;

  if (!g_gl.on)
    {
      return;
    }

  g_gl.ncache = 0;                        /* (the old map's surfaces are gone) */
  free_map();
  g_gl.world = m;
  table_check();
  for (i = 0; i < m->numtextures && i < MAXTEX; i++)
    {
      texture_t *t = m->textures[i];

      g_gl.ntex = i + 1;
      if (t == NULL)
        {
          continue;
        }

      if (!strncmp(t->name, "sky", 3))
        {
          /* The sky: its back layer (the right half) and its front one
           * (the left half, 0 transparent: 255, the key) as textures
           */

          static uint8_t layer[128 * 128];
          const uint8_t *src = (const uint8_t *)t + t->offsets[0];
          int h = t->height < 128 ? t->height : 128;
          int r;
          int c;

          for (r = 0; r < h; r++)
            {
              memcpy(layer + r * 128, src + r * t->width + 128, 128);
            }

          glGenTextures(1, &g_gl.gltex[i]);
          glBindTexture(GL_TEXTURE_2D, g_gl.gltex[i]);
          glTexImage2D(GL_TEXTURE_2D, 0, GL_COLOR_INDEX, 128, h, 0,
                       GL_COLOR_INDEX, GL_UNSIGNED_BYTE, layer);
          for (r = 0; r < h; r++)
            {
              for (c = 0; c < 128; c++)
                {
                  uint8_t p = src[r * t->width + c];

                  layer[r * 128 + c] = p == 0 ? 255 : p;
                }
            }

          if (g_gl.skyfront == 0)
            {
              glGenTextures(1, &g_gl.skyfront);
            }

          glBindTexture(GL_TEXTURE_2D, g_gl.skyfront);
          glTexImage2D(GL_TEXTURE_2D, 0, GL_COLOR_INDEX, 128, h, 0,
                       GL_COLOR_INDEX, GL_UNSIGNED_BYTE, layer);
        }
      else if (t->name[0] == '*')
        {
          /* Water, slime, lava: the texture itself, repeated and warped */

          glGenTextures(1, &g_gl.gltex[i]);
          glBindTexture(GL_TEXTURE_2D, g_gl.gltex[i]);
          glTexImage2D(GL_TEXTURE_2D, 0, GL_COLOR_INDEX, t->width, t->height,
                       0, GL_COLOR_INDEX, GL_UNSIGNED_BYTE,
                       (const uint8_t *)t + t->offsets[0]);
        }
      else
        {
          for (k = 0; k < MIPLEVELS; k++)
            {
              int w = t->width >> k;
              int h = t->height >> k;

              if (w < 1 || h < 1)
                {
                  continue;
                }

              g_gl.mtex[i][k] = mip_texture((const uint8_t *)t + t->offsets[k],
                                            w, h, false);
              g_gl.fbtex[i][k] = mip_texture((const uint8_t *)t +
                                             t->offsets[k], w, h, true);
              if (g_gl.mtex[i][k] == 0 || glGetError() != GL_NO_ERROR)
                {
                  syslog(LOG_WARNING, "quake: no room for %s's mips\n",
                         t->name);
                }
            }
        }
    }

  /* The lightmaps' atlas: as tall as the space has room for */

  for (k = ATLAS_H; k >= 256; k -= 64)
    {
      if (fglSpaceAlloc(ATLAS_W, k, &g_gl.ax, &g_gl.ay) == 0)
        {
          g_gl.aw = ATLAS_W;
          g_gl.ah = k;
          break;
        }
    }

  g_gl.nshelf = 0;
  g_gl.top = 0;
  syslog(LOG_INFO, "quake: -gl: %d textures, the lightmaps' atlas %d x %d\n",
         g_gl.ntex, g_gl.aw, g_gl.ah);
}

/****************************************************************************
 * Surfaces
 ****************************************************************************/

static int texture_index(texture_t *t)
{
  int i;

  for (i = 0; i < g_gl.ntex; i++)
    {
      if (g_gl.world->textures[i] == t)
        {
          return i;
        }
    }

  return -1;
}

/* The surface's block at mip level mip in the atlas, built if it is not
 * there or its lighting or texture frame changed (D_CacheSurface's test)
 */

/* A surface's lightmap at mip (4 texels a light block, 2 at mip 1, 1
 * beyond), made again when its lighting changed (D_CacheSurface's test):
 * the light grid as R_BuildLightMap makes it (8.8, its level the
 * colormap's row), interpolated across each block as the software
 * renderer does, the level's brightness (1.7: 128 is 1) as grey, dithered
 * to RGB565
 */

static const uint8_t g_bayer[4][4] =
{
  { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 }
};

static struct gcache_s *lightmap_block(msurface_t *s, int mip)
{
  static uint16_t lm[68 * 68];
  struct gcache_s *c = (struct gcache_s *)s->cachespots[mip];
  int sc = mip == 0 ? 4 : mip == 1 ? 2 : 1;
  int lw = (s->extents[0] >> 4) + 1;
  int lh = (s->extents[1] >> 4) + 1;
  int w = (lw - 1) * sc;
  int h = (lh - 1) * sc;
  int x;
  int y;
  int i;

  r_drawsurf.lightadj[0] = d_lightstylevalue[s->styles[0]];
  r_drawsurf.lightadj[1] = d_lightstylevalue[s->styles[1]];
  r_drawsurf.lightadj[2] = d_lightstylevalue[s->styles[2]];
  r_drawsurf.lightadj[3] = d_lightstylevalue[s->styles[3]];
  if (c != NULL && !c->dlight && s->dlightframe != r_framecount &&
      c->lightadj[0] == r_drawsurf.lightadj[0] &&
      c->lightadj[1] == r_drawsurf.lightadj[1] &&
      c->lightadj[2] == r_drawsurf.lightadj[2] &&
      c->lightadj[3] == r_drawsurf.lightadj[3])
    {
      return c;
    }

  if (w < 1 || h < 1 || w > 68 || h > 68)
    {
      return NULL;
    }

  if (c == NULL)
    {
      if (g_gl.ncache == CACHES)
        {
          atlas_flush();
        }

      c = &g_gl.cache[g_gl.ncache];
      if (!atlas_alloc(w, h, &c->x, &c->y))
        {
          atlas_flush();
          c = &g_gl.cache[0];
          if (!atlas_alloc(w, h, &c->x, &c->y))
            {
              return NULL;
            }
        }

      g_gl.ncache++;
      c->surf = s;
      c->mip = (uint8_t)mip;
      c->w = (int16_t)w;
      c->h = (int16_t)h;
      s->cachespots[mip] = (struct surfcache_s *)c;
    }

  c->dlight = s->dlightframe == r_framecount;
  for (i = 0; i < MAXLIGHTMAPS; i++)
    {
      c->lightadj[i] = r_drawsurf.lightadj[i];
    }

  r_drawsurf.surf = s;
  R_BuildLightMap();
  for (y = 0; y < h; y++)
    {
      /* Texel (x, y) at (x + 0.5, y + 0.5) / sc blocks */

      int fy = (2 * y + 1) * 256 / (2 * sc);
      int by = fy >> 8;
      int ty = fy & 255;

      for (x = 0; x < w; x++)
        {
          int fx = (2 * x + 1) * 256 / (2 * sc);
          int bx = fx >> 8;
          int tx = fx & 255;
          const unsigned *b = &blocklights[by * lw + bx];
          int top = (int)b[0] + (((int)b[1] - (int)b[0]) * tx >> 8);
          int bot = (int)b[lw] + (((int)b[lw + 1] - (int)b[lw]) * tx >> 8);
          int light = top + ((bot - top) * ty >> 8);
          int level = light >> 8;
          int f;
          int r5;
          int g6;

          level = level < 0 ? 0 : level > 63 ? 63 : level;
          f = g_gl.factor[level];
          r5 = (f + (g_bayer[y & 3][x & 3] >> 1)) >> 3;
          g6 = (f + (g_bayer[y & 3][x & 3] >> 2)) >> 2;
          r5 = r5 > 31 ? 31 : r5;
          g6 = g6 > 63 ? 63 : g6;
          lm[y * w + x] = (uint16_t)(r5 << 11 | g6 << 5 | r5);
        }
    }

  fglPut(c->x, c->y, w, h, lm, (size_t)w * 2);
  g_gl.lightmaps++;
  return c;
}

/* D_MipLevelForScale on the surface's nearest point */

static int surface_mip(msurface_t *s, const vec3_t *v, int n)
{
  float scale_for_mip = xscale > yscale ? xscale : yscale;
  float zmin = 99999.0f;
  float scale;
  int i;

  for (i = 0; i < n; i++)
    {
      float z = (v[i][0] - r_origin[0]) * vpn[0] +
                (v[i][1] - r_origin[1]) * vpn[1] +
                (v[i][2] - r_origin[2]) * vpn[2];

      zmin = z < zmin ? z : zmin;
    }

  zmin = zmin < 1.0f ? 1.0f : zmin;
  scale = scale_for_mip * s->texinfo->mipadjust / zmin;
  if (scale >= d_scalemip[0])
    {
      return 0;
    }
  else if (scale >= d_scalemip[1])
    {
      return 1;
    }
  else if (scale >= d_scalemip[2])
    {
      return 2;
    }

  return 3;
}

/* A surface's polygon (its vertices from the model's edges) */

static int surface_verts(model_t *m, msurface_t *s, vec3_t *v)
{
  int n = s->numedges < 64 ? s->numedges : 64;
  int i;

  for (i = 0; i < n; i++)
    {
      int e = m->surfedges[s->firstedge + i];
      float *p = e >= 0 ? m->vertexes[m->edges[e].v[0]].position :
                          m->vertexes[m->edges[-e].v[1]].position;

      VectorCopy(p, v[i]);
    }

  return n;
}

/* The software renderer's warp (D_DrawTurbulent8Span): 8 texels across a
 * period of 128, moving 20 a second
 */

static float turb(float x)
{
  return 8.0f * sinf(x * (2.0f * (float)M_PI / 128.0f));
}

/* A water or sky polygon's texture coordinates at a vertex: water warped
 * through the turbulence, the sky from the view's direction (GLQuake's
 * flattened sphere; the front layer twice as fast)
 */

static void warp_vertex(msurface_t *s, texture_t *tex, const float *p,
                        int layer)
{
  mtexinfo_t *ti = s->texinfo;
  float ss;
  float tt;

  if (s->flags & SURF_DRAWSKY)
    {
      float speed = (float)cl.time * (layer ? 16.0f : 8.0f);
      vec3_t dir;
      float len;

      VectorSubtract(p, r_origin, dir);
      dir[2] *= 3.0f;
      len = 6.0f * 63.0f / sqrtf(DotProduct(dir, dir));
      ss = (speed + dir[0] * len) / 128.0f;
      tt = (speed + dir[1] * len) / 128.0f;
    }
  else
    {
      float os = DotProduct(p, ti->vecs[0]) + ti->vecs[0][3];
      float ot = DotProduct(p, ti->vecs[1]) + ti->vecs[1][3];

      ss = (os + turb(ot + (float)cl.time * 20.0f)) / (float)tex->width;
      tt = (ot + turb(os + (float)cl.time * 20.0f)) / (float)tex->height;
    }

  glTexCoord2f(ss, tt);
  glVertex3fv(p);
}

/* The polygon cut on a grid (GLQuake's SubdividePolygon, without
 * recursion: the stack is small), each piece drawn with its vertices'
 * coordinates: water every 32 units (a quarter of the warp's period), the
 * sky every 128
 */

#define WARP_POOL    24
#define WARP_VERTS   64

static void warp_polys(msurface_t *s, texture_t *tex, const vec3_t *v0,
                       int n0, int layer)
{
  static float pool[WARP_POOL][WARP_VERTS][3];
  static int count[WARP_POOL];
  float grid = (s->flags & SURF_DRAWSKY) ? 128.0f : 32.0f;
  int sp = 1;
  int i;
  int j;

  memcpy(pool[0], v0, (size_t)n0 * sizeof(vec3_t));
  count[0] = n0;
  while (sp > 0)
    {
      float (*p)[3] = pool[--sp];
      int n = count[sp];
      float mins[3] = { 1e9f, 1e9f, 1e9f };
      float maxs[3] = { -1e9f, -1e9f, -1e9f };
      int axis = -1;
      float m = 0.0f;

      for (j = 0; j < n; j++)
        {
          for (i = 0; i < 3; i++)
            {
              mins[i] = p[j][i] < mins[i] ? p[j][i] : mins[i];
              maxs[i] = p[j][i] > maxs[i] ? p[j][i] : maxs[i];
            }
        }

      for (i = 0; i < 3 && sp + 2 <= WARP_POOL && n + 2 <= WARP_VERTS; i++)
        {
          m = grid * floorf((mins[i] + maxs[i]) * 0.5f / grid + 0.5f);
          if (maxs[i] - m >= grid / 16.0f && m - mins[i] >= grid / 16.0f)
            {
              axis = i;
              break;
            }
        }

      if (axis < 0)
        {
          glBegin(GL_POLYGON);
          for (j = 0; j < n; j++)
            {
              warp_vertex(s, tex, p[j], layer);
            }

          glEnd();
          continue;
        }

      /* Cut at m: the front half into the next slot, the back half into
       * the one after (the polygon itself is read from a copy)
       */

      {
        static float cp[WARP_VERTS][3];
        float (*f)[3] = pool[sp];
        float (*b)[3] = pool[sp + 1];
        int nf = 0;
        int nb = 0;

        memcpy(cp, p, (size_t)n * sizeof(cp[0]));
        for (j = 0; j < n; j++)
          {
            const float *a = cp[j];
            const float *c = cp[(j + 1) % n];
            float da = a[axis] - m;
            float dc = c[axis] - m;

            if (da >= 0.0f)
              {
                VectorCopy(a, f[nf]);
                nf++;
              }

            if (da <= 0.0f)
              {
                VectorCopy(a, b[nb]);
                nb++;
              }

            if (da != 0.0f && dc != 0.0f && (da > 0.0f) != (dc > 0.0f))
              {
                float t = da / (da - dc);

                for (i = 0; i < 3; i++)
                  {
                    f[nf][i] = a[i] + t * (c[i] - a[i]);
                  }

                VectorCopy(f[nf], b[nb]);
                nf++;
                nb++;
              }
          }

        count[sp] = nf;
        count[sp + 1] = nb;
        sp += 2;
      }
    }
}

static void draw_surface(model_t *m, msurface_t *s)
{
  static vec3_t v[64];
  mtexinfo_t *ti = s->texinfo;
  texture_t *tex = R_TextureAnimation(ti->texture);
  int n = surface_verts(m, s, v);
  int i;

  if (n < 3)
    {
      return;
    }

  g_gl.surfs++;
  if (s->flags & (SURF_DRAWSKY | SURF_DRAWTURB))
    {
      int k = texture_index(ti->texture);

      if (k < 0 || g_gl.gltex[k] == 0)
        {
          return;
        }

      glEnable(GL_TEXTURE_2D);
      glColor3f(1.0f, 1.0f, 1.0f);
      glBindTexture(GL_TEXTURE_2D, g_gl.gltex[k]);
      warp_polys(s, tex, v, n, 0);
      if ((s->flags & SURF_DRAWSKY) && g_gl.skyfront != 0)
        {
          /* The front layer over it, keyed, twice as fast (on the same
           * depth: GL_LEQUAL)
           */

          glBindTexture(GL_TEXTURE_2D, g_gl.skyfront);
          glEnable(GL_ALPHA_TEST);
          glDepthFunc(GL_LEQUAL);
          warp_polys(s, tex, v, n, 1);
          glDepthFunc(GL_LESS);
          glDisable(GL_ALPHA_TEST);
        }

      return;
    }

  {
    int mip = surface_mip(s, (const vec3_t *)v, n);
    int k = texture_index(tex);
    struct gcache_s *c;
    float iw = 1.0f / (float)tex->width;
    float ih = 1.0f / (float)tex->height;
    float lw = 1.0f / (float)s->extents[0];
    float lh = 1.0f / (float)s->extents[1];
    static float st[64][4];

    if (k < 0 || g_gl.mtex[k][mip] == 0)
      {
        return;
      }

    c = lightmap_block(s, mip);
    for (i = 0; i < n; i++)
      {
        float ss = DotProduct(v[i], ti->vecs[0]) + ti->vecs[0][3];
        float tt = DotProduct(v[i], ti->vecs[1]) + ti->vecs[1][3];

        st[i][0] = ss * iw;
        st[i][1] = tt * ih;
        st[i][2] = (ss - s->texturemins[0]) * lw;
        st[i][3] = (tt - s->texturemins[1]) * lh;
      }

    /* The texture */

    glEnable(GL_TEXTURE_2D);
    glColor3f(1.0f, 1.0f, 1.0f);
    glBindTexture(GL_TEXTURE_2D, g_gl.mtex[k][mip]);
    glBegin(GL_POLYGON);
    for (i = 0; i < n; i++)
      {
        glTexCoord2f(st[i][0], st[i][1]);
        glVertex3fv(v[i]);
      }

    glEnd();

    /* Its lightmap blended on (2 x the product), on the same depth */

    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    if (c != NULL)
      {
        fglBindRect(c->x, c->y, c->w, c->h, GL_FALSE);
        glEnable(GL_BLEND);
        glBlendFunc(GL_DST_COLOR, GL_SRC_COLOR);
        glBegin(GL_POLYGON);
        for (i = 0; i < n; i++)
          {
            glTexCoord2f(st[i][2], st[i][3]);
            glVertex3fv(v[i]);
          }

        glEnd();
        glDisable(GL_BLEND);
      }

    /* The fullbright texels over it, as they are */

    if (g_gl.fbtex[k][mip] != 0)
      {
        glBindTexture(GL_TEXTURE_2D, g_gl.fbtex[k][mip]);
        glEnable(GL_ALPHA_TEST);
        glBegin(GL_POLYGON);
        for (i = 0; i < n; i++)
          {
            glTexCoord2f(st[i][0], st[i][1]);
            glVertex3fv(v[i]);
          }

        glEnd();
        glDisable(GL_ALPHA_TEST);
      }

    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
  }
}

/****************************************************************************
 * The world
 ****************************************************************************/

static qboolean cull_box(const vec3_t mins, const vec3_t maxs)
{
  int i;

  for (i = 0; i < 4; i++)
    {
      if (BoxOnPlaneSide((float *)mins, (float *)maxs, &g_gl.frustum[i]) == 2)
        {
          return true;
        }
    }

  return false;
}

static void world_node(mnode_t *node)
{
  msurface_t *surf;
  mplane_t *plane;
  float dot;
  int side;
  int c;

  if (node->contents == CONTENTS_SOLID || node->visframe != r_visframecount)
    {
      return;
    }

  {
    vec3_t mins;
    vec3_t maxs;

    mins[0] = node->minmaxs[0]; mins[1] = node->minmaxs[1]; mins[2] = node->minmaxs[2];
    maxs[0] = node->minmaxs[3]; maxs[1] = node->minmaxs[4]; maxs[2] = node->minmaxs[5];
    if (cull_box(mins, maxs))
      {
        return;
      }
  }

  if (node->contents < 0)                 /* A leaf: its surfaces seen */
    {
      mleaf_t *leaf = (mleaf_t *)node;
      msurface_t **mark = leaf->firstmarksurface;

      for (c = leaf->nummarksurfaces; c > 0; c--)
        {
          (*mark++)->visframe = r_framecount;
        }

      if (leaf->efrags)
        {
          R_StoreEfrags(&leaf->efrags);
        }

      return;
    }

  plane = node->plane;
  dot = plane->type < 3 ? modelorg[plane->type] - plane->dist :
                          DotProduct(modelorg, plane->normal) - plane->dist;
  side = dot >= 0.0f ? 0 : 1;
  world_node(node->children[side]);

  surf = cl.worldmodel->surfaces + node->firstsurface;
  for (c = node->numsurfaces; c > 0; c--, surf++)
    {
      if (surf->visframe == r_framecount &&
          ((surf->flags & SURF_PLANEBACK) != 0) == (side != 0))
        {
          draw_surface(cl.worldmodel, surf);
        }
    }

  world_node(node->children[!side]);
}

/****************************************************************************
 * Entities
 ****************************************************************************/

static void rotate_for_entity(entity_t *e)
{
  glTranslatef(e->origin[0], e->origin[1], e->origin[2]);
  glRotatef(e->angles[1], 0.0f, 0.0f, 1.0f);
  glRotatef(-e->angles[0], 0.0f, 1.0f, 0.0f);
  glRotatef(e->angles[2], 1.0f, 0.0f, 0.0f);
}

static void brush_model(entity_t *e)
{
  model_t *m = e->model;
  msurface_t *s;
  vec3_t mins;
  vec3_t maxs;
  vec3_t org;
  vec3_t temp;
  qboolean rotated = e->angles[0] || e->angles[1] || e->angles[2];
  int i;
  int k;

  if (rotated)
    {
      for (i = 0; i < 3; i++)
        {
          mins[i] = e->origin[i] - m->radius;
          maxs[i] = e->origin[i] + m->radius;
        }
    }
  else
    {
      VectorAdd(e->origin, m->mins, mins);
      VectorAdd(e->origin, m->maxs, maxs);
    }

  if (cull_box(mins, maxs))
    {
      return;
    }

  /* Where the view is in the model's own frame (the planes' side) */

  VectorSubtract(r_origin, e->origin, org);
  if (rotated)
    {
      vec3_t f;
      vec3_t r;
      vec3_t u;

      VectorCopy(org, temp);
      AngleVectors(e->angles, f, r, u);
      org[0] = DotProduct(temp, f);
      org[1] = -DotProduct(temp, r);
      org[2] = DotProduct(temp, u);
    }

  /* Its surfaces lit by the dynamic lights too */

  for (k = 0; k < MAX_DLIGHTS; k++)
    {
      if (cl_dlights[k].die >= cl.time && cl_dlights[k].radius)
        {
          R_MarkLights(&cl_dlights[k], 1 << k,
                       m->nodes + m->hulls[0].firstclipnode);
        }
    }

  currententity = e;
  glPushMatrix();
  rotate_for_entity(e);
  s = m->surfaces + m->firstmodelsurface;
  for (i = 0; i < m->nummodelsurfaces; i++, s++)
    {
      float dot = DotProduct(org, s->plane->normal) - s->plane->dist;

      if (((s->flags & SURF_PLANEBACK) && dot < -BACKFACE_EPSILON) ||
          (!(s->flags & SURF_PLANEBACK) && dot > BACKFACE_EPSILON))
        {
          draw_surface(m, s);
        }
    }

  glPopMatrix();
}

/* A model's skin (or a sprite frame) as a texture, made when first used */

static GLuint image_texture(qboolean sprite, const void *key, int w, int h,
                            const uint8_t *px)
{
  int i;
  int n = sprite ? g_gl.nsprite : g_gl.nskin;
  int max = sprite ? MAXSPRITES : MAXSKINS;
  typeof(&g_gl.skin[0]) tab = sprite ? g_gl.sprite : g_gl.skin;

  for (i = 0; i < n; i++)
    {
      if (tab[i].key == key)
        {
          return tab[i].t;
        }
    }

  if (n == max)
    {
      /* Full: the first goes (it is made again if it comes back) */

      glDeleteTextures(1, &tab[0].t);
      memmove(&tab[0], &tab[1], sizeof(tab[0]) * (max - 1));
      n--;
    }

  tab[n].key = key;
  tab[n].w = w;
  tab[n].h = h;
  glGenTextures(1, &tab[n].t);
  glBindTexture(GL_TEXTURE_2D, tab[n].t);
  glGetError();
  glTexImage2D(GL_TEXTURE_2D, 0, GL_COLOR_INDEX, w, h, 0, GL_COLOR_INDEX,
               GL_UNSIGNED_BYTE, px);
  {
    GLenum err = glGetError();

    if (err != GL_NO_ERROR)
      {
        syslog(LOG_WARNING, "quake: -gl: a %s %d x %d: GL error %#x\n",
               sprite ? "sprite frame" : "skin", w, h, err);
      }
  }

  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP);
  if (sprite)
    {
      g_gl.nsprite = n + 1;
    }
  else
    {
      g_gl.nskin = n + 1;
    }

  return tab[n].t;
}

static void alias_model(entity_t *e, alight_t *lighting, qboolean gun)
{
  aliashdr_t *hdr = Mod_Extradata(e->model);
  mdl_t *mdl = (mdl_t *)((byte *)hdr + hdr->model);
  stvert_t *stv = (stvert_t *)((byte *)hdr + hdr->stverts);
  mtriangle_t *tri = (mtriangle_t *)((byte *)hdr + hdr->triangles);
  maliasskindesc_t *skin;
  trivertx_t *verts;
  vec3_t f;
  vec3_t r;
  vec3_t u;
  float lvec[3];
  int ambient;
  float shadelight;
  int frame = e->frame;
  int skinnum = e->skinnum;
  GLuint skintex;
  int i;
  int k;

  /* The frame (groups by time), as R_AliasSetupFrame */

  if (frame >= mdl->numframes || frame < 0)
    {
      frame = 0;
    }

  if (hdr->frames[frame].type == ALIAS_SINGLE)
    {
      verts = (trivertx_t *)((byte *)hdr + hdr->frames[frame].frame);
    }
  else
    {
      maliasgroup_t *g = (maliasgroup_t *)((byte *)hdr + hdr->frames[frame].frame);
      float *iv = (float *)((byte *)hdr + g->intervals);
      float full = iv[g->numframes - 1];
      float time = cl.time + e->syncbase;
      float target = time - ((int)(time / full)) * full;

      for (i = 0; i < g->numframes - 1; i++)
        {
          if (iv[i] > target)
            {
              break;
            }
        }

      verts = (trivertx_t *)((byte *)hdr + g->frames[i].frame);
    }

  /* The skin (groups by time), as R_AliasSetupSkin */

  if (skinnum >= mdl->numskins || skinnum < 0)
    {
      skinnum = 0;
    }

  skin = (maliasskindesc_t *)((byte *)hdr + hdr->skindesc) + skinnum;
  if (skin->type == ALIAS_SKIN_GROUP)
    {
      maliasskingroup_t *g = (maliasskingroup_t *)((byte *)hdr + skin->skin);
      float *iv = (float *)((byte *)hdr + g->intervals);
      float full = iv[g->numskins - 1];
      float time = cl.time + e->syncbase;
      float target = time - ((int)(time / full)) * full;

      for (i = 0; i < g->numskins - 1; i++)
        {
          if (iv[i] > target)
            {
              break;
            }
        }

      skin = &g->skindescs[i];
    }

  skintex = image_texture(false, (byte *)hdr + skin->skin, mdl->skinwidth,
                          mdl->skinheight, (byte *)hdr + skin->skin);

  /* The light, as R_AliasSetupLighting: the direction into the model's
   * frame, the ambient and shade light in the colormap's units
   */

  AngleVectors(e->angles, f, r, u);
  lvec[0] = DotProduct(lighting->plightvec, f);
  lvec[1] = -DotProduct(lighting->plightvec, r);
  lvec[2] = DotProduct(lighting->plightvec, u);
  ambient = lighting->ambientlight < LIGHT_MIN ? LIGHT_MIN : lighting->ambientlight;
  ambient = (255 - ambient) << VID_CBITS;
  ambient = ambient < LIGHT_MIN ? LIGHT_MIN : ambient;
  shadelight = (lighting->shadelight < 0 ? 0 : lighting->shadelight) * VID_GRADES;

  glPushMatrix();
  rotate_for_entity(e);
  if (gun)
    {
      glDepthRange(0.0f, 0.3f);
    }

  glEnable(GL_TEXTURE_2D);
  glBindTexture(GL_TEXTURE_2D, skintex);
  glEnable(GL_CULL_FACE);
  glBegin(GL_TRIANGLES);
  for (i = 0; i < mdl->numtris; i++, tri++)
    {
      /* (Quake's triangles wind the other way: their vertices backwards) */

      for (k = 2; k >= 0; k--)
        {
          int vi = tri->vertindex[k];
          trivertx_t *tv = &verts[vi];
          float *n = r_avertexnormals[tv->lightnormalindex];
          float lc = DotProduct(n, lvec);
          int temp = ambient;
          float s = (float)(stv[vi].s >> 16);   /* (the software's 16.16) */
          float shade;

          if (lc < 0)
            {
              temp += (int)(shadelight * lc);
              temp = temp < 0 ? 0 : temp;
            }

          shade = g_gl.factor[(temp >> 8) & 63] / 128.0f;
          if (stv[vi].onseam && !tri->facesfront)
            {
              s += mdl->skinwidth / 2;
            }

          glColor3f(shade, shade, shade);
          glTexCoord2f((s + 0.5f) / mdl->skinwidth,
                       ((float)(stv[vi].t >> 16) + 0.5f) / mdl->skinheight);
          glVertex3f(tv->v[0] * mdl->scale[0] + mdl->scale_origin[0],
                     tv->v[1] * mdl->scale[1] + mdl->scale_origin[1],
                     tv->v[2] * mdl->scale[2] + mdl->scale_origin[2]);
        }
    }

  glEnd();
  glDisable(GL_CULL_FACE);
  if (gun)
    {
      glDepthRange(0.0f, 1.0f);
    }

  glPopMatrix();
}

/* An alias model's light, as R_DrawEntitiesOnList / R_DrawViewModel */

static void alias_light(entity_t *e, alight_t *l, float *lightvec, int least)
{
  int j = R_LightPoint(e->origin);
  int k;

  j = j < least ? least : j;
  l->ambientlight = j;
  l->shadelight = j;
  l->plightvec = lightvec;
  for (k = 0; k < MAX_DLIGHTS; k++)
    {
      if (cl_dlights[k].die >= cl.time)
        {
          vec3_t dist;
          float add;

          VectorSubtract(e->origin, cl_dlights[k].origin, dist);
          add = cl_dlights[k].radius - Length(dist);
          if (add > 0)
            {
              l->ambientlight += add;
            }
        }
    }

  if (l->ambientlight > 128)
    {
      l->ambientlight = 128;
    }

  if (l->ambientlight + l->shadelight > 192)
    {
      l->shadelight = 192 - l->ambientlight;
    }
}

static void sprite_entity(entity_t *e)
{
  mspriteframe_t *f;
  GLuint t;
  vec3_t p;

  currententity = e;
  f = R_GetSpriteframe((msprite_t *)e->model->cache.data);
  if (f == NULL)
    {
      return;
    }

  t = image_texture(true, f, f->width, f->height, f->pixels);
  glEnable(GL_TEXTURE_2D);
  glEnable(GL_ALPHA_TEST);                 /* (255: transparent, the key) */
  glBindTexture(GL_TEXTURE_2D, t);
  glColor3f(1.0f, 1.0f, 1.0f);
  glBegin(GL_QUADS);
  VectorMA(e->origin, f->down, vup, p);
  VectorMA(p, f->left, vright, p);
  glTexCoord2f(0.0f, 1.0f);
  glVertex3fv(p);
  VectorMA(e->origin, f->up, vup, p);
  VectorMA(p, f->left, vright, p);
  glTexCoord2f(0.0f, 0.0f);
  glVertex3fv(p);
  VectorMA(e->origin, f->up, vup, p);
  VectorMA(p, f->right, vright, p);
  glTexCoord2f(1.0f, 0.0f);
  glVertex3fv(p);
  VectorMA(e->origin, f->down, vup, p);
  VectorMA(p, f->right, vright, p);
  glTexCoord2f(1.0f, 1.0f);
  glVertex3fv(p);
  glEnd();
  glDisable(GL_ALPHA_TEST);
}

/****************************************************************************
 * Particles (D_DrawParticle: the game still moves them)
 ****************************************************************************/

void fpcq_gl_particle(particle_t *p)
{
  if (g_gl.npart < MAXPARTS)
    {
      VectorCopy(p->org, g_gl.part[g_gl.npart].org);
      g_gl.part[g_gl.npart].colour = g_gl.pal[(int)p->color & 255];
      g_gl.npart++;
    }
}

static void particles(void)
{
  vec3_t up;
  vec3_t right;
  int i;

  VectorScale(vup, 1.2f, up);
  VectorScale(vright, 1.2f, right);
  glDisable(GL_TEXTURE_2D);
  glBegin(GL_TRIANGLES);
  for (i = 0; i < g_gl.npart; i++)
    {
      uint16_t c = g_gl.part[i].colour;
      float *o = g_gl.part[i].org;

      glColor3f(((c >> 11) & 31) / 31.0f, ((c >> 5) & 63) / 63.0f,
                (c & 31) / 31.0f);
      glVertex3f(o[0], o[1], o[2]);
      glVertex3f(o[0] + up[0], o[1] + up[1], o[2] + up[2]);
      glVertex3f(o[0] + right[0], o[1] + right[1], o[2] + right[2]);
    }

  glEnd();
  g_gl.npart = 0;
}

/****************************************************************************
 * The view
 ****************************************************************************/

static void setup_view(void)
{
  float ymax = 4.0f * tanf(r_refdef.fov_y * (float)M_PI / 360.0f);
  float aspect = (float)r_refdef.vrect.width / (float)r_refdef.vrect.height;
  int i;

  glViewport(r_refdef.vrect.x,
             fglHeight() - (r_refdef.vrect.y + r_refdef.vrect.height),
             r_refdef.vrect.width, r_refdef.vrect.height);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glFrustum(-ymax * aspect, ymax * aspect, -ymax, ymax, 4.0, 4096.0);
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();
  glRotatef(-90.0f, 1.0f, 0.0f, 0.0f);    /* (z up) */
  glRotatef(90.0f, 0.0f, 0.0f, 1.0f);
  glRotatef(-r_refdef.viewangles[2], 1.0f, 0.0f, 0.0f);
  glRotatef(-r_refdef.viewangles[0], 0.0f, 1.0f, 0.0f);
  glRotatef(-r_refdef.viewangles[1], 0.0f, 0.0f, 1.0f);
  glTranslatef(-r_refdef.vieworg[0], -r_refdef.vieworg[1], -r_refdef.vieworg[2]);

  /* The frustum's side planes (GLQuake's R_SetFrustum) */

  RotatePointAroundVector(g_gl.frustum[0].normal, vup, vpn,
                          -(90.0f - r_refdef.fov_x / 2.0f));
  RotatePointAroundVector(g_gl.frustum[1].normal, vup, vpn,
                          90.0f - r_refdef.fov_x / 2.0f);
  RotatePointAroundVector(g_gl.frustum[2].normal, vright, vpn,
                          90.0f - r_refdef.fov_y / 2.0f);
  RotatePointAroundVector(g_gl.frustum[3].normal, vright, vpn,
                          -(90.0f - r_refdef.fov_y / 2.0f));
  for (i = 0; i < 4; i++)
    {
      g_gl.frustum[i].type = PLANE_ANYZ;
      g_gl.frustum[i].dist = DotProduct(r_origin, g_gl.frustum[i].normal);
      g_gl.frustum[i].signbits = (g_gl.frustum[i].normal[0] < 0) |
                                 (g_gl.frustum[i].normal[1] < 0) << 1 |
                                 (g_gl.frustum[i].normal[2] < 0) << 2;
    }
}

/* R_RenderView after R_SetupFrame and R_MarkLeaves: the whole 3D view */

void fpcq_gl_view(void)
{
  float lightvec[3] = { -1.0f, 0.0f, 0.0f };
  alight_t lighting;
  uint16_t tint;
  int alpha;
  int i;
  int y;

  /* The view in the 8-bit frame: nothing (the key), the 2D parts go over
   * the picture
   */

  for (y = 0; y < r_refdef.vrect.height; y++)
    {
      memset(vid.buffer + (r_refdef.vrect.y + y) * vid.rowbytes +
             r_refdef.vrect.x, 255, r_refdef.vrect.width);
    }

  table_check();
  setup_view();
  glEnable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

  /* The world */

  currententity = &cl_entities[0];
  VectorCopy(r_origin, modelorg);
  R_PushDlights();
  world_node(cl.worldmodel->nodes);

  /* The entities */

  if (r_drawentities.value)
    {
      for (i = 0; i < cl_numvisedicts; i++)
        {
          entity_t *e = cl_visedicts[i];

          if (e == &cl_entities[cl.viewentity])
            {
              continue;
            }

          currententity = e;
          switch (e->model->type)
            {
              case mod_brush:
                brush_model(e);
                break;

              case mod_alias:
                alias_light(e, &lighting, lightvec, 0);
                alias_model(e, &lighting, false);
                break;

              case mod_sprite:
                sprite_entity(e);
                break;

              default:
                break;
            }
        }
    }

  /* The gun */

  if (r_drawviewmodel.value && !(cl.items & IT_INVISIBILITY) &&
      cl.stats[STAT_HEALTH] > 0 && cl.viewent.model != NULL)
    {
      currententity = &cl.viewent;
      alias_light(&cl.viewent, &lighting, lightvec, 24);
      alias_model(&cl.viewent, &lighting, true);
    }

  /* The particles (R_DrawParticles moves them, D_DrawParticle gives them) */

  R_DrawParticles();
  particles();

  /* The tints */

  fpcq_tint(&tint, &alpha);
  if (alpha > 0)
    {
      fglBlendView(tint, alpha);
    }

  glFlush();
  g_gl.frames++;
}

void fpcq_gl_report(void)
{
  syslog(LOG_INFO, "quake: -gl: %" PRIu32 " frames, %" PRIu32 " surfaces drawn, "
         "%" PRIu32 " lightmaps made, %" PRIu32 " atlas flushes, %" PRIu32
         " triangles\n", g_gl.frames, g_gl.surfs, g_gl.lightmaps,
         g_gl.flushes, fglTriangles());
}
