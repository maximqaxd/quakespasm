/*
================================================================================
pvr_alloc.c -- VRAM texture allocator for the PVR renderer

The PVR samples textures from VRAM (pvr_mem). Plain pvr_mem_malloc works, but it
has no 2K-page awareness and fragments badly as brush/model textures churn on map
changes -- which starves large allocations (mip chains) even when the total free
byte count looks fine. This is a page-aware sub-allocator: we grab one big block
from pvr_mem at init and hand out 256-byte-granular slices from it, keeping every
allocation >= 2K aligned to a 2K boundary and never letting a small allocation
straddle a 2K page. The PVR fetches faster when a texture doesn't cross a 2K
address, and VQ codebooks land on their own page.

Ported from the xash3d_dc PVR reference allocator (GLdc lineage). Housekeeping
(a 1-byte-per-2K-block usage bitmask) lives in main RAM so we never read back from
VRAM to check occupancy.
================================================================================
*/
#include "pvr_local.h"

#if defined(PLATFORM_DREAMCAST) && defined(USE_PVR_RENDER)

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdlib.h>

// Reserve a little headroom so KOS internal PVR allocations (framebuffers,
// vertex buffers already taken before we grab the pool) never collide with us.
#define PVR_MEM_BUFFER_SIZE (64 * 1024)

#define PVR_RAM_BLOCK_SIZE  (2 * 1024)
#define PVR_RAM_MAX_MB      16
#define PVR_RAM_MAX_BYTES   ((size_t)PVR_RAM_MAX_MB * 1024 * 1024)
#define BLOCK_COUNT         (PVR_RAM_MAX_BYTES / PVR_RAM_BLOCK_SIZE)

static void *vram_alloc_base = NULL;	// the raw pvr_mem block backing the pool

static inline intptr_t round_up (intptr_t n, int multiple)
{
	if ((n % multiple) == 0)
		return n;
	return ((n + multiple - 1) / multiple) * multiple;
}

struct AllocEntry {
	void			*pointer;
	size_t			 size;
	struct AllocEntry	*next;
};

typedef struct {
	// One usage byte per 2K block: each of the 8 bits marks a 256-byte
	// sub-block (255 = full, 0 = free).
	uint8_t		 block_usage[BLOCK_COUNT];
	uint8_t		*pool;			// pointer to the memory pool
	size_t		 pool_size;		// size of the memory pool
	uint8_t		*base_address;		// first 2K-aligned address in the pool
	size_t		 block_count;		// number of 2K blocks in the pool
	struct AllocEntry *allocations;		// live allocations, ordered by size desc
} PoolHeader;

static PoolHeader pool_header = { {0}, NULL, 0, NULL, 0, NULL };

static inline void *calc_address (uint8_t *block_usage_iterator, int bit_offset,
				  size_t required_subblocks, size_t *start_subblock_out)
{
	uintptr_t offset = (block_usage_iterator - pool_header.block_usage) * 8;
	offset += (bit_offset + 1);
	offset -= required_subblocks;

	if (start_subblock_out)
		*start_subblock_out = offset;

	return pool_header.base_address + (offset * 256);
}

static void *alloc_next_available_ex (size_t required_size, size_t *start_subblock_out,
				      size_t *required_subblocks_out)
{
	uint8_t *it = pool_header.block_usage;
	uint32_t required_subblocks = (required_size / 256);
	if (required_size % 256)
		required_subblocks += 1;

	// Anything >= a full block must be aligned to a block boundary.
	bool requires_alignment = required_size >= PVR_RAM_BLOCK_SIZE;

	if (required_subblocks_out)
		*required_subblocks_out = required_subblocks;

	// Fallback slot: a candidate that fits but isn't ideally aligned. Used only
	// if we reach the end of the search without finding anything better.
	uint8_t *poor_option = NULL;
	size_t poor_start_subblock = 0;

	uint32_t found_subblocks = 0;
	uint32_t found_poor_subblocks = 0;
	size_t j;

	for (j = 0; j < pool_header.block_count; ++j, ++it)
	{
		if (found_subblocks < required_subblocks)
		{
			uint8_t t = *it;

			if (t == 255)	// full block: skip
			{
				found_subblocks = 0;
				found_poor_subblocks = 0;
			}
			else
			{
				int i;
				for (i = 0; i < 8; ++i)
				{
					if ((t & 0x80) == 0)
					{
						bool block_overflow = (
							required_size < PVR_RAM_BLOCK_SIZE && found_subblocks > 0 && i == 0);

						bool reset_subblocks = (
							(requires_alignment && found_subblocks == 0 && i != 0) ||
							block_overflow);

						if (reset_subblocks)
							found_subblocks = 0;
						else
							found_subblocks++;

						if (block_overflow)
							found_subblocks++;

						found_poor_subblocks++;

						if (found_subblocks >= required_subblocks)
							return calc_address (it, i, required_subblocks, start_subblock_out);

						if (!poor_option && (found_poor_subblocks >= required_subblocks))
							poor_option = calc_address (it, i, required_subblocks, &poor_start_subblock);
					}
					else
					{
						found_subblocks = 0;
						found_poor_subblocks = 0;
					}

					t <<= 1;
				}
			}
		}
	}

	if (poor_option)
	{
		if (start_subblock_out)
			*start_subblock_out = poor_start_subblock;
		return poor_option;
	}

	return NULL;
}

static int alloc_init (void *pool, size_t size)
{
	uint8_t *p = (uint8_t *) pool;
	intptr_t base_address;

	if (pool_header.pool)
		return -1;
	if (size == 0 || size > PVR_RAM_MAX_BYTES)
		return -1;

	memset (pool_header.block_usage, 0, sizeof (pool_header.block_usage));
	pool_header.pool = pool;

	base_address = (intptr_t) pool_header.pool;
	base_address = round_up (base_address, PVR_RAM_BLOCK_SIZE);

	pool_header.base_address = (uint8_t *) base_address;
	pool_header.block_count = ((p + size) - pool_header.base_address) / PVR_RAM_BLOCK_SIZE;
	if (pool_header.block_count > BLOCK_COUNT)
		pool_header.block_count = BLOCK_COUNT;

	// The usable pool may be smaller than the requested size if the memory
	// wasn't 2K-aligned or we hit BLOCK_COUNT.
	pool_header.pool_size = pool_header.block_count * PVR_RAM_BLOCK_SIZE;
	pool_header.allocations = NULL;

	return 0;
}

static void alloc_shutdown (void)
{
	struct AllocEntry *it;

	if (!pool_header.pool)
		return;

	it = pool_header.allocations;
	while (it)
	{
		struct AllocEntry *next = it->next;
		free (it);
		it = next;
	}

	memset (&pool_header, 0, sizeof (pool_header));
	pool_header.pool = NULL;
}

static inline uint32_t size_to_subblock_count (size_t size)
{
	uint32_t required_subblocks = (size / 256);
	if (size % 256)
		required_subblocks += 1;
	return required_subblocks;
}

static inline uint32_t subblock_from_pointer (void *p)
{
	uint8_t *ptr = (uint8_t *) p;
	return (ptr - pool_header.base_address) / 256;
}

static inline void block_and_offset_from_subblock (size_t sb, size_t *b, uint8_t *off)
{
	*b = sb / 8;
	*off = (sb % 8);
}

static void *alloc_malloc (size_t size)
{
	size_t start_subblock, required_subblocks;
	void *ret = alloc_next_available_ex (size, &start_subblock, &required_subblocks);

	if (ret)
	{
		size_t block;
		uint8_t offset;
		uint8_t mask = 0;
		int c, i;
		struct AllocEntry *new_entry, *it, *last;

		block_and_offset_from_subblock (start_subblock, &block, &offset);

		// Mark the leading (possibly partial) block.
		c = (required_subblocks < 8) ? (int) required_subblocks : 8;
		for (i = 0; i < c; ++i)
		{
			mask |= (1 << (7 - (offset + i)));
			required_subblocks--;
		}
		if (mask)
			pool_header.block_usage[block++] |= mask;

		// Fill whole blocks in the middle.
		while (required_subblocks > 8)
		{
			pool_header.block_usage[block++] = 255;
			required_subblocks -= 8;
		}

		// Fill any trailing sub-blocks.
		mask = 0;
		for (i = 0; i < (int) required_subblocks; ++i)
			mask |= (1 << (7 - i));
		if (mask)
			pool_header.block_usage[block++] |= mask;

		// Insert into the list ordered by size descending.
		new_entry = (struct AllocEntry *) malloc (sizeof (struct AllocEntry));
		new_entry->pointer = ret;
		new_entry->size = size;
		new_entry->next = NULL;

		it = pool_header.allocations;
		last = NULL;

		if (!it)
		{
			pool_header.allocations = new_entry;
		}
		else
		{
			while (it)
			{
				if (it->size < size)
				{
					if (last)
						last->next = new_entry;
					else
						pool_header.allocations = new_entry;
					new_entry->next = it;
					break;
				}
				else if (!it->next)
				{
					it->next = new_entry;
					new_entry->next = NULL;
					break;
				}
				last = it;
				it = it->next;
			}
		}
	}

	return ret;
}

static void alloc_release_blocks (struct AllocEntry *it)
{
	size_t used_subblocks = size_to_subblock_count (it->size);
	size_t subblock = subblock_from_pointer (it->pointer);
	size_t block;
	uint8_t offset;
	uint8_t mask = 0;
	int c, i;

	block_and_offset_from_subblock (subblock, &block, &offset);

	c = (used_subblocks < 8) ? (int) used_subblocks : 8;
	for (i = 0; i < c; ++i)
	{
		mask |= (1 << (7 - (offset + i)));
		used_subblocks--;
	}
	if (mask)
		pool_header.block_usage[block++] &= ~mask;

	while (used_subblocks > 8)
	{
		pool_header.block_usage[block++] = 0;
		used_subblocks -= 8;
	}

	mask = 0;
	for (i = 0; i < (int) used_subblocks; ++i)
		mask |= (1 << (7 - i));
	if (mask)
		pool_header.block_usage[block++] &= ~mask;
}

static void alloc_free (void *p)
{
	struct AllocEntry *it = pool_header.allocations;
	struct AllocEntry *last = NULL;

	while (it)
	{
		if (it->pointer == p)
		{
			alloc_release_blocks (it);

			if (last)
				last->next = it->next;
			else
				pool_header.allocations = it->next;

			free (it);
			return;
		}
		last = it;
		it = it->next;
	}
	// Freeing an unknown pointer means heap corruption; ignore quietly.
}

static inline uint8_t count_ones (uint8_t byte)
{
	static const uint8_t NIBBLE_LOOKUP[16] = {
		0, 1, 1, 2, 1, 2, 2, 3, 1, 2, 2, 3, 2, 3, 3, 4
	};
	return NIBBLE_LOOKUP[byte & 0x0F] + NIBBLE_LOOKUP[byte >> 4];
}

static size_t alloc_count_free (void)
{
	size_t total_used = 0;
	size_t i;

	for (i = 0; i < pool_header.block_count; ++i)
		total_used += count_ones (pool_header.block_usage[i]) * 256;

	return pool_header.pool_size - total_used;
}

static size_t alloc_count_continuous (void)
{
	size_t contiguous_free = 0;
	size_t most_contiguous = 0;
	size_t i;

	for (i = 0; i < pool_header.block_count; ++i)
	{
		uint8_t t = pool_header.block_usage[i];
		int b;
		for (b = 7; b >= 0; --b)
		{
			if (!(t & (1 << b)))
			{
				++contiguous_free;
			}
			else
			{
				if (contiguous_free > most_contiguous)
					most_contiguous = contiguous_free;
				contiguous_free = 0;
			}
		}
	}

	if (contiguous_free > most_contiguous)
		most_contiguous = contiguous_free;

	return most_contiguous * 256;
}

//==============================================================================
// Public renderer API
//==============================================================================

void PVR_TexAlloc_Init (void)
{
	size_t vram_free, alloc_size;

	if (vram_alloc_base)
		return;					// already initialised

	vram_free = pvr_mem_available ();
	alloc_size = (vram_free > PVR_MEM_BUFFER_SIZE) ? (vram_free - PVR_MEM_BUFFER_SIZE) : vram_free;

	vram_alloc_base = pvr_mem_malloc (alloc_size);
	if (!vram_alloc_base)
	{
		Con_Printf ("PVR_TexAlloc_Init: could not grab %u KB VRAM pool -- falling back to pvr_mem\n",
			    (unsigned)(alloc_size / 1024));
		return;
	}

	if (alloc_init (vram_alloc_base, alloc_size) != 0)
	{
		Con_Printf ("PVR_TexAlloc_Init: allocator init failed -- falling back to pvr_mem\n");
		pvr_mem_free (vram_alloc_base);
		vram_alloc_base = NULL;
		return;
	}

	Con_Printf ("PVR VRAM pool: %u KB (%u blocks of 2K)\n",
		    (unsigned)(pool_header.pool_size / 1024),
		    (unsigned)pool_header.block_count);
}

void PVR_TexAlloc_Shutdown (void)
{
	if (!vram_alloc_base)
		return;
	alloc_shutdown ();
	pvr_mem_free (vram_alloc_base);
	vram_alloc_base = NULL;
}

/*
==============
PVR_VramAlloc / PVR_VramFree

Front the page-aware pool when it's up; otherwise fall back to raw pvr_mem so the
renderer keeps working even if the pool couldn't be created.
==============
*/
void *PVR_VramAlloc (unsigned bytes)
{
	if (!bytes)
		return NULL;
	if (vram_alloc_base)
		return alloc_malloc ((size_t) bytes);
	return pvr_mem_malloc (bytes);
}

void PVR_VramFree (void *ptr)
{
	if (!ptr)
		return;
	if (vram_alloc_base)
		alloc_free (ptr);
	else
		pvr_mem_free (ptr);
}

/*
==============
PVR_VramFreeBytes / PVR_VramLargestFreeBytes

Reporting helpers. When the pool is active these reflect the sub-allocator's own
accounting (total free, and the largest contiguous run); otherwise they fall back
to KOS pvr_mem_available.
==============
*/
size_t PVR_VramFreeBytes (void)
{
	if (vram_alloc_base)
		return alloc_count_free ();
	return pvr_mem_available ();
}

size_t PVR_VramLargestFreeBytes (void)
{
	if (vram_alloc_base)
		return alloc_count_continuous ();
	return pvr_mem_available ();
}

size_t PVR_VramUsedBytes (void)
{
	if (vram_alloc_base)
		return pool_header.pool_size - alloc_count_free ();
	return 0;
}

size_t PVR_VramPoolBytes (void)
{
	if (vram_alloc_base)
		return pool_header.pool_size;
	return 0;
}

#endif	/* PLATFORM_DREAMCAST && USE_PVR_RENDER */
