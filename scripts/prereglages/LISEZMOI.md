# Préréglages graphiques (`Variables.txt`) — V1.5, 22/10/2026

Des **clés de réglage** de StarCraft II (aucun contenu du jeu), à semer par `SC2_VARIABLES=<fichier>` (avec
`SC2_VARIABLES_FIGE=1` pour les réécrire à chaque lancement) : `scripts/banc-options.sh`, `BANC_VARIABLES` de
`scripts/banc-mac.sh`, `sc2.env` du Fold. Le jeu lit `C:\Users\sc2\Documents\StarCraft II\Variables.txt`, soit
`$SC2_ROOT/Users/sc2/Documents/StarCraft II/Variables.txt` (`hpath()` de `recomp-sc2/runtime/sc2_iat.c`).

| fichier | contenu | provenance |
|---|---|---|
| `low.txt`, `medium.txt`, `high.txt`, `ultra.txt` | Graphics Quality ET Texture Quality au même niveau | écrits par le jeu lui-même après « Accept » dans son menu Options (banc Mac, 22/10), puis expurgés des clés hors graphismes |
| `fold.txt` | **Ultra** : préréglage par défaut du Fold (décision du mainteneur du 23/10 après S1 ; Medium + textures High du 22/10 au 23/10) | copie de `ultra.txt` ; copie intégrée dans `android/jni/sc2_android_main.c` (`VARS_FOLD`), semée quand l'utilisateur n'a aucun Variables.txt |
| `canon.txt` | ce que le jeu détecte et écrit seul pour la GeForce 8600 GT 512 Mo annoncée (Medium, textures Low) | Variables.txt écrit par le jeu sans semis (diag/vide du 21/10) ; canon épinglé (`scripts/banc-options.sh --canon`) |

Faits mesurés le 22/10 (banc Mac, `~/dev/sc2-bancs/20261022/options/exp/`) :
- le jeu **re-détecte** ses réglages, et écrase tout le préréglage semé, quand `lastDeviceId` manque ou n'est pas
  le DeviceId de la carte annoncée (matrice du 22/10 sans la clé : Low, Medium, High et Ultra relus tous en
  Medium, textures Low, `~/dev/sc2-bancs/20261022/options/matrice-sans-device/`). Les préréglages n'ont donc PAS
  cette clé (elle dépend de la carte) : le semis l'ajoute, 1026 (8600 GT, profil historique) ou 1507 (GTX 285,
  `SC2_GPU_PROFIL=dx10`) — `sc2_variables_device_id()`, `recomp-sc2/runtime/sc2_documents.c`. Une source qui l'a
  avec une autre valeur la voit remplacée (le `Variables.txt` de `/sdcard` du Fold, écrit sous la 8600 GT, semé en
  `dx10` : 1026 → 1507, réglages de l'utilisateur conservés). `canon.txt`, copie exacte de ce qu'écrit le jeu, garde
  son `lastDeviceId=1026` en historique (semé octet pour octet) ;
- le jeu ne réécrit `Variables.txt` qu'après un changement **accepté** dans le menu (semis puis « Accept » sans
  changement, ou High → Ultra → Accept : fichier intact) ; `scripts/entrees/options-accept.txt` déplace donc le curseur
  Gamma, une fois la mesure faite, pour obtenir la preuve d'application ;
- la qualité des textures tient en deux clés : l'option d'interface `GraphicsOptionTextureQuality[2]` (0 Low à 3 Ultra ;
  l'indice `[2]` reste le même quelle que soit la valeur) et la clé effective `texQualityLevel`, avec
  `targettexeldensity`, `TerrainTextureSize`, `TerrainTextureHiResCacheSize`, `TerrainTextureLowResCacheSize`. Semer la
  seule `GraphicsOptionTextureQuality[2]=2` affiche « High » dans le menu mais laisse les textures en Low (mêmes
  tailles et mêmes Mo téléversés, `SC2_TRACE=1`) : c'est l'erreur du « q-ultra » du 21/10 ;
- la liste Texture Quality du menu va de Low à Ultra (bulle d'aide : 128, 256, 512, 1024 Mo de VRAM) ; « Ultra » dans
  Graphics Quality ne touche pas aux textures ; `GraphicsOptionModelQuality=2` est le maximum (High) même en Ultra.
