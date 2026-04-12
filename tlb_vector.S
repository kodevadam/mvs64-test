/**
 * TLB Refill exception handler for NeoGeo 68K address space.
 *
 * Virtual addresses 0xFF000000-0xFFFFFFFF map to the 68K's 16MB
 * address space. This handler is installed at the TLB Refill vector
 * (0x80000000) and installs TLB entries on-demand from a pre-built
 * mapping table.
 *
 * Each TLB entry covers 128KB (two 64KB pages). The handler always
 * indexes by the even page of the pair, loading Lo0 and Lo1 from
 * adjacent tlb_map[] entries. This correctly handles mirrored regions
 * (e.g. WORK_RAM mirroring every 64KB).
 */

    .set noreorder
    .set noat

/* Register aliases using numeric names for N64 toolchain */
#define k0 $26
#define k1 $27
#define t0 $8
#define t1 $9
#define t2 $10
#define t3 $11
#define ra $31

    .data
    .align 4

    .globl tlb_map
tlb_map:
    .space 256 * 4

    .text
    .align 4

/**
 * TLB Refill handler — installed at exception vector 0x80000000.
 * Uses only k0/k1 (kernel temporaries).
 * Fast path: ~14 instructions for a mapped region.
 */
    .globl tlb_refill_handler
tlb_refill_handler:
    mfc0    k0, $8              /* C0_BADVADDR */

    /* Check if high byte is 0xFF */
    srl     k1, k0, 24
    xori    k1, k1, 0xFF
    bnez    k1, tlb_refill_passthrough
    nop

    /* Set PageMask for 64KB pages early (frees k0/k1 for table load) */
    lui     k1, 0x0001
    ori     k1, k1, 0xE000
    mtc0    k1, $5              /* C0_PAGEMASK */

    /* Index into mapping table: bits [23:16], masked to even (0xFE)
     * because each TLB entry covers a PAIR of 64KB pages. */
    srl     k1, k0, 16
    andi    k1, k1, 0xFE
    sll     k1, k1, 2

    /* Load both Lo0 and Lo1 from adjacent table entries */
    lui     k0, %hi(tlb_map)
    addiu   k0, k0, %lo(tlb_map)
    addu    k0, k0, k1
    lw      k1, 0(k0)          /* Lo0 = even page */
    lw      k0, 4(k0)          /* Lo1 = odd page */

    /* Unmapped if Lo0 is zero */
    beqz    k1, tlb_refill_passthrough
    nop

    /* Install the TLB entry */
    mtc0    k1, $2              /* C0_ENTRYLO0 */
    mtc0    k0, $3              /* C0_ENTRYLO1 */

    tlbwr
    eret

tlb_refill_passthrough:
    /* Fall through to libdragon's general exception handler */
    lui     k0, 0x8000
    ori     k0, k0, 0x0180
    jr      k0
    nop

    .globl tlb_refill_handler_end
tlb_refill_handler_end:


/**
 * tlb_handler_install: copy handler to exception vector 0x80000000
 */
    .globl tlb_handler_install
tlb_handler_install:
    la      t0, tlb_refill_handler
    la      t1, tlb_refill_handler_end
    li      t2, 0xA0000000      /* kseg1 (uncached) — bypasses D-cache,
                                 * writes directly to RDRAM at phys 0x0 */

1:  lw      t3, 0(t0)
    sw      t3, 0(t2)
    addiu   t0, t0, 4
    addiu   t2, t2, 4
    bne     t0, t1, 1b
    nop

    /* Invalidate any stale D-cache lines at kseg0 vector address */
    li      t0, 0x80000000
    li      t1, 0x80000080
3:  cache   0x11, 0(t0)        /* Hit Invalidate — D-cache (16-byte lines) */
    addiu   t0, t0, 16
    bne     t0, t1, 3b
    nop

    /* Invalidate I-cache so CPU fetches fresh code from RDRAM */
    li      t0, 0x80000000
    li      t1, 0x80000080
2:  cache   0x10, 0(t0)        /* Hit Invalidate — I-cache (32-byte lines) */
    addiu   t0, t0, 32
    bne     t0, t1, 2b
    nop

    jr      ra
    nop
