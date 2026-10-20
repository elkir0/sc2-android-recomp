/* sc2_entrees_bt.h — passe 8h : souris et clavier (Bluetooth ou USB) Android → messages Win32 du jeu.
 *
 * Un clavier et une souris appairés dans les Réglages Android arrivent à la NativeActivity comme des AInputEvent
 * ordinaires : rien à configurer côté jeu. Ce module les traduit, sans dépendre du NDK (constantes Android
 * recopiées : il se teste sur l'hôte, android/outils/test-entrees-8h.c) :
 *   souris  survol (HOVER_MOVE) et glisser → WM_MOUSEMOVE (fusionnés, une fois par frame, comme le tactile) ;
 *           boutons (buttonState : primaire, secondaire, tertiaire) → WM_L/R/MBUTTONDOWN/UP, à chaque changement
 *           d'état quel que soit l'action qui le porte (DOWN/UP, BUTTON_PRESS/RELEASE, MOVE) ;
 *           molette (SCROLL, axes VSCROLL/HSCROLL) → WM_MOUSEWHEEL/WM_MOUSEHWHEEL, ±120 par cran (crans
 *           fractionnaires des molettes fines cumulés) ;
 *   clavier AKEYCODE_* → VK Win32 (table ci-dessous, disposition rendue par Android : une touche « A » d'un
 *           clavier AZERTY arrive en KEYCODE_A → VK_A, comme sous Windows FR) ; répétition = WM_KEYDOWN répété ;
 *           modificateurs gauche/droit → VK_LSHIFT… (le runtime en déduit VK_SHIFT, WM_SYSKEY sous Alt) ;
 *           les caractères (WM_CHAR) sont produits par TranslateMessage du runtime (disposition US).
 * Le retour Android déclenché par un clic DROIT (repli système « bouton secondaire = retour ») est ignoré : la
 * glue le reconnaît à sa source souris (sc2_android_main.c). */
#ifndef SC2_ENTREES_BT_H
#define SC2_ENTREES_BT_H
#include <stddef.h>
#include <stdint.h>

/* Actions AMotionEvent (android/input.h), recopiées pour l'hôte. */
enum { SC2BT_DOWN = 0, SC2BT_UP = 1, SC2BT_MOVE = 2, SC2BT_CANCEL = 3, SC2BT_HOVER_MOVE = 7, SC2BT_SCROLL = 8,
       SC2BT_HOVER_ENTER = 9, SC2BT_HOVER_EXIT = 10, SC2BT_BUTTON_PRESS = 11, SC2BT_BUTTON_RELEASE = 12 };
/* buttonState (AMOTION_EVENT_BUTTON_*) */
enum { SC2BT_B_PRIMAIRE = 1, SC2BT_B_SECONDAIRE = 2, SC2BT_B_TERTIAIRE = 4 };

uint32_t sc2bt_vk(int32_t keycode);                 /* VK Win32, 0 = touche non traduite (laissée au système) */
/* Événement clavier : 1 = consommé (VK connu), 0 = laissé au système (volume, etc.). */
int  sc2bt_cle(int32_t keycode, int bas, int32_t repetition);
/* Événement souris, (x, y) déjà en coordonnées CLIENT du jeu ; vscroll/hscroll : axes de l'action SCROLL. */
void sc2bt_souris(int32_t action, int32_t boutons, int32_t x, int32_t y, float vscroll, float hscroll);
void sc2bt_bilan(char *o, size_t cap);

/* Sorties (sc2_android_hooks.c ; doublées par le test hôte) */
void sc2a_deplacer(int32_t x, int32_t y);           /* fusionné, livré au Present */
void sc2a_bouton(int bouton, int bas, int32_t x, int32_t y);   /* 0 gauche, 1 milieu, 2 droit */
void sc2a_molette(int32_t delta, int horizontale, int32_t x, int32_t y);
void sc2a_cle(uint32_t vk, int bas);

#endif
