# pcrecomp-mmx — MMX lifting (mm0-mm7 as 64-bit CPU fields)

Generic lifter patch, engine-agnostic (zero game constant, zero address).
MMX instructions used to abort the lift of a whole function
(`not an xmm operand: mmN`). mm0-mm7 now lift as their own `uint64_t` CPU
fields (appended last in `cpu.h` so no existing offset moves); each
operation is a `cpu.h` helper on 64-bit values with lanes through a
little-endian union:

movd/movq/movntq, padd/psub b/w/d/q (+ saturating padds/paddus/psubs/psubus),
pmullw/pmulhw/pmulhuw/pmaddwd/pmuludq, pmaxsw/pminsw/pmaxub/pminub,
pavgb/pavgw/psadbw, pcmpeq/pcmpgt b/w/d, punpckl/h bw/wd/dq,
packsswb/packuswb/packssdw, pand/pandn/por/pxor,
psll/psrl/psra (imm or mm/m64 count), pshufw, pmovmskb, pextrw/pinsrw, emms.

## Files / Fichiers

- `0001-base….patch` — shared local lifter base (movhps/movhpd high lane,
  FPU control word…), same base as `tools/pcrecomp-movhps/`
- `0002-lift32_cpu-MMX-support….patch` — the MMX support itself

Apply in order: `git am 0001* 0002*` on a clean pcrecomp tree.

## Proof (private bench — none of it is in this repo)

Per-function equivalence against a hardware-reference emulator: 30/30 on the
targeted function class (307-3669 instrs, bytes written identical);
200-function control run: 0 regression; the unlocked YCbCr 4:2:0 -> BGRA
conversion function now executes in-game (full arcade mini-game playable
end to end, mission-load path unchanged).
