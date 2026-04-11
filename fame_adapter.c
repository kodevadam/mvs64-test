/*
 * fame_adapter.c — FAME 68K adapter for MVS64 NeoGeo emulator.
 *
 * Provides the memory callbacks, fetch table setup, and interrupt handling
 * glue between FAME and the NeoGeo hardware emulation in hw.c.
 *
 * Build with -DUSE_FAME to enable; otherwise Musashi is used.
 */
#ifdef USE_FAME

#include <string.h>
#include "cpu/fame/fame.h"
#include "hw.h"
#include "roms.h"
#include "platform.h"

/* The global FAME CPU context */
M68K_CONTEXT fame_ctx;

/* Track the initial timeslice for mid-execution clock calculation */
static int fame_initial_cycles;

/* Forward declarations for NeoGeo HWIO handlers (defined in hw.c) */
extern uint32_t read_hwio(uint32_t addr, int sz);
extern void write_hwio(uint32_t addr, uint32_t val, int sz);
extern uint32_t video_palette_r(uint32_t addr, int sz);
extern void video_palette_w(uint32_t addr, uint32_t val, int sz);
extern uint32_t read_pbrom(uint32_t addr, int sz);
extern void write_pbrom(uint32_t addr, uint32_t val, int sz);

/* External memory arrays */
extern uint8_t *P_ROM;
extern uint8_t BIOS[];
extern uint8_t WORK_RAM[];
extern uint8_t BACKUP_RAM[];
extern uint16_t PALETTE_RAM[];
extern int PALETTE_RAM_BANK;

/* Typedefs for unaligned 32-bit access (68K does 32-bit at 16-bit alignment) */
typedef uint32_t u_uint32_t __attribute__((aligned(1)));

/*
 * FAME memory callbacks for NeoGeo.
 * These match the bank layout in hw.c:
 *   0x0: P-ROM (1MB, read-only)
 *   0x1: WORK_RAM (64KB, R/W)
 *   0x2: PBROM (banked, callbacks)
 *   0x3: HWIO (callbacks)
 *   0x4: Palette RAM (callbacks)
 *   0xC: BIOS (128KB, read-only)
 *   0xD: Backup RAM (64KB)
 */

static unsigned int fame_read_byte(unsigned int address) {
    address &= 0xFFFFFF;
    unsigned int bank = (address >> 20) & 0xF;
    switch (bank) {
    case 0x0: return *(uint8_t*)(P_ROM + (address & 0xFFFFF));
    case 0x1: return *(uint8_t*)(WORK_RAM + (address & 0xFFFF));
    case 0x2: return read_pbrom(address, 1);
    case 0x3: return read_hwio(address, 1);
    case 0x4: return video_palette_r(address, 1);
    case 0xC: return *(uint8_t*)(BIOS + (address & 0x1FFFF));
    case 0xD: return *(uint8_t*)(BACKUP_RAM + (address & 0xFFFF));
    default:  return 0xFF;
    }
}

static unsigned int fame_read_word(unsigned int address) {
    address &= 0xFFFFFF;
    unsigned int bank = (address >> 20) & 0xF;
    switch (bank) {
    case 0x0: return *(uint16_t*)(P_ROM + (address & 0xFFFFF));
    case 0x1: return *(uint16_t*)(WORK_RAM + (address & 0xFFFF));
    case 0x2: return read_pbrom(address, 2);
    case 0x3: return read_hwio(address, 2);
    case 0x4: return video_palette_r(address, 2);
    case 0xC: return *(uint16_t*)(BIOS + (address & 0x1FFFF));
    case 0xD: return *(uint16_t*)(BACKUP_RAM + (address & 0xFFFF));
    default:  return 0xFFFF;
    }
}

static unsigned int fame_read_long(unsigned int address) {
    address &= 0xFFFFFF;
    unsigned int bank = (address >> 20) & 0xF;
    switch (bank) {
    case 0x0: return *(u_uint32_t*)(P_ROM + (address & 0xFFFFF));
    case 0x1: return *(u_uint32_t*)(WORK_RAM + (address & 0xFFFF));
    case 0x2: return read_pbrom(address, 4);
    case 0x3: return read_hwio(address, 4);
    case 0x4: return video_palette_r(address, 4);
    case 0xC: return *(u_uint32_t*)(BIOS + (address & 0x1FFFF));
    case 0xD: return *(u_uint32_t*)(BACKUP_RAM + (address & 0xFFFF));
    default:  return 0;
    }
}

static void fame_write_byte(unsigned int address, unsigned char value) {
    address &= 0xFFFFFF;
    unsigned int bank = (address >> 20) & 0xF;
    switch (bank) {
    case 0x1: *(uint8_t*)(WORK_RAM + (address & 0xFFFF)) = value; return;
    case 0x2: write_pbrom(address, value, 1); return;
    case 0x3: write_hwio(address, value, 1); return;
    case 0x4: video_palette_w(address, value, 1); return;
    case 0xD: *(uint8_t*)(BACKUP_RAM + (address & 0xFFFF)) = value; return;
    default:  return; /* writes to ROM are ignored */
    }
}

static void fame_write_word(unsigned int address, unsigned short value) {
    address &= 0xFFFFFF;
    unsigned int bank = (address >> 20) & 0xF;
    switch (bank) {
    case 0x1: *(uint16_t*)(WORK_RAM + (address & 0xFFFF)) = value; return;
    case 0x2: write_pbrom(address, value, 2); return;
    case 0x3: write_hwio(address, value, 2); return;
    case 0x4: video_palette_w(address, value, 2); return;
    case 0xD: *(uint16_t*)(BACKUP_RAM + (address & 0xFFFF)) = value; return;
    default:  return;
    }
}

static void fame_write_long(unsigned int address, unsigned int value) {
    address &= 0xFFFFFF;
    unsigned int bank = (address >> 20) & 0xF;
    switch (bank) {
    case 0x1: *(u_uint32_t*)(WORK_RAM + (address & 0xFFFF)) = value; return;
    case 0x2: write_pbrom(address, value, 4); return;
    case 0x3: write_hwio(address, value, 4); return;
    case 0x4: video_palette_w(address, value, 4); return;
    case 0xD: *(u_uint32_t*)(BACKUP_RAM + (address & 0xFFFF)) = value; return;
    default:  return;
    }
}

/*
 * Interrupt acknowledge handler.
 * On NeoGeo, interrupts are manually acknowledged by writing to 0x3C000C.
 * We do nothing here — just leave the IRQ asserted until the game acks it.
 */
static void fame_iack_handler(unsigned level) {
    (void)level;
    /* Don't auto-clear — NeoGeo requires manual ack via LSPC register */
}

/*
 * Initialize the FAME context for NeoGeo emulation.
 */
void fame_adapter_init(void) {
    /* Initialize the global jump table (call once) */
    fm68k_init();

    /* Zero the context */
    memset(&fame_ctx, 0, sizeof(fame_ctx));

    /* Set memory callbacks */
    fame_ctx.read_byte  = fame_read_byte;
    fame_ctx.read_word  = fame_read_word;
    fame_ctx.read_long  = fame_read_long;
    fame_ctx.write_byte = fame_write_byte;
    fame_ctx.write_word = fame_write_word;
    fame_ctx.write_long = fame_write_long;

    /* Set interrupt acknowledge handler */
    fame_ctx.iack_handler = fame_iack_handler;

    /* Set initial SR (supervisor mode, all interrupts masked, Z flag set) */
    fame_ctx.sr = 0x2704;

    /* Populate the Fetch table for direct opcode access.
     * Each entry covers 64KB (1 << (24 - FAMEC_FETCHBITS)).
     * Fetch[bank] = base_ptr - bank_start so that
     * (u16*)(pc + Fetch[pc >> 16]) gives the right host pointer.
     */
    int i;

    /* P-ROM: 0x000000-0x0FFFFF (banks 0x00-0x0F) */
    for (i = 0x00; i <= 0x0F; i++)
        fame_ctx.Fetch[i] = (uintptr_t)P_ROM;

    /* WORK_RAM: 0x100000-0x10FFFF (bank 0x10, mirrored) */
    fame_ctx.Fetch[0x10] = (uintptr_t)WORK_RAM - 0x100000;

    /* BIOS: 0xC00000-0xC1FFFF (banks 0xC0-0xC1) */
    for (i = 0xC0; i <= 0xC1; i++)
        fame_ctx.Fetch[i] = (uintptr_t)BIOS - 0xC00000;
}

/*
 * Reset the FAME CPU (reads SP from addr 0, PC from addr 4).
 */
void fame_adapter_reset(void) {
    fm68k_reset(&fame_ctx);
}

/*
 * Execute 68K instructions for the given number of cycles.
 * Returns the actual number of cycles consumed.
 */
int fame_adapter_execute(int cycles) {
    fame_initial_cycles = cycles;

    return fm68k_emulate(&fame_ctx, cycles, fm68k_reason_emulate);
}

/*
 * Get cycles consumed so far in the current timeslice.
 * FAME's io_cycle_counter counts DOWN from the initial value.
 */
int fame_adapter_cycles_run(void) {
    return fame_initial_cycles - fame_ctx.io_cycle_counter;
}

/*
 * Set/clear a virtual IRQ line (NeoGeo uses levels 1-3).
 */
void fame_adapter_set_virq(int level, int active) {
    if (active) {
        /* Assert: set the pending level if it's higher than current */
        if (level > (int)fame_ctx.interrupts[0])
            fame_ctx.interrupts[0] = level;
    } else {
        /* Deassert: clear if this was the pending level */
        if (level == (int)fame_ctx.interrupts[0])
            fame_ctx.interrupts[0] = 0;
    }
}

/*
 * Get the current PC.
 */
uint32_t fame_adapter_get_pc(void) {
    return fm68k_get_pc(&fame_ctx) & 0xFFFFFF;
}


/*
 * End the current timeslice early (for event rescheduling).
 */
void fame_adapter_end_timeslice(void) {
    fame_ctx.io_cycle_counter = 0;
}

#endif /* USE_FAME */
