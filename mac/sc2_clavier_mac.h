/* mac/sc2_clavier_mac.h — clavier macOS → touches et caractères Win32, en C pur (V3, lot M2, 22/10).
 *
 * Table et ordre de décision : inventaire v3-entrees §6.3-6.4 ; interface : conception A §2.4 (L6), options de B §2.7.
 * Aucune dépendance à AppKit ni à Carbon à la compilation : testé sur l'hôte (mac/tests/test_clavier_mac.c, à la main
 * par « cc -Wall -Wextra -I mac mac/tests/test_clavier_mac.c mac/sc2_clavier_mac.c »). Utilisé par mac/sc2_entrees_mac.m
 * (M8), sur le fil principal : aucun état global ici, sinon les deux fonctions de HIToolbox (sc2m_iso*).
 *
 * Chaîne d'un keyDown (M8) :
 *   vk = sc2m_vk_de_kvk(keyCode, 1er caractère de charactersByApplyingModifiers:0, sc2m_iso_de_clavier(type de
 *        l'événement)) ;
 *   vk ≠ 0 : vk = sc2m_clavier_appui(&etat, keyCode, vk) (répétition : le VK de l'appui), vk ≠ 0 →
 *   sc2_input_touche_plateforme(vk, 1), puis chaque sortie de
 *   sc2m_caracteres(vk, modifierFlags, &opt, characters, charactersIgnoringModifiers) → sc2_input_caractere(cp) ;
 * keyUp : vk = sc2m_clavier_relache(&etat, keyCode) (le VK MÉMORISÉ à l'appui, jamais recalculé) ; vk ≠ 0 →
 *   sc2_input_touche_plateforme(vk, 0) ;
 * flagsChanged : vk = sc2m_vk_modificateur(keyCode, modifierFlags, &opt, &bas) ; 0x14 (Verr. maj) : bas = état du
 *   verrou après l'événement, l'appelant envoie touche(0x14, 1) puis touche(0x14, 0) (un seul événement par bascule) ;
 *   autre vk ≠ 0 : sc2_input_touche_plateforme(vk, bas) ; 0 : rien (fn, Ctrl physique en mode « clic », Option en
 *   mode « altgr »).
 * Perte du focus : sc2m_clavier_reinitialiser(&etat) (le runtime relâche lui-même par sc2_input_tout_relacher). */
#ifndef SC2_CLAVIER_MAC_H
#define SC2_CLAVIER_MAC_H
#include <stddef.h>
#include <stdint.h>

/* NSEvent.modifierFlags, sans AppKit. Indépendants du côté (NSEvent.h) : */
#define SC2M_DRAP_VERR_MAJ  (1ull << 16)   /* NSEventModifierFlagCapsLock */
#define SC2M_DRAP_MAJ       (1ull << 17)   /* NSEventModifierFlagShift */
#define SC2M_DRAP_CTRL      (1ull << 18)   /* NSEventModifierFlagControl (Ctrl PHYSIQUE) */
#define SC2M_DRAP_OPTION    (1ull << 19)   /* NSEventModifierFlagOption */
#define SC2M_DRAP_CMD       (1ull << 20)   /* NSEventModifierFlagCommand */
#define SC2M_DRAP_PAVE      (1ull << 21)   /* NSEventModifierFlagNumericPad */
#define SC2M_DRAP_FN        (1ull << 23)   /* NSEventModifierFlagFunction */
/* … et par côté (IOLLEvent.h:253-261, NX_DEVICE*KEYMASK), dans les 16 bits bas : */
#define SC2M_DEV_LCTRL      0x0001u
#define SC2M_DEV_LMAJ       0x0002u
#define SC2M_DEV_RMAJ       0x0004u
#define SC2M_DEV_LCMD       0x0008u
#define SC2M_DEV_RCMD       0x0010u
#define SC2M_DEV_LOPTION    0x0020u
#define SC2M_DEV_ROPTION    0x0040u
#define SC2M_DEV_RCTRL      0x2000u

/* Réglages (décisions du mainteneur, §3.6 de v3-critique ; M8 les lit de SC2_MAC_CMD, SC2_MAC_CTRL, SC2_MAC_OPTION). */
typedef struct {
    int cmd_vers_ctrl;   /* 1 (défaut, « ctrl ») : Cmd → VK_LCONTROL/VK_RCONTROL ; 0 (« cmd ») : VK_LWIN/VK_RWIN */
    int ctrl_transmis;   /* 0 (défaut, « clic ») : Ctrl physique réservé au Ctrl+clic ; 1 (« ctrl ») : → VK_L/RCONTROL */
    int option_altgr;    /* 0 (défaut, « alt ») : Option → VK_LMENU/VK_RMENU, caractère sans Option en WM_SYSCHAR ;
                          * 1 (« altgr ») : Option non transmise, caractère composé de `characters` (« { » sur AZERTY) */
} sc2m_clavier_options;
#define SC2M_CLAVIER_OPTIONS_DEFAUT { 1, 0, 0 }

/* Valeurs de SC2_MAC_CMD (ctrl|cmd), SC2_MAC_CTRL (clic|ctrl) et SC2_MAC_OPTION (alt|altgr) ; NULL ou "" = défaut.
 * Rend le nombre de valeurs non reconnues (le défaut est alors gardé pour celle-là, à journaliser par l'appelant). */
int sc2m_clavier_options_lire(sc2m_clavier_options *o, const char *cmd, const char *ctrl, const char *option);

/* keyDown : kVK (Events.h) → VK Win32 ; 0 = non traduite (« [CLAVIER] kVK 0x.. non traduit », journalisé par M8).
 * car_sans_modif : 1er caractère de charactersByApplyingModifiers:0 (à défaut charactersIgnoringModifiers), 0 si aucun.
 * Ordre (inv. E §6.4) : 1. table fixe par position (rangée des chiffres en VK_0..VK_9 comme Windows FR, Entrée, flèches,
 * F1-F20, pavé…) ; 2. lettre a-z du caractère → VK_A..VK_Z (AZERTY : la touche en position Q donne « a » → 0x41) ;
 * 3. ponctuation par caractère (AZERTY : kVK_ANSI_M donne « , » → 0xBC) ; 4. repli par position US (dispositions non
 * latines, caractères accentués), avec l'inversion ISO de 0x0A et 0x32 ; sinon 0. Modificateurs : 0 (flagsChanged). */
uint32_t sc2m_vk_de_kvk(uint16_t kvk, uint32_t car_sans_modif, int iso);

/* flagsChanged → 0xA0/0xA1 (Maj), 0xA2/0xA3 (Ctrl : Cmd, ou Ctrl physique transmis), 0xA4/0xA5 (Option en « alt »),
 * 0x5B/0x5C (Cmd en « cmd »), 0x14 (Verr. maj : *bas = verrou allumé), 0 = à ignorer. *bas lu sur le bit du CÔTÉ ;
 * événement sans bits de côté pour sa famille (synthétique) : bit indépendant du côté. o = NULL : défauts. */
uint32_t sc2m_vk_modificateur(uint16_t kvk, uint64_t drapeaux, const sc2m_clavier_options *o, int *bas);

/* keyDown : caractères à remettre au jeu (sc2_input_caractere, un par sortie, UTF-32), règle Win32 :
 *   - Ctrl vu du jeu, sans Alt : lettre → caractère de contrôle vk − 0x40 (Ctrl+A = 0x01, comme i_TranslateMessage),
 *     toute autre touche → rien ; Cmd en « cmd » (touche Windows) : rien ;
 *   - Alt vu du jeu (Option en « alt ») : car_sans_option (charactersIgnoringModifiers, Maj gardée) → WM_SYSCHAR ;
 *   - sinon : car (characters) ; touche morte : car vide → rien.
 * Entrées en UTF-16 (NSString getCharacters:), paires de substitution décodées. Normalisation Mac → Win32 : 0x7F
 * (Retour arrière) → 0x08, 0x03 (Entrée du pavé) → 0x0D, 0x19 (Maj+Tab) → 0x09 ; écartés : U+F700-U+F8FF (touches
 * de fonction d'AppKit : flèches, F-touches, Suppr…), autres caractères de contrôle (sauf 0x08, 0x09, 0x0D, 0x1B),
 * demi-paires orphelines. Rend le nombre de caractères écrits (≤ cap). */
int sc2m_caracteres(uint32_t vk, uint64_t drapeaux, const sc2m_clavier_options *o,
                    const uint16_t *car, size_t ncar, const uint16_t *car_sans_option, size_t nsans,
                    uint32_t *sortie, int cap);

/* VK mémorisé par touche physique : keyUp rejoue celui de l'appui, même si la disposition ou les modificateurs ont
 * changé entre-temps (risque n° 2 de l'inventaire : touche collée). */
typedef struct {
    uint8_t  vk_de[128];   /* kVK → VK de l'appui en cours, 0 = relâchée */
    uint64_t appuis, repetitions, relaches_orphelins;
} sc2m_clavier_etat;
void     sc2m_clavier_reinitialiser(sc2m_clavier_etat *e);                 /* perte du focus (compteurs gardés) */
uint32_t sc2m_clavier_appui(sc2m_clavier_etat *e, uint16_t kvk, uint32_t vk);   /* rend vk ; répétition comptée */
uint32_t sc2m_clavier_relache(sc2m_clavier_etat *e, uint16_t kvk);          /* VK de l'appui, 0 si aucun appui connu */
int      sc2m_clavier_tenues(const sc2m_clavier_etat *e);                  /* bilan « touches tenues N » */

/* Clavier ISO (0x0A à gauche du 1, 0x32 à côté de Maj gauche ; le pilote inverse ces deux codes selon le type) :
 * KBGetLayoutType(type) == 'ISO ' (HIToolbox par dlsym : la fonction n'est plus déclarée par le SDK ; 0 hors macOS ou
 * si l'appel manque). type = celui de l'ÉVÉNEMENT, à préférer : CGEventGetIntegerValueField([e CGEvent],
 * kCGKeyboardEventKeyboardType) — il suit le clavier réellement tapé (interne, externe branché en cours de partie).
 * sc2m_iso() lit LMGetKbdType(), le type du dernier événement reçu par le processus : avant le premier, une valeur
 * par défaut (43 = ANSI, mesuré le 22/10 sur le MacBook du mainteneur en ligne de commande). Aucun résultat mis en cache. */
int sc2m_iso_de_clavier(int type_clavier);
int sc2m_iso(void);
/* Le test sur une valeur de KBGetLayoutType : 1 pour 'ISO ', 0 sinon ('ANSI', 'JIS ', inconnu). */
int sc2m_iso_de_type(uint32_t type_disposition);

#endif
