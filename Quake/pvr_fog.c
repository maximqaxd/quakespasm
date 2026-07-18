/*
================================================================================
pvr_fog.c -- hardware table fog for the native PVR renderer (maximqad)

Quake's fog (gl_fog.c) is GL_EXP2 with color + density from the map's worldspawn
(and smoothly interpolated on fog changes). On the PVR we use the hardware's
TABLE fog: the TA blends each pixel toward the fog color by a factor looked up
from the vertex's 1/w -- exactly the depth we already submit -- so fog is free
per vertex; only the poly header carries the enable (pvr_context sets
gen.fog_type when pvr_fog_active).

Mapping matches GLdc's glFog path (which QuakeSpasm-on-GLdc drove the same way):
GL_EXP2 density -> pvr_fog_table_exp2, color -> pvr_fog_table_color, and Fog_
SetupFrame feeds Fog_GetDensity()/64. The table is rebuilt each frame so fog
fades follow; it's only ~128 entries and only when the map actually has fog.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

qboolean	pvr_fog_active;		// read by pvr_context.c PVR_FlushState

/*
==============
PVR_SetupFog -- called once per frame from Fog_SetupFrame

density is the GL_EXP2 density (Fog_GetDensity()/64, as the GL path uses); r/g/b
is the fog color. density <= 0 means the map has no fog -> disable.
==============
*/
void PVR_SetupFog (float density, float r, float g, float b)
{
	if (density <= 0.0f)
	{
		pvr_fog_active = false;
		return;
	}

	pvr_fog_table_exp2 (density);		// builds the exp2 table + sets FOG_DENSITY
	pvr_fog_table_color (1.0f, r, g, b);	// (alpha, r, g, b)
	pvr_fog_active = true;
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
