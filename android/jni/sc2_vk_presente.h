/* sc2_vk_presente.h — présentation GPU par swapchain Vulkan (passe 7b), cf. sc2_vk_presente.c.
 * Compilé seulement avec l'option CMake SC2_SWAPCHAIN=ON (macro SC2_SWAPCHAIN) ; actif à
 * l'exécution avec SC2_SWAPCHAIN=1. Sinon : copie CPU de la passe 7a (chemin par défaut). */
#ifndef SC2_VK_PRESENTE_H
#define SC2_VK_PRESENTE_H
#include <stddef.h>
#include <stdint.h>
#include "sc2_barre.h"

int  sc2v_actif(void);                                   /* interposition réussie (instance + device) */
/* Fenêtre courante (ANativeWindow*, ou NULL + taille pour la surface sans écran du banc) ;
 * (NULL, 0, 0) = fenêtre perdue : surface et swapchain détruites AVANT le retour. */
void sc2v_fenetre(void *fenetre, uint32_t w, uint32_t h);
/* Après le Present n°n du backend : blit de la cible id vers la swapchain. 0 = présentée ;
 * -1 = rien fait (pas de fenêtre, swapchain à refaire, échec) → l'appelant peut copier par CPU. */
int  sc2v_presenter(int id, unsigned n);
/* Passe 10r (v13) : sémaphore « rendu » du backend non attendu par un blit (Present en échec, présentation non appelée) :
 * consommé par une soumission vide. Sans effet s'il a déjà été pris par sc2v_presenter. */
void sc2v_consommer_rendu(void);
/* Changement de taille possible (APP_CMD_WINDOW_RESIZED / CONFIG_CHANGED) : vérifié au prochain Present. */
void sc2v_taille_changee(void);
void sc2v_bilan(char *o, size_t cap);
/* Passe 10o : voie de présentation courante en quelques mots (« swapchain 2176x1812 », « swapchain basculée : … »). */
void sc2v_etat_court(char *o, size_t cap);
/* Passe 7o : back buffer sw×sh blitté dans la swapchain dw×dh au dernier Present (rectangle :
 * sc2_geo_ajuster). 0 = connue ; -1 = rien présenté encore. Lisible depuis n'importe quel fil. */
int  sc2v_geometrie(uint32_t *sw, uint32_t *sh, uint32_t *dw, uint32_t *dh);

/* 21/10 : barre de raccourcis dans la bande libre (sc2_barre.h). voulue : 1 au toucher, 0 dès qu'une souris ou un
 * clavier physique sert (SC2_BARRE=0/1 l'impose). barre_appuyer : point (sx, sy) en pixels de SWAPCHAIN ; rend -1 hors
 * de la barre, sinon le nombre de VK écrits (0 : modificateur, page, ou entre deux touches). barre_geo : disposition
 * du dernier Present (rectangle de l'image du jeu compris) ; -1 si rien présenté. */
void sc2v_barre_voulue(int voulue);
int  sc2v_barre_appuyer(int32_t sx, int32_t sy, uint16_t *vk, uint8_t *bas, int max);
int  sc2v_barre_geo(barre_geo *g);

#endif
