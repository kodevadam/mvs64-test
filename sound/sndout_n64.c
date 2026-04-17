/*
 * N64 sound output driver for PicoDrive
 *
 * Uses libdragon audio subsystem to output Genesis audio.
 * Replaces libpicofe sndout.o for N64 platform.
 *
 * (C) 2026
 * This work is licensed under the terms of MAME license.
 * See COPYING file in the top-level directory.
 */
#include <stdio.h>
#include <string.h>
#include "../libpicofe/sndout.h"
#include "n64.h"

static int snd_started = 0;

static int sndout_n64_init(void)
{
	return 0;
}

static void sndout_n64_exit(void)
{
	if (snd_started) {
		audio_close();
		snd_started = 0;
	}
}

static int sndout_n64_start(int rate, int stereo)
{
	audio_init(rate, stereo ? 2 : 1);
	snd_started = 1;
	return 0;
}

static void sndout_n64_stop(void)
{
	if (snd_started) {
		audio_close();
		snd_started = 0;
	}
}

static void sndout_n64_wait(void)
{
	/* libdragon audio_write handles blocking/buffering internally */
}

static int sndout_n64_write_nb(const void *data, int bytes)
{
	if (!snd_started || !data || bytes <= 0)
		return 0;

	audio_write((short *)data);
	return bytes;
}

/* Provide the global driver struct that sndout.h inlines delegate to */
struct sndout_driver sndout_current;

/* Called once at startup, replaces libpicofe sndout_init() */
void sndout_init(void)
{
	sndout_current.name     = "n64";
	sndout_current.init     = sndout_n64_init;
	sndout_current.exit     = sndout_n64_exit;
	sndout_current.start    = sndout_n64_start;
	sndout_current.stop     = sndout_n64_stop;
	sndout_current.wait     = sndout_n64_wait;
	sndout_current.write_nb = sndout_n64_write_nb;
}
