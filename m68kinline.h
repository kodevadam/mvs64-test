#ifndef M68KINLINE_H
#define M68KINLINE_H

#include <stdint.h>
#include <stdbool.h>
#include "roms.h"
#include "hw.h"
#include "platform.h"

// Typedefs for unaligned memory accesses (68K does 32-bit at 16-bit alignment)
typedef uint32_t u_uint32_t __attribute__((aligned(1)));

// Slow-path functions defined in hw.c (full bank lookup + callback dispatch)
unsigned int m68k_read_memory_8_slow(unsigned int address);
unsigned int m68k_read_memory_16_slow(unsigned int address);
unsigned int m68k_read_memory_32_slow(unsigned int address);
void m68k_write_memory_8_slow(unsigned int address, unsigned int value);
void m68k_write_memory_16_slow(unsigned int address, unsigned int value);
void m68k_write_memory_32_slow(unsigned int address, unsigned int value);

// Inline fast-path memory access.
// The NeoGeo 68K spends most of its time fetching instructions from P-ROM
// (0x0xxxxx) and accessing WORK_RAM (0x1xxxxx). By inlining these two cases,
// we eliminate the bank table lookup, function pointer check, and call overhead
// for ~80% of all memory accesses.

static inline unsigned int m68k_read_memory_16(unsigned int address) {
	unsigned int bank = (address >> 20) & 0xF;
	if (bank == 0)
		return BE16(*(uint16_t*)(P_ROM + (address & 0xFFFFF)));
	if (bank == 1)
		return BE16(*(uint16_t*)(WORK_RAM + (address & 0xFFFF)));
	if (bank == 0xC)
		return BE16(*(uint16_t*)(BIOS + (address & 0x1FFFF)));
	return m68k_read_memory_16_slow(address);
}

static inline unsigned int m68k_read_memory_8(unsigned int address) {
	unsigned int bank = (address >> 20) & 0xF;
	if (bank == 0)
		return *(uint8_t*)(P_ROM + (address & 0xFFFFF));
	if (bank == 1)
		return *(uint8_t*)(WORK_RAM + (address & 0xFFFF));
	if (bank == 0xC)
		return *(uint8_t*)(BIOS + (address & 0x1FFFF));
	return m68k_read_memory_8_slow(address);
}

static inline unsigned int m68k_read_memory_32(unsigned int address) {
	unsigned int bank = (address >> 20) & 0xF;
	if (bank == 0)
		return BE32(*(u_uint32_t*)(P_ROM + (address & 0xFFFFF)));
	if (bank == 1)
		return BE32(*(u_uint32_t*)(WORK_RAM + (address & 0xFFFF)));
	if (bank == 0xC)
		return BE32(*(u_uint32_t*)(BIOS + (address & 0x1FFFF)));
	return m68k_read_memory_32_slow(address);
}

static inline void m68k_write_memory_8(unsigned int address, unsigned int value) {
	unsigned int bank = (address >> 20) & 0xF;
	if (bank == 1) {
		*(uint8_t*)(WORK_RAM + (address & 0xFFFF)) = value;
		return;
	}
	m68k_write_memory_8_slow(address, value);
}

static inline void m68k_write_memory_16(unsigned int address, unsigned int value) {
	unsigned int bank = (address >> 20) & 0xF;
	if (bank == 1) {
		*(uint16_t*)(WORK_RAM + (address & 0xFFFF)) = BE16(value);
		return;
	}
	m68k_write_memory_16_slow(address, value);
}

static inline void m68k_write_memory_32(unsigned int address, unsigned int value) {
	unsigned int bank = (address >> 20) & 0xF;
	if (bank == 1) {
		*(u_uint32_t*)(WORK_RAM + (address & 0xFFFF)) = BE32(value);
		return;
	}
	m68k_write_memory_32_slow(address, value);
}

static inline bool m68k_check_idle_skip(unsigned int address) {
	return address == rom_pc_idle_skip;
}

#endif
