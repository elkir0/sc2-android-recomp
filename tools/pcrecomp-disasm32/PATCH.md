# Patch disasm32.py — fin de `find_functions` qui crashait sur SC2.exe

Copie patchée de `~/tools/pcrecomp/tools/disasm/disasm32.py` (pcrecomp `904ac31`,
sha256 de l'original `a93bebd0c1c1ed009cca006c3ce1cf6d21df8fcdd8e2cfa06f9c68614a426206`).
**L'outillage commun `~/tools/pcrecomp` n'est pas modifié.** Diff : `disasm32.patch`
(+11 lignes, aucune suppression).

## Symptôme

Sur SC2.exe build 15405 (15,7 Mo), sous `ulimit -v 8000000` :

| run | lancement | issue | point de mort |
|---|---|---|---|
| 1 | CLI `--min-size 32` (02:09) | `MemoryError` | juste après « Added 441 branch targets » |
| 2 | CLI `--min-size 32` (02:35) | SIGSEGV rc=139 | idem |
| 3 | CLI `--min-size 64` (09:48) | SIGSEGV rc=139 | idem |
| 4 | wrapper `export-catalogue-lots.py` (10:09) | SIGSEGV | idem, `lots/` resté vide |

## Cause (prouvée)

1. **Le lieu.** La trace du run 1 (`extraction/pcrecomp/disasm32.log`, l. 100-125) meurt dans
   la **dernière boucle de `find_functions`** :
   `disasm32.py:1028 _add_func(t, "alias")` → `disassemble_function` → `decode` → capstone
   `CsInsn.__init__` → `ctypes.pointer(all_info.detail._type_())` → `MemoryError`.
   « Added 441 » est le 3e et dernier tour de `close_dispatch_targets` (`max_rounds=3`). Le crash
   suit donc son retour, dans la boucle qui décode les 3753+3524+441 = **7 718 cibles alias**.
2. **Le mécanisme.** Un run de diagnostic du module NON patché, avec `-X faulthandler` et un
   relevé de `/proc/PID/status` toutes les 10 s, a donné :
   - de 10:45 à 10:53, plateau à **7 333 888 kio** (fin de la découverte, `clamp_extents`,
     `close_dispatch_targets`) ;
   - de 10:53:38 à 10:54:38, **+650 Mo** jusqu'à **VmPeak = 7 975 696 kio**, pour une limite
     `ulimit -v` de 8 000 000 kio : **24 Mo de marge**.

   Ce run-là est passé de justesse. Les 4 précédents ont franchi la limite.
3. **Le défaut.** `release_operands=True` (passé par le wrapper et par le CLI) libère
   `ins.operands` après chaque tour de découverte, car chaque liste d'opérandes retient un
   `cs_detail` capstone entier. La boucle finale qui ajoute les alias **a été oubliée** : leurs
   opérandes, qui ne seront plus jamais lus, restent tous en mémoire (≈ 84 Ko par alias en
   moyenne). L'espace d'adressage s'épuise alors au plafond `ulimit -v`. Selon l'endroit où
   l'allocation échoue, Python lève `MemoryError`, ou bien le code C (capstone/ctypes)
   déréférence un `malloc` nul : SIGSEGV. Ce n'est ni une récursion ni un bug d'index.

## Correctif

Dans la boucle finale, même libération que dans les tours, **sous la même condition**
`if release_operands:`. Sans `release_operands` (chemin du lifter en processus, qui lit
`insn.operands`), le comportement est strictement inchangé pour tous les binaires. Avec,
rien ne relit ces opérandes après la boucle : `find_functions` retourne aussitôt, et
`_func_to_dict` ne les sérialise pas (même en `full=True`).

Vérifications :
- `disasm32.py --selftest` : OK sur la copie, comme sur l'original. En CLI, la copie, sortie
  de l'arborescence pcrecomp, doit trouver `pe_analyze` :
  `PYTHONPATH=~/tools/pcrecomp/tools/pe python disasm32.py …`. Le wrapper ajoute lui-même
  ce chemin.
- Run complet patché : voir ci-dessous.

## Preuve : run complet patché (2026-10-14, 10:55:55 → 11:14:55)

`scripts/run-export-lots.sh`, sous `ulimit -v 8000000`, journal `extraction/pcrecomp/export-lots.log` :

| | non patché (diagnostic) | **patché** |
|---|---|---|
| issue | rc=0, de justesse (4 runs précédents morts) | **rc=0, `EXPORT_LOTS_OK`** |
| VmPeak | 7 975 696 kio (marge 24 Mo) | **7 356 416 kio (marge 644 Mo)** |
| boucle finale | +650 Mo | ≈ +22 Mo (plateau 7,33 Go) |
| `free -h` avant / après | 18 Gi libres / 18 Gi | 18 Gi libres / 18 Gi |

Sortie : `extraction/pcrecomp/lots/catalogue.jsonl`, 13 658 901 octets, 54 123 lignes, JSON
valide ligne par ligne ; journal : 101 691 fonctions brutes ; `meta.json` : 54 123 de taille ≥ 64,
8 153 feuilles, 12 554 328 instructions, couverture 84,58 % du code.
**Le catalogue et la méta sont identiques octet pour octet (`cmp`) à ceux du run non
patché** : le patch ne change que la mémoire, pas le résultat.

## Usage

`scripts/export-catalogue-lots.py` charge maintenant cette copie (constante `D32`, chemin
relatif au script). Lanceur : `scripts/run-export-lots.sh` (ulimit -v 8 Go, free -h avant et
après, VmPeak, faulthandler), journal dans `extraction/pcrecomp/export-lots.log`.

À remonter dans pcrecomp quand l'outillage commun sera rouvert : le diff est autonome.
