# Notes for agents

You are a coding agent working in `sc2-android-recomp`: the original runtime, tools and method
used to statically recompile StarCraft II (x86-32 Windows, D3D9) to native ARM64 on macOS Apple
Silicon and Android. This file tells you what the repository covers, the rules you must keep, and
which guide to open for your task. Read it whole before your first change.

## Scope

The person you work for owns a legitimate copy of the game, installed on their machine. This
repository gives you the parts that are original work and the method around them.

**Covered here**

- the host runtime: the Win32 layer, memory, threads and fibers, files and saves, audio, video
  (`runtime/`, fast tests in `tests-hote/`);
- the fake D3D9 device and its Vulkan backend, and the offline shader translation chain
  (`runtime/`, `tools/d3d9-shaders/`);
- the generic patches for the pcrecomp lifter, the lift tools and the per-function oracle
  (`tools/pcrecomp-*/`, `tools/lift/`, `tools/difftest/`);
- the macOS host (`mac/`) and the Android port (`android/`);
- the benches, event-driven scenarios and non-regression (`scripts/`);
- the method and the measured results (`docs/`).

**Not covered, and not accepted in contributions**

- obtaining or preparing the game executable for analysis, and anything about protections
  applied to it;
- launching the game: the publisher's launcher or client, its launch and licence checks, any way
  of starting the game without them, and the game's local APIs (the title-screen menus of our own
  hosts in `mac/` and `android/` are in scope);
- licensing, accounts, Battle.net, authentication, online or multiplayer play;
- any game content: code, lifted or decompiled code, assets, shader bytecode or its
  translations, saves, maps, file hashes, bulk screenshots.

So this toolkit does not turn an installed game into a running port by itself. If your task needs
one of the excluded steps, stop and tell the person you work for that it is outside what this
repository provides. Do not write it, do not search for it, do not suggest where to find it.

## Hard rules

1. **Everything derived from the game stays on the user's machine, outside this clone**: lifted C,
   extracted archives, shader caches and SPIR-V packs, builds, APKs, `.app` bundles, captures,
   logs that quote game data. Never `git add` them. [LEGAL.md](LEGAL.md) is the policy; the
   `legal-scan` CI fails on what it can detect, and violations are reverted.
2. **Never publish or share a built APK or `.app`**: they contain recompiled game code.
3. **No personal data** in commits, issues or logs you paste: home paths, IP addresses, e-mail
   addresses, device serials. `scripts/rapport-bug.py` scrubs a report for you.
4. **Commit only when the person you work for asks.** One change per pull request.
5. **Win32 answers must be true.** When the runtime cannot do something, refuse with the
   documented Windows error rather than invent success: every lie surfaced later as a harder bug
   ([docs/CLIENT-5.0.md](docs/CLIENT-5.0.md), [docs/D3D9-PORTING-GUIDE.md](docs/D3D9-PORTING-GUIDE.md) §4).
6. **Measure, don't guess.** A performance or rendering claim needs an A/B/A run at the plateau,
   or a counter, or a frame fingerprint compared by script
   ([docs/agents/06-bench-and-regress.md](docs/agents/06-bench-and-regress.md)).
7. **Fix the cause, generally.** A fix that only holds for one mission, one frame or one machine
   needs a written reason.
8. **Every `SC2_*` environment variable read by the code is documented** in
   [docs/VARIABLES.md](docs/VARIABLES.md); `scripts/variables-sc2.sh verifier` checks it.

## Where to start

| Your task | Open |
|---|---|
| First clone: toolchains, the verification gate | [docs/agents/01-setup.md](docs/agents/01-setup.md) |
| Host runtime: Win32 layer, memory, threads, files, audio, video | [docs/agents/02-runtime.md](docs/agents/02-runtime.md) |
| Rendering: D3D9 device, Vulkan backend, shaders, caps | [docs/agents/03-rendering.md](docs/agents/03-rendering.md) |
| Lifter patches, lift tools, an oracle divergence | [docs/agents/04-lift-and-oracle.md](docs/agents/04-lift-and-oracle.md) |
| macOS host or Android port | [docs/agents/05-hosts.md](docs/agents/05-hosts.md) |
| Measure performance, run a bench, non-regression | [docs/agents/06-bench-and-regress.md](docs/agents/06-bench-and-regress.md) |
| You see an error, a wrong image, a hang, a slow scene | [docs/agents/symptoms.md](docs/agents/symptoms.md) — grep it first |
| Port another D3D9-era game with this method | [docs/D3D9-PORTING-GUIDE.md](docs/D3D9-PORTING-GUIDE.md) |
| What changed for the 5.0.17 client | [docs/CLIENT-5.0.md](docs/CLIENT-5.0.md) |
| Report a bug, open a pull request | [CONTRIBUTING.md](CONTRIBUTING.md) |

The guide index, with the reading order, is [docs/agents/README.md](docs/agents/README.md).

## Before you hand your work back

1. `scripts/verifier.sh` — the gate: host tests without the lifted code, Python unit tests, shell
   syntax, bench judges on fixtures. Under two minutes, no game data, no phone needed (checks that
   need one report SKIP). Use `scripts/verifier.sh --rapide` while iterating; the gate is the
   command without option.
2. After editing documentation: `python3 -m unittest scripts/test_docs.py` (word limits, links).
3. Before a push: the `legal-scan` workflow must pass. You can replay it locally by running the
   `run:` block of `.github/workflows/legal-scan.yml` from the repository root.
4. Report what you verified and what you could not, with the command output. "Should work" is not
   a result.

## Conventions

- Code comments and several method documents are in French; the agent guides and this file are in
  English. Keep the language of the file you edit.
- Every script documents its usage, options and exit codes in its header comment: read it before
  running the script, and keep it current when you change the script.
- Scripts and tests keep some paths of the private working tree (`recomp-sc2/runtime` there is
  `runtime/` here); everything they read from the game comes from the user's install, located
  through the variables in [docs/VARIABLES.md](docs/VARIABLES.md).
- Word limits: this file stays under 1,500 words, each guide in `docs/agents/` under 6,000
  (`scripts/test_docs.py` enforces both). Put detail in the guides, not here.
- Add a row to [docs/agents/symptoms.md](docs/agents/symptoms.md) when you solve a problem another
  agent could hit again, with a link to where the fix is explained.
