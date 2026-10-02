/****************************************************************************
 * apps/graphics/fpcgame/tri/tri_fpc.c
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * TRI's game runtime on the FPC.  TRI's own SDK sources (tri_gfx.c,
 * tri_text.c, tri_audio.c, as they are) turn a game's calls into TRI's
 * command words (tri_isa.h) for its two video processors; this file stands
 * in for the rest of TRI's runtime (the frame loop, the pad, time, random
 * numbers, tri_sdk.c and tri_system.c) and for the processors themselves:
 * it keeps their VRAM, palettes and definitions as TRI's do, and each
 * frame draws with the FPC game SDK what they would show:
 *
 *   the backdrop; the layers below the objects (0-3, in order); the
 *   objects of the frame (VDP1's list: sprites, rectangles, affine
 *   sprites, in call order); the layers above them.
 *
 * Sheets (VDP1, 8 bits a pixel) become RGB565 images through each palette
 * they are drawn with, kept until their bytes or the palette change;
 * tilesets (VDP2) likewise.  A layer keeps its map drawn in a surface
 * (fg_internal.h) and redraws the tiles whose entries in VRAM changed
 * since (compared each frame: however the game wrote them), or all of
 * them when its tileset, palette or configuration did.  Sound commands go
 * to TRI's own engine (fg_audio.c), which reads samples from VDP2's VRAM
 * here.  The random numbers are TRI's xorshift32 with TRI's seed, so a
 * game plays out as it does on TRI.
 *
 * Differences from TRI: objects are drawn in the frame they are recorded
 * in (TRI shows them a frame later); a translucent or added sprite blends
 * with whatever is below it (on TRI only with other sprites); the overlay
 * (layer 4, tileset 7) is not drawn; the video mode is the game's for the
 * whole run.  docs/games.md, "TRI games".
 ****************************************************************************/

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>

#include "fg_internal.h"
#include "fpc_gpu.h"
#include "tri_sdk_internal.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define V1_SRAM          TRI_VDP1_VRAM_SIZE
#define V2_SRAM          TRI_VDP2_VRAM_SIZE
#define PSRAM_STEP       (256 * 1024)
#define TRI_LAYERS       4                /* The game's (4 is the overlay) */
#define HOLD_D_FRAMES    60

/****************************************************************************
 * Private Types
 ****************************************************************************/

struct vram_s
{
  FAR uint8_t *sram;
  uint32_t sram_size;
  FAR uint8_t *psram;                     /* Grown as it is written */
  uint32_t psram_len;
};

struct sheet_s
{
  uint32_t off;
  uint16_t w;
  uint16_t h;
  int8_t img[16];                         /* Per palette: an image, or -1 */
};

struct tileset_s
{
  uint32_t off;
  uint32_t ntiles;
  int8_t img[16];
};

struct tilemap_s
{
  uint32_t off;
  uint8_t log2w;
  uint8_t log2h;
  bool defined;
};

struct layer_s
{
  uint32_t cfg;                           /* TRI_LAYER_CFG */
  int16_t sx;
  int16_t sy;

  /* What is drawn in its surface */

  bool ready;
  struct fg_rect_s surf;
  uint32_t drawn_cfg;
  int drawn_w;                            /* Tiles */
  int drawn_h;
  FAR uint16_t *drawn;                    /* The entries drawn */
  bool all;                               /* Every tile again */
};

/****************************************************************************
 * Private Data
 ****************************************************************************/

static struct
{
  FAR const tri_game_t *game;
  int mode;                               /* TRI_VIDEO_* for the run */
  struct vram_s v[3];                     /* [TRI_DEST_VDP1], [TRI_DEST_VDP2] */
  uint16_t pal1[16][256];
  uint16_t pal2[16][256];
  struct sheet_s sheet[16];
  struct tileset_s tileset[8];
  struct tilemap_s tilemap[8];
  struct layer_s layer[TRI_LAYERS];
  uint16_t backdrop;

  /* The frame's objects (VDP1's list) */

  FAR uint32_t *obj;
  int nobj;
  int maxobj;

  tri_stats_t st;
  uint32_t api_errors_shown;
  int hold_d;
} g_tri;

static void layers_redraw(int tileset, int pal);

/****************************************************************************
 * VRAM
 ****************************************************************************/

/* [off, off + len) of a VDP's VRAM, wholly in its SRAM or its PSRAM; the
 * PSRAM grown to hold it if grow (NULL: outside, or no memory)
 */

static FAR uint8_t *vram(int dest, uint32_t off, uint32_t len, bool grow)
{
  FAR struct vram_s *v = &g_tri.v[dest];
  uint32_t p;

  if (dest != TRI_DEST_VDP1 && dest != TRI_DEST_VDP2)
    {
      return NULL;
    }

  if (off < v->sram_size)
    {
      return len <= v->sram_size - off ? v->sram + off : NULL;
    }

  if (off < TRI_VRAM_PSRAM || off - TRI_VRAM_PSRAM >= TRI_VRAM_PSRAM_SIZE ||
      len > TRI_VRAM_PSRAM_SIZE - (off - TRI_VRAM_PSRAM))
    {
      return NULL;
    }

  p = off - TRI_VRAM_PSRAM;
  if (p + len > v->psram_len)
    {
      FAR uint8_t *n;
      uint32_t size;

      if (!grow)
        {
          return NULL;
        }

      size = (p + len + PSRAM_STEP - 1) & ~(PSRAM_STEP - 1);
      n = realloc(v->psram, size);
      if (n == NULL)
        {
          return NULL;
        }

      memset(n + v->psram_len, 0, size - v->psram_len);
      if (n != v->psram && dest == TRI_DEST_VDP2)
        {
          fg_audio_stop_all();            /* (voices pointed into the old copy) */
        }

      v->psram = n;
      v->psram_len = size;
    }

  return v->psram + p;
}

/* The sound engine's view of VDP2's VRAM (fg_audio_vram()) */

static bool v2_range(uint32_t off, uint32_t len)
{
  return vram(TRI_DEST_VDP2, off, len, false) != NULL;
}

static FAR const uint8_t *v2_base(uint32_t off)
{
  return vram(TRI_DEST_VDP2, off, 1, false);
}

/****************************************************************************
 * Images of sheets and tilesets, per palette
 ****************************************************************************/

static void sheet_forget(FAR struct sheet_s *s, int pal)
{
  int p;

  for (p = 0; p < 16; p++)
    {
      if ((pal < 0 || p == pal) && s->img[p] >= 0)
        {
          fg_image_free(s->img[p]);
          s->img[p] = -1;
        }
    }
}

static void tileset_forget(FAR struct tileset_s *t, int pal)
{
  int p;

  for (p = 0; p < 16; p++)
    {
      if ((pal < 0 || p == pal) && t->img[p] >= 0)
        {
          fg_image_free(t->img[p]);
          t->img[p] = -1;
        }
    }
}

static void pal565(FAR const uint16_t *pal555, FAR uint16_t *out)
{
  int i;

  for (i = 0; i < 256; i++)
    {
      out[i] = fg_rgb555(pal555[i]);
    }
}

static int sheet_image(int id, int pal)
{
  FAR struct sheet_s *s = &g_tri.sheet[id];
  FAR const uint8_t *px;
  uint16_t pal16[256];

  if (s->w == 0)
    {
      return -1;
    }

  if (s->img[pal] >= 0)
    {
      return s->img[pal];
    }

  px = vram(TRI_DEST_VDP1, s->off, (uint32_t)s->w * s->h, true);
  if (px == NULL)
    {
      return -1;
    }

  pal565(g_tri.pal1[pal], pal16);
  s->img[pal] = (int8_t)fg_image_from8(s->w, s->h, px, s->w, pal16);
  return s->img[pal];
}

/* A tileset through palette pal: an image of its tiles, 16 a row, tile n
 * at place n (fg_internal.h's layers: tile 0 empty)
 */

static int tileset_image(int id, int pal)
{
  FAR struct tileset_s *t = &g_tri.tileset[id];
  FAR const uint8_t *tiles;
  uint16_t pal16[256];
  uint8_t row[128 * 8];
  int rows;
  int img;
  int r;

  if (t->ntiles == 0)
    {
      return -1;
    }

  if (t->img[pal] >= 0)
    {
      return t->img[pal];
    }

  tiles = vram(TRI_DEST_VDP2, t->off, t->ntiles * 64, true);
  if (tiles == NULL)
    {
      return -1;
    }

  rows = (int)(t->ntiles + 15) / 16;
  img = fg_image_new(128, rows * 8);
  if (img < 0)
    {
      return -1;
    }

  pal565(g_tri.pal2[pal], pal16);
  for (r = 0; r < rows; r++)
    {
      int k;

      memset(row, 0, sizeof(row));
      for (k = 0; k < 16 && (uint32_t)(r * 16 + k) < t->ntiles; k++)
        {
          FAR const uint8_t *tp = tiles + (size_t)(r * 16 + k) * 64;
          int y;

          for (y = 0; y < 8; y++)
            {
              memcpy(row + y * 128 + k * 8, tp + y * 8, 8);
            }
        }

      fg_image_put8(img, 0, r * 8, 128, 8, row, 128, pal16);
    }

  t->img[pal] = (int8_t)img;
  return img;
}

/* VRAM [off, off + len) of dest has changed: what was made from it goes */

static void vram_changed(int dest, uint32_t off, uint32_t len)
{
  int i;

  if (dest == TRI_DEST_VDP1)
    {
      for (i = 0; i < 16; i++)
        {
          FAR struct sheet_s *s = &g_tri.sheet[i];
          uint32_t n = (uint32_t)s->w * s->h;

          if (s->w != 0 && off < s->off + n && s->off < off + len)
            {
              sheet_forget(s, -1);
            }
        }
    }
  else
    {
      for (i = 0; i < 8; i++)
        {
          FAR struct tileset_s *t = &g_tri.tileset[i];

          if (t->ntiles != 0 && off < t->off + t->ntiles * 64 &&
              t->off < off + len)
            {
              tileset_forget(t, -1);
              layers_redraw(i, -1);
            }
        }
    }
}

/****************************************************************************
 * Layers
 ****************************************************************************/

#define CFG_ON(c)       ((c) & 1)
#define CFG_TILESET(c)  (((c) >> 1) & 7)
#define CFG_TILEMAP(c)  (((c) >> 4) & 7)
#define CFG_PALBASE(c)  (((c) >> 8) & 15)
#define CFG_PRIO(c)     (((c) >> 12) & 1)

static void layer_tile(FAR struct layer_s *l, int x, int y, uint16_t e)
{
  int ts = CFG_TILESET(l->cfg);
  int t = TRI_TILE_INDEX(e);
  int pal = (CFG_PALBASE(l->cfg) + TRI_TILE_PAL(e)) & 15;
  int dx = l->surf.x + x * 8;
  int dy = l->surf.y + y * 8;
  FAR const struct fg_rect_s *r;
  int img;
  int sx;
  int sy;

  if (t == 0 || (uint32_t)t >= g_tri.tileset[ts].ntiles ||
      (img = tileset_image(ts, pal)) < 0 ||
      (r = fg_image_rect(img)) == NULL)
    {
      fpc_gpu_fill(dx, dy, 8, 8, FG_KEY);
      return;
    }

  sx = r->x + (t & 15) * 8;
  sy = r->y + (t >> 4) * 8;
  if (!TRI_TILE_HFLIP(e) && !TRI_TILE_VFLIP(e))
    {
      fpc_gpu_copy(sx, sy, dx, dy, 8, 8);
    }
  else
    {
      struct fpc_gpu_sprite_s s;

      memset(&s, 0, sizeof(s));
      s.dx    = dx;
      s.dy    = dy;
      s.w     = 8;
      s.h     = 8;
      s.cx    = sx;
      s.cy    = sy;
      s.cw    = 8;
      s.ch    = 8;
      s.a     = TRI_TILE_HFLIP(e) ? -256 : 256;
      s.d     = TRI_TILE_VFLIP(e) ? -256 : 256;
      s.u0    = TRI_TILE_HFLIP(e) ? 8 * 256 - 129 : 128;
      s.v0    = TRI_TILE_VFLIP(e) ? 8 * 256 - 129 : 128;
      s.alpha = 255;
      fpc_gpu_sprite(&s);
    }

  g_fg.tiles++;
}

static void layer_draw(int n)
{
  FAR struct layer_s *l = &g_tri.layer[n];
  FAR struct tilemap_s *m = &g_tri.tilemap[CFG_TILEMAP(l->cfg)];
  FAR const uint16_t *map;
  int w;
  int h;
  int x;
  int y;

  if (!m->defined)
    {
      return;
    }

  w = 1 << m->log2w;
  h = 1 << m->log2h;
  map = (FAR const uint16_t *)vram(TRI_DEST_VDP2, m->off, (uint32_t)w * h * 2,
                                   true);
  if (map == NULL || w * 8 > FPC_GPU_SPACE_W || h * 8 > FPC_GPU_SPACE_H)
    {
      return;
    }

  /* Its surface, as large as its map */

  if (!l->ready || l->drawn_w != w || l->drawn_h != h)
    {
      int sx;
      int sy;

      if (l->ready)
        {
          fpc_gpu_game_free(l->surf.x, l->surf.y);
          free(l->drawn);
          l->ready = false;
        }

      l->drawn = malloc((size_t)w * h * 2);
      if (l->drawn == NULL || fpc_gpu_game_alloc(w * 8, h * 8, &sx, &sy) < 0)
        {
          free(l->drawn);
          l->drawn = NULL;
          return;
        }

      l->surf.x  = sx;
      l->surf.y  = sy;
      l->surf.w  = w * 8;
      l->surf.h  = h * 8;
      l->drawn_w = w;
      l->drawn_h = h;
      l->ready   = true;
      l->all     = true;
    }

  if ((l->cfg & ~1u & ~(1u << 12)) != (l->drawn_cfg & ~1u & ~(1u << 12)))
    {
      l->all = true;                      /* Another tileset, map or palette */
    }

  /* Every tile (one FILL makes them all see-through, then the ones that
   * are not empty), or those whose entries changed
   */

  if (l->all)
    {
      fpc_gpu_fill(l->surf.x, l->surf.y, l->surf.w, l->surf.h, FG_KEY);
    }

  for (y = 0; y < h; y++)
    {
      for (x = 0; x < w; x++)
        {
          int i = y * w + x;

          if (l->all ? TRI_TILE_INDEX(map[i]) != 0 : l->drawn[i] != map[i])
            {
              layer_tile(l, x, y, map[i]);
            }

          l->drawn[i] = map[i];
        }
    }

  l->all = false;
  l->drawn_cfg = l->cfg;
  fg_blit_wrap(&l->surf, l->sx, l->sy);
}

/* What a change of tileset id (its bytes) or VDP2 palette pal means for
 * the layers: every tile drawn with it again
 */

static void layers_redraw(int tileset, int pal)
{
  int n;

  for (n = 0; n < TRI_LAYERS; n++)
    {
      FAR struct layer_s *l = &g_tri.layer[n];

      if ((tileset >= 0 && CFG_TILESET(l->cfg) == (uint32_t)tileset) ||
          (pal >= 0 && (uint32_t)(pal - CFG_PALBASE(l->cfg)) < 4))
        {
          l->all = true;
        }
    }
}

/****************************************************************************
 * The video processors' commands
 ****************************************************************************/

static void obj_add(FAR const uint32_t *w, int n)
{
  if (g_tri.nobj + n > g_tri.maxobj)
    {
      int max = g_tri.maxobj ? g_tri.maxobj * 2 : 4096;
      FAR uint32_t *o;

      while (max < g_tri.nobj + n)
        {
          max *= 2;
        }

      o = realloc(g_tri.obj, (size_t)max * 4);
      if (o == NULL)
        {
          return;
        }

      g_tri.obj = o;
      g_tri.maxobj = max;
    }

  memcpy(g_tri.obj + g_tri.nobj, w, (size_t)n * 4);
  g_tri.nobj += n;
}

static void palette(int dest, uint32_t imm, FAR const uint32_t *p, int nw)
{
  FAR uint16_t *pal = dest == TRI_DEST_VDP1 ? g_tri.pal1[imm & 15] :
                      g_tri.pal2[imm & 15];
  int first = (imm >> 4) & 0xff;
  bool changed = false;
  int i;

  for (i = 0; i < 2 * nw && first + i < 256; i++)
    {
      uint16_t c = (uint16_t)(p[i / 2] >> (16 * (i & 1)));

      if (pal[first + i] != c)
        {
          pal[first + i] = c;
          changed = true;
        }
    }

  if (!changed)
    {
      return;
    }

  if (dest == TRI_DEST_VDP1)
    {
      for (i = 0; i < 16; i++)
        {
          sheet_forget(&g_tri.sheet[i], imm & 15);
        }
    }
  else
    {
      for (i = 0; i < 8; i++)
        {
          tileset_forget(&g_tri.tileset[i], imm & 15);
        }

      layers_redraw(-1, imm & 15);
    }
}

/* One command for a video processor */

static void exec(int dest, FAR const uint32_t *w, int n)
{
  uint32_t op = TRI_HDR_OP(w[0]);
  uint32_t imm = TRI_HDR_IMM(w[0]);
  FAR const uint32_t *p = w + 1;
  int nw = n - 1;

  switch (op)
    {
      case TRI_OP_NOP:
      case TRI_OP_FRAME_END:
      case TRI_OP_VIDEO_MODE:
      case TRI_OP_OVERLAY:
        break;

      case TRI_OP_FRAME_BEGIN:
        if (dest == TRI_DEST_VDP1)
          {
            g_tri.nobj = 0;
          }
        break;

      case TRI_OP_UPLOAD:
        if (nw >= 2 && p[1] <= (uint32_t)(nw - 2) * 4)
          {
            FAR uint8_t *d = vram(dest, p[0], p[1], true);

            if (d != NULL)
              {
                memcpy(d, p + 2, p[1]);
                vram_changed(dest, p[0], p[1]);
              }
          }
        break;

      case TRI_OP_VRAM_COPY:
        if (nw >= 3)
          {
            FAR uint8_t *d = vram(dest, p[0], p[2], true);
            FAR uint8_t *s = vram(dest, p[1], p[2], true);

            if (d != NULL && s != NULL)
              {
                memmove(d, s, p[2]);
                vram_changed(dest, p[0], p[2]);
              }
          }
        break;

      case TRI_OP_PALETTE:
        palette(dest, imm, p, nw);
        break;

      /* VDP1 */

      case TRI_OP_SHEET_DEF:
        if (dest == TRI_DEST_VDP1 && nw >= 2 && imm < 16)
          {
            FAR struct sheet_s *s = &g_tri.sheet[imm];

            sheet_forget(s, -1);
            s->off = p[0];
            s->w   = p[1] & 0xffff;
            s->h   = p[1] >> 16;
          }
        break;

      case TRI_OP_SPRITE:
      case TRI_OP_RECT:
      case TRI_OP_SPRITE_AFFINE:
        if (dest == TRI_DEST_VDP1)
          {
            obj_add(w, n);                /* Drawn in order, with the frame */
          }
        break;

      /* VDP2 */

      case TRI_OP_TILESET_DEF:
        if (dest == TRI_DEST_VDP2 && nw >= 2 && imm < 8)
          {
            tileset_forget(&g_tri.tileset[imm], -1);
            g_tri.tileset[imm].off    = p[0];
            g_tri.tileset[imm].ntiles = p[1] > 4096 ? 4096 : p[1];
            layers_redraw(imm, -1);
          }
        break;

      case TRI_OP_TILEMAP_DEF:
        if (dest == TRI_DEST_VDP2 && nw >= 2 && imm < 8)
          {
            g_tri.tilemap[imm].off     = p[0];
            g_tri.tilemap[imm].log2w   = p[1] & 0xff;
            g_tri.tilemap[imm].log2h   = (p[1] >> 8) & 0xff;
            g_tri.tilemap[imm].defined = (p[1] & 0xff) <= 10 &&
                                         ((p[1] >> 8) & 0xff) <= 10;
          }
        break;

      case TRI_OP_LAYER:
        if (dest == TRI_DEST_VDP2 && nw >= 1 && imm < TRI_LAYERS)
          {
            g_tri.layer[imm].cfg = p[0];
          }
        break;

      case TRI_OP_SCROLL:
        if (dest == TRI_DEST_VDP2 && nw >= 1 && imm < TRI_LAYERS)
          {
            g_tri.layer[imm].sx = (int16_t)(p[0] & 0xffff);
            g_tri.layer[imm].sy = (int16_t)(p[0] >> 16);
          }
        break;

      case TRI_OP_BACKDROP:
        g_tri.backdrop = imm & 0x7fff;
        break;

      default:
        if (dest == TRI_DEST_VDP2 && op >= TRI_OP_AUDIO_SAMPLE_DEF &&
            op <= TRI_OP_AUDIO_FM_FREQ)
          {
            fg_audio_cmd(op, imm, p, nw);
          }
        break;
    }
}

/* The frame's objects, in order */

static void draw_objects(void)
{
  int i = 0;

  while (i < g_tri.nobj)
    {
      FAR const uint32_t *w = g_tri.obj + i;
      FAR const uint32_t *p = w + 1;
      int n = 1 + TRI_HDR_NWORDS(w[0]);

      if (i + n > g_tri.nobj)
        {
          break;
        }

      switch (TRI_HDR_OP(w[0]))
        {
          case TRI_OP_RECT:
            if (n >= 3)
              {
                fg_rect((int16_t)(p[0] & 0xffff), (int16_t)(p[0] >> 16),
                        (int16_t)(p[1] & 0xffff), (int16_t)(p[1] >> 16),
                        fg_rgb555(TRI_HDR_IMM(w[0])));
              }
            break;

          case TRI_OP_SPRITE:
          case TRI_OP_SPRITE_AFFINE:
            if (n >= 5)
              {
                uint32_t attr = p[3];
                int sheet = attr & 15;
                int pal = (attr >> 4) & 15;
                int blend = (attr >> 10) & 3;
                int sx = (int16_t)(p[1] & 0xffff);
                int sy = (int16_t)(p[1] >> 16);
                int cw = (int16_t)(p[2] & 0xffff);
                int ch = (int16_t)(p[2] >> 16);
                FAR struct sheet_s *s = &g_tri.sheet[sheet];
                uint32_t fl;
                int img;

                if (s->w == 0 || sx < 0 || sy < 0 || cw <= 0 || ch <= 0 ||
                    sx + cw > s->w || sy + ch > s->h ||
                    (img = sheet_image(sheet, pal)) < 0)
                  {
                    break;                /* (TRI rejects these too) */
                  }

                fl = ((attr >> 8) & 1 ? FG_HFLIP : 0) |
                     ((attr >> 9) & 1 ? FG_VFLIP : 0) |
                     (blend == TRI_BLEND_HALF ? FG_HALF : 0) |
                     (blend == TRI_BLEND_ADD ? FG_ADD : 0);
                if (TRI_HDR_OP(w[0]) == TRI_OP_SPRITE)
                  {
                    fg_sprite(img, sx, sy, cw, ch, (int16_t)(p[0] & 0xffff),
                              (int16_t)(p[0] >> 16), fl);
                  }
                else if (n >= 7)
                  {
                    fg_sprite_matrix(img, sx, sy, cw, ch,
                                     (int16_t)(p[0] & 0xffff),
                                     (int16_t)(p[0] >> 16), fl,
                                     (int16_t)(p[4] & 0xffff),
                                     (int16_t)(p[4] >> 16),
                                     (int16_t)(p[5] & 0xffff),
                                     (int16_t)(p[5] >> 16));
                  }
              }
            break;

          default:
            break;
        }

      i += n;
    }
}

static void render_frame(void)
{
  int n;

  fg_clear(fg_rgb555(g_tri.backdrop));
  for (n = 0; n < TRI_LAYERS; n++)
    {
      if (CFG_ON(g_tri.layer[n].cfg) && CFG_PRIO(g_tri.layer[n].cfg) == 0)
        {
          layer_draw(n);
        }
    }

  draw_objects();
  for (n = 0; n < TRI_LAYERS; n++)
    {
      if (CFG_ON(g_tri.layer[n].cfg) && CFG_PRIO(g_tri.layer[n].cfg) == 1)
        {
          layer_draw(n);
        }
    }
}

/****************************************************************************
 * TRI's runtime: command recording (tri_sdk_internal.h)
 ****************************************************************************/

void tri_emit(int dest, const uint32_t *words, int n)
{
  int i = 0;

  while (i < n)
    {
      int k = 1 + TRI_HDR_NWORDS(words[i]);

      if (i + k > n)
        {
          break;
        }

      exec(dest, words + i, k);
      i += k;
    }
}

void tri_emit_deliver(int dest, const uint32_t *words, int n)
{
  tri_emit(dest, words, n);
}

void tri_flush(int dest)
{
}

void tri_link_quiesce(void)
{
}

void tri_api_error(const char *fn, const char *what)
{
  g_tri.st.api_errors++;
  if (g_tri.api_errors_shown < 8)
    {
      g_tri.api_errors_shown++;
      syslog(LOG_WARNING, "tri: %s: %s\n", fn, what);
    }
}

/****************************************************************************
 * TRI's runtime: the rest of tri_sdk.h
 ****************************************************************************/

int tri_video_mode(void)
{
  return g_tri.mode;
}

void tri_video_mode_set(int mode)
{
  if (mode != g_tri.mode)
    {
      tri_api_error("tri_video_mode_set", "the FPC keeps the game's mode");
    }
}

bool tri_video_mode_save(void)
{
  return false;
}

const char *tri_video_mode_name(int mode)
{
  return mode == TRI_VIDEO_CLASSIC ? "4:3 320x240" :
         mode == TRI_VIDEO_WIDE ? "16:9 360x240" :
         mode == TRI_VIDEO_SQUARE ? "16:9 424x240" : "?";
}

int tri_video_mode_width(int mode)
{
  return mode >= 0 && mode < TRI_VIDEO_MODES ? TRI_VIDEO_W(mode) :
         TRI_VIDEO_W(g_tri.mode);
}

int tri_screen_width(void)
{
  return TRI_VIDEO_W(g_tri.mode);
}

int tri_screen_height(void)
{
  return TRI_RENDER_H;
}

int tri_text_columns(void)
{
  return TRI_VIDEO_W(g_tri.mode) / 8;
}

int tri_text_rows(void)
{
  return TRI_RENDER_H / 8;
}

uint32_t tri_frame_count(void)
{
  return fg_frame();
}

uint32_t tri_time_us(void)
{
  return fg_time_us();
}

uint32_t tri_time_ms(void)
{
  return fg_time_ms();
}

bool tri_connected(void)
{
  return true;
}

const tri_stats_t *tri_stats(void)
{
  FAR const struct fg_stats_s *f = fg_stats();

  g_tri.st.frame             = fg_frame();
  g_tri.st.vsync_period_us   = 16667;
  g_tri.st.frame_time_us     = f->frame_us;
  g_tri.st.frame_time_max_us = f->frame_us_max;
  g_tri.st.lists_sent        = f->frames;
  g_tri.st.reconnects        = 1;
  g_tri.st.connected         = true;
  g_tri.st.pad_present       = true;
  g_tri.st.video_mode        = (uint8_t)g_tri.mode;
  return &g_tri.st;
}

uint32_t tri_random(void)
{
  return fg_random();
}

int tri_random_range(int lo, int hi)
{
  return fg_random_range(lo, hi);
}

void tri_random_seed(uint32_t seed)
{
  fg_random_seed(seed);
}

/* The pad: the SDK's (the same bits as TRI_PAD_*) */

uint32_t tri_pad_buttons(void)
{
  return fg_pad() & 0x1ff;
}

uint32_t tri_pad_pressed(void)
{
  return fg_pad_pressed() & 0x1ff;
}

uint32_t tri_pad_released(void)
{
  return fg_pad_released() & 0x1ff;
}

uint32_t tri_pad_stick(void)
{
  return fg_pad() & (TRI_PAD_UP | TRI_PAD_DOWN | TRI_PAD_LEFT | TRI_PAD_RIGHT);
}

int tri_pad_axis_x(void)
{
  return fg_axis_x();
}

int tri_pad_axis_y(void)
{
  return fg_axis_y();
}

bool tri_pad_present(void)
{
  return true;
}

size_t tri_psram_size(void)
{
  return 0;                               /* (TRI_PSRAM arrays are plain memory here) */
}

size_t tri_psram_free(void)
{
  return 0;
}

void *tri_psram_alloc(size_t bytes)
{
  return NULL;
}

void tri_psram_flush(void)
{
}

void tri_overlay_show(bool show)
{
}

bool tri_overlay_visible(void)
{
  return false;
}

void tri_os_exit(void)
{
  fg_exit();
}

int tri_rom_scan(tri_rom_entry_t *out, int max)
{
  return 0;
}

bool tri_rom_read(int slot, tri_rom_entry_t *out)
{
  return false;
}

void tri_rom_launch(int slot)
{
}

void tri_os_run(const tri_game_t *menu)
{
  tri_game_run(menu);
}

/****************************************************************************
 * The frame loop: the game's callbacks inside the SDK's
 ****************************************************************************/

static void tri_load(void)
{
  int i;

  g_tri.v[TRI_DEST_VDP1].sram_size = V1_SRAM;
  g_tri.v[TRI_DEST_VDP2].sram_size = V2_SRAM;
  for (i = 0; i < 16; i++)
    {
      memset(g_tri.sheet[i].img, -1, sizeof(g_tri.sheet[i].img));
    }

  for (i = 0; i < 8; i++)
    {
      memset(g_tri.tileset[i].img, -1, sizeof(g_tri.tileset[i].img));
    }

  fg_audio_vram(v2_base, v2_range);
  tri_gfx_reset();
  tri_text_reset();
  tri_audio_reset();
  if (g_tri.game->load != NULL)
    {
      g_tri.game->load();
    }
}

static void tri_init(void)
{
  if (g_tri.game->init != NULL)
    {
      g_tri.game->init();
    }
}

static void tri_update(void)
{
  uint32_t fb = TRI_HDR(TRI_OP_FRAME_BEGIN, 0, 0);

  /* Holding D for a second leaves, as on TRI (Escape too: the SDK's) */

  g_tri.hold_d = (fg_pad() & TRI_PAD_D) ? g_tri.hold_d + 1 : 0;
  if (g_tri.hold_d >= HOLD_D_FRAMES)
    {
      fg_exit();
    }

  tri_emit(TRI_DEST_VDP1, &fb, 1);
  if (g_tri.game->update != NULL)
    {
      g_tri.game->update();
    }

  tri_music_tick();
}

static void tri_draw(void)
{
  uint32_t fe = TRI_HDR(TRI_OP_FRAME_END, 0, 0);

  if (g_tri.game->draw != NULL)
    {
      g_tri.game->draw();
    }

  tri_emit(TRI_DEST_VDP1, &fe, 1);
  render_frame();
}

static void tri_quit(void)
{
  int i;

  for (i = 0; i < TRI_LAYERS; i++)
    {
      free(g_tri.layer[i].drawn);
    }

  free(g_tri.v[TRI_DEST_VDP1].sram);
  free(g_tri.v[TRI_DEST_VDP2].sram);
  free(g_tri.v[TRI_DEST_VDP1].psram);
  free(g_tri.v[TRI_DEST_VDP2].psram);
  free(g_tri.obj);
}

int tri_game_main(const tri_game_t *game, int argc, char *argv[])
{
  if (argc == 3 && strcmp(argv[1], "-s") == 0)
    {
      fg_script(argv[2]);
    }
  else if (argc != 1)
    {
      fprintf(stderr, "usage: %s [-s SCRIPT]   (fpcgame.h: fg_script())\n",
              argv[0]);
      return 1;
    }

  return tri_game_run(game) < 0 ? 1 : 0;
}

int tri_game_run(const tri_game_t *game)
{
  struct fg_game_s g;
  int ret;

  memset(&g_tri, 0, sizeof(g_tri));
  g_tri.game = game;
  g_tri.mode = game->video_mode < TRI_VIDEO_MODES ? game->video_mode :
               TRI_VIDEO_DEFAULT;
  g_tri.v[TRI_DEST_VDP1].sram = calloc(1, V1_SRAM);
  g_tri.v[TRI_DEST_VDP2].sram = calloc(1, V2_SRAM);
  if (g_tri.v[TRI_DEST_VDP1].sram == NULL ||
      g_tri.v[TRI_DEST_VDP2].sram == NULL)
    {
      tri_quit();
      return -ENOMEM;
    }

  memset(&g, 0, sizeof(g));
  g.name   = game->name;
  g.width  = TRI_VIDEO_W(g_tri.mode);
  g.height = TRI_RENDER_H;
  g.load   = tri_load;
  g.init   = tri_init;
  g.update = tri_update;
  g.draw   = tri_draw;
  g.quit   = tri_quit;

  ret = fg_run(&g);
  if (ret < 0)
    {
      tri_quit();                         /* (quit() runs only for a game that ran) */
    }

  return ret;
}
