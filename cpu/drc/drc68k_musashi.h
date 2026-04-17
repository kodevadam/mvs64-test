/*
 * DRC68K adapter for Musashi (MVS64)
 *
 * Maps Musashi's m68ki_cpu_core fields to the offsets the DRC expects.
 * The DRC was written against FAME's M68K_CONTEXT; this header bridges
 * the gap so drc68k.c can target Musashi without rewriting every access.
 *
 * Key differences from FAME:
 *   - Registers: Musashi uses dar[16] (D0-D7 then A0-A7 contiguous).
 *     FAME has separate dreg[8] and areg[8].
 *   - Flags: both use individual uint32 fields, but the encoding differs.
 *     Musashi stores n_flag = result >> 24 (check bit 7 for N).
 *     FAME stores flag_N = full result (check bit 31 for N).
 *     The DRC's emit_update_nz_long() must be adapted.
 *   - Cycles: Musashi uses an external global (m68ki_remaining_cycles).
 *     FAME has io_cycle_counter inside the context struct.
 *   - PC fetch: Musashi has pc_fetch_base (cached RDRAM pointer).
 *     FAME has a Fetch[] table indexed by address range.
 */

#ifndef DRC68K_MUSASHI_H
#define DRC68K_MUSASHI_H

#include "m68kcpu.h"

/* The DRC operates on a pointer to m68ki_cpu_core (Musashi's CPU state).
 * Replace FAME's M68K_CONTEXT with Musashi's type. */
typedef m68ki_cpu_core DRC_CPU_CONTEXT;

/* Field offsets into m68ki_cpu_core for register access from emitted MIPS.
 * Musashi stores D0-D7 in dar[0..7], A0-A7 in dar[8..15]. */
#define DRC_OFF_DREG(n)   (offsetof(m68ki_cpu_core, dar) + (n) * sizeof(uint))
#define DRC_OFF_AREG(n)   (offsetof(m68ki_cpu_core, dar) + (8 + (n)) * sizeof(uint))
#define DRC_OFF_PC        (offsetof(m68ki_cpu_core, pc))
#define DRC_OFF_PPC       (offsetof(m68ki_cpu_core, ppc))

/* Flag field offsets.
 * Musashi flag encoding (all uint32):
 *   n_flag:     result >> 24 for long-size ops (N = bit 7)
 *   not_z_flag: full result (Z set when == 0)
 *   v_flag:     overflow indicator
 *   c_flag:     carry indicator
 *   x_flag:     extend (same as carry for arithmetic)
 */
#define DRC_OFF_FLAG_N    (offsetof(m68ki_cpu_core, n_flag))
#define DRC_OFF_FLAG_NZ   (offsetof(m68ki_cpu_core, not_z_flag))
#define DRC_OFF_FLAG_V    (offsetof(m68ki_cpu_core, v_flag))
#define DRC_OFF_FLAG_C    (offsetof(m68ki_cpu_core, c_flag))
#define DRC_OFF_FLAG_X    (offsetof(m68ki_cpu_core, x_flag))

/* Cycle counter: Musashi uses an external global, not a struct field.
 * The DRC will pass a pointer to m68ki_remaining_cycles as a separate
 * argument, or the compiled block can access it via a dedicated register. */
#define DRC_OFF_FETCH_BASE (offsetof(m68ki_cpu_core, pc_fetch_base))

/* Musashi's memory access functions (declared in m68kinline.h).
 * The DRC calls these for memory operations that can't be inlined. */
extern unsigned int m68k_read_memory_8(unsigned int address);
extern unsigned int m68k_read_memory_16(unsigned int address);
extern unsigned int m68k_read_memory_32(unsigned int address);
extern void m68k_write_memory_8(unsigned int address, unsigned int value);
extern void m68k_write_memory_16(unsigned int address, unsigned int value);
extern void m68k_write_memory_32(unsigned int address, unsigned int value);

/* ROM pointers for instruction fetch during block compilation.
 * The DRC reads opcodes directly from these at compile time. */
extern uint8_t P_ROM[];
extern uint8_t *BIOS;
extern uint8_t WORK_RAM[];

/* Fetch a 68K opcode word from the ROM for compilation (not execution).
 * Uses the same bank logic as m68ki_update_fetch_base. */
static inline uint16_t drc_fetch_68k_word(uint32_t addr)
{
    addr &= 0xFFFFFF;
    uint32_t bank = (addr >> 20) & 0xF;
    const uint8_t *base;
    switch (bank) {
    case 0x0: base = P_ROM; break;
    case 0x1: base = WORK_RAM - 0x100000; break;
    case 0xC: base = BIOS - 0xC00000; break;
    default:  return 0x4E71; /* NOP for unmapped regions */
    }
    return (base[addr] << 8) | base[addr + 1]; /* big-endian 68K */
}

#endif /* DRC68K_MUSASHI_H */
