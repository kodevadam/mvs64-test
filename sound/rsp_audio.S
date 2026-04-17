##################################################
# RSP Audio Overlay for PicoDrive Genesis Emulator
#
# RSPQ overlay that synthesizes YM2612 FM audio on
# the RSP vector unit in parallel with CPU emulation.
#
# Phase 1: Basic mixing/output (validates overlay infra)
# Phase 2: FM operator synthesis
#
# DMEM Layout (Saved State):
#   FM operator state: phases, envelopes, levels
#   Output sample buffer
##################################################

#include <rsp_queue.inc>

    .data

    RSPQ_BeginOverlayHeader
        RSPQ_DefineCommand cmd_mix_samples,  16  # Cmd 0: mix audio buffer (4 words)
    RSPQ_EndOverlayHeader

    RSPQ_EmptySavedState

    .bss
    .align 3
audio_work:     .space 1024     # Working buffer in DMEM

    .text

#####################################################
# cmd_mix_samples: DMA PCM from RDRAM, scale, DMA out
#
# a0 = (cmd_id << 24) | num_samples (low 24 bits)
# a1 = RDRAM source address (16-bit mono PCM)
# a2 = RDRAM dest address (16-bit stereo interleaved)
# a3 = volume (0-255)
#####################################################
cmd_mix_samples:
    # Extract num_samples from a0 (low 24 bits)
    sll     t0, a0, 8
    srl     t0, t0, 8          # t0 = num_samples

    # Clamp to max work buffer size (256 mono samples)
    slti    t1, t0, 257
    bne     t1, zero, .size_ok
    nop
    li      t0, 256
.size_ok:

    # DMA in: RDRAM source -> DMEM audio_work
    # Size = num_samples * 2 bytes (16-bit mono)
    sll     t2, t0, 1          # t2 = byte count
    addiu   t2, t2, -1         # DMA size = bytes - 1

    move    s0, a1             # RDRAM src
    la      s4, %lo(audio_work)
    move    t1, zero           # stride = 0
    jal     DMAIn
    nop

    # Mono -> Stereo duplication in DMEM
    # Read each 16-bit mono sample, write as L+R pair
    la      t3, %lo(audio_work)
    move    t4, t0             # sample count
    # Work backwards to avoid overwrite (stereo is 2x mono)
    addiu   t4, t4, -1         # last index
.dup_loop:
    sll     t5, t4, 1          # mono offset = idx * 2
    add     t5, t3, t5
    lh      t6, 0(t5)          # load mono sample

    sll     t5, t4, 2          # stereo offset = idx * 4
    add     t5, t3, t5
    sh      t6, 0(t5)          # store left
    sh      t6, 2(t5)          # store right

    addiu   t4, t4, -1
    bgez    t4, .dup_loop
    nop

    # DMA out: DMEM stereo -> RDRAM dest
    sll     t2, t0, 2          # byte count = samples * 4 (stereo 16-bit)
    addiu   t2, t2, -1

    move    s0, a2             # RDRAM dst
    la      s4, %lo(audio_work)
    move    t1, zero
    jal     DMAOut
    nop

    jr      ra
    nop
