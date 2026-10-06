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
// vid_null.c -- null video driver to aid porting efforts

#include "quakedef.h"
#include "d_local.h"

#include "quakegeneric.h"

viddef_t	vid;				// global video state

// FPC: 320 x 200 (shown at 4:3), or -res W's W x 3W/4 (fpc_quake.c); the
// frame and the depth buffer in the SRAM if there is room, else from the
// game's heap

byte	*vid_buffer;
short	*zbuffer;
byte	*surfcache;
size_t	surfcache_size;

void	VID_SetPalette (unsigned char *palette)
{
	// quake generic
	QG_SetPalette(palette);
}

void	VID_ShiftPalette (unsigned char *palette)
{
	// quake generic
	QG_SetPalette(palette);
}

void	VID_Init (unsigned char *palette)
{
	int	w, h;

	fpcq_vid_size (&w, &h);		/* FPC */
	vid.width = vid.conwidth = w;
	vid.height = vid.conheight = h;
	vid.maxwarpwidth = WARP_WIDTH;	/* (r_warpbuffer's: underwater) */
	vid.maxwarpheight = WARP_HEIGHT;
	vid.aspect = fpcq_vid_aspect ();	/* FPC: the frame shown at 4:3 */
	vid.numpages = 1;
	vid.colormap = host_colormap;
	vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));
	vid_buffer = fpcq_fast_alloc (w*h, "the frame");
	zbuffer = fpcq_fast_alloc (w*h*sizeof(short), "the z-buffer");	/* FPC */
	if (zbuffer == NULL)
		Sys_Error ("no memory for the depth buffer");
	vid.buffer = vid.conbuffer = vid_buffer;
	vid.rowbytes = vid.conrowbytes = w;
	
	d_pzbuffer = zbuffer;

	surfcache_size = fpcq_surfcache_size ();	/* FPC: what the heap has room for */
	if (surfcache_size == 0)
		surfcache_size = D_SurfaceCacheForRes(w, h);
	surfcache = malloc(surfcache_size);
	if (surfcache == NULL)
		Sys_Error ("no memory for a surface cache of %d KB", (int)(surfcache_size / 1024));
	D_InitCaches (surfcache, surfcache_size);

	// quake generic
	QG_Init();
}

void	VID_Shutdown (void)
{
		free(surfcache);
}

void	VID_Update (vrect_t *rects)
{
	// quake generic
	QG_DrawFrame(vid.buffer);
}

/*
================
D_BeginDirectRect
================
*/
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
}


/*
================
D_EndDirectRect
================
*/
void D_EndDirectRect (int x, int y, int width, int height)
{
}


