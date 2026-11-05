# The current client — StarCraft II 5.0.17 on ARM64

The first weeks of this project ported the 2010 Wings of Liberty client (build 15405). Since late
October the same method runs the **current client, 5.0.17 (build 98310)**, natively on Apple
Silicon and as an Android APK, from our own legitimate copy. Everything derived from the game
(lifted code, shaders, data, binaries) stays on our machine. This page records what changed, what
we learned and what we measured, up to **November 2, 2026**.

## Results

| | Current client (5.0.17) |
|---|---|
| Functions lifted to portable C | **609,384** (×7 the 2010 client), Mac binary 467 MB |
| Whole campaign | **Wings of Liberty in one process**, from empty campaign banks: bar, 21 missions, the Haven choice, Char, All In, ending cinematic, credits — 0 fault, 0 stop (wins forced with the game's own single-player cheat to keep the run to about 2 h) |
| Bench, 29 missions | **29 / 29**, 0 fault, 0 stop, 0 draw without a translated shader; in-game **32–105 FPS, median 57.8** on an Apple M4 (38.8 the day before: +49 %) |
| Real play | **Liberation Day and The Outlaws won by a scripted player with real orders**, no cheat (The Outlaws: 23 min, workers on minerals, control groups, barracks, depots, attack waves). Zero Hour: lost twice — the game's DEFEAT screen and *Load Last Checkpoint* both work |
| Endurance | 2 h 05 in one process (11 replays of a mission), 0 fault, guest arena 1,161 → 1,216 MB, 0.81 s of audio lost over 603 ten-second windows |
| Saves | save and load from the in-game menu |
| Shaders | **13,398** translated and validated locally, across Low, Medium, High and Ultra |
| Hosts | macOS window (Metal through MoltenVK, CoreAudio, real mouse and keyboard, fullscreen); Android APK with gestures — so far on the Android emulator only, the Galaxy Z Fold 8 measurement is next |

## What carried over unchanged

The D3D9-on-Vulkan backend of this repository was taken **as is**: the first frame of the 5.0
client came up with it, and the 463 shaders of that first screen were translated and validated in
4.7 s by the same translator. The input module, the bench command channel, the ICU number
formatting and the software audio path also carried over. The engine changed a lot in seven
years; the Direct3D 9 contract did not. That is the strongest argument for the method: **the
expensive parts are written against the API, not against the game.**

## What was new, and what we learned

### 1. Threads: one host thread, deterministic order

The 5.0 client starts up to 23 threads. They all run on **one host thread**: each guest thread is
a fiber (guest CPU state, thread block, guest stack, native stack), switched at every wait and
after a quantum of dispatches. Waits use virtual time (1 ms per quantum, jump to the first deadline
when every fiber is blocked), so the order does not depend on the clock: two runs print the same
thread and import sequence line by line, and the boot is identical at quanta of 7, 200 and
200,000. Real critical sections, events, semaphores, mutexes and condition variables are
implemented in guest memory; a deadlock stops the run with the list of fibers. A dedicated suite
runs 3,084 checks.

One host thread also makes every `lock`-prefixed instruction exact as a plain read-modify-write
(5,616 `xchg`, 586 fetch-and-add, 173 compare-and-swap in the lift). That fixed a real ARM64
crash: `lock cmpxchg8b` on a 4-byte-aligned address raised SIGBUS when translated to a native
atomic.

### 2. Win32 answers must be true — every lie surfaced later

The rule of the 2010 port, *refuse, don't lie*, held again. Each of these stubs looked harmless:

| Plausible answer | What it broke |
|---|---|
| `CPUID` returned the same values for every leaf | a 1.6-million-call spin at boot: leaf 4 never ended (`eax & 0x1F` never 0) |
| `GetVersionExW` filled 0x11C bytes into a 0x114-byte `OSVERSIONINFOW` | the caller's saved frame pointer |
| `FILETIME` counted from 1970 | `time()` went negative, `localtime` returned NULL, `strftime` raised an invalid-parameter fault (Windows counts from 1601) |
| `VirtualQuery` returned one allocation base for everything | the C runtime allocator refused **every** release: memory ran out at 2,955 MB; with true answers, 1,211 MB, stable |
| `WriteFile` / `ReadFile` ignored the `OVERLAPPED` offset | saves were written with the archive header at the wrong offset; fixing it made readable some maps that had been failing silently all along |
| `CreateDirectoryW`, `DeleteFileW` did not touch the disk; `GetFileAttributesW` called any missing path a directory | "Save Failed" |
| `MapVirtualKeyExW` returned its input | key labels shown as "Key/F1" |
| `InitCommonControlsEx` returned 0 | a progress callback read it as *Cancel* |
| `GetCursorPos` returned the screen centre, `GetCursorInfo` failed | middle-button camera panning did nothing (the game measures the drag from the cursor position) |

The last one was found on the last night, after the whole campaign had already been played:
a stub can hide for days when the feature that needs it is never exercised.

### 3. Calling conventions and import names

Every import of this build was named from its call sites, and **a name is only a hypothesis until
the site is re-read**. Several were wrong for days: `WriteFile` and `ReadFile` swapped; a
three-argument `CreateMutexA` taken for a two-argument function; `HeapSize` taken for
`HeapAlloc` — the pointer passed as a size failed silently while the heap was bounded, then ate
512 MB per call once large blocks moved to the arena.

- **An unknown arity must stop the run immediately.** A stdcall argument left on the stack shifts
  every register restore of the caller; the signature is unmistakable once known: `ebx ← edi`,
  `edi ← esi`, `esi ← the extra value`.
- **A stub of known arity that returns 0 does not stop anything.** Read the end-of-run summary of
  calls per import at every new front: one file-attribute query called 257 times without a handler
  was the whole cause of a storage failure.
- **When the game writes its own logs, read them first.** They gave the CPUID values, the paths it
  probes and its error codes in plain text.

### 4. Memory

- Allocations climbed into the thread stacks before the guest address space was re-laid out
  (heap, arena, thread stacks in separate ranges); the first symptom was a NULL frame pointer on a
  return, far from the cause.
- The 5.0 client is *large address aware*: 1.2 GB committed for 593 MB allocated during the first
  mission. The guest space now extends to 0xF0000000.
- Leaks are found by counting live blocks **per caller**, never by lowering settings: the refused
  releases of §2 looked exactly like real consumption.

### 5. Rendering

- **Multiple render targets.** The backend bound only the first target while the game draws
  thousands of times to 2 to 4 targets: depth and normals were never written, depth-of-field blurred
  everything, scenes looked flat. With all targets attached, cleared and masked, portraits and
  cinematics are correct.
- **Shader coverage per quality level.** The pack covered only the levels that had been played.
  Switching to Medium in the in-game options gave a black world: 1.6 million draws without a
  translated shader. Harvesting each quality level over the 29 missions took the pack from 8,215 to
  13,398 shaders. *Lesson: a 0-missing-shader bench proves coverage for its own settings only.*
- **Clip plane.** The game enables a single user clip plane; it now reaches every vertex shader as
  `gl_ClipDistance[0] = dot(position, plane)`, with the plane as a push constant. No visible
  difference has been spotted yet on the scenes we checked.
- **Presentation on macOS.** MoltenVK encodes everything inside `vkQueueSubmit`; asynchronous queue
  submits took the present path from 10.6 % to 2.1 % of the game thread.

### 6. Audio

The engine's FMOD runs unmodified on a DirectSound we provide; the host callback (CoreAudio or
AAudio) reads the guest buffer directly.

- FMOD's enumeration callback ignores a device with a null GUID: announce a real device too.
- The mixer thread needs wall-clock time; the virtual time of the scheduler starved it.
- After a stall longer than half the buffer, the write cursor must be re-synchronised to FMOD's own
  phase, and after a stall close to a whole buffer, caught up by one full turn — otherwise the
  output stays silent until the next buffer. Each case was found by a single silent run among dozens,
  because it depends on the length of the stall. The logic is now a pure function with a host test
  that replays both recorded silences and 100,000 random mixer steps.
- Open: under very heavy machine load (swapping), losses still cluster during loads, when the game
  thread stalls for seconds.

### 7. Performance — from 13–24 to 36–43 FPS on the heaviest missions

Measured in the order it paid, same scenes, A/B under a sampling profiler:

| Lever | Measured effect |
|---|---|
| Check the compile flags first | the first builds were at `-O0` |
| Direct-mapped dispatch cache (each computed call and jump did a binary search over 609k entries: **52 % of the main thread**) | 27 → 45 FPS on the first mission, boot 90 → 58 s |
| MMX resolved at decode instead of compared by name per instruction, then translated to direct C (247 instructions checked against the interpreter on 2,000 random states) | bar scene 3.5 → 19 FPS (decode once), then 17 → 79 (direct C); a heavy mission 10.8 → 29 |
| The lift passes of the 2010 port, never applied to this build: **direct calls** (1.73 million constant call sites), **guest registers and flags in local variables** (607,809 of 609,384 functions, x87 included, SSE above 16 uses) | about **+50 %**: four heavy scenes 12–24 → 22–37 FPS |
| Constant jumps as `musttail` calls (≈ 1.04 million): the host stack does not grow, chains stay flat | within noise on its own |
| No `volatile` guest accesses — safe because one host thread runs all lifted code | on a hot file: code −16 %, loads −48 % |
| Hot functions grouped at link time (9,450 functions from a profile); on Android, hidden visibility and `-Bsymbolic` so the lifted library calls itself without PLT or GOT | not measured separately |
| Asynchronous queue submits (macOS) | present 10.6 → 2.1 % of the game thread |

All together: one heavy scene 12–18 → 36–39 FPS, another 13–24 → 41–43; dispatch 13 % → 5 % of the
game thread; binary 555 → 467 MB, APK 654 → 537 MB. **Correctness of the passes is checked by a control build** that compares every local
register copy with the CPU structure at each boundary: 0 discrepancy over four missions. What
remains is the lifted code itself and, on macOS, descriptor updates.

### 8. Bench

- **A command channel on a live game** (a FIFO: click, right click, drag, wheel, middle button,
  keys, capture, quit) is worth ten restarts: missions are played, options changed and screens
  captured without relaunching.
- **A scripted player** issues real orders through it and judges the outcome by matching the
  game's own victory banner; it replays the Liberation Day win in about 5 minutes.
- **A campaign driver** walks the whole campaign in one process and records the campaign banks
  after each mission.
- **Bench isolation**: each bench gets its own profile copy and pipeline cache. A profile that is a
  symbolic link is not copied by a naive copy — two benches then share, and can wipe, the real one.

### 9. Pitfalls worth sharing

- `adb install -r` sent while the Android emulator was still booting failed, the package was
  reinstalled under a new uid, and Android wiped the app's files — 26 GB of locally pushed game data.
  Wait for `sys.boot_completed` and check `pm path` before installing.
- The work runs with AI coding workers (below). One of them ran `pkill -x sh` in its own tests and
  killed every shell of the user, including a 13-minute compile. Briefs now forbid `pkill` and
  `killall`. Another one's fake game took the real bench lock and silently queued every bench
  behind it: tests use their own lock.
- A helper script named `dis.py` shadowed Python's own `dis` module.

## How the work was done

A supervisor model (Claude Opus) does the diagnosis, the design and the review; well-specified
pieces (a lift pass, a host test, a gesture recogniser, an install script) go to GLM 5.3 coding
workers in separate worktrees. Nothing is accepted on a worker's word: the supervisor reads every
diff and re-runs its acceptance tests. On the last night (8 hours, unattended): 9 dispatches, 8
accepted, 1 rejected.

## Not in this document, and not in this repository

- **Battle.net sign-in and the client's licensing**: left untouched, and not described.
- **How the client is started on our machine**: not described.
- **The 5.0 runtime and its tools**: not published yet. They still mix original code with facts
  specific to this build; they will go through the same triage as the 2010 runtime before any
  publication ([LEGAL.md](../LEGAL.md)).
- **Lifted code, shaders, game data, binaries, saves**: never.
