# Scénarios de diagnostic (banc Mac, 21/10/2026)

Scénarios `SC2_BANC_ENTREES` des diagnostics du 21/10 (menu Options en pleine partie, réglage Ultra, survol au bar),
renommés le 22/10 (V0.4). Coordonnées du client 1280×720 (à lancer sans `SC2_ECRAN=auto`). Les preuves (journaux .gz,
Variables.txt, StoryModeDebug, captures) sont archivées hors git dans `~/code/sc2-apk/mesures/20261021/banc/diag/`.

| scénario | ancien nom | preuves | ce qu'il fait |
|---|---|---|---|
| `f10-ouvrir.txt` | `menu.txt` | `diag/menu` | TRaynor01 : aide fermée (« Close » 953,588 ×3), F10 (VK 0x79), QUIT à 45 s |
| `f10-options.txt` | `menu2.txt` | `diag/menu2` | … puis « Options » (640,131) |
| `f10-options-graphismes.txt` | `menu3.txt` | `diag/menu3` | … puis l'onglet des graphismes (632,353) |
| `f10-options-ultra-accept.txt` | `menu4.txt` | `diag/menu4` | … puis Ultra (580,435), « Accept » (1002,644), un clic au centre (640,376) |
| `f10-options-ultra-accept-retour.txt` | `menu5.txt` | `diag/menu5` | même chemin, deux clics au centre après « Accept » (essai de retour au jeu : le menu ne se referme pas) |
| `mission-aide-fermee.txt` | `ultra.txt` | `diag/q-ultra`, `diag/q-medium` | aide fermée puis QUIT à 45 s : sert avec un Variables.txt semé (q-ultra / q-medium) |
| `bar-survol-tychus.txt` | `survol.txt` | `diag/ab-tychus.png` | bar : cinématique passée (Échap ×5), survol de Tychus (560,380) puis ailleurs (900,200) |
| `bar-survol-tychus-lent.txt` | `survol2.txt` | — | même survol, plus tard et à petits pas |
| `bar-survol-hors-tychus.txt` | `survol3.txt` | — | survols hors de Tychus (900,200 et 760,300) : témoin |
| `bar-cinematique-passee.txt` | `vide.txt` | `diag/vide` | bar : Échap ×5, aucune autre entrée, QUIT à 70 s (témoin d'images/s sans survol) |

V1.5 en tire `scripts/entrees/options-ultra.txt` et `options-accept.txt` ; V1.1 les réécrira en déclencheurs (`QUAND:CARTE`, `REF:`).

## L2 (24/10) : le menu F10 « qui ne se refermait pas » après Accept

Ce n'était pas un défaut du runtime. Après « Accept », le jeu affiche une boîte de confirmation du nouveau mode
d'affichage, avec compte à rebours de 15 s avant retour arrière (YES en 513,334 et NO en 767,334, client 1280×720), par-dessus le
menu de jeu. Les clics au centre (640,376) de `f10-options-ultra-accept-retour.txt` tombaient à côté, et le 3e Reset était
le retour automatique au bout du compte à rebours. Avec « YES », puis « Return to Game », le menu se referme et F10 se
rouvre normalement. Le 1024×768 écrit dans Variables.txt n'y est pour rien : avec un bureau annoncé en 1280×720, la boîte
apparaît de la même façon (le premier Accept écrit width/height, absents du préréglage semé). Preuves hors git :
`~/dev/sc2-bancs/20261023/l2-f10/`. Scénario d'acceptation : `scripts/entrees/f10-menu-evt.txt` (Options → Accept → YES →
retour au jeu → Restart → Quit Campaign, qui termine le jeu en mode lanceur : `SC2_MENU_PRINCIPAL`).
Autres repères du menu (1280×720, @ui) : Save 580,173 ; Load 700,173 ; Restart 640,215 puis « Restart » du choix de difficulté
640,250 ; Abort Mission 640,257 (retour direct au bar, sans confirmation) ; Quit Campaign 640,299 ; Return to Game 640,376.
Boîte « Save Game » : bouton Save 1003,614 (nom proposé : celui de la mission).
