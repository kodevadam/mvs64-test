/* Minimal pico_port.h shim for CZ80 and FAME integration into MVS64.
 * Provides type definitions and macros that PicoDrive CPU cores need. */
#ifndef PICO_PORT_H
#define PICO_PORT_H

#include <stdlib.h>
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

/* GCC attributes used by CZ80 */
#ifdef __GNUC__
#define REGPARM(x)  __attribute__((regparm(x)))
#define NOINLINE    __attribute__((noinline))
#define ALIGNED(n)  __attribute__((aligned(n)))
#define unlikely(x) __builtin_expect((x), 0)
#define likely(x)   __builtin_expect(!!(x), 1)
#else
#define REGPARM(x)
#define NOINLINE
#define ALIGNED(n)
#define unlikely(x) (x)
#define likely(x)   (x)
#endif

/* Logging stubs for PicoDrive components */
#ifndef elprintf
#define EL_STATUS  0
#define EL_ANOMALY 0
#define elprintf(level, ...) do { } while(0)
#endif

#endif /* PICO_PORT_H */
