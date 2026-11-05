# pcrecomp-movhps — correctif lifter : `movhps`/`movhpd` chargeaient la voie BASSE

**EN:** One-character fix to the pcrecomp CPU lifter (`tools/lift/lift32_cpu.py`):
the mnemonic parser chose the SSE lane by testing the wrong character of the
mnemonic (`m[4]`), so `movhps`/`movhpd` (load into the **high** 64-bit lane)
were lifted exactly like `movlps`/`movlpd` (low lane).

**FR:** Correctif d'un caractère au lifter CPU de pcrecomp (`tools/lift/lift32_cpu.py`) :
le parseur de mnémoniques choisissait la voie SSE en testant le mauvais caractère
du mnémonique (`m[4]`), donc `movhps`/`movhpd` (chargement dans la voie **haute**
de 64 bits) étaient liftés exactement comme `movlps`/`movlpd` (voie basse).

## Symptôme / Symptom

**EN:** Any engine that multiplies 4×4 matrices with `movhps`-based SSE code gets
**degenerate view-projection matrices** (rows duplicated as `a,b,a,b`) — every
3D triangle collapses, the 3D scene renders as a black screen while 2D/UI draws
still work. On the showcase engine, all affected sites sat in the 4×4 matrix
multiply routine; the 3D menu scene went from 100 % black frames to fully
rendered (ship over a ringed planet) after the fix.

**FR:** Tout moteur qui multiplie des matrices 4×4 via du SSE construit sur
`movhps` obtient des **matrices de vue-projection dégénérées** (lignes
dupliquées en `a,b,a,b`) — tous les triangles 3D s'effondrent, la scène 3D
s'affiche en écran noir tandis que les draws 2D/UI fonctionnent. Sur le moteur
de démonstration, tous les sites touchés étaient dans la routine de produit de
matrices 4×4 ; la scène 3D du menu est passée de 100 % de frames noires à
entièrement rendue (vaisseau au-dessus d'une planète à anneaux) après correctif.

## Détection générique / Generic detection

**EN:** `grep -n 'm\[4\] == "h"' lift32_cpu.py` — any hit means the bug is present.

**FR:** `grep -n 'm\[4\] == "h"' lift32_cpu.py` — toute occurrence = le bug est présent.

## Application

    git apply pcrecomp-movhps.patch

Patch applies to the stock pcrecomp lifter; zero constants of any particular
binary (fully generic). Verified per-function against a hardware-reference
emulator (Unicorn) before/after; unaffected encodings (`movhlps`/`movlhps`,
register-to-register) unchanged.
