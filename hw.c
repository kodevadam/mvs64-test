#include <stdio.h>
#include <stdbool.h>
#include <memory.h>
#include "hw.h"
#include "roms.h"
#include "video.h"
#include "emu.h"
#include "m68k.h"
#include "platform.h"

// Typedefs for unaligned memory accesses
typedef uint16_t u_uint16_t __attribute__((aligned(1)));
typedef uint32_t u_uint32_t __attribute__((aligned(1)));

// Z80 sound CPU stub: fake the command/response protocol.
// On real hardware, the 68K writes a command byte to 0x320000 which latches
// in the NEO-C1 and triggers a Z80 NMI. The Z80 reads the command (port $00),
// processes it, and writes a response byte (port $0C) that the 68K can read
// back from 0x320000.
//
// Protocol details (from NeoGeo dev wiki and freemlib sound driver):
//   Command 0x01 (slot switch): Z80 echoes 0x01 back to signal readiness.
//       BIOS polls until it reads 0x01; timeout causes "Z80 ERROR".
//   Command 0x02 (eyecatch BGM): No reply expected, just plays music.
//   Command 0x03 (soft reset):   Z80 clears latch to 0x00, then restarts.
//       No reply expected by BIOS.
//   Other commands:               Sound drivers echo cmd | 0x80 when done.
//       68K can poll bit 7 to detect completion.
//
// Without a Z80 emulator, we latch the expected response immediately.
static uint8_t z80_result = 0x01;  // Z80 "ready" value expected during BIOS boot

#ifdef N64
#define ALIGN_64K __attribute__((aligned(64*1024)))
#else
#define ALIGN_64K
#endif

uint8_t P_ROM_VECTOR[0x80];
uint8_t BIOS[128*1024];
uint8_t WORK_RAM[64*1024] ALIGN_64K;
uint8_t BACKUP_RAM[64*1024] ALIGN_64K;
uint16_t PALETTE_RAM[8*1024];  // two banks
uint16_t VIDEO_RAM[34*1024];

int PALETTE_RAM_BANK;

typedef uint32_t (*ReadCB)(uint32_t addr, int sz);
typedef void (*WriteCB)(uint32_t addr, uint32_t val, int sz);

typedef struct {
	uint8_t *mem;
	uint32_t mask;
	ReadCB r;
	WriteCB w;
} Bank;

static Bank banks[16];

#include "rtc.c"
#include "lspc.c"
#include "input.c"
#include "watchdog.c"

#define DIPSW_SETTINGS_MODE    (1<<0)
#define DIPSW_FREEPLAY         (1<<6)

static void write_unk(uint32_t addr, uint32_t val, int sz) {
	debugf("[MEM] unknown write%d: %06x <- %0*x\n", sz*8, (unsigned int)addr, sz*2, (unsigned int)val);
}

uint32_t pbrom_bank = 0;
uint32_t pbrom_memid = 0;

void write_pbrom(uint32_t addr, uint32_t val, int sz) {
	if (addr >= 0x2FFFF0 && addr <= 0x2FFFFF) {
		val &= 7;
		if (pbrom_bank == (val << 20))
			return;
		pbrom_bank = val << 20;
		// debugf("[CART] bankswitch %x <= %x (linear: %d)\n", (unsigned int)addr, (unsigned int)pbrom_bank, (bool)banks[0x2].mem);

		// If the PBROM area linearly mapped, update the mapping.
		if (banks[0x2].mem) {
			banks[0x2].mem = pbrom_linear() + val*0x100000;
		}
		return;
	}

	// debugf("[CART] unknown write%d: %06x <- %0*x (PC:%08lx)\n", sz*8, (unsigned int)addr, sz*2, (unsigned int)val, emu_pc());
}

uint32_t read_pbrom(uint32_t addr, int sz) {
	// Most (all?) games seem to keep the bank number at 0x2fffef,
	// and they check for it after switching banks (in a loop, maybe
	// to wait until the bank is effectively mapped).
	// Since games often uselessly bankswitch and they only access
	// this location, we shortcircuit it to avoid loading that banks
	// uselessly everytime.
	// FIXME: check if we really need this
	// FIXME: see if we can detect when this is false, to avoid bugs
	if (addr == 0x2fffef && sz == 1) return pbrom_bank >> 20;

	uint8_t *rom = pbrom_cache_lookup(pbrom_bank | (addr & 0x0FFFFF));
	if (sz == 4) return BE32(*(u_uint32_t*)rom);
	if (sz == 2) return BE16(*(uint16_t*)rom);
	return *rom;
}

uint32_t read_hwio(uint32_t addr, int sz)  {
	if (sz == 4) {
		// NOTE: order is important
		uint32_t val = read_hwio(addr+0, 2) << 16;
		return val | read_hwio(addr+2, 2);
	}

	// Idle skip for RTC Wait Pulse in BIOS boot
	#if 0
	if (addr == 0x320001 && emu_pc() == 0xC11DA2) {
		m68k_consume_timeslice();
	}
	#endif

	// debugf("[HWIO] read%d: %06x (68K PC:%x)\n", sz*8, (unsigned int)addr, m68k_get_reg(NULL, M68K_REG_PC));
	// debugf("[HWIO] read%d: %06x (68K PC:%x EPC:%lx)\n", sz*8, (unsigned int)addr, m68k_get_reg(NULL, M68K_REG_PC), C0_READ_EPC());

	if ((addr>>16) == 0x30) switch (addr&0xFFFF) {
		case 0x00: assert(sz==1); return input_p1cnt_r();
		case 0x01: assert(sz==1); return 0xFF ^ DIPSW_FREEPLAY; // dipswitches

	} else if ((addr>>16) == 0x32) switch (addr&0xFFFF) {
		case 0x00: assert(sz==1); return z80_result;
		case 0x01: assert(sz==1); return input_status_a_r();

	} else if ((addr>>16) == 0x38) switch (addr&0xFFFF) {
		case 0x00: assert(sz==1); return input_status_b_r();

	} else if ((addr>>16) == 0x3A) switch (addr&0xFFFF) {

	} else if ((addr>>16) == 0x3C) switch (addr&0xFFFF) {
		case 0x00: case 0x08: case 0x0A:
		case 0x02: assert(sz==2); return lspc_vram_data_r();
		case 0x0C:
		case 0x04: assert(sz==2); return lspc_vram_modulo_r();
		case 0x0E:
		case 0x06: assert(sz==2); return lspc_mode_r();
	}

	// debugf("[HWIO] unknown read%d: %06x\n", sz*8, (unsigned int)addr);
	return 0xFFFFFFFF;
}

void write_hwio(uint32_t addr, uint32_t val, int sz)  {
	if (sz == 4) {
		write_hwio(addr+0, val>>16, 2);
		write_hwio(addr+2, val&0xFFFF, 2);
		return;
	}

	if ((addr>>16) == 0x30) switch (addr&0xFFFF) {
		case 0x01: watchdog_kick(); return;

	} else if ((addr>>16) == 0x32) switch (addr&0xFFFF) {
		case 0x00: assert(sz==1);
			// Fake Z80 command processing without a real Z80 CPU.
			if (val == 0x01)
				z80_result = 0x01;  // slot switch: echo 0x01 (BIOS polls for this)
			else if (val == 0x03)
				z80_result = 0x01;  // soft reset: Z80 clears to 0x00 then restarts as "ready" (0x01)
			else
				z80_result = val | 0x80;  // general commands: echo with bit 7 set
			return;

	} else if ((addr>>16) == 0x38) switch (addr&0xFFFF) {
		case 0x51: rtc_data_w(val&1); rtc_clock_w(val&2); rtc_stb_w(val&4); return;

	} else if ((addr>>16) == 0x3A) switch (addr&0xFFFF) {
		case 0x03: assert(sz==1); memcpy(P_ROM, BIOS, sizeof(P_ROM_VECTOR)); return;
		case 0x13: assert(sz==1); memcpy(P_ROM, P_ROM_VECTOR, sizeof(P_ROM_VECTOR)); return;
		case 0x0F: assert(sz==1); PALETTE_RAM_BANK = 0x1000; return;
		case 0x1F: assert(sz==1); PALETTE_RAM_BANK = 0x0000; return;
		case 0x0D: assert(sz==1); banks[0xD].w = write_unk; return;
		case 0x1D: assert(sz==1); banks[0xD].w = NULL; return;
		case 0x0B: assert(sz==1); srom_set_bank(0); return;
		case 0x1B: assert(sz==1); srom_set_bank(1); return;

	} else if ((addr>>16) == 0x3C) switch (addr&0xFFFF) {
		case 0x00: assert(sz==2); lspc_vram_addr_w(val); return;
		case 0x02: assert(sz==2); lspc_vram_data_w(val); return;
		case 0x04: assert(sz==2); lspc_vram_modulo_w(val); return;
		case 0x06: assert(sz==2); lspc_mode_w(val); return;
		case 0x0C: if (val&1) emu_cpu_irq(3,false); if (val&2) emu_cpu_irq(2,false); if (val&4) emu_cpu_irq(1,false); return;
	}

	// debugf("[HWIO] unknown write%d: %06x <- %0*x (PC=%06lx)\n", sz*8, (unsigned int)addr, sz*2, (unsigned int)val, emu_pc());
}


// Memory access functions.
// The fast path for P-ROM (0x0xxxxx) and WORK_RAM (0x1xxxxx) is inlined
// in m68kinline.h. These functions handle the remaining cases.
unsigned int m68k_read_memory_8_slow(unsigned int address) {
	Bank *b = &banks[(address>>20)&0xF];
	if (b->r) return b->r(address, 1);
	if (b->mem) return *(b->mem + (address & b->mask));
	return 0xFF;
}

unsigned int m68k_read_memory_16_slow(unsigned int address) {
	Bank *b = &banks[(address>>20)&0xF];
	if (b->r) return b->r(address, 2);
	if (b->mem) return BE16(*(u_uint16_t*)(b->mem + (address & b->mask)));
	return 0xFFFF;
}

unsigned int m68k_read_memory_32_slow(unsigned int address) {
	Bank *b = &banks[(address>>20)&0xF];
	if (b->r) return b->r(address, 4);
	if (b->mem) return BE32(*(u_uint32_t*)(b->mem + (address & b->mask)));
	return 0;
}

void m68k_write_memory_8_slow(unsigned int address, unsigned int value) {
	Bank *b = &banks[(address>>20)&0xF];
	if (b->w) { b->w(address, value, 1); return; }
	if (b->mem) { *(b->mem + (address & b->mask)) = value; return; }
}

void m68k_write_memory_16_slow(unsigned int address, unsigned int value) {
	Bank *b = &banks[(address>>20)&0xF];
	if (b->w) { b->w(address, value, 2); return; }
	if (b->mem) { *(u_uint16_t*)(b->mem + (address & b->mask)) = BE16(value); return; }
}

void m68k_write_memory_32_slow(unsigned int address, unsigned int value) {
	Bank *b = &banks[(address>>20)&0xF];
	if (b->w) { b->w(address, value, 4); return; }
	if (b->mem) { *(u_uint32_t*)(b->mem + (address & b->mask)) = BE32(value); return; }
}


unsigned int m68k_read_disassembler_8(unsigned int address) {
	Bank *b = &banks[(address>>20)&0xF];
	if (b->mem)
		return *(b->mem + (address & b->mask));
	return 0xFFFFFFFF;
}

unsigned int m68k_read_disassembler_16(unsigned int address) {
	Bank *b = &banks[(address>>20)&0xF];
	if (b->mem)
		return BE16(*(u_uint16_t*)(b->mem + (address & b->mask)));
	return 0xFFFFFFFF;
}

unsigned int m68k_read_disassembler_32(unsigned int address) {
	Bank *b = &banks[(address>>20)&0xF];
	if (b->mem)
		return BE32(*(u_uint32_t*)(b->mem + (address & b->mask)));
	return 0xFFFFFFFF;
}

void hw_init(void) {
	uint8_t *PB_ROM = pbrom_linear();

	memset(banks, 0, sizeof(banks));
	memcpy(P_ROM_VECTOR, P_ROM, sizeof(P_ROM_VECTOR));
	PALETTE_RAM_BANK = 0x0000;

	banks[0x0] = (Bank){ P_ROM+0x000000,   0xFFFFF,   NULL,            write_unk };
	banks[0x1] = (Bank){ WORK_RAM,         0x0FFFF,   NULL,            NULL };
	if (PB_ROM)
		banks[0x2] = (Bank){ PB_ROM,       0xFFFFF,   NULL,            write_pbrom };
	else
		banks[0x2] = (Bank){ NULL,         0xFFFFF,   read_pbrom,      write_pbrom };
	banks[0x3] = (Bank){ NULL,             0x00000,   read_hwio,       write_hwio };
	banks[0x4] = (Bank){ NULL,             0x00000,   video_palette_r, video_palette_w };
	banks[0xC] = (Bank){ BIOS,             0x1FFFF,   NULL,            write_unk };
	banks[0xD] = (Bank){ BACKUP_RAM,       0x0FFFF,   NULL,            write_unk };

	// NOTE: m64k TLB memory mapping and custom exception vectors removed.
	// Musashi uses the bank-based m68k_read/write_memory callbacks above.

	rtc_init_();
	watchdog_init();
}

/* Update the fetch pointer for fast instruction reads.
 * Maps 68K PC regions to direct host memory pointers so that
 * m68ki_read_imm_16 can bypass the bank lookup entirely. */
void m68k_reset_fetch_ptr(void) {
	extern m68ki_cpu_core m68ki_cpu;
	m68ki_cpu.fetch_ptr = NULL;
	m68ki_cpu.fetch_region = 0xFF;
}

void m68k_update_fetch_ptr(unsigned int pc) {
	extern m68ki_cpu_core m68ki_cpu;
	unsigned int region = (pc >> 20) & 0xF;

	/* fetch_region is initialized to 0xFF (invalid) at startup to force
	 * the first call to always recalculate. */

	/* Only recalculate if we changed regions */
	if (region == m68ki_cpu.fetch_region)
		return;
	m68ki_cpu.fetch_region = region;

	switch (region) {
	case 0x0:
		/* P-ROM: 0x000000-0x0FFFFF, 1MB */
		m68ki_cpu.fetch_ptr = (uint16*)P_ROM;
		break;
	case 0x1:
		/* WORK_RAM: 0x100000-0x10FFFF, 64KB (mirrored) */
		m68ki_cpu.fetch_ptr = (uint16*)(WORK_RAM - 0x100000);
		break;
	case 0xC:
		/* BIOS: 0xC00000-0xC1FFFF, 128KB */
		m68ki_cpu.fetch_ptr = (uint16*)(BIOS - 0xC00000);
		break;
	default:
		/* Unknown region or HWIO — use slow path */
		m68ki_cpu.fetch_ptr = NULL;
		break;
	}
}

void hw_vblank(void) {
	lspc_vblank();
	watchdog_vblank();
}
