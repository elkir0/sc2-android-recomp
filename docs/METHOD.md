# Method: AI-assisted static recompilation of an x86-32 game to native ARM64

This document describes the end-to-end methodology we validated on a 2010
DirectX 9 title (15.7 MB x86-32 executable, 86,650 discovered functions).
It assumes you work **on your own legitimate copy of the game**; every derived
artifact is generated locally on your machine.

## 1. Inventory and extraction (over your own copy)
- Identify the disc/archive formats. 2010 Blizzard installers use
  stream-encrypted MPQ (`.MPQE`); the decryption keys are published by the
  publisher itself (see StormLib's `FileStream.cpp`, STREAM_PROVIDER_MPQE).
  [StormLib](https://github.com/ladislav-zezula/StormLib) (OSS) reads them
  via the `mpqe-file:` prefix.
- Inventory every file (type, size, hash) before touching anything.

## 2. Static analysis
- Import the executable into Ghidra headless (bounded memory: `Xmx12g` +
  cgroup). Triage mechanically (cheap models / scripts): entry point, CRT,
  import table, string table, section map.
- Build the boot map: entry → CRT init → WinMain → fiber/game-main → the
  game's own staged init. Names like "Must run from the launcher" tell you
  exactly where the launch-chain checks live.

## 3. Lift to portable C (CPU-struct model)
- Use [pcrecomp](https://github.com/sp00nznet/pcrecomp)'s pipeline:
  `pe_analyze` → `disasm32` (recursive descent) → `lift32_cpu`.
- The CPU-struct model (explicit `CPU *c` state, all memory accesses through
  `rd*/wr*` helpers) is **reentrant and multi-thread friendly** — this is the
  single most important architectural choice for later parallelism.
- Expect ~1 line of C per byte of machine code for large titles (86k functions
  → ~400 MB of C here). Compile it as a normal host program: the same source
  builds on x86-64 Linux, Apple Silicon macOS, and aarch64 Android.

## 4. Host runtime ("fake Win32")
- Implement what the engine actually calls: fibers (host `ucontext` or
  threads), a 32-bit handle table, TLS/TEB, the import table (IAT) with
  traced stubs, file layer over your extracted install, MPQ access via
  StormLib.
- **Key trick for hosts without fixed low mappings (macOS/Android)**: reserve
  one anonymous host region for the whole guest address space and make every
  guest-VA access go through a single relocatable macro (`GPTR(va) = base +
  va`). Because the CPU-struct model funnels ALL memory accesses through one
  pair of helpers, this is a ~30-line change.

## 5. Rendering: D3D9 → Vulkan headless first
- Start with a **headless Vulkan device** (Lavapipe on CI/dev machines):
  implement the IDirect3D9/Device9 surface the engine traces (Clear,
  BeginScene/EndScene, Present) and dump the backbuffer to PPM after frames.
  "921,600 non-black pixels" is a milestone you can actually measure.
- Then translate the game's real shaders offline (dump the shader cache from
  the data files, translate bytecode → GLSL → SPIR-V, embed as built-ins —
  the NFSU2-recompiled approach), and light up real draws.

## 6. The oracle: per-function differential testing (this toolkit)
- `runner-difftest.c`: links every lifted function (X-macro table), takes a
  binary state blob (GPRs, flags, x87, SSE, guest memory windows) on stdin,
  executes ONE function, dumps the resulting state.
- `difftest-sc2.py`: generates seeded random states, replays the ORIGINAL
  bytes of the same function in **Unicorn** (the hardware reference) until
  the frame's `ret`, simulating the return so ESP matches the lifted model;
  diffs every register, the arithmetic flags, and every byte Unicorn wrote.
- Functions whose flow leaves `.text` (import calls) are skipped by design —
  deterministic comparisons only.
- Run it as a continuous campaign (one report JSON per batch); classify the
  failures (convention artifacts vs real semantic divergences) and bisect the
  real ones per-instruction.
- Known convention artifacts to mask: DF is not modelled by the runtime;
  AF after fcompi diverges benignly; only compare CF/PF/ZF/SF/OF (0x8C5).

## 7. Android packaging
- Cross-compile the runtime with the NDK (aarch64-linux-android, clang),
  link `libvulkan.so`, wrap in a NativeActivity (minSdk 26, arm64-v8a),
  full-screen landscape. Ship the data via `adb push` to app storage; point
  the file layer at it (one environment variable).
- Touch → mouse for v1; gamepad/DeX later. Keep the exit path clean (your
  runtime should dump its BILAN/summary on quit — invaluable for field logs).

## 8. Cadence and supervision
- Two heavyweight agent sessions in parallel max per machine; a heartbeat
  cron that wakes when NOTHING advances (idle-stable is a stall, not peace);
  a written coordination bus between agent sessions; milestones posted with
  proof (trace excerpts, pixel counts, byte-exact rates).
- Lessons from the public wave (Heumann, NFSU2): the oracle comes BEFORE the
  volume; throwing away bad code is cheaper than fixing it; correctness
  outranks productivity.
