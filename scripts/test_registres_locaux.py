#!/usr/bin/env python3
"""Tests unittest de scripts/registres-locaux.py — fixtures manuscrites, jamais de code du jeu.

Chaque fixture est une fonction liftée factice (mêmes formes que le lifter :
une instruction x86 par ligne, commentaire /* VA: asm */ en fin de ligne) ; les
aides de cpu.h réelles (push32, flags_sub…) ne sont que citées, jamais définies
ici. Les masques attendus sont calculés à la main depuis la sémantique x86 et
les bits de sc2_lifted.h (eax 1, ecx 2, edx 4, ebx 8, esp 10, ebp 20, esi 40,
edi 80, cf 100, zf 200, sf 400, of 800, pf 1000, af 2000). Pour --x87 (passe
10t) : s0 1, s1 2, s2 4, s3 8, s4 10, s5 20, s6 40, s7 80, t 100, sw 200 (3
chiffres hexa) ; PUSH fait monter les bits s d'un cran (D' = ((D & 0x07F) << 1)
| 0x001 | 0x100 | (D & 0x200)), POP les fait tourner vers le bas (s0 part en s7),
écriture ST(i) salit 1 << i, fcom salit sw.
"""
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).resolve().parent / "registres-locaux.py"
CPU_H = Path(__file__).resolve().parent.parent / "recomp-sc2" / "runtime" / "cpu.h"

ENTETE = '/* AUTO-GENERATED */\n#include "cpu.h"\n#include "sc2_lifted.h"\n\n'


def fonction(nom, lignes):
    """Fonction liftée factice (sans l'en-tête de fichier)."""
    return f"void L_{nom}(CPU *c)\n{{\n" + "".join(lignes) + "}\n\n"


# ---- fixtures : une forme par test ----

# simple : deux écritures, un return → masque du return = eax|ebx
F_SIMPLE = fonction("00A00000", [
    "    c->eax = 0x1u;                                            /* 00A00000: mov eax, 1 */\n",
    "    c->ebx = c->eax;                                          /* 00A00005: mov ebx, eax */\n",
    "    return;                                                   /* 00A0000A: ret */\n",
])

# push puis appel : push32 écrit esp (0x0010) ; frontière d'appel = 0x0010
F_PUSH_APPEL = fonction("00A00010", [
    "    push32(c, 0x00A00030u);                                   /* 00A00010: push 0xa00030 */\n",
    "    dispatch(c, 0x00A00040u);                                 /* 00A00015: call 0xa00040 */\n",
    "    return;                                                   /* 00A0001A: ret */\n",
])

# deux appels successifs : le second a un masque vide (tout rechargé au premier)
F_DEUX_APPELS = fonction("00A00020", [
    "    c->eax = 0x7u;                                            /* 00A00020: mov eax, 7 */\n",
    "    dispatch(c, 0x00A00040u);                                 /* 00A00025: call 0xa00040 */\n",
    "    dispatch(c, 0x00A00050u);                                 /* 00A0002A: call 0xa00050 */\n",
    "    return;                                                   /* 00A0002F: ret */\n",
])

# fusion à une étiquette : deux chemins d'écritures disjointes, union au label
F_FUSION = fonction("00A00030", [
    "    c->ecx = 1u;                                              /* 00A00030: mov ecx, 1 */\n",
    "    if (c->zf) goto L_00A0004A;                               /* 00A00035: jz 0xa0004a */\n",
    "    c->edx = 2u;                                              /* 00A0003B: mov edx, 2 */\n",
    "    goto L_00A0004A;                                          /* 00A00040: jmp 0xa0004a */\n",
    "L_00A0004A:\n",
    "    c->esi = c->edi;                                          /* 00A0004A: mov esi, edi */\n",
    "    return;                                                   /* 00A0004F: ret */\n",
])

# boucle : point fixe, l'arc arrière force l'union à l'étiquette de boucle
F_BOUCLE = fonction("00A00050", [
    "    c->ecx = 10u;                                             /* 00A00050: mov ecx, 0xa */\n",
    "L_00A00055:\n",
    "    c->eax = c->ecx;                                          /* 00A00055: mov eax, ecx */\n",
    "    c->ecx = c->ecx - 1u;                                     /* 00A0005A: dec ecx */\n",
    "    flags_sub(c, c->ecx, 1u, 4);                              /* 00A0005A: dec ecx */\n",
    "    if (!c->zf) goto L_00A00055;                              /* 00A0005F: jne 0xa00055 */\n",
    "    return;                                                   /* 00A00064: ret */\n",
])

# table de sauts : bloc multi-lignes { si _jt == … goto … ; dispatch_jmp ; return }
F_JUMP_TABLE = fonction("00A00060", [
    "    { uint32_t _jt = rd32((c->eax*4 + GVA(0x00A000C0)));       /* 00A00060: jmp [eax*4+0xa000c0] */\n",
    "      if (_jt == GVA(0x00A00070)) goto L_00A00070;             /* 00A00060: jmp [eax*4+0xa000c0] */\n",
    "      dispatch_jmp(c, _jt); return; }                          /* 00A00060: jmp [eax*4+0xa000c0] */\n",
    "L_00A00070:\n",
    "    c->edx = 1u;                                              /* 00A00070: mov edx, 1 */\n",
    "    return;                                                   /* 00A00075: ret */\n",
])

# if (…) { SC2_SAUT ; return; } : frontière dans un bloc terminal — transformable
F_SI_SAUT = fonction("00A00080", [
    "    c->eax = c->eax + 1u;                                     /* 00A00080: inc eax */\n",
    "    flags_add(c, c->eax, 1u, 4);                              /* 00A00080: inc eax */\n",
    "    if (c->zf) { SC2_SAUT(c, 0x00A00090u); return; }           /* 00A00085: jz 0xa00090 */\n",
    "    return;                                                   /* 00A0008A: ret */\n",
])

# rep movs : while (c->ecx) { … } toléré, écritures fondues dans la chute
F_REP_MOVS = fonction("00A00090", [
    "    c->ecx = 4u;                                              /* 00A00090: mov ecx, 4 */\n",
    "    while (c->ecx) { wr32(c->edi, rd32(c->esi)); c->esi += 4; c->edi += 4; c->ecx--; } /* 00A00095: rep movsd */\n",
    "    return;                                                   /* 00A0009A: ret */\n",
])

# cmpxchg8b : bloc multi-lignes, écriture conditionnelle sans frontière
# attente active (0x009D1B00 du jeu, réécrite) : la lecture doit rester dans la boucle → barrière en tête de boucle ;
# l'étiquette atteinte seulement par un goto AVANT n'en reçoit pas
F_ATTENTE = fonction("00A000D0", [
    "    if (c->zf) goto L_00A000E0;                               /* 00A000D0: je 0xa000e0 */\n",
    "L_00A000D6:\n",
    "    flags_sub(c, rd8((c->edi + 0x0000027Du)), 0x0u, 1);       /* 00A000D6: cmp byte ptr [edi + 0x27d], 0 */\n",
    "    if (!c->zf) goto L_00A000D6;                              /* 00A000DD: jne 0xa000d6 */\n",
    "L_00A000E0:\n",
    "    return;                                                   /* 00A000E0: ret */\n",
])

# commentaire AVANT le code (« /* indirect jmp */ dispatch_jmp(…); return; », 804 lignes du lift) : la ligne doit être
# analysée et réécrite comme les autres, le commentaire de tête recopié tel quel
F_COMMENTAIRE_TETE = fonction("00A000B0", [
    "    c->eax = 0x5u;                                            /* 00A000B0: mov eax, 5 */\n",
    "    /* indirect jmp */ dispatch_jmp(c, c->eax); return;       /* 00A000B5: jmp eax */\n",
])

# même forme sans registre dans la cible : seul l'emballage trahit un oubli
F_COMMENTAIRE_TETE_GVA = fonction("00A000C0", [
    "    c->eax = 0x5u;                                            /* 00A000C0: mov eax, 5 */\n",
    "    /* indirect jmp */ dispatch_jmp(c, rd32((GVA(0x01419308)))); return; /* 00A000C5: jmp [0x1419308] */\n",
])

F_CMPXCHG8B = fonction("00A000A0", [
    "    c->eax = (uint32_t)(c->ebx);                              /* 00A000A0: mov eax, ebx */\n",
    "    c->edx = (uint32_t)(c->ecx);                              /* 00A000A2: mov edx, ecx */\n",
    "    { uint64_t _e = ((uint64_t)c->edx << 32) | c->eax, _n = ((uint64_t)c->ecx << 32) | c->ebx; /* 00A000A4: lock cmpxchg8b */\n",
    "      uint64_t _old = __sync_val_compare_and_swap((volatile uint64_t *)GPTR((c->ecx)), _e, _n); /* 00A000A4: lock cmpxchg8b */\n",
    "      c->zf = (_old == _e);                                   /* 00A000A4: lock cmpxchg8b */\n",
    "      if (!c->zf) { c->eax = (uint32_t)_old; c->edx = (uint32_t)(_old >> 32); } } /* 00A000A4: lock cmpxchg8b */\n",
    "    return;                                                   /* 00A000A8: ret */\n",
])

# frontières runtime : RECOMP_TODO (sans c textuel) et SC2_SETJMP3
F_RUNTIME = fonction("00A000B0", [
    "    c->ebx = 0x2u;                                            /* 00A000B0: mov ebx, 2 */\n",
    '    RECOMP_TODO(0x00A000B5, "rdtsc");                         /* 00A000B5: rdtsc */\n',
    "    SC2_SETJMP3(c, 0x00A000C0u);                              /* 00A000BA: call _setjmp3 */\n",
    "    return;                                                   /* 00A000BF: ret */\n",
])

# SC2_APPEL (produit par la passe précédente) et appel direct L_X(c)
F_APPELS_DIRECTS = fonction("00A000C0", [
    "    c->esi = 0x3u;                                            /* 00A000C0: mov esi, 3 */\n",
    "    SC2_APPEL(c, L_00A000D0, 0x00A000D0u);                    /* 00A000C5: call 0xa000d0 */\n",
    "    L_00A000D0(c);                                            /* 00A000CA: call 0xa000d0 */\n",
    "    return;                                                   /* 00A000CF: ret */\n",
])

# aide hors cache (fst) : reste avec c, pas de frontière
F_HORS_CACHE = fonction("00A000D0", [
    "    fst(c, 0);                                                /* 00A000D0: fstp st(0) */\n",
    "    c->edi = 0x5u;                                            /* 00A000D2: mov edi, 5 */\n",
    "    return;                                                   /* 00A000D7: ret */\n",
])

# SET8L : macro d'écriture de sous-registre
F_SET8L = fonction("00A000E0", [
    "    SET8L(c->eax, c->ebx & 0xFFu);                            /* 00A000E0: mov al, bl */\n",
    "    return;                                                   /* 00A000E5: ret */\n",
])

# pour (int _i …) SSE : admis (ne touche que xmm)
F_POUR_SSE = fonction("00A000F0", [
    "    for (int _i = 0; _i < 4; _i++) { c->xmm[_i] = c->xmm[_i]; } /* 00A000F0: movaps */\n",
    "    return;                                                   /* 00A000F5: ret */\n",
])

# ---- replis §5 : la fonction doit ressortir octet pour octet identique ----

# frontière dans un if sans return terminal
R_FRONT_COND = fonction("00B00000", [
    "    if (c->zf) { dispatch(c, 0x00B00010u); c->eax = 1u; }      /* 00B00000: jz + call */\n",
    "    return;                                                   /* 00B00005: ret */\n",
])

# frontière dans un while
R_FRONT_WHILE = fonction("00B00010", [
    "    while (c->ecx) { dispatch(c, 0x00B00020u); c->ecx--; }     /* 00B00010: bizarre */\n",
    "    return;                                                   /* 00B00015: ret */\n",
])

# c utilisé autrement que c->champ ou premier argument
R_C_AUTREMENT = fonction("00B00020", [
    "    CPU *p = c;                                               /* 00B00020: mov reg, reg */\n",
    "    return;                                                   /* 00B00025: ret */\n",
])

# goto vers une étiquette absente
R_GOTO_ABSENT = fonction("00B00030", [
    "    goto L_00B000FF;                                          /* 00B00030: jmp 0xb000ff */\n",
    "    return;                                                   /* 00B00035: ret */\n",
])

# switch (jamais dans le canon réel, repli exigé)
R_SWITCH = fonction("00B00040", [
    "    switch (c->eax) { default: break; }                       /* 00B00040: bizarre */\n",
    "    return;                                                   /* 00B00045: ret */\n",
])

# en-tête différente (paramètre nommé autrement)
R_EN_TETE = ("void L_00B00050(CPU *cpu)\n{\n"
             "    cpu->eax = 1u;\n"
             "    return;\n"
             "}\n\n")

# identifiant rl déjà présent
R_RL_DEJA = fonction("00B00060", [
    "    int rl = 0;                                               /* 00B00060: bizarre */\n",
    "    return;                                                   /* 00B00065: ret */\n",
])

# for non SSE : repli
R_POUR_NON_SSE = fonction("00B00070", [
    "    for (int i = 0; i < 4; i++) { wr32(c->eax + i, 0u); }     /* 00B00070: bizarre */\n",
    "    return;                                                   /* 00B00075: ret */\n",
])

# frontière au milieu d'une expression : le rechargement écraserait l'affectation → repli
R_FRONT_EXPR = fonction("00B00080", [
    "    c->eax = inconnue(c, 1u);                                 /* 00B00080: (forme non émise) */\n",
    "    return;                                                   /* 00B00085: ret */\n",
])

# ---- vivacité interprocédurale des drapeaux (--vivacite-drapeaux) ----
# masques de drapeaux : cf 0x0100, zf 0x0200, sf 0x0400, of 0x0800, pf 0x1000, af 0x2000 ; D6 = 0x3F00

# appelée qui ÉCRIT tous les drapeaux avant d'en lire un : E = ∅
V_TOUT_ECRIT = fonction("00A00100", [
    "    c->eax = (uint32_t)(flags_add(c, c->eax, 1u, 4));          /* 00A00100: add eax, 1 */\n",
    "    if (c->zf) goto L_00A0010A;                                /* 00A00105: jnz 0xa0010a */\n",
    "    return;                                                    /* 00A00107: ret */\n",
    "L_00A0010A:\n",
    "    return;                                                    /* 00A0010A: ret */\n",
])

# appelant type : flags_sub salit les six, push32 l'esp, appel direct, flags_add réécrit les six
V_APPELANT_REECRIT = fonction("00A00110", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00110: cmp eax, 1 */\n",
    "    push32(c, 0x00A00100u);                                    /* 00A00112: push 0xa00100 */\n",
    "    dispatch(c, 0x00A00100u);                                  /* 00A00117: call 0xa00100 */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A0011C: add esp, 4 */\n",
    "    return;                                                    /* 00A00121: ret */\n",
])

# appelée qui lit cf à l'entrée (flags_adc) : E = {cf}
V_ADC = fonction("00A00200", [
    "    c->eax = (uint32_t)(flags_adc(c, c->eax, c->edx, 4));      /* 00A00200: adc eax, edx */\n",
    "    return;                                                    /* 00A00205: ret */\n",
])

V_APPELANT_ADC = fonction("00A00210", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00210: cmp eax, 1 */\n",
    "    push32(c, 0x00A00200u);                                    /* 00A00212: push 0xa00200 */\n",
    "    dispatch(c, 0x00A00200u);                                  /* 00A00217: call 0xa00200 */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A0021C: add esp, 4 */\n",
    "    return;                                                    /* 00A00221: ret */\n",
])

# appelant qui relit cf APRÈS le retour par une aide qui lit ET écrit (flags_adc) : la lecture précède l'écriture,
# cf doit rester vivant après l'appel (régression : l'arrière retirait cf avant de l'ajouter)
V_APPELANT_ADC_APRES = fonction("00A00220", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00220: cmp eax, 1 */\n",
    "    push32(c, 0x00A00100u);                                    /* 00A00222: push 0xa00100 */\n",
    "    dispatch(c, 0x00A00100u);                                  /* 00A00227: call 0xa00100 */\n",
    "    c->eax = (uint32_t)(flags_adc(c, c->eax, c->edx, 4));      /* 00A0022C: adc eax, edx */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A0022E: add esp, 4 */\n",
    "    return;                                                    /* 00A00231: ret */\n",
])

# chaîne A→B→C où seul C lit zf à l'entrée : le point fixe propage E jusqu'à A
V_CHAINE_C = fonction("00A00300", [
    "    c->ebx = c->zf;                                            /* 00A00300: sete ebx */\n",
    "    return;                                                    /* 00A00305: ret */\n",
])
V_CHAINE_B = fonction("00A00310", [
    "    push32(c, 0x00A00300u);                                    /* 00A00310: push 0xa00300 */\n",
    "    dispatch(c, 0x00A00300u);                                  /* 00A00315: call 0xa00300 */\n",
    "    return;                                                    /* 00A0031A: ret */\n",
])
V_CHAINE_A = fonction("00A00320", [
    "    push32(c, 0x00A00310u);                                    /* 00A00320: push 0xa00310 */\n",
    "    dispatch(c, 0x00A00310u);                                  /* 00A00325: call 0xa00310 */\n",
    "    return;                                                    /* 00A0032A: ret */\n",
])
V_APPELANT_CHAINE = fonction("00A00330", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00330: cmp eax, 1 */\n",
    "    push32(c, 0x00A00320u);                                    /* 00A00332: push 0xa00320 */\n",
    "    dispatch(c, 0x00A00320u);                                  /* 00A00337: call 0xa00320 */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A0033C: add esp, 4 */\n",
    "    return;                                                    /* 00A00341: ret */\n",
])

# récursion : lit sf à l'entrée et s'appelle — E = {sf} au point fixe
V_RECURSIF = fonction("00A00400", [
    "    if (c->sf) goto L_00A00410;                                /* 00A00400: js 0xa00410 */\n",
    "    push32(c, 0x00A00400u);                                    /* 00A00402: push 0xa00400 */\n",
    "    dispatch(c, 0x00A00400u);                                  /* 00A00407: call 0xa00400 */\n",
    "    return;                                                    /* 00A0040C: ret */\n",
    "L_00A00410:\n",
    "    return;                                                    /* 00A00410: ret */\n",
])
V_APPELANT_RECURSIF = fonction("00A00420", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00420: cmp eax, 1 */\n",
    "    push32(c, 0x00A00400u);                                    /* 00A00422: push 0xa00400 */\n",
    "    dispatch(c, 0x00A00400u);                                  /* 00A00427: call 0xa00400 */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A0042C: add esp, 4 */\n",
    "    return;                                                    /* 00A00431: ret */\n",
])

# écriture conditionnelle (op_shl : compte nul n'écrase pas) : ne protège pas la lecture → E = {zf}
V_SHL = fonction("00A00500", [
    "    c->eax = (uint32_t)(op_shl(c, c->eax, c->ecx, 4));         /* 00A00500: shl eax, cl */\n",
    "    if (c->zf) goto L_00A00510;                                /* 00A00502: jz 0xa00510 */\n",
    "    return;                                                    /* 00A00504: ret */\n",
    "L_00A00510:\n",
    "    return;                                                    /* 00A00510: ret */\n",
])
V_APPELANT_SHL = fonction("00A00510", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00520: cmp eax, 1 */\n",
    "    push32(c, 0x00A00500u);                                    /* 00A00522: push 0xa00500 */\n",
    "    dispatch(c, 0x00A00500u);                                  /* 00A00527: call 0xa00500 */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A0052C: add esp, 4 */\n",
    "    return;                                                    /* 00A00531: ret */\n",
])

# appelant qui lit zf après le retour (puis réécrit les six avant chaque return) : zf doit rester vidé
V_APPELANT_LIT_ZF = fonction("00A00610", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00610: cmp eax, 1 */\n",
    "    push32(c, 0x00A00100u);                                    /* 00A00612: push 0xa00100 */\n",
    "    dispatch(c, 0x00A00100u);                                  /* 00A00617: call 0xa00100 */\n",
    "    if (c->zf) goto L_00A00622;                                /* 00A0061C: jz 0xa00622 */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A0061E: add esp, 4 */\n",
    "    return;                                                    /* 00A00623: ret */\n",
    "L_00A00622:\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A00622: add esp, 4 */\n",
    "    return;                                                    /* 00A00627: ret */\n",
])

# appel d'import (slot 0x01419999 de sc2_imports.h factice) : E = ∅, seuls les vivants après comptent
V_IMPORT_REECRIT = fonction("00A00710", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00710: cmp eax, 1 */\n",
    "    { uint32_t _ct = rd32((GVA(0x01419999))); push32(c, 0x00A00720u); dispatch(c, _ct); } /* 00A00712: call dword ptr [0x1419999] */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A00717: add esp, 4 */\n",
    "    return;                                                    /* 00A0071C: ret */\n",
])
V_IMPORT_LIT_CF = fonction("00A00730", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00730: cmp eax, 1 */\n",
    "    { uint32_t _ct = rd32((GVA(0x01419999))); push32(c, 0x00A00744u); dispatch(c, _ct); } /* 00A00732: call dword ptr [0x1419999] */\n",
    "    if (c->cf) goto L_00A0073C;                                /* 00A00737: jc 0xa0073c */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A00739: add esp, 4 */\n",
    "    return;                                                    /* 00A0073E: ret */\n",
    "L_00A0073C:\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A0073C: add esp, 4 */\n",
    "    return;                                                    /* 00A00741: ret */\n",
])
# même forme mais slot HORS IAT (0x01419998 absent de sc2_imports.h) : appel calculé, rien omis
V_IMPORT_HORS_IAT = fonction("00A00750", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00750: cmp eax, 1 */\n",
    "    { uint32_t _ct = rd32((GVA(0x01419998))); push32(c, 0x00A00760u); dispatch(c, _ct); } /* 00A00752: call dword ptr [0x1419998] */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A00757: add esp, 4 */\n",
    "    return;                                                    /* 00A0075C: ret */\n",
])

# appel calculé : jamais omis ; un appel DIRECT avant lui exclut E_indirect ({of} via V_LIT_OF)
V_LIT_OF = fonction("00A00800", [
    "    c->ecx = c->of;                                            /* 00A00800: seto ecx */\n",
    "    return;                                                    /* 00A00805: ret */\n",
])
V_CALCULE = fonction("00A00810", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00810: cmp eax, 1 */\n",
    "    dispatch(c, c->eax);                                       /* 00A00812: call eax */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A00817: add esp, 4 */\n",
    "    return;                                                    /* 00A0081C: ret */\n",
])
V_DIRECT_AVANT_CALCULE = fonction("00A00820", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00820: cmp eax, 1 */\n",
    "    dispatch(c, 0x00A00100u);                                  /* 00A00822: call 0xa00100 */\n",
    "    dispatch(c, c->eax);                                       /* 00A00827: call eax */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A0082C: add esp, 4 */\n",
    "    return;                                                    /* 00A00831: ret */\n",
])

# cible d'un SC2_SAUT : la sautante hérite de E de la cible ({pf})
V_CIBLE_PF = fonction("00A00900", [
    "    c->ebx = c->pf;                                            /* 00A00900: setp ebx */\n",
    "    return;                                                    /* 00A00905: ret */\n",
])
V_SAUTANTE = fonction("00A00910", [
    "    push32(c, 0x00A00900u);                                    /* 00A00910: push 0xa00900 */\n",
    "    SC2_SAUT(c, 0x00A00900u); return;                          /* 00A00915: jmp 0xa00900 */\n",
])
V_APPELANT_SAUT = fonction("00A00920", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00920: cmp eax, 1 */\n",
    "    push32(c, 0x00A00910u);                                    /* 00A00922: push 0xa00910 */\n",
    "    dispatch(c, 0x00A00910u);                                  /* 00A00927: call 0xa00910 */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A0092C: add esp, 4 */\n",
    "    return;                                                    /* 00A00931: ret */\n",
])

# appelée laissée intacte (repli) : E = D6, l'appelant n'omet rien
V_REPLI = fonction("00A00A00", [
    "    CPU *p = c;                                                /* 00A00A00: (forme non transformable) */\n",
    "    return;                                                    /* 00A00A05: ret */\n",
])
V_APPELANT_REPLI = fonction("00A00A10", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00A00A10: cmp eax, 1 */\n",
    "    push32(c, 0x00A00A00u);                                    /* 00A00A12: push 0xa00a00 */\n",
    "    dispatch(c, 0x00A00A00u);                                  /* 00A00A17: call 0xa00a00 */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00A00A1C: add esp, 4 */\n",
    "    return;                                                    /* 00A00A21: ret */\n",
])


# ---- pile x87 locale relative au sommet (--x87, passe 10t) ----
# masques x87 : s0 0x001, s1 0x002, s2 0x004, s3 0x008, s4 0x010, s5 0x020, s6 0x040, s7 0x080,
#               t 0x100, sw 0x200 ; PUSH : ((D & 0x07F) << 1) | 0x101 | (D & 0x200) ; POP : rotation
#               cyclique vers le bas ((D & 0x0FE) >> 1) | ((D & 0x001) << 7) | 0x100 | (D & 0x200)

# fld + fstp : PUSH salit s0|t (0x101) ; le POP fait partir s0 en s7 → return = s7|t = 0x180
X_PUSH_FSTP = fonction("00C00000", [
    "    c->ecx = (uint32_t)(rd32((c->edi + 0x00000008u)));         /* 00C00000: mov ecx, [edi + 8] */\n",
    "    fpush(c, rdf32((c->edi + c->eax*1 + 0x00000008u)));       /* 00C00005: fld dword ptr [edi + eax + 8] */\n",
    "    wrf32((c->ecx), *fst(c, 0)); fpop(c);                     /* 00C0000A: fstp dword ptr [ecx] */\n",
    "    return;                                                   /* 00C0000F: ret */\n",
])

# PUSH ×8 puis POP ×8 : 0 → 101 → 103 → 107 → 10F → 11F → 13F → 17F → 1FF (chaque poussée fait
# monter le retard d'un cran et écrase s7) ; les 8 POP tournent 0xFF sur lui-même → 0x1FF final
X_PUSH8_POP8 = fonction("00C00010", [
    "    fpush(c, rdf64((GVA(0x01422108))));                       /* 00C00010: fld qword ptr [0x1422108] */\n",
    "    fpush(c, rdf64((GVA(0x01422110))));                       /* 00C00015: fld qword ptr [0x1422110] */\n",
    "    fpush(c, rdf64((GVA(0x01422118))));                       /* 00C0001A: fld qword ptr [0x1422118] */\n",
    "    fpush(c, rdf64((GVA(0x01422120))));                       /* 00C0001F: fld qword ptr [0x1422120] */\n",
    "    fpush(c, rdf64((GVA(0x01422128))));                       /* 00C00024: fld qword ptr [0x1422128] */\n",
    "    fpush(c, rdf64((GVA(0x01422130))));                       /* 00C00029: fld qword ptr [0x1422130] */\n",
    "    fpush(c, rdf64((GVA(0x01422138))));                       /* 00C0002E: fld qword ptr [0x1422138] */\n",
    "    fpush(c, rdf64((GVA(0x01422140))));                       /* 00C00033: fld qword ptr [0x1422140] */\n",
    "    fpop(c);                                                   /* 00C00038: fstp st(0) */\n",
    "    fpop(c);                                                   /* 00C00039: fstp st(0) */\n",
    "    fpop(c);                                                   /* 00C0003A: fstp st(0) */\n",
    "    fpop(c);                                                   /* 00C0003B: fstp st(0) */\n",
    "    fpop(c);                                                   /* 00C0003C: fstp st(0) */\n",
    "    fpop(c);                                                   /* 00C0003D: fstp st(0) */\n",
    "    fpop(c);                                                   /* 00C0003E: fstp st(0) */\n",
    "    fpop(c);                                                   /* 00C0003F: fstp st(0) */\n",
    "    return;                                                    /* 00C00040: ret */\n",
])

# fld st(1) : v est lu AVANT la rotation (SC2_X87_PUSH(SC2_X87_ST(1))) ; masque = 0x101
X_FLD_ST1 = fonction("00C00050", [
    "    fpush(c, *fst(c, 1));                                      /* 00C00050: fld st(1) */\n",
    "    return;                                                    /* 00C00052: ret */\n",
])

# fxch st(1) : trois affectations, écritures de s0 et s1 → 0x003 (ni t ni sw)
X_FXCH = fonction("00C00060", [
    "    { double _t = *fst(c, 0); *fst(c, 0) = *fst(c, 1); *fst(c, 1) = _t; } /* 00C00060: fxch st(1) */\n",
    "    return;                                                    /* 00C00062: ret */\n",
])

# fucomp + fnstsw : fcom salit sw (0x200), fnstsw est une lecture ; eax écrit côté rl
X_FCOM_FNSTSW = fonction("00C00070", [
    "    fcompare(c, *fst(c, 0), *fst(c, 2));                       /* 00C00070: fucomp st(2) */\n",
    "    SET16(c->eax, (uint16_t)fpu_fnstsw(c));                    /* 00C00072: fnstsw ax */\n",
    "    return;                                                    /* 00C00074: ret */\n",
])

# frontière au milieu d'une séquence x87 : PUSH (0x101) vidé avant l'appel, rechargé après,
# puis fstp+POP (0x180) au return
X_FRONTIERE = fonction("00C00080", [
    "    fpush(c, rdf64((GVA(0x01422108))));                       /* 00C00080: fld qword ptr [0x1422108] */\n",
    "    dispatch(c, 0x00A00040u);                                  /* 00C00085: call 0xa00040 */\n",
    "    wrf32((c->ecx), *fst(c, 0)); fpop(c);                     /* 00C0008A: fstp dword ptr [ecx] */\n",
    "    return;                                                    /* 00C0008F: ret */\n",
])

# return avec x87 sale : la pile reste à 0x101 au retour
X_RETURN_SALE = fonction("00C00090", [
    "    fpush(c, 1.0);                                             /* 00C00090: fld1 */\n",
    "    return;                                                    /* 00C00092: ret */\n",
])

# boucle : l'arc arrière unionne à l'étiquette l'état d'entrée et l'état après un tour ; le
# point fixe de PUSH grimpe d'un cran par itération (0x101, 0x103, 0x107, … 0x1FF) : après
# n'importe quel nombre de tours, tout slot peut être sale → 0x1FF au return (sans sw : pas de fcom)
X_BOUCLE = fonction("00C000A0", [
    "    c->ecx = 4u;                                               /* 00C000A0: mov ecx, 4 */\n",
    "L_00C000A5:\n",
    "    fpush(c, 1.0);                                             /* 00C000A5: fld1 */\n",
    "    c->ecx = c->ecx - 1u;                                      /* 00C000A7: dec ecx */\n",
    "    if (c->ecx) goto L_00C000A5;                               /* 00C000A9: jne 0xc000a5 */\n",
    "    return;                                                    /* 00C000AB: ret */\n",
])

# faddp : écriture de s1 (0x002) PUIS pop ((0x002 >> 1) = 0x001, s0 propre repart en s7 mais
# est vide) → 0x101 : l'ordre écriture-puis-rotation place bien le retard sur le nouveau sommet
X_FADDP = fonction("00C000B0", [
    "    *fst(c, 1) = *fst(c, 1) + *fst(c, 0); fpop(c);             /* 00C000B0: faddp st(1), st(0) */\n",
    "    return;                                                    /* 00C000B2: ret */\n",
])

# while avec x87 : 0 à n rotations possibles → conservateur, les 8 slots et t (0x1FF)
X_WHILE = fonction("00C000C0", [
    "    while (c->ecx) { fpush(c, 1.0); c->ecx--; }                /* 00C000C0: boucle bizarre */\n",
    "    return;                                                    /* 00C000C2: ret */\n",
])

# repli x87 : fpu_fninit touche fpu_sw et fpu_top sans être réécrite → la passe 10s
# s'applique, les formes x87 restent sur c
X_REPLI_FNINIT = fonction("00C000D0", [
    "    fpush(c, 1.0);                                             /* 00C000D0: fld1 */\n",
    "    fpu_fninit(c);                                             /* 00C000D2: fninit */\n",
    "    return;                                                    /* 00C000D4: ret */\n",
])

# repli x87 : fst non déréférencé (l'adresse s'échapperait de la pile locale)
X_REPLI_FST_ADRESSE = fonction("00C000E0", [
    "    double *p = fst(c, 0);                                     /* 00C000E0: (forme non émise) */\n",
    "    return;                                                    /* 00C000E2: ret */\n",
])

# repli x87 : indice de fst non littéral
X_REPLI_FST_INDICE = fonction("00C000F0", [
    "    wrf32((c->ecx), *fst(c, c->eax));                          /* 00C000F0: (forme non émise) */\n",
    "    return;                                                    /* 00C000F2: ret */\n",
])

# vivacité + x87 : l'appel direct combine SC2_RL_VIDEX (drapeaux omis) et SC2_X87_VIDE (0x101)
V_X87_APPELANT = fonction("00C00110", [
    "    flags_sub(c, c->eax, 1u, 4);                               /* 00C00110: cmp eax, 1 */\n",
    "    push32(c, 0x00A00100u);                                    /* 00C00112: push 0xa00100 */\n",
    "    fpush(c, rdf64((GVA(0x01422108))));                       /* 00C00117: fld qword ptr [0x1422108] */\n",
    "    dispatch(c, 0x00A00100u);                                  /* 00C0011C: call 0xa00100 */\n",
    "    c->esp = (uint32_t)(flags_add(c, c->esp, 0x4u, 4));        /* 00C00121: add esp, 4 */\n",
    "    fpop(c);                                                   /* 00C00126: fstp st(0) */\n",
    "    return;                                                    /* 00C00128: ret */\n",
])


class TestRegistresLocaux(unittest.TestCase):
    def setUp(self):
        self.racine = Path(tempfile.mkdtemp(prefix="registres-locaux-test-"))
        self.addCleanup(shutil.rmtree, self.racine, True)
        self.entree = self.racine / "entree"
        self.entree.mkdir()
        (self.entree / "README.md").write_text("notes de test : é\n", encoding="utf-8")
        # slots d'IAT factices (le lift réel contient sc2_imports.h, lu avec --vivacite-drapeaux)
        (self.entree / "sc2_imports.h").write_text(
            '/* AUTO-GENERATED (test) */\n#define SC2_IMPORTS(X) \\\n'
            '    X(0x01419999u, "TEST.dll", "Fonction") \\\n', encoding="utf-8")
        self.sortie = self.racine / "sortie"

    def lancer(self, *args):
        return subprocess.run([sys.executable, str(SCRIPT), *map(str, args)],
                              capture_output=True, text=True)

    def passage(self, entree=None, sortie=None):
        resultat = self.lancer(entree or self.entree, sortie or self.sortie, "--cpu-h", str(CPU_H))
        self.assertEqual(resultat.returncode, 0, resultat.stderr)
        return resultat

    def passage_vivacite(self, entree=None, sortie=None, extra=()):
        resultat = self.lancer(entree or self.entree, sortie or self.sortie, "--cpu-h", str(CPU_H),
                               "--vivacite-drapeaux", *extra)
        self.assertEqual(resultat.returncode, 0, resultat.stderr)
        return resultat

    def transformer(self, fonction_c):
        """Applique la passe à un fichier réduit à une fonction ; renvoie son texte transformé."""
        (self.entree / "recomp_00.c").write_text(ENTETE + fonction_c, encoding="utf-8")
        self.passage()
        return (self.sortie / "recomp_00.c").read_text(encoding="utf-8")

    def transformer_vivacite(self, *fonctions_c, extra=()):
        """Passe avec --vivacite-drapeaux sur un fichier réduit aux fonctions données."""
        (self.entree / "recomp_00.c").write_text(ENTETE + "".join(fonctions_c), encoding="utf-8")
        self.passage_vivacite(extra=extra)
        return (self.sortie / "recomp_00.c").read_text(encoding="utf-8")

    def passage_x87(self, entree=None, sortie=None, extra=()):
        resultat = self.lancer(entree or self.entree, sortie or self.sortie, "--cpu-h", str(CPU_H),
                               "--x87", *extra)
        self.assertEqual(resultat.returncode, 0, resultat.stderr)
        return resultat

    def transformer_x87(self, *fonctions_c, extra=()):
        """Passe avec --x87 sur un fichier réduit aux fonctions données."""
        (self.entree / "recomp_00.c").write_text(ENTETE + "".join(fonctions_c), encoding="utf-8")
        self.passage_x87(extra=extra)
        return (self.sortie / "recomp_00.c").read_text(encoding="utf-8")

    def repli_x87(self, fonction_c, motif):
        """Repli x87 seul : passe 10s appliquée, formes x87 intactes sur c, motif compté."""
        (self.entree / "recomp_00.c").write_text(ENTETE + fonction_c, encoding="utf-8")
        resultat = self.passage_x87()
        sortie = (self.sortie / "recomp_00.c").read_text(encoding="utf-8")
        self.assertIn("SC2_RL_DECL(c);\n", sortie)      # la passe 10s s'applique bien
        self.assertNotIn("SC2_X87_", sortie)
        self.assertIn(motif, resultat.stdout)
        return sortie

    def repli(self, fonction_c, motif):
        """La fonction doit ressortir octet pour octet identique, comptée sous ce motif."""
        entree = ENTETE + fonction_c
        (self.entree / "recomp_00.c").write_text(entree, encoding="utf-8")
        resultat = self.passage()
        self.assertEqual((self.sortie / "recomp_00.c").read_text(encoding="utf-8"), entree)
        self.assertIn(motif, resultat.stdout)
        return resultat

    # ---- transformations ----

    def test_simple(self):
        """Deux écritures, un return : DECL, rl->, masque eax|ebx au return, vidage final 0."""
        sortie = self.transformer(F_SIMPLE)
        self.assertIn("    SC2_RL_DECL(c);\n", sortie)
        self.assertIn("rl->eax = 0x1u;", sortie)
        self.assertIn("rl->ebx = rl->eax;", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0009u); return; }", sortie)   # eax|ebx
        self.assertIn("    SC2_RL_VIDE(c, 0x0000u);\n}", sortie)

    def test_push_puis_appel(self):
        """push32 écrit esp : frontière d'appel avec masque esp, push32 reçoit rl."""
        sortie = self.transformer(F_PUSH_APPEL)
        self.assertIn("push32(rl, 0x00A00030u);", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0010u); dispatch(c, 0x00A00040u); SC2_RL_CHARGE(c); }", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); return; }", sortie)

    def test_deux_appels_successifs(self):
        """Deux appels de suite : le second repart d'un masque vide (rechargé au premier)."""
        sortie = self.transformer(F_DEUX_APPELS)
        self.assertEqual(sortie.count("SC2_RL_CHARGE(c);"), 2)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0001u); dispatch(c, 0x00A00040u); SC2_RL_CHARGE(c); }", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); dispatch(c, 0x00A00050u); SC2_RL_CHARGE(c); }", sortie)

    def test_fusion_etiquette(self):
        """Deux chemins d'écritures disjointes : union au label, puis esi écrit au return."""
        sortie = self.transformer(F_FUSION)
        # union ecx|edx au label, puis esi : return = ecx|edx|esi = 0x0046
        self.assertIn("{ SC2_RL_VIDE(c, 0x0046u); return; }", sortie)

    def test_boucle_point_fixe(self):
        """Arc arrière : eax, ecx et les drapeaux de flags_sub sont sales à la sortie de boucle."""
        sortie = self.transformer(F_BOUCLE)
        # eax|ecx|cf|zf|sf|of|pf|af = 0x3F03
        self.assertIn("{ SC2_RL_VIDE(c, 0x3F03u); return; }", sortie)

    def test_jump_table(self):
        """Bloc multi-lignes _jt : si_goto reconnus, dispatch_jmp enveloppé dans son bloc."""
        sortie = self.transformer(F_JUMP_TABLE)
        self.assertIn("if (_jt == GVA(0x00A00070)) goto L_00A00070;", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); dispatch_jmp(c, _jt); SC2_RL_CHARGE(c); } "
                      "{ SC2_RL_VIDE(c, 0x0000u); return; } }", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0004u); return; }", sortie)   # edx au return du label

    def test_si_saut_terminal(self):
        """if (…) { SC2_SAUT ; return; } : bloc terminal, frontière enveloppée DANS le bloc."""
        sortie = self.transformer(F_SI_SAUT)
        # eax|cf|zf|sf|of|pf|af = 0x3F01 (flags_add écrit les six drapeaux) ; la chute garde son masque
        self.assertIn("if (rl->zf) { { SC2_RL_VIDE(c, 0x3F01u); SC2_SAUT(c, 0x00A00090u); SC2_RL_CHARGE(c); } "
                      "{ SC2_RL_VIDE(c, 0x0000u); return; } }", sortie)
        self.assertIn("    { SC2_RL_VIDE(c, 0x3F01u); return; }", sortie)

    def test_rep_movs_while(self):
        """rep movsd en while : écritures du corps fondues dans la chute (sur-approximation)."""
        sortie = self.transformer(F_REP_MOVS)
        self.assertIn("while (rl->ecx) {", sortie)
        # ecx|esi|edi = 0x00C2
        self.assertIn("{ SC2_RL_VIDE(c, 0x00C2u); return; }", sortie)

    def test_cmpxchg8b_conditionnel(self):
        """cmpxchg8b : zf écrit, eax/edx conditionnels — tous dans le masque final."""
        sortie = self.transformer(F_CMPXCHG8B)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0205u); return; }", sortie)   # eax|edx|zf
        self.assertIn("rl->zf = (_old == _e);", sortie)
        self.assertIn("if (!rl->zf) { rl->eax = (uint32_t)_old; rl->edx = (uint32_t)(_old >> 32); }", sortie)

    def test_runtime_recomp_todo_setjmp3(self):
        """RECOMP_TODO (frontière sans c) et SC2_SETJMP3 : enveloppés comme frontières runtime."""
        sortie = self.transformer(F_RUNTIME)
        self.assertIn('{ SC2_RL_VIDE(c, 0x0008u); RECOMP_TODO(0x00A000B5, "rdtsc"); SC2_RL_CHARGE(c); }', sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); SC2_SETJMP3(c, 0x00A000C0u); SC2_RL_CHARGE(c); }", sortie)

    def test_appels_directs(self):
        """SC2_APPEL et L_X(c) : frontières d'appel ; masque du second vide (rechargé)."""
        sortie = self.transformer(F_APPELS_DIRECTS)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0040u); SC2_APPEL(c, L_00A000D0, 0x00A000D0u); SC2_RL_CHARGE(c); }", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); L_00A000D0(c); SC2_RL_CHARGE(c); }", sortie)

    def test_hors_cache_garde_c(self):
        """fst (hors cache) garde c, n'est pas une frontière ; edi sale au return."""
        sortie = self.transformer(F_HORS_CACHE)
        self.assertIn("fst(c, 0);", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0080u); return; }", sortie)

    def test_set8l(self):
        """SET8L(c->eax, …) : écriture de eax reconnue (masque 0x0001 au return)."""
        sortie = self.transformer(F_SET8L)
        self.assertIn("SET8L(rl->eax, rl->ebx & 0xFFu);", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0001u); return; }", sortie)

    def test_pour_sse(self):
        """for (int _i …) sur xmm : admis, c->xmm inchangé (pas un champ en cache)."""
        sortie = self.transformer(F_POUR_SSE)
        self.assertIn("for (int _i = 0; _i < 4; _i++) { c->xmm[_i] = c->xmm[_i]; }", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); return; }", sortie)

    # ---- replis : octet pour octet ----

    def test_repli_frontiere_conditionnelle(self):
        self.repli(R_FRONT_COND, "frontière conditionnelle")

    def test_repli_frontiere_dans_while(self):
        self.repli(R_FRONT_WHILE, "frontière dans un while")

    def test_repli_c_autrement(self):
        self.repli(R_C_AUTREMENT, "c autrement")

    def test_repli_goto_absent(self):
        self.repli(R_GOTO_ABSENT, "goto absent")

    def test_repli_switch(self):
        self.repli(R_SWITCH, "switch/for/do")

    def test_repli_en_tete_differente(self):
        self.repli(R_EN_TETE, "en-tête différente")

    def test_repli_rl_deja_present(self):
        self.repli(R_RL_DEJA, "rl déjà présent")

    def test_repli_pour_non_sse(self):
        self.repli(R_POUR_NON_SSE, "switch/for/do")

    def test_repli_frontiere_dans_expression(self):
        self.repli(R_FRONT_EXPR, "frontière dans une expression")

    def test_aucune_transformable(self):
        """Un fichier sans fonction transformable : recopié tel quel, compté intact."""
        entree = ENTETE + R_SWITCH + R_C_AUTREMENT
        (self.entree / "recomp_00.c").write_text(entree, encoding="utf-8")
        resultat = self.passage()
        self.assertRegex(resultat.stdout, r"fonctions transformées : 0 sur 2")
        self.assertEqual((self.sortie / "recomp_00.c").read_text(encoding="utf-8"), entree)

    # ---- harnais ----

    def test_fichier_non_recomp_recopie(self):
        (self.entree / "recomp_00.c").write_text(ENTETE + F_SIMPLE, encoding="utf-8")
        self.passage()
        self.assertEqual((self.sortie / "README.md").read_bytes(),
                         (self.entree / "README.md").read_bytes())

    def test_idempotence_sans_reecriture(self):
        """Relancer sur la sortie ne change rien et ne réécrit aucun fichier."""
        (self.entree / "recomp_00.c").write_text(ENTETE + F_SIMPLE, encoding="utf-8")
        self.passage()
        avant = {p.name: p.stat().st_mtime_ns for p in self.sortie.iterdir()}
        contenus = {p.name: p.read_bytes() for p in self.sortie.iterdir()}
        resultat = self.passage(entree=self.sortie, sortie=self.sortie)
        self.assertEqual({p.name: p.read_bytes() for p in self.sortie.iterdir()}, contenus)
        self.assertEqual({p.name: p.stat().st_mtime_ns for p in self.sortie.iterdir()}, avant)
        self.assertRegex(resultat.stdout, r"0 réécrit")

    def test_determinisme(self):
        """Deux exécutions indépendantes donnent des sorties identiques octet pour octet."""
        (self.entree / "recomp_00.c").write_text(ENTETE + F_BOUCLE, encoding="utf-8")
        s1, s2 = self.racine / "s1", self.racine / "s2"
        self.passage(sortie=s1)
        self.passage(sortie=s2)
        self.assertEqual((s1 / "recomp_00.c").read_bytes(), (s2 / "recomp_00.c").read_bytes())

    def test_verifier_conforme(self):
        (self.entree / "recomp_00.c").write_text(
            ENTETE + F_SIMPLE + F_BOUCLE + F_JUMP_TABLE + F_CMPXCHG8B, encoding="utf-8")
        self.passage()
        resultat = self.lancer(self.entree, self.sortie, "--cpu-h", str(CPU_H), "--verifier")
        self.assertEqual(resultat.returncode, 0, resultat.stderr)
        self.assertIn("conformes", resultat.stdout)

    def test_verifier_refuse_masque_modifie(self):
        (self.entree / "recomp_00.c").write_text(ENTETE + F_SIMPLE, encoding="utf-8")
        self.passage()
        cible = self.sortie / "recomp_00.c"
        cible.write_text(cible.read_text(encoding="utf-8").replace(
            "SC2_RL_VIDE(c, 0x0009u)", "SC2_RL_VIDE(c, 0x0001u)"), encoding="utf-8")
        resultat = self.lancer(self.entree, self.sortie, "--cpu-h", str(CPU_H), "--verifier")
        self.assertNotEqual(resultat.returncode, 0)
        self.assertIn("écart", resultat.stderr)

    def test_commentaire_en_tete_de_ligne(self):
        """Commentaire avant le code : frontière et return emballés, c->eax réécrit, commentaire de tête intact."""
        sortie = self.transformer(F_COMMENTAIRE_TETE)
        self.assertIn("    /* indirect jmp */ { SC2_RL_VIDE(c, 0x0001u); dispatch_jmp(c, rl->eax); SC2_RL_CHARGE(c); } "
                      "{ SC2_RL_VIDE(c, 0x0000u); return; }       /* 00A000B5: jmp eax */", sortie)
        self.assertNotIn("c->eax", sortie.replace("(c)->eax", ""))

    def test_verifier_refuse_frontiere_non_emballee(self):
        """Une frontière laissée nue (bug des commentaires de tête) : le vérificateur la signale."""
        (self.entree / "recomp_00.c").write_text(ENTETE + F_COMMENTAIRE_TETE_GVA, encoding="utf-8")
        self.passage()
        cible = self.sortie / "recomp_00.c"
        texte = cible.read_text(encoding="utf-8")
        emballee = ("    /* indirect jmp */ { SC2_RL_VIDE(c, 0x0001u); dispatch_jmp(c, rd32((GVA(0x01419308)))); "
                    "SC2_RL_CHARGE(c); } { SC2_RL_VIDE(c, 0x0000u); return; }")
        self.assertIn(emballee, texte)
        cible.write_text(texte.replace(emballee, "    /* indirect jmp */ dispatch_jmp(c, rd32((GVA(0x01419308)))); "
                                       "return;"), encoding="utf-8")
        resultat = self.lancer(self.entree, self.sortie, "--cpu-h", str(CPU_H), "--verifier")
        self.assertNotEqual(resultat.returncode, 0)
        self.assertIn("non emballé", resultat.stderr)

    def test_attente_active_barriere(self):
        """Tête de boucle (goto arrière) : SC2_RL_BOUCLE() juste après l'étiquette ; cible d'un goto avant : rien."""
        sortie = self.transformer(F_ATTENTE)
        self.assertIn("L_00A000D6:\n    SC2_RL_BOUCLE();\n    flags_sub(rl, rd8((rl->edi + 0x0000027Du)), 0x0u, 1);", sortie)
        self.assertIn("L_00A000E0:\n    { SC2_RL_VIDE(c, 0x3F00u); return; }", sortie)
        self.assertEqual(sortie.count("SC2_RL_BOUCLE();"), 1)

    def test_verifier_refuse_barriere_retiree(self):
        (self.entree / "recomp_00.c").write_text(ENTETE + F_ATTENTE, encoding="utf-8")
        self.passage()
        cible = self.sortie / "recomp_00.c"
        cible.write_text(cible.read_text(encoding="utf-8").replace("    SC2_RL_BOUCLE();\n", ""), encoding="utf-8")
        resultat = self.lancer(self.entree, self.sortie, "--cpu-h", str(CPU_H), "--verifier")
        self.assertNotEqual(resultat.returncode, 0)
        self.assertIn("sans SC2_RL_BOUCLE", resultat.stderr)

    def test_verifier_refuse_c_eax_reintroduit(self):
        (self.entree / "recomp_00.c").write_text(ENTETE + F_SIMPLE, encoding="utf-8")
        self.passage()
        cible = self.sortie / "recomp_00.c"
        cible.write_text(cible.read_text(encoding="utf-8").replace(
            "rl->eax = 0x1u;", "c->eax = 0x1u;"), encoding="utf-8")
        resultat = self.lancer(self.entree, self.sortie, "--cpu-h", str(CPU_H), "--verifier")
        self.assertNotEqual(resultat.returncode, 0)
        self.assertIn("écart", resultat.stderr)

    def test_verifier_refuse_recopie_alteree(self):
        (self.entree / "recomp_00.c").write_text(ENTETE + F_SIMPLE, encoding="utf-8")
        self.passage()
        with open(self.sortie / "README.md", "a", encoding="utf-8") as f:
            f.write("altéré\n")
        resultat = self.lancer(self.entree, self.sortie, "--cpu-h", str(CPU_H), "--verifier")
        self.assertNotEqual(resultat.returncode, 0)
        self.assertIn("écart", resultat.stderr)

    def test_cpu_h_classification(self):
        """Sur le vrai cpu.h : push32 en cache {esp}, flags_logicz {cf of sf zf pf}, fst hors cache, aucune mixte."""
        processus = subprocess.run(
            [sys.executable, "-c",
             "import importlib.util\n"
             f"spec = importlib.util.spec_from_file_location('rl', {str(SCRIPT)!r})\n"
             "m = importlib.util.module_from_spec(spec)\n"
             "spec.loader.exec_module(m)\n"
             f"cl, ec = m.classer_aides(m.lire_texte({str(CPU_H)!r}))\n"
             "print(cl['push32'], hex(ec['push32']), cl['flags_logicz'], hex(ec['flags_logicz']),"
             " cl['fst'], sum(1 for v in cl.values() if v == 'mixte'))"],
            capture_output=True, text=True)
        self.assertEqual(processus.returncode, 0, processus.stderr)
        self.assertEqual(processus.stdout.strip(), "cache 0x10 cache 0x1f00 hors 0")

    def test_compilation_cc(self):
        """Un fixture transformé compile avec cc -fsyntax-only (sc2_lifted.h inclus de force)."""
        if shutil.which("cc") is None:
            self.skipTest("cc indisponible")
        (self.entree / "recomp_00.c").write_text(
            ENTETE + F_SIMPLE + F_BOUCLE + F_PUSH_APPEL + F_JUMP_TABLE + F_CMPXCHG8B + F_ATTENTE
            + F_COMMENTAIRE_TETE, encoding="utf-8")
        self.passage()
        for options in ([], ["-DSC2_ACCES_VOLATILS"], ["-DSC2_ACCES_VOLATILS", "-DSC2_RL_CONTROLE"]):
            resultat = subprocess.run(["cc", "-fsyntax-only", "-Wno-parentheses-equality", *options,
                                       "-include", str(CPU_H.parent / "sc2_lifted.h"),
                                       "-I", str(CPU_H.parent),
                                       str(self.sortie / "recomp_00.c")],
                                      capture_output=True, text=True)
            self.assertEqual(resultat.returncode, 0, f"{options} : {resultat.stderr}")

    # ---- vivacité interprocédurale des drapeaux (--vivacite-drapeaux) ----

    def test_vivacite_e_vide_tout_omis(self):
        """Appelée qui écrit tout avant de lire (E = ∅), appelant qui réécrit : les six omis."""
        sortie = self.transformer_vivacite(V_TOUT_ECRIT, V_APPELANT_REECRIT)
        self.assertIn("{ SC2_RL_VIDEX(c, 0x0010u, 0x3F00u); dispatch(c, 0x00A00100u); "
                      "SC2_RL_CHARGE(c); }", sortie)          # esp seul vidé, six drapeaux omis

    def test_vivacite_callee_lit_cf(self):
        """L'appelée lit cf à l'entrée (flags_adc) : cf vidé, les cinq autres omis."""
        sortie = self.transformer_vivacite(V_ADC, V_APPELANT_ADC)
        self.assertIn("{ SC2_RL_VIDEX(c, 0x0110u, 0x3E00u); dispatch(c, 0x00A00200u); "
                      "SC2_RL_CHARGE(c); }", sortie)          # m = esp|cf, x = D6 ∖ cf

    def test_vivacite_adc_apres_retour(self):
        """flags_adc après l'appel lit cf avant d'écrire les six : cf vidé à l'appel, les cinq autres omis."""
        sortie = self.transformer_vivacite(V_TOUT_ECRIT, V_APPELANT_ADC_APRES)
        self.assertIn("{ SC2_RL_VIDEX(c, 0x0110u, 0x3E00u); dispatch(c, 0x00A00100u); "
                      "SC2_RL_CHARGE(c); }", sortie)          # m = esp|cf, x = D6 ∖ cf

    def test_vivacite_chaine_point_fixe(self):
        """Chaîne A→B→C où seul C lit zf : E se propage, l'appel à A épargne zf."""
        chemin = self.racine / "viv.txt"
        sortie = self.transformer_vivacite(V_CHAINE_C, V_CHAINE_B, V_CHAINE_A, V_APPELANT_CHAINE,
                                           extra=("--dump-vivacite", chemin))
        self.assertIn("{ SC2_RL_VIDEX(c, 0x0210u, 0x3D00u); dispatch(c, 0x00A00320u); "
                      "SC2_RL_CHARGE(c); }", sortie)          # m = esp|zf, x = D6 ∖ {zf}
        self.assertEqual(chemin.read_text(encoding="utf-8"),
                         "L_00A00300 0x0200\nL_00A00310 0x0200\nL_00A00320 0x0200\n"
                         "L_00A00330 0x0000\n")

    def test_vivacite_recursivite(self):
        """Récursion : E({sf}) stable au point fixe, sf vidé à l'appel."""
        sortie = self.transformer_vivacite(V_RECURSIF, V_APPELANT_RECURSIF)
        self.assertIn("{ SC2_RL_VIDEX(c, 0x0410u, 0x3B00u); dispatch(c, 0x00A00400u); "
                      "SC2_RL_CHARGE(c); }", sortie)          # m = esp|sf, x = D6 ∖ {sf}

    def test_vivacite_ecriture_conditionnelle(self):
        """op_shl (écriture POSSIBLE, compte nul) ne protège pas la lecture : E = {zf}."""
        sortie = self.transformer_vivacite(V_SHL, V_APPELANT_SHL)
        self.assertIn("{ SC2_RL_VIDEX(c, 0x0210u, 0x3D00u); dispatch(c, 0x00A00500u); "
                      "SC2_RL_CHARGE(c); }", sortie)          # zf vidé, cinq omis

    def test_vivacite_appelant_lit_zf(self):
        """L'appelant lit zf après le retour (if (c->zf) goto) : zf vidé, les autres omis."""
        sortie = self.transformer_vivacite(V_TOUT_ECRIT, V_APPELANT_LIT_ZF)
        self.assertIn("{ SC2_RL_VIDEX(c, 0x0210u, 0x3D00u); dispatch(c, 0x00A00100u); "
                      "SC2_RL_CHARGE(c); }", sortie)

    def test_vivacite_import(self):
        """Appel d'import (slot d'IAT) : E = ∅ ; seuls les drapeaux vivants après comptent."""
        sortie = self.transformer_vivacite(V_IMPORT_REECRIT, V_IMPORT_LIT_CF)
        self.assertIn("push32(rl, 0x00A00720u); { SC2_RL_VIDEX(c, 0x0010u, 0x3F00u); "
                      "dispatch(c, _ct); SC2_RL_CHARGE(c); }", sortie)   # tout omis
        self.assertIn("push32(rl, 0x00A00744u); { SC2_RL_VIDEX(c, 0x0110u, 0x3E00u); "
                      "dispatch(c, _ct); SC2_RL_CHARGE(c); }", sortie)   # cf lu après : vidé

    def test_vivacite_import_hors_iat(self):
        """Même forme mais slot absent de sc2_imports.h : appel calculé, masque complet."""
        sortie = self.transformer_vivacite(V_IMPORT_HORS_IAT)
        self.assertIn("push32(rl, 0x00A00760u); { SC2_RL_VIDE(c, 0x3F10u); "
                      "dispatch(c, _ct); SC2_RL_CHARGE(c); }", sortie)
        self.assertNotIn("SC2_RL_VIDEX", sortie)

    def test_vivacite_appel_calcule(self):
        """Appel calculé : rien omis ; un appel direct avant lui exclut E_indirect ({of})."""
        sortie = self.transformer_vivacite(V_LIT_OF, V_CALCULE, V_DIRECT_AVANT_CALCULE, V_TOUT_ECRIT)
        self.assertIn("{ SC2_RL_VIDE(c, 0x3F00u); dispatch(c, rl->eax); SC2_RL_CHARGE(c); }",
                      sortie)                                 # calculé : masque complet
        self.assertIn("{ SC2_RL_VIDEX(c, 0x0800u, 0x3700u); dispatch(c, 0x00A00100u); "
                      "SC2_RL_CHARGE(c); }", sortie)          # direct : of (E_indirect) vidé

    def test_vivacite_saut_e_cible(self):
        """SC2_SAUT vers une liftée : la sautante hérite de E de la cible ({pf})."""
        sortie = self.transformer_vivacite(V_CIBLE_PF, V_SAUTANTE, V_APPELANT_SAUT)
        self.assertIn("{ SC2_RL_VIDEX(c, 0x1010u, 0x2F00u); dispatch(c, 0x00A00910u); "
                      "SC2_RL_CHARGE(c); }", sortie)          # m = esp|pf, x = D6 ∖ {pf}

    def test_vivacite_repli_conservateur(self):
        """Appelée laissée intacte (E = D6) : l'appelant n'omet aucun drapeau."""
        sortie = self.transformer_vivacite(V_REPLI, V_APPELANT_REPLI)
        self.assertIn("{ SC2_RL_VIDE(c, 0x3F10u); dispatch(c, 0x00A00A00u); SC2_RL_CHARGE(c); }",
                      sortie)
        self.assertNotIn("SC2_RL_VIDEX", sortie)

    def test_vivacite_sans_option_identique(self):
        """Sans l'option : masque complet aux appels, aucune macro SC2_RL_VIDEX émise."""
        (self.entree / "recomp_00.c").write_text(ENTETE + V_TOUT_ECRIT + V_APPELANT_REECRIT,
                                                 encoding="utf-8")
        self.passage()
        sortie = (self.sortie / "recomp_00.c").read_text(encoding="utf-8")
        self.assertIn("{ SC2_RL_VIDE(c, 0x3F10u); dispatch(c, 0x00A00100u); SC2_RL_CHARGE(c); }",
                      sortie)
        self.assertNotIn("SC2_RL_VIDEX", sortie)

    def test_vivacite_stats_et_dump(self):
        """Ligne de statistiques de vivacité et dump trié de E par fonction."""
        chemin = self.racine / "viv.txt"
        (self.entree / "recomp_00.c").write_text(ENTETE + V_TOUT_ECRIT + V_APPELANT_REECRIT,
                                                 encoding="utf-8")
        resultat = self.passage_vivacite(extra=("--dump-vivacite", chemin))
        self.assertRegex(resultat.stdout,
                         r"vivacité des drapeaux : E vide pour 2 fonctions sur 2, "
                         r"E_indirect = 0x0000, drapeaux omis aux appels : 6 "
                         r"\(moyenne 6\.00 par appel direct ou d'import\)")
        self.assertEqual(chemin.read_text(encoding="utf-8"),
                         "L_00A00100 0x0000\nL_00A00110 0x0000\n")

    def test_vivacite_verifier_conforme(self):
        """--verifier avec l'option (aux deux appels) : retrait octet pour octet, x recalculés."""
        (self.entree / "recomp_00.c").write_text(
            ENTETE + V_TOUT_ECRIT + V_APPELANT_ADC + V_ADC + V_IMPORT_REECRIT + V_SAUTANTE
            + V_CIBLE_PF + V_DIRECT_AVANT_CALCULE + V_LIT_OF + V_CALCULE, encoding="utf-8")
        self.passage_vivacite()
        resultat = self.lancer(self.entree, self.sortie, "--cpu-h", str(CPU_H),
                               "--vivacite-drapeaux", "--verifier")
        self.assertEqual(resultat.returncode, 0, resultat.stderr)
        self.assertIn("conformes", resultat.stdout)

    def test_vivacite_verifier_refuse_x_altere(self):
        """Un x de SC2_RL_VIDEX altéré : le vérificateur signale l'écart."""
        (self.entree / "recomp_00.c").write_text(ENTETE + V_TOUT_ECRIT + V_APPELANT_REECRIT,
                                                 encoding="utf-8")
        self.passage_vivacite()
        cible = self.sortie / "recomp_00.c"
        texte = cible.read_text(encoding="utf-8")
        self.assertIn("SC2_RL_VIDEX(c, 0x0010u, 0x3F00u)", texte)
        cible.write_text(texte.replace("SC2_RL_VIDEX(c, 0x0010u, 0x3F00u)",
                                       "SC2_RL_VIDEX(c, 0x0010u, 0x3E00u)"), encoding="utf-8")
        resultat = self.lancer(self.entree, self.sortie, "--cpu-h", str(CPU_H),
                               "--vivacite-drapeaux", "--verifier")
        self.assertNotEqual(resultat.returncode, 0)
        self.assertIn("écart", resultat.stderr)

    def test_vivacite_compilation_cc(self):
        """Une sortie avec SC2_RL_VIDEX compile avec cc, avec et sans -DSC2_RL_CONTROLE."""
        if shutil.which("cc") is None:
            self.skipTest("cc indisponible")
        (self.entree / "recomp_00.c").write_text(ENTETE + V_TOUT_ECRIT + V_APPELANT_REECRIT
                                                 + V_IMPORT_REECRIT, encoding="utf-8")
        self.passage_vivacite()
        for options in ([], ["-DSC2_ACCES_VOLATILS"], ["-DSC2_ACCES_VOLATILS", "-DSC2_RL_CONTROLE"]):
            resultat = subprocess.run(["cc", "-fsyntax-only", "-Wno-parentheses-equality", *options,
                                       "-include", str(CPU_H.parent / "sc2_lifted.h"),
                                       "-I", str(CPU_H.parent),
                                       str(self.sortie / "recomp_00.c")],
                                      capture_output=True, text=True)
            self.assertEqual(resultat.returncode, 0, f"{options} : {resultat.stderr}")

    # ---- pile x87 locale relative au sommet (--x87, passe 10t) ----

    def test_x87_push_fstp(self):
        """fld puis fstp : PUSH, ST(0) lu, POP ; return = s7|t = 0x180, DECL sous celui de rl."""
        sortie = self.transformer_x87(X_PUSH_FSTP)
        self.assertIn("    SC2_RL_DECL(c);\n    SC2_X87_DECL(c);\n", sortie)
        self.assertIn("SC2_X87_PUSH(rdf32((rl->edi + rl->eax*1 + 0x00000008u)));", sortie)
        self.assertIn("wrf32((rl->ecx), SC2_X87_ST(0)); SC2_X87_POP();", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0002u); SC2_X87_VIDE(c, 0x180u); return; }", sortie)
        self.assertIn("    SC2_RL_VIDE(c, 0x0000u);\n    SC2_X87_VIDE(c, 0x000u);\n}", sortie)

    def test_x87_push_huit_pop_huit(self):
        """PUSH ×8 salit tout (0x1FF) ; POP ×8 tourne 0xFF sur lui-même : return = 0x1FF."""
        sortie = self.transformer_x87(X_PUSH8_POP8)
        self.assertEqual(sortie.count("fpop(c"), 0)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); SC2_X87_VIDE(c, 0x1FFu); return; }", sortie)
        self.assertIn("    SC2_RL_VIDE(c, 0x0000u);\n    SC2_X87_VIDE(c, 0x000u);\n}", sortie)

    def test_x87_fld_st1_lecture_avant_rotation(self):
        """fld st(1) : v évalué avant la rotation — SC2_X87_PUSH(SC2_X87_ST(1)), masque 0x101."""
        sortie = self.transformer_x87(X_FLD_ST1)
        self.assertIn("SC2_X87_PUSH(SC2_X87_ST(1));", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); SC2_X87_VIDE(c, 0x101u); return; }", sortie)

    def test_x87_fxch_trois_affectations(self):
        """fxch : écritures de s0 et s1 (0x003), ni t ni sw."""
        sortie = self.transformer_x87(X_FXCH)
        self.assertIn("{ double _t = SC2_X87_ST(0); SC2_X87_ST(0) = SC2_X87_ST(1); "
                      "SC2_X87_ST(1) = _t; }", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); SC2_X87_VIDE(c, 0x003u); return; }", sortie)

    def test_x87_fcom_fnstsw(self):
        """fcompare salit sw (0x200) ; fnstsw est une lecture, eax écrit côté rl (0x001)."""
        sortie = self.transformer_x87(X_FCOM_FNSTSW)
        self.assertIn("SC2_X87_FCOM(SC2_X87_ST(0), SC2_X87_ST(2));", sortie)
        self.assertIn("SET16(rl->eax, (uint16_t)SC2_X87_FNSTSW());", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0001u); SC2_X87_VIDE(c, 0x200u); return; }", sortie)

    def test_x87_frontiere_au_milieu(self):
        """Appel entre le PUSH et le fstp : VIDE x87 0x101 avant, CHARGE après, 0x180 au return."""
        sortie = self.transformer_x87(X_FRONTIERE)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); SC2_X87_VIDE(c, 0x101u); dispatch(c, 0x00A00040u); "
                      "SC2_RL_CHARGE(c); SC2_X87_CHARGE(c); }", sortie)
        # après la frontière la pile est rechargée propre : le POP ne salit plus que t (0x100)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); SC2_X87_VIDE(c, 0x100u); return; }", sortie)
        self.assertIn("    SC2_X87_VIDE(c, 0x000u);\n", sortie)   # vidage final : 0 après la frontière

    def test_x87_return_sale(self):
        """return avec la pile sale : masque 0x101 (s0|t) au retour et au vidage final."""
        sortie = self.transformer_x87(X_RETURN_SALE)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); SC2_X87_VIDE(c, 0x101u); return; }", sortie)

    def test_x87_boucle_fusion_union(self):
        """Boucle : le point fixe de PUSH monte d'un cran par tour → 0x1FF (tout slot possible)."""
        sortie = self.transformer_x87(X_BOUCLE)
        self.assertIn("L_00C000A5:\n    SC2_RL_BOUCLE();\n    SC2_X87_PUSH(1.0);", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0002u); SC2_X87_VIDE(c, 0x1FFu); return; }", sortie)

    def test_x87_faddp_ecriture_puis_pop(self):
        """faddp : écriture de s1 PUIS rotation → le retard atterrit sur s0 : 0x101."""
        sortie = self.transformer_x87(X_FADDP)
        self.assertIn("SC2_X87_ST(1) = SC2_X87_ST(1) + SC2_X87_ST(0); SC2_X87_POP();", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0000u); SC2_X87_VIDE(c, 0x101u); return; }", sortie)

    def test_x87_while_conservateur(self):
        """while avec x87 : 0 à n rotations possibles → tous les slots et t (0x1FF)."""
        sortie = self.transformer_x87(X_WHILE)
        self.assertIn("while (rl->ecx) { SC2_X87_PUSH(1.0); rl->ecx--; }", sortie)
        self.assertIn("{ SC2_RL_VIDE(c, 0x0002u); SC2_X87_VIDE(c, 0x1FFu); return; }", sortie)

    def test_x87_repli_aide_non_reecrite(self):
        """fpu_fninit (touche fpu_sw/fpu_top) : repli x87 seul, formes intactes sur c."""
        sortie = self.repli_x87(X_REPLI_FNINIT, "x87 : aide x87 non réécrite (fpu_fninit)")
        self.assertIn("fpush(c, 1.0);", sortie)
        self.assertIn("fpu_fninit(c);", sortie)

    def test_x87_repli_fst_non_dereference(self):
        self.repli_x87(X_REPLI_FST_ADRESSE, "x87 : fst non déréférencé")

    def test_x87_repli_indice_non_litteral(self):
        self.repli_x87(X_REPLI_FST_INDICE, "x87 : indice fst non littéral")

    def test_x87_fonction_sans_x87_inchangee(self):
        """Sans forme x87, la sortie de --x87 est identique octet pour octet à la sortie sans option."""
        (self.entree / "recomp_00.c").write_text(ENTETE + F_SIMPLE, encoding="utf-8")
        s1, s2 = self.racine / "s1", self.racine / "s2"
        self.passage(sortie=s1)
        self.passage_x87(sortie=s2)
        self.assertEqual((s1 / "recomp_00.c").read_bytes(), (s2 / "recomp_00.c").read_bytes())
        self.assertNotIn("SC2_X87_", (s2 / "recomp_00.c").read_text(encoding="utf-8"))

    def test_x87_sans_option_identique(self):
        """Sans --x87, les formes x87 restent sur c, aucune macro SC2_X87_* émise."""
        (self.entree / "recomp_00.c").write_text(ENTETE + X_PUSH_FSTP, encoding="utf-8")
        self.passage()
        sortie = (self.sortie / "recomp_00.c").read_text(encoding="utf-8")
        self.assertIn("fpush(c, rdf32((rl->edi + rl->eax*1 + 0x00000008u)));", sortie)
        self.assertIn("wrf32((rl->ecx), *fst(c, 0)); fpop(c);", sortie)
        self.assertNotIn("SC2_X87_", sortie)

    def test_x87_stats(self):
        """Ligne de statistiques x87 : transformées, replis par motif, formes réécrites."""
        (self.entree / "recomp_00.c").write_text(ENTETE + X_PUSH_FSTP + X_REPLI_FNINIT
                                                 + X_FCOM_FNSTSW, encoding="utf-8")
        resultat = self.passage_x87()
        self.assertRegex(resultat.stdout,
                         r"x87 : fonctions transformées côté x87 : 2 sur 3, "
                         r"replis x87 : 1 \(x87 : aide x87 non réécrite \(fpu_fninit\) : 1\), "
                         r"PUSH 1, POP 1, ST 3, FCOM 1, FNSTSW 1, vidages x87 : 4, "
                         r"bits par masque x87 : [0-9]+\.[0-9]{2}")

    def test_x87_verifier_conforme(self):
        """--verifier --x87 : retrait octet pour octet (macros SC2_X87_* déballées), invariants."""
        (self.entree / "recomp_00.c").write_text(
            ENTETE + X_PUSH_FSTP + X_PUSH8_POP8 + X_FXCH + X_FCOM_FNSTSW + X_FRONTIERE
            + X_BOUCLE + X_FADDP + X_WHILE + X_FLD_ST1 + X_RETURN_SALE + X_REPLI_FNINIT
            + X_REPLI_FST_ADRESSE + X_REPLI_FST_INDICE + F_SIMPLE, encoding="utf-8")
        self.passage_x87()
        resultat = self.lancer(self.entree, self.sortie, "--cpu-h", str(CPU_H), "--x87", "--verifier")
        self.assertEqual(resultat.returncode, 0, resultat.stderr)
        self.assertIn("pile x87 comprise", resultat.stdout)

    def test_x87_verifier_refuse_masque_altere(self):
        """Un masque SC2_X87_VIDE altéré : le vérificateur signale l'écart."""
        (self.entree / "recomp_00.c").write_text(ENTETE + X_PUSH_FSTP, encoding="utf-8")
        self.passage_x87()
        cible = self.sortie / "recomp_00.c"
        cible.write_text(cible.read_text(encoding="utf-8").replace(
            "SC2_X87_VIDE(c, 0x180u); return;", "SC2_X87_VIDE(c, 0x1FFu); return;"), encoding="utf-8")
        resultat = self.lancer(self.entree, self.sortie, "--cpu-h", str(CPU_H), "--x87", "--verifier")
        self.assertNotEqual(resultat.returncode, 0)
        self.assertIn("masques x87", resultat.stderr)

    def test_x87_compilation_cc(self):
        """Une sortie --x87 compile avec cc, avec et sans -DSC2_RL_CONTROLE."""
        if shutil.which("cc") is None:
            self.skipTest("cc indisponible")
        (self.entree / "recomp_00.c").write_text(ENTETE + X_PUSH_FSTP + X_FCOM_FNSTSW + X_FXCH
                                                 + X_FRONTIERE + X_REPLI_FNINIT, encoding="utf-8")
        self.passage_x87()
        for options in ([], ["-DSC2_ACCES_VOLATILS"], ["-DSC2_ACCES_VOLATILS", "-DSC2_RL_CONTROLE"]):
            resultat = subprocess.run(["cc", "-fsyntax-only", "-Wno-parentheses-equality", *options,
                                       "-include", str(CPU_H.parent / "sc2_lifted.h"),
                                       "-I", str(CPU_H.parent),
                                       str(self.sortie / "recomp_00.c")],
                                      capture_output=True, text=True)
            self.assertEqual(resultat.returncode, 0, f"{options} : {resultat.stderr}")

    def test_x87_vivacite_combine(self):
        """Vivacité + x87 : SC2_RL_VIDEX (drapeaux omis) suivi de SC2_X87_VIDE à l'appel direct."""
        sortie = self.transformer_x87(V_TOUT_ECRIT, V_X87_APPELANT,
                                       extra=("--vivacite-drapeaux",))
        self.assertIn("{ SC2_RL_VIDEX(c, 0x0010u, 0x3F00u); SC2_X87_VIDE(c, 0x101u); "
                      "dispatch(c, 0x00A00100u); SC2_RL_CHARGE(c); SC2_X87_CHARGE(c); }", sortie)


if __name__ == "__main__":
    unittest.main()
