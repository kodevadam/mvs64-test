# MVS64 - N64 Performance Optimization Plan

This document outlines a comprehensive plan to improve performance of the MVS64
NeoGeo emulator when running on Nintendo 64 hardware. Items are grouped by
subsystem and roughly ordered by expected impact.

---

## 1. CPU Emulation (m64k core)

### 1.1 Enable idle-skip (`rom_pc_idle_skip`)
- **File:** `roms.c:410`, `emu.c:68-79`
- **Problem:** `rom_pc_idle_skip` is parsed from `game.ini` but then immediately
  overwritten to 0 (`roms.c:410`). Many NeoGeo games have a tight spin loop in
  their main loop waiting for VBlank (e.g. `bne.s *-2`). The emulator currently
  burns real N64 CPU cycles emulating these busy-wait loops instruction by
  instruction.
- **Fix:** Remove the `rom_pc_idle_skip = 0;` override. Implement idle-skip in
  `m64k_exec()`: when the 68K PC matches the idle-skip address, advance the
  m68k clock to the next event instead of emulating individual opcodes. This
  could save 20-40% of CPU time per frame depending on the game.
- **Complexity:** Low

### 1.2 Use coarse timing mode for select games
- **File:** `m64k/m64k_config.h:18-19`
- **Problem:** `M64K_CONFIG_TIMING_ACCURACY` is 0 (approximate). For games that
  don't depend on cycle-exact timing, setting it to a negative value (fixed
  cycle count per opcode) would be faster.
- **Fix:** Add per-game timing configuration in `game.ini`. Games that work fine
  with coarse timing (e.g. puzzle games, simple fighters) can use `-4` or `-8`
  for a meaningful speedup in the CPU interpreter loop.
- **Complexity:** Low

### 1.3 AOT recompilation for hot game code
- **File:** `genhle.c`, `m68k_recompiler.h`
- **Problem:** The AOT (Ahead-Of-Time) recompiler infrastructure exists
  (`genhle.c`) but doesn't appear to be integrated into the N64 runtime. Even a
  static recompilation of known-hot functions (VBlank handler, main game loop)
  into native MIPS would eliminate the 68K decode/dispatch overhead for those
  paths.
- **Fix:** Integrate `genhle` output into the N64 build. Identify per-game hot
  functions and replace them with native MIPS. Even partial coverage of the top
  5-10 functions would yield large gains.
- **Complexity:** High (but infrastructure already exists)

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

| Priority | Item | Expected Gain | Effort |
|----------|------|---------------|--------|
| 1 | 1.1 Enable idle-skip | 20-40% CPU | Low |
| 2 | 5.1 Auto-frameskip | Playability | Trivial |
| 3 | 2.1 Reduce RDP syncs | 15-25% RDP | Medium |
| 4 | 6.3 Remove hot-path debugf | 5-10% CPU | Low |
| 5 | 3.1 Increase sprite cache | 10-20% DMA | Low |
| 6 | 2.3 Better palette caching | 5-15% RDP | Low |
| 7 | 3.2 Fix cache invalidation | 3-5% DMA | Low |
| 8 | 4.1 Fast-path HWIO reads | 5-10% CPU | Medium |
| 9 | 2.4 Skip offscreen sprites | 5-10% render | Low |
| 10 | 1.2 Coarse timing mode | 5-15% CPU | Low |
| 11 | 6.1 Uncached RDP buffers | 3-5% render | Low |
| 12 | 3.4 Larger PBROM banks | 5-10% DMA | Low |
| 13 | 6.2 Optimize event system | 2-3% CPU | Low |
| 14 | 2.5 Wider Copy mode use | 5-10% RDP | Medium |
| 15 | 2.2 Batch sprite commands | 10-15% RSP | Medium-High |
| 16 | 3.3 Prefetch sprites | 5-10% DMA | Medium |
| 17 | 2.6 Skip empty fix tiles | 2-5% render | Low |
| 18 | 1.3 AOT recompilation | 30-50% CPU | High |
| 19 | 4.2 Avoid delay-slot MMIO | 3-5% CPU | High |
| 20 | 7.1 RSP audio | Future | High |

---

## Profiling Approach

The codebase already has profiling instrumentation (`emu.c:300-311`). Use the
existing `cpu%/io%/draw%/dma%` breakdown to identify which category dominates
for each game, and focus optimization effort accordingly:

- **cpu% high:** Focus on items 1.1, 1.2, 1.3, 6.3
- **io% high:** Focus on items 4.1, 4.2
- **draw% high:** Focus on items 2.1-2.6
- **dma% high:** Focus on items 3.1-3.4
