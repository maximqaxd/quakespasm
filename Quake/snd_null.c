/*
 * snd_null.c -- null SNDDMA backend.
 *
 * Implements the sound-driver interface as no-ops. SNDDMA_Init() returns false,
 * so snd_dma.c brings the engine up with sound disabled. Used on Dreamcast to
 * take the audio path out of the picture while bringing up the renderer.
 */

#include "quakedef.h"

qboolean SNDDMA_Init (dma_t *dma)
{
	(void) dma;
	return false;	/* no sound device */
}

int SNDDMA_GetDMAPos (void)
{
	return 0;
}

void SNDDMA_Shutdown (void)
{
}

void SNDDMA_LockBuffer (void)
{
}

void SNDDMA_Submit (void)
{
}

void SNDDMA_BlockSound (void)
{
}

void SNDDMA_UnblockSound (void)
{
}
