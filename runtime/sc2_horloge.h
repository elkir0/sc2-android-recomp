/* sc2_horloge.h — passe 9j : horloge des temps Win32 (ms, CLOCK_MONOTONIC tronqué à 32 bits comme GetTickCount),
 * partagée par GetTickCount / timeGetTime (sc2_iat.c), GetMessageTime (sc2_iat_p4.c, sc2_input_sim.c) et le champ
 * MSG.time (sc2_input_sim.c). En-tête seul : testable sur l'hôte (tests/test_horloge_9j.c), aucun fichier de build. */
#ifndef SC2_HORLOGE_H
#define SC2_HORLOGE_H
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
/* 21/10 : temps passé en PAUSE (app Android en arrière-plan, jeu suspendu), retiré de l'horloge monotone vue par le jeu :
 * à la reprise, aucun saut de temps (pas de rattrapage de la simulation). Définition FAIBLE dans chaque unité qui inclut
 * l'en-tête : l'éditeur de liens n'en garde qu'une (ELF et Mach-O), en-tête seul toujours suffisant (tests). */
_Atomic int64_t g_sc2_pause_ns __attribute__((weak)) = 0;
static inline int64_t sc2_monotone_ns(void)
{
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    int64_t v = (int64_t)t.tv_sec * 1000000000 + (int64_t)t.tv_nsec;
    return v - atomic_load(&g_sc2_pause_ns);
}
static inline uint32_t sc2_tick_ms(void)
{
    return (uint32_t)((uint64_t)sc2_monotone_ns() / 1000000u);
}
#endif
