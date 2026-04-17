/*
 * RSP Audio Overlay - CPU side
 *
 * Registers the RSP audio overlay with rspq and provides
 * a simple API to enqueue audio processing commands.
 */
#include <stdio.h>
#include <libdragon.h>
#include "rsp_audio.h"

DEFINE_RSP_UCODE(rsp_audio);

static uint32_t audio_ovl_id;

enum {
	CMD_MIX_SAMPLES = 0,
};

void rsp_audio_init(void)
{
	audio_ovl_id = rspq_overlay_register(&rsp_audio);
}

void rsp_audio_push(int16_t *src, int16_t *dst, int nsamples)
{
	if (nsamples <= 0) return;

	/* Process in chunks of 256 (DMEM work buffer limit) */
	while (nsamples > 0) {
		int chunk = (nsamples > 256) ? 256 : nsamples;
		rspq_write(audio_ovl_id, CMD_MIX_SAMPLES,
			   chunk,
			   PhysicalAddr(src),
			   PhysicalAddr(dst),
			   255 /* volume */);
		src += chunk;
		dst += chunk * 2; /* stereo output is 2x */
		nsamples -= chunk;
	}
	rspq_flush();
}
