/*
 * 68k Dynamic Recompiler for MIPS III (VR4300/N64)
 *
 * Translates 68k basic blocks to native MIPS III instructions at runtime.
 * Falls back to Musashi interpreter for unsupported opcodes.
 *
 * Ported from PicoDrive64's FAME-based DRC.  Adapted for Musashi's
 * CPU state layout via drc68k_musashi.h.
 *
 * Original (C) 2026, licensed under MAME license.
 */

#ifndef DRC68K_H
#define DRC68K_H

#include <stdint.h>
#include <stddef.h>

/* Portable type aliases (PicoDrive uses pico_int.h for these) */
#ifndef u8
typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef uintptr_t uptr;
#endif

/* Block cache configuration */
#define DRC68K_CACHE_SIZE       (256 * 1024)  /* 256 KB code cache */
#define DRC68K_BLOCK_MAX        4096          /* max compiled blocks */
#define DRC68K_HASH_SIZE        8192          /* hash table entries (power of 2) */
#define DRC68K_HASH_MASK        (DRC68K_HASH_SIZE - 1)
#define DRC68K_MAX_BLOCK_INSNS  64            /* max 68k insns per block */

/* Block descriptor */
typedef struct {
	u32 addr_68k;       /* 68k start address of this block */
	void *code;         /* pointer to compiled MIPS code */
	u16 insn_count;     /* number of 68k instructions compiled */
	u16 code_size;      /* size of generated MIPS code in bytes */
	s32 cycles;         /* total cycle cost of this block */
} drc68k_block_t;

/* DRC state */
typedef struct {
	u8 *cache;              /* code cache memory */
	u8 *cache_ptr;          /* current write position in cache */
	u8 *cache_end;          /* end of cache */

	drc68k_block_t blocks[DRC68K_BLOCK_MAX];
	int block_count;

	/* Hash table: 68k address -> block index (or -1) */
	s16 hash[DRC68K_HASH_SIZE];

	/* Statistics */
	u32 blocks_compiled;
	u32 blocks_executed;
	u32 fallbacks;          /* opcodes that fell back to interpreter */
} drc68k_state_t;

extern drc68k_state_t drc68k;

/* API */
void drc68k_init(void);
void drc68k_reset(void);
void drc68k_cleanup(void);

/* Try to execute compiled code at addr. Returns cycles used, or -1 if no block. */
int drc68k_execute(u32 addr, int cycles_max);

/* Hash function for block lookup */
static inline int drc68k_hash(u32 addr)
{
	return (addr >> 1) & DRC68K_HASH_MASK;
}

#endif /* DRC68K_H */
