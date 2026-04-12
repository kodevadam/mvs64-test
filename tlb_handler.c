/**
 * TLB handler initialization for NeoGeo 68K address space.
 *
 * Populates the tlb_map[] table with pre-built EntryLo values for
 * each 64KB region of the 68K's 16MB address space, then installs
 * the TLB Refill exception handler at vector 0x80000000.
 */

#ifdef N64
#ifdef USE_TLB_FETCH

#include <stdint.h>
#include <string.h>
#include <libdragon.h>
#include "hw.h"
#include "roms.h"

/* Defined in tlb_vector.S */
extern uint32_t tlb_map[256];
extern void tlb_handler_install(void);

/**
 * Build an EntryLo value for a given physical address.
 * EntryLo format: [PFN(25:6)] [C(5:3)] [D(2)] [V(1)] [G(0)]
 */
static uint32_t make_entrylo(uint32_t phys_addr, int writable) {
    uint32_t entry = (phys_addr >> 6) & 0x03FFFFC0;  /* PFN at bits [25:6] */
    entry |= (3 << 3);  /* cached */
    entry |= (1 << 1);  /* valid */
    entry |= (1 << 0);  /* global */
    if (writable)
        entry |= (1 << 2);  /* dirty */
    return entry;
}

/**
 * Flush all 32 TLB entries by writing unmatchable EntryHi values.
 */
static void tlb_flush_all(void) {
    for (int i = 0; i < 32; i++) {
        C0_WRITE_INDEX(i);
        C0_WRITE_ENTRYHI(0x80000000 | (i << 13));
        C0_WRITE_ENTRYLO0(0);
        C0_WRITE_ENTRYLO1(0);
        C0_WRITE_PAGEMASK(0);
        C0_TLBWI();
    }
}

/**
 * Map one 68K megabyte bank (16 x 64KB pages) into tlb_map[].
 * Handles mirroring: offset wraps with (i * 0x10000) & mask.
 * bank_num: 0x0-0xF (68K address bits [23:20])
 */
void tlb_map_bank(int bank_num, uint8_t *mem, uint32_t mask, int writable) {
    if (!mem) return;
    uint32_t phys_base = PhysicalAddr(mem);
    int base_idx = bank_num * 16;
    for (int i = 0; i < 16; i++) {
        uint32_t offset = (i * 0x10000) & mask;
        tlb_map[base_idx + i] = make_entrylo(phys_base + offset, writable);
    }
}

/**
 * Clear one 68K megabyte bank in tlb_map[].
 */
void tlb_unmap_bank(int bank_num) {
    memset(&tlb_map[bank_num * 16], 0, 16 * sizeof(uint32_t));
}

void tlb_handler_init(void) {
    memset(tlb_map, 0, sizeof(uint32_t) * 256);

    /* Reset all TLB entries */
    tlb_flush_all();
    C0_WRITE_WIRED(0);

    /* Install the refill handler at the exception vector */
    disable_interrupts();
    tlb_handler_install();
    enable_interrupts();

    debugf("[TLB] Refill handler installed at 0x80000000 + 0x80000080\n");
}

/**
 * Flush TLB hardware entries after mapping changes (e.g. bank switch).
 */
void tlb_flush(void) {
    tlb_flush_all();
}

#endif /* USE_TLB_FETCH */
#endif /* N64 */
