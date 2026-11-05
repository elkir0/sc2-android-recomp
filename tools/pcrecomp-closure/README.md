# pcrecomp-closure — extend an existing lifted closure without a global relift

Generic, engine-agnostic (zero game constant, zero address, no game file shipped).

**EN:** A whole-program lift with pcrecomp's `lift32_cpu` produces a closure: every function reachable from the
roots it was given. When the runtime later hits a function outside that closure (a `RECOMP_TODO`, an indirect target
discovered at run time), relifting the whole program is slow and churns every generated file. `closure-plus.py`
lifts only what is missing: starting from the roots you pass, it lifts each function, scans the generated C for
direct `dispatch(c, 0x…u)` targets, and recurses into every target that lies in `.text` and is not already in your
known list. Function extent = bounded descent of local jumps (stops at `ret`/`jmp`/`int3`/`ud2`, 16 KiB cap); the
PE base relocations are fed to the lifter so absolute addresses stay relocatable. A function the lifter refuses
becomes a `RECOMP_TODO` stub instead of aborting the run.

**FR :** Un lift global avec `lift32_cpu` (pcrecomp) produit une closure : toutes les fonctions atteignables depuis
ses racines. Quand le runtime tombe ensuite sur une fonction hors closure, relifter tout le programme est lent et
modifie tous les fichiers générés. `closure-plus.py` ne lifte que le manquant : depuis les racines données, il lifte
chaque fonction, relève dans le C produit les cibles directes `dispatch(c, 0x…u)` et descend récursivement dans
chacune si elle est dans `.text` et absente de la liste connue. Une fonction refusée par le lifter devient un
stub `RECOMP_TODO` au lieu d'interrompre le run.

```sh
closure-plus.py [--include H]... [--known-re RE] <pcrecomp>/tools/lift <your-image.exe> <known-list> <out.c> <root-hex>...
```

| Argument | Role / Rôle |
|---|---|
| `tools/lift` dir | the pcrecomp folder holding `lift32_cpu.py` (apply `tools/pcrecomp-*` patches first if you use them) |
| image | **your own** PE32 image, read locally — never distributed / **votre** image PE32, lue localement |
| known list | text file of already-lifted functions; every VA matched by `--known-re` (default `X\(([0-9A-Fa-f]{8})\)`, the pcrecomp list X-macro) is skipped; `-` = none |
| `--include H` | header written at the top of `out.c` (repeatable; default `cpu.h`) — add your project's lifted-functions header |
| roots | hex VAs to start from |

stdout: the space-separated VAs that were added (feed them to your function list / dispatch table);
stderr: one line per function (size, direct targets, new ones, lift failure).

Requires: Python 3, Capstone (already a pcrecomp dependency). Checked on a hand-built synthetic PE32
(3 functions, call chain, one pre-known): 2 new functions lifted, the known one skipped, recursion followed.
