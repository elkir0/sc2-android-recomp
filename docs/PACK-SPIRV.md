# Pack SPIR-V `sc2-spirv.pak` (format v1, passe 10t)

Fichier binaire, petit-boutiste, NON compressé, conçu pour être ouvert par `mmap` en lecture seule et lu sans copie.

1. **En-tête : 64 octets** à l'offset 0.

   | offset | type | champ |
   |---:|---|---|
   | 0 | 8 octets | `magic` = `"SC2SPV01"` (ASCII, sans zéro final) |
   | 8 | uint32 | `version` = 1 |
   | 12 | uint32 | `n` = nombre d'entrées |
   | 16 | uint64 | `off_index` = offset de l'index depuis le début du fichier (= 64) |
   | 24 | uint64 | `taille` = taille totale du fichier en octets (contrôle de troncature) |
   | 32 | 32 octets | réservés, à zéro |

2. **Index : `n` entrées de 96 octets**, à `off_index`, **triées par (`hash` croissant, puis `ps` croissant)**, sans doublon
   (hash, ps).

   | offset | type | champ |
   |---:|---|---|
   | 0 | uint64 | `hash` = FNV-1a 64 du bytecode D3D9 (celui du nom `vs_<hash>.bin` / `ps_<hash>.bin`) |
   | 8 | uint8 | `ps` = 1 pixel shader, 0 vertex shader |
   | 9 | uint8 | `nentrees` = nombre d'entrées utiles dans `entrees` (≤ 16) |
   | 10 | 2 octets | réservés, à zéro |
   | 12 | uint32 | `nmots` = longueur du SPIR-V normal, en mots de 32 bits (> 0) |
   | 16 | uint32 | `nmots_ombre` = longueur de la variante « ombres » (0 si aucune) |
   | 20 | uint32 | réservé, à zéro |
   | 24 | uint64 | `off_spv` = offset du SPIR-V normal ; la variante « ombres », si `nmots_ombre` > 0, suit IMMÉDIATEMENT (à `off_spv + 4·nmots`) |
   | 32 | 48 octets | `entrees[16]` : 16 × (uint8 `reg`, uint8 `usage`, uint8 `index`), mêmes valeurs que la table générée `sc2_shaders_spv.h` (`t.entrees`), entrées au-delà de `nentrees` à zéro |
   | 80 | 16 octets | `samplers[16]` : uint8 par sampler (0 aucun, 2 2D, 3 cube, 4 volume), comme `t.samplers` |

3. **Données SPIR-V** : après l'index, chaque blob aligné sur **16 octets** (bourrage à zéro), mots de 32 bits petit-boutistes
   tels que produits par le compilateur (premier mot = magic SPIR-V 0x07230203).

Invariants vérifiables : `off_index` = 64 ; `off_index + 96·n` ≤ premier `off_spv` ; chaque `off_spv` multiple de 16 ;
`off_spv + 4·(nmots + nmots_ombre)` ≤ `taille` ; `taille` = taille réelle du fichier ; premier mot de chaque blob =
0x07230203.

Générateur : `scripts/d3d9-vers-spirv.py --pack` ; chargeur : `recomp-sc2/runtime/sc2_spv_pack.c` (repli de `spv_de()`
quand la table embarquée n'a pas le shader). Chemin : `SC2_SPIRV_PACK`, sinon `<parent de SC2_INSTALL>/sc2-spirv.pak`.
