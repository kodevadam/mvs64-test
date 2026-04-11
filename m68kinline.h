#ifndef M68KINLINE_H
#define M68KINLINE_H

#include <stdint.h>
#include <stdbool.h>
#include "roms.h"

// NOTE: The N64 inline memory handlers that used direct TLB-mapped access
// (for m64k) have been removed. Musashi uses the bank-based callbacks
// defined in hw.c (m68k_read_memory_8/16/32, m68k_write_memory_8/16/32).

static inline bool m68k_check_idle_skip(unsigned int address) {
	return address == rom_pc_idle_skip;
}

#endif
