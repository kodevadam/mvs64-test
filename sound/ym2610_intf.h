/*
 * YM2610 interface for MVS64
 *
 * Connects the NeoGeo Z80 sound CPU to the YM2610 FM/ADPCM/SSG chip
 * and the RSP FM synthesis overlay.
 *
 * Architecture:
 *   68K writes command → Z80 NMI → Z80 runs sound driver →
 *   Z80 writes YM2610 registers (ports $04-$07) →
 *   CPU-side updates envelope/state → fills rsp_fm_state →
 *   RSP synthesizes FM samples in parallel with next frame's 68K
 *
 * Z80 I/O ports (NeoGeo):
 *   $00 read:  command latch from 68K (clears NMI)
 *   $04 write: YM2610 address port 1
 *   $05 write: YM2610 data port 1
 *   $06 write: YM2610 address port 2
 *   $07 write: YM2610 data port 2
 *   $08 write: Z80 bank switch (for accessing 68K memory space)
 *   $0C write: response latch to 68K
 */

#ifndef YM2610_INTF_H
#define YM2610_INTF_H

#include <stdint.h>

/* Z80 clock: 4 MHz on NeoGeo */
#define Z80_CLOCK       4000000

/* YM2610 FM sample rate (native) */
#define YM2610_FM_RATE  (8000000 / 144)  /* ~55.5 KHz from 8 MHz master */

/* Sound output rate (N64 side) */
#define SND_OUTPUT_RATE 22050

/* Initialize the Z80 + YM2610 subsystem.
 * z80_rom: pointer to Z80 sound driver ROM (from game's M1 ROM)
 * z80_rom_size: size in bytes */
void sound_init(const uint8_t *z80_rom, int z80_rom_size);

/* Reset the Z80 + YM2610 */
void sound_reset(void);

/* Run the Z80 for the given number of master clock cycles.
 * Called from emu.c's frame loop. */
void sound_update(int cycles);

/* Send a command from the 68K to the Z80 (triggers NMI).
 * Called from hw.c when 68K writes to 0x320000. */
void sound_command(uint8_t cmd);

/* Read the Z80's response byte (68K reads from 0x320000). */
uint8_t sound_result(void);

/* Generate audio samples for one frame.
 * Fills output buffer with 16-bit stereo PCM.
 * Returns number of samples generated. */
int sound_render(int16_t *buf, int max_samples);

#endif /* YM2610_INTF_H */
