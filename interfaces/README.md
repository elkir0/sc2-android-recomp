# src/lifted/ — code lifté de SC2.exe (build 15405), passe 4

**Généré, ne pas éditer, hors git** (≈ 420 Mo de C : `recomp_*.c` et `recomp_sc2_list.h` sont
ignorés). Seuls `LIFT-REPORT.json`, `sc2_imports.h` et `sc2_image.h` sont suivis. Pour régénérer (lift-boot pose `PCRECOMP_FPU_CW=1` depuis la passe 7k : formes x87 à mot de contrôle)
(≈ 2,5 min, puis ≈ 100 s de compilation à `-j4`) :

```sh
ulimit -v 8000000
~/code/reverse/sc2-partout/.venv/bin/python scripts/lift-boot.py \
    --roots-extra recomp-sc2/lift-roots-runs.txt --init-offset 2 \
    --max-funcs 100000 --max-mb 1000 --roots-reloc
```

| fichier | contenu |
|---|---|
| `recomp_NN[_k].c` | fonctions `void L_XXXXXXXX(CPU *c)`, émises par `~/tools/pcrecomp/tools/lift/lift32_cpu.py` (lifter CPU-struct importé comme module ; **patché en passe 7i** pour l'atomicité de `xchg` mémoire, `lock …` et `cmpxchg8b` — voir `~/tools/pcrecomp/NOTES-SC2.md`) ; une plage de VA de 256 Ko par `NN`, subdivisée en `_k` au-delà de 4 Mo (`--file-mb`) |
| `recomp_sc2_list.h` | X-macro `LIFTED_FUNCS(X)` : la table de dispatch de `runtime/sc2_runtime.c` en est construite |
| `sc2_imports.h` | les 755 slots IAT (VA, DLL, nom ou `DLL#ordinal`) → stubs de `runtime/sc2_iat.c` / `sc2_iat_p4.c` |
| `sc2_image.h` | géométrie du PE : base, taille, entry, sections, répertoire TLS |
| `LIFT-REPORT.json` | racines, profondeurs, borne, origine des bornes, échecs de lift, setjmp réécrits |

## Ce que la passe 4 a changé (et pourquoi)

La passe 3 était bornée à 20 Mo (≈ 2 000 fonctions) et évaluait la fermeture complète à
« 18 136 fonctions / 219 Mo ». La passe 4 lifte **tout ce que le boot peut atteindre**, appels
indirects compris : **86 650 fonctions, 420 Mo de C, fermeture complète** (aucune borne atteinte).

1. **Étendue par descente** (`etendue_descente`). Une cible sans borne de catalogue (type C) ou une
   entrée *alias* du catalogue B (au milieu d'une fonction, taille = jusqu'à la fin de
   l'englobante) était liftée jusqu'à l'entrée suivante : les ~2 400 funclets SEH de 0x01405xxx
   pesaient 82 Mo de C quasi dupliqué, les alias des bras de switch de 0x010A1710 ≈ 600 Mo. On ne
   garde plus que les octets atteignables depuis la cible (capstone, descente récursive, fenêtre
   `--alias-max` 2 Ko) : 220 → 88 Mo pour la fermeture directe.
2. **Racines relogées** (`--roots-reloc`). Les cibles d'appels *indirects* (vtables, callbacks,
   pointeurs de fonction posés par le code) ne sont pas des arêtes `dispatch(c, 0x…)`. Toute valeur
   relogée qui tombe sur une entrée de catalogue (vtables de .rdata/.data), ou — hors de toute
   fonction connue — sur une adresse alignée sur 16 précédée de bourrage (CC/90) ou d'un `ret`
   (0x008DF800, 0x00EFC940, 0x009AA980 : absents des deux catalogues) devient racine : 29 821.
3. **Fonctions non liftables** : `lift32_cpu` lève une exception sur les registres MMX (`mm4`) ;
   la fonction est alors émise avec un corps `RECOMP_TODO(...)` qui arrête le run en se nommant
   (1 cas : 0x00C72900, jamais exécuté — `cpuid` annonce un CPU sans MMX).
4. **setjmp** : chaque appel à `_setjmp3` (14, via le thunk 0x013470A4) est réécrit en
   `SC2_SETJMP3(c, retour)` : le `sigsetjmp` hôte doit avoir lieu dans le cadre C de la fonction
   liftée appelante pour qu'un `longjmp` invité y revienne (voir `runtime/sc2_lifted.h`).

**Bornes des fonctions** : catalogue disasm32 patché (B, `start`) ; à défaut Ghidra (A) ; alias B
et cibles sans borne : fenêtre 2 Ko + descente (voir 1). `origine_bornes` dans le rapport.
