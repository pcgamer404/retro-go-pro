/* Neo Geo sprite pager for mame-go (see README_MAMEGO.md).
 *
 * Neo Geo sprite ROMs (the C ROMs, 4-64 MB) do not fit the 8 MB of PSRAM
 * next to the program and the rest of the board, and the driver decodes
 * every tile in place the first time it draws it. Instead, the first launch
 * converts the sprite regions once into an SD file of already decoded tiles
 * plus their pen_usage table:
 *
 *     <core_sys_directory>/neospr/<game>_gfx<n>.spr
 *     header | pen_usage[tiles] (u32) | tiles[tiles] (128 bytes, decoded)
 *
 * and the renderer reads tiles through a PSRAM cache of 8 KB pages (64
 * tiles), least recently used first; pages used in the current frame are
 * not evicted. Transparent tiles are skipped from pen_usage alone, without
 * touching the card. The region itself stays unallocated (a 16-byte stub
 * with the full length, so the driver's tile count is unchanged).
 */
#include "driver.h"
#include "unzip.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef ESP_PLATFORM
#include <esp_heap_caps.h>
#endif

#define NEOSPR_TILE        128
#define NEOSPR_PAGE_TILES  64
#define NEOSPR_PAGE        (NEOSPR_TILE * NEOSPR_PAGE_TILES)
#define NEOSPR_MAX_REGIONS 2   /* REGION_GFX2 and REGION_GFX3 */
#define NEOSPR_VERSION     1

struct neospr_header
{
	char magic[8];          /* "NEOSPR\0\0" */
	uint32_t version;
	uint32_t region_size;
	uint32_t tiles;
	uint32_t rom_key;       /* sum of the source ROMs' CRCs and lengths */
	uint32_t data_offset;   /* file offset of tile 0 */
	uint32_t reserved[9];
};

struct neospr_region
{
	FILE *f;
	unsigned char *stub;
	uint32_t tiles;
	uint32_t data_offset;
	uint32_t first_page;    /* global page number of this region's tile 0 */
};

static struct neospr_region regions[NEOSPR_MAX_REGIONS];
static int neosnd_alloc(void);
unsigned neosnd_take_misses(void);
static int region_count;

static unsigned char *cache;
static int cache_slots;
static int32_t *page_slot;  /* global page -> slot, -1 when not cached */
static int32_t *slot_page;
static uint32_t *slot_used; /* frame number of the last use */
static uint32_t total_pages, frame, next_victim;
static unsigned misses_frame, misses_total, frames_total, worst_frame;

extern char core_sys_directory[];
extern int neogeo_mvs_vh_start(void);
extern void init_mgd2(void);

/* Pageable only for the Neo Geo video hardware, and not for the mgd2 sets
   whose driver_init reshuffles the whole sprite region in memory. */
int neospr_wanted(int type)
{
	if (type != REGION_GFX2 && type != REGION_GFX3)
		return 0;
#ifndef ESP_PLATFORM
	if (getenv("NEOSPR") && !strcmp(getenv("NEOSPR"), "0")) /* PC A/B runs */
		return 0;
#endif
	if (!Machine->drv || Machine->drv->vh_start != neogeo_mvs_vh_start)
		return 0;
	if (Machine->gamedrv->driver_init == init_mgd2)
		return 0;
	return region_count < NEOSPR_MAX_REGIONS;
}

int neospr_active(void)
{
	return region_count > 0;
}

int neospr_owns(const unsigned char *region)
{
	int i;
	for (i = 0; i < region_count; i++)
		if (regions[i].stub == region)
			return 1;
	return 0;
}

static void decode_tile(const unsigned char *raw, uint32_t *out, uint32_t *pens)
{
	/* same bit order as decodetile() in vidhrdw/neogeo.c */
	int x, y;
	uint32_t usage = 0;

	for (y = 0; y < 16; y++)
	{
		uint32_t dw = 0;
		for (x = 0; x < 8; x++)
		{
			unsigned pen;
			pen  = ((raw[64 + 4*y + 3] >> x) & 1) << 3;
			pen |= ((raw[64 + 4*y + 1] >> x) & 1) << 2;
			pen |= ((raw[64 + 4*y + 2] >> x) & 1) << 1;
			pen |=  (raw[64 + 4*y    ] >> x) & 1;
			dw |= pen << 4*(7-x);
			usage |= 1u << pen;
		}
		*out++ = dw;

		dw = 0;
		for (x = 0; x < 8; x++)
		{
			unsigned pen;
			pen  = ((raw[4*y + 3] >> x) & 1) << 3;
			pen |= ((raw[4*y + 1] >> x) & 1) << 2;
			pen |= ((raw[4*y + 2] >> x) & 1) << 1;
			pen |=  (raw[4*y    ] >> x) & 1;
			dw |= pen << 4*(7-x);
			usage |= 1u << pen;
		}
		*out++ = dw;
	}
	*pens = usage;
}

static void *open_rom(const struct RomModule *romp)
{
	const struct GameDriver *drv = Machine->gamedrv;
	void *f = NULL;
	char crc[9];

	do { f = osd_fopen(drv->name, romp->name, OSD_FILETYPE_ROM, 0); drv = drv->clone_of; } while (!f && drv);
	if (f)
		return f;
	sprintf(crc, "%08x", (unsigned int)romp->crc);
	drv = Machine->gamedrv;
	do { f = osd_fopen(drv->name, crc, OSD_FILETYPE_ROM, 0); drv = drv->clone_of; } while (!f && drv);
	return f;
}

/* A ROM read a piece at a time: streamed from the zip picked in the launcher
   (8 MB sprite ROMs never fit in memory whole), else through the file layer,
   which unzips the whole file (fine for the small ones / parent zips). */
struct romrd
{
	struct zipstream *zs;
	void *f;
};

static int romrd_open(struct romrd *r, const struct RomModule *rom)
{
	extern char mamego_zip_path[];
	r->zs = NULL;
	r->f = NULL;
	if (mamego_zip_path[0])
		r->zs = zipstream_open(mamego_zip_path, rom->name, rom->crc, NULL);
	if (!r->zs)
		r->f = open_rom(rom);
	return (r->zs || r->f) ? 0 : -1;
}

static int romrd_read(struct romrd *r, void *buf, unsigned len)
{
	return r->zs ? zipstream_read(r->zs, buf, len) : osd_fread(r->f, buf, len);
}

static void romrd_close(struct romrd *r)
{
	if (r->zs)
		zipstream_close(r->zs);
	if (r->f)
		osd_fclose(r->f);
	r->zs = NULL;
	r->f = NULL;
}

/* the same reader for other drivers (CPS1 graphics, vidhrdw/cps1.c) */
void *mamego_romrd_open(const struct RomModule *rom)
{
	struct romrd *r = malloc(sizeof(*r));
	if (r && romrd_open(r, rom) != 0)
	{
		free(r);
		r = NULL;
	}
	return r;
}

int mamego_romrd_read(void *r, void *buf, unsigned len)
{
	return romrd_read((struct romrd *)r, buf, len);
}

void mamego_romrd_close(void *r)
{
	if (r)
	{
		romrd_close((struct romrd *)r);
		free(r);
	}
}

/* Convert the region's ROMs (ROM_LOAD_GFX_EVEN/ODD pairs) into the .spr
   file. A pair is streamed 64 KB at a time: besides the two zip entries
   the file layer keeps in memory, only a 128 KB interleave buffer is used.
   The byte placement is exactly readroms()'s for ROM_LOAD_EVEN/ODD. */
#define NEOSPR_CHUNK (64 * 1024)
static int build_file(FILE *out, const struct RomModule *first, int entries, uint32_t tiles, uint32_t data_offset)
{
	uint32_t *pens = calloc(tiles, sizeof(uint32_t));
	unsigned char *chunk = malloc(2 * NEOSPR_CHUNK), *part = malloc(NEOSPR_CHUNK);
	int done[64] = {0};
	int i, j, ret = -1;

	if (!pens || !chunk || !part || entries > 64)
		goto out;

	for (i = 0; i < entries; i++)
	{
		const struct RomModule *group[8];
		struct romrd files[8];
		unsigned base, length, pos, t;
		int n = 0, k;

		if (done[i])
			continue;
		base = first[i].offset & ~1;
		length = first[i].length & ~ROMFLAG_MASK;
		for (j = i; j < entries && n < 8; j++)
		{
			if (done[j] || (first[j].offset & ~1) != base || (first[j].length & ~ROMFLAG_MASK) != length)
				continue;
			if (romrd_open(&files[n], &first[j]) != 0)
			{
				printf("neospr: cannot open %s\n", first[j].name);
				while (n--) romrd_close(&files[n]);
				goto out;
			}
			printf("neospr: converting %s\n", first[j].name);
			group[n++] = &first[j];
			done[j] = 1;
		}

		for (pos = 0; pos < length; pos += NEOSPR_CHUNK)
		{
			unsigned len = length - pos < NEOSPR_CHUNK ? length - pos : NEOSPR_CHUNK;

			memset(chunk, 0, 2 * len);
			for (k = 0; k < n; k++)
			{
			#ifdef MSB_FIRST
				unsigned lane = group[k]->offset - base;
			#else
				unsigned lane = (group[k]->offset - base) ^ 1;
			#endif
				unsigned b;
				if (romrd_read(&files[k], part, len) != (int)len)
					memset(part, 0, len);
				for (b = 0; b < len; b++)
					chunk[lane + 2 * b] = part[b];
			}
			for (t = 0; t < 2 * len / NEOSPR_TILE; t++)
			{
				uint32_t tile = (base + 2 * pos) / NEOSPR_TILE + t;
				uint32_t decoded[32];
				decode_tile(chunk + t * NEOSPR_TILE, decoded, &pens[tile]);
				memcpy(chunk + t * NEOSPR_TILE, decoded, NEOSPR_TILE);
			}
			fseek(out, data_offset + base + 2 * pos, SEEK_SET);
			if (fwrite(chunk, 1, 2 * len, out) != 2 * len)
			{
				printf("neospr: write failed (card full?)\n");
				for (k = 0; k < n; k++) romrd_close(&files[k]);
				goto out;
			}
		}
		for (k = 0; k < n; k++)
			romrd_close(&files[k]);
	}

	fseek(out, sizeof(struct neospr_header), SEEK_SET);
	if (fwrite(pens, sizeof(uint32_t), tiles, out) == tiles)
		ret = 0;
out:
	free(pens); free(chunk); free(part);
	return ret;
}

/* Called by readroms() instead of allocating a Neo Geo sprite region.
   `first` points at the region's first ROM entry, `entries` ROM_LOADs follow.
   Returns the stub to store as the region pointer, or NULL to load it the
   normal way (unsupported layout, SD error). */
unsigned char *neospr_region_load(int type, const struct RomModule *first, int entries, unsigned region_size)
{
	struct neospr_region *reg = &regions[region_count];
	struct neospr_header h, want;
	char path[1024];
	uint32_t key = region_size;
	FILE *f;
	int i;

	for (i = 0; i < entries; i++)
	{
		const struct RomModule *r = &first[i];
		/* only plain even/odd byte pairs: no CONTINUE/RELOAD/nibble/quad/wide */
		if (!r->name || r->name == (char *)-1 || !(r->length & ROMFLAG_ALTERNATE)
			|| (r->length & (ROMFLAG_NIBBLE | ROMFLAG_QUAD | ROMFLAG_WIDE)))
		{
			printf("neospr: region %d has an unsupported ROM layout, loading it into RAM\n", type);
			return NULL;
		}
		key += (uint32_t)r->crc + (r->length & ~ROMFLAG_MASK) + r->offset;
	}

	memset(&want, 0, sizeof(want));
	memcpy(want.magic, "NEOSPR", 6);
	want.version = NEOSPR_VERSION;
	want.region_size = region_size;
	want.tiles = region_size / NEOSPR_TILE;
	want.rom_key = key;
	want.data_offset = (sizeof(want) + want.tiles * sizeof(uint32_t) + 4095) & ~4095u;

	snprintf(path, sizeof(path), "%s/neospr", core_sys_directory);
	mkdir(path, 0777);
	snprintf(path, sizeof(path), "%s/neospr/%s_gfx%d.spr", core_sys_directory, Machine->gamedrv->name,
		type - REGION_GFX1 + 1);

	f = fopen(path, "rb");
	if (!f || fread(&h, sizeof(h), 1, f) != 1 || memcmp(&h, &want, sizeof(h)) != 0)
	{
		if (f)
			fclose(f);
		printf("neospr: preparing %s (one time, %u KB)\n", path, region_size / 1024);
		if (!(f = fopen(path, "w+b")) || fwrite(&want, sizeof(want), 1, f) != 1
			|| build_file(f, first, entries, want.tiles, want.data_offset) != 0)
		{
			if (f)
				fclose(f);
			remove(path);
			return NULL;
		}
		/* close it now: FAT only records the size on close, and a crash
		   later in this run would otherwise make the next launch rebuild */
		fclose(f);
		if (!(f = fopen(path, "rb")))
			return NULL;
	}

	/* Nothing else stays allocated until neospr_start(): the program and
	   sample ROMs load next and need large contiguous PSRAM blocks (a
	   256 KB pen_usage table here made the 3 MB sample region fail). */
	reg->f = f;
	reg->tiles = want.tiles;
	reg->data_offset = want.data_offset;
	reg->first_page = total_pages;
	reg->stub = malloc(16);
	total_pages += (want.tiles + NEOSPR_PAGE_TILES - 1) / NEOSPR_PAGE_TILES;
	region_count++;
	printf("neospr: %s, %u tiles paged from the card\n", path, want.tiles);
	return reg->stub;
}

/* Fill the driver's pen_usage (tiles of all paged regions, in order) and
   size the page cache from what is left in PSRAM. */
int neospr_start(uint32_t *pen_usage, unsigned total_tiles)
{
	size_t bytes;
	uint32_t i, n = 0;
	int r;

	for (r = 0; r < region_count; r++)
	{
		if (n + regions[r].tiles > total_tiles)
			return -1;
		fseek(regions[r].f, sizeof(struct neospr_header), SEEK_SET);
		if (fread(pen_usage + n, sizeof(uint32_t), regions[r].tiles, regions[r].f) != regions[r].tiles)
			return -1;
		n += regions[r].tiles;
	}

	if (neosnd_alloc() != 0) /* the samples get their share first */
		return -1;
#ifdef ESP_PLATFORM
	{
		/* leave 1.5 MB: the frame bitmaps and display surfaces of the
		   second-core video path (~420 KB) are allocated after this */
		size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
		bytes = largest > 2048 * 1024 ? largest - 1536 * 1024 : 512 * 1024;
	}
#else
	bytes = getenv("NEOSPR_CACHE_KB") ? (size_t)atoi(getenv("NEOSPR_CACHE_KB")) * 1024 : 2048 * 1024;
#endif
#ifdef ESP_PLATFORM
	if (bytes > 3 * 1024 * 1024)
		bytes = 3 * 1024 * 1024;
#endif
	cache_slots = bytes / NEOSPR_PAGE;
	cache = malloc((size_t)cache_slots * NEOSPR_PAGE);
	page_slot = malloc(total_pages * sizeof(int32_t));
	slot_page = malloc(cache_slots * sizeof(int32_t));
	slot_used = calloc(cache_slots, sizeof(uint32_t));
	if (!cache || !page_slot || !slot_page || !slot_used)
		return -1;
	for (i = 0; i < total_pages; i++)
		page_slot[i] = -1;
	for (i = 0; i < (uint32_t)cache_slots; i++)
		slot_page[i] = -1;
	frame = 1;
	printf("neospr: page cache %d KB (%d pages of %d tiles)\n", cache_slots * NEOSPR_PAGE / 1024, cache_slots, NEOSPR_PAGE_TILES);
	return 0;
}

/* Save states: the page cache is the only block in PSRAM big enough for a
 * Neo Geo state (0.5 MB) while a game runs. Called between frames, when no
 * tile pointer is held; the cache is emptied and refills from the card.
 * NULL when there is no cache or it is too small. */
void *neospr_borrow(size_t size)
{
	uint32_t i;
	if (!cache || (size_t)cache_slots * NEOSPR_PAGE < size)
		return NULL;
	for (i = 0; i < total_pages; i++)
		page_slot[i] = -1;
	for (i = 0; i < (uint32_t)cache_slots; i++)
	{
		slot_page[i] = -1;
		slot_used[i] = 0;
	}
	next_victim = 0;
	return cache;
}

static int pick_victim(void)
{
	/* least recently used slot, skipping the ones this frame already uses */
	int best = -1, i;
	uint32_t oldest = ~0u;

	for (i = 0; i < cache_slots; i++)
	{
		int s = (next_victim + i) % cache_slots;
		if (slot_page[s] < 0)
			return s;
		if (slot_used[s] != frame && slot_used[s] < oldest)
		{
			oldest = slot_used[s];
			best = s;
		}
	}
	if (best < 0) /* the whole cache is in this frame: take the next one */
		best = next_victim;
	next_victim = (best + 1) % cache_slots;
	return best;
}

uint32_t *neospr_tile(int tileno)
{
	struct neospr_region *reg = &regions[0];
	uint32_t page, in_region = tileno;
	int slot;

	if (region_count > 1 && in_region >= regions[0].tiles)
	{
		in_region -= regions[0].tiles;
		reg = &regions[1];
	}
	page = reg->first_page + in_region / NEOSPR_PAGE_TILES;
	slot = page_slot[page];
	if (slot < 0)
	{
		unsigned char *dst;
		uint32_t first_tile = in_region & ~(NEOSPR_PAGE_TILES - 1);
		uint32_t count = reg->tiles - first_tile < NEOSPR_PAGE_TILES ? reg->tiles - first_tile : NEOSPR_PAGE_TILES;

		slot = pick_victim();
		if (slot_page[slot] >= 0)
			page_slot[slot_page[slot]] = -1;
		dst = cache + (size_t)slot * NEOSPR_PAGE;
		fseek(reg->f, reg->data_offset + first_tile * NEOSPR_TILE, SEEK_SET);
		if (fread(dst, NEOSPR_TILE, count, reg->f) != count)
			memset(dst, 0, NEOSPR_PAGE);
		slot_page[slot] = page;
		page_slot[page] = slot;
		misses_frame++;
	}
	slot_used[slot] = frame;
	return (uint32_t *)(cache + (size_t)slot * NEOSPR_PAGE + (in_region % NEOSPR_PAGE_TILES) * NEOSPR_TILE);
}

void neospr_frame(void)
{
	misses_total += misses_frame;
	if (misses_frame > worst_frame)
		worst_frame = misses_frame;
	misses_frame = 0;
	frame++;
	if (++frames_total % 600 == 0)
	{
		printf("neospr: %u page reads in the last 600 frames, worst frame %u; samples: %u page reads\n", misses_total, worst_frame, neosnd_take_misses());
		misses_total = worst_frame = 0;
	}
}

void neospr_stop(void)
{
	int r;
	for (r = 0; r < region_count; r++)
	{
		if (regions[r].f)
			fclose(regions[r].f);
		/* the stub is freed with the memory regions */
	}
	memset(regions, 0, sizeof(regions));
	region_count = 0;
	free(cache); free(page_slot); free(slot_page); free(slot_used);
	cache = NULL; page_slot = slot_page = NULL; slot_used = NULL;
	total_pages = cache_slots = 0;
	misses_frame = misses_total = frames_total = worst_frame = next_victim = 0;
}

/* ------------------------------------------------------------------------
 * Neo Geo sample ROMs (YM2610 ADPCM-A/B) paged from the SD card.
 *
 * Sample regions up to NEOSND_FLASH_MAX go to the mamerom flash partition
 * (common.c, fastest). Bigger ones (Metal Slug 2: 8 MB; the partition is
 * 4 MB and the flash is full) are copied once to
 *     <core_sys_directory>/neospr/<game>_snd<n>.pcm   (header | raw bytes)
 * and the chip's two byte reads (fm.c ADPCM-A, ymdeltat.c ADPCM-B) go
 * through a PSRAM cache of 4 KB pages. The chip reads each sample
 * sequentially: a page is ~0.4 s of one voice, so misses are rare.
 * ------------------------------------------------------------------------ */
#define NEOSND_PAGE        4096
#define NEOSND_FLASH_MAX   (3584 * 1024)
#define NEOSND_MAX_REGIONS 2

struct neosnd_region
{
	FILE *f;
	unsigned char *stub;
	uint32_t size;
	uint32_t data_offset;
	uint32_t first_page;
};

static struct neosnd_region snd_regions[NEOSND_MAX_REGIONS];
static int snd_count;
int neosnd_active;                    /* read by fm.c / ymdeltat.c */
static unsigned char *snd_cache;
static int snd_slots;
static int16_t *snd_page_slot;
static int32_t *snd_slot_page;
static uint32_t *snd_slot_used, snd_clock, snd_pages;
static unsigned snd_misses;

/* set by readroms() before the pre-pass: the program will need all of the
   flash partition (see mamego_regions_to_flash()) */
int neosnd_big_program;

int neosnd_wanted(int type, unsigned size)
{
	if (type < REGION_SOUND1 || type > REGION_SOUND8)
		return 0;
	if (!Machine->drv || Machine->drv->vh_start != neogeo_mvs_vh_start || snd_count >= NEOSND_MAX_REGIONS)
		return 0;
#ifndef ESP_PLATFORM
	if (getenv("NEOSND")) /* PC tests: 1 = always page, 0 = never */
		return atoi(getenv("NEOSND"));
#endif
	return size > NEOSND_FLASH_MAX || neosnd_big_program;
}

/* Plain ROM_LOADs laid end to end, copied into the .pcm file as they are. */
unsigned char *neosnd_region_load(int type, const struct RomModule *first, int entries, unsigned region_size)
{
	struct neosnd_region *reg = &snd_regions[snd_count];
	struct neospr_header h, want;
	unsigned char *buf = NULL;
	uint32_t key = region_size, expect = 0;
	char path[1024];
	FILE *f;
	int i;

	for (i = 0; i < entries; i++)
	{
		const struct RomModule *r = &first[i];
		unsigned length = r->length & ~ROMFLAG_MASK;
		/* plain ROM_LOADs in order; gaps between them are allowed (KOF '95:
		   600000-7fffff empty) and read as zeros */
		if (!r->name || r->name == (char *)-1 || (r->length & ROMFLAG_MASK) || r->offset < expect
			|| r->offset + length > region_size)
		{
			printf("neosnd: region %d has an unsupported ROM layout\n", type);
			return NULL;
		}
		expect = r->offset + length;
		key += (uint32_t)r->crc + length;
	}
	memset(&want, 0, sizeof(want));
	memcpy(want.magic, "NEOSND", 6);
	want.version = NEOSPR_VERSION;
	want.region_size = region_size;
	want.rom_key = key;
	want.data_offset = 4096;

	snprintf(path, sizeof(path), "%s/neospr", core_sys_directory);
	mkdir(path, 0777);
	snprintf(path, sizeof(path), "%s/neospr/%s_snd%d.pcm", core_sys_directory, Machine->gamedrv->name,
		type - REGION_SOUND1 + 1);

	f = fopen(path, "rb");
	if (!f || fread(&h, sizeof(h), 1, f) != 1 || memcmp(&h, &want, sizeof(h)) != 0)
	{
		if (f)
			fclose(f);
		printf("neosnd: preparing %s (one time, %u KB)\n", path, region_size / 1024);
		if (!(f = fopen(path, "wb")) || fwrite(&want, sizeof(want), 1, f) != 1 || !(buf = calloc(1, NEOSPR_CHUNK)))
			goto fail;
		{
			/* zeros over the whole region first: the gaps between ROMs read as silence */
			unsigned pos;
			fseek(f, want.data_offset, SEEK_SET);
			for (pos = 0; pos < region_size; pos += NEOSPR_CHUNK)
			{
				unsigned len = region_size - pos < NEOSPR_CHUNK ? region_size - pos : NEOSPR_CHUNK;
				if (fwrite(buf, 1, len, f) != len)
					goto fail;
			}
		}
		for (i = 0; i < entries; i++)
		{
			struct romrd rd;
			unsigned length = first[i].length & ~ROMFLAG_MASK, pos;
			if (romrd_open(&rd, &first[i]) != 0)
			{
				printf("neosnd: cannot open %s\n", first[i].name);
				goto fail;
			}
			printf("neosnd: copying %s\n", first[i].name);
			fseek(f, want.data_offset + first[i].offset, SEEK_SET);
			for (pos = 0; pos < length; pos += NEOSPR_CHUNK)
			{
				unsigned len = length - pos < NEOSPR_CHUNK ? length - pos : NEOSPR_CHUNK;
				if (romrd_read(&rd, buf, len) != (int)len || fwrite(buf, 1, len, f) != len)
				{
					romrd_close(&rd);
					goto fail;
				}
			}
			romrd_close(&rd);
		}
		free(buf);
		buf = NULL;
		fclose(f); /* FAT records the size on close */
		if (!(f = fopen(path, "rb")))
			return NULL;
	}

	reg->f = f;
	reg->size = region_size;
	reg->data_offset = want.data_offset;
	reg->first_page = snd_pages;
	reg->stub = malloc(16);
	snd_pages += (region_size + NEOSND_PAGE - 1) / NEOSND_PAGE;
	snd_count++;
	neosnd_active = 1;
	printf("neosnd: %s, %u KB of samples paged from the card\n", path, region_size / 1024);
	return reg->stub;

fail:
	free(buf);
	if (f)
		fclose(f);
	remove(path);
	return NULL;
}

int neosnd_owns(const unsigned char *region)
{
	int i;
	for (i = 0; i < snd_count; i++)
		if (snd_regions[i].stub == region)
			return 1;
	return 0;
}

/* Called before the sprite cache is sized: the samples get their share first. */
static int neosnd_alloc(void)
{
	size_t bytes;
	uint32_t i;

	if (!snd_count || snd_cache)
		return 0;
	/* Metal Slug 2 on the PC harness: <= 60 page reads per 600 frames with
	   256 KB already; 512 KB leaves the sprites and the program the rest */
#ifdef ESP_PLATFORM
	bytes = 512 * 1024;
#else
	bytes = getenv("NEOSND_CACHE_KB") ? (size_t)atoi(getenv("NEOSND_CACHE_KB")) * 1024 : 512 * 1024;
#endif
	snd_slots = bytes / NEOSND_PAGE;
	snd_cache = malloc((size_t)snd_slots * NEOSND_PAGE);
	snd_page_slot = malloc(snd_pages * sizeof(int16_t));
	snd_slot_page = malloc(snd_slots * sizeof(int32_t));
	snd_slot_used = calloc(snd_slots, sizeof(uint32_t));
	if (!snd_cache || !snd_page_slot || !snd_slot_page || !snd_slot_used)
		return -1;
	for (i = 0; i < snd_pages; i++)
		snd_page_slot[i] = -1;
	for (i = 0; i < (uint32_t)snd_slots; i++)
		snd_slot_page[i] = -1;
	printf("neosnd: sample cache %d KB (%d pages)\n", snd_slots * NEOSND_PAGE / 1024, snd_slots);
	return 0;
}

uint8_t neosnd_read(const uint8_t *base, uint32_t offset)
{
	struct neosnd_region *reg = NULL;
	uint32_t page;
	int slot, i;

	for (i = 0; i < snd_count; i++)
		if (snd_regions[i].stub == base)
			reg = &snd_regions[i];
	if (!reg)
		return base[offset];
	if (offset >= reg->size || (!snd_cache && neosnd_alloc() != 0))
		return 0;

	page = reg->first_page + offset / NEOSND_PAGE;
	slot = snd_page_slot[page];
	if (slot < 0)
	{
		/* least recently used page */
		uint32_t oldest = ~0u;
		int s;
		slot = 0;
		for (s = 0; s < snd_slots; s++)
		{
			if (snd_slot_page[s] < 0) { slot = s; break; }
			if (snd_slot_used[s] < oldest) { oldest = snd_slot_used[s]; slot = s; }
		}
		if (snd_slot_page[slot] >= 0)
			snd_page_slot[snd_slot_page[slot]] = -1;
		fseek(reg->f, reg->data_offset + (offset & ~(NEOSND_PAGE - 1)), SEEK_SET);
		if (fread(snd_cache + (size_t)slot * NEOSND_PAGE, 1, NEOSND_PAGE, reg->f) == 0)
			memset(snd_cache + (size_t)slot * NEOSND_PAGE, 0, NEOSND_PAGE);
		snd_slot_page[slot] = page;
		snd_page_slot[page] = slot;
		snd_misses++;
	}
	snd_slot_used[slot] = ++snd_clock;
	return snd_cache[(size_t)slot * NEOSND_PAGE + (offset & (NEOSND_PAGE - 1))];
}

unsigned neosnd_take_misses(void)
{
	unsigned m = snd_misses;
	snd_misses = 0;
	return m;
}

void neosnd_stop(void)
{
	int i;
	for (i = 0; i < snd_count; i++)
		if (snd_regions[i].f)
			fclose(snd_regions[i].f);
	memset(snd_regions, 0, sizeof(snd_regions));
	snd_count = 0;
	neosnd_active = 0;
	free(snd_cache); free(snd_page_slot); free(snd_slot_page); free(snd_slot_used);
	snd_cache = NULL; snd_page_slot = NULL; snd_slot_page = NULL; snd_slot_used = NULL;
	snd_pages = snd_slots = snd_clock = snd_misses = 0;
}
