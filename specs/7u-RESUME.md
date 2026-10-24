# 7u-MoltenVK — résumé (16/10/2026, session bg 86cd8f38)
1. État : la 7u tourne en ARM64 natif sur le M4 avec MoltenVK 1.4.2 de Homebrew (dlopen direct, sans loader). 1 200 f, rc=0, visuellement identique à la réf. x86 (`~/code/sc2-partout-mac/BANC-MAC.md`).
2. L'« arbitrage officiel vs sources » n'est consigné nulle part (NOTES, BUS, skill, transcript ddb9b36c) : opus2 est morte avant de recevoir sa mission. Il est reconstitué dans la SPEC §1.2.
3. Décision recommandée : Homebrew épinglé pour le développement ; tar officiel Khronos v1.4.2 (même tag, sha256 figé) pour la référence et l'embarquement ; compilation depuis les sources seulement sur déclencheur mesuré (SPEC étape 9).
4. Raisons d'écarter les sources pour l'instant : aucun bug MoltenVK identifié, 5,3 Gio de disque libre, Xcode non sélectionné, et l'émulateur vit sur le M4.
5. Urgent : main serveur Linux `835ba54` n'a ni le fallback MoltenVK ni `ZONE_FIN=0xF0000000`. Un Mac synchronisé sur main meurt à la frame 466.
6. Plan en 10 étapes : inventaire et `brew pin`, tar officiel, patchs (ordre de dlopen, portabilité, subset, CMake), build, runs A/B, ombres, validation, paquet, porte C, docs.
7. Critères mesurables S1–S8 (rc, 1 200 f, % de pixels à écart > 16 par plage de frames, 0 draw refusé, durée) ; DoD dans la SPEC §6.
8. Risques principaux : disque, RAM et émulateur, `MAP_FIXED` silencieux, `portability_subset` non activé, triangle fans, samplers d'ombres.
9. Aucun build ni run n'a été lancé dans cette session : uniquement de la lecture et un appel léger à l'API GitHub.
10. SPEC : `7u-moltenvk-SPEC.md`. Prochaine action : libérer du disque, puis étapes 1–2 (légères), puis feu vert pour l'étape 3.

## Mise à jour 7U1 (16/10/2026) — étapes 1 et 2 de la SPEC §4 (arbitrage validé : A + brew pin en dev, B pour référence/embarquement)
- Étape 1 : `brew pin molten-vk` fait ; `brew list --pinned` contient `molten-vk` (1.4.2). Seul `molten-vk` est épinglé (pas vulkan-loader ni vulkan-headers).
- Étape 2 : `MoltenVK-macos.tar` v1.4.2 téléchargé depuis GitHub Releases (`KhronosGroup/MoltenVK`, tag v1.4.2), sans extraction.
  - Taille : 59 563 008 octets (≈ 59,6 Mo).
  - sha256 : `f95765a6229cb7b915990a2890ce12ebe36a730b021545d3d52ae69ce4c4024e`
  - Chemins repérés dans l'archive : `MoltenVK/MoltenVK/dynamic/dylib/macOS/libMoltenVK.dylib` et `.../MoltenVK_icd.json`.
  - Copie locale (non versionnée) : `mvk-officiel-1.4.2/dl/` dans ce worktree ; empreinte dans `mvk-officiel-1.4.2/MoltenVK-macos.tar.sha256`.
- Non fait, par consigne : aucun build, aucun run, extraction, inventaire du dylib Homebrew (SPEC étape 1, partie inventaire), `lipo`/`otool`/`codesign` du dylib officiel (étape 2, partie extraction). Rien écrit hors du worktree.
- Émulateur vivant au moment de la mission (qemu-system-aarch64, pid 68700) : seules des opérations légères ont été exécutées.
