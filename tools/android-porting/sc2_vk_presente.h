/* sc2_vk_presente.h — présentation GPU par swapchain Vulkan (passe 7b), cf. sc2_vk_presente.c.
 * Compilé seulement avec l'option CMake SC2_SWAPCHAIN=ON (macro SC2_SWAPCHAIN) ; actif à
 * l'exécution avec SC2_SWAPCHAIN=1. Sinon : copie CPU de la passe 7a (chemin par défaut). */
#ifndef SC2_VK_PRESENTE_H
#define SC2_VK_PRESENTE_H
#include <stddef.h>
#include <stdint.h>

int  sc2v_actif(void);                                   /* interposition réussie (instance + device) */
/* Fenêtre courante (ANativeWindow*, ou NULL + taille pour la surface sans écran du banc) ;
 * (NULL, 0, 0) = fenêtre perdue : surface et swapchain détruites AVANT le retour. */
void sc2v_fenetre(void *fenetre, uint32_t w, uint32_t h);
/* Après le Present n°n du backend : blit de la cible id vers la swapchain. 0 = présentée ;
 * -1 = rien fait (pas de fenêtre, swapchain à refaire, échec) → l'appelant peut copier par CPU. */
int  sc2v_presenter(int id, unsigned n);
void sc2v_bilan(char *o, size_t cap);

#endif
