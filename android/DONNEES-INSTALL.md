# Mettre SA copie de StarCraft II sur l'appareil (Galaxy Fold) — données de la campagne

Passe 7y. Ce document dit **quels fichiers** du jeu copier, **combien** cela pèse, **comment** les pousser, et
ce qui se passe s'il en manque. La liste de référence est `android/manifest-install.txt`. Les preuves (runs avec et
sans chaque archive) sont dans `android/TRACE-PASSE7Y.md`.

Les données viennent de **votre** DVD de *StarCraft II : Wings of Liberty* (version Base15405, anglais enUS). Elles ne
sont jamais dans le dépôt git ni dans un APK.

## 1. Combien de place

| Classe | Contenu | Taille |
|---|---|---|
| **A — démarrage** | `SC2.exe`, `Core.SC2Mod` (base + enUS + index des locales), cache de shaders, dossier `Support/` vide | **0,20 Go** |
| **B — campagne** | `Liberty.SC2Mod`, `LibertyMulti.SC2Mod`, `Liberty.SC2Campaign` (les 33 cartes de la campagne, ses modèles, sons, dialogues), `LibertyStory.SC2Campaign` | **7,34 Go** |
| **À pousser (A + B)** | 20 fichiers + 1 dossier | **7,54 Go** (7 538 407 027 octets) |
| C — inutile | `Battle.net.MPQ`, `Challenges.SC2Mod`, lanceur `StarCraft II.exe`, DLL Windows de `Support/` et de `Versions/` (le runtime les émule) | 0,12 Go, **à ne pas pousser** |

Prévoir **8 Go libres** sur l'appareil, plus 45 Mo pour l'APK.

Le détail par fichier (taille exacte et rôle) est dans `android/manifest-install.txt`.

## 2. Obtenir les fichiers (une fois, sur le devbox)

L'installation est extraite du DVD par deux passes :
- `scripts/extraire-install.sh` (passe 6a) produit `extraction/install` ;
- `AJOUTS_7H=1 scripts/extraire-install.sh` (passe 7h) produit `extraction/install-ajouts-7h`, avec les archives de
  la campagne et de la locale enUS.

On construit ensuite sur le devbox une **vue** limitée au manifest. Ce sont des liens symboliques, rien n'est copié :

```sh
cd ~/code/reverse/sc2-partout          # ou le worktree qui porte la passe 7y
android/outils/vue-manifest-7y.sh ~/sc2-vue-campagne AB
# → « vue AB : 20 liens, 1 dossier(s), 7538407027 octets (7.538 Go) ; absents : aucun »
```

`absents : aucun` doit apparaître. Sinon, une archive manque à l'extraction : relancer la passe concernée.

## 3. Copier sur le Mac, puis pousser sur l'appareil

Le Fold est branché au Mac (USB, ou débogage sans fil). Il faut d'abord rapatrier les vrais fichiers sur le Mac.
`-L` suit les liens de la vue :

```sh
rsync -aL --progress devbox:sc2-vue-campagne/ ~/sc2-install-campagne/      # 7,54 Go
```

Ensuite, installer l'APK et pousser les données :

```sh
adb install -r sc2-partout-vk.apk
cd ~/dev/sc2-partout                     # la copie du dépôt sur le Mac (git pull)
bash android/outils/pousser-donnees-7y.sh ~/sc2-install-campagne
```

Le script lit `android/manifest-install.txt` à côté de lui. Il :
- cherche chaque fichier des classes A et B du manifest dans le ou les dossiers sources donnés ;
- le pousse dans `/sdcard/SC2Partout/install/<chemin>` **seulement s'il manque ou si sa taille diffère**. Relancer
  après une coupure USB reprend où l'on en était ;
- crée le dossier `Support/` ;
- donne à l'app l'accès « tous les fichiers » (`appops … MANAGE_EXTERNAL_STORAGE allow`) ;
- affiche un bilan (`classes AB : 20 fichiers, … poussés …`). Il rend une erreur si un fichier est absent des sources
  ou si un push échoue.

`adb` est cherché dans le PATH, puis dans `$ANDROID_SDK_ROOT`, `~/Library/Android/sdk` et `~/tools/android-sdk`.
On peut aussi le forcer avec `ADB=…`.

Sans adb, on peut copier à la main le contenu de `~/sc2-install-campagne/` dans `SC2Partout/install/` de la mémoire
interne, par câble ou Android File Transfer. L'arborescence doit être exactement celle du manifest (`Mods/…`,
`Campaigns/…`, `Versions/…`, `Support/`). Il faut ensuite accorder « Gestion de tous les fichiers » à SC2 Partout dans
Réglages › Applications.

### Où l'app cherche les données

Elle prend le premier dossier qui contient `Versions/Base15405/SC2.exe` :
1. `SC2_INSTALL` de `sc2.env` ;
2. `/sdcard/SC2Partout/install` (recommandé) ;
3. `/sdcard/Android/data/fr.sc2partout/files/install` (aucune permission nécessaire ; dans ce cas, pousser avec
   `SD=/sdcard/Android/data/fr.sc2partout/files`).

## 4. Réglages : `/sdcard/SC2Partout/sc2.env`

La voie qui marche aujourd'hui pour jouer est le **lancement direct d'une carte** de la campagne. Le menu n'est
pas navigable : voir §6.

```sh
cat > /tmp/sc2.env <<'FIN'
SC2_RUN_SANS_BNET=1
SC2_ARGS=-run Maps/Campaign/TRaynor01.SC2Map
FIN
adb push /tmp/sc2.env /sdcard/SC2Partout/sc2.env
```

Les 33 cartes de la campagne sont dans `Campaigns/Liberty.SC2Campaign/Base.SC2Maps`. Il suffit de remplacer
`TRaynor01` par l'une d'elles :
- THanson01, THanson02, THanson03A, THanson03B ;
- THorner01, THorner02, THorner03, THorner04, THorner05S ;
- TRaynor01, TRaynor02, TRaynor03 ;
- TTosh01, TTosh02, TTosh03A, TTosh03B ;
- TTychus01, TTychus02, TTychus03, TTychus04, TTychus05 ;
- TValerian01, TValerian02A, TValerian02B, TValerian03 ;
- TZeratul01, TZeratul02, TZeratul03, TZeratul04 ;
- TStory01, TTutorial01, TArcade, TSimulator.

Seule TRaynor01 a été validée de bout en bout par les passes 7q, 7u et 7v.

Les autres réglages (écran, FPS, journal) sont décrits dans `docs/PIPELINE-ANDROID.md` §4-§5.

## 4bis. Réglages du jeu : `/sdcard/SC2Partout/Variables.txt`

Le dossier `sc2-c` du stockage interne (le C:\ émulé) est effacé à chaque lancement : sans transfert, les
réglages graphiques, son, etc. changés en jeu (fichier `Variables.txt` du jeu) seraient perdus. À chaque
lancement, l'app fait donc, dans cet ordre :

1. **Sauvegarde** (avant l'effacement) : le `Variables.txt` du run précédent
   (`sc2-c/Users/sc2/Documents/StarCraft II/Variables.txt`) est copié vers
   `/sdcard/SC2Partout/Variables.txt` — sauf si le fichier fait plus de 64 Kio, s'il ne contient pas la
   section `GraphicsOption` (garde-fou : ce n'est pas un Variables.txt), ou si `sc2.env` pose
   `SC2_VARIABLES_FIGE=1` (l'utilisateur fige son préréglage : les changements en jeu ne sont pas repris).
2. **Semis** (après l'effacement) : la source fournie par `SC2_VARIABLES` dans `sc2.env` (chemin
   quelconque), sinon `/sdcard/SC2Partout/Variables.txt`, est copiée dans le C:\ neuf, là où le jeu le
   relit. Si aucune source n'existe, le jeu démarre sur ses réglages par défaut.

Usage : déposer un préréglage (`adb push Variables.txt /sdcard/SC2Partout/Variables.txt`), jouer, les
réglages changés en jeu reviennent au lancement suivant. Le suivi est dans logcat, préfixe `[VARS]`.

## 5. Vérifier

Commande : `adb logcat -s SC2Partout`.

| Ligne de logcat | Signification |
|---|---|
| `SC2_INSTALL=/sdcard/SC2Partout/install` | données trouvées |
| `[PHASE] chargement à …` | la carte est ouverte |
| `[MPQ] Campaigns\Liberty.SC2Campaign\Base.SC2Maps → Maps\Campaign\TRaynor01.SC2Map\…` | la carte est lue (24 fichiers pour TRaynor01) |
| `données introuvables` | rien dans les 3 dossiers (§3), ou accès « tous les fichiers » non accordé |

## 6. Ce qui casse s'il manque quelque chose

Mesuré sur le banc x86 du devbox : même moteur, run mission TRaynor01 de 300 s. Le détail est dans
`android/TRACE-PASSE7Y.md`.

| Retiré | Effet |
|---|---|
| rien (A + B, ce manifest) | identique à l'installation complète : écran de chargement WoL, 24 fichiers de carte, musique et voix de mission |
| `Campaigns/Liberty.SC2Campaign/Base.SC2Maps` (72 Mo) | dialogue **« Unable to open map. »** : aucune mission |
| `Mods/Liberty.SC2Mod/base.SC2Assets` (2,4 Go) | la partie démarre, mais le monde est remplacé par des **formes de secours** (sphères violettes) et il n'y a pas de musique |
| `Campaigns/Liberty.SC2Campaign/base.SC2Assets` (3,5 Go) | la mission démarre (scène d'intro), mais les **modèles propres à la campagne** sont remplacés par la forme de secours (sphère), et il y a 284 lectures en moins |
| `Mods/Liberty.SC2Mod/enUS.SC2Assets` (336 Mo) | aucun effet visible dans les 300 premières secondes. Contient les voix enUS des unités (1 540 .ogg) et les textures de texte : à garder |
| `Campaigns/Liberty.SC2Campaign/enUS.SC2Assets` (934 Mo) | **dialogues muets** (8 voix introuvables dès TRaynor01) ; l'image est intacte |
| `Campaigns/LibertyStory.SC2Campaign/Base.SC2Data` (2 Mo) | la mission charge, mais la bande-son choisie change. Ce sont les données de l'Hypérion (entre les missions) : à garder |
| `Versions/Shaders14513/baselineCache.bin` (142 Mo) | le jeu tourne (chargement, sons) mais l'**écran reste noir** |
| `Mods/Liberty.SC2Mod` (tout) | **erreur fatale du jeu** au bout de 16 s : le jeu écrit `GameLogs\…Error` et veut lancer `ErrorReporter.exe` (`CreateProcessW` non implémenté), puis l'app s'arrête proprement |
| `Mods/Liberty.SC2Mod/Base.SC2Data` seul | dialogue « UI/GameLaunchInvalidFiles » (passe 7m : c'est le fichier que le jeu valide) |
| `Battle.net.MPQ`, `Challenges.SC2Mod`, DLL de `Support/` | **rien** : ils ne sont pas dans ce manifest, et le run A + B est identique au complet |

## 7. Note : la voie « menu » (sans `SC2_ARGS`)

Constat du banc émulateur (supervision, 23:42 UTC) : sans `SC2_ARGS`, l'app s'arrête proprement au bout de 2 s.
Ce que montrent les runs de la passe 7y sur le devbox :
- **Si `Mods/Liberty.SC2Mod` manque**, le jeu ouvre son rapport d'erreur fatale et l'app s'arrête proprement. Il le
  fait aussi pour d'autres fichiers attendus : c'est l'arrêt « propre » typique. Dans logcat :
  `[ARRÊT] … CreateProcessW appelé mais NON IMPLÉMENTÉ (appelant 0x0099DCD5)`, précédé de
  `GetFileAttributesW("…\Mods\Liberty.SC2Mod") → absent` (ou d'un autre fichier absent).
- **Avec toutes les données (A + B, ou tout le DVD)**, la voie menu tourne (400 frames, rc=0). Mais l'**écran reste
  noir** : le jeu en enUS reste sur « Loading », et 320 draws n'ont pas de shader traduit.
- **Sans les archives enUS** (vue `extraction/install-base` des passes 7m/7n/7p), le menu de campagne s'affiche bien.
  Le dashboard de campagne (« Tumor », Battle.net) n'est cependant pas cliquable (passe 7p).

Ce n'est pas un problème de données : le manifest est complet pour la voie menu. C'est une limite du moteur (des shaders
absents de la table actuelle) et de l'interface Battle.net. En attendant, on joue par le lancement direct
(§4). Pour diagnostiquer un arrêt à 2 s sur l'appareil, relever dans logcat la 1re ligne `→ absent` qui précède
`[ARRÊT]`.
