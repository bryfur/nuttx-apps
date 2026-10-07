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
// r_surf.c: surface-related refresh code

#include "quakedef.h"
#include "r_local.h"
#include "../fpc_sio.h"	// FPC: the blocks on the interpolators

drawsurf_t	r_drawsurf;

int				lightleft, sourcesstep, blocksize, sourcetstep;
int				lightdelta, lightdeltastep;
int				lightright, lightleftstep, lightrightstep, blockdivshift;
unsigned		blockdivmask;
void			*prowdestbase;
unsigned char	*pbasesource;
int				surfrowbytes;	// used by ASM files
unsigned		*r_lightptr;
int				r_stepback;
int				r_lightwidth;
int				r_numhblocks, r_numvblocks;
unsigned char	*r_source, *r_sourcemax;

FPCQ_FAST void R_DrawSurfaceBlock8_mip0 (rblock_t *rb);
FPCQ_FAST void R_DrawSurfaceBlock8_mip1 (rblock_t *rb);
FPCQ_FAST void R_DrawSurfaceBlock8_mip2 (rblock_t *rb);
FPCQ_FAST void R_DrawSurfaceBlock8_mip3 (rblock_t *rb);

static void	(*surfmiptable[4])(rblock_t *rb) = {
	R_DrawSurfaceBlock8_mip0,
	R_DrawSurfaceBlock8_mip1,
	R_DrawSurfaceBlock8_mip2,
	R_DrawSurfaceBlock8_mip3
};



unsigned		blocklights[18*18];

/*
===============
R_AddDynamicLights
===============
*/
void R_AddDynamicLights (void)
{
	msurface_t *surf;
	int			lnum;
	int			sd, td;
	float		dist, rad, minlight;
	vec3_t		impact, local;
	int			s, t;
	int			i;
	int			smax, tmax;
	mtexinfo_t	*tex;

	surf = r_drawsurf.surf;
	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	tex = surf->texinfo;

	for (lnum=0 ; lnum<MAX_DLIGHTS ; lnum++)
	{
		if ( !(surf->dlightbits & (1<<lnum) ) )
			continue;		// not lit by this light

		rad = cl_dlights[lnum].radius;
		dist = DotProduct (cl_dlights[lnum].origin, surf->plane->normal) -
				surf->plane->dist;
		rad -= fabs(dist);
		minlight = cl_dlights[lnum].minlight;
		if (rad < minlight)
			continue;
		minlight = rad - minlight;

		for (i=0 ; i<3 ; i++)
		{
			impact[i] = cl_dlights[lnum].origin[i] -
					surf->plane->normal[i]*dist;
		}

		local[0] = DotProduct (impact, tex->vecs[0]) + tex->vecs[0][3];
		local[1] = DotProduct (impact, tex->vecs[1]) + tex->vecs[1][3];

		local[0] -= surf->texturemins[0];
		local[1] -= surf->texturemins[1];
		
		for (t = 0 ; t<tmax ; t++)
		{
			td = local[1] - t*16;
			if (td < 0)
				td = -td;
			for (s=0 ; s<smax ; s++)
			{
				sd = local[0] - s*16;
				if (sd < 0)
					sd = -sd;
				if (sd > td)
					dist = sd + (td>>1);
				else
					dist = td + (sd>>1);
				if (dist < minlight)
					blocklights[t*smax + s] += (rad - dist)*256;
			}
		}
	}
}

/*
===============
R_BuildLightMap

Combine and scale multiple lightmaps into the 8.8 format in blocklights
===============
*/
void R_BuildLightMap (void)
{
	int			smax, tmax;
	int			t;
	int			i, size;
	byte		*lightmap;
	unsigned	scale;
	int			maps;
	msurface_t	*surf;

	surf = r_drawsurf.surf;

	smax = (surf->extents[0]>>4)+1;
	tmax = (surf->extents[1]>>4)+1;
	size = smax*tmax;
	lightmap = surf->samples;

	if (r_fullbright.value || !cl.worldmodel->lightdata)
	{
		for (i=0 ; i<size ; i++)
			blocklights[i] = 0;
		return;
	}

// clear to ambient
	for (i=0 ; i<size ; i++)
		blocklights[i] = r_refdef.ambientlight<<8;


// add all the lightmaps
	if (lightmap)
		for (maps = 0 ; maps < MAXLIGHTMAPS && surf->styles[maps] != 255 ;
			 maps++)
		{
			scale = r_drawsurf.lightadj[maps];	// 8.8 fraction		
			for (i=0 ; i<size ; i++)
				blocklights[i] += lightmap[i] * scale;
			lightmap += size;	// skip to next lightmap
		}

// add all the dynamic lights
	if (surf->dlightframe == r_framecount)
		R_AddDynamicLights ();

// bound, invert, and shift
	for (i=0 ; i<size ; i++)
	{
		t = (255*256 - (int)blocklights[i]) >> (8 - VID_CBITS);

		if (t < (1 << 6))
			t = (1 << 6);

		blocklights[i] = t;
	}
}


/*
===============
R_TextureAnimation

Returns the proper texture for a given time and base texture
===============
*/
texture_t *R_TextureAnimation (texture_t *base)
{
	return R_TextureAnimationEnt (base, currententity);
}

// FPC: for the given entity (D_DrawSurfaces looks for a surface's cache
// block before it makes the surface's entity the current one)
texture_t *R_TextureAnimationEnt (texture_t *base, entity_t *ent)
{
	int		reletive;
	int		count;

	if (ent->frame)
	{
		if (base->alternate_anims)
			base = base->alternate_anims;
	}
	
	if (!base->anim_total)
		return base;

	reletive = (int)(cl.time*10) % base->anim_total;

	count = 0;	
	while (base->anim_min > reletive || base->anim_max <= reletive)
	{
		base = base->anim_next;
		if (!base)
			Sys_Error ("R_TextureAnimation: broken cycle");
		if (++count > 100)
			Sys_Error ("R_TextureAnimation: infinite cycle");
	}

	return base;
}


/*
===============
R_DrawSurface
===============
*/
FPCQ_FAST void R_DrawSurface (void)
{
	// FPC: the columns' parts (the other core may draw some, fpc_drawq.c)
	static rcols_t	cols;
	unsigned char	*basetptr;
	int				smax, tmax, twidth;
	int				u;
	int				soffset, basetoffset, texwidth;
	texture_t		*mt;

// calculate the lightings
	R_BuildLightMap ();
	
	surfrowbytes = r_drawsurf.rowbytes;

	mt = r_drawsurf.texture;
	
	r_source = (byte *)mt + mt->offsets[r_drawsurf.surfmip];
	
// the fractional light values should range from 0 to (VID_GRADES - 1) << 16
// from a source range of 0 - 255
	
	texwidth = mt->width >> r_drawsurf.surfmip;

	blocksize = 16 >> r_drawsurf.surfmip;
	blockdivshift = 4 - r_drawsurf.surfmip;
	blockdivmask = (1 << blockdivshift) - 1;
	
	r_lightwidth = (r_drawsurf.surf->extents[0]>>4)+1;

	r_numhblocks = r_drawsurf.surfwidth >> blockdivshift;
	r_numvblocks = r_drawsurf.surfheight >> blockdivshift;

//==============================

	smax = mt->width >> r_drawsurf.surfmip;
	twidth = texwidth;
	tmax = mt->height >> r_drawsurf.surfmip;
	sourcetstep = texwidth;
	r_stepback = tmax * twidth;

	r_sourcemax = r_source + (tmax * smax);

	soffset = r_drawsurf.surf->texturemins[0];
	basetoffset = r_drawsurf.surf->texturemins[1];

// << 16 components are to guarantee positive values for %
	soffset = ((soffset >> r_drawsurf.surfmip) + (smax << 16)) % smax;
	basetptr = &r_source[((((basetoffset >> r_drawsurf.surfmip) 
		+ (tmax << 16)) % tmax) * twidth)];

	// FPC: each column of blocks as the loop here made it (fpcq_cols_draw():
	// on both cores when the other one is free)
	cols.proto.numvblocks = r_numvblocks;
	cols.proto.lightwidth = r_lightwidth;
	cols.proto.sourcetstep = sourcetstep;
	cols.proto.surfrowbytes = surfrowbytes;
	cols.proto.stepback = r_stepback;
	cols.proto.sourcemax = r_sourcemax;
	cols.mip = r_drawsurf.surfmip;
	cols.n = r_numhblocks;
	cols.blocksize = blocksize;
	cols.dest = r_drawsurf.surfdat;
	cols.basetptr = basetptr;
	if (cols.n > RCOLS_MAX)
		Sys_Error ("R_DrawSurface: %d columns", cols.n);

	for (u=0 ; u<r_numhblocks; u++)
	{
		cols.soffset[u] = soffset;

		soffset = soffset + blocksize;
		if (soffset >= smax)
			soffset = 0;
	}

	fpcq_cols_draw (&cols);
}

// FPC: one column of a surface's blocks (R_DrawSurface's loop), on this
// core's interpolators if it has them (fpc_sio.c)
void R_DrawSurfaceColumn (const rcols_t *c, int u)
{
	rblock_t	rb = c->proto;
	void		(*pblockdrawer)(rblock_t *) = surfmiptable[c->mip];

#ifdef FPCQ_SIO
	if (fpcq_sio_here() & FPCQ_SIO_BLOCKS)
		pblockdrawer = fpcq_sio_blocks[c->mip];
#endif
	rb.lightptr = blocklights + u;
	rb.prowdest = c->dest + u * c->blocksize;
	rb.psource = c->basetptr + c->soffset[u];
	(*pblockdrawer)(&rb);
}

//=============================================================================

/*
================
R_DrawSurfaceBlock8_mip0..3

FPC: the four as one body (the block's size 16 >> mip), their state in
rb (the column's, R_DrawSurfaceColumn) rather than in globals, what they
left in the globals left in it; the same arithmetic
================
*/
static inline __attribute__((always_inline))
void R_DrawSurfaceBlock8Body (rblock_t *rb, const int shift)
{
	const int		size = 1 << shift;
	int				v, i, b, lightstep, lighttemp, light;
	unsigned char	pix, *psource, *prowdest;
	unsigned		*lightptr = rb->lightptr;
	int				lightleft, lightright, lightleftstep, lightrightstep;

	psource = rb->psource;
	prowdest = rb->prowdest;
	lightleft = rb->lightleft;
	lightright = rb->lightright;
	lightleftstep = rb->lightleftstep;
	lightrightstep = rb->lightrightstep;

	for (v=0 ; v<rb->numvblocks ; v++)
	{
	// FIXME: make these locals?
	// FIXME: use delta rather than both right and left, like ASM?
		lightleft = lightptr[0];
		lightright = lightptr[1];
		lightptr += rb->lightwidth;
		lightleftstep = (lightptr[0] - lightleft) >> shift;
		lightrightstep = (lightptr[1] - lightright) >> shift;

		for (i=0 ; i<size ; i++)
		{
			lighttemp = lightleft - lightright;
			lightstep = lighttemp >> shift;

			light = lightright;

			for (b=size-1; b>=0; b--)
			{
				pix = psource[b];
				prowdest[b] = ((unsigned char *)vid.colormap)
						[(light & 0xFF00) + pix];
				light += lightstep;
			}
	
			psource += rb->sourcetstep;
			lightright += lightrightstep;
			lightleft += lightleftstep;
			prowdest += rb->surfrowbytes;
		}

		if (psource >= rb->sourcemax)
			psource -= rb->stepback;
	}

	rb->lightptr = lightptr;
	rb->lightleft = lightleft;
	rb->lightright = lightright;
	rb->lightleftstep = lightleftstep;
	rb->lightrightstep = lightrightstep;
}

FPCQ_FAST void R_DrawSurfaceBlock8_mip0 (rblock_t *rb)
{
	R_DrawSurfaceBlock8Body (rb, 4);
}

FPCQ_FAST void R_DrawSurfaceBlock8_mip1 (rblock_t *rb)
{
	R_DrawSurfaceBlock8Body (rb, 3);
}

FPCQ_FAST void R_DrawSurfaceBlock8_mip2 (rblock_t *rb)
{
	R_DrawSurfaceBlock8Body (rb, 2);
}

FPCQ_FAST void R_DrawSurfaceBlock8_mip3 (rblock_t *rb)
{
	R_DrawSurfaceBlock8Body (rb, 1);
}

//============================================================================

/*
================
R_GenTurbTile
================
*/
void R_GenTurbTile (pixel_t *pbasetex, void *pdest)
{
	int		*turb;
	int		i, j, s, t;
	byte	*pd;
	
	turb = sintable + ((int)(cl.time*SPEED)&(CYCLE-1));
	pd = (byte *)pdest;

	for (i=0 ; i<TILE_SIZE ; i++)
	{
		for (j=0 ; j<TILE_SIZE ; j++)
		{	
			s = (((j << 16) + turb[i & (CYCLE-1)]) >> 16) & 63;
			t = (((i << 16) + turb[j & (CYCLE-1)]) >> 16) & 63;
			*pd++ = *(pbasetex + (t<<6) + s);
		}
	}
}


/*
================
R_GenTile
================
*/
void R_GenTile (msurface_t *psurf, void *pdest)
{
	if (psurf->flags & SURF_DRAWTURB)
	{
		R_GenTurbTile ((pixel_t *)
				((byte *)psurf->texinfo->texture + psurf->texinfo->texture->offsets[0]), pdest);
	}
	else if (psurf->flags & SURF_DRAWSKY)
	{
		R_GenSkyTile (pdest);
	}
	else
	{
		Sys_Error ("Unknown tile type");
	}
}

