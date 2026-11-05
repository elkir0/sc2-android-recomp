# Agent guides

One guide per task. Each one starts with its purpose, gives numbered steps grounded in the scripts
of this repository, and ends with a **Verify** section you can run. Read [AGENTS.md](../../AGENTS.md)
first: it sets the scope and the hard rules that every guide assumes.

| # | Guide | Use it to |
|---|---|---|
| 1 | [01-setup.md](01-setup.md) | install the toolchains for macOS, Linux and Android, and get `scripts/verifier.sh` green |
| 2 | [02-runtime.md](02-runtime.md) | find your way in `runtime/`, keep the Win32 layer truthful, add a host test |
| 3 | [03-rendering.md](03-rendering.md) | work on the fake D3D9 device, the Vulkan backend and the offline shader chain |
| 4 | [04-lift-and-oracle.md](04-lift-and-oracle.md) | apply the lifter patches, use the lift tools, classify an oracle divergence |
| 5 | [05-hosts.md](05-hosts.md) | build and test the macOS host and the Android port |
| 6 | [06-bench-and-regress.md](06-bench-and-regress.md) | measure, run the benches and the two-level non-regression |
| — | [symptoms.md](symptoms.md) | go from an observed symptom to its known cause and fix |

## Reading order

- **New to the repository:** 1, then the guide of your task.
- **Debugging:** grep [symptoms.md](symptoms.md) for the error text or the effect you see, open the
  reference it gives, then the guide of that subsystem.
- **Porting another D3D9-era game:** [../D3D9-PORTING-GUIDE.md](../D3D9-PORTING-GUIDE.md) is the
  recipe; guides 2 to 6 show how each step was tooled here.

## Background documents

The guides summarise and link these; open them when a guide points you there.

| Document | Content |
|---|---|
| [../METHOD.md](../METHOD.md) | the pipeline in one page |
| [../D3D9-PORTING-GUIDE.md](../D3D9-PORTING-GUIDE.md) | the full recipe, with effort calibration and a pitfall checklist |
| [../CLIENT-5.0.md](../CLIENT-5.0.md) | the 5.0.17 client: results, discoveries, performance levers |
| [../RENDU-D3D9-VERS-VULKAN.md](../RENDU-D3D9-VERS-VULKAN.md) | the D3D9→Vulkan translation method (French) |
| [../RETROSPECTIVE-RECOMPILATION.md](../RETROSPECTIVE-RECOMPILATION.md) | difficulties, measurement protocol, dead ends (French) |
| [../BANC-SCENARIOS.md](../BANC-SCENARIOS.md), [../BANC-MAC.md](../BANC-MAC.md) | benches and the native macOS bench (French) |
| [../CAPS.md](../CAPS.md), [../PACK-SPIRV.md](../PACK-SPIRV.md), [../VARIABLES.md](../VARIABLES.md) | device caps, SPIR-V pack format, environment variables (French) |

## Keeping the guides useful

- Each guide stays under 6,000 words; `scripts/test_docs.py` checks it, and checks that every
  guide is listed in this index and that every relative link resolves.
- When a script changes its options, update the guide that quotes it in the same pull request.
- Nothing in a guide may cross the scope set in [AGENTS.md](../../AGENTS.md#scope).
