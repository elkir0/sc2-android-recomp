/* mac/sc2_mac_fenetre.h — interface PRIVÉE du lot M7 (V3, 24/10) entre mac/sc2_mac_fenetre.m (fenêtre, cycle de vie,
 * sortie) et mac/sc2_mac_evenements_ns.m (rejeu SC2_MAC_EVENEMENTS par NSEvent et vrais appels AppKit). Les interfaces
 * entre lots restent dans mac/sc2_mac.h ; ici, seulement ce que les deux fichiers de M7 se doivent.
 * Fil principal (P) partout, sauf mention. En-tête C : les objets AppKit passent en void *. */
#ifndef SC2_MAC_FENETRE_H
#define SC2_MAC_FENETRE_H
#include <stdint.h>

/* ---- rejeu (sc2_mac_evenements_ns.m) ------------------------------------------------------------------------------ */
/* P, AVANT NSApplication (l'isolement décide de la fenêtre et de la politique d'activation) : analyse le scénario de
 * spec (« @fichier » ou texte ; NULL ou vide : aucun). Rend 0 sans scénario, 1 scénario prêt, -1 erreurs (chaque ligne
 * « [MACEV] erreur … » journalisée : l'hôte sort, code 5). *isole : sc2en_isole (SC2_MAC_ISOLE l'emporte, sinon ISOLE du
 * scénario, sinon 1) ; sans scénario : SC2_MAC_ISOLE=1, sinon 0. *pourquoi : « variable », « scénario » ou « défaut ». */
int  sc2mr_preparer(const char *spec, int *isole, const char **pourquoi);
int  sc2mr_actif(void);                         /* T ; 1 : un scénario est armé et pas fini */
void sc2mr_arreter(const char *pourquoi);       /* P ; plus rien n'est rejoué (sortie demandée) ; bilan une fois */

/* ---- fenêtre (sc2_mac_fenetre.m), pour le rejeu ------------------------------------------------------------------- */
int  sc2mf_isole(void);                         /* T ; rejeu isolé : notifications réelles et NSEvent matériels ignorés */
void sc2mf_focus(int actif, int simule);        /* chemin des notifications de focus ; simule = 1 : commande FOCUS du
                                                 * rejeu (passe même en mode isolé) */
void sc2mf_occultation(int occultee, int simule);   /* chemin de NSWindowDidChangeOcclusionState (sc2v_visible) */
void sc2mf_sortie_code(int code, const char *pourquoi);  /* code noté (sc2m_sortie_noter), puis sc2m_quitter : le
                                                 * dernier gestionnaire atexit sort avec CE code (délai du rejeu : 5) */
void sc2mf_ignores(uint64_t *materiels, uint64_t *notifications);   /* T ; comptes du mode isolé */
double sc2mf_ms(void);                          /* T ; horloge monotone de l'hôte, ms */
#endif
