# D3D9 Shader Bytecode → SPIR-V (offline translator)

Generic toolchain to translate **Direct3D 9 shader model 2.0/3.0 bytecode** into
**SPIR-V**, offline. Written for a static-recompilation project; contains no game
content — it operates on data each user supplies from their own copy.

## Components

| File | Role |
|---|---|
| `d3d9sm.py` | SM2/3 bytecode decoder: version, instructions, dcl/def/defi/defb, source/dest parameters (mask, swizzle, modifiers, relative addressing), CTAB. |
| `d3d9-vers-spirv.py` | Full translator: bytecode → GLSL 450 → `glslangValidator` → `spirv-val` → embedded SPIR-V table indexed by FNV-1a 64 of the bytecode. |
| `extraire-shaders.py` | Extractor for precompiled shader caches: scans any blob for `vs_/ps_` version tokens, validates instruction-by-instruction, dedupes by hash, emits an index TSV. |
| `ppm-blocs.py` | PPM frame metric: 16×16 block averages, counts distinct blocks (render-regression detector). |
| `ppm2png.py` | PPM (P6) → PNG in pure stdlib (`zlib` + `struct`, optional brightness gain) — dependency-free proof captures from bench runs. |

## Usage

```sh
# 1) extract distinct bytecodes from a precompiled shader cache blob
python3 extraire-shaders.py <cache.bin> out-dir/

# 2) translate every dumped shader and build the embedded table
python3 d3d9-vers-spirv.py out-dir/boot out-dir/sc2_shaders_spv.h --glsl /tmp/glsl

# 3) measure a rendered frame (how many distinct 16x16 blocks?)
python3 ppm-blocs.py frame_00010.ppm
```

Dependencies: Python 3 (stdlib only) + `glslangValidator` (glslang-tools) +
`spirv-val` (spirv-tools).

## Proven results (on our own copy, headless Vulkan/Lavapipe)

- 75,797 distinct bytecodes extracted from a 141 MB precompiled cache (91% of the
  full cache decoded to GLSL);
- 202/202 boot shaders translated and **validated by `spirv-val`**;
- 5,771 real draws executed with those SPIR-V modules — the game's title screen
  renders on the recompiled runtime (2,570 distinct 16×16 blocks on frame 10 vs
  1 flat black before).

## Licence — MIT. Original work, no game content.
