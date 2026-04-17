/*
 * RSP FM Synthesis - CPU side (MVS64 / YM2610)
 *
 * Adapted from PicoDrive64's YM2612 RSP FM overlay.
 * CPU handles envelope state machine + register writes,
 * RSP handles the per-sample synthesis hot loop.
 *
 * YM2610 has 4 FM channels (vs YM2612's 6); operator structure
 * and algorithm routing are identical.
 */
#include <stdio.h>
#include <string.h>
#include <math.h>

#ifdef N64
#include <libdragon.h>
#endif

#include "rsp_fm.h"

#ifdef N64

DEFINE_RSP_UCODE(rsp_fm);

static uint32_t fm_ovl_id;
static int tables_uploaded = 0;

static uint16_t rsp_sin_tab[256] __attribute__((aligned(16)));
static uint16_t rsp_exp_tab[256] __attribute__((aligned(16)));

enum {
	CMD_FM_RENDER = 0,
	CMD_FM_INIT_TABLES = 1,
};

void rsp_fm_init(void)
{
	fm_ovl_id = rspq_overlay_register(&rsp_fm);
}

static void upload_tables(void)
{
	if (tables_uploaded) return;

	/* Build sin_tab from the standard FM formula.
	 * This is the same table used by YM2612/YM2610. */
	for (int i = 0; i < 256; i++) {
		double m = sin(((i*2)+1) * M_PI / 1024.0);
		double o;
		int n;
		if (m > 0.0)
			o = 8*log(1.0/m)/log(2.0);
		else
			o = 8*log(-1.0/m)/log(2.0);
		o = o / (128.0/1024.0 / 4);
		n = (int)(2.0*o);
		if (n&1) n = (n>>1)+1;
		else     n = n>>1;
		rsp_sin_tab[i] = n;
	}

	/* Build exp_tab (power-of-2 attenuation lookup).
	 * Standard FM: exp_tab[i] = pow(2, -(i/256.0)) * 1024
	 * TODO: can also copy from ym2610 core if available. */
	for (int i = 0; i < 256; i++) {
		rsp_exp_tab[i] = (uint16_t)(pow(2.0, 1.0 - (i / 256.0)) * 1024.0 + 0.5);
	}

	data_cache_hit_writeback(rsp_sin_tab, sizeof(rsp_sin_tab));
	data_cache_hit_writeback(rsp_exp_tab, sizeof(rsp_exp_tab));

	rspq_write(fm_ovl_id, CMD_FM_INIT_TABLES,
		   0,
		   PhysicalAddr(rsp_sin_tab),
		   PhysicalAddr(rsp_exp_tab));
	rspq_flush();
	rspq_wait();

	tables_uploaded = 1;
}

void rsp_fm_render(struct rsp_fm_state *state, int32_t *out_buf)
{
	upload_tables();

	data_cache_hit_writeback(state, (sizeof(*state) + 15) & ~15);

	rspq_write(fm_ovl_id, CMD_FM_RENDER,
		   state->num_samples,
		   PhysicalAddr(state),
		   PhysicalAddr(out_buf));
	rspq_flush();
}

#else
/* Non-N64 stubs */
void rsp_fm_init(void) {}
void rsp_fm_render(struct rsp_fm_state *state, int32_t *out_buf) {}
#endif
