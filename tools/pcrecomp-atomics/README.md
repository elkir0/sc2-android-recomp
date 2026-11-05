# pcrecomp-atomics — atomic x86 → C lifting (xchg with memory, lock RMW, cmpxchg8b)

Generic patch for the pcrecomp static-recompilation toolchain: it teaches the
x86 lifter to emit C atomics (`__atomic_*` builtins) instead of plain
load/store pairs for the instructions whose hardware semantics are atomic:

- `xchg` with a memory operand (8/16/32-bit) — atomic with or without `lock`;
- `lock add|sub|and|or|xor|inc|dec` — read-modify-write, flags computed from
  the old value;
- `lock xadd` and `cmpxchg` 8/16-bit;
- `cmpxchg8b` — 64-bit compare-and-swap (previously a TODO).

The runtime side gains a single `GPTR(va)` macro (host pointer for a guest
address — identity by default; a host that maps the guest at an offset
defines its own before including `cpu.h`).

**Generic by construction**: the patch carries no address, offset, symbol or
any other constant of any specific binary — it applies to any pcrecomp
userland-lifting workload.
md5 of the patch: `191282f52defbf3a5c14e1e2596864dc`.

## Apply

    patch -p1 -d <your-pcrecomp-checkout> < pcrecomp-atomics.patch

Then re-lift your target and diff the generated C: only the sites listed
above should change. Validate with your usual differential harness before
trusting a re-lift (we check equivalence against a hardware-reference
emulator on both x86-64 and aarch64 hosts — 20/20 on every function touched).
