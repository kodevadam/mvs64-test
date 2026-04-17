#include <rsp_queue.inc>

    .data

    RSPQ_BeginOverlayHeader
        RSPQ_DefineCommand cmd_fm_render, 12
        RSPQ_DefineCommand cmd_fm_init_tables, 12
    RSPQ_EndOverlayHeader

    RSPQ_BeginSavedState
    .align 3
sin_tab:    .space 512       /* 256 x uint16_t */
exp_tab:    .space 512       /* 256 x uint16_t */
    RSPQ_EndSavedState

    .bss
    .align 3
chan_state:  .space 228       /* YM2610: 4 channels * 56 bytes + 4 bytes header */
work_out:   .space 1024      /* up to 256 x int32_t output samples */
rdram_state: .space 4        /* saved RDRAM addr for state DMA back */
rdram_out:   .space 4        /* saved RDRAM addr for output DMA */

    .text

cmd_fm_init_tables:
    /* a1 = PhysicalAddr(sin_tab), a2 = PhysicalAddr(exp_tab) */
    move    v0, ra

    move    s0, a1
    li      s4, %lo(sin_tab)
    li      t0, DMA_SIZE(512, 1)
    jal     DMAIn
    nop

    move    s0, a2
    li      s4, %lo(exp_tab)
    li      t0, DMA_SIZE(512, 1)
    jal     DMAIn
    nop

    jr      v0
    nop

    ##############################################
    # cmd_fm_render
    #
    # a0[23:0]  = num_samples
    # a1        = PhysicalAddr(state)  [344 bytes]
    # a2        = PhysicalAddr(out_buf)
    #
    # Register allocation:
    #   v0 = saved ra (return to rspq)
    #   v1 = channel pointer in DMEM
    #   fp = channel counter (6..0)        ** NOT gp - gp is rspq_dmem_buf_ptr! **
    #   k1 = num_samples (preserved)
    #   k0 = output pointer (per sample)
    #   t9 = sample counter
    #   s1 = routing value for op1
    #   s2 = C1 result
    #   s3 = M2 result
    #   s5 = C2 result
    ##############################################

cmd_fm_render:
    move    v0, ra

    /* Save RDRAM addresses */
    sw      a1, %lo(rdram_state)
    sw      a2, %lo(rdram_out)
    andi    k1, a0, 0xFFFF          /* k1 = num_samples */

    /* DMA channel state from RDRAM to DMEM (YM2610: 228 bytes) */
    move    s0, a1
    li      s4, %lo(chan_state)
    li      t0, DMA_SIZE(228, 1)
    jal     DMAIn
    nop

    /* Clear output buffer */
    li      t3, %lo(work_out)
    sll     t4, k1, 2
    add     t5, t3, t4
clear_loop:
    sw      zero, 0(t3)
    addiu   t3, t3, 4
    bne     t3, t5, clear_loop
    nop

    /* ---- Process 4 FM channels (YM2610) ---- */
    li      v1, %lo(chan_state)
    li      fp, 4

chan_loop:
    lbu     t0, 43(v1)              /* enabled */
    beqz    t0, chan_next
    nop

    move    t9, k1                  /* sample counter */
    li      k0, %lo(work_out)      /* output pointer */

sample_loop:
    /* ============ Operator M1 (SLOT1) with feedback ============ */
    lw      t0, 0(v1)              /* phase[0] */
    lhu     t1, 32(v1)             /* vol_out[0] */
    lw      t2, 44(v1)             /* op1_out (prev<<16 | curr) */
    lbu     t3, 41(v1)             /* fb_shift */

    /* Feedback: pm = ((op1_out + (op1_out<<16)) >> 16) << fb_shift */
    beqz    t3, op1_no_fb
    move    t4, zero               /* pm = 0 if no feedback */
    sll     t5, t2, 16
    addu    t5, t2, t5
    sra     t4, t5, 16             /* prev + curr */
    sllv    t4, t4, t3             /* << fb_shift */
op1_no_fb:

    /* op_calc1: sin = (phase + pm) >> 16 */
    addu    t5, t0, t4
    srl     t5, t5, 16
    andi    t6, t5, 0x200          /* neg flag */
    andi    t7, t5, 0x100
    beqz    t7, op1_no_flip
    andi    t5, t5, 0xFF
    xori    t5, t5, 0xFF
    andi    t5, t5, 0xFF
op1_no_flip:
    andi    t1, t1, 0xFFFE         /* env &= ~1 */
    sll     t7, t1, 2              /* env << 2 */
    sll     t8, t5, 1
    lhu     t8, %lo(sin_tab)(t8)
    addu    t7, t7, t8             /* p = (env<<2) + sin_tab[sin] */
    li      t8, 3328
    slt     t8, t7, t8
    beqz    t8, op1_silent
    nop

    andi    t8, t7, 0xFF
    sll     t8, t8, 1
    lhu     t5, %lo(exp_tab)(t8)
    srl     t7, t7, 8
    srlv    t5, t5, t7

    bnez    t6, op1_neg
    nop
    b       op1_update
    nop
op1_neg:
    subu    t5, zero, t5
    b       op1_update
    nop

op1_silent:
    move    t5, zero

op1_update:
    /* Update op1_out: (old << 16) | (new & 0xFFFF) */
    sll     t2, t2, 16
    andi    t8, t5, 0xFFFF
    or      t2, t2, t8
    sw      t2, 44(v1)

    /* s1 = op1_out>>16 for algorithm routing */
    sra     s1, t2, 16

    /* ============ Operator C1 (SLOT2) ============ */
    lbu     t3, 40(v1)             /* c1_mod */
    move    t4, zero
    beqz    t3, c1_calc
    nop
    move    t4, s1                 /* pm = op1 routing value */

c1_calc:
    lw      t0, 4(v1)              /* phase[1] */
    lhu     t1, 34(v1)             /* vol_out[1] */
    srl     t5, t0, 16
    sra     t8, t4, 1
    addu    t5, t5, t8
    andi    t6, t5, 0x200
    andi    t7, t5, 0x100
    beqz    t7, c1_no_flip
    andi    t5, t5, 0xFF
    xori    t5, t5, 0xFF
    andi    t5, t5, 0xFF
c1_no_flip:
    andi    t1, t1, 0xFFFE
    sll     t7, t1, 2
    sll     t8, t5, 1
    lhu     t8, %lo(sin_tab)(t8)
    addu    t7, t7, t8
    li      t8, 3328
    slt     t8, t7, t8
    beqz    t8, c1_silent
    nop
    andi    t8, t7, 0xFF
    sll     t8, t8, 1
    lhu     s2, %lo(exp_tab)(t8)
    srl     t7, t7, 8
    srlv    s2, s2, t7
    beqz    t6, c1_done
    nop
    subu    s2, zero, s2
    b       c1_done
    nop
c1_silent:
    move    s2, zero
c1_done:

    /* ============ Operator M2 (SLOT3) ============ */
    lbu     t3, 42(v1)             /* m2_mod */
    move    t4, zero
    beqz    t3, m2_calc
    nop
    lw      t4, 48(v1)             /* pm = mem */

m2_calc:
    lw      t0, 8(v1)              /* phase[2] */
    lhu     t1, 36(v1)             /* vol_out[2] */
    srl     t5, t0, 16
    sra     t8, t4, 1
    addu    t5, t5, t8
    andi    t6, t5, 0x200
    andi    t7, t5, 0x100
    beqz    t7, m2_no_flip
    andi    t5, t5, 0xFF
    xori    t5, t5, 0xFF
    andi    t5, t5, 0xFF
m2_no_flip:
    andi    t1, t1, 0xFFFE
    sll     t7, t1, 2
    sll     t8, t5, 1
    lhu     t8, %lo(sin_tab)(t8)
    addu    t7, t7, t8
    li      t8, 3328
    slt     t8, t7, t8
    beqz    t8, m2_silent
    nop
    andi    t8, t7, 0xFF
    sll     t8, t8, 1
    lhu     s3, %lo(exp_tab)(t8)
    srl     t7, t7, 8
    srlv    s3, s3, t7
    beqz    t6, m2_done
    nop
    subu    s3, zero, s3
    b       m2_done
    nop
m2_silent:
    move    s3, zero
m2_done:

    /* ============ Operator C2 (SLOT4) ============ */
    /* c2_mod: 0=zero, 1=M2, 2=op1+M2, 3=mem+M2, 4=op1 */
    lbu     t3, 52(v1)
    move    t4, zero
    beqz    t3, c2_calc
    li      t8, 1
    beq     t3, t8, c2_m2
    li      t8, 2
    beq     t3, t8, c2_op1m2
    li      t8, 3
    beq     t3, t8, c2_memm2
    li      t8, 4
    beq     t3, t8, c2_op1
    nop
    b       c2_calc
    nop
c2_m2:
    b       c2_calc
    move    t4, s3
c2_op1m2:
    b       c2_calc
    addu    t4, s1, s3
c2_memm2:
    lw      t4, 48(v1)
    b       c2_calc
    addu    t4, t4, s3
c2_op1:
    move    t4, s1

c2_calc:
    lw      t0, 12(v1)             /* phase[3] */
    lhu     t1, 38(v1)             /* vol_out[3] */
    srl     t5, t0, 16
    sra     t8, t4, 1
    addu    t5, t5, t8
    andi    t6, t5, 0x200
    andi    t7, t5, 0x100
    beqz    t7, c2_no_flip
    andi    t5, t5, 0xFF
    xori    t5, t5, 0xFF
    andi    t5, t5, 0xFF
c2_no_flip:
    andi    t1, t1, 0xFFFE
    sll     t7, t1, 2
    sll     t8, t5, 1
    lhu     t8, %lo(sin_tab)(t8)
    addu    t7, t7, t8
    li      t8, 3328
    slt     t8, t7, t8
    beqz    t8, c2_silent
    nop
    andi    t8, t7, 0xFF
    sll     t8, t8, 1
    lhu     s5, %lo(exp_tab)(t8)
    srl     t7, t7, 8
    srlv    s5, s5, t7
    beqz    t6, c2_done
    nop
    subu    s5, zero, s5
    b       c2_done
    nop
c2_silent:
    move    s5, zero
c2_done:

    /* ============ Update delay memory ============ */
    /* mem_src: 0=none, 1=C1, 2=op1+C1, 3=op1 */
    lbu     t3, 54(v1)
    beqz    t3, mem_done
    li      t8, 1
    beq     t3, t8, mem_c1
    li      t8, 2
    beq     t3, t8, mem_op1c1
    li      t8, 3
    beq     t3, t8, mem_op1
    nop
    b       mem_done
    nop
mem_c1:
    b       mem_done
    sw      s2, 48(v1)
mem_op1c1:
    addu    t0, s1, s2
    b       mem_done
    sw      t0, 48(v1)
mem_op1:
    sw      s1, 48(v1)
mem_done:

    /* ============ Accumulate output ============ */
    lbu     t3, 53(v1)
    lw      t0, 0(k0)

    andi    t8, t3, 1
    beqz    t8, no_op1
    nop
    addu    t0, t0, s1
no_op1:
    andi    t8, t3, 2
    beqz    t8, no_c1
    nop
    addu    t0, t0, s2
no_c1:
    andi    t8, t3, 4
    beqz    t8, no_m2
    nop
    addu    t0, t0, s3
no_m2:
    andi    t8, t3, 8
    beqz    t8, no_c2
    nop
    addu    t0, t0, s5
no_c2:
    sw      t0, 0(k0)
    addiu   k0, k0, 4

    /* ============ Advance phases (AFTER all operators) ============ */
    lw      t0, 0(v1)
    lw      t1, 16(v1)
    addu    t0, t0, t1
    sw      t0, 0(v1)

    lw      t0, 4(v1)
    lw      t1, 20(v1)
    addu    t0, t0, t1
    sw      t0, 4(v1)

    lw      t0, 8(v1)
    lw      t1, 24(v1)
    addu    t0, t0, t1
    sw      t0, 8(v1)

    lw      t0, 12(v1)
    lw      t1, 28(v1)
    addu    t0, t0, t1
    sw      t0, 12(v1)

    addiu   t9, t9, -1
    bnez    t9, sample_loop
    nop

chan_next:
    addiu   v1, v1, 56
    addiu   fp, fp, -1
    bnez    fp, chan_loop
    nop

    /* ---- DMA state back to RDRAM (YM2610: 228 bytes) ---- */
    lw      s0, %lo(rdram_state)
    li      s4, %lo(chan_state)
    li      t0, DMA_SIZE(228, 1)
    jal     DMAOut
    nop

    /* ---- DMA output to RDRAM ---- */
    lw      s0, %lo(rdram_out)
    li      s4, %lo(work_out)
    sll     t0, k1, 2
    addiu   t0, t0, 7
    andi    t0, t0, 0xFFF8
    addiu   t0, t0, -1
    jal     DMAOut
    nop

    jr      v0
    nop
