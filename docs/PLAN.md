# mina-recomp: plan and architecture

A static recompilation of **Mina the Hollower** (Yacht Club Games, 2025) from
the arm64 slice of its macOS executable to portable C, aimed at the
**PlayStation Vita** (ARMv7, 32-bit, 512 MB). Same approach as igi-recomp:
the build recompiles *your* executable, the game reads *your* data, nothing of
the game goes into the repo.

## What the executable gives us (phase 1 findings)

- `MinaTheHollower`, universal (x86_64 + arm64). We recompile the **arm64**
  slice: fixed-width instructions, no x87, exact function bounds from
  `LC_FUNCTION_STARTS`.
- **27,271 functions, 3.3M instructions, 230 distinct mnemonics**, all of them
  decodable (`tools/analysis/census.py`). Mostly integer, scalar float and
  loads/stores; NEON is light (thousands of `dup/zip/bsl/movi`, the rest rare);
  ~200 LSE atomics.
- **Not stripped**: 31k named functions (C++ mangled), 8k STABS entries (only
  for the statically linked SDL2). Every function can be found and replaced by
  name.
- Engine: Yacht Club's "gothic" engine (`yc*` classes) + SDL2 2.x linked
  statically (Cocoa video, Metal renderer backend, CoreAudio, HIDAPI).
  The game draws through its own renderer on **Metal**; shaders ship as MSL in
  `data/shaders.pak.yc`.
- Imports (779 stub/GOT slots): libSystem (libc, pthreads, libdispatch, mach
  time, TLS), 9 libc++ runtime helpers, Steam API (9), AppKit/Foundation/
  CoreFoundation/CoreAudio/IOKit/Metal (mostly through `objc_msgSend`).
  No `__cxa_throw`: C++ exceptions are never raised, so no unwinding.
  `operator new` lives inside the game (own allocator).
- Image: `__TEXT` 13.6 MB, `__DATA_CONST` 0.5 MB, `__DATA` 69 MB of which
  **66 MB is `__bss`** (static pools). 57,669 rebased pointers, 643 binds;
  classic `LC_DYLD_INFO_ONLY` opcodes (no chained fixups).
- Data: 107 `*.yc` files, 870 MB (630 MB is the music bank). Container
  `YCD\0`, entries compressed with **wfLZ**.

## Guest model

- **Address space**: guest pointers stay 64-bit values in registers and in
  memory (structure layouts are untouched), but every guest address the
  program can ever see is below 4 GB. Host address = `g_mem + (uint32_t)a`.
  On the Vita that is one add per access; on the Mac `g_mem` is a 4 GB
  reservation.
- **Image relocation**: the image is moved from 0x1_0000_0000 to
  `GUEST_IMAGE_BASE` (low 4 GB). The loader applies the rebase list; the
  generator relocates every `adrp/adr/literal` constant at generation time.
- **Heap and stacks**: `malloc`/`calloc`/`realloc`/`mmap` are implemented by
  the runtime inside the guest space. Each guest thread gets a guest stack.
- **Imports**: each import gets a guest address in a thunk area; calling it
  (directly through a stub, or indirectly through a GOT/function pointer)
  runs the host implementation, which reads arguments from the context.

## Generated code

- One C function per guest function: `void f_<addr>(Ctx *c)`.
- AAPCS64 calling convention is exploited: arguments (x0–x8, v0–v7, sp) go
  through the context; returns come back in x0/x1, v0–v3. Callee-saved
  registers (x19–x29, d8–d15) stay in C locals across calls because the guest
  callee restores them itself.
- Registers are C locals inside a function; the compiler keeps them in host
  registers. NZCV is computed as locals at each flag setter, with direct
  comparisons when the consumer follows its setter (dead flags vanish).
- Direct calls are C calls; indirect calls (`blr`, vtables) go through a hash
  of function addresses; `br` jump tables are resolved at generation time.
- Functions can be replaced by hand-written C by name (SDL, renderer, audio,
  memcpy & co., hot paths), the way igi-recomp grew `native_*.c`.

## Platform layer

Replace at the **SDL** and **renderer** level, not at Cocoa/Metal level:

- SDL2 functions inside the binary are swapped for the host's SDL2 (desktop)
  or for Vita implementations (input, audio, threads, timers, files).
- The engine's Metal renderer classes are replaced by an OpenGL backend
  (desktop) / vitaGL (Vita). MSL shaders are rewritten as GLSL/CG.
- Steam is stubbed.

## Phases

1. **Analysis** *(done)*: Mach-O reader, imports, census, data format.
2. **Recompiler + desktop runtime**: generate C for every function, load the
   image, run `main` on macOS with the host SDL2 and a GL renderer. Validate
   instruction semantics against the real CPU (the Mac is arm64: the original
   code can run natively side by side with its translation).
3. **32-bit**: build the same runtime for a 32-bit target, measure memory and
   CPU cost, first run on Vita3K / hardware. Go / no-go on performance.
4. **Vita port**: vitaGL renderer, audio, controls, LiveArea, natives for hot
   paths, memory budget (the 66 MB bss and the music bank streaming).
