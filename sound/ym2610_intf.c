/*
 * YM2610 interface for MVS64
 *
 * Z80 sound CPU + YM2610 register interface.
 * Uses CZ80 for Z80 emulation, RSP overlay for FM synthesis.
 *
 * TODO: This is a scaffold. The YM2610 register decoder and
 * envelope state machine need to be implemented to actually
 * produce sound. Currently sets up Z80 execution and I/O
 * port routing.
 */

#include <stdio.h>
#include <string.h>
#include "ym2610_intf.h"
#include "rsp_fm.h"

/* CZ80 Z80 emulator */
#include "../cpu/cz80/cz80.h"

/* ---- Z80 Memory Map (NeoGeo) ---- */

#define Z80_ROM_SIZE    (128 * 1024)  /* M1 ROM, up to 128 KB */
#define Z80_RAM_SIZE    (2 * 1024)    /* 2 KB work RAM */

static uint8_t z80_rom[Z80_ROM_SIZE];
static uint8_t z80_ram[Z80_RAM_SIZE];
static cz80_struc z80_cpu;

/* 68K↔Z80 command/response latches */
static uint8_t cmd_latch;
static uint8_t result_latch = 0x01; /* "ready" initial value */
static int nmi_pending;

/* YM2610 register state */
static uint8_t ym_addr1;  /* address port 1 latch */
static uint8_t ym_addr2;  /* address port 2 latch */

/* RSP FM state (filled by CPU-side envelope updates) */
static struct rsp_fm_state __attribute__((aligned(8))) fm_state;
static int32_t __attribute__((aligned(8))) fm_output[512]; /* max samples per frame */

/* ---- Z80 memory read/write callbacks ---- */

static uint8_t z80_read(void *ctx, uint16_t addr)
{
	if (addr < Z80_ROM_SIZE)
		return z80_rom[addr];
	if (addr >= 0xF800)
		return z80_ram[addr & (Z80_RAM_SIZE - 1)];
	return 0xFF;
}

static void z80_write(void *ctx, uint16_t addr, uint8_t val)
{
	if (addr >= 0xF800) {
		z80_ram[addr & (Z80_RAM_SIZE - 1)] = val;
	}
}

/* ---- Z80 I/O port callbacks ---- */

static uint8_t z80_port_read(void *ctx, uint16_t port)
{
	port &= 0xFF;
	switch (port) {
	case 0x00: /* Command from 68K (clears NMI) */
		nmi_pending = 0;
		return cmd_latch;
	case 0x04: /* YM2610 status port 1 */
		return 0x00; /* TODO: timer flags */
	case 0x06: /* YM2610 status port 2 */
		return 0x00;
	default:
		return 0xFF;
	}
}

static void z80_port_write(void *ctx, uint16_t port, uint8_t val)
{
	port &= 0xFF;
	switch (port) {
	case 0x04: /* YM2610 address port 1 */
		ym_addr1 = val;
		break;
	case 0x05: /* YM2610 data port 1 */
		ym2610_write_reg1(ym_addr1, val);
		break;
	case 0x06: /* YM2610 address port 2 */
		ym_addr2 = val;
		break;
	case 0x07: /* YM2610 data port 2 */
		ym2610_write_reg2(ym_addr2, val);
		break;
	case 0x08: /* Z80 bank switch */
		/* TODO: bank switching for accessing 68K ROM space */
		break;
	case 0x0C: /* Response to 68K */
		result_latch = val;
		break;
	}
}

/* ---- YM2610 register write handlers ---- */

/*
 * YM2610 register map (simplified):
 *
 * Port 1 ($04/$05):
 *   $00-$0F: SSG (AY-3-8910 compatible)
 *   $10-$1F: ADPCM-B
 *   $20:     LFO frequency
 *   $24-$27: Timer A/B
 *   $28:     Key on/off
 *   $30-$9F: FM ch1/ch3 operator params (DT/MUL, TL, KS/AR, DR, SR, SL/RR)
 *   $A0-$A7: FM ch1/ch3 frequency
 *   $B0-$B7: FM ch1/ch3 feedback/algorithm
 *
 * Port 2 ($06/$07):
 *   $00-$0F: ADPCM-A
 *   $10-$1F: ADPCM-A per-channel volume
 *   $30-$9F: FM ch2/ch4 operator params
 *   $A0-$A7: FM ch2/ch4 frequency
 *   $B0-$B7: FM ch2/ch4 feedback/algorithm
 */

/* ---- FM Operator / Channel state (CPU-side) ---- */

/* Detune table (same as YM2612) */
static const int32_t dt_tab[4 * 32] = {
/* DT0 */ 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,
/* DT1 */ 0,0,0,0,1,1,1,1,1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,8,8,
/* DT2 */ 1,1,1,1,2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,16,16,16,
/* DT3 */ 2,2,2,2,2,3,3,3,4,4,4,5,5,6,6,7,8,8,9,10,11,12,13,14,16,17,19,20,22,22,22,22
};

/* Frequency number to phase increment table.
 * fn_table[fnum] = (fnum * 2^20 / (144 * 2)) for the base octave. */
static uint32_t fn_table[2048];
static int fn_table_built = 0;

static void build_fn_table(void)
{
	if (fn_table_built) return;
	for (int i = 0; i < 2048; i++) {
		/* YM2610 formula: phase_inc = fnum * 2^(block-1) / (144/2) */
		fn_table[i] = (uint32_t)((double)i * 64.0 / 144.0 * 65536.0);
	}
	fn_table_built = 1;
}

/* Algorithm routing decomposition table.
 * For each of 8 algorithms, gives: c1_mod, m2_mod, c2_mod, out_flags, mem_src */
static const uint8_t algo_table[8][5] = {
	/* algo 0: M1→C1→M2→C2 */       {1, 1, 1, 8,  1},
	/* algo 1: (M1+C1)→M2→C2 */     {0, 1, 1, 8,  2},
	/* algo 2: (M1+(C1→M2))→C2 */   {0, 0, 3, 8,  3},
	/* algo 3: ((M1→C1)+M2)→C2 */   {1, 0, 1, 8,  1},
	/* algo 4: (M1→C1)+(M2→C2) */   {1, 0, 1, 12, 0},
	/* algo 5: M1→(C1+M2+C2) */     {1, 1, 1, 14, 0},
	/* algo 6: (M1→C1)+M2+C2 */     {1, 0, 0, 14, 0},
	/* algo 7: M1+C1+M2+C2 */       {0, 0, 0, 15, 0},
};

/* Per-operator state */
typedef struct {
	uint8_t  dt;       /* detune (0-7) */
	uint8_t  mul;      /* multiply (0-15) */
	uint8_t  tl;       /* total level (0-127, attenuation in 0.75dB steps) */
	uint8_t  ks;       /* key scale (0-3) */
	uint8_t  ar;       /* attack rate (0-31) */
	uint8_t  d1r;      /* decay 1 rate (0-31) */
	uint8_t  d2r;      /* decay 2 rate (0-31) */
	uint8_t  rr;       /* release rate (0-15) */
	uint8_t  sl;       /* sustain level (0-15) */
	uint8_t  key_on;   /* key state */
} ym_slot_t;

/* Per-channel state */
typedef struct {
	ym_slot_t slot[4];   /* 4 operators: M1(0), C1(1), M2(2), C2(3) */
	uint16_t  fnum;      /* frequency number (11 bits) */
	uint8_t   block;     /* block/octave (3 bits) */
	uint8_t   fb;        /* feedback (0-7) */
	uint8_t   algo;      /* algorithm (0-7) */
	uint8_t   active;    /* any key on? */
} ym_chan_t;

static ym_chan_t ym_ch[4]; /* 4 FM channels for YM2610 */

/* Map operator register offset to slot index.
 * YM2610 slot ordering: 0→M1, 1→M2, 2→C1, 3→C2
 * (different from logical order — inherited from OPN family) */
static const int slot_map[4] = { 0, 2, 1, 3 };

/* Update the rsp_fm_state for one channel from CPU-side state */
static void update_fm_channel(int ch_idx)
{
	ym_chan_t *ch = &ym_ch[ch_idx];
	struct rsp_fm_chan *rch = &fm_state.ch[ch_idx];

	build_fn_table();

	/* Frequency → phase increment for each operator */
	uint32_t base_inc = fn_table[ch->fnum & 0x7FF] >> (7 - ch->block);

	for (int op = 0; op < 4; op++) {
		ym_slot_t *sl = &ch->slot[op];

		/* Apply detune */
		int dt_idx = (sl->dt & 3) * 32 + (ch->fnum >> 7);
		int32_t dt_val = dt_tab[dt_idx];
		if (sl->dt & 4) dt_val = -dt_val;

		/* Apply multiply */
		uint32_t inc = base_inc;
		if (sl->mul) inc = (inc * sl->mul) >> 0;
		else         inc = inc >> 1;
		inc += dt_val;

		rch->phase[op] = rch->phase[op]; /* preserve running phase */
		rch->incr[op] = inc;

		/* Volume: total level → attenuation.
		 * For now, use TL directly as envelope value.
		 * Full envelope (ADSR) will modulate this per-sample on CPU
		 * and update vol_out before each RSP render call. */
		if (sl->key_on)
			rch->vol_out[op] = sl->tl << 3; /* scale TL to envelope range */
		else
			rch->vol_out[op] = 0x3FF; /* max attenuation = silent */
	}

	/* Algorithm routing */
	const uint8_t *at = algo_table[ch->algo & 7];
	rch->c1_mod    = at[0];
	rch->m2_mod    = at[1];
	rch->c2_mod    = at[2];
	rch->out_flags = at[3];
	rch->mem_src   = at[4];

	/* Feedback */
	rch->fb_shift = ch->fb ? (ch->fb + 6) : 0;

	/* Channel active */
	rch->enabled = ch->active;
}

/* Write to a FM operator register.
 * ch_pair: 0 for channels 0/1 (port 1), 1 for channels 2/3 (port 2) */
static void ym_write_fm_reg(int ch_pair, uint8_t addr, uint8_t val)
{
	int ch_offset = (addr & 0x02) ? 1 : 0; /* bit 1 selects channel within pair */
	int ch_idx = ch_pair * 2 + ch_offset;
	if (ch_idx >= 4) return;

	ym_chan_t *ch = &ym_ch[ch_idx];

	if (addr >= 0x30 && addr < 0xA0) {
		/* Operator parameter registers */
		int op_idx = slot_map[(addr >> 2) & 3];
		ym_slot_t *sl = &ch->slot[op_idx];
		int reg = addr & 0xF0;

		switch (reg) {
		case 0x30: /* DT / MUL */
			sl->dt  = (val >> 4) & 7;
			sl->mul = val & 0xF;
			break;
		case 0x40: /* TL */
			sl->tl = val & 0x7F;
			break;
		case 0x50: /* KS / AR */
			sl->ks = (val >> 6) & 3;
			sl->ar = val & 0x1F;
			break;
		case 0x60: /* D1R (+ AM on bit 7) */
			sl->d1r = val & 0x1F;
			break;
		case 0x70: /* D2R */
			sl->d2r = val & 0x1F;
			break;
		case 0x80: /* SL / RR */
			sl->sl = (val >> 4) & 0xF;
			sl->rr = val & 0xF;
			break;
		}
		update_fm_channel(ch_idx);

	} else if (addr >= 0xA0 && addr < 0xB0) {
		/* Frequency registers */
		int freg = addr & 0x0F;
		if (freg < 4) {
			/* A0-A3: frequency number low 8 bits */
			ch->fnum = (ch->fnum & 0x700) | val;
			update_fm_channel(ch_idx);
		} else if (freg >= 4 && freg < 8) {
			/* A4-A7: block + frequency number high 3 bits */
			ch->fnum = (ch->fnum & 0xFF) | ((val & 0x07) << 8);
			ch->block = (val >> 3) & 7;
			update_fm_channel(ch_idx);
		}

	} else if (addr >= 0xB0 && addr < 0xB8) {
		/* B0-B3: feedback / algorithm */
		ch->fb   = (val >> 3) & 7;
		ch->algo = val & 7;
		update_fm_channel(ch_idx);
	}
}

void ym2610_write_reg1(uint8_t addr, uint8_t val)
{
	if (addr == 0x28) {
		/* Key on/off — applies to all channels */
		int ch_idx = val & 0x03;
		if (ch_idx >= 4) return;
		ym_chan_t *ch = &ym_ch[ch_idx];
		ch->slot[0].key_on = (val >> 4) & 1;
		ch->slot[1].key_on = (val >> 5) & 1;
		ch->slot[2].key_on = (val >> 6) & 1;
		ch->slot[3].key_on = (val >> 7) & 1;
		ch->active = (val >> 4) != 0;
		update_fm_channel(ch_idx);
		return;
	}

	if (addr >= 0x30)
		ym_write_fm_reg(0, addr, val); /* channels 0 & 1 */

	/* TODO: $00-$0F SSG, $10-$1F ADPCM-B, $20 LFO, $24-$27 timers */
}

void ym2610_write_reg2(uint8_t addr, uint8_t val)
{
	if (addr >= 0x30)
		ym_write_fm_reg(1, addr, val); /* channels 2 & 3 */

	/* TODO: $00-$0F ADPCM-A channels */
}

/* ---- Public API ---- */

void sound_init(const uint8_t *rom, int rom_size)
{
	memset(&z80_cpu, 0, sizeof(z80_cpu));
	memset(z80_rom, 0, sizeof(z80_rom));
	memset(z80_ram, 0, sizeof(z80_ram));
	memset(&fm_state, 0, sizeof(fm_state));

	if (rom && rom_size > 0) {
		int copy_size = rom_size < Z80_ROM_SIZE ? rom_size : Z80_ROM_SIZE;
		memcpy(z80_rom, rom, copy_size);
	}

	/* Initialize CZ80 */
	Cz80_Init(&z80_cpu);

	/* Set up memory map: Z80 address space is 64 KB */
	for (int i = 0; i < CZ80_FETCH_BANK; i++) {
		uint32_t base = i << CZ80_FETCH_SFT;
		if (base < Z80_ROM_SIZE)
			Cz80_Set_Fetch(&z80_cpu, base, base + (1 << CZ80_FETCH_SFT) - 1,
				(uintptr_t)z80_rom);
	}

	Cz80_Set_ReadB(&z80_cpu, z80_read);
	Cz80_Set_WriteB(&z80_cpu, z80_write);
	Cz80_Set_INPort(&z80_cpu, z80_port_read);
	Cz80_Set_OUTPort(&z80_cpu, z80_port_write);

	result_latch = 0x01;
	nmi_pending = 0;

#ifdef N64
	rsp_fm_init();
#endif
}

void sound_reset(void)
{
	Cz80_Reset(&z80_cpu);
	result_latch = 0x01;
	nmi_pending = 0;
	memset(&fm_state, 0, sizeof(fm_state));
}

void sound_update(int cycles)
{
	if (nmi_pending) {
		Cz80_Set_NMI(&z80_cpu);
		nmi_pending = 0;
	}
	Cz80_Exec(&z80_cpu, cycles);
}

void sound_command(uint8_t cmd)
{
	cmd_latch = cmd;
	nmi_pending = 1;
}

uint8_t sound_result(void)
{
	return result_latch;
}

int sound_render(int16_t *buf, int max_samples)
{
	int nsamples = max_samples;
	if (nsamples > 256) nsamples = 256;

	fm_state.num_samples = nsamples;

	memset(fm_output, 0, nsamples * sizeof(int32_t));

#ifdef N64
	rsp_fm_render(&fm_state, fm_output);
	/* RSP runs async — we need to wait before reading output.
	 * TODO: pipeline this so RSP runs while CPU does next frame's 68K. */
	rspq_wait();
#endif

	/* Convert 32-bit mono → 16-bit stereo */
	for (int i = 0; i < nsamples; i++) {
		int32_t s = fm_output[i];
		if (s > 32767) s = 32767;
		if (s < -32768) s = -32768;
		buf[i * 2 + 0] = (int16_t)s; /* L */
		buf[i * 2 + 1] = (int16_t)s; /* R */
	}

	return nsamples;
}
