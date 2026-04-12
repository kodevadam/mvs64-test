/**
 * TLB Refill exception handler for NeoGeo 68K address space.
 *
 * Virtual addresses 0xFF000000-0xFFFFFFFF map to the 68K's 16MB
 * address space. This handler is installed at the TLB Refill vector
 * (0x80000000) and installs TLB entries on-demand from a pre-built
 * mapping table.
 *
 * The mapping table (tlb_map) has 256 entries (one per 64KB region
 * of the 68K's 16MB space). Each entry is a pre-built EntryLo value
 * (PFN + flags) ready to write to C0_ENTRYLO. Zero = unmapped.
 */

    .set noreorder
    .set noat

    .data
    .align 4

/* Pre-built TLB mapping table: 256 entries covering 16MB in 64KB pages.
 * Each entry is a ready-made EntryLo value (PFN | Valid | Global | Dirty).
 * Populated by tlb_handler_init() at startup. Zero = unmapped. */
    .globl tlb_map
tlb_map:
    .space 256 * 4

/* Ring counter for non-wired TLB entries (indices 0-31) */
tlb_ring_idx:
    .word 0

    .text
    .align 4

/**
 * TLB Refill handler — installed at exception vector 0x80000000.
 * Must be position-independent since it's copied to the vector address.
 * Uses only k0/k1 (kernel temporaries, not saved by normal code).
 *
 * Fast path: ~12 instructions for a mapped region.
 */
    .globl tlb_refill_handler
tlb_refill_handler:
    mfc0    k0, $8              /* C0_BADVADDR: faulting virtual address */

    /* Check if address is in our range: 0xFF000000-0xFFFFFFFF.
     * BADVADDR high byte must be 0xFF. */
    srl     k1, k0, 24
    xori    k1, k1, 0xFF
    bnez    k1, tlb_refill_passthrough
    nop

    /* Extract 68K region index: bits [23:16] of the virtual address.
     * This gives us an index 0-255 into the 64KB-granularity map. */
    srl     k1, k0, 16
    andi    k1, k1, 0xFF
    sll     k1, k1, 2          /* word offset */

    /* Load pre-built EntryLo value from mapping table */
    la      k0, tlb_map
    add     k0, k0, k1
    lw      k1, 0(k0)

    /* If zero, this region is unmapped — fall through to libdragon */
    beqz    k1, tlb_refill_passthrough
    nop

    /* Install TLB entry.
     * EntryHi is already set by the hardware (C0_BADVADDR context).
     * We use 64KB pages (PageMask = 0x0F << 13 = 0x1E000).
     * Each TLB entry maps two 64KB pages (EntryLo0 + EntryLo1). */
    li      k0, 0x0001E000      /* PageMask for 64KB pages */
    mtc0    k0, $5              /* C0_PAGEMASK */

    /* EntryLo0 = mapping for the even page */
    mtc0    k1, $2              /* C0_ENTRYLO0 */
    /* EntryLo1 = next 64KB page (PFN + 0x40 = next 64KB) */
    addiu   k1, k1, 0x400      /* add 64KB worth of PFN (0x10000 >> 6) */
    mtc0    k1, $3              /* C0_ENTRYLO1 */

    /* Write to a random TLB slot (non-wired) */
    tlbwr

    eret

tlb_refill_passthrough:
    /* Not our address range or unmapped — jump to libdragon's handler.
     * The original instruction at 0x80000000 was saved and we jump
     * to 0x80000004 (the second instruction of libdragon's handler). */
    la      k0, 0x80000180     /* libdragon's general exception vector */
    jr      k0
    nop

    .globl tlb_refill_handler_end
tlb_refill_handler_end:

    .size tlb_refill_handler, tlb_refill_handler_end - tlb_refill_handler


/**
 * C-callable function to initialize the TLB mapping table and install
 * the refill handler at the exception vector.
 *
 * void tlb_handler_init(void);
 *
 * Must be called after ROMs are loaded and memory is allocated.
 */
    .text
    .align 4
    .globl tlb_handler_install
tlb_handler_install:
    /* Copy the refill handler to exception vector 0x80000000 */
    la      t0, tlb_refill_handler
    la      t1, tlb_refill_handler_end
    li      t2, 0x80000000

1:  lw      t3, 0(t0)
    sw      t3, 0(t2)
    addiu   t0, t0, 4
    addiu   t2, t2, 4
    bne     t0, t1, 1b
    nop

    /* Flush instruction cache for the vector area */
    li      t0, 0x80000000
    li      t1, 0x80000080     /* 128 bytes should be enough */
2:  cache   0x10, 0(t0)        /* Hit Invalidate I-cache */
    addiu   t0, t0, 32
    bne     t0, t1, 2b
    nop

    jr      ra
    nop
