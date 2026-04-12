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
 * EntryLo format: [PFN(29:6)] [C(5:3)] [D(2)] [V(1)] [G(0)]
 *   PFN = physical_address >> 12, shifted to bits [29:6]
 *   C = cache algorithm (3 = cached)
 *   D = dirty (writable)
 *   V = valid
 *   G = global
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

void tlb_handler_init(void) {
    /* Clear the mapping table */
    memset(tlb_map, 0, sizeof(uint32_t) * 256);

    /* Map P-ROM: 68K 0x000000-0x0FFFFF (1MB = 16 x 64KB pages)
     * Physical address of P_ROM, read-only */
    uint32_t prom_phys = PhysicalAddr(P_ROM);
    for (int i = 0; i < 16; i++) {
        tlb_map[i] = make_entrylo(prom_phys + i * 0x10000, 0);
    }

    /* Map WORK_RAM: 68K 0x100000-0x10FFFF (64KB = 1 x 64KB page)
     * Physical address of WORK_RAM, read-write */
    uint32_t wram_phys = PhysicalAddr(WORK_RAM);
    tlb_map[0x10] = make_entrylo(wram_phys, 1);

    /* Map BIOS: 68K 0xC00000-0xC1FFFF (128KB = 2 x 64KB pages)
     * Physical address of BIOS, read-only */
    uint32_t bios_phys = PhysicalAddr(BIOS);
    tlb_map[0xC0] = make_entrylo(bios_phys, 0);
    tlb_map[0xC1] = make_entrylo(bios_phys + 0x10000, 0);

    debugf("[TLB] P-ROM phys=0x%08lx WORK_RAM phys=0x%08lx BIOS phys=0x%08lx\n",
           prom_phys, wram_phys, bios_phys);
    debugf("[TLB] Map entries: P-ROM[0]=%08lx WRAM[0x10]=%08lx BIOS[0xC0]=%08lx\n",
           tlb_map[0], tlb_map[0x10], tlb_map[0xC0]);

    /* Reset all TLB entries first */
    for (int i = 0; i < 32; i++) {
        C0_WRITE_INDEX(i);
        C0_WRITE_ENTRYHI(0x80000000 | (i << 13)); /* unmapped addresses */
        C0_WRITE_ENTRYLO0(0);
        C0_WRITE_ENTRYLO1(0);
        C0_WRITE_PAGEMASK(0);
        C0_TLBWI();
    }
    C0_WRITE_WIRED(0);

    /* Install the refill handler at the exception vector */
    disable_interrupts();
    tlb_handler_install();
    enable_interrupts();

    debugf("[TLB] Refill handler installed at 0x80000000\n");
}

#endif /* USE_TLB_FETCH */
#endif /* N64 */
