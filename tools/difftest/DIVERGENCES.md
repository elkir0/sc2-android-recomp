# DIVERGENCES — passe 7k : le stock difftest de la campagne M4, classé, expliqué, résorbé

Session sc2-passe5a-arm64, 2026-10-15. Branche `hermes/20261015-passe7k-divergences` (base main 4a4f12c).
Contrat : `~/missions/sc2-passe7k-divergences.md`.

## 0. Résultat en une table

Périmètre : les 76 650 fonctions des 154 lots de la campagne M4 (index 10 000 → 86 649 de la liste du
14/10 ; graine = 1 + rang dans le lot de 500, exactement celles du Mac).

| mesure | harnais | lift / runtime | ok (certifiées) | échecs | skip | écarts masqués (conventions) |
|---|---|---|---:|---:|---:|---:|
| campagne M4 (Mac, 14/10) | v1 Mac | 14/10 | 3 314 | **3 985** | 69 351 | ? (DF, AF/fcompi, ret-cadre) |
| A0 | v2, disposition `m4`, sans masque | 7i | 7 398 | 1 380 | 67 872 | — |
| A1 | v2, disposition `v2`, sans masque | 7i | 9 688 | 2 024 | 64 938 | — |
| **C-7i** (avant) | v2 final | 7i (main) | 11 667 | **6** | 64 977 | 1 980 fonctions |
| **C-7k** (après) | v2 final | 7k | **11 673** | **0** | 64 977 | 1 970 fonctions |

- **Stock M4 : 3 985 → 0** (−100 %). Aucune des 3 985 n'est restée en échec : **1 989 certifiées**,
  **1 996 non testables** (identifiées : voir §2 — leur « échec » M4 était un artefact du harnais).
- **Taux de certification** (ok / (ok + échecs)) : M4 45,4 % → C-7i 99,95 % → **C-7k 100 %**.
  **Couverture** (ok / lot) : 4,3 % → **15,2 %** (×3,5).
- **Vraies divergences sémantiques trouvées et corrigées : 5 familles** (§3), dont une majeure (le mot de
  contrôle x87 : toute conversion flottant → entier tronquée de MSVC arrondissait au plus proche), toutes
  re-certifiées : test ciblé 147/147 sur x86-64 ET aarch64 (avant : 85/147 et 73/147), lot complet de la
  famille, campagne entière.

Rien n'est masqué en silence : les masques sont des listes nommées (`--masques`), chaque écart masqué est
gardé dans le rapport (`masques`, `n_masques`) et compté (dernière colonne).

## 1. Méthode

`difftest-sc2.py` v2 (même CLI que la v1, + `--adresses`, `--jobs`, `--disposition`, `--masques`, `--tout`,
`--image-runner`, `--runner-prefixe` pour qemu-aarch64) :

- **cadre de retour commun** : une adresse sentinelle (0x00401000) est posée à `[esp0-4]` dans l'état initial
  DES DEUX CÔTÉS ; Unicorn s'arrête quand il y saute, le lifté revient de `fn()` après son `ret`. Plus de
  « ret-cadre simulé » d'un seul côté ;
- **dernier écrivain** : pour chaque registre, drapeau et octet divergent, l'instruction Unicorn qui a écrit
  la valeur en dernier (adresse + désassemblage) — c'est la clé de famille et le point de départ de la
  bissection (`classer-divergences.py`) ;
- **drapeaux indéfinis** : table capstone corrigée d'après le SDM (OF des décalages hors compte 1, SF/ZF/PF/AF
  d'`imul`, OF des rotations de compte 1, AF des opérations logiques sous toutes leurs formes — capstone 5
  ne déclare rien pour `test m32, r32`), compte `cl` lu à l'exécution ;
- **runner** : plantage = signal + fonction liftée en cours + adresse invitée (`[SIGNAL]`), `[ARRÊT]` MANQUE/TODO
  rapporté, délai 20 s ;
- **disposition `v2`** (défaut) : l'image de SC2.exe (en-têtes + octets de toutes les sections) est la même des
  deux côtés (`SC2_DT_IMAGE`, segments non nuls) ; slots d'IAT = sentinelles hors `.text` (skip `import`) ;
  fenêtre scratch hors de l'image (0x04000000) ; mot de contrôle x87 de Windows (0x027F) côté Unicorn ;
  DF = TF = 0 à l'entrée (ABI Win32) ; pas de page 0xCC d'un seul côté.

Outils : `difftest-sc2.py` (harnais), `runner-difftest.c` (runner), `classer-divergences.py` (familles,
`--adresses` pour rejouer une famille), `croiser-stock-m4.py` (situe chaque échec M4 dans un rapport v2),
`test-x87-decalages.py` (test ciblé des corrections). Rapports bruts : `recomp-sc2/build/dt7k/` (hors git).

## 2. Le stock M4 (3 985) : d'où venaient les échecs

`croiser-stock-m4.py rapports-difftest-masse/ build/dt7k/S-7k.json` (le stock rejoué tel quel, mêmes
adresses, mêmes graines) :

| échec M4 | → v2 final | nombre | cause (famille) | nature |
|---|---|---:|---|---|
| `mem@00720000` (pile) et/ou `mem@01000000` | ok | 1 981 | cadre de retour du harnais M4 (« ret-cadre simulé ») : sur les 1 476 de ces fonctions déjà certifiées en A0 (disposition M4, cadre commun), 1 460 n'ont d'écart M4 QUE sur la fenêtre de pile, dont 1 372 en multiples exacts de 4 octets (adresses de retour) ; tous disparaissent avec un cadre commun | convention (harnais) |
| `ERR runner` | skip `import` | 1 630 | `jmp [IAT]` (thunks d'import du CRT, ex. 0x01041026) : l'IAT du fichier n'est pas résolue → saut vers 0 en M4 (`emu_start(…, until=0)` arrête Unicorn SANS erreur, la v1 le prenait pour une fin normale) puis MANQUE côté runner | convention (harnais) |
| `ERR runner` | skip `hors_texte` | 190 | appel de stub Win32 / données hors `.text` : non comparable (le runner n'exécute aucun stub) | convention (harnais) |
| `mem`/`reg` | skip `hors_texte` | 136 | idem (avec l'image complète, le flux Unicorn sort de `.text` : la v1 lisait des zéros et prenait un autre chemin) | convention (harnais) |
| `mem`/`reg` | skip `import` | 24 | idem `import` | convention (harnais) |
| `ERR runner` | ok | 8 | 7 : saut vers 0 en disposition M4 (pointeur ou table lu dans l'image, absente côté runner → zéro → MANQUE) ; 1 : AF indéfini ; certifiées avec l'image complète des deux côtés | convention (harnais) |
| divers | skip `saut_vers_0` / `privilegiee` / `non_deterministe` / `exception` / `budget` | 16 | pointeur nul en `.bss` (10), `in eax, dx` (sonde anti-VM / données, 3), `rdtsc` (1), faute (1), boucle (1) | non testables (identifiés) |

Les 1 380 échecs A0 (même disposition que M4, cadre commun) donnent la même conclusion : 1 311 sont des AF
indéfinis après `test/xor/or/and/shr/…` (Intel : AF indéfini ; Unicorn le met à 0, le lifté le laisse) —
c'était le « AF » du stock, masqué en v2 avec raison, compté.

## 3. Familles — tableau complet (C-7i, harnais v2 final, sans masque)

Rang = nombre de fonctions (une fonction = une famille, la plus prioritaire de ses écarts : arrêt runner >
registre > mémoire > drapeau ; pour les familles masquées : la plus prioritaire de ses écarts masqués,
C-7i). Toutes les familles rencontrées en A0/A1/B/C sont listées ici. Les familles 1 à 7b totalisent
1 971 fonctions ; avec les familles 8 (6) et 9 (3) : les 1 980 fonctions à écart masqué de C-7i.

| # | famille (champ ← dernier écrivain Unicorn) | fonctions | nature | traitement |
|---:|---|---:|---|---|
| 1 | AF ← `test` | 1 133 | convention : indéfini (SDM) | masque `drapeaux-indefinis` |
| 2 | AF ← `xor` | 635 | idem | idem |
| 3 | AF ← `or` | 71 | idem | idem |
| 4 | AF ← `shr` | 43 | idem (AF indéfini après décalage) | idem |
| 5 | PF/AF/SF/ZF ← `imul` (1 et 2 op.) | 49 | idem (SF/ZF/AF/PF indéfinis ; capstone dit SF « modifié » : corrigé) | idem |
| 6 | AF ← `and` | 23 | idem | idem |
| 7 | OF ← `shr`/`shl` compte > 1 | 11 (7i) / 1 (7k) | idem (OF défini pour un compte de 1 seulement ; la formule 7k coïncide avec QEMU au-delà, d'où 1 seul écart masqué restant) | idem |
| 7b | AF ← `shl`/`sar`/`idiv` | 6 | idem | idem |
| 8 | AF/SF ← `fcomi`/`fcompi` | 6 | **erreur de l'ORACLE** : Intel met OF = SF = AF = 0 (ce que fait le lifté) ; QEMU/Unicorn ne touche que ZF/PF/CF | masque `oracle-qemu` |
| 9 | ESP ← `ret 0xc149` (données désassemblées) | 3 | **erreur de l'oracle** : QEMU lit l'imm16 de `ret` signé | masque `oracle-qemu` |
| 10 | GPR/mem ← `rdtsc` | 5 (skip) | non déterministe des deux côtés (valeur propagée en mémoire) | skip `non_deterministe` |
| 11 | MANQUE vers 0x00D69xxx–0x00D6Dxxx | 35 | IAT non résolue dans l'image du fichier (`jmp [0x14194F8]` → RVA du nom de l'import, au milieu de `.text`) | skip `import` (sentinelles d'IAT) |
| 12 | `in eax, dx` / TODO `in` | 2 | instruction privilégiée (exception en mode utilisateur) | skip `privilegiee` |
| 13 | mot de contrôle x87 en mémoire ← `fnstcw` (A0) | 20 | convention : Unicorn démarre à 0x037F, Windows à 0x027F | mot de Windows posé côté Unicorn |
| **14** | **OF ← `shr r, 1` / `shl r, cl` (cl = 1)** | **2 (+3 en A0)** | **VRAIE** : `op_shl/op_shr/op_sar/op_shld/op_shrd` n'écrivaient jamais OF (défini pour un compte de 1 ; `jo/jl/jg/jle/jge` le lisent) | **corrigé (cpu.h)** |
| **15** | **EAX ← `fnstsw ax`** | **1** | **VRAIE** : le mot d'état rendu sans le champ TOP (bits 11-13) | **corrigé (lifter + cpu.h)** |
| **16** | **GPR ← `lea`/`mov al`/`xor al` après `fistp`** (00C7C690, 0121EF10, 0121D360) | **3** | **VRAIE** : `fldcw` ignoré → `fistp` arrondit au lieu de tronquer (+ TOP de fnstsw en aval) | **corrigé (lifter + cpu.h)** |
| **17** | **`fistp` hors plage / NaN** | (test ciblé) | **VRAIE** : cast C indéfini → x86-64 rend 0x80000000 par chance, **AArch64 SATURE** (0x7FFFFFFF) | **corrigé (cpu.h)** |
| **18** | **`frndint`, `fninit`, `fnstcw`** | (test ciblé) | **VRAIES** : RC ignoré ; fninit sans effet ; fnstcw constant | **corrigé (lifter + cpu.h)** |

Familles 14-16 dans la campagne : C-7i 6 fonctions en échec → C-7k 0.

## 4. La correction majeure : le mot de contrôle x87

**Constat.** `lift32_cpu` traduisait `fldcw` en commentaire (`/* fldcw ignored */`) et `fnstcw` en constante
0x027F. MSVC (VS2008, x87) tronque un flottant en entier ainsi (0x00802360, et 1 414 `fldcw` dans SC2.exe) :

```
fnstcw word ptr [ebp-2] ; movzx eax, [ebp-2] ; or eax, 0xC00 ; mov [ebp-0x10], eax
fldcw  word ptr [ebp-0x10]          ; RC = 11b : vers zéro
fistp  qword ptr [ebp-0x14]         ; (int)x
fldcw  word ptr [ebp-2]             ; restaure
```

Le lifté arrondissait donc au plus proche : `(int)2.7` = 3, `(int)-2.7` = -3, `(int)2.5` = 2 (pair). Toute
conversion flottant → entier du jeu passée par ce motif (positions → cases de grille, temps → ticks, index de
tableau calculés) était fausse d'une unité une fois sur deux.

**Impact prouvé en jeu (passe 7j, BUS 18:05)** : dans la boucle de terrain 0x00CB8170 (les arrêts post-768 Mo de
la 7i), l'indice de cellule y valait arrondi(7,52) = 8 sur une grille de 8 → écritures hors grille (crash NULL,
blocage, emballement). Avec le cpu.h 7k + relift `PCRECOMP_FPU_CW=1`, la 7j passe de 1 005 à 1 067 lectures
(expérience non commitée de son côté : elle prendra la correction par fusion de la 7k).

**Correction** (lifter opt-in `PCRECOMP_FPU_CW=1`, posé par `scripts/lift-boot.py` ; helpers dans `cpu.h`) :
- `CPU.fpu_cwx` : mot de contrôle stocké XOR 0x027F (un `CPU` remis à zéro a le mot de Windows) ;
- `fldcw` → `fpu_fldcw`, `fnstcw` → `fpu_fnstcw`, `fninit` → `fpu_fninit` (0x037F, TOP = 0, SW = 0) ;
- `fist/fistp` → `wri16r/wri32r/wri64r` : arrondi selon RC (proche-pair / −∞ / +∞ / zéro) ; hors plage et NaN
  → « integer indefinite » (0x8000, 0x80000000, 0x8000000000000000) au lieu d'un cast C indéfini (qui SATURE
  sur AArch64 : x86 et ARM64 divergeaient) ; `wri16/32/64` (anciennes formes) bornées de même ;
- `frndint` → `fpu_rndint` (RC) ; `fnstsw` → `fpu_fnstsw` (C0-C3 + TOP).
- Précision (PC) : le modèle reste en `double`, soit PC = 53 bits — celle de Windows (0x027F) : cohérent.

Relift : 7 738 lignes changent, exactement 4 801 `fnstsw` + 1 414 `fldcw` + 706 `fnstcw` + 817 `fist(p)`
(dont 1 `fist` 16 bits) ; table des fonctions identique (86 651).

**Re-certification.**
- `test-x87-decalages.py` (147 séquences : fldcw+fistp 16/32/64 × 4 modes RC × 9 valeurs dont hors plage et NaN,
  frndint, fnstcw, fninit, fnstsw/TOP, OF des décalages ; Unicorn vs gcc x86-64 et aarch64-linux-gnu-gcc sous
  qemu-aarch64) : **cpu.h de main 85/147 (x86-64), 73/147 (aarch64) → cpu.h 7k 147/147 et 147/147**.
- Famille entière (1 150 fonctions liftées qui contiennent une forme x87 modifiée × 3 graines) : voir §6.
- Campagne entière : C-7i 6 échecs (familles 14-16) → C-7k 0.

## 5. OF des décalages

`op_shl/op_shr/op_sar` (et `op_shld/op_shrd`) calculaient CF/ZF/SF/PF mais jamais OF. Intel le définit pour un
compte de 1 : SHL → MSB(r) xor CF ; SHR → MSB de l'opérande ; SAR → 0 ; SHLD/SHRD → changement de signe. Au-delà
il est indéfini (masqué). Corrigé dans `cpu.h` (SC2) et dans le `cpu.h` générique de pcrecomp.

## 6. Re-certification par famille, run du jeu, ARM64

**Familles entières** (harnais final ; chaque fonction liftée concernée × 3 graines ; lots
`build/dt7k/famille-x87.txt`, `famille-dec.txt`) :

| famille | fonctions × graines | 7i : ok / échec | 7k : ok / échec | skip |
|---|---:|---:|---:|---:|
| x87 à mot de contrôle (fldcw, fnstcw, fist(p), frndint, fnstsw) | 1 150 × 3 | 107 / **14** | **121 / 0** | 3 329 |
| décalages (shl/shr/sar/shld/shrd) | 12 341 × 3 | 2 709 / **4** | **2 713 / 0** | 34 310 |

(Les skips de la famille x87 sont des fonctions qui appellent la CRT ou des stubs : non comparables ici.)

**Jeu complet** (`recomp-sc2`, run par défaut 600 f, installation de référence, timeout 300 s ; même build,
A/B strict : seuls `cpu.h` et les 74 fichiers liftés changés diffèrent) :

| binaire | rc | lectures MPQ | jalons | f1 | f10 | f100 → f600 |
|---|---|---:|---:|---|---|---|
| 7i (cpu.h de main + lift 7i) ×2 | 0 | 255 | 19 | be01318cdf69 | afc945b66cd5 | 7ecdf5b31e2a (= référence R2 7i) |
| **7k** ×3 | 0 | 255 | 19 | be01318cdf69 | **d3cd40be732f** | **12450c3ab300** |

Les empreintes changent, de façon reproductible (×3, binaire md5 c5391755e342) : c'est l'effet attendu de la
correction. Écart mesuré : écran-titre visuellement identique, mais 52 % des pixels varient faiblement (mise à
l'échelle) ; dialogue de la frame 100 : bord haut-gauche déplacé d'UN pixel (y 209 → 208, x 403 → 402),
les autres bords inchangés — une coordonnée flottante désormais TRONQUÉE comme sous Windows au lieu d'être
arrondie. Rien d'autre ne change (rc, lectures, jalons). **Re-baseline à faire à la fusion** (la 7l a déjà
re-baseliné f100 sur main : les deux effets se cumulent).

Note de reproduction : un worktree neuf n'a pas `src/shaders/sc2_shaders_spv.h` (généré, hors git) ; sans lui
le jeu rend des frames noires (draws « lumière »), et `__has_include` n'est pas une dépendance make (toucher
`runtime/sc2_d3d9.c` après l'avoir copié).

**ARM64** : `test-x87-decalages.py` compile le même C avec `aarch64-linux-gnu-gcc` et l'exécute sous
qemu-aarch64 : 147/147 (73/147 avant — 12 cas de plus qu'en x86 échouaient : le cast flottant → entier hors
plage SATURE en AArch64).

**Runner ARM64** (`build-arm64/runner-difftest`, compilé croisé Debug, exécuté par le harnais via
`--runner-prefixe "qemu-aarch64 -L ~/tools/arm64-vk-root …"`, mêmes lots, mêmes graines, lift 7k) :

| lot | ok | échecs | skip | (x86-64, C-7k) |
|---|---:|---:|---:|---|
| campagne (76 650) | 11 672 | **1** | 64 977 | 11 673 / 0 |
| famille x87 (1 150 × 3) | 119 | **2** | 3 329 | 121 / 0 |
| stock M4 (3 985) | 1 988 | **1** | 1 996 | 1 989 / 0 |

Les 4 échecs sont UNE fonction (0x012CB7E0, graines 52, 1, 2) et UNE famille, **propre à ARM64** :
`fstp dword ptr [ebp+0xc]` d'un NaN né d'une opération invalide → octet haut 0xFF en x86 (le « real
indefinite » x87 est le QNaN NÉGATIF 0xFFC00000) contre 0x7F sur AArch64 (NaN par défaut POSITIF 0x7FC00000).
**Vraie divergence, NON corrigée en 7k** (canonicaliser le NaN produit par les opérations x87/SSE sur hôte ARM :
helper dans cpu.h + rebuild massif, hors du cadre de fin de passe ; impact : signe d'un NaN, visible seulement
si le jeu teste ce NaN bit à bit). Piste : `fpu_nan(v)` = `isnan(v) && signbit absent → -NaN` sur les écritures
`wrf32/wrf64` sous `__aarch64__`.

## 7. Ce qui reste / limites

- **Skips** : 64 977 / 76 650 fonctions ne sont pas comparables par construction (le flux Unicorn sort de
  `.text` : appels de stubs Win32, `import` 4 293, pointeurs nuls 105, instructions privilégiées 56, exceptions
  1 031, boucles 84, `rdtsc` 5). La v2 les NOMME au lieu de les compter en échec. Étendre la couverture demande
  de simuler les imports côté Unicorn (hors périmètre 7k).
- **x87 en `double`** : les registres ST ne sont pas comparés (le runner ne les rend pas au harnais) ; seules
  les valeurs écrites en mémoire / registres entiers le sont. La précision étendue (PC = 64 bits, si le jeu la
  demandait par fldcw) n'est pas modélisée : le champ PC est mémorisé, pas appliqué.
- **DF** : `std` (4 sites, dont 2 dans des données désassemblées) reste non modélisé (les opérations de chaîne
  vont toujours vers l'avant) ; aucune fonction certifiable de la campagne ne l'atteint.
- **NaN par défaut sur ARM64** (§6) : 1 fonction, signe du QNaN ; à corriger dans une passe suivante.
- **Lots 0 → 9 999** de la liste : hors campagne M4, non mesurés ici.
- La campagne M4 du Mac avait un harnais non versionné (« ECRITS divergents », ret-cadre simulé) : le mécanisme
  exact de ses écarts de pile N×4 n'est pas reconstruit ligne à ligne ; il est établi que ces écarts sont
  confinés à la fenêtre de pile et disparaissent avec un cadre commun, pour la totalité des 1 981 fonctions.

## 8. Fichiers

| fichier | rôle |
|---|---|
| `recomp-sc2/tools/difftest/difftest-sc2.py` | harnais v2 |
| `recomp-sc2/tools/difftest/runner-difftest.c` | runner : `SC2_DT_IMAGE` (DTI1 ou plate), `[SIGNAL]` |
| `recomp-sc2/tools/difftest/classer-divergences.py` | familles, lots de re-certification |
| `recomp-sc2/tools/difftest/croiser-stock-m4.py` | ventilation du stock M4 |
| `recomp-sc2/tools/difftest/test-x87-decalages.py` | test ciblé x87 / décalages (x86-64 + aarch64) |
| `recomp-sc2/tools/difftest/pcrecomp-7k.patch` | patch du lifter et du cpu.h générique (pcrecomp hors git ; aussi `~/tools/pcrecomp/NOTES-SC2-7k.patch`, `NOTES-SC2.md`) |
| `recomp-sc2/runtime/cpu.h` | mot de contrôle x87, fist(p) bornés, fnstsw TOP, OF des décalages |
| `scripts/lift-boot.py` | pose `PCRECOMP_FPU_CW=1` avant d'importer le lifter |
