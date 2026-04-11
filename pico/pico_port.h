/* Minimal pico_port.h shim for FAME integration into mvs64.
 * Provides type definitions and endianness macros that FAME needs. */
#ifndef PICO_PORT_H
#define PICO_PORT_H

#include <stdint.h>
#include <string.h>

typedef uint8_t        u8;
typedef int8_t         s8;
typedef uint16_t       u16;
typedef int16_t        s16;
typedef uint32_t       u32;
typedef int32_t        s32;
typedef uint64_t       u64;
typedef int64_t        s64;
typedef uintptr_t      uptr;

/* N64 (MIPS R4300i) is big-endian */
#define CPU_IS_LE 0

/* Big-endian byte access within 16-bit and 32-bit values */
#define MEM_LE2(a)  ((a)^1)
#define MEM_LE4(a)  ((a)^3)

#endif /* PICO_PORT_H */
