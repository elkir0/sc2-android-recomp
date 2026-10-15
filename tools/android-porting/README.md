# Android Porting Kit (bionic) — fibres & arena-delta helpers

Original support code for running a **statically recompiled x86 game runtime**
inside an Android app process (NDK r28, arm64-v8a). Generic: no game content.

| File | Role |
|---|---|
| `sc2_ucontext_aarch64.S` + `sc2_ucontext.c` + `sc2_ucontext_offsets.h` | User-space context switching (makecontext/swapcontext equivalents) for **bionic**, which ships no `<ucontext.h>`: assembly save/restore of callee-saved regs + stack pivot, driven by a `ucontext_t`-layout struct. Used to host the recomp runtime's fibers on Android. |
| `cpu_delta.cmake` | CMake function that **generates at configure time** a patched copy of the recomp runtime's `cpu.h` whose 32-bit atomics (`lock xadd`, `lock cmpxchg`, `xchg`) go through the guest-pointer macro (`GPTR`) instead of raw addresses — required as soon as the guest arena is *relocated* (delta mode) instead of fixed-mapped. Self-checking: warns if the expected 3 patch sites are not found. |
| `sc2_vk_presente.c` + `sc2_vk_presente.h` | **GPU swapchain presenter**: link-time interposition (`--wrap=dlsym`) on the Vulkan loader that adds `VK_KHR_surface` + `VK_KHR_android_surface` (phone) / `VK_KHR_xcb_surface` (headless bench) to the instance, creates a swapchain on the app's `ANativeWindow` with its own present queue, and blits the engine's back buffer into it (GPU scaling, aspect-ratio preserved, black bars, FIFO). Emits `[VKP]` logcat diagnostics. No game content, no engine code touched — pure Vulkan glue. |

## Note: `VK_KHR_push_descriptor` on emulators

The D3D9→Vulkan backend's *real-draw* path currently requires
`VK_KHR_push_descriptor`. Desktop Lavapipe (Mesa ≥ 22) and Adreno GPUs expose it;
**neither emulator GPU does** (SwiftShader software *and* host-Apple-M4 through
the emulator's Vulkan translation layer both omit it), so on an emulator the
engine falls back to a degenerate pipeline and the screen stays black while the
engine itself runs fine. A classic-descriptor fallback (pool + sets + updates)
is the known fix. Verified on an Android 14 arm64 emulator (Apple M4 host):
engine boots to the game main loop, 1,800+ frames presented, game data streamed.


## Why "delta mode"?

The recompiled runtime expects its guest memory at a fixed virtual address. A
fixed mapping is impossible inside an Android app process (the ART heap and
ASLR occupy the low 4 GB), so the arena is allocated anywhere **4 GB-aligned**
and every guest access goes through `GPTR(x) = (uint8_t*)(x) + g_host_delta`.
`cpu_delta.cmake` exists because the stock atomics bypassed that macro.

## Licence — MIT. Original work, no game content.
