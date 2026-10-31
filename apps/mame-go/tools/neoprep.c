/* neoprep: prepare Neo Geo sprite/sample files for mame-go on a PC.
 *
 * Loads a ROM zip with the same mame2000 code the board runs (PC build):
 * loading converts the sprite ROMs to <sysdir>/neospr/<game>_gfx<n>.spr and
 * the big sample ROMs to <game>_snd<n>.pcm, byte for byte what the board
 * would write (both little-endian). Copy that neospr/ folder to
 * /sd/retro-go/mame/mame2000/neospr/ and the board skips its own (slow)
 * first-launch conversion. Built and driven by scripts/neogeo_prepare.py.
 *
 *     neoprep <sysdir> <game.zip>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "libretro.h"

static const char *sysdir;

static bool env(unsigned cmd, void *data)
{
	switch (cmd)
	{
	case RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY:
	case RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY:
		*(const char **)data = sysdir;
		return true;
	case RETRO_ENVIRONMENT_SET_PIXEL_FORMAT:
	case RETRO_ENVIRONMENT_GET_INPUT_BITMASKS:
		return true;
	case RETRO_ENVIRONMENT_GET_VARIABLE:
	{
		struct retro_variable *v = data;
		if (!strcmp(v->key, "mame2000-sample_rate")) { v->value = "32000"; return true; }
		return false;
	}
	default:
		return false;
	}
}
static void video(const void *d, unsigned w, unsigned h, size_t p) {}
static void sample(int16_t l, int16_t r) {}
static size_t batch(const int16_t *d, size_t n) { return n; }
static void poll(void) {}
static int16_t state(unsigned port, unsigned dev, unsigned idx, unsigned id) { return 0; }

/* the board keeps big regions in its flash partition; here they stay in RAM */
unsigned char *mamego_flash_store(const unsigned char *data, size_t len, size_t *offset) { return NULL; }

int main(int argc, char **argv)
{
	struct retro_game_info game = {0};

	if (argc != 3)
	{
		fprintf(stderr, "usage: %s <sysdir> <game.zip>\n", argv[0]);
		return 2;
	}
	sysdir = argv[1];
	game.path = argv[2];
	retro_set_environment(env);
	retro_set_video_refresh(video);
	retro_set_audio_sample(sample);
	retro_set_audio_sample_batch(batch);
	retro_set_input_poll(poll);
	retro_set_input_state(state);
	retro_init();
	if (!retro_load_game(&game))
	{
		fprintf(stderr, "neoprep: %s did not load\n", argv[2]);
		return 1;
	}
	retro_run(); /* one frame: the video start reads the prepared files */
	return 0;
}
