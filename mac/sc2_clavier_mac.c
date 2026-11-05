/* mac/sc2_clavier_mac.c — clavier macOS → touches et caractères Win32, en C pur (V3, lot M2 ; cf. sc2_clavier_mac.h).
 * Codes kVK : Events.h du SDK (HIToolbox, l. 197-328) ; bits de côté : IOLLEvent.h:253-261 ; table et ordre de
 * décision : inventaire v3-entrees §6.4. */
#include <string.h>

#include "sc2_clavier_mac.h"

#ifdef __APPLE__
#include <dlfcn.h>
#endif

#define IGN 0xFF   /* FIXE[] : touche connue, volontairement non traduite (pas de repli par caractère ni position) */

/* 1. Table fixe par position, indépendante de la disposition. */
static const uint8_t FIXE[128] = {
    /* rangée des chiffres, par position (VK_1… sur AZERTY, comme Windows FR) */
    [0x12] = 0x31, [0x13] = 0x32, [0x14] = 0x33, [0x15] = 0x34, [0x17] = 0x35,
    [0x16] = 0x36, [0x1A] = 0x37, [0x1C] = 0x38, [0x19] = 0x39, [0x1D] = 0x30,
    [0x24] = 0x0D, [0x4C] = 0x0D,                   /* Entrée, Entrée du pavé */
    [0x30] = 0x09, [0x31] = 0x20, [0x33] = 0x08,    /* Tab, Espace, Retour arrière (kVK_Delete) */
    [0x75] = 0x2E, [0x35] = 0x1B, [0x72] = 0x2D,    /* Suppr (ForwardDelete), Échap, Help → Inser */
    [0x73] = 0x24, [0x77] = 0x23, [0x74] = 0x21, [0x79] = 0x22,   /* Début, Fin, Pg préc., Pg suiv. */
    [0x7B] = 0x25, [0x7C] = 0x27, [0x7D] = 0x28, [0x7E] = 0x26,   /* ← → ↓ ↑ */
    /* F1-F12 */
    [0x7A] = 0x70, [0x78] = 0x71, [0x63] = 0x72, [0x76] = 0x73, [0x60] = 0x74, [0x61] = 0x75,
    [0x62] = 0x76, [0x64] = 0x77, [0x65] = 0x78, [0x6D] = 0x79, [0x67] = 0x7A, [0x6F] = 0x7B,
    /* F13-F20 */
    [0x69] = 0x7C, [0x6B] = 0x7D, [0x71] = 0x7E, [0x6A] = 0x7F, [0x40] = 0x80, [0x4F] = 0x81, [0x50] = 0x82, [0x5A] = 0x83,
    /* pavé numérique (pas de Verr. num sur Mac : toujours numérique) */
    [0x52] = 0x60, [0x53] = 0x61, [0x54] = 0x62, [0x55] = 0x63, [0x56] = 0x64,
    [0x57] = 0x65, [0x58] = 0x66, [0x59] = 0x67, [0x5B] = 0x68, [0x5C] = 0x69,
    [0x41] = 0x6E, [0x43] = 0x6A, [0x45] = 0x6B, [0x4E] = 0x6D, [0x4B] = 0x6F,   /* . * + - / */
    [0x47] = 0x0C, [0x51] = IGN,                    /* Clear → VK_CLEAR ; = du pavé : rien */
    [0x6E] = 0x5D,                                  /* menu contextuel → VK_APPS */
    /* modificateurs et fn : flagsChanged seulement (sc2m_vk_modificateur) */
    [0x36] = IGN, [0x37] = IGN, [0x38] = IGN, [0x39] = IGN, [0x3A] = IGN,
    [0x3B] = IGN, [0x3C] = IGN, [0x3D] = IGN, [0x3E] = IGN, [0x3F] = IGN,
    [0x48] = IGN, [0x49] = IGN, [0x4A] = IGN,       /* volume +, volume −, sourdine */
    [0x5D] = IGN, [0x5E] = IGN, [0x5F] = IGN, [0x66] = IGN, [0x68] = IGN,   /* JIS : Yen, _, virgule du pavé, Eisu, Kana */
};

/* 4. Repli par position US (lettres ANSI et ponctuation) ; 0x0A et 0x32 : cf. position_iso(). */
static const uint8_t POS_US[128] = {
    [0x00] = 'A', [0x01] = 'S', [0x02] = 'D', [0x03] = 'F', [0x04] = 'H', [0x05] = 'G', [0x06] = 'Z', [0x07] = 'X',
    [0x08] = 'C', [0x09] = 'V', [0x0B] = 'B', [0x0C] = 'Q', [0x0D] = 'W', [0x0E] = 'E', [0x0F] = 'R', [0x10] = 'Y',
    [0x11] = 'T', [0x1F] = 'O', [0x20] = 'U', [0x22] = 'I', [0x23] = 'P', [0x25] = 'L', [0x26] = 'J', [0x28] = 'K',
    [0x2D] = 'N', [0x2E] = 'M',
    [0x18] = 0xBB, [0x1B] = 0xBD, [0x1E] = 0xDD, [0x21] = 0xDB, [0x27] = 0xDE, [0x29] = 0xBA, [0x2A] = 0xDC,
    [0x2B] = 0xBC, [0x2C] = 0xBF, [0x2F] = 0xBE,
};

/* 3. Ponctuation par caractère (indispensable sur AZERTY : kVK_ANSI_M y produit « , »). */
static uint32_t ponctuation(uint32_t c)
{
    switch (c) {
    case ',': return 0xBC;  case '.': return 0xBE;  case ';': return 0xBA;  case '/': return 0xBF;
    case '-': return 0xBD;  case '=': return 0xBB;  case '[': return 0xDB;  case ']': return 0xDD;
    case '\'': return 0xDE; case '\\': return 0xDC; case '`': return 0xC0;  case '<': return 0xE2;
    default: return 0;
    }
}

uint32_t sc2m_vk_de_kvk(uint16_t kvk, uint32_t car_sans_modif, int iso)
{
    if (kvk < 128 && FIXE[kvk]) return FIXE[kvk] == IGN ? 0 : FIXE[kvk];
    uint32_t c = car_sans_modif;
    if (c >= 'A' && c <= 'Z') c += 0x20;
    if (c >= 'a' && c <= 'z') return 0x41 + (c - 'a');              /* 2. lettre (minuscule) */
    uint32_t p = ponctuation(c);
    if (p) return p;                                                /* 3. ponctuation */
    if (kvk == 0x32) return iso ? 0xE2 : 0xC0;                      /* 4. ISO : 0x32 = touche à côté de Maj gauche */
    if (kvk == 0x0A) return iso ? 0xC0 : 0xE2;                      /*    ISO : 0x0A (Section) = touche à gauche du 1 */
    return kvk < 128 ? POS_US[kvk] : 0;
}

static const sc2m_clavier_options DEFAUT = SC2M_CLAVIER_OPTIONS_DEFAUT;

uint32_t sc2m_vk_modificateur(uint16_t kvk, uint64_t drapeaux, const sc2m_clavier_options *o, int *bas)
{
    int b = 0;
    uint32_t vk = 0;
    uint64_t famille = 0, cote = 0, indep = 0;
    if (!o) o = &DEFAUT;
    switch (kvk) {
    case 0x38: vk = 0xA0; famille = SC2M_DEV_LMAJ | SC2M_DEV_RMAJ; cote = SC2M_DEV_LMAJ; indep = SC2M_DRAP_MAJ; break;
    case 0x3C: vk = 0xA1; famille = SC2M_DEV_LMAJ | SC2M_DEV_RMAJ; cote = SC2M_DEV_RMAJ; indep = SC2M_DRAP_MAJ; break;
    case 0x3B: vk = o->ctrl_transmis ? 0xA2 : 0;
               famille = SC2M_DEV_LCTRL | SC2M_DEV_RCTRL; cote = SC2M_DEV_LCTRL; indep = SC2M_DRAP_CTRL; break;
    case 0x3E: vk = o->ctrl_transmis ? 0xA3 : 0;
               famille = SC2M_DEV_LCTRL | SC2M_DEV_RCTRL; cote = SC2M_DEV_RCTRL; indep = SC2M_DRAP_CTRL; break;
    case 0x37: vk = o->cmd_vers_ctrl ? 0xA2 : 0x5B;
               famille = SC2M_DEV_LCMD | SC2M_DEV_RCMD; cote = SC2M_DEV_LCMD; indep = SC2M_DRAP_CMD; break;
    case 0x36: vk = o->cmd_vers_ctrl ? 0xA3 : 0x5C;
               famille = SC2M_DEV_LCMD | SC2M_DEV_RCMD; cote = SC2M_DEV_RCMD; indep = SC2M_DRAP_CMD; break;
    case 0x3A: vk = o->option_altgr ? 0 : 0xA4;
               famille = SC2M_DEV_LOPTION | SC2M_DEV_ROPTION; cote = SC2M_DEV_LOPTION; indep = SC2M_DRAP_OPTION; break;
    case 0x3D: vk = o->option_altgr ? 0 : 0xA5;
               famille = SC2M_DEV_LOPTION | SC2M_DEV_ROPTION; cote = SC2M_DEV_ROPTION; indep = SC2M_DRAP_OPTION; break;
    case 0x39: if (bas) *bas = (drapeaux & SC2M_DRAP_VERR_MAJ) != 0;   /* un événement par bascule, pas de keyUp */
               return 0x14;
    default:   if (bas) *bas = 0;                                   /* fn (0x3F) et le reste : ignorés */
               return 0;
    }
    /* bit du côté si l'événement en porte pour cette famille ; sinon (NSEvent synthétique) bit indépendant du côté */
    b = (drapeaux & famille) ? (drapeaux & cote) != 0 : (drapeaux & indep) != 0;
    if (bas) *bas = b;
    return vk;
}

int sc2m_caracteres(uint32_t vk, uint64_t drapeaux, const sc2m_clavier_options *o,
                    const uint16_t *car, size_t ncar, const uint16_t *car_sans_option, size_t nsans,
                    uint32_t *sortie, int cap)
{
    if (!o) o = &DEFAUT;
    if (!sortie || cap <= 0) return 0;
    int cmd = (drapeaux & SC2M_DRAP_CMD) != 0, ctrl = (drapeaux & SC2M_DRAP_CTRL) != 0;
    int opt = (drapeaux & SC2M_DRAP_OPTION) != 0;
    if (cmd && !o->cmd_vers_ctrl) return 0;                        /* Cmd = touche Windows : aucun caractère */
    int ctrl_jeu = (cmd && o->cmd_vers_ctrl) || (ctrl && o->ctrl_transmis);
    int alt_jeu = opt && !o->option_altgr;
    if (ctrl_jeu) {                                                 /* Win32 : seul Ctrl+lettre produit vk − 0x40 */
        if (alt_jeu || vk < 0x41 || vk > 0x5A) return 0;
        sortie[0] = vk - 0x40;
        return 1;
    }
    if (ctrl) return 0;                                             /* Ctrl physique réservé au Ctrl+clic : pas de texte */
    const uint16_t *s = alt_jeu ? car_sans_option : car;
    size_t n = alt_jeu ? nsans : ncar;
    int k = 0;
    for (size_t i = 0; s && i < n && k < cap; i++) {
        uint32_t u = s[i];
        if (u >= 0xD800 && u <= 0xDBFF) {                           /* paire de substitution */
            if (i + 1 < n && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (uint32_t)(s[i + 1] - 0xDC00);
                i++;
            } else continue;                                        /* demi-paire orpheline */
        } else if (u >= 0xDC00 && u <= 0xDFFF) continue;
        if (u == 0x7F) u = 0x08;                                    /* Retour arrière (NSDeleteCharacter) */
        else if (u == 0x03) u = 0x0D;                               /* Entrée du pavé (NSEnterCharacter) */
        else if (u == 0x19) u = 0x09;                               /* Maj+Tab (NSBackTabCharacter) */
        if (u >= 0xF700 && u <= 0xF8FF) continue;                   /* NS*FunctionKey : flèches, F-touches, Suppr… */
        if (u < 0x20 && u != 0x08 && u != 0x09 && u != 0x0D && u != 0x1B) continue;
        if (u >= 0x80 && u <= 0x9F) continue;                       /* contrôles C1 */
        sortie[k++] = u;
    }
    return k;
}

int sc2m_clavier_options_lire(sc2m_clavier_options *o, const char *cmd, const char *ctrl, const char *option)
{
    int inconnues = 0;
    *o = DEFAUT;
    if (cmd && *cmd) {
        if (!strcmp(cmd, "ctrl")) o->cmd_vers_ctrl = 1;
        else if (!strcmp(cmd, "cmd")) o->cmd_vers_ctrl = 0;
        else inconnues++;
    }
    if (ctrl && *ctrl) {
        if (!strcmp(ctrl, "clic")) o->ctrl_transmis = 0;
        else if (!strcmp(ctrl, "ctrl")) o->ctrl_transmis = 1;
        else inconnues++;
    }
    if (option && *option) {
        if (!strcmp(option, "alt")) o->option_altgr = 0;
        else if (!strcmp(option, "altgr")) o->option_altgr = 1;
        else inconnues++;
    }
    return inconnues;
}

void sc2m_clavier_reinitialiser(sc2m_clavier_etat *e)
{
    memset(e->vk_de, 0, sizeof e->vk_de);
}

uint32_t sc2m_clavier_appui(sc2m_clavier_etat *e, uint16_t kvk, uint32_t vk)
{
    if (kvk >= sizeof e->vk_de || !vk || vk > 0xFF) return 0;      /* hors table : refusée (jamais de touche collée) */
    if (e->vk_de[kvk]) { e->repetitions++; return e->vk_de[kvk]; }   /* répétition : le VK de l'appui */
    e->vk_de[kvk] = (uint8_t)vk;
    e->appuis++;
    return vk;
}

uint32_t sc2m_clavier_relache(sc2m_clavier_etat *e, uint16_t kvk)
{
    if (kvk >= sizeof e->vk_de || !e->vk_de[kvk]) { e->relaches_orphelins++; return 0; }
    uint32_t vk = e->vk_de[kvk];
    e->vk_de[kvk] = 0;
    return vk;
}

int sc2m_clavier_tenues(const sc2m_clavier_etat *e)
{
    int n = 0;
    for (size_t i = 0; i < sizeof e->vk_de; i++) n += e->vk_de[i] != 0;
    return n;
}

int sc2m_iso_de_type(uint32_t type_disposition)
{
    return type_disposition == 0x49534F20u;                         /* kKeyboardISO = 'ISO ' */
}

#ifdef __APPLE__
static uint32_t (*g_kb_type)(int16_t);              /* KBGetLayoutType */
static uint8_t (*g_lm_type)(void);                  /* LMGetKbdType */
static int g_hitoolbox = -1;                        /* -1 : pas encore cherché ; 0 : absent ; 1 : résolu */
static int hitoolbox(void)
{
    if (g_hitoolbox >= 0) return g_hitoolbox;
    void *h = dlopen("/System/Library/Frameworks/Carbon.framework/Versions/A/Frameworks/HIToolbox.framework/Versions/A/HIToolbox",
                     RTLD_LAZY | RTLD_LOCAL);
    if (h) {
        g_kb_type = (uint32_t (*)(int16_t))dlsym(h, "KBGetLayoutType");
        g_lm_type = (uint8_t (*)(void))dlsym(h, "LMGetKbdType");
    }
    g_hitoolbox = g_kb_type && g_lm_type;
    return g_hitoolbox;
}
#endif

int sc2m_iso_de_clavier(int type_clavier)
{
#ifdef __APPLE__
    if (hitoolbox()) return sc2m_iso_de_type(g_kb_type((int16_t)type_clavier));
#else
    (void)type_clavier;
#endif
    return 0;
}

int sc2m_iso(void)
{
#ifdef __APPLE__
    if (hitoolbox()) return sc2m_iso_de_clavier(g_lm_type());
#endif
    return 0;
}
