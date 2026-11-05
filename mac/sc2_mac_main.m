/* mac/sc2_mac_main.m — main() de l'exécutable recomp-sc2 sur macOS (V3, lot M0, 22/10 ; v3-critique.md §3.4).
 *
 * Un binaire, deux modes, décidés ici une fois pour toutes (sc2m_mode_fenetre) :
 *   - SC2_FENETRE absente ou ≠ 1 : BANC historique. sc2m_banc → sc2_poc_main (recomp-sc2/src/main.c, main renommé par
 *     mac/mac.cmake) sur le fil principal ; l'exit(code) du runtime est conservé ; rien d'AppKit n'est appelé.
 *   - SC2_FENETRE=1 (posée par LSEnvironment de l'Info.plist pour tout lancement par le Finder ou par open) : APP.
 *     Copie de l'environnement (relance, M9), gestionnaire de sortie sans finaliseurs (1er enregistré, donc exécuté en
 *     dernier), puis l'hôte Cocoa (sc2m_fenetre_lancer, lot M7 ; talon de M0 : « mode fenêtre absent », code 3).
 */
#include <stdlib.h>

#include "sc2_mac.h"

int main(int argc, char **argv)
{
    if (!sc2m_mode_fenetre()) return sc2m_banc(argc, argv);
    sc2m_env_capturer();
    atexit(sc2m_sortie_sans_finaliseurs);
    @autoreleasepool {
        return sc2m_fenetre_lancer(argc, argv);
    }
}
