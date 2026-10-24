# SPEC — passe 7u-MoltenVK (portage macOS ARM64, Mac M4)

*Rédigée le 16/10/2026 (session bg 86cd8f38), après la mort du serveur tmux Mac (~10:00 AST) qui a emporté la session « opus2 ».
Analyse et rédaction uniquement : aucun build ni run n'a été lancé pour écrire ce document.*

---

## 0. Où vivent les choses (à lire d'abord, les chemins du brief étaient approximatifs)

| Brief | Réalité constatée |
|---|---|
| « miroir `~/code/reverse/sc2-partout-mac/recomp-sc2` » | Miroir **ancien** (passe 5a, `6ed4b21`) : **aucun** patch MoltenVK, `ZONE_FIN = 0x38000000`. Ne pas s'en servir comme base. |
| (non cité) | **Vrai banc Mac** : `~/code/sc2-partout-mac/` (hors git) — `BANC-MAC.md`, `7q/` (arbre c2dedfe + lift + patchs Mac), `build-7q/`, `run-7u.sh`, `install-7u/`, `runs/`, `captures/`, `ref-7u-x86/`. |
| `references/portage-arm64-macos.md` | `~/.hermes/profiles/reversosc2/skills/security/sc2-partout/references/portage-arm64-macos.md` (skill Hermes). Il couvre la zone hôte + delta et les guards Darwin, **pas MoltenVK**. |
| « NOTES.md du dépôt » | Sur le serveur Linux : `~/code/reverse/sc2-partout/NOTES.md` (main `835ba54`), lignes 1784–1854 ; et `~/missions/BUS-SC2.md` l. 1126 et 1141. |
| Transcript de la session morte | `~/.claude/projects/<user>/ddb9b36c-…jsonl` (cwd `~/code`, dernier message 12:48Z). |

## 1. État exact de la piste MoltenVK (reconstitué)

### 1.1 Ce qui est fait et vérifié (source : BANC-MAC.md + transcript ddb9b36c)
- **MoltenVK installé via Homebrew** le 15/10 21:37Z : `brew install vulkan-headers molten-vk vulkan-loader`.
  - `molten-vk` **1.4.2** (bottle `arm64_tahoe`, ad hoc signé, arm64 seul, 5,3 Mo). C'est la **dernière release amont** : tag `v1.4.2`, publié le 2026-07-24, vérifié via l'API GitHub le 16/10.
  - `vulkan-loader` et `vulkan-headers` 1.4.363.0, qui est aussi la version du LunarG SDK macOS « latest ».
  - ICD présent : `/opt/homebrew/etc/vulkan/icd.d/MoltenVK_icd.json` (`is_portability_driver: true`).
- **Seul patch Vulkan** : `runtime/sc2_d3d9v.c`, bloc `#ifdef __APPLE__`. Après l'échec de `dlopen("libvulkan.so.1")`, le code charge **directement** `/opt/homebrew/lib/libMoltenVK.dylib`, puis `libMoltenVK.dylib`. Le loader Khronos est contourné : MoltenVK exporte lui-même `vkGetInstanceProcAddr`.
- **En-têtes** : liens manuels `build-*/vk-entetes/{vulkan,vk_video}` → `/opt/homebrew/include`, parce que le CMake ne lie que `/usr/include`.
- **Résultats** :
  - 600 frames en 58 s. Log : `Apple M4 (Vulkan 1.3.357, type 1)`, BC natif, `VK_KHR_push_descriptor` actif, 0 draw refusé, run déterministe (PPM identiques au bit sur deux runs).
  - **7u buildée sur Mac** (`build-7q`) : 1 200 frames, rc=0, et **visuellement identique** à la réf. 7u x86 + llvmpipe avec la table SPIR-V 1 634.
    - f10–500 : 0–0,07 % des pixels à écart > 16.
    - f750–1200 : 1,6 % des pixels à écart > 16.
- **Correctif Mac indispensable** (copie `7q/` seulement) : `ZONE_FIN = 0xF0000000` dans `zone_hote_init`. Sans lui, l'arène LAA de débordement 0x80000000–0xF0000000 tombe hors zone et le run meurt à la frame 466 avec « arène invitée épuisée ».
- MoltenVK est **innocenté** sur la fidélité : le même arbre sous qemu + lavapipe donne le même état de jeu.
- **Main serveur Linux `835ba54` ne contient NI le fallback MoltenVK NI le correctif `ZONE_FIN`** (vérifié par grep le 16/10). Main a depuis gagné la table 1 639 (7s) et les samplers d'ombres à comparaison (7v2/7v3).

### 1.2 L'« arbitrage en suspens » : binaire officiel vs compilation depuis les sources
**Aucune trace écrite de cet arbitrage nulle part** : ni NOTES.md, ni BUS-SC2.md, ni le skill, ni le transcript ddb9b36c (recherche `molten|officiel|sources|MVK`). La session opus2 est morte **avant** d'avoir reçu sa mission « 7u MoltenVK ». La supervision l'a relancée comme « mission à re-spécifier » (NOTES l. 1831). L'arbitrage est donc reconstitué ici à partir de l'état technique. Il se pose pour trois raisons concrètes :

1. **Dépendance implicite à Homebrew.** Le chemin `/opt/homebrew/lib/libMoltenVK.dylib` est codé en dur. Un `brew upgrade` changerait le pilote sans bruit, ce qui casse la reproductibilité des empreintes PPM. Rien ne tourne sur un Mac sans Homebrew.
2. **Distribution.** Un binaire Mac livrable doit embarquer son `libMoltenVK.dylib`.
3. **Débogage.** Sans loader, pas de couches de validation. On ne sait donc pas si le backend respecte `VK_KHR_portability_subset`, que le code n'active pas alors que la spec l'exige.

| Option | Ce que c'est | Pour | Contre |
|---|---|---|---|
| **A. Homebrew (actuel)** | bottle `molten-vk` 1.4.2, compilée par Homebrew depuis le tag v1.4.2 | déjà validée (7u 1 200 f) ; aucun coût | chemin en dur ; version mobile (`brew upgrade`) ; non distribuable |
| **B. Binaire officiel Khronos** | `MoltenVK-macos.tar` v1.4.2 (59,6 Mo, GitHub Releases) ; variante `-privateapi` existante | **même tag source** que A ; figeable par sha256 ; embarquable à côté du binaire ; pas de toolchain | téléchargement de 60 Mo à vérifier ; ad hoc signé (OK pour dlopen non-hardened) |
| **C. Compilation depuis les sources** | `git clone v1.4.2` + `fetchDependencies --macos` + `make macos` | permet de patcher MoltenVK, de garder les symboles de debug et de changer la config par défaut | **Xcode complet requis** (présent, mais `xcode-select` → CLT) ; **plusieurs Go** de dépendances (SPIRV-Cross, SPIRV-Tools, glslang…) alors que **5,3 Gio restent libres** ; build CPU lourd **interdit tant que l'émulateur vit sur le M4** ; aucun gain sans patch à appliquer |

**Recommandation :**
- **Développement** : garder **A**, mais l'**épingler** avec `brew pin molten-vk`.
- **Référence figée et embarquement** : **B**, avec le même tag v1.4.2. Le pilote devient reproductible et indépendant de Homebrew, sans aucune compilation.
- **C n'est retenue que sur déclencheur explicite** (§4, étape 9). Aujourd'hui aucun bug MoltenVK n'est identifié : les 1,6 % d'écart s'expliquent par la table SPIR-V et le rastériseur. Compiler les sources coûterait disque et CPU sur une machine partagée avec l'émulateur, sans rien apporter.

## 2. Objectif de la passe
Rendre le chemin MoltenVK du portage Mac **explicite, figé, remonté dans `main` et vérifiable** :
- pilote choisi par ordre défini et journalisé ;
- correctif `ZONE_FIN` remonté ;
- conformité `portability_subset` contrôlée par les couches de validation ;
- un binaire qui tourne avec un MoltenVK embarqué.

L'arbitrage A/B/C est tranché (§1.2), mais **C peut encore être activée** par un déclencheur mesuré.

**Hors périmètre** : fenêtre et swapchain réelles (`VK_EXT_metal_surface`, le backend reste headless + PPM), APK Android, modification du lift.

## 3. Préconditions (à vérifier avant toute étape lourde, c'est-à-dire les étapes 4, 5, 6 et 7)
```sh
pgrep -fl 'qemu-system|emulator' && echo "ÉMULATEUR VIVANT → STOP, attendre le feu vert supervision"
memory_pressure | tail -1          # exiger « System-wide memory free percentage » ≥ 40 %
df -h / | tail -1                  # exiger ≥ 8 Gio libres pour les runs (≥ 15 Gio pour l'option C)
uptime                             # load < 4
```
- Constat du 16/10 : 16 Go de RAM, **5,3 Gio de disque libres** (déjà en dessous du seuil), émulateur M4 de 6 Go vivant.
- **Libérer du disque d'abord.** Candidats, à faire valider par le mainteneur : `~/code/sc2-partout-mac/build-mac` (obsolète depuis `build-7q`), `runs/*/frames` hors `7u-spv1634` et `vk`, et `~/code/reverse/sc2-partout-mac/recomp-sc2/build{,-asan,-r2}` (miroir 5a).

## 4. Plan étape par étape

### Étape 1 — Inventaire figé du pilote actuel (léger, faisable même avec l'émulateur)
```sh
mkdir -p ~/code/sc2-partout-mac/mvk && cd ~/code/sc2-partout-mac/mvk
{ brew list --versions molten-vk vulkan-loader vulkan-headers
  shasum -a 256 /opt/homebrew/Cellar/molten-vk/1.4.2/lib/libMoltenVK.dylib
  codesign -dv /opt/homebrew/lib/libMoltenVK.dylib 2>&1 | grep -E 'Identifier|flags'
  lipo -info /opt/homebrew/lib/libMoltenVK.dylib; } > inventaire-homebrew.txt
brew pin molten-vk vulkan-loader vulkan-headers
```
**Succès** : `inventaire-homebrew.txt` existe, et `brew list --pinned` contient `molten-vk`.

### Étape 2 — Binaire officiel Khronos v1.4.2 (léger : téléchargement de 60 Mo, aucune compilation)
Le tar est une donnée externe : le placer dans un dossier neuf et vide, sans rien exécuter dedans.
```sh
D=~/code/sc2-partout-mac/mvk/officiel-1.4.2; mkdir -p $D/dl && cd $D/dl
curl -fL -o MoltenVK-macos.tar https://github.com/KhronosGroup/MoltenVK/releases/download/v1.4.2/MoltenVK-macos.tar
shasum -a 256 MoltenVK-macos.tar | tee ../MoltenVK-macos.tar.sha256
tar -tf MoltenVK-macos.tar | grep -E 'libMoltenVK\.dylib$|MoltenVK_icd\.json$'   # repérer les chemins réels
tar -xf MoltenVK-macos.tar -C ..
L=$(find .. -path '*dynamic*' -name libMoltenVK.dylib -path '*macOS*' | head -1); echo "$L"
lipo -info "$L"; otool -L "$L" | head -3; codesign -dv "$L" 2>&1 | grep flags
cp "$L" ../libMoltenVK.dylib && shasum -a 256 ../libMoltenVK.dylib >> ../MoltenVK-macos.tar.sha256
```
**Succès** :
- le dylib contient l'architecture `arm64` (universel accepté) ;
- `otool -L` ne référence que des frameworks système et `libc++` ;
- les deux sha256 sont consignés.

Le dylib officiel ne sera **pas identique au bit** à la bottle Homebrew (toolchain et options différentes). C'est attendu : l'équivalence se juge au run (étape 5), pas au sha.

### Étape 3 — Patchs runtime à remonter dans `main` (code ; sur le serveur Linux, worktree de mission)
- **Base** : `main` du serveur Linux (`835ba54` ou plus récent). Pas le miroir 5a, pas `7q/`.
- **Branche** : `hermes/<AAAAMMJJ>-passe7u-moltenvk`, créée par `git worktree add` selon la procédure de passation du serveur Linux (§2, document privé). **Baseline** sous Linux avant toute modification : défaut ×2, empreintes `be01318c/d3cd40be/8d1ec057`.

1. **`sc2_runtime.c` / `zone_hote_init`** : `ZONE_FIN = 0xF0000000u` sous `__APPLE__`. Toujours sous `__APPLE__`, ajouter un garde-fou : tout `mmap` « FIXED » doit viser une adresse comprise dans `[zone, zone+taille)`, sinon le code doit tracer l'appel et le refuser. Ce garde-fou neutralise le danger de l'alias `MAP_FIXED_NOREPLACE → MAP_FIXED`.
2. **`sc2_d3d9v.c` : ordre de chargement explicite et journalisé** (`__APPLE__`) :
   1. `getenv("SC2_VK_LIB")` (chemin complet) ;
   2. `@executable_path/libMoltenVK.dylib`, construit via `_NSGetExecutablePath` + `dirname` ;
   3. `libvulkan.1.dylib` (loader, active les couches) ;
   4. `/opt/homebrew/lib/libMoltenVK.dylib` ;
   5. `libMoltenVK.dylib`.

   Journaliser `[VK] bibliothèque : <chemin réel>` en résolvant le chemin réel via `dladdr(gipa)`.
3. **Si le chemin passe par le loader** : ajouter `VK_KHR_portability_enumeration` à l'instance avec `VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR`. Sans ce drapeau, le loader 1.3.216+ ne liste pas MoltenVK et renvoie `VK_ERROR_INCOMPATIBLE_DRIVER`.
4. **Si le device expose `VK_KHR_portability_subset`** : l'activer (la spec l'exige) et journaliser une ligne `[VK] portability_subset : triangleFans=%d mutableComparisonSamplers=%d …` à partir de `VkPhysicalDevicePortabilitySubsetFeaturesKHR`.
   - Si `triangleFans == 0`, ajouter un **compteur** de draws `D3DPT_TRIANGLEFAN` (topologie D3D 6 → `TRIANGLE_FAN`, `sc2_d3d9v.c` l. ~1278), avec une ligne `[BILAN]`.
   - La conversion fan → liste n'est à faire que si le compteur est > 0.
5. **`CMakeLists.txt` (APPLE)** : `find_path(VK_INC vulkan/vulkan.h HINTS /opt/homebrew/include $ENV{VULKAN_SDK}/include)` remplace les liens manuels `vk-entetes`. Ajouter `-Wl,-rpath,@executable_path`.

**Rétrocompatibilité exigée** : Linux et Android doivent rester octet pour octet. Tout le code neuf est sous `#ifdef __APPLE__`, sauf le compteur fan, qui se contente de journaliser.

**Succès sous Linux** :
- `make` + `ctest` verts ;
- défaut ×2 avec empreintes inchangées ;
- md5 du binaire x86 inchangé **si** le compteur est sous `__APPLE__` ; sinon, seules les lignes de journal diffèrent.

### Étape 4 — Sync + build Mac (lourd mais court ; préconditions §3)
```sh
rsync -a serveur:~/code/reverse/sc2-partout/.claude/worktrees/<wt>/recomp-sc2/ ~/code/sc2-partout-mac/7u-mvk/recomp-sc2/
rsync -a serveur:~/code/reverse/sc2-partout/extraction/lifted-source/ ~/code/sc2-partout-mac/7u-mvk/recomp-sc2/src/lifted/   # SANS --delete
cmake -S ~/code/sc2-partout-mac/7u-mvk/recomp-sc2 -B ~/code/sc2-partout-mac/build-7u-mvk -DCMAKE_BUILD_TYPE=Release
nice cmake --build ~/code/sc2-partout-mac/build-7u-mvk -j6 --target recomp-sc2 2>&1 | tee ~/code/sc2-partout-mac/runs/build-7u-mvk.log | grep -E 'error|Built target'
ls -l ~/code/sc2-partout-mac/build-7u-mvk/recomp-sc2; date
```
**Succès** :
- `Built target recomp-sc2` présent ;
- l'horodatage du binaire est postérieur à `date` moins 5 min (leçon du skill : build échoué en silence derrière un pipe) ;
- le log `[LIFT]` du runtime au démarrage indique un lift à jour (garde 7w8).

### Étape 5 — Runs A/B du pilote (lourd : environ 6 min chacun ; séquentiels, jamais en parallèle)
`run-7u.sh` (config officielle 7u : 1 200 f, capture toutes les 50, `SC2_RUN_SANS_BNET=1`, `-run Maps/Campaign/TRaynor01.SC2Map`) :
```sh
cd ~/code/sc2-partout-mac
./run-7u.sh mvk-brew     build-7u-mvk/recomp-sc2 SC2_VK_LIB=/opt/homebrew/lib/libMoltenVK.dylib
./run-7u.sh mvk-officiel build-7u-mvk/recomp-sc2 SC2_VK_LIB=$PWD/mvk/officiel-1.4.2/libMoltenVK.dylib
for r in mvk-brew mvk-officiel; do grep -a -E '^\[VK\] (bibliothèque|backend|portability)' runs/7u-$r/run.log; cat runs/7u-$r/rc.txt; done
```
Comparaison des PPM : réutiliser le script numpy de BANC-MAC (% de pixels à écart > 16 et écart moyen).

**Critères de succès mesurables** :

| # | Critère | Seuil |
|---|---|---|
| S1 | rc des deux runs | `rc=0`, `[BILAN] frames` = 1 200, 0 initialiseur abandonné |
| S2 | ligne `[VK] bibliothèque` | égale au chemin demandé dans chaque run |
| S3 | brew vs officiel, frames 1 à 500 | ≤ 0,1 % de pixels à écart > 16 (attendu : identique au bit, même tag) |
| S4 | brew vs officiel, frames 750 à 1 200 | ≤ 0,5 % de pixels à écart > 16 |
| S5 | officiel vs `ref-7u-x86/`, frames 10 à 500 | ≤ 0,1 % de pixels à écart > 16 (référence : 0–0,07 %) |
| S6 | officiel vs `ref-7u-x86/`, frames 750 à 1 200 | ≤ 2 % de pixels à écart > 16 avec la table 1 634 ; avec la table actuelle de main (1 639, passe 7s), consigner la valeur, sans seuil |
| S7 | draws refusés | 0 |
| S8 | durée | ≤ 1,2 × 353 s |

La transition f550–700 est exclue des seuils : elle dépend de l'horloge, voir BANC-MAC.

### Étape 6 — Ombres (`SC2_OMBRES=1`) sous MoltenVK (lourd, un run)
Main porte désormais les samplers à comparaison 7v2/7v3, sur une carte d'ombres D32. C'est la fonctionnalité la plus exposée aux limites du portability subset.
```sh
./run-7u.sh mvk-ombres build-7u-mvk/recomp-sc2 SC2_VK_LIB=$PWD/mvk/officiel-1.4.2/libMoltenVK.dylib SC2_OMBRES=1
```
**Succès** :
- rc=0 sur 1 200 f ;
- `[BILAN]` affiche des échantillonnages de la carte d'ombres > 0 ;
- capture f1200 vue à l'œil : ombres portées visibles, comme dans TRACE-PASSE7V3 ;
- 0 erreur `[VK]`.

### Étape 7 — Couches de validation via le loader (lourd mais court : 150 frames)
```sh
brew install vulkan-validationlayers   # ou le LunarG SDK 1.4.363.0 si la formule manque
VK_LOADER_DEBUG=error VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation \
  ./run-7u.sh mvk-valid build-7u-mvk/recomp-sc2 SC2_VK_LIB=/opt/homebrew/lib/libvulkan.1.dylib SC2_FRAMES_MAX=150
grep -a -o 'VUID-[A-Za-z0-9_-]*' runs/7u-mvk-valid/run.log | sort | uniq -c | sort -rn > runs/7u-mvk-valid/vuids.txt
```
**Succès** :
- la ligne `[VK] bibliothèque` indique le loader ;
- les deux extensions de portabilité sont actives ;
- **0 VUID qui mentionne `portability`** ;
- les autres VUID sont listés dans `vuids.txt` et triés en « bloquant » ou « cosmétique ». Il n'est pas exigé de les corriger dans cette passe.

### Étape 8 — Embarquement : binaire + dylib officiel (léger)
```sh
P=~/code/sc2-partout-mac/paquet-7u-mvk; rm -rf $P; mkdir -p $P
cp build-7u-mvk/recomp-sc2 mvk/officiel-1.4.2/libMoltenVK.dylib $P/
codesign --force -s - $P/libMoltenVK.dylib $P/recomp-sc2
( cd $P && shasum -a 256 recomp-sc2 libMoltenVK.dylib > SHA256SUMS )
brew unlink molten-vk          # simule un Mac sans Homebrew MoltenVK ; RELINK juste après
./run-7u.sh mvk-paquet $P/recomp-sc2 SC2_FRAMES_MAX=100 ; brew link molten-vk
```
**Succès** :
- `[VK] bibliothèque : …/paquet-7u-mvk/libMoltenVK.dylib`, sans `SC2_VK_LIB` ;
- rc=0 ;
- f100 identique au bit à `7u-mvk-officiel` f100 ;
- `brew link` refait : vérifier `ls -l /opt/homebrew/lib/libMoltenVK.dylib`.

### Étape 9 — Porte de décision « option C » (compilation depuis les sources)
**Déclencheurs** : il en faut au moins un, mesuré.
- (a) Un écart S3, S4 ou S6 attribuable au pilote. La méthode : une frame où brew et l'officiel divergent avec le même binaire, ou un artefact absent sous lavapipe.
- (b) Un VUID bloquant de l'étape 7 qui exige un correctif côté MoltenVK.
- (c) Un plantage dans `libMoltenVK` (rapport `.ips`) qui demande les symboles de debug.
- (d) Un besoin d'option de compilation non exposée par `MVK_CONFIG_*` à l'exécution.

**Si un déclencheur est présent, et seulement après avoir libéré ≥ 15 Gio et arrêté l'émulateur :**
```sh
export DEVELOPER_DIR=/Applications/Xcode.app/Contents/Developer   # xcode-select pointe sur les CLT
mkdir -p ~/code/sc2-partout-mac/mvk/src && cd ~/code/sc2-partout-mac/mvk/src
git clone --depth 1 --branch v1.4.2 https://github.com/KhronosGroup/MoltenVK.git && cd MoltenVK
./fetchDependencies --macos            # télécharge et compile SPIRV-Cross, SPIRV-Tools, glslang… (long)
make macos-debug                       # ou : make macos
find Package -name libMoltenVK.dylib -path '*macOS*'
```
**Succès C** : le dylib compilé passe S1 à S8, **et** le déclencheur disparaît, ce que montre un A/B avec la même frame et la même mesure.

**Sans déclencheur** : consigner « C non requise » avec les mesures S3, S4 et S7 comme preuve.

### Étape 10 — Documentation et livraison
- **`BANC-MAC.md`** : nouvelle section « 7u-MoltenVK » avec le tableau S1–S8 et le chemin du paquet.
- **Skill `references/portage-arm64-macos.md`** : nouvelle section « MoltenVK ». Elle couvre l'ordre de chargement, le drapeau de portabilité pour le loader, `portability_subset`, `brew pin`, le tar officiel avec son sha256, et le piège `ZONE_FIN`.
- **Serveur Linux** : fusion uniquement sur **feu vert écrit du mainteneur**, après une R2 Linux (défaut ×2 avec empreintes exactes).
- **Fin de passe** : marqueurs `P7UMVK_FIN`.

## 5. Risques

| Risque | Probabilité / impact | Parade |
|---|---|---|
| **Disque Mac** : 5,3 Gio libres | certain / bloquant pour C, tendu pour les runs (install-7u + frames) | nettoyage validé §3 avant l'étape 4 ; C exige ≥ 15 Gio |
| **RAM et émulateur** de 6 Go sur une machine de 16 Go | élevé / OOM, ou perturbation du banc M4 de la supervision | règle `memory_pressure` ; étapes 4 à 7 seulement quand l'émulateur est arrêté, sur feu vert |
| **Le correctif `ZONE_FIN` n'est pas dans main** | certain / un Mac synchronisé sur main meurt à la frame 466 | étape 3.1 prioritaire |
| **`MAP_FIXED` silencieux** : alias de `MAP_FIXED_NOREPLACE` | faible / corruption de mémoire hôte non détectée | garde-fou de bornes, étape 3.1 |
| **Dérive Homebrew** (`brew upgrade`) | moyen / empreintes PPM qui changent sans changement de code | `brew pin` (étape 1) ; référence figée = tar officiel avec sha256 |
| **`portability_subset` non activé** aujourd'hui | moyen / comportement non défini, invisible sans validation | étapes 3.4 et 7 |
| **Triangle fans** non supportés par MoltenVK, si `triangleFans=0` | inconnu (aucune erreur à ce jour) / draws perdus | compteur `[BILAN]`, conversion seulement si > 0 |
| **Samplers à comparaison** (ombres 7v2/7v3) | inconnu / ombres absentes ou fausses sur Metal | étape 6 ; regarder `mutableComparisonSamplers` dans la ligne subset |
| **Table SPIR-V 1 066 perdue** | certain / la référence 7u n'est pas reproductible exactement | seuils S5/S6 calés sur la table 1 634, connue ; la table 1 639 de main est consignée sans seuil |
| **Écarts dus à l'horloge** (f550–700 ; le Mac est ~1,7× plus rapide) | certain / faux positifs | plage exclue des seuils |
| **Variante `-privateapi`** du tar officiel | — | non retenue : elle utilise des API Metal privées ; à n'envisager que si un déclencheur de l'étape 9 la désigne |
| **Toolchain pour C** : Xcode 26 présent mais `xcode-select` pointe sur les CLT, `metal` introuvable | élevé si C / build cassé | `DEVELOPER_DIR` explicite ; si la toolchain Metal manque : `xcodebuild -downloadComponent MetalToolchain` (à vérifier) |
| **Divergence miroir et banc** : `~/code/reverse/sc2-partout-mac/recomp-sc2` est obsolète | élevé / patcher le mauvais arbre | base unique : le worktree serveur Linux de l'étape 3 ; le miroir 5a n'est pas touché |

## 6. Définition de fini (DoD)
- S1 à S8 verts.
- Étapes 6 et 7 consignées.
- Paquet de l'étape 8 qui tourne sans Homebrew MoltenVK.
- Décision sur l'option C écrite avec ses mesures.
- Patchs remontés sur une branche serveur Linux avec la R2 Linux verte.
- BANC-MAC et skill mis à jour.
- Aucun run lancé pendant que l'émulateur vivait.
