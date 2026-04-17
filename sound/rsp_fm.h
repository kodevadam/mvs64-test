#ifndef RSP_FM_H
#define RSP_FM_H

#include <stdint.h>

/*
 * RSP FM synthesis state — adapted from PicoDrive64 for NeoGeo YM2610.
 *
 * YM2610 has 4 FM channels (vs YM2612's 6), but the operator structure
 * is identical: 4 operators per channel, 8 algorithms, same feedback/
 * modulation routing.
 *
 * Algorithm routing parameters (computed by CPU, consumed by RSP):
 *   c1_mod:    modulation source for C1 (SLOT2): 0=zero, 1=op1_out>>16
 *   m2_mod:    modulation source for M2 (SLOT3): 0=zero, 1=prev_mem
 *   c2_mod:    modulation source for C2 (SLOT4):
 *                0=zero, 1=M2_out, 2=op1s+M2, 3=mem+M2, 4=op1s
 *   out_flags: which operators contribute to output:
 *                bit0=op1s, bit1=C1, bit2=M2, bit3=C2
 *   mem_src:   what writes to delay memory:
 *                0=none, 1=C1, 2=op1s+C1, 3=op1s
 */

#define YM2610_FM_CHANNELS  4

/* Per-channel state sent to RSP for FM synthesis */
struct rsp_fm_chan {
	uint32_t phase[4];     /* 0-15: operator phase counters */
	uint32_t incr[4];      /* 16-31: phase increments */
	uint16_t vol_out[4];   /* 32-39: envelope levels (attenuation) */
	uint8_t  c1_mod;       /* 40: C1 modulation source */
	uint8_t  fb_shift;     /* 41: feedback shift (0=off, 1-7) */
	uint8_t  m2_mod;       /* 42: M2 modulation source */
	uint8_t  enabled;      /* 43: channel active */
	int32_t  op1_out;      /* 44-47: operator 1 feedback state */
	int32_t  mem;          /* 48-51: algorithm delay memory */
	uint8_t  c2_mod;       /* 52: C2 modulation type */
	uint8_t  out_flags;    /* 53: output contribution flags */
	uint8_t  mem_src;      /* 54: delay memory source */
	uint8_t  pad;          /* 55: alignment */
}; /* 56 bytes */

/* Full state for YM2610 FM channels */
struct rsp_fm_state {
	struct rsp_fm_chan ch[YM2610_FM_CHANNELS];  /* 224 bytes */
	uint16_t num_samples;
	uint16_t pad;
}; /* 228 bytes */

void rsp_fm_init(void);
void rsp_fm_render(struct rsp_fm_state *state, int32_t *out_buf);

#endif
