# Traduire un moteur D3D9 vers Vulkan dans une recompilation statique — la méthode complète

*Ce document décrit, de bout en bout, comment nous avons fait tourner le renderer
Direct3D 9 d'un jeu x86-32 de 2010 recompilé statiquement (StarCraft II: Wings of
Liberty, ~86 600 fonctions) sur un backend **Vulkan** natif — headless sur CI, MoltenVK
au banc macOS, Adreno sur Android — jusqu'à une mission de campagne jouable à ~28 FPS
sur téléphone. Tout le code évoqué ici est soit publié dans ce dépôt, soit régénéré
localement depuis **votre propre copie** du jeu. Rien du jeu n'est distribué.*

Public visé : les projets de recompilation statique (pcrecomp, ps3recomp et cousins)
et, plus largement, quiconque doit réimplémenter une API graphique fermée devant un
moteur qui l'appelle « pour de vrai ».

---

## 0. Trois principes directeurs

Ces principes ont plus valu que n'importe quelle technique individuelle :

1. **« Rien n'est inventé en silence. »** Toute méthode D3D9 appelée par le moteur et
   non gérée **arrête l'exécution en se nommant** (interface, n° de vtable, appelant) ;
   les méthodes volontairement sans effet sont des `m_ok` *comptés et journalisés*. Un
   device qui ment (succès factice sur une fonction critique) coûte des heures de
   diagnostic ; un device qui avoue coûte une ligne de log.
2. **Prouver le pipeline avant les pixels.** Avant d'avoir traduit un seul shader du
   jeu, nous faisions des draws **réels** avec un triangle dégénéré hors clip (le
   « mode lumière ») : plumbing Vulkan complet, zéro pixel. Tout l'outillage de mesure
   (compteurs de draws, empreintes d'images) s'est construit sur ce socle.
3. **Mesurer avant de corriger.** Chaque écran noir, chaque saccade, a d'abord eu son
   instrument (compteurs par appel, empreintes de frames, timestamps GPU, simpleperf)
   et son protocole A/B/A. Voir la rétrospective pour la méthode de mesure complète.

## 1. Le problème spécifique à la recompilation (vs Wine/DXVK)

Un portage par couche (wine + DXVK) intercepte une DLL ; une recompilation statique
n'a **rien à intercepter** : le code du moteur, recompilé en C, appelle directement
des adresses de vtables. Le « device D3D9 » doit donc vivre **dans l'univers mémoire
du jeu recompilé** — notre « arène invitée », un espace adressable 32 bits contigu
réservé côté hôte.

L'architecture qui en découle, trois étages strictement séparés :

| Étage | Fichier(s) | Rôle | Dépend du jeu ? |
|---|---|---|---|
| 1. Device D3D9 « factice mais cohérent » | `sc2_d3d9.c` | Objets COM dans l'arène, suivi d'état D3D9 brut, copies mémoire invitées | Connaît l'ABI COM/x86, pas le jeu |
| 2. Backend Vulkan pur | `sc2_d3d9v.c` | Cibles, samplers, pipelines, transferts — **C pur sans aucun état invité** | Non — testable seul |
| 3. Présentation | `sc2_vk_presente.c` (Android) | Swapchain, blit, files | Non — interposée au loader |

### 1.1 COM factice dans l'arène invitée

Les objets D3D9 du moteur sont des blocs `[vtbl, ref, données…]` alloués **dans
l'arène**, à des adresses virtuelles synthétiques (`base + n*0x1000 + 16*k`) que le
runtime sait décoder arithmétiquement : quand le code lifté appelle `pDev->lpVtbl->Release(pDev)`,
le dispatch retrouve l'objet et la méthode par soustraction, dépile les arguments
stdcall au mot près, et route. Une méthode inconnue arrête le run en se nommant
(principe 1).

Pourquoi c'est le bon modèle : le code recompilé **n'est pas modifiable** (ou
coûteusement, par re-lift). Tout ce que le runtime doit offrir, il doit l'offrir aux
adresses mêmes où le jeu croit le trouver.

### 1.2 Backend sans état invité

`sc2_dd9v` ne sait rien du CPU invité : il reçoit par draw un POD plat
(`d3dv_dessin` : blend, masque couleur, Z, cull, formats d'attributs, topologie…).
Conséquence : il se teste **sans le jeu** (`test_d3d9v.c` tourne sur n'importe quelle
machine avec un loader Vulkan), et le même binaire tourne sur Lavapipe, MoltenVK et
Adreno.

### 1.3 Présentation interposée, jamais invasive

Sur Android, la swapchain est ajoutée **à l'insu du backend** : le présentateur
s'interpose sur `vkGetInstanceProcAddr` (linker `--wrap=dlsym`), complète les
extensions de surface, ouvre une seconde file de la même famille, et observe
`vkCreateImage` pour reconnaître le back buffer. Le backend reste headless ; la
présentation est une couche amovible. Au banc macOS, le même backend charge MoltenVK
directement (`VK_KHR_portability_subset` géré avec journal de ses limites).

## 2. Couvrir l'API : inventer moins, refuser proprement

- **Inventaire d'abord** : la table d'imports (IAT) du binaire dit exactement ce que
  le moteur peut appeler (`d3d9.dll`, `d3dx9_41.dll` : 18 imports chez nous — c'est
  petit) ; la trace d'exécution dit ce qu'il appelle *vraiment*. On implémente
  l'intersection, on refuse le reste *en se nommant*.
- **Caps cohérentes** : le `D3DCAPS9` annoncé (SM 3.0, formats, limites) doit être
  celui qu'on tient : le moteur *essaie* ce qu'on lui dit. Nous annoncions un
  « GeForce 8600 GT » de 2007 — assez pour SM3, assez proche pour les chemins du jeu.
- **Refus exploratoires pilotés par variable** : `CheckDeviceFormat` accepte tout par
  défaut mais peut refuser *à la demande* (`SC2_FORMATS_REFUS`) — c'est ainsi qu'on
  découvre les chemins de repli du moteur (le refus du R32F a révélé la voie des
  ombres, cf. §6.5).
- **Ce qui n'existe pas pour nous** : state blocks (no-op tracés), `ProcessVertices`,
  gamma/palettes (`m_ok` comptés), multisample refusé à 1×. Le moteur 2010 s'en
  accommode — le nôtre aussi.

## 3. Modèle d'état : brut au Set, traduit au Draw

Choix fondateur : **stocker l'état D3D9 brut** (render states `rs[256]`, sampler
states `ss[20][14]`, texture stages `tss[16][33]`, transforms `xf[512][16]`,
constantes VS/PS F/I/B + compteurs de génération) et ne **traduire vers Vulkan qu'au
moment du draw**, dans un POD jetable.

Pourquoi :

- Les `Set*State` deviennent des écritures de table — pas de pipeline Vulkan touché,
  pas de synchronisation, et les `Get*State` du moteur retombent sur la vérité.
- La divergence d'état se paie **une fois**, en création de pipeline : la clé de
  pipeline est une struct entière hachée FNV **sur les mêmes octets que le memcmp**
  d'égalité (modules VS/PS, topologie, formats d'attributs, strides, blend, profondeur,
  drapeaux d'ombres). Un état change → autre clé → autre pipeline. Pas de delta
  tracking incrémental à maintenir, pas de bug d'état résiduel : le memcmp garantit
  qu'on ne réutilise un pipeline que si *tout* l'état pertinent est identique.
- Le cache `VkPipelineCache` est **persistant sur disque** (écriture atomique
  périodique) : après le premier lancement, plus aucun à-coup de compilation.

Viewport et scissor sont les deux seuls états **dynamiques** du pipeline. Le
fixed-function (transforms, lumières, TSS) est mémorisé et rendu aux `Get*`, jamais
traduit : SC2 est 100 % shaders SM2/3 — vérifiez ce point *avant* de vous lancer,
c'est lui qui décide de la taille du chantier.

## 4. Descripteurs : un chemin rapide, un repli honnête

- **Chemin primaire : `VK_KHR_push_descriptor`.** Un seul set : bindings 0/1 = UBO de
  constantes VS/PS, 2+k = sampler k, 18+k = sampler à comparaison (ombres). Mise à
  jour **par draw** par `vkCmdPushDescriptorSetKHR` — zéro allocation par frame.
- **Repli classique** (extension absente — Lavapipe, les GPU d'émulateur Android, ou
  coupure volontaire) : un set par draw alloué dans le pool **de l'emplacement de
  frame** (4096 sets), pool remis à zéro quand l'emplacement revient après sa fence.
  Piège mesuré : en mode classique **tous** les bindings doivent être valides —
  Lavapipe déréférence la vue nulle (SIGSEGV) — d'où textures factices blanches pour
  les samplers inutilisés.
- **Samplers** : cache à ≤ 64 clés compactées (filtres, biais quantifié au 1/16,
  maxmip, aniso, bordure réduite aux 3 couleurs Vulkan canoniques).
- **Push constants** : demi-pixel (VS) et alpha-test (PS) — les deux seuls états
  traduits à la volée dans les shaders (cf. §6.3).

## 5. Textures et surfaces : là où se cachent les pièges

### 5.1 Formats et BC logiciel

Table de correspondance D3D9→Vulkan pour tout ce qui se mappe (A8R8G8B8→B8G8R8A8,
X8→swizzle A=ONE, L8/A8L8 par swizzle, R5G6B5, floats 16/32), conversion pixel par
pixel pour les autres (R8G8B8, A4R4G4B4…). **DXT1/2/3/4/5 → BC1/2/3 si
`textureCompressionBC`, sinon décodage CPU** conforme à la spec Khronos S3TC —
l'Adreno et les émulateurs n'ont pas toujours BC, et le décodeur logiciel est un
chemin de premier ordre : testé contre le GPU à ±8 près, il a débloqué l'écran-titre
complet là où d'autres voient un aplat.

### 5.2 `LockRect(pRect)` : le bug invisible d'une semaine

L'atlas de glyphes du moteur (polices A8L8) se remplit glyphe par glyphe par des
`LockRect` **avec rectangle** ; un device qui ignore `pRect` écrase chaque glyphe en
(0,0) — l'UI reste noire sans aucune erreur. `pBits` doit pointer sur le coin
haut-gauche du rectangle (bloc 4×4 contenant pour le DXTn). Morale : les rectangles
de verrouillage sont une **fonctionnalité**, pas un détail.

### 5.3 La famille relecture/composition

- `UpdateSurface`/`UpdateTexture` : copies memmove **invitées** ligne à ligne (blocs
  DXTn alignés), génération de contenu incrémentée.
- `StretchRect` = `vkCmdBlitImage` (LINEAR seulement hors profondeur).
- `ColorFill` = bascule de cible + `vkCmdClearAttachments` + restauration.
- **`GetRenderTargetData` doit vraiment relire** : notre no-op initial bloquait la
  composition des sprites d'unités — le moteur dessine les unités *depuis* ses cibles
  de rendu relues. Implémentation : relecture GPU RGBA → réécriture BGRA en mémoire
  invitée (bornée 4096²). Coût connu : ~231 relectures/session vident le GPU — à
  traiter un jour.
- `D3DXLoadSurfaceFromMemory` : cœur de conversion **testable seul** + glue de lecture
  invitée **faute-sûre** par `process_vm_readv` sur soi-même (EFAULT au lieu de
  SIGSEGV pour un pointeur invité douteux — testé contre une page `PROT_NONE`).
- **TGA** : la couche D3DX du jeu ne voit que du DDS — la minicarte de carte (TGA,
  tous sous-formats, RLE) est décodée en DDS en mémoire. Sans ça : minicarte noire,
  sans erreur.

### 5.4 Mips, cubes, volumes

Tous les niveaux sont uploadés par un `vkCmdCopyBufferToImage` **multi-régions**, la
chaîne bornée aux niveaux réellement présents (les DDS du jeu mentent parfois sur le
count), cubes = 6 couches × chaîne. `AUTOGENMIPMAP` : seul le niveau 0 est servi
(tracé).

## 6. Les shaders : traduire SM2/3 sans le source

Le cœur du chantier. Le moteur compile ses shaders HLSL à l'installation dans un cache
(`baselineCache.bin`, **75 797 bytecodes distincts** chez nous) ; il ne nous donne
jamais que du **bytecode D3D9 SM2/3**.

### 6.1 Chaîne hors-ligne

```
cache du jeu ──extraire-shaders.py──▶ bytecodes .bin
bytecode ──d3d9sm.py (décodeur SM2/3)──▶ instructions
instructions ──d3d9-vers-spirv.py──▶ GLSL 450
GLSL ──glslangValidator -V + spirv-val──▶ SPIR-V validé
SPIR-V + métadonnées ──▶ table embarquée (C) / pack (binaire mmap)
```

**Seuls les SPIR-V qui passent `spirv-val` entrent en table.** Ce que le traducteur
refuse encore : prédicats, `call`/`label`, `oFog`/`oPts`, registres `t` en ps_3 — le
draw correspondant tombe en repli (§6.4). Le contrôle de flux if/rep/break passe.

### 6.2 L'interface verrouillée avec le backend (le contrat)

Avant de générer le moindre SPIR-V, figer le contrat — chaque divergence coûte un
redémarrage complet de la table :

- **Y inversé** en sortie VS (D3D9 rend « haut vers le bas »), **Z [0,1] conservé**.
- **16 varyings `vec4` distincts** — pas de tableaux : l'Adreno refuse l'appariement
  tableau↔scalaire entre étages VS/PS. (Leçon matérielle payée une fois.)
- **Demi-pixel** (VS) et **alpha-test** (PS) passés par **push constants**, pas par
  variables de recompilation : le D3D9 offsette de ½ pixel ; sans ça, tout l'UI est
  décalé d'un pixel et l'alpha-test logiciel du moteur affiche des franges.
- **Les métadonnées d'interface voyagent avec le SPIR-V** : entrées VS (registre,
  usage, index) et types de samplers sont stockés dans la même structure que le
  binaire — au draw, le backend apparie déclaration D3D9 ↔ entrées VS **sans
  re-parser le bytecode**.
- **Identité = FNV-1a 64 du bytecode, jeton par jeton, commentaires sautés** : le même
  hash au runtime (quand le moteur crée le shader), dans le traducteur et dans le
  pack. Dédoublonnage à la création ; les adresses d'objets ne sont jamais réutilisées
  → les caches par hash sont sûrs.

### 6.3 La table embarquée et le mode « lumière »

La table (`sc2_shaders_spv.h`) est **générée localement par chaque utilisateur**
depuis son propre cache — rien de dérivé du jeu dans ce dépôt ; le runtime l'inclut
par `#if __has_include`. Couverture au 20/10 : **2 856 traduits** (boot, menu,
missions jouées, cinématique du bar) sur 75 797 — toute scène inédite perd des draws
en attendant le pack complet.

Le **repli « lumière »** : un draw sans module SPIR-V n'est pas silencieusement sauté —
il est rendu par un pipeline **réel** dont le VS place un triangle dégénéré hors clip :

```glsl
gl_Position = vec4(2.0, 2.0, 0.5, 1.0);   // shaders/lumiere.vert
```

Le plumbing Vulkan s'exécute (et se mesure), zéro pixel ne ment à l'écran. Chaque
draw ainsi replié est **compté** (« draws lumière ») : c'est le compteur qui a mené au
diagnostic de la cinématique du bar (§6.6).

### 6.4 Ombres : sampler à comparaison sans toucher au défaut

Diagnostic mesuré d'abord : le moteur **n'échantillonne jamais** sa surface de
profondeur D32 — il attend que le *sampler* fasse la comparaison. Correctif :

- binding 2+k : vue **brute** de la texture de profondeur ; binding 18+k : même vue
  avec **sampler à comparaison** `LESS_OR_EQUAL` (PCF 2×2) ;
- le traducteur émet
  `profondeur_k != 0 ? texture(sampler2DShadow 18+k, uv+réf) : texture(sampler2D 2+k, uv)`
  où `profondeur_k` est une **constante de spécialisation** (ID 100+k) ;
- **un seul binaire PS** sert les deux chemins ; la valeur par défaut (0) reproduit
  octet pour octet la sortie d'avant — prouvé par empreintes.

Les biais de profondeur D3D9 (`SLOPESCALEBIAS`, `DEPTHBIAS`) sont portés aux états
Vulkan équivalents : pas d'acné. Les ombres sont opt-in (`SC2_OMBRES=1`) tant que la
sortie par défaut n'a pas été re-certifiée visuellement.

### 6.5 Le pack SPIR-V (couverture complète, à la demande)

Format v1 (`docs/PACK-SPIRV.md`) : fichier **mmap** — en-tête 64 o (magie
`SC2SPV01`, dénombrement), index trié de 96 o par entrée (hash FNV, `ps`, longueurs,
offset, métadonnées d'interface 16+16), blobs alignés sur 16 o, non compressé. La
table compilée reste prioritaire ; le pack sert les shaders qui lui manquent, en
repli dans la résolution par hash. ~2 Go estimés pour les 75 797 — c'est le chantier
en cours ; chaque motif de refus du traducteur (prédicats, `call`/`label`) est un
ticket de travail listé par le rapport de génération.

### 6.6 Étude de cas : le verre de la cinématique du bar

Le défaut : 57 % des draws sautés faute de SPIR-V (table de 2 501 à l'époque) ;
symptôme spectaculaire — un verre en tourbillons colorés, un bar invisible : une
passe sautée ne redessine **pas sa cible de rendu**, et le verre réfracte *les restes
de la scène précédente* — visible sur Adreno (mémoire recyclée non nulle), invisible
au banc Mac (mémoire neuve à zéro). Six hypothèses écartées une à une (DXT logiciel,
précision GLSL, temporaires, effacements, mips) avant la cause racine. Correctif :
capture ciblée de 355 bytecodes de la scène → table 2 856 → draws « lumière » de
18 387 à 278, verre propre. Leçons : (a) un GPU sans BC et une mémoire neuve à zéro
*masquent* ce genre de bug ; (b) un compteur de repli bien placé vaut dix captures
d'écran.

## 7. Présentation : Android sans réécrire le backend

### 7.1 La discipline SUBOPTIMAL (la bataille de l'écran noir)

Sur Android, en portrait natif tenu en paysage, `vkQueuePresentKHR` rend
`VK_SUBOPTIMAL_KHR` **à chaque frame** (preTransform IDENTITY ≠ rotation courante).
Recréer la swapchain sur SUBOPTIMAL (le réflexe documenté partout) **affame le
compositeur** : une création ≈ une frame, écran noir perpétuel — nous avions 4 809
créations pour 2 800 frames. Discipline finale :

- recréer **seulement** sur `VK_OUT_OF_DATE`, SUBOPTIMAL **avec changement de taille**,
  ou redimensionnement signalé par la plate-forme ;
- SUBOPTIMAL à taille inchangée : ignoré (journalisé une fois), rotation laissée au
  compositeur ;
- dégradation **bornée** : échecs répétés (5 en 10 s) → bascule définitive en copie
  CPU, réarmable par nouvelle fenêtre (≤ 8 fois) — jamais de boucle infinie.

Résultat : exactement **1 création** de swapchain sur un run de 2 800+ frames.

### 7.2 Deux frames en vol

Le présentateur fait le blit back buffer → image swapchain (rapport conservé, centré,
bandes noires) sur sa propre file. Chaînage : le backend signale un sémaphore
« rendu » que le blit attend ; le blit signale « blit fini » attendu par la soumission
suivante du backend ; **la fence du blit n'est attendue qu'au début de la présentation
d'après**. Règle Vulkan facile à violer : un sémaphore binaire signalé doit être
consommé **exactement une fois — y compris par une soumission vide** (nous en émettons
une délibérée quand rien ne fut dessiné). En arrière-plan, c'est la fin du
`vkQueueWaitIdle` par frame : à lui seul, ce levier a fait passer 12,5 → 21,3 FPS.

### 7.3 MoltenVK au banc

Même backend, chargement explicite du loader (variable d'environnement, voisin de
l'exécutable, loader Khronos, Homebrew — dans cet ordre), `VK_KHR_portability_enumeration`
exigé par le loader, sous-ensemble portability activé **avec journal de ses limites** :
`triangleFans` absent (conversion fan→liste à écrire) et `samplerMipLodBias` clampé
à 0. Le banc macOS fait tourner le moteur complet en natif — c'est lui qui a permis
d'itérer sans téléphone.

## 8. Transferts : l'anneau, les époques, les générations

- **Un grand tampon de staging mappé**, une **moitié par emplacement de frame**
  (défaut 256 Mo) ; réservation « tout ou rien » : si un frame dépasse sa moitié,
  soumission forcée qui attend la frame précédente — **comptée**. C'est ce compteur
  qui a révélé que notre anneau de 64 Mo débordait en milieu de frame (1,93
  soumission/Present, 9,2 ms d'attente de fence — un faux GPU saturé : les timestamps
  montraient le GPU occupé < 40 % du temps). Passage à 256 Mo : 1,07 soumission,
  0,4 ms, et +4 FPS.
- **Époques d'anneau** : tout offset mis en cache (constantes, plages volatiles) n'est
  valable que pour l'époque courante, incrémentée à chaque soumission — invalidation
  gratuite, sans remise à zéro ni désallocation.
- **Constantes par générations** : les blocs VS/PS sont assemblés **au fil des
  `Set*ShaderConstant`** (le mot changé est écrit à sa place ; la génération n'avance
  que si un mot change) ; au draw, (génération, époque) inchangées ⇒ l'emplacement
  d'anneau est réutilisé sans recopie. Le reste — ces 4 608 octets recopiés quand
  même à chaque dessin changé — est le prochain chantier (ne recopier que la plage
  de registres réellement lue par le shader : le traducteur la connaît).
- **Tampons statiques** : VB/IB non dynamiques copiés **une fois** dans un `VkBuffer`
  dédié, lié à l'offset demandé ; régénération → `VkBuffer` neuf, ancien libéré **en
  différé par emplacement** ; **garde de volatilité** : plus de deux remplacements ⇒
  le tampon est reclassé volatile et repart à l'anneau. (Le cache sans garde était
  *contre-productif* sur Adreno — mesuré.)
- **Plages volatiles** (dernière passe) : table par adressage ouvert, clé
  (objet, génération, décalage, taille, genre) → emplacement d'anneau valide pour
  l'époque courante : la même plage redessinée N fois par frame n'est copiée qu'une
  fois (552 903 recopies → ~3 700 plages distinctes au banc).

## 9. Les décisions contre-intuitives qui ont payé

1. Le triangle dégénéré « mode lumière » : un pipeline **réel** pour prouver le
   plumbing avant d'avoir un seul shader traduit.
2. La clé de pipeline **hachée sur les octets du memcmp** : l'égalité de hachage *est*
   l'égalité d'état — le cache ne peut pas mentir.
3. **Traduire au draw, pas au Set** : des milliers de `Set*State` par frame deviennent
   des écritures mémoire.
4. Les **métadonnées d'interface dans la table de shaders** : le draw n'apparie jamais
   deux représentations du même bytecode.
5. **Un seul binaire PS pour ombres on/off** par constantes de spécialisation — le
   défaut reste l'octet près celui d'avant, prouvé par empreintes.
6. Le **repli DXTn logiciel en chemin de premier ordre**, testé contre le GPU.
7. `process_vm_readv` **sur soi-même** pour lire la mémoire invitée : EFAULT au lieu
   du SIGSEGV sur pointeur douteux.
8. **Ignorer SUBOPTIMAL à taille constante** — contre toute la littérature, dicté par
   la mesure (1 création de swapchain au total).
9. **Dégradation bornée avec bascule définitive** copie CPU : l'échec de présentation
   ne peut pas boucler.
10. L'**interposition au loader** plutôt que la modification du backend : la couche
    présentation est amovible, le backend testable.
11. **Sémaphores consommés une fois, soumissions vides incluses.**
12. Les **époques d'anneau** comme invalidation gratuite de tous les caches d'offsets.
13. **Constantes assemblées au Set, réutilisées au draw** : la génération ne bouge que
    si un mot change.
14. **Garde de volatilité** avant de croire un tampon « statique » (le moteur en
    régénère plus que prévu).
15. **Un mode unique 1280×720 mensonger assumé** (le moteur croit rendre son écran
    fixe ; le présentateur adapte) : le HUD et les coordonnées d'entrées restent
    cohérents.

## 10. Limites connues (au 20/10)

MRT ignoré (une couleur) ; scissor de draw non appliqué ; sRGB ignoré (UNORM
partout) ; fixed-function jamais traduit (sans objet pour SC2 — pas pour vos jeux
antérieurs à 2002) ; prédicats/`call`/`label` non traduits ; `GetRenderTargetData`
coûteux ; fan→liste MoltenVK à écrire ; couverture shaders bornée à la capture en
attendant le pack.

## 11. Checklist de transfert à un autre jeu

1. Dumper la table d'imports — elle borne le périmètre API réel.
2. Tracer les appels réels au boot ; implémenter l'intersection ; **refuser en se
   nommant** le reste.
3. Headless + Lavapipe d'abord ; « N pixels non noirs » est un jalon mesurable.
4. Mode « triangle dégénéré » pour instrumenter avant de traduire.
5. Figer le contrat d'interface des shaders **avant** de générer la moindre table.
6. Hash unique (FNV du bytecode, commentaires sautés) partagé runtime/traducteur/pack.
7. Compteur de repli par draw — c'est lui qui mène aux diagnostics.
8. Mesurer (A/B/A, empreintes de frames, timestamps GPU) avant chaque « optimisation » ;
   deux de nos leviers « évidents » étaient des pertes mesurées.
9. Attention aux matrices mémoire-neuve : un bug invisible au banc (mémoire à zéro)
   peut être visible sur le matériel (mémoire recyclée).
10. Ne jamais mentir au moteur : un succès factice coûte plus cher qu'un arrêt nommé.
