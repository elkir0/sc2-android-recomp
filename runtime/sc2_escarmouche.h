/* sc2_escarmouche.h — V4.10 (23/10) : escarmouche contre l'IA, hors ligne (« carte d'escarmouche dérivée »).
 *
 * Les 63 cartes mêlée de WoL sont des entrées du cache Battle.net pré-rempli de l'installation
 * (Mods/Liberty.SC2Mod/Base.SC2Data : BattlenetCache\(N)<Nom>.SC2Map\<sha256>.s2ma). Au démarrage, si
 * SC2_ESCARMOUCHE nomme l'une d'elles, le runtime en fait une COPIE privée dans le C:\ émulé
 * ($SC2_ROOT/Users/sc2/Documents/StarCraft II/Maps/Escarmouche/<Nom>.SC2Map, jamais dans l'installation), y réécrit
 * le MapInfo (joueur 1 humain, joueurs 2… Computer, races et couleurs) et le MapScript.galaxy (PlayerSetDifficulty
 * avant MeleeInitResources()), puis le lanceur simulé (sc2_iat.c, run_lanceur_preparer, mode 2 « tester le
 * document ») la donne au jeu à la place de la carte d'histoire. Le reste (bases, IA, victoire, défaite) est au jeu.
 * Diagnostic et preuves : rapport V4.10 du 22/10 (hors dépôt) — cartes mêlée du cache, MapInfo 0x18, mode 2 du
 * lanceur = « tester le document » (joueur 1 seul : les autres viennent du MapInfo), énumération c_skir*.
 *
 * Interface en types C seulement (StormLib.h apporte ses propres HANDLE/DWORD, cf. sc2_mpq.h).
 */
#ifndef RECOMP_SC2_ESCARMOUCHE_H
#define RECOMP_SC2_ESCARMOUCHE_H

/* Point d'appel (src/main.c, avant tout code du jeu). racine = $SC2_ROOT (« sc2-c » par défaut).
 *   SC2_ESCARMOUCHE absente, vide ou « 0 » : rien (aucune lecture, aucun journal), rend 0.
 *   « liste »    : cartes mêlée du cache journalisées (n°, nom, joueurs), rend 1 et *code_sortie = 0 (arrêt propre) ;
 *   « verifier » : prépare les 63 cartes dans un fichier temporaire et relit chacune (MapInfo, MapScript), rend 1 et
 *                  *code_sortie = 0 si toutes passent, 1 sinon ;
 *   <nom> ou <n°> : prépare la carte ; rend 0 (succès ou échec : un échec est journalisé « [ESCARMOUCHE] ÉCHEC … »
 *                  et le jeu démarre normalement, sans escarmouche). Pose SC2_RUN_SANS_BNET=1 si elle manque.
 * L'appelant s'arrête (return *code_sortie) quand la fonction rend 1. */
int sc2_escarmouche_demarrer(const char *racine, int *code_sortie);

/* Chemin Windows de la carte préparée (« C:\Users\sc2\Documents\StarCraft II\Maps\Escarmouche\<Nom>.SC2Map »), ou
 * NULL (pas d'escarmouche demandée, ou préparation en échec). Lu par le lanceur simulé. */
const char *sc2_escarmouche_carte(void);

#endif
