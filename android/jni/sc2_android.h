/* sc2_android.h — interface entre les accroches (sc2_android_hooks.c, communes) et la
 * plate-forme qui les héberge : l'activité Android (sc2_android_main.c) ou le banc Linux/qemu
 * (android/sim/sc2_sim_main.c). Passe 7a. */
#ifndef SC2_ANDROID_H
#define SC2_ANDROID_H
#include <stdint.h>

/* main() de recomp-sc2/src/main.c, renommé à la compilation (-Dmain=sc2_poc_main). */
int sc2_poc_main(int argc, char **argv);

/* Zone invitée décalée de 4 Go (g_hote_delta) : à appeler AVANT sc2_poc_main. 0 = OK. */
int       sc2a_zone_init(void);
uintptr_t sc2a_zone(void);

/* Taille du back buffer (celle passée au runtime par SC2_ECRAN) : active la relecture. */
void sc2a_taille_ecran(uint32_t w, uint32_t h);

/* Fournis par la plate-forme, appelés sur le fil du jeu après le Present n°n. rgba == NULL : la
 * frame a déjà été présentée par le GPU (swapchain, passe 7b) — à compter seulement. */
int  sc2a_frame_voulue(unsigned n);                                  /* relire cette frame ? */
void sc2a_frame(unsigned n, const uint8_t *rgba, uint32_t w, uint32_t h);
void sc2a_avant_sortie(void);                                        /* juste avant le bilan */

/* Passe 9d — observabilité de l'affichage, fournis par les accroches (sc2_android_hooks.c) :
 * Present du moteur vus, Present sans fenêtre (rien relu ni posté), cibles refusées par la relecture CPU. */
unsigned sc2a_presents(void);
/* Passe 10o : profil de la boucle depuis l'appel précédent (ms/frame jeu / backend / présentation) ; remet à zéro. */
void sc2a_profil_texte(char *o, size_t cap);
void sc2a_bilan_presents(uint64_t *sans_fenetre, uint64_t *cible_refusee);
/* Taille des cibles de rendu (appelé par __wrap_d3dv_cible) : la relecture CPU refuse une cible ≠ g_w×g_h. */
void sc2a_cible_vue(int id, uint32_t w, uint32_t h, int profondeur);
/* Passe 10a — fourni par l'activité (APK) : la géométrie du tampon CPU est à reposer (bascule swapchain → copie CPU).
 * Facultatif : référencé en faible par sc2_vk_presente.c, le banc android/sim ne le fournit pas. */
void sc2a_geo_invalider(void);

/* Sortie propre : demandée par n'importe quel fil, exécutée par le fil du jeu au prochain
 * Present (sc2a_sortir → sc2_stop(0) → [BILAN] → exit(0)). */
void sc2a_demander_sortie(void);
int  sc2a_sortie_demandee(void);
/* 21/10 : 1 = activité en arrière-plan → le fil du jeu s'arrête à son prochain Present (horloge du jeu gelée, son en
 * pause) ; 0 = reprise. */
void sc2a_suspendre(int suspendu);
void sc2a_sortir(const char *ou) __attribute__((noreturn));

/* Entrées (passe 7d, API runtime sc2_input.h) : pointeur unique = souris gauche, coordonnées CLIENT
 * du jeu (back buffer). sc2a_entrees_activer() : AVANT sc2_poc_main (cf. sc2_android_hooks.c). */
enum { SC2A_APPUI, SC2A_BOUGE, SC2A_LEVE };
void sc2a_entrees_activer(void);
void sc2a_tactile(int action, int32_t x, int32_t y);
void sc2a_touche(uint32_t vk);                       /* enfoncée puis relâchée (VK Win32) */
void sc2a_bilan_entrees(uint64_t *injectes, uint64_t *moves_fusionnes);

#endif
