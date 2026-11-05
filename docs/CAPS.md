# D3DCAPS9 annoncées par le device simulé (V1.0, 22/10/2026)

`caps_ecrire` (`recomp-sc2/runtime/sc2_d3d9.c`, table `g_caps_masques`) remplit le D3DCAPS9 rendu par
`IDirect3D9::GetDeviceCaps` et `IDirect3DDevice9::GetDeviceCaps`. Jusqu'au 22/10, les champs de bits de `Caps` à
`LineCaps` (décalages 8 à 84) valaient tous **0xFFFFFFFF** : le runtime annonçait toutes les fonctions, y compris
celles qu'il n'a pas, et aussi des **restrictions** (textures en puissances de 2 seulement, carrées seulement…).
V1.0 les remplace par des masques explicites. La ligne `[CAPS]` du journal donne, au premier appel, les masques
annoncés et les valeurs lues par le jeu.

## Règle

1. **Un bit que le jeu lit garde sa valeur d'avant (1)**, implémenté ou non : le changer changerait le chemin du
   jeu, donc le canon. Son état est noté ci-dessous, et le chantier qui l'implémente est nommé.
2. **Tout autre bit vaut l'implémentation réelle** : 1 si le runtime ou le backend Vulkan fait ce que le bit
   promet, 0 sinon (fonction absente, état ignoré, restriction, bit indéfini).
3. Les caps restent **statiques** : `IDirect3D9::GetDeviceCaps` est appelé avant `CreateDevice` (lift : 009F6A9D
   puis 009F6B56), donc avant `d3dv_init`. Les limites du GPU Vulkan (anisotropie, biais de LOD nul sous MoltenVK)
   ne sont pas connues à ce moment-là.

## Ce que le jeu lit (preuve dans le lift)

Deux appelants seulement, un par interface, appelés une fois par run (39 journaux du 21/10, inventaire V1 §3) :

- `L_00A00FC0` (`src/lifted/recomp_07_2.c`, IDirect3D9::GetDeviceCaps en 00A01013, tampon `[ebp-0x144]`) lit :
  `NumSimultaneousRTs` (+240, en 00A0111E), `MaxUserClipPlanes` (+164, 00A01121), `MaxSimultaneousTextures` (+152,
  00A0112D), `TextureAddressCaps` (+76, 00A011CE, `shr 3 ; and 1` = **BORDER**), `MaxTextureWidth/Height` (+88/+92,
  00A011E3/00A011D4), `Caps2` (+12, 00A0131C : `and 0x20000` **FULLSCREENGAMMA**, `and 0x100000` **CANCALIBRATEGAMMA**).
- `L_009FFE90` (IDirect3DDevice9::GetDeviceCaps en 009FFF37) lit `VertexShaderVersion` (+196) et
  `PixelShaderVersion` (+204).
- Les deux passent le tampon à `L_009FFD20` (recomp_07_2.c:20488), qui lit `PrimitiveMiscCaps` (+32) `& 0x40000`
  **MRTINDEPENDENTBITDEPTHS** (009FFD26) et `& 0x4000` **INDEPENDENTWRITEMASKS** (009FFD39), `StencilCaps` (+136)
  `& 0x100` TWOSIDEDSTENCIL (009FFD49), `MaxVertexShaderConst` (+200), et les versions VS/PS.

**Aucun autre bit des champs de bits n'est lu.** Les retraits ci-dessous sont donc neutres pour le jeu (vérifié au
banc : canon, bar et TRaynor01 identiques, voir la fin du document). Ni la D3DX simulée ni la couche Android ne lisent
les caps (`caps_ecrire` n'est appelée que par les deux GetDeviceCaps).

Abréviations : *lu* = lu par le jeu ; *impl.* = implémenté ; numéros de ligne au commit V1.0.

## Champs de bits

### Caps (+8) : 0xFFFFFFFF → **0x00000000**

| bit | valeur | annoncé | état, preuve |
|---|---|---|---|
| READ_SCANLINE | 0x20000 | **retiré** | `v_GetRasterStatus` et `sw_GetRasterStatus` rendent toujours {InVBlank 0, ScanLine 0} (sc2_d3d9.c:662) : pas de vraie ligne de balayage ; GetRasterStatus : 0 appel en 39 runs |
| OVERLAY | 0x800 | retiré | D3D9Ex, absent |
| autres | — | retirés | indéfinis |

### Caps2 (+12, **lu**) : 0xFFFFFFFF → **0x20120000**

| bit | valeur | annoncé | état, preuve |
|---|---|---|---|
| FULLSCREENGAMMA | 0x20000 | **gardé (lu)** | non impl. : SetGammaRamp = `m_ok` (sc2_d3d9.c:1564), GetGammaRamp rend l'identité (l. 663). Lu en 00A01324 → [+0x29A4]. V2.8 tranche |
| CANCALIBRATEGAMMA | 0x100000 | **gardé (lu)** | non impl., idem. Lu en 00A0132A → [+0x29A8] |
| DYNAMICTEXTURES | 0x20000000 | gardé | impl. : Lock rend la vraie mémoire de la ressource |
| CANAUTOGENMIPMAP | 0x40000000 | **retiré** | non impl. : seul le niveau 0 est échantillonné pour une texture AUTOGENMIPMAP (`O_USAGE & 0x400` → 1 niveau, l. 1283 et 1297), GenerateMipSubLevels = `m_ok` (l. 262) |
| CANMANAGERESOURCE, CANSHARERESOURCE, RESERVED | 0x10000000, 0x80000000, 0x2000000 | retirés | absents (D3D9Ex ou réservés) |

### Caps3 (+16) : 0xFFFFFFFF → **0x00000320**

| bit | valeur | annoncé | état, preuve |
|---|---|---|---|
| ALPHA_FULLSCREEN_FLIP_OR_DISCARD | 0x20 | gardé | le mélange alpha s'applique dans la cible ; la présentation Vulkan recopie le back buffer |
| COPY_TO_VIDMEM | 0x100 | gardé | impl. : `v_UpdateSurface` (l. 732), `v_UpdateTexture` (l. 758) |
| COPY_TO_SYSTEMMEM | 0x200 | gardé | impl. : `v_GetRenderTargetData` relit la cible (l. 842) |
| LINEAR_TO_SRGB_PRESENTATION | 0x80 | **retiré** | non impl. : aucun format `_SRGB` dans sc2_d3d9v.c |
| DXVAHD, DXVAHD_LIMITED | 0x400, 0x800 | retirés | absents |
| RESERVED | 0x8000001F | **retirés** | doivent valoir 0 |

### CursorCaps (+24) : 0xFFFFFFFF → **0x00000000**

COLOR 0x1 et LOWRES 0x2 **retirés** : le curseur matériel n'existe pas (`SetCursorProperties` = `m_ok`, `ShowCursor`
= `m_zero`, sc2_d3d9.c:1558).

### DevCaps (+28) : 0xFFFFFFFF → **0x001BBFF0**

| bit | valeur | annoncé | état, preuve |
|---|---|---|---|
| EXECUTESYSTEMMEMORY, EXECUTEVIDEOMEMORY, TLVERTEXSYSTEMMEMORY, TLVERTEXVIDEOMEMORY | 0x10, 0x20, 0x40, 0x80 | gardés | historiques ; tout tampon est servi, quelle que soit sa réserve |
| TEXTURESYSTEMMEMORY | 0x100 | gardé | impl. de fait : `dessin_reel` ne teste pas `O_POOL`, une texture SYSTEMMEM liée est téléversée et échantillonnée (l. 1280-1290) |
| TEXTUREVIDEOMEMORY, DRAWPRIMTLVERTEX, CANRENDERAFTERFLIP, TEXTURENONLOCALVIDMEM, DRAWPRIMITIVES2, DRAWPRIMITIVES2EX | 0x200 … 0x8000 | gardés | sans effet observable |
| SEPARATETEXTUREMEMORIES | 0x4000 | **retiré** | c'est une restriction (« texturage depuis des réserves séparées ») |
| HWTRANSFORMANDLIGHT, HWRASTERIZATION, CANBLTSYSTONONLOCAL | 0x10000, 0x80000, 0x20000 | gardés | le jeu crée le device en HARDWARE_VERTEXPROCESSING (0x50, 009FFEC7) ; le pipeline fixe n'est pas traduit, mais le jeu ne s'en sert pas (vs_3_0) |
| PUREDEVICE | 0x100000 | gardé | impl. de fait : le jeu crée en PURE \| HWVP sans lire DevCaps, et n'appelle aucun Get*State |
| QUINTICRTPATCHES, RTPATCHES, RTPATCHHANDLEZERO, NPATCHES | 0x200000 … 0x1000000 | **retirés** | absents : DrawRectPatch, DrawTriPatch = `m_ok` (l. 1612), SetNPatchMode = `m_ok` (l. 1593) |

### PrimitiveMiscCaps (+32, **lu**) : 0xFFFFFFFF → **0x000648F2**

| bit | valeur | annoncé | état, preuve |
|---|---|---|---|
| MASKZ | 0x2 | gardé | impl. : `depthWriteEnable = df && zt && zw` (sc2_d3d9v.c:2742) |
| CULLNONE, CULLCW, CULLCCW | 0x10, 0x20, 0x40 | gardés | impl. : sc2_d3d9v.c:2725-2731 |
| COLORWRITEENABLE | 0x80 | gardé | impl. pour RT0 : `colorWriteMask = masque & 0xF` (sc2_d3d9v.c:2772), rs[168] (sc2_d3d9.c:1310) |
| BLENDOP | 0x800 | gardé | impl. : `operation()` (sc2_d3d9v.c:2681) |
| **INDEPENDENTWRITEMASKS** | 0x4000 | **gardé (lu)** | non impl. : COLORWRITEENABLE1-3 (190-192) jamais lus ; lu en 009FFD39 → bit 2 de [+0x2BC8]. **V2.1 doit les traduire** (INDEPENDENTBLEND n'existe pas en D3D9) |
| SEPARATEALPHABLEND | 0x20000 | gardé | impl. : rs[206..209] (sc2_d3d9.c:1308), facteurs alpha séparés (sc2_d3d9v.c:2771) |
| **MRTINDEPENDENTBITDEPTHS** | 0x40000 | **gardé (lu)** | non impl. (pas de MRT) ; lu en 009FFD26 → bit 1 de [+0x2BC8]. V2.1 (rendu dynamique : un format par attachement) |
| MRTPOSTPIXELSHADERBLENDING | 0x80000 | **retiré** | non impl. : RT1-3 ne sont jamais attachées (`colorAttachmentCount = 1`, sc2_d3d9v.c:1321, 2780) ; V2.1 le remet |
| NULLREFERENCE | 0x1000 | **retiré** | faux : signifie « device de référence qui ne rend rien » |
| LINEPATTERNREP, CLIPPLANESCALEDPOINTS, CLIPTLVERTS | 0x4, 0x100, 0x200 | retirés | sans objet ou absents |
| TSSARGTEMP, PERSTAGECONSTANT | 0x400, 0x8000 | retirés | pipeline fixe non traduit (D.tss jamais lu) |
| FOGANDSPECULARALPHA, FOGVERTEXCLAMPED | 0x10000, 0x100000 | retirés | brouillard fixe non traduit (rs 28-38 jamais lus) |
| POSTBLENDSRGBCONVERT | 0x200000 | **retiré** | non impl. : SRGBWRITEENABLE (194) jamais lu ; à remettre avec le sRGB si V1.3 le justifie |

### RasterCaps (+36) : 0xFFFFFFFF → **0x06422010**

| bit | valeur | annoncé | état, preuve |
|---|---|---|---|
| ZTEST | 0x10 | gardé | impl. (sc2_d3d9v.c:2742-2743) |
| MIPMAPLODBIAS | 0x2000 | gardé | impl., borné à `maxSamplerLodBias` du GPU (sc2_d3d9v.c:1129-1143 ; nul sous MoltenVK) |
| ANISOTROPY | 0x20000 | gardé | impl. si le GPU a `samplerAnisotropy` (sc2_d3d9v.c:1127), sinon linéaire |
| COLORPERSPECTIVE | 0x400000 | gardé | interpolation perspective de Vulkan |
| SLOPESCALEDEPTHBIAS, DEPTHBIAS | 0x2000000, 0x4000000 | gardés | partiel : traduits avec les ombres actives (défaut) et une profondeur attachée (sc2_d3d9v.c:2733-2739, rs[175]/rs[195] en sc2_d3d9.c:1312) ; DEPTHBIAS ×2^24 est une approximation |
| **SCISSORTEST** | 0x1000000 | **retiré** | non impl. : le rectangle Vulkan est toujours la cible entière (sc2_d3d9v.c:3013) ; SetScissorRect : 0 appel en 39 runs. V2.C le remet |
| DITHER | 0x1 | retiré | rs[26] jamais lu |
| FOGVERTEX, FOGTABLE, FOGRANGE, WFOG, ZFOG | 0x80, 0x100, 0x10000, 0x100000, 0x200000 | retirés | brouillard fixe non traduit |
| ZBUFFERLESSHSR | 0x8000 | **retiré** | faux (le device a un tampon de profondeur) |
| WBUFFER | 0x40000 | **retiré** | non impl. : ZENABLE = 2 est traité comme Z |
| MULTISAMPLE_TOGGLE | 0x8000000 | retiré | pas de MSAA (CheckDeviceMultiSampleType refuse au-delà de 1×) |
| autres | — | retirés | indéfinis |

### ZCmpCaps (+40) et AlphaCmpCaps (+52) : 0xFFFFFFFF → **0x000000FF**

Les 8 fonctions NEVER … ALWAYS (0x1 … 0x80). Z : `depthCompareOp = zf - 1` (sc2_d3d9v.c:2743). Alpha : test dans le
PS, les 8 fonctions (`scripts/d3d9-vers-spirv.py:416-418`, `pc.at`). Bits au-delà de 0x80 indéfinis : retirés.

### SrcBlendCaps (+44) et DestBlendCaps (+48) : 0xFFFFFFFF → **0x000007FF**

Table `facteur()` (sc2_d3d9v.c:2672).

| bit | D3DBLEND | annoncé | état |
|---|---|---|---|
| ZERO … INVDESTCOLOR, SRCALPHASAT | 0x1 … 0x400 (1-11) | gardés | impl. (Vulkan accepte SRC_ALPHA_SATURATE en destination) |
| BOTHSRCALPHA, BOTHINVSRCALPHA | 0x800, 0x1000 (12, 13) | **retirés** | partiel : seul le facteur source est posé ; D3D9 impose aussi la destination |
| **BLENDFACTOR** | 0x2000 (14, 15) | **retiré** | faux : CONSTANT_COLOR, mais `blendConstants` n'est jamais posé (vaut 0, pas d'état dynamique), et rs[193] n'est pas lu. V2.8, si V1.3 compte des dessins |
| SRCCOLOR2, INVSRCCOLOR2 | 0x4000, 0x8000 (16, 17) | retirés | D3D9Ex ; `b < 16` sinon ONE |

### ShadeCaps (+56) : 0xFFFFFFFF → **0x00004208**

COLORGOURAUDRGB 0x8, SPECULARGOURAUDRGB 0x200 et ALPHAGOURAUDBLEND 0x4000 gardés (les sorties du VS sont interpolées).
FOGGOURAUD 0x80000 **retiré** (brouillard fixe non traduit). SHADEMODE (9) n'est jamais lu : pas d'ombrage plat.

### TextureCaps (+60) : 0xFFFFFFFF → **0x0001EC45**

| bit | valeur | annoncé | état, preuve |
|---|---|---|---|
| PERSPECTIVE, ALPHA | 0x1, 0x4 | gardés | impl. |
| TEXREPEATNOTSCALEDBYSIZE | 0x40 | gardé | coordonnées normalisées (HYPOTHÈSE : valeur des pilotes NVIDIA) |
| PROJECTED | 0x400 | gardé | impl. : texldp → textureProj (d3d9-vers-spirv.py) |
| CUBEMAP, VOLUMEMAP | 0x800, 0x2000 | gardés | impl. (genres 5 et 4 dans `dessin_reel`) |
| MIPMAP, MIPVOLUMEMAP, MIPCUBEMAP | 0x4000, 0x8000, 0x10000 | gardés | impl. : tous les niveaux sont téléversés |
| **POW2, SQUAREONLY, NONPOW2CONDITIONAL** | 0x2, 0x20, 0x100 | **retirés** | ce sont des **restrictions** ; les cibles NPOT (1280×720) marchent |
| **CUBEMAP_POW2, VOLUMEMAP_POW2, NOPROJECTEDBUMPENV** | 0x20000, 0x40000, 0x200000 | **retirés** | restrictions |
| ALPHAPALETTE | 0x80 | retiré | SetPaletteEntries = `m_ok` (sc2_d3d9.c:1589) |
| autres | — | retirés | indéfinis |

### TextureFilterCaps (+64), CubeTextureFilterCaps (+68) : → **0x07030700** ; VolumeTextureFilterCaps (+72) : → **0x03030300**

Échantillonneur : `sampler_cmp` (sc2_d3d9v.c:2634-2668).

| bit | valeur | annoncé | état |
|---|---|---|---|
| MINFPOINT, MINFLINEAR, MAGFPOINT, MAGFLINEAR, MIPFPOINT, MIPFLINEAR | 0x100, 0x200, 0x1000000, 0x2000000, 0x10000, 0x20000 | gardés | impl. |
| MINFANISOTROPIC, MAGFANISOTROPIC | 0x400, 0x4000000 | gardés (sauf volume) | impl. si le GPU a l'anisotropie, sinon linéaire |
| MINFPYRAMIDALQUAD, MINFGAUSSIANQUAD, MAGFPYRAMIDALQUAD, MAGFGAUSSIANQUAD | 0x800, 0x1000, 0x8000000, 0x10000000 | **retirés** | partiel : filtres 6 et 7 → linéaire |
| CONVOLUTIONMONO | 0x40000 | retiré | absent |

### TextureAddressCaps (+76, **lu**) et VolumeTextureAddressCaps (+80) : → **0x0000001F**

| bit | valeur | annoncé | état, preuve |
|---|---|---|---|
| WRAP, MIRROR, CLAMP, INDEPENDENTUV | 0x1, 0x2, 0x4, 0x10 | gardés | impl. : table `m[8]` (sc2_d3d9v.c:2652), U, V, W séparés |
| **BORDER** | 0x8 | **gardé (lu)** | partiel : couleur arrondie à l'une des 3 couleurs Vulkan de base (`bordure_vk`, sc2_d3d9v.c:2622). Lu en 00A011E9 → [+0x2960] ; le jeu choisit alors BORDER plutôt que CLAMP. Complet avec `VK_EXT_custom_border_color` |
| **MIRRORONCE** | 0x20 | **retiré** | faux : 5 → CLAMP_TO_EDGE (sc2_d3d9v.c:2654) au lieu de MIRROR_CLAMP_TO_EDGE |

### LineCaps (+84) : 0xFFFFFFFF → **0x0000000F**

TEXTURE 0x1, ZTEST 0x2, BLEND 0x4, ALPHACMP 0x8 gardés (LINE_LIST et LINE_STRIP, sc2_d3d9v.c:2717). FOG 0x10 et
ANTIALIAS 0x20 **retirés** (brouillard fixe et ANTIALIASEDLINEENABLE, rs[176], jamais lus).

## Champs déjà explicites, corrigés au passage

| champ | avant | après | raison |
|---|---|---|---|
| DevCaps2 (+212) | 0x7F | **0x51** | STREAMOFFSET 0x1 (sc2_d3d9.c, `v_SetStreamSource` l. 1521), CAN_STRETCHRECT_FROM_TEXTURES 0x10 (`v_StretchRect` l. 807), VERTEXELEMENTSCANSHARESTREAMOFFSET 0x40 gardés ; DMAPNPATCH 0x2, ADAPTIVETESSRTPATCH 0x4, ADAPTIVETESSNPATCH 0x8, PRESAMPLEDDMAPNPATCH 0x20 **retirés** (pas de tessellation) |
| DeclTypes (+236) | 0x3FF | **0x33F** | UDEC3 0x40 et DEC3N 0x80 **retirés** : `vk_decl` rend UNDEFINED pour 13 et 14 (sc2_d3d9.c:1084), et l'élément devient l'attribut constant (0,0,0,1) |
| StretchRectFilterCaps (+244) | 0x03030303 | **0x03000300** | MIN/MAG POINT et LINEAR (`d3dv_copier(…, filtre == 2)`) ; 0x1, 0x2 (indéfinis) et MIPF* (sans objet pour StretchRect) retirés |
| VertexTextureFilterCaps (+284) | 0x03030300 | **0** | lecture de textures dans le VS non impl. : le traducteur ne déclare de samplers que dans les PS ; aucun VS du cache n'échantillonne (« 0 bytecodes non traduits » sur 75 797) |

## Champs numériques (inchangés)

Lus par le jeu, gardés tels quels : `MaxTextureWidth/Height` 8192, `StencilCaps` 0x1FF (8 opérations et deux faces
impl.), `MaxSimultaneousTextures` 8, `MaxUserClipPlanes` **6** (non impl. : `D.clip` et rs[152] jamais lus — V2.3 décide
d'après V1.3), `VertexShaderVersion` vs_3_0, `MaxVertexShaderConst` 256, `PixelShaderVersion` ps_3_0,
`NumSimultaneousRTs` **4** (non impl. : RT1-3 jamais transmises — V2.1). Baisser `MaxUserClipPlanes` ou
`NumSimultaneousRTs` changerait le chemin du jeu : jamais dans V1.0, seulement en A/B chiffré.

Non lus : `PresentationIntervals` 0x8000000F, `FVFCaps`, `TextureOpCaps`, `MaxTextureBlendStages`,
`VertexProcessingCaps`, `MaxActiveLights` (pipeline fixe), `MaxPointSize` (PSIZE non écrit par le traducteur),
`VS20Caps`/`PS20Caps`, nombres d'instructions.

**Instanciation de flux** (impliquée par vs_3_0, aucun bit de caps : `SetStreamSourceFreq`) : **impl. V2.2** (22/10).
INDEXEDDATA | N → `instanceCount = N`, INSTANCEDATA | 1 → `VK_VERTEX_INPUT_RATE_INSTANCE` (cœur de Vulkan 1.0, aucune
extension), éléments `[0, N)` sans BaseVertexIndex ; diviseur > 1 → repli journalisé, dessin d'avant (0 cas en V1.3) ;
`SC2_INSTANCES=0` rend l'ancien dessin (une instance, tous les flux par sommet). Plan pur : `runtime/sc2_d3d9_inst.h`.

## Pièges

- **Défauts D3D9 absents de CreateDevice** : rs[190..192] valent 0 au lieu de 0xF, rs[193] 0 au lieu de 0xFFFFFFFF.
  INDEPENDENTWRITEMASKS étant annoncé, V2.1 doit poser ces défauts (CreateDevice **et** Reset) avant de lire les
  masques par cible, sinon RT1-3 ne recevraient rien.
- `stage()` teste 0x100000 (D3DUSAGE_QUERY_VERTEXTEXTURE) au lieu de 257..260 (D3DVERTEXTEXTURESAMPLERn) : inerte,
  puisque VertexTextureFilterCaps vaut 0 et qu'aucun VS n'échantillonne.
- `v_GetCreationParameters` rend BehaviorFlags 0x40 alors que le jeu a créé le device en 0x50 ; jamais appelé.

## Acceptation (banc Mac, 22/10)

Binaire V1.0 contre le binaire de `main` (08853c3, construit à 11 h 58) ; sorties hors git dans
`~/dev/sc2-bancs/20261022/caps-etats/` (`v10/`, `main/`) et `…/integ-v0/` pour le canon et le bar de `main`.

| critère | V1.0 | main |
|---|---|---|
| canon `banc-mac.sh defaut` | rc 0 ; image 1 `be01318`, image 10 `b5f7db71` (variante tolérée), 100-300 `ae1ff947` | rc 0 ; `be01318`, `470ebae2`, `ae1ff947` |
| `banc-bar.sh` | rc 0 en 170 s ; TStory01 (12 lignes) puis TRaynor02 (27 lignes) ; 0 « lumière » | rc 0 en 170 s ; 12 et 27 lignes ; 0 « lumière » |
| `campagne-banc.sh … TRaynor01` | rc 0, victoire t=67,0 s, 0 MANQUE, 0 « lumière », 0 arrêt | rc 0, victoire t=66,9 s, 0 MANQUE, 0 « lumière », 0 arrêt |

Le canon a été rejoué avec les masques définitifs (DevCaps 0x001BBFF0, TEXTURESYSTEMMEMORY compris) : mêmes empreintes.

Binaire final de la branche (fd4ae18 : V1.0, profil GPU, V1.3 par époques, CAPTURE), rejoué le 22/10 à 19 h, sorties
`…/caps-etats/final/` :

| critère | binaire final |
|---|---|
| canon `banc-mac.sh defaut` | rc 0 en 18 s ; `be01318`, `b5f7db71` (variante tolérée), 100-300 `ae1ff947` |
| `campagne-banc.sh … TRaynor01` | rc 0, victoire t=67,3 s, 0 MANQUE, 0 « lumière », 0 arrêt |
| CAPTURE, T2 (`SC2_FRAMES=0`, `SC2_CIBLES_DUMP=capture`, `SC2_CAPTURE_TEST=100:essai_cap-1.a,150:mauvais/nom,200:.cache`, 300 frames) | rc 0 ; `essai_cap-1.a.ppm` (`ae1ff947`) et 7 `cible_00100_*.ppm` écrits, aucune `frame_*.ppm` ; « mauvais/nom » et « .cache » refusés et journalisés |
| CAPTURE, T3 (`SC2_FRAMES=1`, `SC2_CAPTURE_TEST=100:essai100`) | rc 0 ; `essai100.ppm` identique octet à octet à `frame_00100.ppm` ; frames 1/10/100-300 du canon |

## Adaptateur : mémoire vidéo par DirectDraw (V2.5a, 22/10)

Le jeu n'appelle jamais `GetAvailableTextureMem` : il mesure la mémoire vidéo par `DirectDrawCreateEx` puis
`IDirectDraw7::GetAvailableVidMem` (sonde `L_00A00FC0`, lift 00A01098-00A0110B : DDSCAPS2 0x10004000 → `[+0x2BC0]`,
0x20005000 → `[+0x2BC4]`, puis `Release` ; ni QueryInterface ni AddRef, codes de retour non lus). Le seul lecteur trouvé
de `[+0x2BC0]` (`L_00BFB810`) en tire la **suggestion** de `GraphicsOptionTextureQuality` : 3 au-dessus de 768 Mo, 2
au-dessus de 384, 1 au-dessus de 192, 0 sinon (seuils stricts), borné par le niveau du GPU de GPUQualityLevels.txt.
Elle ne vaut qu'au premier lancement sans Variables.txt (ou « Restore Defaults ») : une valeur écrite n'est pas bornée.

`sc2_iat_p4.c` (`p_DirectDrawCreateEx`), décision en logique pure dans `sc2_ddraw.h` (test hôte `test_ddraw`), d'après
la carte que `sc2_d3d9.c` annonce (`sc2_gpu_device_id`) :
- profil historique (8600 GT, 0x0402 ; défaut, canon) : refus d'avant à l'octet (DDERR_NODIRECTDRAWHW,
  `*lplpDD = 0`), aucune allocation. Seul écart de journal : la ligne `[GPU] profil …` précède la ligne `[DDRAW]` ;
- profil moderne (`dx10`) : IDirectDraw7 factice, `GetAvailableVidMem` = `SC2_VRAM_MO` (défaut 1024) en mémoire
  locale, 0 en non locale ;
- **Android : refus par défaut, même en `dx10`** (garde-fou du 23/10) : le lanceur pose `dx10` par défaut, et toute
  suggestion du jeu (« Restore Defaults », Variables.txt absent ou d'un autre `lastDeviceId`) donnerait des textures
  Ultra au lieu de Low, alors que le PSS et le LMK du Fold n'ont pas été mesurés en High/Ultra. `SC2_DDRAW=1` le sert
  (mesure du Fold) ; à retirer, ou à remplacer par un `SC2_VRAM_MO` par appareil, après cette mesure ;
- `SC2_DDRAW=0|1` (diagnostic) : refus ou service forcés, tous profils.

Garde du canon : `scripts/banc-mac.sh defaut` appelle `scripts/garde-adaptateur.sh` (historique sans `SC2_DDRAW`) et
échoue (code 5) sans le refus `[DDRAW] … depuis 0x00A010AA → DDERR_NODIRECTDRAWHW`, avec une ligne `IDirectDraw7`, ou
si le Variables.txt écrit par le jeu n'a pas `GraphicsOptionTextureQuality[2]=0` et `lastDeviceId=1026`. Les empreintes
seules ne voient pas un DirectDraw servi en historique (mutant de la revue : mêmes images 1, 10 et 100-400).

Les deux méthodes appelées réutilisent les stubs stdcall de même arité du fichier (n° 23 GetAvailableVidMem → stub de
DirectDrawCreateEx, 4 mots ; n° 2 Release → stub de Direct3DCreate9, 1 mot), qui reconnaissent l'objet en premier
argument ; les 28 autres pointent vers 0xF2DD0000 + 16 × n° (arrêt « MANQUE : saut hors image »). En dx10, le
`[BILAN]` des imports compte donc DirectDrawCreateEx×3 et Direct3DCreate9×2.

Banc Mac, 600 frames sans Variables.txt (`Variables.txt` écrit par le jeu, sorties
`~/dev/sc2-bancs/20261022/vram-ddraw/vram/`) :

| clé | historique | dx10 | dx10, `SC2_VRAM_MO=768` | dx10, `SC2_VRAM_MO=384` | dx10, `SC2_DDRAW=0` |
|---|---:|---:|---:|---:|---:|
| GraphicsOptionTextureQuality[2] | 0 | **3** | **2** | **1** | 0 |
| texQualityLevel | — | 3 | 2 | 1 | — |
| TerrainTextureSize | 480 | 1024 | 768 | 768 | 480 |
| targettexeldensity | 0,15 | 10,6 | 1,4 | 0,56 | 0,15 |
| GraphicsOptionOverallQualityVer7[5] | 1 | 3 | 3 | 3 | 3 |
| creep, Effects, Lighting, Movies, Physics, PostProcessing, Shader, Shadow, Terrain | 1 (Lighting, Movies 0) | 3 | 3 | 3 | 3 |
| Reflections | 0 | 2 | 2 | 2 | 2 |
| ModelQuality, Portraits | 2 | 2 | 2 | 2 | 2 |
| lastDeviceId | 1026 | 1507 | 1507 | 1507 | 1507 |

Les quatre paliers prédits par le modèle de `L_00BFB810` sont observés. `historique` écrit le même Variables.txt qu'avant
(diff vide contre `caps-etats/gpu/historique`), `dx10` + `SC2_DDRAW=0` le même que le dx10 d'avant (diff vide contre
`caps-etats/gpu/dx10`) ; entre dx10 et dx10 + `SC2_DDRAW=0`, seules les clés de textures diffèrent
(`GraphicsOptionTextureQuality`, `texQualityLevel`, `TerrainTextureSize`, `TerrainTexture*CacheSize`,
`targettexeldensity`).

TRaynor01 (`campagne-banc.sh`, scénario de victoire, `SC2_ARENE_JOURNAL=5`, sans Variables.txt, cache de pipelines froid,
un run par cas ; banc Mac M4 ; sorties `…/vram-ddraw/mission/`). E = arène principale (768 Mo) + débordement ;
C = 768 + 1 792 = 2 560 Mo ; marge = 1 − Ê/C. Le sommet tombe en fin de run (t ≈ 187 s, écrans d'après la victoire, la
mémoire croît encore) ; « t = 65 s » = fin de la partie jouée. Images/s de plateau : tours de pompe entre les événements
du scénario à t = 40 s et t = 65,4 s ; globales : frames / durée du banc.

| cas | textures suggérées | principale (pic) | débordement (pic) | Ê | marge | E à t = 65 s | img/s plateau | img/s globales | rc, MANQUE, « lumière » |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---|
| historique | 0 | 768 | 279 | 1 047 | 59,1 % | 923 | 94,3 | 329,4 | 0, 0, 0 |
| dx10 (1 024 Mo) | 3 | 767 | 1 322 | **2 089** | **18,4 %** | 1 536 | 68,4 | 177,6 | 0, 0, 0 |
| dx10, `SC2_VRAM_MO=768` | 2 | 768 | 893 | 1 661 | 35,1 % | 1 226 | 66,2 | 193,2 | 0, 0, 0 |
| dx10, `SC2_DDRAW=0` | 0 | 768 | 275 | 1 043 | 59,3 % | 928 | 69,4 | 197,8 | 0, 0, 0 |

Lecture (corrigée après revue, 23/10) : **Ê n'est pas un sommet**, c'est la valeur au QUIT du scénario (t = 187 s) ;
après la victoire, E croît encore de ≈ +3,9 Mo/s en 1 024 (1 697 Mo à t = 87 s → 2 089 Mo) et de ≈ +3,1 Mo/s en 768
(1 353 → 1 661), contre ≈ +0,6 Mo/s en textures Low. **1 024 Mo dépasse déjà le seuil du §2.8 (Ê ≤ 1 920 Mo, 25 % de
marge) dans ces chiffres mêmes, à t = 153 s** ; « 768 passe » ne tient qu'à l'arrêt du run. Les images/s de ce tableau
sont faussées par le cache de pipelines froid (18,6 s de `vkCreateGraphicsPipelines` en dx10 1 024, 18,2 s en
historique, < 1 s pour les runs suivants) et un run par cas. **Paliers 1 024 et 768 : non décidés** tant que la mesure
prolongée (après-victoire jusqu'à ≈ 470 s), THanson03B en préréglages high/ultra (§4.6) et les images/s à cache chaud
par profil (A/B/A) ne sont pas faites.

Historique à l'octet (T-H) : le même run de 600 frames avec un binaire témoin de `main` (0b0a075, seul
`sc2_iat_p4.c` reconstruit) donne un Variables.txt identique, les mêmes lignes `[DDRAW]`, `[USER32]` et
`[D3D9] CreateDevice`, les mêmes adresses invitées `RELOGÉE` (0x08E8E130, 0x09E024E0) et les mêmes empreintes des
frames 1 à 400 ; le canon `banc-mac.sh defaut` reste `be01318` / `b5f7db71` (variante tolérée) / `ae1ff947`.
