/* sc2_lifted.h — ce que le code lifté de SC2.exe (lift32_cpu, modèle CPU-struct)
 * attend du runtime, en plus de cpu.h. Inclus de force (-include) AVANT cpu.h
 * dans chaque recomp_NN.c, pour que RECOMP_TODO soit le nôtre et non abort().
 */
#ifndef RECOMP_SC2_LIFTED_H
#define RECOMP_SC2_LIFTED_H

#include <stdint.h>

struct CPU_s;
/* Transfert vers une VA invitée : fonction liftée, stub d'import, ou manque
 * (rapporté et arrêt sur point identifié — runtime/sc2_dispatch.c). */
#define dispatch(c, va)     sc2_dispatch((void *)(c), (uint32_t)(va))
#define dispatch_jmp(c, va) sc2_dispatch((void *)(c), (uint32_t)(va))
void sc2_dispatch(void *c, uint32_t va);
/* Saut vers une AUTRE fonction liftée (passe 7j) : lift-boot.py réécrit les `{ dispatch(c, X); return; }` des
 * sauts (pas des appels) en SC2_SAUT. La cible est confiée au sc2_dispatch qui a appelé la fonction courante, qui
 * l'enchaîne en boucle au même niveau de pile — un vrai saut, pas une récursion C : une boucle invitée dont l'arc
 * arrière passe d'un morceau lifté à un autre (fonction découpée par le catalogue) ne fait plus grossir la pile hôte
 * à chaque tour (0x00F6F670 ↔ 0x00F6F87F, débordement à 8 191 cadres en 7j). */
/* Passe 10r : le saut en attente est un champ du CPU ((c)->saut, cpu.h), plus une variable __thread : sur Android, la .so
 * est chargée par dlopen et chaque accès __thread coûtait un appel au résolveur TLS dynamique. */
#define SC2_SAUT(c, va) do { if ((c)->saut) *(c)->saut = (uint32_t)(va); else sc2_dispatch((void *)(c), (uint32_t)(va)); } while (0)

/* Passe 10r : appel DIRECT d'une fonction liftée à adresse constante (réécriture de scripts/appels-directs.py, 90 % des
 * sites d'appel). Même sémantique que sc2_dispatch → dispatch_un : pile fantôme tenue, sauts SC2_SAUT de l'appelée
 * enchaînés au même niveau de pile (le premier par sc2_dispatch, qui boucle sur les suivants). Sans recherche de la
 * cible ni diagnostics par appel : compteurs, espion, jalons et « nouvelles fonctions » ne voient que les appels
 * restés sur sc2_dispatch (calculés, non liftés, ou exclus par scripts/appels-directs-exclus.txt). */
#ifndef SC2_SHADOW_DEFINI
#define SC2_SHADOW_DEFINI
typedef struct { uint32_t va[8192]; int n; uint32_t esp[8192]; } sc2_shadow;   /* esp : à l'entrée (passe 4) */
extern __thread sc2_shadow *g_sc2_ss;
#endif
#define SC2_APPEL(c, fn, adr) do { \
        uint32_t *sc2_ancien_ = (c)->saut, sc2_saut_ = 0; \
        sc2_shadow *sc2_ss_ = (c)->fantome ? (sc2_shadow *)(c)->fantome : g_sc2_ss; \
        if (sc2_ss_->n < (int)(sizeof sc2_ss_->va / sizeof sc2_ss_->va[0])) { \
            sc2_ss_->va[sc2_ss_->n] = (uint32_t)(adr); sc2_ss_->esp[sc2_ss_->n] = (c)->esp; } \
        sc2_ss_->n++; (c)->saut = &sc2_saut_; \
        fn(c); \
        (c)->saut = sc2_ancien_; sc2_ss_->n--; \
        if (sc2_saut_) sc2_dispatch((void *)(c), sc2_saut_); \
    } while (0)

/* Instruction que le lifter n'a pas su traduire. La macro est développée dans le
 * corps lifté, où `c` (CPU *) est en portée : le runtime émule les rares cas
 * simples (rdtsc, cmpxchg8b qword ptr [reg]) et continue ; sinon arrêt identifié,
 * pas abort() muet. */
void sc2_todo_c(void *c, uint32_t va, const char *text);
#define RECOMP_TODO(va, text) sc2_todo_c((void *)(c), (uint32_t)(va), (text))

/* setjmp / longjmp invités (passe 4). lift-boot.py remplace chaque `call _setjmp3`
 * par SC2_SETJMP3(c, retour) : le sigsetjmp hôte a lieu dans le cadre C de la
 * fonction liftée appelante (qui reste active tant que le jmp_buf est valide, comme
 * en C). sc2_setjmp_prepare mémorise ebx/esi/edi/ebp/esp invités et la profondeur
 * fantôme, indexés par l'adresse du jmp_buf invité ; le longjmp invité (stub IAT)
 * les restaure et fait siglongjmp ici ; sc2_setjmp_done émule le `ret` de _setjmp3. */
#include <setjmp.h>
sigjmp_buf *sc2_setjmp_prepare(void *c);
void sc2_setjmp_done(void *c, int v);
#define SC2_SETJMP3(c, ret) do { \
        push32((c), (ret)); \
        sigjmp_buf *sj_ = sc2_setjmp_prepare((void *)(c)); \
        int v_ = sigsetjmp(*sj_, 0); \
        sc2_setjmp_done((void *)(c), v_); \
    } while (0)

/* Passe 10s : registres invités en variables locales (scripts/registres-locaux.py, -DSC2_REGISTRES_LOCAUX=ON).
 * Une fonction transformée travaille sur une copie locale `rl` des 8 registres généraux et des 6 drapeaux. Seules les
 * aides de cpu.h toujours intégrées la reçoivent : son adresse ne s'échappe pas, le compilateur la scalarise (SROA) et
 * tient ces valeurs en registres hôtes, alors que les champs de *c doivent être relus après chaque écriture invitée
 * (wr32 peut faire alias avec *c). *c n'est à jour qu'aux frontières : avant un appel, un saut, un retour ou une entrée
 * dans le runtime, la passe vide les champs modifiés depuis la dernière synchronisation (masque calculé par flot de
 * données), puis recharge tout après. Le reste de l'état (x87, SSE, MMX, segments, fs_base, saut, fantome) reste dans
 * *c. TRACE-PASSE10S.md. */
#define SC2_RL_DECL(c) CPU sc2_rl_; CPU *const rl = &sc2_rl_; SC2_RL_CHARGE(c)
/* En tête de chaque boucle (cible d'un goto arrière) : barrière de compilation, aucune instruction émise. Sans elle, une
 * attente active invitée (« L: cmp byte ptr [edi+0x27d], 0 ; jne L », 0x009D1B00) ne contient plus aucun store (les
 * drapeaux vivent dans rl) et le compilateur hisse la lecture hors de la boucle : le fil ne voit jamais l'écriture de
 * l'autre fil. Dans le lift canon, les stores dans *c à chaque tour l'en empêchaient par accident. rl, jamais échappé,
 * n'est pas concerné. */
#define SC2_RL_BOUCLE() __asm__ __volatile__("" ::: "memory")
#define SC2_RL_CHARGE(c) do { \
        rl->eax = (c)->eax; rl->ecx = (c)->ecx; rl->edx = (c)->edx; rl->ebx = (c)->ebx; \
        rl->esp = (c)->esp; rl->ebp = (c)->ebp; rl->esi = (c)->esi; rl->edi = (c)->edi; \
        rl->cf = (c)->cf; rl->zf = (c)->zf; rl->sf = (c)->sf; rl->of = (c)->of; rl->pf = (c)->pf; rl->af = (c)->af; \
    } while (0)
/* masque : eax 0x0001, ecx 0x0002, edx 0x0004, ebx 0x0008, esp 0x0010, ebp 0x0020, esi 0x0040, edi 0x0080,
 *          cf 0x0100, zf 0x0200, sf 0x0400, of 0x0800, pf 0x1000, af 0x2000 (constant : les tests se replient) */
#ifdef SC2_RL_CONTROLE
/* Build de contrôle (-DSC2_RL_CONTROLE dans SC2_LIFTED_OPT) : à chaque frontière, un champ ABSENT du masque doit déjà
 * valoir la même chose dans rl et dans *c ; sinon le flot de données de la passe a oublié une écriture. Rapport par
 * sc2_rl_ecart (runtime/sc2_runtime.c) : fonction liftée, champs en écart, masque. */
void sc2_rl_ecart(const char *fn, uint32_t ecarts, uint32_t masque);
#define SC2_RL_CONTROLE_(c, m) do { uint32_t e_ = 0; \
        e_ |= rl->eax != (c)->eax ? 0x0001u : 0; e_ |= rl->ecx != (c)->ecx ? 0x0002u : 0; \
        e_ |= rl->edx != (c)->edx ? 0x0004u : 0; e_ |= rl->ebx != (c)->ebx ? 0x0008u : 0; \
        e_ |= rl->esp != (c)->esp ? 0x0010u : 0; e_ |= rl->ebp != (c)->ebp ? 0x0020u : 0; \
        e_ |= rl->esi != (c)->esi ? 0x0040u : 0; e_ |= rl->edi != (c)->edi ? 0x0080u : 0; \
        e_ |= rl->cf != (c)->cf ? 0x0100u : 0;   e_ |= rl->zf != (c)->zf ? 0x0200u : 0; \
        e_ |= rl->sf != (c)->sf ? 0x0400u : 0;   e_ |= rl->of != (c)->of ? 0x0800u : 0; \
        e_ |= rl->pf != (c)->pf ? 0x1000u : 0;   e_ |= rl->af != (c)->af ? 0x2000u : 0; \
        if (e_ & ~(uint32_t)(m)) sc2_rl_ecart(__func__, e_ & ~(uint32_t)(m), (uint32_t)(m)); \
    } while (0)
#else
#define SC2_RL_CONTROLE_(c, m) do { } while (0)
#endif
#define SC2_RL_VIDE(c, m) do { SC2_RL_CONTROLE_(c, m); \
        if ((m) & 0x0001u) (c)->eax = rl->eax; if ((m) & 0x0002u) (c)->ecx = rl->ecx; \
        if ((m) & 0x0004u) (c)->edx = rl->edx; if ((m) & 0x0008u) (c)->ebx = rl->ebx; \
        if ((m) & 0x0010u) (c)->esp = rl->esp; if ((m) & 0x0020u) (c)->ebp = rl->ebp; \
        if ((m) & 0x0040u) (c)->esi = rl->esi; if ((m) & 0x0080u) (c)->edi = rl->edi; \
        if ((m) & 0x0100u) (c)->cf = rl->cf;   if ((m) & 0x0200u) (c)->zf = rl->zf; \
        if ((m) & 0x0400u) (c)->sf = rl->sf;   if ((m) & 0x0800u) (c)->of = rl->of; \
        if ((m) & 0x1000u) (c)->pf = rl->pf;   if ((m) & 0x2000u) (c)->af = rl->af; \
    } while (0)
/* Passe 10s, vivacité interprocédurale des drapeaux (scripts/registres-locaux.py --vivacite-drapeaux) : à un appel
 * direct, x = drapeaux SALES que la passe ne vide pas, parce que ni l'appelée (ni ses propres appelées) ne les lit avant
 * de les écrire, ni l'appelant après le retour. Build normal : vidage de m seulement. Build de contrôle : x est écrit
 * INVERSÉ dans *c (empoisonné), et le contrôle de synchronisation porte sur les champs hors de m | x. Si la vivacité se
 * trompe, le jeu lit une valeur fausse et diverge (empreintes, mission). */
#ifdef SC2_RL_CONTROLE
#define SC2_RL_POISON_(c, x) do { \
        if ((x) & 0x0100u) (c)->cf = rl->cf ^ 1u; if ((x) & 0x0200u) (c)->zf = rl->zf ^ 1u; \
        if ((x) & 0x0400u) (c)->sf = rl->sf ^ 1u; if ((x) & 0x0800u) (c)->of = rl->of ^ 1u; \
        if ((x) & 0x1000u) (c)->pf = rl->pf ^ 1u; if ((x) & 0x2000u) (c)->af = rl->af ^ 1u; \
    } while (0)
#define SC2_RL_VIDEX(c, m, x) do { SC2_RL_CONTROLE_(c, (m) | (x)); SC2_RL_POISON_(c, x); \
        SC2_RL_VIDE_SEUL_(c, m); } while (0)
#else
#define SC2_RL_VIDEX(c, m, x) SC2_RL_VIDE_SEUL_(c, m)
#endif
#define SC2_RL_VIDE_SEUL_(c, m) do { \
        if ((m) & 0x0001u) (c)->eax = rl->eax; if ((m) & 0x0002u) (c)->ecx = rl->ecx; \
        if ((m) & 0x0004u) (c)->edx = rl->edx; if ((m) & 0x0008u) (c)->ebx = rl->ebx; \
        if ((m) & 0x0010u) (c)->esp = rl->esp; if ((m) & 0x0020u) (c)->ebp = rl->ebp; \
        if ((m) & 0x0040u) (c)->esi = rl->esi; if ((m) & 0x0080u) (c)->edi = rl->edi; \
        if ((m) & 0x0100u) (c)->cf = rl->cf;   if ((m) & 0x0200u) (c)->zf = rl->zf; \
        if ((m) & 0x0400u) (c)->sf = rl->sf;   if ((m) & 0x0800u) (c)->of = rl->of; \
        if ((m) & 0x1000u) (c)->pf = rl->pf;   if ((m) & 0x2000u) (c)->af = rl->af; \
    } while (0)

/* Passe 10t : pile x87 « relative au sommet » en variable locale (scripts/registres-locaux.py --x87,
 * option CMake SC2_RL_X87). Dans une fonction transformée, x87->s[k] (k CONSTANT, 0..7) est st(k) relatif au
 * sommet COURANT, x87->t le fpu_top courant (dynamique), x87->sw le mot d'état ; invariant :
 * x87->s[k] ≙ (c)->st[(x87->t + k) & 7]. POURQUOI RELATIF AU SOMMET ET PAS DANS rl : fst(c, i) adresse
 * st[(fpu_top + i) & 7] à indice DYNAMIQUE — un tel accès dans rl ferait abandonner SROA sur toute la variable
 * (accès à décalage inconnu) et perdrait le gain de la passe 10s dans la moitié du code chaud. Ici chaque accès
 * porte un indice littéral : clang scalarise s[0..7] et tient les valeurs en registres flottants. PUSH/POP
 * décalent les indices par rotation explicite (v évalué AVANT la rotation : « fld st(1) » lit l'ancien st(1) ;
 * la valeur dépilée reste dans s[7], même case physique — le canon ne l'efface pas). Synchronisé avec *c aux
 * MÊMES frontières que rl (masque 10 bits par flot de données : s0..s7 = 0x001..0x080, t = 0x100, sw = 0x200),
 * SC2_X87_VIDE juste après SC2_RL_VIDE/SC2_RL_VIDEX, SC2_X87_CHARGE juste après SC2_RL_CHARGE. TRACE-PASSE10S.md §8.3. */
#include <string.h>   /* memcmp du build de contrôle (NaN compris : comparaison bit à bit) */
struct sc2_x87 { double s[8]; uint32_t t, sw; };
#define SC2_X87_DECL(c) struct sc2_x87 sc2_x87_; struct sc2_x87 *const x87 = &sc2_x87_; SC2_X87_CHARGE(c)
#define SC2_X87_CHARGE(c) do { \
        x87->t = (uint32_t)(c)->fpu_top & 7u; x87->sw = (c)->fpu_sw; \
        x87->s[0] = (c)->st[(x87->t + 0u) & 7u]; x87->s[1] = (c)->st[(x87->t + 1u) & 7u]; \
        x87->s[2] = (c)->st[(x87->t + 2u) & 7u]; x87->s[3] = (c)->st[(x87->t + 3u) & 7u]; \
        x87->s[4] = (c)->st[(x87->t + 4u) & 7u]; x87->s[5] = (c)->st[(x87->t + 5u) & 7u]; \
        x87->s[6] = (c)->st[(x87->t + 6u) & 7u]; x87->s[7] = (c)->st[(x87->t + 7u) & 7u]; \
    } while (0)
#ifdef SC2_RL_CONTROLE
/* Build de contrôle : un champ x87 ABSENT du masque doit déjà être égal (bit à bit pour les slots : un NaN
 * ne passe pas ==) entre x87 et *c à la frontière ; rapport par sc2_x87_ecart (runtime/sc2_runtime.c). */
void sc2_x87_ecart(const char *fn, uint32_t ecarts, uint32_t masque);
#define SC2_X87_CONTROLE_(c, m) do { uint32_t e_ = 0; \
        e_ |= memcmp(&x87->s[0], &(c)->st[(x87->t + 0u) & 7u], 8) ? 0x001u : 0; \
        e_ |= memcmp(&x87->s[1], &(c)->st[(x87->t + 1u) & 7u], 8) ? 0x002u : 0; \
        e_ |= memcmp(&x87->s[2], &(c)->st[(x87->t + 2u) & 7u], 8) ? 0x004u : 0; \
        e_ |= memcmp(&x87->s[3], &(c)->st[(x87->t + 3u) & 7u], 8) ? 0x008u : 0; \
        e_ |= memcmp(&x87->s[4], &(c)->st[(x87->t + 4u) & 7u], 8) ? 0x010u : 0; \
        e_ |= memcmp(&x87->s[5], &(c)->st[(x87->t + 5u) & 7u], 8) ? 0x020u : 0; \
        e_ |= memcmp(&x87->s[6], &(c)->st[(x87->t + 6u) & 7u], 8) ? 0x040u : 0; \
        e_ |= memcmp(&x87->s[7], &(c)->st[(x87->t + 7u) & 7u], 8) ? 0x080u : 0; \
        e_ |= x87->t != ((uint32_t)(c)->fpu_top & 7u) ? 0x100u : 0; \
        e_ |= x87->sw != (c)->fpu_sw ? 0x200u : 0; \
        if (e_ & ~(uint32_t)(m)) sc2_x87_ecart(__func__, e_ & ~(uint32_t)(m), (uint32_t)(m)); \
    } while (0)
#else
#define SC2_X87_CONTROLE_(c, m) do { } while (0)
#endif
#define SC2_X87_VIDE(c, m) do { SC2_X87_CONTROLE_(c, m); \
        if ((m) & 0x001u) (c)->st[(x87->t + 0u) & 7u] = x87->s[0]; \
        if ((m) & 0x002u) (c)->st[(x87->t + 1u) & 7u] = x87->s[1]; \
        if ((m) & 0x004u) (c)->st[(x87->t + 2u) & 7u] = x87->s[2]; \
        if ((m) & 0x008u) (c)->st[(x87->t + 3u) & 7u] = x87->s[3]; \
        if ((m) & 0x010u) (c)->st[(x87->t + 4u) & 7u] = x87->s[4]; \
        if ((m) & 0x020u) (c)->st[(x87->t + 5u) & 7u] = x87->s[5]; \
        if ((m) & 0x040u) (c)->st[(x87->t + 6u) & 7u] = x87->s[6]; \
        if ((m) & 0x080u) (c)->st[(x87->t + 7u) & 7u] = x87->s[7]; \
        if ((m) & 0x100u) (c)->fpu_top = (int)x87->t; \
        if ((m) & 0x200u) (c)->fpu_sw = x87->sw; \
    } while (0)
#define SC2_X87_PUSH(v) do { double v_ = (v); \
        x87->t = (x87->t - 1u) & 7u; \
        x87->s[7] = x87->s[6]; x87->s[6] = x87->s[5]; x87->s[5] = x87->s[4]; x87->s[4] = x87->s[3]; \
        x87->s[3] = x87->s[2]; x87->s[2] = x87->s[1]; x87->s[1] = x87->s[0]; x87->s[0] = v_; \
    } while (0)
#define SC2_X87_POP() do { double v_ = x87->s[0]; \
        x87->s[0] = x87->s[1]; x87->s[1] = x87->s[2]; x87->s[2] = x87->s[3]; x87->s[3] = x87->s[4]; \
        x87->s[4] = x87->s[5]; x87->s[5] = x87->s[6]; x87->s[6] = x87->s[7]; x87->s[7] = v_; \
        x87->t = (x87->t + 1u) & 7u; \
    } while (0)
#define SC2_X87_ST(i) (x87->s[(i)])
#define SC2_X87_FCOM(a, b) fcompare_sw(&x87->sw, (a), (b))
#define SC2_X87_FNSTSW() ((x87->sw & ~0x3800u & 0xFFFFu) | ((x87->t & 7u) << 11))

/* Passe 10u : registres SSE xmm0..xmm7 en variable locale (scripts/registres-locaux.py --sse, option CMake
 * SC2_RL_SSE). Dans une fonction transformée, chaque c->xmm[K] (K littéral 0..7, toutes formes : champ
 * .f32/.u32/.u64…, registre entier, &sse->x[K] de sse_load_ss/sse_load_sd, valeur passée à wrxm) devient
 * sse->x[K] : indices constants, clang scalarise les 8 × 16 octets en registres hôtes au lieu d'un accès
 * mémoire à *c par lecture/écriture. Synchronisé avec *c aux MÊMES frontières que rl (masque 8 bits par flot
 * de données : xmm0..xmm7 = 0x01..0x80), SC2_SSE_VIDE juste après SC2_RL_VIDE/SC2_RL_VIDEX (et SC2_X87_VIDE),
 * SC2_SSE_CHARGE juste après SC2_RL_CHARGE (et SC2_X87_CHARGE). La structure est définie DANS le corps de la
 * fonction : sc2_lifted.h est incluse avant cpu.h, où XMM n'existe pas encore ; au développement de la macro,
 * cpu.h est déjà lu. TRACE-PASSE10S.md. */
#define SC2_SSE_DECL(c) struct sc2_sse { XMM x[8]; } sc2_sse_; \
    struct sc2_sse *const sse = &sc2_sse_; SC2_SSE_CHARGE(c)
#define SC2_SSE_CHARGE(c) do { \
        sse->x[0] = (c)->xmm[0]; sse->x[1] = (c)->xmm[1]; sse->x[2] = (c)->xmm[2]; sse->x[3] = (c)->xmm[3]; \
        sse->x[4] = (c)->xmm[4]; sse->x[5] = (c)->xmm[5]; sse->x[6] = (c)->xmm[6]; sse->x[7] = (c)->xmm[7]; \
    } while (0)
#ifdef SC2_RL_CONTROLE
/* Build de contrôle : un registre ABSENT du masque doit déjà être égal (bit à bit, memcmp — un NaN ne passe
 * pas ==) entre sse et *c à la frontière ; rapport par sc2_sse_ecart (runtime/sc2_runtime.c). */
void sc2_sse_ecart(const char *fn, uint32_t ecarts, uint32_t masque);
#define SC2_SSE_CONTROLE_(c, m) do { uint32_t e_ = 0; \
        e_ |= memcmp(&sse->x[0], &(c)->xmm[0], sizeof(XMM)) ? 0x01u : 0; \
        e_ |= memcmp(&sse->x[1], &(c)->xmm[1], sizeof(XMM)) ? 0x02u : 0; \
        e_ |= memcmp(&sse->x[2], &(c)->xmm[2], sizeof(XMM)) ? 0x04u : 0; \
        e_ |= memcmp(&sse->x[3], &(c)->xmm[3], sizeof(XMM)) ? 0x08u : 0; \
        e_ |= memcmp(&sse->x[4], &(c)->xmm[4], sizeof(XMM)) ? 0x10u : 0; \
        e_ |= memcmp(&sse->x[5], &(c)->xmm[5], sizeof(XMM)) ? 0x20u : 0; \
        e_ |= memcmp(&sse->x[6], &(c)->xmm[6], sizeof(XMM)) ? 0x40u : 0; \
        e_ |= memcmp(&sse->x[7], &(c)->xmm[7], sizeof(XMM)) ? 0x80u : 0; \
        if (e_ & ~(uint32_t)(m)) sc2_sse_ecart(__func__, e_ & ~(uint32_t)(m), (uint32_t)(m)); \
    } while (0)
#else
#define SC2_SSE_CONTROLE_(c, m) do { } while (0)
#endif
#define SC2_SSE_VIDE(c, m) do { SC2_SSE_CONTROLE_(c, m); \
        if ((m) & 0x01u) (c)->xmm[0] = sse->x[0]; \
        if ((m) & 0x02u) (c)->xmm[1] = sse->x[1]; \
        if ((m) & 0x04u) (c)->xmm[2] = sse->x[2]; \
        if ((m) & 0x08u) (c)->xmm[3] = sse->x[3]; \
        if ((m) & 0x10u) (c)->xmm[4] = sse->x[4]; \
        if ((m) & 0x20u) (c)->xmm[5] = sse->x[5]; \
        if ((m) & 0x40u) (c)->xmm[6] = sse->x[6]; \
        if ((m) & 0x80u) (c)->xmm[7] = sse->x[7]; \
    } while (0)

#endif
