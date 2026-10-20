/* image_base.c — définition du delta d'image du socle recomp-sc2.
 *
 * runtime/cpu.h (copié de pcrecomp recomp32_cpu) déclare `g_image_delta`
 * en extern : sans cette définition, l'édition de liens du binaire vide
 * échoue. Il reste à 0 tant que l'image est mappée à sa base préférée
 * (SC2.exe build 15405 : ImageBase 0x00800000, CARTE-INIT en-tête) ;
 * le futur loader PE win32hle l'établira à live_base - 0x00800000 pour
 * la macro GVA() des références absolues relogées.
 */
#include "cpu.h"

uint32_t g_image_delta = 0;
