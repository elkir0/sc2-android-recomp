/* mac/sc2_mac_sortie.m — sortie du processus en mode fenêtre (V3, lot M10, 24/10/2026 ; v3-critique.md §3.4 fiche M10 et
 * greffe 1 de §3.1 ; v3-conception-A.md §1.2 « secours de sortie » et §1.3 cas 10-12 ; revue adverse de M7, MIN-7).
 *
 * sc2_stop (sc2_runtime.c) appelle sc2_sortie_crochet(code) sur le fil appelant, après son bilan et fflush, juste avant
 * exit(code). L'hôte le pose en mode fenêtre seulement (sc2m_fenetre_lancer, M7) ; le banc le laisse à NULL : exit(code)
 * comme avant, sur son fil. Avant M10, exit() tournait sur le fil du jeu PENDANT qu'AppKit tournait (critique A-4) :
 * finaliseurs de MoltenVK sur le mauvais fil, et aucune ligne « [MAC] code de sortie N » (M7, critère 10).
 *
 *   - code ≥ 128 (faute : on_fault → sc2_stop dans le gestionnaire de signal ; le fil fautif peut tenir le verrou de
 *     malloc ou celui du présentateur) ou sc2_stop sur le fil principal (AppKit interrompu dans un état inconnu, et le
 *     fil principal ne peut pas se renvoyer exit à lui-même) : « [MAC] code de sortie N » par write(), puis _exit(N) —
 *     aucun gestionnaire atexit, aucun finaliseur ;
 *   - sinon (fil du jeu au Present, ExitProcess ou SC2_FRAMES_MAX ; fil du secours de sc2m_quitter ; fil invité ou chien
 *     de garde) :
 *       1. code noté (sc2m_sortie_noter : le dernier gestionnaire atexit sortira avec lui) et « sortie en cours » :
 *          le secours de 3 s de sc2m_quitter ne libère plus la swapchain pendant les finaliseurs (MIN-7) ;
 *       2. sc2a_demander_sortie : si l'appelant n'est pas le fil du jeu, celui-ci se gare à son Present suivant
 *          (sc2a_sortir, puis le verrou de sc2_stop) au lieu de présenter pendant exit() ;
 *       3. secours armés (ci-dessous), AVANT l'étape 4, qui peut attendre une frame en vol ;
 *       4. swapchain et surface libérées sur CE fil (sc2v_fenetre(NULL, 0, 0) : le présentateur est au repos, ou finit
 *          sa frame sous son verrou), tant que MoltenVK est entier ;
 *       5. exit(N) renvoyé au fil principal (CFRunLoopPerformBlock en modes communs et dispatch_async, jamais
 *          d'attente), et ce fil reste garé (pause()).
 *     Le fil principal : « [MAC] sortie : code N, exit() sur le fil principal », exit(N) — chaîne atexit (bilan de
 *     l'hôte, finaliseurs de MoltenVK, bilans du runtime), puis en dernier sc2m_sortie_sans_finaliseurs (sc2_mac_hote.c,
 *     1er enregistré par main) : « [MAC] code de sortie N », _exit(N).
 *   - secours (A §1.2 : 3 s puis 8 s, comptés depuis le crochet) : 3 s sans que le fil principal ait pris exit (bloqué)
 *     → exit(N) depuis le fil de secours, une seule fois (le premier des deux qui prend la main) ; 8 s, processus
 *     toujours vivant (exit bloqué dans un finaliseur, ou l'étape 4 sur un présentateur figé) → _exit(3).
 * Un code déjà noté par l'hôte (délai du rejeu : 5, sc2mf_sortie_code) l'emporte sur celui de sc2_stop, sauf une faute.
 */
#import <Foundation/Foundation.h>
#include <dispatch/dispatch.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sc2_mac.h"
#include "sc2_vk_presente.h"

/* runtime (sc2_host.h tire cpu.h : pas d'inclusion en Objective-C) */
void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

static atomic_int g_en_cours, g_exit_pris;

/* T ; 1 dès que sc2_stop a remis la sortie au crochet (bilan du runtime imprimé, exit en route) : lu par le secours de
 * sc2m_quitter (sc2_mac_fenetre.m) avant sc2a_sortir. */
int sc2m_sortie_en_cours(void) { return atomic_load(&g_en_cours); }

static void ecrire(const char *m) { (void)!write(1, m, strlen(m)); }

void sc2m_sortie_crochet(int code)
{
    int deja = sc2m_sortie_code();
    int c = code >= 128 || deja < 0 ? code : deja;
    sc2m_sortie_noter(c);
    atomic_store(&g_en_cours, 1);
    if (c >= 128 || pthread_main_np()) {
        char m[200];
        snprintf(m, sizeof m, "[MAC] code de sortie %d (%s : _exit immédiat, sans gestionnaire atexit ni finaliseur)\n",
                 c, c >= 128 ? "faute" : "sc2_stop sur le fil principal");
        ecrire(m);
        _exit(c);
    }
    char nom[64] = "";
    pthread_getname_np(pthread_self(), nom, sizeof nom);
    sc2_log("[MAC] sortie : sc2_stop(%d) sur le fil « %s »%s — exit(%d) renvoyé au fil principal, ce fil garé", code,
            nom[0] ? nom : "sans nom", c != code ? " (code noté par l'hôte gardé)" : "", c);
    sc2a_demander_sortie();                          /* un autre fil appelant : le jeu se gare à son Present suivant */
    dispatch_queue_t q = dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), q, ^{
        if (atomic_exchange(&g_exit_pris, 1)) return;   /* le fil principal a pris exit : ses finaliseurs tournent */
        sc2_log("[MAC] sortie de secours : fil principal sans réponse 3 s après sc2_stop — exit(%d) depuis un fil de "
                "secours", c);
        exit(c);
    });
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 8 * NSEC_PER_SEC), q, ^{
        char m[200];
        snprintf(m, sizeof m, "[MAC] sortie de secours : processus toujours vivant 8 s après sc2_stop(%d) — _exit(3)\n"
                 "[MAC] code de sortie 3 (secours)\n", c);
        ecrire(m);
        _exit(3);
    });
    sc2v_fenetre(NULL, 0, 0);                        /* AVANT les finaliseurs de MoltenVK, sur ce fil */
    sc2_log("[MAC] sortie : swapchain et surface libérées avant les finaliseurs de MoltenVK (fil de sc2_stop)");
    void (^sortir)(void) = ^{
        if (atomic_exchange(&g_exit_pris, 1)) return;   /* l'autre voie, ou le secours de 3 s, l'a pris */
        sc2_log("[MAC] sortie : code %d, exit() sur le fil principal", c);
        exit(c);
    };
    /* Deux voies, la première servie prend la main. La file principale n'est PAS vidée dans une boucle imbriquée lancée
     * depuis un de ses propres blocs : QUITTER et FERMER du rejeu appellent AppKit dans une source dispatch de la file
     * principale, et NSTerminateLater fait tourner la boucle en NSModalPanelRunLoopMode jusqu'à la fin (essai hors jeu
     * tl.m, 24/10 : dispatch_async jamais servi, CFRunLoopPerformBlock en modes communs servi en 0,2 s ; un vrai Cmd+Q,
     * venu de la boucle d'événements, sert les deux). */
    CFRunLoopPerformBlock(CFRunLoopGetMain(), kCFRunLoopCommonModes, sortir);
    CFRunLoopWakeUp(CFRunLoopGetMain());
    dispatch_async(dispatch_get_main_queue(), sortir);
    for (;;) pause();
}
