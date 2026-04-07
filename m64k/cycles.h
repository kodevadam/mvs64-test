#ifndef M64K_CYCLES_H
#define M64K_CYCLES_H

#include <stdint.h>

// Exception cycle costs for the 68000 CPU.
// Derived from the Musashi m68ki_exception_cycle_table[0] (68000 row).
static const uint8_t __m64k_exception_cycle_table[256] = {
     40, /*  0: Reset - Initial Stack Pointer                      */
      4, /*  1: Reset - Initial Program Counter                    */
     50, /*  2: Bus Error                                          */
     50, /*  3: Address Error                                      */
     34, /*  4: Illegal Instruction                                */
     38, /*  5: Divide by Zero                                     */
     40, /*  6: CHK                                                */
     34, /*  7: TRAPV                                              */
     34, /*  8: Privilege Violation                                 */
     34, /*  9: Trace                                              */
     34, /* 10: 1010                                               */
     34, /* 11: 1111                                               */
      4, /* 12: RESERVED                                           */
      4, /* 13: Coprocessor Protocol Violation                     */
      4, /* 14: Format Error                                       */
     44, /* 15: Uninitialized Interrupt                            */
      4, /* 16: RESERVED                                           */
      4, /* 17: RESERVED                                           */
      4, /* 18: RESERVED                                           */
      4, /* 19: RESERVED                                           */
      4, /* 20: RESERVED                                           */
      4, /* 21: RESERVED                                           */
      4, /* 22: RESERVED                                           */
      4, /* 23: RESERVED                                           */
     44, /* 24: Spurious Interrupt                                 */
     44, /* 25: Level 1 Interrupt Autovector                       */
     44, /* 26: Level 2 Interrupt Autovector                       */
     44, /* 27: Level 3 Interrupt Autovector                       */
     44, /* 28: Level 4 Interrupt Autovector                       */
     44, /* 29: Level 5 Interrupt Autovector                       */
     44, /* 30: Level 6 Interrupt Autovector                       */
     44, /* 31: Level 7 Interrupt Autovector                       */
     34, /* 32: TRAP #0                                            */
     34, /* 33: TRAP #1                                            */
     34, /* 34: TRAP #2                                            */
     34, /* 35: TRAP #3                                            */
     34, /* 36: TRAP #4                                            */
     34, /* 37: TRAP #5                                            */
     34, /* 38: TRAP #6                                            */
     34, /* 39: TRAP #7                                            */
     34, /* 40: TRAP #8                                            */
     34, /* 41: TRAP #9                                            */
     34, /* 42: TRAP #10                                           */
     34, /* 43: TRAP #11                                           */
     34, /* 44: TRAP #12                                           */
     34, /* 45: TRAP #13                                           */
     34, /* 46: TRAP #14                                           */
     34, /* 47: TRAP #15                                           */
      4,4,4,4,4,4,4,4, /* 48-55: FP exceptions (unemulated)       */
      4,4,4,4,4,4,4,4, /* 56-63: MMU / RESERVED                   */
    /* 64-255: User Defined */
      4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
      4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
      4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
      4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
      4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
      4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4
};

#endif
