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

void ym2610_write_reg1(uint8_t addr, uint8_t val)
{
	/* TODO: implement register decoder.
	 * This needs to update fm_state for FM channels 1 & 3,
	 * SSG state for channels 0-2, and ADPCM-B state. */
	(void)addr;
	(void)val;
}

void ym2610_write_reg2(uint8_t addr, uint8_t val)
{
	/* TODO: implement register decoder.
	 * This needs to update fm_state for FM channels 2 & 4,
	 * and ADPCM-A state for channels 0-5. */
	(void)addr;
	(void)val;
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
