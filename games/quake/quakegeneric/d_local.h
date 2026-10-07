/*
Copyright (C) 1996-1997 Id Software, Inc.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  

See the GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.

*/
// d_local.h:  private rasterization driver defs

#include "r_shared.h"

//
// TODO: fine-tune this; it's based on providing some overage even if there
// is a 2k-wide scan, with subdivision every 8, for 256 spans of 12 bytes each
//
#define SCANBUFFERPAD		0x1000

#define R_SKY_SMASK	0x007F0000
#define R_SKY_TMASK	0x007F0000

#define DS_SPAN_LIST_END	-128

#define SURFCACHE_SIZE_AT_320X200	600*1024

typedef struct surfcache_s
{
	struct surfcache_s	*next;
	struct surfcache_s 	**owner;		// NULL is an empty chunk of memory
	int					lightadj[MAXLIGHTMAPS]; // checked for strobe flush
	int					dlight;
	int					size;		// including header
	unsigned			width;
	unsigned			height;		// DEBUG only needed for debug
	float				mipscale;
	struct texture_s	*texture;	// checked for animating textures
	short				gx, gy;		// FPC: in the GPU's atlas (fpc_gpu3d.c)
	unsigned			gen;		// FPC: ... of this atlas generation (0: not)
	int					gdirty;		// FPC: rebuilt since it was uploaded
	byte				data[4];	// width*height elements
} surfcache_t;

// !!! if this is changed, it must be changed in asm_draw.h too !!!
typedef struct sspan_s
{
	int				u, v, count;
} sspan_t;

extern cvar_t	d_subdiv16;

extern float	scale_for_mip;

extern qboolean		d_roverwrapped;
extern surfcache_t	*sc_rover;
extern surfcache_t	*d_initial_rover;

extern float	d_sdivzstepu, d_tdivzstepu, d_zistepu;
extern float	d_sdivzstepv, d_tdivzstepv, d_zistepv;
extern float	d_sdivzorigin, d_tdivzorigin, d_ziorigin;

extern fixed16_t	sadjust, tadjust;
extern fixed16_t	bbextents, bbextentt;


void D_DrawSpans8 (espan_t *pspans);
void D_DrawSpans16 (espan_t *pspans);
void D_DrawZSpans (espan_t *pspans);

// FPC: what D_DrawSpans8 and D_DrawZSpans read, as one value (a surface's
// spans drawn on the other core, fpc_drawq.c)

typedef struct
{
	pixel_t		*cacheblock;
	int			cachewidth;
	pixel_t		*viewbuffer;
	int			screenwidth;
	short		*pzbuffer;
	int			zwidth;
	float		sdivzstepu, tdivzstepu, zistepu;
	float		sdivzstepv, tdivzstepv, zistepv;
	float		sdivzorigin, tdivzorigin, ziorigin;
	fixed16_t	sadjust, tadjust;
	fixed16_t	bbextents, bbextentt;
} dspanctx_t;

void D_SpanCtx (dspanctx_t *c);			// from the globals
void D_DrawSpans8Ctx (const dspanctx_t *c, espan_t *pspan);
void D_DrawZSpansCtx (const dspanctx_t *c, espan_t *pspan);
qboolean fpcq_dq_surface (espan_t *spans, surfcache_t *cache);
qboolean fpcq_dq_zspans (espan_t *spans);
void fpcq_dq_wait (void);
void fpcq_dq_batch_end (void);
void fpcq_dq_guard (const void *p, int n);
qboolean fpcq_dq_room (void);
void fpcq_dq_begin (void);
qboolean fpcq_dq_on (void);
surfcache_t *D_SurfaceCached (msurface_t *surface, int miplevel, texture_t *texture);

// FPC: the world textured by the GPU (fpc_gpu3d.c, fpc_quake.c)
void fpcq_g3_init (void);
void fpcq_g3_report (void);
void fpcq_g3_frame (qboolean warp);
qboolean fpcq_g3_drawing (void);
void fpcq_g3_end (void);
qboolean fpcq_g3_surface (surfcache_t *c);
void fpcq_g3_spans (espan_t *pspan, surfcache_t *c);
void fpcq_g3_spans_ctx (espan_t *pspan, surfcache_t *c, const dspanctx_t *g);
qboolean fpcq_g3_fits (msurface_t *pface, int miplevel);
qboolean fpcq_g3_queue (espan_t *spans, msurface_t *pface, int miplevel,
                        texture_t *texture);
void fpcq_g3_cpu (void);
void fpcq_canvas (int *x, int *y);
const unsigned short *fpcq_base_lut (void);
const unsigned short *fpcq_gpu_lut (void);
void Turbulent8 (espan_t *pspan);
void D_SpriteDrawSpans (sspan_t *pspan);

void D_DrawSkyScans8 (espan_t *pspan);
void D_DrawSkyScans16 (espan_t *pspan);

void R_ShowSubDiv (void);
surfcache_t	*D_CacheSurface (msurface_t *surface, int miplevel);
surfcache_t	*D_CacheSurfaceTex (msurface_t *surface, int miplevel,
                                texture_t *texture);	// FPC
texture_t *R_TextureAnimation (texture_t *base);	// FPC (r_local.h's)
texture_t *R_TextureAnimationEnt (texture_t *base, entity_t *ent);

extern int D_MipLevelForScale (float scale);

extern short *d_pzbuffer;
extern unsigned int d_zrowbytes, d_zwidth;

extern int	*d_pscantable;
extern int	d_scantable[MAXHEIGHT];

extern int	d_vrectx, d_vrecty, d_vrectright_particle, d_vrectbottom_particle;

extern int	d_y_aspect_shift, d_pix_min, d_pix_max, d_pix_shift;

extern pixel_t	*d_viewbuffer;

extern short	*zspantable[MAXHEIGHT];

extern int		d_minmip;
extern float	d_scalemip[3];

extern void (*d_drawspans) (espan_t *pspan);

