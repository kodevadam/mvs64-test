# MVS64 - N64 Performance Optimization Plan

This document outlines a comprehensive plan to improve performance of the MVS64
NeoGeo emulator when running on Nintendo 64 hardware. Items are grouped by
subsystem and roughly ordered by expected impact.

---

## Empirical Findings (April 2026)

Several CPU-side optimization attempts were measured on real hardware running
Blazing Star, with the existing PROFILE instrumentation. The findings below
should inform any future CPU work.

### What worked: profile-guided AOT (item 1.3)

Wiring `genhle` into the build and selecting hot 68K function entry points
from PROFILE samples produced the first durable, regression-free speedup:

| Metric | Baseline interpreter | With HLE (8 PCs) | Δ |
|---|---|---|---|
| Floor FPS | 41 | 42 | +2% |
| Median FPS | ~43 | ~47.5 | +10% |
| Average FPS | ~42.5 | ~48 | +13% |
| Peak FPS | 45 | 59.2 | +30% |

Critically, **variance stayed healthy** — no scenes regressed, unlike earlier
attempts. The win is consistent across light, medium, and heavy scenes.

How it works: `m68k_execute()` now checks `hle_get_func(REG_PC)` before each
interpreter dispatch (`m68kcpu.c:1000`).  When the PC matches a recompiled
function, control transfers to native code that operates on the CPU state
through restricted pointers, then returns the next PC to resume from.  Exits
back to the interpreter on subroutine calls or out-of-function jumps.

Required fixes to genhle to make it production-viable:
1. Strip leading `USE_CYCLES(...)` from the pasted handler body — genhle was
   double-counting cycles, causing the emulated CPU to run at ~half speed.
2. Convert unsatisfied forward-jump targets into bail-to-interpreter stubs
   instead of panicking.  Real 68K functions frequently branch outside their
   "natural" body and the recompiler must degrade gracefully.

### Follow-up wins: idle-skip + one more HLE PC (Apr 2026, round 2)

After banking the first HLE milestone, two more single-step experiments
moved the floor substantially further:

1. **Idle-skip for Blazing Star at PC 0x43fe.**  The dominant ~90%-of-samples
   idle loop is now caught by the existing `m68k_check_idle_skip()` branch-
   macro hook (`m68kcpu.h:1550`) instead of being HLE'd.  Key insight: HLE
   preempts the interpreter entirely, so an HLE'd idle loop *defeats* idle-
   skip by spinning in generated C gotos and never returning until cycles
   are already consumed.  Removing 0x43fe from `HLE_PCS` and configuring
   `idle_skip=0x43fe` in game.ini lets the branch-macro path burn the
   timeslice in O(1) on loop back-edges.
   - **False start:** An initial attempt added the idle check at the *top*
     of `m68k_execute()`'s dispatch loop, before any instruction ran.  That
     bricked boot — the CMP that polls the VBL flag never executed, so the
     game couldn't exit the loop, and the watchdog reset.  The branch-macro
     path works because it only fires *after* at least one pass through the
     loop body, where polling already happened.  Lesson: idle-skip semantics
     require "we are looping back," not "we are here."
2. **Add 0x5866 to `HLE_PCS` based on heavy-scene profile hotspots.**  This
   single-entry addition produced the biggest single-change FPS delta
   measured on this project so far.

| Metric | 7-PC HLE + idle-skip | 8-PC HLE (+0x5866) + idle-skip | Δ |
|---|---|---|---|
| Gameplay floor FPS | 39.2 | 46.5 | **+7.3** |
| Median gameplay FPS | ~46.5 | ~53 | **+6.5** |
| Peak gameplay FPS | 53.4 | 57.4 | +4.0 |
| FPS spread | 14 | 11 | tighter |
| Heavy-scene CPU% | 150–165 | 125–140 | lower |

The profile *shape* changed too.  Heavy scenes that previously ran at
FPS 39–41 with CPU~160% / draw~22% now run at FPS 52–57 with
CPU~130% / draw~40–47% / DMA~15–22%.  **The bottleneck in the hardest
scenes flipped from CPU to draw/DMA.**  The CPU-bound floor (46–49 FPS
with modest draw) remains, but with more scattered hot PCs — the big,
obvious CPU lever has been pulled.

This is the signal to pivot: further HLE slots have diminishing returns
(workload is now spread across many lukewarm PCs, and going past 8
entries risks PBROM allocation failure), while draw/DMA has become a
fresh, credible ceiling.  Frozen milestone build: `HLE_PCS = 5cba 5b50
5152 522e 5382 53d2 5a26 5866` with `idle_skip=0x43fe` for Blazing Star.

### What did NOT work: interpreter micro-optimizations

Four separate attempts to reduce per-instruction interpreter overhead were
measured and reverted:

1. **Pin hot variables to MIPS callee-saved registers via `register int x asm()`**
   Failed: GCC 14.2.0 + mips64-elf + LTO has multiple incompatibilities with
   global register variables.  Symptoms ranged from compile errors ("global
   register variable follows a function definition") to subtle runtime
   corruption (NULL pointer dereferences in unrelated TUs because `-ffixed-16`
   broke register allocation across LTO-merged TUs).

2. **Embed cycle counter as offset 0 of `m68ki_cpu` struct**
   Theory: GCC could share one base-address load between cycle counter and
   other CPU fields after each dispatch call.  Result: ~20% CPU regression
   (skip frames went from 95-108% to 114-131%).  GCC was apparently already
   keeping the standalone global in a callee-saved register; wrapping it in
   a struct member defeated that optimization.

3. **Computed-goto (threaded) dispatch with function calls retained**
   The "hybrid" approach: replace the two-level indirect call table with a
   computed-goto dispatch, but still call the existing handler functions from
   each label.  Result: improved best-case scenes (54 FPS peak) but regressed
   worst-case (34 FPS floor), with **doubled variance**.  The 256KB flat label
   table thrashed D-cache worse than the original 1KB-subtable design did.

4. **Full handler inlining** was not attempted after #3 made it clear that
   even the hybrid version fought the cache.  A 40k-line mega-function would
   only worsen I-cache thrashing.

### The architectural lesson

> **On this toolchain, GCC + LTO already produce near-optimal code for the
> Musashi interpreter loop.  Tweaks at the dispatch layer either lose to the
> compiler or trade variance for peak speed.  The path to durable gains is
> *avoiding the interpreter*, not optimizing it — i.e. profile-guided AOT.**

Subsequent CPU work should:
- Bias toward removing work entirely (idle-skip, HLE more functions, coarse
  timing) rather than making per-instruction execution faster.
- Re-baseline with fresh PROFILE data after every win — heavy/light scenes
  shift hot PCs around, and an HLE list tuned for the wrong scene set hurts.
- Treat the interpreter as the trusted reference, not the optimization target.

### Memory budget caveat

HLE costs RAM.  Each `hle_*.c` adds a few KB of code/data, and Blazing Star's
PBROM allocation requires a 1 MiB-aligned contiguous region from the heap.  A
16-PC list crashed on boot ("cannot allocate PBROM buffer") on 4 MiB consoles;
8 PCs is the verified-safe ceiling.  Verify per-game headroom before expanding.

---

## 1. CPU Emulation (m64k core)

### 1.1 Enable idle-skip (`rom_pc_idle_skip`)  ✅ DONE (Apr 2026)
- **File:** `roms.c:418`, `m68kinline.h:90`, `m68kcpu.h:1550,1557,1568`,
  `mvsmakerom.c:519`
- **Status:** Per-game `idle_skip` values are parsed from the synthetic
  `game.ini` baked in by `mvsmakerom.c`; `rom_pc_idle_skip` is populated at
  load time and consumed by `m68k_check_idle_skip()` inside the Musashi branch
  macros (`m68ki_branch_8/16/32`).  On a branch-back to the idle PC,
  `m68k_consume_timeslice()` sets cycles to 0 and the dispatch loop exits.
  **Must live in the branch macros, not at dispatch-loop entry** — see
  "Follow-up wins" above for the false-start analysis.
- **Result:** Combined with the 8th HLE PC, lifted the Blazing Star floor
  from 39.2 → 46.5 FPS.

### 1.2 Use coarse timing mode for select games
- **File:** `m64k/m64k_config.h:18-19`
- **Problem:** `M64K_CONFIG_TIMING_ACCURACY` is 0 (approximate). For games that
  don't depend on cycle-exact timing, setting it to a negative value (fixed
  cycle count per opcode) would be faster.
- **Fix:** Add per-game timing configuration in `game.ini`. Games that work fine
  with coarse timing (e.g. puzzle games, simple fighters) can use `-4` or `-8`
  for a meaningful speedup in the CPU interpreter loop.
- **Complexity:** Low

### 1.3 AOT recompilation for hot game code  ✅ DONE (Apr 2026)
- **File:** `genhle.c`, `m68k_recompiler.h`, `Makefile.mvs64`, `m68kcpu.c:1000`
- **Status:** Wired into the build, gated by `-DUSE_HLE`. Per-game hot PC list
  in `Makefile.mvs64` (`HLE_PCS`); `make mvs64 USE_HLE=1 ...` regenerates
  hle_*.c automatically from the byteswapped P-ROM/BIOS.
- **Result:** +13% average FPS, +30% peak on Blazing Star (see Empirical
  Findings).  Generates ~C output, not native MIPS — but with restricted-
  pointer ABI in `m68k_recompiler.h`, GCC produces tight code that beats the
  interpreter's per-instruction decode + dispatch overhead.
- **Follow-up work:**
  1. Re-baseline against a longer PROFILE session and prune/expand HLE_PCS.
  2. Verify which next-hottest PCs (e.g. 0x51de, 0x5b3c, 0x5a3a from the
     Apr 2026 profile) survive the re-baseline before adding them.
  3. Stay under the 8-entry RAM-budget ceiling for Blazing Star, or verify
     per-game.
  4. Consider extending `genhle` to handle the patterns it currently bails on
     (jump tables outside duff devices, certain branch types) for broader
     coverage.

---

## 2. Rendering / RDP Pipeline

### 2.1 Reduce RDP sync commands in sprite rendering
- **File:** `rsp_video.S:373-375`
- **Problem:** Every sprite draw issues `SyncPipe + SyncTile + SyncLoad` (3
  sync commands) before the actual draw. RDP syncs stall the pipeline. Most of
  these are unnecessary when consecutive sprites don't share TMEM/tile state.
- **Fix:** Track RDP state in the RSP overlay and only emit syncs when actually
  needed (e.g., when TMEM is about to be overwritten while the previous
  primitive that references it hasn't finished). This is the single biggest RDP
  optimization opportunity.
- **Complexity:** Medium

### 2.2 Batch sprite draws into fewer RSP commands
- **File:** `video.c:89-198`, `rsp_video.S:467-642`
- **Problem:** Each sprite tile results in a separate `rspq_write()` call from
  the CPU and a separate RSP command. A NeoGeo frame can have 380+ sprites with
  multiple tiles each. The per-command overhead (rspq ring buffer write, RSP
  command dispatch) adds up.
- **Fix:** Implement a batched sprite command that accepts multiple sprites in a
  single RSP command (e.g., a DMA list of sprite descriptors). The RSP would
  process the batch in one dispatch cycle, reducing overhead.
- **Complexity:** Medium-High

### 2.3 Eliminate unnecessary palette loads via better caching
- **File:** `rsp_video.S:492-523`, `video_n64.c:52-68`
- **Problem:** The RSP sprite path uses `andi palslot, palettenum, 0xF` to pick
  a TMEM palette slot (line 497). This is essentially `palnum % 16`. If two
  palettes have the same lower 4 bits, they'll evict each other every frame. The
  CPU path (`video_n64.c:62-63`) has a similar round-robin issue.
- **Fix:** Implement proper LRU or least-recently-used slot selection. Track
  the 16 TMEM palette slots with an age counter. On miss, evict the oldest slot.
  Even a simple "clock hand" approach would reduce TMEM thrashing significantly.
- **Complexity:** Low

### 2.4 Skip fully offscreen and empty sprites early
- **File:** `video.c:89-198`
- **Problem:** The sprite loop iterates all 381 sprites and only does basic
  `sx >= 320 && sx+sw <= 512` culling (line 109). Individual tiles within a
  sprite are still processed even when they're entirely above/below screen.
- **Fix:** Add early-out checks:
  1. Skip sprites where `sy + sh` is entirely above screen (y > 512-16 area)
  2. Skip individual tiles where `ssy >= 224` and `ssy+ssh < 512` (fully below
     visible area)
  3. Skip tiles with `tnum == 0` (empty/blank tile) at the tile loop level
- **Complexity:** Low

### 2.5 Use RDP Copy mode more aggressively
- **File:** `video_n64.c:89-127`
- **Problem:** Copy mode (4 pixels/cycle) is only used for unscaled, unflipped,
  unclipped 16x16 sprites. Many sprites are unscaled but flipped or slightly
  clipped. Copy mode can handle Y-clipping (already done) and could be extended.
- **Fix:** Handle X-clipping in copy mode by adjusting the rectangle
  coordinates. Handle flipx in copy mode by pre-flipping the tile data in TMEM
  (cheap on RSP). This would increase the percentage of sprites drawn at 4x
  speed.
- **Complexity:** Medium

### 2.6 Fix layer: skip empty tiles
- **File:** `video.c:61-77`
- **Problem:** The fix layer iterates all 40x28 = 1120 tile positions. The `if
  (v)` check (line 71) skips tiles with value 0, but many games have sparse fix
  layers (score display only). Each non-zero tile still generates RSP commands
  even if the tile graphics are all transparent.
- **Fix:** Pre-scan the fix layer VRAM and build a list of only non-zero tiles.
  Consider caching this list and invalidating on VRAM writes to the fix region.
- **Complexity:** Low

---

## 3. Sprite Cache / ROM Access

### 3.1 Increase CROM sprite cache size
- **File:** `roms.c:50-51`
- **Problem:** CROM cache is only 1280 sprites (1280 * 128 bytes = 160 KB).
  NeoGeo games routinely use more unique tiles per frame than this, causing
  constant cache eviction and DFS reads from cartridge ROM every frame.
- **Fix:** On systems with the Expansion Pak (8MB RDRAM), increase the cache to
  2560+ sprites. Use `get_memory_size()` to detect available RAM and scale the
  cache accordingly. Each additional 1280 sprites costs only 160 KB.
- **Complexity:** Low

### 3.2 Remove unnecessary `data_cache_hit_writeback_invalidate` on sprite loads
- **File:** `roms.c:66, 90`
- **Problem:** After `dfs_read()` into the sprite cache, the code does
  `data_cache_hit_writeback_invalidate()` which flushes *and* invalidates the
  cache line. The FIXME comments already note this may be unnecessary. Since
  `dfs_read()` uses DMA which bypasses the CPU cache, we only need
  `data_cache_hit_invalidate()` (invalidate without writeback), which is cheaper.
- **Fix:** Replace `data_cache_hit_writeback_invalidate` with
  `data_cache_hit_invalidate` for DFS reads into freshly-allocated sprite cache
  buffers (which have no dirty data to write back).
- **Complexity:** Low

### 3.3 Prefetch sprites for the next frame
- **File:** `roms.c`, `video.c`
- **Problem:** Sprite cache misses cause synchronous DFS reads mid-frame, which
  stall the CPU and block rendering. Games with scrolling backgrounds constantly
  introduce new tiles.
- **Fix:** During render, record cache misses. After the frame, kick off
  asynchronous DFS reads for those sprites so they'll be in cache by next frame.
  Most scrolling games reuse the same tiles across consecutive frames.
- **Complexity:** Medium

### 3.4 Increase PBROM bank size for cached mode
- **File:** `roms.c:183`
- **Problem:** `PBROM_BANK_BITS = 6` means 64-byte banks. Each cache miss
  triggers a `dfs_read()` of only 66 bytes. The fixed overhead of each DFS
  operation (seek + DMA setup) dominates when loading such tiny amounts.
- **Fix:** Experiment with `PBROM_BANK_BITS = 8` (256 bytes) or even 10 (1 KB).
  Larger banks amortize the per-read overhead. The hash table will have fewer
  entries but each load brings in more useful data (spatial locality).
- **Complexity:** Low

---

## 4. TLB / Exception Handler

### 4.1 Fast-path more MMIO operations in assembly
- **File:** `hw_n64.S:551-569`
- **Problem:** The assembly fast-path handles PBROM reads, watchdog, LSPC VRAM,
  and palette writes. But HWIO reads (ports 0x30xxxx, 0x32xxxx, 0x38xxxx) still
  fall through to the slow C handler path (`tlbcallhandler_standard`) which
  saves/restores 20+ registers.
- **Fix:** Add assembly fast-paths for the most common HWIO reads:
  - `0x300000` (P1 controller input) - just read a cached byte
  - `0x300001` (dipswitches) - return a constant
  - `0x320001` (status A) - read a cached byte
  - `0x380000` (status B) - read a cached byte
  These are read every frame and the full C context switch is wasteful.
- **Complexity:** Medium

### 4.2 Avoid branch-delay-slot analysis when possible
- **File:** `hw_n64.S:409-493`
- **Problem:** The TLB exception epilogue must analyze whether the faulting
  instruction was in a branch delay slot, and if so, decode the branch target.
  This is ~20 instructions of overhead per MMIO access.
- **Fix:** In the m64k core, avoid placing memory accesses in branch delay slots
  when generating code (if applicable). Alternatively, cache the branch target
  from the instruction decode phase so the exception handler doesn't need to
  re-decode it.
- **Complexity:** High (touches m64k internals)

---

## 5. Frame Pacing / Frameskip

### 5.1 Enable auto-frameskip by default
- **File:** `emu.h:8`
- **Problem:** `CONFIG_FRAMESKIP_MODE` is 0 (disabled). When the emulator can't
  maintain 60 FPS, it just runs slow. This makes games feel laggy instead of
  dropping visual frames to maintain gameplay speed.
- **Fix:** Set `CONFIG_FRAMESKIP_MODE` to 2 (auto). The auto-skip code already
  exists (`emu.c:174-191`) and caps at 4 skipped frames. This maintains game
  speed at the cost of visual smoothness, which is generally the better
  tradeoff for playability.
- **Complexity:** Trivial

### 5.2 Skip render work during frameskip, not just display
- **File:** `emu.c:171-216`
- **Problem:** When frameskipping, `emu_render()` returns early, but
  `rom_next_frame()` is still called (line 314) which ticks the sprite cache.
  More importantly, the palette conversion (`render_begin` in `video_n64.c:219-
  223`) and RSP work are skipped, but only because `emu_render` returns before
  calling `video_render()`. This is correct but could be more explicit.
- **Fix:** Ensure sprite cache ticks are preserved during frameskip (they are)
  but also ensure no RSP/RDP work leaks through. Currently this is fine, but
  worth verifying if new render paths are added.
- **Complexity:** N/A (verification)

---

## 6. Memory & Data Structure Optimizations

### 6.1 Use uncached memory for RDP-consumed buffers
- **File:** `roms.c:39`, `video.c:49`
- **Problem:** Sprite pixel data and palette data are in cached RDRAM. Before
  the RDP can use them, they must be explicitly flushed from cache
  (`data_cache_hit_writeback`). This is both slow and error-prone.
- **Fix:** Allocate `PALETTE_RAM_EMU` and sprite cache pixel buffers in
  uncached memory (`malloc_uncached_aligned`). The CPU writes to uncached
  addresses go straight to RDRAM, eliminating the need for cache flushes before
  RDP use. The sprite cache already uses `memalign`, change to
  `malloc_uncached_aligned` for the pixel buffer.
- **Complexity:** Low

### 6.2 Optimize the event system
- **File:** `emu.c:83-90`
- **Problem:** `next_event()` does a linear scan of all 8 events every time
  it's called, and it's called in a tight loop in `emu_run_frame()`.
- **Fix:** Maintain events in a sorted array or use a minimum variable that
  tracks the earliest event. Since MAX_EVENTS is only 8, a simple "cache the
  minimum" approach works: update it on add/change/complete. This eliminates the
  scan on every iteration.
- **Complexity:** Low

### 6.3 Reduce `debugf` calls in hot paths
- **File:** `emu.c:72, 165, 201, 240`, `roms.c:321`
- **Problem:** `debugf()` calls are scattered throughout the hot path, including
  inside `m68k_exec()` (line 72), `emu_render()` (line 201), and
  `emu_run_frame()` (line 240). Even when debug output is disabled, the format
  string arguments are still evaluated and the function call overhead exists.
- **Fix:** Wrap hot-path debugf calls in `#ifndef NDEBUG` or use a macro that
  compiles to nothing in release builds. Alternatively, use
  `__attribute__((cold))` on debug paths to keep them out of the instruction
  cache.
- **Complexity:** Low

---

## 7. Audio (Future)

### 7.1 Plan for Z80 + YM2610 emulation overhead
- **Problem:** Sound is not yet implemented. When it is, it will add significant
  CPU load (Z80 CPU emulation + YM2610 FM synthesis + ADPCM decoding).
- **Fix:** Plan now to run sound emulation on the RSP during idle cycles (after
  the RSP finishes rendering). The RSP's vector unit is well-suited for audio
  mixing and FM synthesis. Alternatively, run the Z80 at reduced accuracy or
  implement audio-only frameskip.
- **Complexity:** High (future work)

---

## Priority Order (Estimated Impact)

| Priority | Item | Expected Gain | Effort | Status |
|----------|------|---------------|--------|--------|
| ✅ | 1.3 AOT recompilation (HLE) | +13% measured (Blazing Star) | High | Done Apr 2026 |
| ✅ | 1.1 Enable idle-skip | +7 FPS floor (combined w/ 8th HLE PC) | Low | Done Apr 2026 |
| 1 | 2.1 Reduce RDP syncs | 15-25% RDP | Medium | **← next** (draw% pivot) |
| 2 | 5.1 Auto-frameskip | Playability | Trivial | |
| 3 | 3.1 Increase sprite cache | 10-20% DMA | Low | candidate (dma% pivot) |
| 4 | 6.3 Remove hot-path debugf | 5-10% CPU | Low | |
| 5 | 2.3 Better palette caching | 5-15% RDP | Low | |
| 7 | 3.2 Fix cache invalidation | 3-5% DMA | Low | |
| 8 | 4.1 Fast-path HWIO reads | 5-10% CPU | Medium | |
| 9 | 2.4 Skip offscreen sprites | 5-10% render | Low | |
| 10 | 1.2 Coarse timing mode | 5-15% CPU | Low | |
| 11 | 6.1 Uncached RDP buffers | 3-5% render | Low | |
| 12 | 3.4 Larger PBROM banks | 5-10% DMA | Low | |
| 13 | 6.2 Optimize event system | 2-3% CPU | Low | |
| 14 | 2.5 Wider Copy mode use | 5-10% RDP | Medium | |
| 15 | 2.2 Batch sprite commands | 10-15% RSP | Medium-High | |
| 16 | 3.3 Prefetch sprites | 5-10% DMA | Medium | |
| 17 | 2.6 Skip empty fix tiles | 2-5% render | Low | |
| 18 | 4.2 Avoid delay-slot MMIO | 3-5% CPU | High | |
| 19 | 7.1 RSP audio | Future | High | |

**Items intentionally NOT in this table** (proven to either fail or regress on
the current toolchain — see Empirical Findings):

| Attempted | Outcome |
|---|---|
| Pin hot vars to MIPS s-registers (`register int x asm()`) | Toolchain incompatible; LTO breaks register allocation |
| Embed `m68ki_remaining_cycles` in `m68ki_cpu` struct | -20% CPU regression; GCC was already doing better |
| Computed-goto dispatch (hybrid w/ function calls) | Doubled FPS variance; cache-hostile; reverted |

---

## Profiling Approach

The codebase already has profiling instrumentation (`emu.c:300-311`). Use the
existing `cpu%/io%/draw%/dma%` breakdown to identify which category dominates
for each game, and focus optimization effort accordingly:

- **cpu% high:** Focus on items 1.1, 1.2, 1.3, 6.3
- **io% high:** Focus on items 4.1, 4.2
- **draw% high:** Focus on items 2.1-2.6
- **dma% high:** Focus on items 3.1-3.4
