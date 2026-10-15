# Android Porting Kit (bionic) — fibres & arena-delta helpers

Original support code for running a **statically recompiled x86 game runtime**
inside an Android app process (NDK r28, arm64-v8a). Generic: no game content.

| File | Role |
|---|---|
| `sc2_ucontext_aarch64.S` + `sc2_ucontext.c` + `sc2_ucontext_offsets.h` | User-space context switching (makecontext/swapcontext equivalents) for **bionic**, which ships no `<ucontext.h>`: assembly save/restore of callee-saved regs + stack pivot, driven by a `ucontext_t`-layout struct. Used to host the recomp runtime's fibers on Android. |
| `cpu_delta.cmake` | CMake function that **generates at configure time** a patched copy of the recomp runtime's `cpu.h` whose 32-bit atomics (`lock xadd`, `lock cmpxchg`, `xchg`) go through the guest-pointer macro (`GPTR`) instead of raw addresses — required as soon as the guest arena is *relocated* (delta mode) instead of fixed-mapped. Self-checking: warns if the expected 3 patch sites are not found. |

## Why "delta mode"?

The recompiled runtime expects its guest memory at a fixed virtual address. A
fixed mapping is impossible inside an Android app process (the ART heap and
ASLR occupy the low 4 GB), so the arena is allocated anywhere **4 GB-aligned**
and every guest access goes through `GPTR(x) = (uint8_t*)(x) + g_host_delta`.
`cpu_delta.cmake` exists because the stock atomics bypassed that macro.

## Licence — MIT. Original work, no game content.
