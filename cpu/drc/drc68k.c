/*
 * 68k Dynamic Recompiler for MIPS III (VR4300/N64)
 *
 * Ported from PicoDrive64.  Adapted for Musashi interpreter (MVS64).
 * Phase 1: Minimal hybrid dynarec
 * - Compiles basic blocks for common opcodes
 * - Falls back to Musashi interpreter for unsupported ops
 * - Emits native MIPS III instructions directly
 *
 * Original (C) 2026, licensed under MAME license.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "drc68k.h"
#include "drc68k_musashi.h"
#include "cmn.h"

#ifdef N64
#include <libdragon.h>
#define host_instructions_updated(a, b, c) \
	data_cache_hit_writeback_invalidate(a, (u8*)(b) - (u8*)(a)); \
	inst_cache_hit_invalidate(a, (u8*)(b) - (u8*)(a))
#else
#define host_instructions_updated(a, b, c)
#endif

static u32 *tcache_ptr;
#define EMIT(x) *tcache_ptr++ = (x)

/* MIPS instruction encoding helpers */
#define MIPS_INSN(op, rs, rt, rd, sa, fn) \
	(((op)<<26)|((rs)<<21)|((rt)<<16)|((rd)<<11)|((sa)<<6)|((fn)<<0))

/* R-type: SPECIAL (op=0) */
#define MIPS_ADDU(rd,rs,rt) MIPS_INSN(0,rs,rt,rd,0,0x21)
#define MIPS_SUBU(rd,rs,rt) MIPS_INSN(0,rs,rt,rd,0,0x23)
#define MIPS_AND(rd,rs,rt)  MIPS_INSN(0,rs,rt,rd,0,0x24)
#define MIPS_OR(rd,rs,rt)   MIPS_INSN(0,rs,rt,rd,0,0x25)
#define MIPS_XOR(rd,rs,rt)  MIPS_INSN(0,rs,rt,rd,0,0x26)
#define MIPS_SLTU(rd,rs,rt) MIPS_INSN(0,rs,rt,rd,0,0x2b)
#define MIPS_JR(rs)         MIPS_INSN(0,rs,0,0,0,0x08)
#define MIPS_SLL(rd,rt,sa)  MIPS_INSN(0,0,rt,rd,sa,0x00)
#define MIPS_SRL(rd,rt,sa)  MIPS_INSN(0,0,rt,rd,sa,0x02)
#define MIPS_SRA(rd,rt,sa)  MIPS_INSN(0,0,rt,rd,sa,0x03)
#define MIPS_NOP            0

/* I-type */
#define MIPS_ADDIU(rt,rs,imm) MIPS_INSN(9,rs,rt,0,0,(u16)(imm))
#define MIPS_ANDI(rt,rs,imm)  MIPS_INSN(12,rs,rt,0,0,(u16)(imm))
#define MIPS_ORI(rt,rs,imm)   MIPS_INSN(13,rs,rt,0,0,(u16)(imm))
#define MIPS_LUI(rt,imm)      MIPS_INSN(15,0,rt,0,0,(u16)(imm))
#define MIPS_LW(rt,off,base)  MIPS_INSN(35,base,rt,0,0,(u16)(off))
#define MIPS_SW(rt,off,base)  MIPS_INSN(43,base,rt,0,0,(u16)(off))
#define MIPS_BEQ(rs,rt,off)   MIPS_INSN(4,rs,rt,0,0,(u16)(off))
#define MIPS_BNE(rs,rt,off)   MIPS_INSN(5,rs,rt,0,0,(u16)(off))
#define MIPS_BLEZ(rs,off)     MIPS_INSN(6,rs,0,0,0,(u16)(off))

/* Registers */
#define Z0 0
#define SP 29
#define LR 31

/* ======== Branch Offset Helper ======== */
/*
 * Safe forward branch pattern:
 *   u32 *patch = emit_branch_placeholder(BNE, rs, rt);
 *   ... emit instructions to skip ...
 *   patch_branch(patch);
 *
 * This eliminates hand-counted offsets entirely.
 * MIPS branch: target = (PC_of_branch + 4) + (offset * 4)
 * So: offset = (target_addr - (branch_addr + 4)) / 4
 */

/* Emit a branch with offset=0 (placeholder). Returns pointer to patch. */
static u32 *emit_branch_placeholder_beq(int rs, int rt)
{
	u32 *p = tcache_ptr;
	EMIT(MIPS_BEQ(rs, rt, 0));
	EMIT(MIPS_NOP); /* delay slot */
	return p;
}

static u32 *emit_branch_placeholder_bne(int rs, int rt)
{
	u32 *p = tcache_ptr;
	EMIT(MIPS_BNE(rs, rt, 0));
	EMIT(MIPS_NOP);
	return p;
}

/* Patch a branch placeholder to jump to current tcache_ptr position */
static void patch_branch(u32 *branch_insn)
{
	/* offset = (target - (branch + 4)) / 4 */
	int offset = (int)(tcache_ptr - branch_insn) - 1;
	/* Replace the low 16 bits (immediate field) with the offset */
	*branch_insn = (*branch_insn & 0xffff0000) | (offset & 0xffff);
}

/* ======== DRC State ======== */

drc68k_state_t drc68k;

/* ======== 68k Register Mapping ======== */

/*
 * 68k has 16 registers (D0-D7, A0-A7) + PC + SR/CCR.
 * MIPS has limited registers. Strategy:
 * - Keep 68k regs in a memory context block (m68ki_cpu_core)
 * - Load into MIPS temporaries for each compiled instruction
 * - Write back after modification
 * - Lazy flag computation: store result, derive NZVC when needed
 *
 * MIPS register usage during compiled blocks:
 *   s0 = pointer to m68ki_cpu_core
 *   s1 = pointer to 68k Fetch table (for PC->real address mapping)
 *   s2 = cycle counter (decremented per instruction)
 *   t0-t6 = temporaries for 68k operations
 *   AT = reserved by emitter
 */
#define REG_CTX     16  /* s0 - m68ki_cpu_core pointer */
#define REG_FETCH   17  /* s1 - Fetch table pointer */
#define REG_CYCLES  18  /* s2 - cycle counter */
#define REG_TMP0     8  /* t0 */
#define REG_TMP1     9  /* t1 */
#define REG_TMP2    10  /* t2 */
#define REG_TMP3    11  /* t3 */
#define REG_TMP4    12  /* t4 */

/* Musashi m68ki_cpu_core field offsets (via drc68k_musashi.h) */
#define CTX_OFF_DREG(n)  DRC_OFF_DREG(n)
#define CTX_OFF_AREG(n)  DRC_OFF_AREG(n)
#define CTX_OFF_PC       DRC_OFF_PC
#define CTX_OFF_FLAG_C   DRC_OFF_FLAG_C
#define CTX_OFF_FLAG_V   DRC_OFF_FLAG_V
#define CTX_OFF_FLAG_NZ  DRC_OFF_FLAG_NZ
#define CTX_OFF_FLAG_N   DRC_OFF_FLAG_N
#define CTX_OFF_FLAG_X   DRC_OFF_FLAG_X

/* ======== 68k Instruction Decoder ======== */

/* Read a 16-bit word from 68k address space (for block compilation).
 * Uses drc_fetch_68k_word() from drc68k_musashi.h which reads directly
 * from MVS64's P_ROM/BIOS/WORK_RAM pointers. */
#define fetch_68k_word(addr)  drc_fetch_68k_word(addr)

/* 68k addressing mode types */
#define EA_DREG     0   /* Dn */
#define EA_AREG     1   /* An */
#define EA_AREG_IND 2   /* (An) */
#define EA_AREG_INC 3   /* (An)+ */
#define EA_AREG_DEC 4   /* -(An) */
#define EA_AREG_DSP 5   /* d16(An) */
#define EA_ABS_W    7   /* xxxx.w (mode=7, reg=0) */
#define EA_ABS_L    8   /* xxxx.l (mode=7, reg=1) */
#define EA_IMM      9   /* #imm (mode=7, reg=4) */
#define EA_UNSUP   -1   /* unsupported */

/* Extract EA mode and register from 68k opcode */
#define OP_EA_MODE(op) (((op) >> 3) & 7)
#define OP_EA_REG(op)  ((op) & 7)
#define OP_SIZE(op)    (((op) >> 6) & 3) /* 0=byte, 1=word, 2=long */

/* ======== Code Emitter Helpers ======== */

/* Emit: load 68k Dn into MIPS reg */
static void emit_load_dreg(int mips_reg, int dreg_num)
{
	/* LW mips_reg, CTX_OFF_DREG(dreg_num)(REG_CTX) */
	EMIT(MIPS_LW(mips_reg, CTX_OFF_DREG(dreg_num), REG_CTX));
}

/* Emit: store MIPS reg to 68k Dn */
static void emit_store_dreg(int dreg_num, int mips_reg)
{
	EMIT(MIPS_SW(mips_reg, CTX_OFF_DREG(dreg_num), REG_CTX));
}

/* Emit: load 68k An into MIPS reg */
static void emit_load_areg(int mips_reg, int areg_num)
{
	EMIT(MIPS_LW(mips_reg, CTX_OFF_AREG(areg_num), REG_CTX));
}

/* Emit: store MIPS reg to 68k An */
static void emit_store_areg(int areg_num, int mips_reg)
{
	EMIT(MIPS_SW(mips_reg, CTX_OFF_AREG(areg_num), REG_CTX));
}

/* Emit: update N and Z flags from result in mips_reg (long size).
 * Musashi flag encoding:
 *   not_z_flag = result (any bit set = Z clear) — same as FAME
 *   n_flag     = result >> 24 (N = bit 7)       — FAME stores full result
 */
static void emit_update_nz_long(int mips_reg)
{
	EMIT(MIPS_SW(mips_reg, CTX_OFF_FLAG_NZ, REG_CTX));
	/* Musashi: n_flag = NFLAG_32(result) = result >> 24 */
	EMIT(MIPS_SRL(REG_TMP4, mips_reg, 24));
	EMIT(MIPS_SW(REG_TMP4, CTX_OFF_FLAG_N, REG_CTX));
}

/* Emit: clear V and C flags.
 * Musashi: v_flag=0 means no overflow, c_flag=0 means no carry. */
static void emit_clear_vc(void)
{
	EMIT(MIPS_SW(Z0, CTX_OFF_FLAG_V, REG_CTX));
	EMIT(MIPS_SW(Z0, CTX_OFF_FLAG_C, REG_CTX));
}

/* Emit: set carry flag from SLTU result (0 or 1).
 * Both Musashi and FAME store carry at bit 8 (CFLAG_SET = 0x100).
 * SLTU produces 0/1, shift left by 8 → 0/0x100.
 * Also set x_flag (extend) = same as carry for arithmetic ops. */
static void emit_set_carry_from_sltu(int sltu_reg)
{
	EMIT(MIPS_SLL(sltu_reg, sltu_reg, 8)); /* carry at bit 8 */
	EMIT(MIPS_SW(sltu_reg, CTX_OFF_FLAG_C, REG_CTX));
	EMIT(MIPS_SW(sltu_reg, CTX_OFF_FLAG_X, REG_CTX));
}

/* Forward declaration */
static void emit_load_imm32(int reg, u32 val);

/*
 * C-callable memory access wrappers for DRC-generated code.
 * Called via JALR from compiled blocks.  These call MVS64's full
 * memory access path (m68kinline.h fast paths + hw.c slow paths),
 * including the WORK_RAM / P-ROM inline checks.
 *
 * NOT static — their addresses are embedded in generated MIPS code.
 */
u32 drc_read16(u32 a)
{
	return m68k_read_memory_16(a & 0xFFFFFF);
}

u32 drc_read32(u32 a)
{
	return m68k_read_memory_32(a & 0xFFFFFF);
}

void drc_write16(u32 a, u32 d)
{
	m68k_write_memory_16(a & 0xFFFFFF, d);
}

void drc_write32(u32 a, u32 d)
{
	m68k_write_memory_32(a & 0xFFFFFF, d);
}

/* Emit: call drc_read16. a0 = address. Result in v0. */
static void emit_drc_read16(void)
{
	EMIT(MIPS_ADDIU(SP, SP, -8));
	EMIT(MIPS_SW(REG_CTX, 0, SP));
	EMIT(MIPS_SW(REG_CYCLES, 4, SP));
	emit_load_imm32(REG_TMP4, (u32)(uptr)drc_read16);
	EMIT(MIPS_INSN(0, REG_TMP4, 0, LR, 0, 0x09));
	EMIT(MIPS_NOP);
	EMIT(MIPS_LW(REG_CTX, 0, SP));
	EMIT(MIPS_LW(REG_CYCLES, 4, SP));
	EMIT(MIPS_ADDIU(SP, SP, 8));
}

static void emit_drc_read32(void)
{
	EMIT(MIPS_ADDIU(SP, SP, -8));
	EMIT(MIPS_SW(REG_CTX, 0, SP));
	EMIT(MIPS_SW(REG_CYCLES, 4, SP));
	emit_load_imm32(REG_TMP4, (u32)(uptr)drc_read32);
	EMIT(MIPS_INSN(0, REG_TMP4, 0, LR, 0, 0x09));
	EMIT(MIPS_NOP);
	EMIT(MIPS_LW(REG_CTX, 0, SP));
	EMIT(MIPS_LW(REG_CYCLES, 4, SP));
	EMIT(MIPS_ADDIU(SP, SP, 8));
}

static void emit_drc_write16(void)
{
	EMIT(MIPS_ADDIU(SP, SP, -8));
	EMIT(MIPS_SW(REG_CTX, 0, SP));
	EMIT(MIPS_SW(REG_CYCLES, 4, SP));
	emit_load_imm32(REG_TMP4, (u32)(uptr)drc_write16);
	EMIT(MIPS_INSN(0, REG_TMP4, 0, LR, 0, 0x09));
	EMIT(MIPS_NOP);
	EMIT(MIPS_LW(REG_CTX, 0, SP));
	EMIT(MIPS_LW(REG_CYCLES, 4, SP));
	EMIT(MIPS_ADDIU(SP, SP, 8));
}

static void emit_drc_write32(void)
{
	EMIT(MIPS_ADDIU(SP, SP, -8));
	EMIT(MIPS_SW(REG_CTX, 0, SP));
	EMIT(MIPS_SW(REG_CYCLES, 4, SP));
	emit_load_imm32(REG_TMP4, (u32)(uptr)drc_write32);
	EMIT(MIPS_INSN(0, REG_TMP4, 0, LR, 0, 0x09));
	EMIT(MIPS_NOP);
	EMIT(MIPS_LW(REG_CTX, 0, SP));
	EMIT(MIPS_LW(REG_CYCLES, 4, SP));
	EMIT(MIPS_ADDIU(SP, SP, 8));
}

/*
 * "Safe" memory access — just aliases for the drc_* emitters.
 * PicoDrive had separate safe/inline/fast paths; in MVS64 all paths
 * go through the same C wrappers (which already contain the inline
 * WORK_RAM/P-ROM fast-path from m68kinline.h).
 */
#define emit_safe_read16  emit_drc_read16
#define emit_safe_read32  emit_drc_read32
#define emit_safe_write16 emit_drc_write16
#define emit_safe_write32 emit_drc_write32

/*
 * Inline fast-path emitters — currently all route to drc_* C wrappers.
 * MVS64's m68k_read_memory / m68k_write_memory functions already contain the
 * WORK_RAM/P-ROM inline fast-paths from m68kinline.h, so no MAP_FLAG
 * logic is needed at the MIPS emission level.
 *
 * TODO: for Phase 2, emit inline bank-check MIPS (check bank 1 for
 * WORK_RAM direct access) to avoid the C function call overhead on
 * the hottest path.
 */
#define emit_inline_read16  emit_drc_read16
#define emit_inline_read32  emit_drc_read32
#define emit_inline_write16 emit_drc_write16
#define emit_inline_write32 emit_drc_write32

/* Emit: load immediate 32-bit value into register */
static void emit_load_imm32(int reg, u32 val)
{
	if (val == 0) {
		EMIT(MIPS_ADDU(reg, Z0, Z0));
	} else if ((s32)val >= -32768 && (s32)val < 32768) {
		EMIT(MIPS_ADDIU(reg, Z0, (s16)val));
	} else if ((val & 0xffff) == 0) {
		EMIT(MIPS_LUI(reg, val >> 16));
	} else {
		EMIT(MIPS_LUI(reg, val >> 16));
		EMIT(MIPS_ORI(reg, reg, val & 0xffff));
	}
}

/* Old PicoDrive MAP_FLAG inline functions removed — MVS64 uses C wrappers.
 * See emit_inline_read16 et al. macros above. */
#if 0
static void OLD_emit_inline_read16(void)
{
	/* a0 &= 0x00fffffe (mask + word-align) */
	EMIT(MIPS_LUI(REG_TMP4, 0x00ff));
	EMIT(MIPS_ORI(REG_TMP4, REG_TMP4, 0xfffe));
	EMIT(MIPS_AND(4, 4, REG_TMP4));

	/* REG_TMP3 = &m68k_read16_map */
	emit_load_imm32(REG_TMP3, (u32)(uptr)m68k_read16_map);

	/* REG_TMP4 = a0 >> 16 (bank index) */
	EMIT(MIPS_SRL(REG_TMP4, 4, 16));

	/* REG_TMP3 = m68k_read16_map[bank] (load map entry) */
	EMIT(MIPS_SLL(REG_TMP4, REG_TMP4, 2)); /* index * 4 (sizeof uptr) */
	EMIT(MIPS_ADDU(REG_TMP3, REG_TMP3, REG_TMP4));
	EMIT(MIPS_LW(REG_TMP3, 0, REG_TMP3)); /* v = map[bank] */

	/* Check MAP_FLAG (top bit) */
	EMIT(MIPS_SRL(REG_TMP4, REG_TMP3, 31)); /* TMP4 = top bit */
	EMIT(MIPS_BNE(REG_TMP4, Z0, 7)); /* if MAP_FLAG set, jump to slow path */
	EMIT(MIPS_NOP);

	/* Fast path: direct memory load */
	/* addr = (v << 1) + a0 */
	EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 1));
	EMIT(MIPS_ADDU(REG_TMP3, REG_TMP3, 4)); /* TMP3 = base + a0 */
	/* v0 = *(u16 *)(TMP3) */
	EMIT(MIPS_INSN(37, REG_TMP3, 2, 0, 0, 0)); /* LHU v0, 0(TMP3) */
	EMIT(MIPS_INSN(4, Z0, Z0, 0, 0, 9)); /* skip slow path (9 insns) */ /* BEQ z0, z0, +5 (skip slow path) */
	EMIT(MIPS_NOP);

	/* Slow path: call function pointer */
	/* func = (v << 1) */
	EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 1));
	EMIT(MIPS_ADDIU(SP, SP, -8));
	EMIT(MIPS_SW(REG_CTX, 0, SP));
	EMIT(MIPS_SW(REG_CYCLES, 4, SP));
	EMIT(MIPS_INSN(0, REG_TMP3, 0, LR, 0, 0x09)); /* JALR ra, TMP3 */
	EMIT(MIPS_NOP);
	EMIT(MIPS_LW(REG_CTX, 0, SP));
	EMIT(MIPS_LW(REG_CYCLES, 4, SP));
	EMIT(MIPS_ADDIU(SP, SP, 8));
	/* v0 = result from function */
}

/* Inline fast-path read32 */
static void emit_inline_read32(void)
{
	EMIT(MIPS_LUI(REG_TMP4, 0x00ff));
	EMIT(MIPS_ORI(REG_TMP4, REG_TMP4, 0xfffc));
	EMIT(MIPS_AND(4, 4, REG_TMP4));

	emit_load_imm32(REG_TMP3, (u32)(uptr)m68k_read16_map);
	EMIT(MIPS_SRL(REG_TMP4, 4, 16));
	EMIT(MIPS_SLL(REG_TMP4, REG_TMP4, 2));
	EMIT(MIPS_ADDU(REG_TMP3, REG_TMP3, REG_TMP4));
	EMIT(MIPS_LW(REG_TMP3, 0, REG_TMP3));

	EMIT(MIPS_SRL(REG_TMP4, REG_TMP3, 31));
	EMIT(MIPS_BNE(REG_TMP4, Z0, 7));
	EMIT(MIPS_NOP);

	/* Fast path: direct 32-bit load */
	EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 1));
	EMIT(MIPS_ADDU(REG_TMP3, REG_TMP3, 4));
	EMIT(MIPS_LW(2, 0, REG_TMP3)); /* LW v0, 0(TMP3) */
	EMIT(MIPS_INSN(4, Z0, Z0, 0, 0, 9)); /* skip slow path (9 insns) */
	EMIT(MIPS_NOP);

	/* Slow path */
	EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 1));
	EMIT(MIPS_ADDIU(SP, SP, -8));
	EMIT(MIPS_SW(REG_CTX, 0, SP));
	EMIT(MIPS_SW(REG_CYCLES, 4, SP));
	EMIT(MIPS_INSN(0, REG_TMP3, 0, LR, 0, 0x09));
	EMIT(MIPS_NOP);
	EMIT(MIPS_LW(REG_CTX, 0, SP));
	EMIT(MIPS_LW(REG_CYCLES, 4, SP));
	EMIT(MIPS_ADDIU(SP, SP, 8));
}

/* Inline fast-path write16. a0=addr, a1=data */
static void emit_inline_write16(void)
{
	EMIT(MIPS_LUI(REG_TMP4, 0x00ff));
	EMIT(MIPS_ORI(REG_TMP4, REG_TMP4, 0xfffe));
	EMIT(MIPS_AND(4, 4, REG_TMP4));

	emit_load_imm32(REG_TMP3, (u32)(uptr)m68k_write16_map);
	EMIT(MIPS_SRL(REG_TMP4, 4, 16));
	EMIT(MIPS_SLL(REG_TMP4, REG_TMP4, 2));
	EMIT(MIPS_ADDU(REG_TMP3, REG_TMP3, REG_TMP4));
	EMIT(MIPS_LW(REG_TMP3, 0, REG_TMP3));

	EMIT(MIPS_SRL(REG_TMP4, REG_TMP3, 31));
	EMIT(MIPS_BNE(REG_TMP4, Z0, 7));
	EMIT(MIPS_NOP);

	/* Fast path: direct store */
	EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 1));
	EMIT(MIPS_ADDU(REG_TMP3, REG_TMP3, 4));
	EMIT(MIPS_INSN(41, REG_TMP3, 5, 0, 0, 0)); /* SH a1, 0(TMP3) */
	EMIT(MIPS_INSN(4, Z0, Z0, 0, 0, 9)); /* skip slow path (9 insns) */
	EMIT(MIPS_NOP);

	/* Slow path */
	EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 1));
	EMIT(MIPS_ADDIU(SP, SP, -8));
	EMIT(MIPS_SW(REG_CTX, 0, SP));
	EMIT(MIPS_SW(REG_CYCLES, 4, SP));
	EMIT(MIPS_INSN(0, REG_TMP3, 0, LR, 0, 0x09));
	EMIT(MIPS_NOP);
	EMIT(MIPS_LW(REG_CTX, 0, SP));
	EMIT(MIPS_LW(REG_CYCLES, 4, SP));
	EMIT(MIPS_ADDIU(SP, SP, 8));
}

/* Inline fast-path write32. a0=addr, a1=data */
static void emit_inline_write32(void)
{
	EMIT(MIPS_LUI(REG_TMP4, 0x00ff));
	EMIT(MIPS_ORI(REG_TMP4, REG_TMP4, 0xfffc));
	EMIT(MIPS_AND(4, 4, REG_TMP4));

	emit_load_imm32(REG_TMP3, (u32)(uptr)m68k_write16_map);
	EMIT(MIPS_SRL(REG_TMP4, 4, 16));
	EMIT(MIPS_SLL(REG_TMP4, REG_TMP4, 2));
	EMIT(MIPS_ADDU(REG_TMP3, REG_TMP3, REG_TMP4));
	EMIT(MIPS_LW(REG_TMP3, 0, REG_TMP3));

	EMIT(MIPS_SRL(REG_TMP4, REG_TMP3, 31));
	EMIT(MIPS_BNE(REG_TMP4, Z0, 7));
	EMIT(MIPS_NOP);

	/* Fast path */
	EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 1));
	EMIT(MIPS_ADDU(REG_TMP3, REG_TMP3, 4));
	EMIT(MIPS_SW(5, 0, REG_TMP3)); /* SW a1, 0(TMP3) */
	EMIT(MIPS_INSN(4, Z0, Z0, 0, 0, 9)); /* skip slow path (9 insns) */
	EMIT(MIPS_NOP);

	/* Slow path */
	EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 1));
	EMIT(MIPS_ADDIU(SP, SP, -8));
	EMIT(MIPS_SW(REG_CTX, 0, SP));
	EMIT(MIPS_SW(REG_CYCLES, 4, SP));
	EMIT(MIPS_INSN(0, REG_TMP3, 0, LR, 0, 0x09));
	EMIT(MIPS_NOP);
	EMIT(MIPS_LW(REG_CTX, 0, SP));
	EMIT(MIPS_LW(REG_CYCLES, 4, SP));
	EMIT(MIPS_ADDIU(SP, SP, 8));
}

#endif /* old inline functions */

/* Emit: subtract cycles and check for exit */
static void emit_cycle_check(int cycles, u32 *exit_label)
{
	EMIT(MIPS_ADDIU(REG_CYCLES, REG_CYCLES, -cycles));
	/* BLEZ REG_CYCLES, exit_label (filled in later) */
	*exit_label = (u32)(tcache_ptr);
	EMIT(MIPS_BLEZ(REG_CYCLES, 0)); /* placeholder */
	EMIT(MIPS_NOP); /* branch delay slot */
}

/* ======== Block Compiler ======== */

/* Compile one 68k instruction. Returns:
 *  > 0: number of 68k bytes consumed
 *  -1:  unsupported opcode, end block
 */
static int compile_one_insn(u32 pc, int *cycles_out)
{
	u16 opcode = fetch_68k_word(pc);
	(void)0; /* unused locals silenced below */
	int size __attribute__((unused));
	int src_reg __attribute__((unused));
	int dst_reg __attribute__((unused));

	/* Decode the major opcode groups */
	switch ((opcode >> 12) & 0xf) {

	case 0x2: /* MOVE.L */
	case 0x3: /* MOVE.W */
	{
		int op_size = ((opcode >> 12) & 0xf) == 0x2 ? 2 : 1; /* 2=long, 1=word */
		int src_mode = OP_EA_MODE(opcode);
		int src_r = OP_EA_REG(opcode);
		int dst_mode = (opcode >> 6) & 7;
		int dst_r = (opcode >> 9) & 7;

		/* Load source value into REG_TMP0.
		 * After this block: REG_TMP0 = source value.
		 * Memory reads use emit_safe_read which clobbers a0, t-regs.
		 * Result comes back in v0 (reg 2), moved to REG_TMP0. */
		int extra_words = 0;

		if (src_mode == 0) {
			/* Dn - register direct */
			emit_load_dreg(REG_TMP0, src_r);
		} else if (src_mode == 1) {
			/* An - address register direct */
			emit_load_areg(REG_TMP0, src_r);
		} else if (src_mode == 7 && src_r == 4) {
			/* #immediate */
			if (op_size == 2) {
				u32 imm = (fetch_68k_word(pc+2) << 16) | fetch_68k_word(pc+4);
				emit_load_imm32(REG_TMP0, imm);
				extra_words = 4;
			} else {
				u16 imm = fetch_68k_word(pc+2);
				emit_load_imm32(REG_TMP0, (s16)imm);
				extra_words = 2;
			}
		} else if (src_mode == 2) {
			/* (An) - simplest memory read */
			emit_load_areg(4, src_r);
			if (op_size == 2) emit_drc_read32(); else emit_drc_read16();
			EMIT(MIPS_ADDU(REG_TMP0, 2, Z0));
		} else if (src_mode == 5) {
			/* d16(An) - displacement read */
			s16 disp = (s16)fetch_68k_word(pc + 2);
			extra_words = 2;
			emit_load_areg(4, src_r);
			EMIT(MIPS_ADDIU(4, 4, disp));
			if (op_size == 2) emit_drc_read32(); else emit_drc_read16();
			EMIT(MIPS_ADDU(REG_TMP0, 2, Z0));
		} else {
			return -1;
		}

		/* Store to destination.
		 * REG_TMP0 has the source value.
		 * For memory writes: save REG_TMP0 to stack before JALR
		 * since all t-regs are caller-saved and may be clobbered. */
		if (dst_mode == 0) {
			/* Dn */
			if (op_size == 2) {
				emit_store_dreg(dst_r, REG_TMP0);
			} else {
				/* preserve upper bits */
				emit_load_dreg(REG_TMP1, dst_r);
				EMIT(MIPS_ANDI(REG_TMP0, REG_TMP0, 0xffff));
				EMIT(MIPS_LUI(REG_TMP2, 0xffff));
				EMIT(MIPS_AND(REG_TMP1, REG_TMP1, REG_TMP2));
				EMIT(MIPS_OR(REG_TMP0, REG_TMP0, REG_TMP1));
				emit_store_dreg(dst_r, REG_TMP0);
			}
		} else if (dst_mode == 1) {
			/* An (MOVEA) - no flags affected */
			emit_store_areg(dst_r, REG_TMP0);
			*cycles_out = 4;
			return 2 + extra_words;
		} else if (dst_mode == 2) {
			/* (An) write */
			emit_load_areg(4, dst_r);
			EMIT(MIPS_ADDU(5, REG_TMP0, Z0));
			if (op_size == 2) emit_drc_write32(); else emit_drc_write16();
		} else if (dst_mode == 5) {
			/* d16(An) write */
			s16 disp = (s16)fetch_68k_word(pc + 2 + extra_words);
			extra_words += 2;
			emit_load_areg(4, dst_r);
			EMIT(MIPS_ADDIU(4, 4, disp));
			EMIT(MIPS_ADDU(5, REG_TMP0, Z0));
			if (op_size == 2) emit_drc_write32(); else emit_drc_write16();
		} else {
			return -1;
		}

		/* Set flags based on operation size */
		if (op_size == 2) {
			emit_update_nz_long(REG_TMP0);
		} else {
			/* Word size: mask to 16 bits for correct N/Z flags */
			EMIT(MIPS_ANDI(REG_TMP1, REG_TMP0, 0xffff));
			emit_update_nz_long(REG_TMP1);
		}
		emit_clear_vc();
		*cycles_out = (src_mode >= 2) ? 12 : 4;
		return 2 + extra_words;
	}

	case 0x1: /* MOVE.B */
	{
		int src_mode = OP_EA_MODE(opcode);
		int src_r = OP_EA_REG(opcode);
		int dst_mode = (opcode >> 6) & 7;
		int dst_r = (opcode >> 9) & 7;

		if (src_mode == 0 && dst_mode == 0) {
			/* MOVE.B Dn, Dn */
			emit_load_dreg(REG_TMP0, src_r);
			emit_load_dreg(REG_TMP1, dst_r);
			EMIT(MIPS_ANDI( REG_TMP0, REG_TMP0, 0xff));
			/* Mask out low byte of dest, OR in source */
			EMIT(MIPS_ANDI( REG_TMP2, REG_TMP1, 0xff00));
			/* Actually need upper 24 bits: use LUI + ORI trick */
			EMIT(MIPS_ADDIU(REG_TMP2, Z0, -256)); /* 0xFFFFFF00 */
			EMIT(MIPS_AND(REG_TMP1, REG_TMP1, REG_TMP2));
			EMIT(MIPS_OR(REG_TMP0, REG_TMP0, REG_TMP1));
			emit_store_dreg(dst_r, REG_TMP0);
			emit_update_nz_long(REG_TMP0);
			emit_clear_vc();
			*cycles_out = 4;
			return 2;
		}
		return -1;
	}

	case 0xd: /* ADD */
	{
		int reg = (opcode >> 9) & 7;
		int op_mode = (opcode >> 6) & 7;
		int ea_mode = OP_EA_MODE(opcode);
		int ea_reg = OP_EA_REG(opcode);

		/* ADD.L Dn, Dn (op_mode=2, ea_mode=0) */
		if (op_mode == 2 && ea_mode == 0) {
			emit_load_dreg(REG_TMP0, ea_reg);  /* source */
			emit_load_dreg(REG_TMP1, reg);     /* dest */
			EMIT(MIPS_ADDU(REG_TMP2, REG_TMP0, REG_TMP1));
			emit_store_dreg(reg, REG_TMP2);
			emit_update_nz_long(REG_TMP2);
			/* C = carry, X = carry (simplified: use SLTU) */
			EMIT(MIPS_SLTU(REG_TMP3, REG_TMP2, REG_TMP0));
			EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 8)); /* carry at bit 8 (CFLAG_SET) */
			EMIT(MIPS_SW(REG_TMP3, CTX_OFF_FLAG_C, REG_CTX));
			EMIT(MIPS_SW(REG_TMP3, CTX_OFF_FLAG_X, REG_CTX));
			/* V = overflow (simplified) */
			EMIT(MIPS_SW(Z0, CTX_OFF_FLAG_V, REG_CTX));
			*cycles_out = 8;
			return 2;
		}
		return -1;
	}

	case 0x9: /* SUB */
	{
		int reg = (opcode >> 9) & 7;
		int op_mode = (opcode >> 6) & 7;
		int ea_mode = OP_EA_MODE(opcode);
		int ea_reg = OP_EA_REG(opcode);

		/* SUB.L Dn, Dn */
		if (op_mode == 2 && ea_mode == 0) {
			emit_load_dreg(REG_TMP0, ea_reg);
			emit_load_dreg(REG_TMP1, reg);
			EMIT(MIPS_SUBU(REG_TMP2, REG_TMP1, REG_TMP0));
			emit_store_dreg(reg, REG_TMP2);
			emit_update_nz_long(REG_TMP2);
			/* C = borrow */
			EMIT(MIPS_SLTU(REG_TMP3, REG_TMP1, REG_TMP0));
			EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 8)); /* carry at bit 8 (CFLAG_SET) */
			EMIT(MIPS_SW(REG_TMP3, CTX_OFF_FLAG_C, REG_CTX));
			EMIT(MIPS_SW(REG_TMP3, CTX_OFF_FLAG_X, REG_CTX));
			EMIT(MIPS_SW(Z0, CTX_OFF_FLAG_V, REG_CTX));
			*cycles_out = 8;
			return 2;
		}
		return -1;
	}

	case 0xb: /* CMP / EOR */
	{
		int reg = (opcode >> 9) & 7;
		int op_mode = (opcode >> 6) & 7;
		int ea_mode = OP_EA_MODE(opcode);
		int ea_reg = OP_EA_REG(opcode);

		/* CMP.L Dn, Dn (op_mode=2, ea_mode=0) */
		if (op_mode == 2 && ea_mode == 0) {
			emit_load_dreg(REG_TMP0, ea_reg);
			emit_load_dreg(REG_TMP1, reg);
			EMIT(MIPS_SUBU(REG_TMP2, REG_TMP1, REG_TMP0));
			/* Don't store result - just set flags */
			emit_update_nz_long(REG_TMP2);
			EMIT(MIPS_SLTU(REG_TMP3, REG_TMP1, REG_TMP0));
			EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 8)); /* carry at bit 8 (CFLAG_SET) */
			EMIT(MIPS_SW(REG_TMP3, CTX_OFF_FLAG_C, REG_CTX));
			EMIT(MIPS_SW(Z0, CTX_OFF_FLAG_V, REG_CTX));
			*cycles_out = 6;
			return 2;
		}
		return -1;
	}

	case 0x7: /* MOVEQ */
	{
		int reg = (opcode >> 9) & 7;
		s8 imm = (s8)(opcode & 0xff);
		/* MOVEQ #imm8, Dn - sign extended to 32 bits */
		EMIT(MIPS_ADDIU(REG_TMP0, Z0, (s16)imm));
		emit_store_dreg(reg, REG_TMP0);
		emit_update_nz_long(REG_TMP0);
		emit_clear_vc();
		*cycles_out = 4;
		return 2;
	}

	case 0x4: /* Miscellaneous */
	{
		if ((opcode & 0xff00) == 0x4200) {
			/* CLR */
			int size = OP_SIZE(opcode);
			int ea_mode = OP_EA_MODE(opcode);
			int ea_reg = OP_EA_REG(opcode);

			if (ea_mode == 0 && size == 2) {
				/* CLR.L Dn */
				emit_store_dreg(ea_reg, Z0);
				emit_update_nz_long(Z0);
				emit_clear_vc();
				*cycles_out = 6;
				return 2;
			}
		}

		if ((opcode & 0xfff0) == 0x4e70) {
			if (opcode == 0x4e71) {
				/* NOP */
				*cycles_out = 4;
				return 2;
			}
			if (opcode == 0x4e75) {
				/* RTS - end block */
				return -1;
			}
		}

		if ((opcode & 0xffc0) == 0x4a00) {
			/* TST */
			int size = OP_SIZE(opcode);
			int ea_mode = OP_EA_MODE(opcode);
			int ea_reg = OP_EA_REG(opcode);

			if (ea_mode == 0 && size == 2) {
				/* TST.L Dn */
				emit_load_dreg(REG_TMP0, ea_reg);
				emit_update_nz_long(REG_TMP0);
				emit_clear_vc();
				*cycles_out = 4;
				return 2;
			}
		}

		/* LEA ea, An */
		if ((opcode & 0xf1c0) == 0x41c0) {
			int an = (opcode >> 9) & 7;
			int ea_mode = OP_EA_MODE(opcode);
			int ea_reg = OP_EA_REG(opcode);

			if (ea_mode == 2) {
				/* LEA (An), An - just copy address reg */
				emit_load_areg(REG_TMP0, ea_reg);
				emit_store_areg(an, REG_TMP0);
				*cycles_out = 4;
				return 2;
			}
			if (ea_mode == 5) {
				/* LEA d16(An), An */
				s16 disp = (s16)fetch_68k_word(pc + 2);
				emit_load_areg(REG_TMP0, ea_reg);
				EMIT(MIPS_ADDIU(REG_TMP0, REG_TMP0, disp));
				emit_store_areg(an, REG_TMP0);
				*cycles_out = 8;
				return 4;
			}
			if (ea_mode == 7 && ea_reg == 2) {
				/* LEA d16(PC), An */
				s16 disp = (s16)fetch_68k_word(pc + 2);
				emit_load_imm32(REG_TMP0, (pc + 2 + disp) & 0xffffff);
				emit_store_areg(an, REG_TMP0);
				*cycles_out = 8;
				return 4;
			}
		}

		return -1;
	}

	case 0xc: /* AND / MUL */
	{
		int reg = (opcode >> 9) & 7;
		int op_mode = (opcode >> 6) & 7;
		int ea_mode = OP_EA_MODE(opcode);
		int ea_reg = OP_EA_REG(opcode);

		/* AND.L Dn, Dn */
		if (op_mode == 2 && ea_mode == 0) {
			emit_load_dreg(REG_TMP0, ea_reg);
			emit_load_dreg(REG_TMP1, reg);
			EMIT(MIPS_AND(REG_TMP2, REG_TMP0, REG_TMP1));
			emit_store_dreg(reg, REG_TMP2);
			emit_update_nz_long(REG_TMP2);
			emit_clear_vc();
			*cycles_out = 8;
			return 2;
		}
		return -1;
	}

	case 0x8: /* OR / DIV */
	{
		int reg = (opcode >> 9) & 7;
		int op_mode = (opcode >> 6) & 7;
		int ea_mode = OP_EA_MODE(opcode);
		int ea_reg = OP_EA_REG(opcode);

		/* OR.L Dn, Dn */
		if (op_mode == 2 && ea_mode == 0) {
			emit_load_dreg(REG_TMP0, ea_reg);
			emit_load_dreg(REG_TMP1, reg);
			EMIT(MIPS_OR(REG_TMP2, REG_TMP0, REG_TMP1));
			emit_store_dreg(reg, REG_TMP2);
			emit_update_nz_long(REG_TMP2);
			emit_clear_vc();
			*cycles_out = 8;
			return 2;
		}
		return -1;
	}

	case 0x5: /* ADDQ / SUBQ / Scc / DBcc */
	{
		int data = (opcode >> 9) & 7;
		if (data == 0) data = 8; /* 0 encodes 8 */
		int sz = OP_SIZE(opcode);
		int ea_mode = OP_EA_MODE(opcode);
		int ea_reg = OP_EA_REG(opcode);

		if ((opcode & 0x0100) == 0 && sz == 2 && ea_mode == 0) {
			/* ADDQ.L #data, Dn */
			emit_load_dreg(REG_TMP0, ea_reg);
			EMIT(MIPS_ADDIU(REG_TMP1, REG_TMP0, data));
			emit_store_dreg(ea_reg, REG_TMP1);
			emit_update_nz_long(REG_TMP1);
			EMIT(MIPS_SLTU(REG_TMP2, REG_TMP1, REG_TMP0));
			EMIT(MIPS_SLL(REG_TMP2, REG_TMP2, 8)); /* carry at bit 8 (CFLAG_SET) */
			EMIT(MIPS_SW(REG_TMP2, CTX_OFF_FLAG_C, REG_CTX));
			EMIT(MIPS_SW(REG_TMP2, CTX_OFF_FLAG_X, REG_CTX));
			EMIT(MIPS_SW(Z0, CTX_OFF_FLAG_V, REG_CTX));
			*cycles_out = 4;
			return 2;
		}
		if ((opcode & 0x0100) && sz == 2 && ea_mode == 0) {
			/* SUBQ.L #data, Dn */
			emit_load_dreg(REG_TMP0, ea_reg);
			EMIT(MIPS_ADDIU(REG_TMP1, REG_TMP0, -data));
			emit_store_dreg(ea_reg, REG_TMP1);
			emit_update_nz_long(REG_TMP1);
			emit_load_imm32(REG_TMP2, data);
			EMIT(MIPS_SLTU(REG_TMP2, REG_TMP0, REG_TMP2));
			EMIT(MIPS_SLL(REG_TMP2, REG_TMP2, 8)); /* carry at bit 8 (CFLAG_SET) */
			EMIT(MIPS_SW(REG_TMP2, CTX_OFF_FLAG_C, REG_CTX));
			EMIT(MIPS_SW(REG_TMP2, CTX_OFF_FLAG_X, REG_CTX));
			EMIT(MIPS_SW(Z0, CTX_OFF_FLAG_V, REG_CTX));
			*cycles_out = 4;
			return 2;
		}
		/* ADDQ/SUBQ to An (no flags) */
		if ((opcode & 0x0100) == 0 && sz == 2 && ea_mode == 1) {
			emit_load_areg(REG_TMP0, ea_reg);
			EMIT(MIPS_ADDIU(REG_TMP0, REG_TMP0, data));
			emit_store_areg(ea_reg, REG_TMP0);
			*cycles_out = 4;
			return 2;
		}
		if ((opcode & 0x0100) && sz == 2 && ea_mode == 1) {
			emit_load_areg(REG_TMP0, ea_reg);
			EMIT(MIPS_ADDIU(REG_TMP0, REG_TMP0, -data));
			emit_store_areg(ea_reg, REG_TMP0);
			*cycles_out = 4;
			return 2;
		}
		return -1;
	}

	case 0x6: /* Bcc / BRA / BSR */
	{
		int cond = (opcode >> 8) & 0xf;
		s32 disp;
		int insn_sz;

		if ((opcode & 0xff) == 0) {
			disp = (s16)fetch_68k_word(pc + 2);
			insn_sz = 4;
		} else if ((opcode & 0xff) == 0xff) {
			disp = (s32)((fetch_68k_word(pc+2) << 16) | fetch_68k_word(pc+4));
			insn_sz = 6;
		} else {
			disp = (s8)(opcode & 0xff);
			insn_sz = 2;
		}

		u32 target = pc + 2 + disp;

		if (cond == 0) {
			/* BRA - unconditional */
			/* End block, set PC to target */
			emit_load_imm32(REG_TMP0, target & 0xffffff);
			EMIT(MIPS_SW(REG_TMP0, CTX_OFF_PC, REG_CTX));
			*cycles_out = 10;
			return insn_sz | 0x8000; /* flag: block-ending */
		}
		if (cond != 0) {
			/* All Bcc disabled - flag format incompatibility with FAME */
			return -1;
		}
		/* BRA + BEQ/BNE + BCC/BCS enabled.
		 * BPL/BMI disabled: flag_N stores raw result, and N bit position
		 * depends on operation size (bit 7/15/31) which we don't track.
		 * BGE/BLT/BGT/BLE disabled: need V flag. */
		if (cond != 0 && cond != 4 && cond != 5 && cond != 6 && cond != 7) {
			return -1;
		}

		/*
		 * Bcc - conditional branch.
		 * FAME flag format (how DRC and FAME both store them):
		 *   flag_NotZ: raw result. Z set if flag_NotZ == 0
		 *   flag_N: raw result. N set if bit 31 set (for .L ops)
		 *   flag_C: SLTU result 0/1 from DRC, or bit 8 from FAME
		 *           We check non-zero for either representation
		 *   flag_V: similar mixed representation
		 *
		 * Since DRC and FAME may store C differently (bit 0 vs bit 8),
		 * we check "!= 0" which works for both representations.
		 */
		EMIT(MIPS_LW(REG_TMP0, CTX_OFF_FLAG_NZ, REG_CTX));  /* for Z */
		EMIT(MIPS_LW(REG_TMP1, CTX_OFF_FLAG_N, REG_CTX));   /* for N */
		EMIT(MIPS_LW(REG_TMP2, CTX_OFF_FLAG_C, REG_CTX));   /* for C */

		u32 taken_pc = target & 0xffffff;
		u32 not_taken_pc = (pc + insn_sz) & 0xffffff;

		/* Default: not-taken PC */
		emit_load_imm32(REG_TMP3, not_taken_pc);

		/* Emit: if condition true, overwrite REG_TMP3 with taken_pc.
		 * Branch offset 3 skips the 2-insn load_imm32 + nop. For
		 * load_imm32 that needs LUI+ORI (2 insns), we skip 2 insns.
		 * Use worst case skip of 4 to be safe with any imm32 size. */
		/* Always use LUI+ORI for taken_pc to ensure fixed 2-insn size */
		switch (cond) {
		case 4: /* BCC (carry clear) - branch if C==0 */
			EMIT(MIPS_BNE(REG_TMP2, Z0, 3));
			EMIT(MIPS_NOP);
			EMIT(MIPS_LUI(REG_TMP3, (taken_pc >> 16) & 0xffff));
			EMIT(MIPS_ORI(REG_TMP3, REG_TMP3, taken_pc & 0xffff));
			break;
		case 5: /* BCS (carry set) - branch if C!=0 */
			EMIT(MIPS_BEQ(REG_TMP2, Z0, 3));
			EMIT(MIPS_NOP);
			EMIT(MIPS_LUI(REG_TMP3, (taken_pc >> 16) & 0xffff));
			EMIT(MIPS_ORI(REG_TMP3, REG_TMP3, taken_pc & 0xffff));
			break;
		case 6: /* BNE (not equal) - branch if Z clear (NotZ != 0) */
			EMIT(MIPS_BEQ(REG_TMP0, Z0, 3));
			EMIT(MIPS_NOP);
			EMIT(MIPS_LUI(REG_TMP3, (taken_pc >> 16) & 0xffff));
			EMIT(MIPS_ORI(REG_TMP3, REG_TMP3, taken_pc & 0xffff));
			break;
		case 7: /* BEQ (equal) - branch if Z set (NotZ == 0) */
			EMIT(MIPS_BNE(REG_TMP0, Z0, 3));
			EMIT(MIPS_NOP);
			EMIT(MIPS_LUI(REG_TMP3, (taken_pc >> 16) & 0xffff));
			EMIT(MIPS_ORI(REG_TMP3, REG_TMP3, taken_pc & 0xffff));
			break;
		case 10: /* BPL (plus) - branch if N clear (bit 31 == 0) */
			EMIT(MIPS_SRL(REG_TMP1, REG_TMP1, 31));
			EMIT(MIPS_BNE(REG_TMP1, Z0, 3));
			EMIT(MIPS_NOP);
			EMIT(MIPS_LUI(REG_TMP3, (taken_pc >> 16) & 0xffff));
			EMIT(MIPS_ORI(REG_TMP3, REG_TMP3, taken_pc & 0xffff));
			break;
		case 11: /* BMI (minus) - branch if N set (bit 31 == 1) */
			EMIT(MIPS_SRL(REG_TMP1, REG_TMP1, 31));
			EMIT(MIPS_BEQ(REG_TMP1, Z0, 3));
			EMIT(MIPS_NOP);
			EMIT(MIPS_LUI(REG_TMP3, (taken_pc >> 16) & 0xffff));
			EMIT(MIPS_ORI(REG_TMP3, REG_TMP3, taken_pc & 0xffff));
			break;
		case 12: /* BGE (greater or equal) - branch if N==V */
		case 13: /* BLT (less than) - branch if N!=V */
		case 14: /* BGT (greater than) */
		case 15: /* BLE (less or equal) */
		default:
			return -1;
		}

		EMIT(MIPS_SW(REG_TMP3, CTX_OFF_PC, REG_CTX));
		*cycles_out = 10;
		return insn_sz | 0x8000; /* flag: block-ending */
	}

	case 0x0: /* ORI/ANDI/SUBI/ADDI/CMPI to Dn */
	{
		int sub_op = (opcode >> 9) & 7;
		int sz = OP_SIZE(opcode);
		int ea_mode = OP_EA_MODE(opcode);
		int ea_reg = OP_EA_REG(opcode);

		if (ea_mode != 0 || sz != 2)
			return -1; /* only Dn, long for now */

		u32 imm;
		imm = (fetch_68k_word(pc+2) << 16) | fetch_68k_word(pc+4);

		switch (sub_op) {
		case 0: /* ORI.L #imm, Dn */
			emit_load_dreg(REG_TMP0, ea_reg);
			emit_load_imm32(REG_TMP1, imm);
			EMIT(MIPS_OR(REG_TMP0, REG_TMP0, REG_TMP1));
			emit_store_dreg(ea_reg, REG_TMP0);
			emit_update_nz_long(REG_TMP0);
			emit_clear_vc();
			*cycles_out = 16;
			return 6;
		case 1: /* ANDI.L #imm, Dn */
			emit_load_dreg(REG_TMP0, ea_reg);
			emit_load_imm32(REG_TMP1, imm);
			EMIT(MIPS_AND(REG_TMP0, REG_TMP0, REG_TMP1));
			emit_store_dreg(ea_reg, REG_TMP0);
			emit_update_nz_long(REG_TMP0);
			emit_clear_vc();
			*cycles_out = 16;
			return 6;
		case 2: /* SUBI.L #imm, Dn */
			emit_load_dreg(REG_TMP0, ea_reg);
			emit_load_imm32(REG_TMP1, imm);
			EMIT(MIPS_SUBU(REG_TMP2, REG_TMP0, REG_TMP1));
			emit_store_dreg(ea_reg, REG_TMP2);
			emit_update_nz_long(REG_TMP2);
			EMIT(MIPS_SLTU(REG_TMP3, REG_TMP0, REG_TMP1));
			EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 8)); /* carry at bit 8 (CFLAG_SET) */
			EMIT(MIPS_SW(REG_TMP3, CTX_OFF_FLAG_C, REG_CTX));
			EMIT(MIPS_SW(REG_TMP3, CTX_OFF_FLAG_X, REG_CTX));
			EMIT(MIPS_SW(Z0, CTX_OFF_FLAG_V, REG_CTX));
			*cycles_out = 16;
			return 6;
		case 3: /* ADDI.L #imm, Dn */
			emit_load_dreg(REG_TMP0, ea_reg);
			emit_load_imm32(REG_TMP1, imm);
			EMIT(MIPS_ADDU(REG_TMP2, REG_TMP0, REG_TMP1));
			emit_store_dreg(ea_reg, REG_TMP2);
			emit_update_nz_long(REG_TMP2);
			EMIT(MIPS_SLTU(REG_TMP3, REG_TMP2, REG_TMP0));
			EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 8)); /* carry at bit 8 (CFLAG_SET) */
			EMIT(MIPS_SW(REG_TMP3, CTX_OFF_FLAG_C, REG_CTX));
			EMIT(MIPS_SW(REG_TMP3, CTX_OFF_FLAG_X, REG_CTX));
			EMIT(MIPS_SW(Z0, CTX_OFF_FLAG_V, REG_CTX));
			*cycles_out = 16;
			return 6;
		case 6: /* CMPI.L #imm, Dn */
			emit_load_dreg(REG_TMP0, ea_reg);
			emit_load_imm32(REG_TMP1, imm);
			EMIT(MIPS_SUBU(REG_TMP2, REG_TMP0, REG_TMP1));
			emit_update_nz_long(REG_TMP2);
			EMIT(MIPS_SLTU(REG_TMP3, REG_TMP0, REG_TMP1));
			EMIT(MIPS_SLL(REG_TMP3, REG_TMP3, 8)); /* carry at bit 8 (CFLAG_SET) */
			EMIT(MIPS_SW(REG_TMP3, CTX_OFF_FLAG_C, REG_CTX));
			EMIT(MIPS_SW(Z0, CTX_OFF_FLAG_V, REG_CTX));
			*cycles_out = 14;
			return 6;
		case 5: /* EORI.L #imm, Dn */
			emit_load_dreg(REG_TMP0, ea_reg);
			emit_load_imm32(REG_TMP1, imm);
			EMIT(MIPS_XOR(REG_TMP0, REG_TMP0, REG_TMP1));
			emit_store_dreg(ea_reg, REG_TMP0);
			emit_update_nz_long(REG_TMP0);
			emit_clear_vc();
			*cycles_out = 16;
			return 6;
		}
		return -1;
	}

	case 0xe: /* Shift/Rotate */
	{
		int count_or_reg = (opcode >> 9) & 7;
		int dr = (opcode >> 8) & 1;  /* 0=right, 1=left */
		int sz = OP_SIZE(opcode);
		int ir = (opcode >> 5) & 1;  /* 0=count, 1=register */
		int type = (opcode >> 3) & 3;
		int reg = opcode & 7;

		/* Only immediate count, long size, register operand for now */
		if (sz != 2 || ir != 0)
			return -1;

		int count = count_or_reg;
		if (count == 0) count = 8;

		emit_load_dreg(REG_TMP0, reg);

		if (type == 0) { /* ASR/ASL */
			if (dr) /* left */
				EMIT(MIPS_SLL(REG_TMP1, REG_TMP0, count));
			else    /* right */
				EMIT(MIPS_SRA(REG_TMP1, REG_TMP0, count));
		} else if (type == 1) { /* LSR/LSL */
			if (dr)
				EMIT(MIPS_SLL(REG_TMP1, REG_TMP0, count));
			else
				EMIT(MIPS_SRL(REG_TMP1, REG_TMP0, count));
		} else {
			return -1;
		}

		emit_store_dreg(reg, REG_TMP1);
		emit_update_nz_long(REG_TMP1);
		/* Simplified: C = last bit shifted out, V = 0 */
		if (dr)
			EMIT(MIPS_SRL(REG_TMP2, REG_TMP0, 32 - count));
		else
			EMIT(MIPS_SRL(REG_TMP2, REG_TMP0, count - 1));
		EMIT(MIPS_ANDI(REG_TMP2, REG_TMP2, 1));
			EMIT(MIPS_SLL(REG_TMP2, REG_TMP2, 8)); /* carry at bit 8 (CFLAG_SET) */
		EMIT(MIPS_SW(REG_TMP2, CTX_OFF_FLAG_C, REG_CTX));
		EMIT(MIPS_SW(REG_TMP2, CTX_OFF_FLAG_X, REG_CTX));
		EMIT(MIPS_SW(Z0, CTX_OFF_FLAG_V, REG_CTX));
		*cycles_out = 6 + 2 * count;
		return 2;
	}

	default:
		return -1;
	}
}

/* Compile a basic block starting at addr_68k.
 * Returns the block, or NULL if compilation fails. */
static drc68k_block_t *compile_block(u32 addr_68k)
{
	drc68k_block_t *block;
	u8 *code_start;
	u32 pc;
	int total_cycles = 0;
	int insn_count = 0;
	int ended_with_branch = 0;

	if (drc68k.block_count >= DRC68K_BLOCK_MAX)
		return NULL;
	if ((u8 *)tcache_ptr + 1024 > drc68k.cache_end)
		return NULL;

	block = &drc68k.blocks[drc68k.block_count];
	block->addr_68k = addr_68k;
	block->code = tcache_ptr;
	code_start = (u8 *)tcache_ptr;

	/* Prologue: save s-regs, load context pointer */
	/* Push s0-s2 onto stack */
	EMIT(MIPS_ADDIU(SP, SP, -16));
	EMIT(MIPS_SW(16, 0, SP));   /* save s0 */
	EMIT(MIPS_SW(17, 4, SP));   /* save s1 */
	EMIT(MIPS_SW(18, 8, SP));   /* save s2 */
	EMIT(MIPS_SW(LR, 12, SP));  /* save ra */

	/* a0 = m68ki_cpu_core*, a1 = cycles */
	EMIT(MIPS_ADDU(REG_CTX, 4, Z0));    /* s0 = a0 (ctx) */
	EMIT(MIPS_ADDU(REG_CYCLES, 5, Z0));  /* s2 = a1 (cycles) */

	/* Compile instructions */
	pc = addr_68k;
	while (insn_count < DRC68K_MAX_BLOCK_INSNS) {
		int insn_cycles = 0;
		int consumed = compile_one_insn(pc, &insn_cycles);

		if (consumed < 0)
			break; /* unsupported opcode */

		int block_end = (consumed & 0x8000) != 0;
		consumed &= 0x7fff;

		pc += consumed;
		total_cycles += insn_cycles;
		insn_count++;

		if (block_end) {
			ended_with_branch = 1;
			break;
		}
	}

	if (insn_count < 2) {
		/* Block too short - prologue/epilogue overhead exceeds benefit. */
		tcache_ptr = (u32 *)code_start;
		return NULL;
	}

	/* Epilogue: write back updated PC (unless branch already did it) */
	if (!ended_with_branch) {
		emit_load_imm32(REG_TMP0, pc & 0xffffff);
		EMIT(MIPS_SW(REG_TMP0, CTX_OFF_PC, REG_CTX));
	}

	/* Return value = total cycles consumed */
	EMIT(MIPS_ADDIU(2, Z0, total_cycles));  /* v0 = total_cycles */

	/* Restore saved regs */
	EMIT(MIPS_LW(16, 0, SP));
	EMIT(MIPS_LW(17, 4, SP));
	EMIT(MIPS_LW(18, 8, SP));
	EMIT(MIPS_LW(LR, 12, SP));
	EMIT(MIPS_ADDIU(SP, SP, 16));

	/* Return */
	EMIT(MIPS_JR(LR));
	EMIT(MIPS_NOP); /* branch delay slot */

	/* Flush caches so the CPU can execute the generated code */
	host_instructions_updated(code_start, (u8 *)tcache_ptr, 0);

	block->insn_count = insn_count;
	block->code_size = (u8 *)tcache_ptr - code_start;
	block->cycles = total_cycles;

	/* Add to hash table */
	int h = drc68k_hash(addr_68k);
	drc68k.hash[h] = drc68k.block_count;

	drc68k.block_count++;
	drc68k.blocks_compiled++;

	return block;
}

/* ======== Public API ======== */

void drc68k_init(void)
{
	memset(&drc68k, 0, sizeof(drc68k));
	memset(drc68k.hash, -1, sizeof(drc68k.hash));

	drc68k.cache = (u8 *)malloc(DRC68K_CACHE_SIZE);
	if (!drc68k.cache) {
		debugf("[DRC] FATAL: cannot allocate %d KB code cache\n",
			DRC68K_CACHE_SIZE / 1024);
		return;
	}

	drc68k.cache_ptr = drc68k.cache;
	drc68k.cache_end = drc68k.cache + DRC68K_CACHE_SIZE;
	tcache_ptr = (u32 *)drc68k.cache;

	debugf("[DRC] initialized, %d KB code cache at %p\n",
		DRC68K_CACHE_SIZE / 1024, drc68k.cache);
}

/* Negative cache (defined in drc68k_execute, declared here for reset) */
#define NEG_CACHE_SIZE 4096
#define NEG_CACHE_MASK (NEG_CACHE_SIZE - 1)
static u32 neg_cache[NEG_CACHE_SIZE];

void drc68k_reset(void)
{
	memset(neg_cache, 0, sizeof(neg_cache));
	drc68k.block_count = 0;
	drc68k.cache_ptr = drc68k.cache;
	tcache_ptr = (u32 *)drc68k.cache;
	memset(drc68k.hash, -1, sizeof(drc68k.hash));
}

void drc68k_cleanup(void)
{
	if (drc68k.cache) {
		free(drc68k.cache);
		drc68k.cache = NULL;
	}
}

int drc68k_execute(u32 addr, int cycles_max)
{
	int h = drc68k_hash(addr);
	drc68k_block_t *block = NULL;
	int cycles_used;

	/* Check negative cache first — instant reject for known-uncompilable */
	int nh = (addr >> 1) & NEG_CACHE_MASK;
	if (neg_cache[nh] == addr)
		return -1;

	/* Look up in block hash table */
	s16 idx = drc68k.hash[h];
	if (idx >= 0 && drc68k.blocks[idx].addr_68k == addr) {
		block = &drc68k.blocks[idx];
	}

	/* Try to compile if not found */
	if (!block) {
		tcache_ptr = (u32 *)drc68k.cache_ptr;
		block = compile_block(addr);
		if (block)
			drc68k.cache_ptr = (u8 *)tcache_ptr;
	}

	if (!block) {
		/* Remember this address can't be compiled */
		neg_cache[nh] = addr;
		drc68k.fallbacks++;
		return -1;
	}

	/* Execute the compiled block.
	 * Block ABI: a0 = pointer to m68ki_cpu_core, a1 = cycles budget.
	 * Returns: v0 = cycles consumed by the block. */
	typedef int (*block_func)(m68ki_cpu_core *cpu, int cycles);
	block_func fn = (block_func)block->code;
	cycles_used = fn(&m68ki_cpu, cycles_max);

	drc68k.blocks_executed++;
	return cycles_used;
}
