/* mac/sc2_lanceur_mac.m — lanceur de l'app macOS : menus, escarmouche, réglages, relance (V3, lot M9, 24/10/2026 ;
 * v3-critique.md §3.4 fiche M9 ; v3-conception-A.md §1.3 cas 14 — variante NSWorkspace, H11 ; §2.7).
 *
 * Définitions FORTES des deux talons de sc2_mac_hote.c (le GLOB de mac/mac.cmake les lie) :
 *   sc2m_lanceur_menu        ajoute « Partie » / « Missions » / « Réglages » à la barre de menus (l'app elle-même,
 *                            « SC2 Partout », est déjà là : plein écran, quitter). Vocabulaire de l'écran-titre
 *                            d'Android : Continuer / Charger une partie / Escarmouche / Missions / Options.
 *   sc2m_lanceur_install_choisir   premier lancement sans SC2_INSTALL conforme : NSOpenPanel (le dossier de
 *                            StarCraft II), contrôlé par sc2m_install_valider ; SC2_MAC_INSTALL_PROPOSEE le
 *                            remplace pour les essais (sans interface).
 *
 * JOUER = réécrire sc2.env puis SE RELANCER (jamais deux moteurs dans un processus) :
 *   1. sc2m_env_ecrire fusionne <support>/sc2.env : les clés du choix (sc2m_jeu_cles_*, mac/sc2_catalogue_mac.c),
 *      les clés de l'AUTRE mode de lancement RETIRÉES (valeur NULL), tout le reste intact ;
 *   2. NSWorkspace openApplicationAtURL: (createsNewApplicationInstance, environnement = sc2m_env_origine(),
 *      capté au démarrage : l'ancien SC2_ARGS de l'environnement n'est pas hérité) — H11 ;
 *   3. sc2m_quitter : la sortie propre du cas 10 (le jeu se gare, exit au fil principal) ;
 *   4. la nouvelle instance attend le verrou d'instance (≤ SC2M_VERROU_ATTENTE_S = 15 s) et journalise
 *      « [MAC] instance précédente terminée après x s » (sc2_config_mac.c).
 * Hors paquet .app (exécution directe du binaire du banc), la relance est REFUSÉE et journalisée.
 *
 * L'environnement du successeur est l'origine CAPTÉE (donc sans les variables posées par sc2m_configurer, qui
 * l'emporteraient sur sc2.env), moins les clés de lancement (SC2_ARGS, SC2_ESCARMOUCHE*, SC2_SAUVEGARDE,
 * SC2_RUN_SANS_BNET : le choix vient d'être écrit dans sc2.env, un reste d'environnement le recouvrirait) et moins
 * les crochets d'essai du lanceur (SC2_MAC_LANCEUR_CHOIX : le successeur rejouerait le choix, sans fin), plus
 * SC2_FENETRE=1. L'installation choisie au panneau (posée par sc2m_env_lire APRÈS la capture, donc absente de
 * l'origine) est reportée dans sc2.env ET dans l'environnement du successeur : sans elle, le panneau reviendrait
 * à chaque relance et à chaque lancement depuis le Finder.
 *
 * Les réglages (écran, ombres, particules — Options.java) ne relancent rien : ils ne posent QUE leur clé, lue au
 * prochain lancement (comme Android, où ils passent par options.env au lancement suivant).
 * « Charger une partie » (V5, SC2_SAUVEGARDE) est un sous-menu RECONSTRUIT à chaque ouverture (menuNeedsUpdate:) :
 * les sauvegardes de <support>/StarCraft II/Saves par sc2m_sauvegardes (miroir de Progression.java — la plus
 * récente d'abord, moins de 4096 octets masquée), une par item « nom — dd/MM HH:mm » ; choisir = sc2.env + relance,
 * comme les autres choix.
 *
 * Essai sans main (critère 7) : SC2_MAC_LANCEUR_CHOIX joue le choix après SC2_MAC_LANCEUR_DELAI_S secondes
 * (diagnostic, docs/VARIABLES.md) — le clic de menu reste le vrai chemin, commun à la main et à l'essai.
 */
#import <AppKit/AppKit.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>   /* strcasecmp */
#include <time.h>      /* localtime_r, strftime */

#include "sc2_config_mac.h"
#include "sc2_mac.h"
#include "sc2_catalogue_mac.h"

/* runtime (sc2_host.h tire cpu.h : pas d'inclusion en Objective-C) */
void sc2_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Clés jamais héritées par le successeur : les clés de lancement (le choix vient d'être écrit dans sc2.env, or
 * l'environnement du processus L'EMPORTE sur sc2.env — sc2m_configurer, étape 1) et les crochets d'essai du
 * lanceur (le successeur rejouerait le choix, sans fin). */
static const char *const k_relance_interdits[] = {
    "SC2_ARGS", "SC2_ESCARMOUCHE", "SC2_ESCARMOUCHE_RACE", "SC2_ESCARMOUCHE_RACE_IA", "SC2_ESCARMOUCHE_IA",
    "SC2_SAUVEGARDE", "SC2_RUN_SANS_BNET", "SC2_MAC_LANCEUR_CHOIX", "SC2_MAC_LANCEUR_DELAI_S", NULL,
};

/* coche l'item d'état courant d'un sous-menu de réglages (une seule coche). */
static void cocher(NSMenu *m, NSInteger tag)
{
    for (NSMenuItem *i in m.itemArray) i.state = i.tag == tag ? NSControlStateValueOn : NSControlStateValueOff;
}

/* ---- cible des menus (une seule, créée par sc2m_lanceur_menu) --------------------------------------------------- */

@interface SC2LanceurCible : NSObject <NSMenuDelegate>
@property (strong) NSWindow *escarmouche;                       /* panneau d'escarmouche, sans boucle modale */
@property (strong) NSPopUpButton *carte, *race, *raceIA, *difficulte;
@property (strong) NSMenu *charger;                             /* sous-menu « Charger une partie » (reconstruit) */
@end

@implementation SC2LanceurCible
{
    char m_support[1024];              /* <support> : sc2.env, verrou, cache des sha */
    int  m_support_ok;
}

/* Le support est recalculé ici (g_dossiers de sc2_mac_fenetre.m est privé au module) : mêmes règles que M3. */
- (const char *)support
{
    if (m_support_ok) return m_support;
    sc2m_dossiers d;
    NSString *paquet = [NSBundle mainBundle].bundlePath;
    if (sc2m_dossiers_defaut(&d, paquet.UTF8String) == 0) {
        snprintf(m_support, sizeof m_support, "%s", d.support);
        m_support_ok = 1;
    }
    return m_support_ok ? m_support : NULL;
}

- (const char *)sc2env:(char *)o cap:(size_t)cap    /* <support>/sc2.env */
{
    const char *s = [self support];
    if (!s || cap < strlen(s) + 9) return NULL;
    snprintf(o, cap, "%s/sc2.env", s);
    return o;
}

/* Fusionne sc2.env puis relance (l'ordre compte : le fichier est au complet avant le lancement du successeur). */
- (void)relancer:(sc2m_jeu_cles *)k pourquoi:(const char *)pourquoi
{
    /* 1. la relance ne se conçoit que depuis le paquet : le binaire du banc n'a pas d'URL d'app. */
    NSString *paquet = [NSBundle mainBundle].bundlePath;
    if (![paquet hasSuffix:@".app"]) {
        sc2_log("[MAC] lanceur : relance refusée, hors paquet .app (%s) — lancer depuis SC2Partout.app",
                paquet.UTF8String);
        return;
    }
    char *const *origine = sc2m_env_origine();
    if (!origine) {
        sc2_log("[MAC] lanceur : relance refusée, environnement d'origine absent (mode fenêtre ?)");
        return;
    }

    /* 2. fusion de sc2.env : les clés du choix, plus SC2_INSTALL si le panneau l'a choisie (elle n'est que dans
     *    l'environnement de cette instance, posée après la capture), le reste intact (sc2m_env_ecrire, M3). */
    const char *inst = getenv("SC2_INSTALL");
    if (inst && *inst && k->n < SC2M_LANCEUR_CLES_MAX
            && (k->n == 0 || strncmp(k->cles[k->n - 1], "SC2_INSTALL", 11))) {
        k->cles[k->n] = "SC2_INSTALL";
        k->vals[k->n] = inst;
        k->n++;
    }
    char env[1104];
    if (![self sc2env:env cap:sizeof env]) { sc2_log("[MAC] lanceur : support introuvable, choix perdu"); return; }
    if (sc2m_env_ecrire(env, k->cles, k->vals, k->n)) {
        sc2_log("[MAC] lanceur : %s : sc2.env (%s) non réécrit — rien n'est relancé", pourquoi, env);
        return;
    }
    sc2_log("[MAC] lanceur : %s : sc2.env réécrit", pourquoi);

    /* 3. le successeur : nouvelle instance, environnement capté AVANT toute configuration (H11), moins les clés
     *    que sc2.env doit gouverner. SC2_FENETRE=1 y est reposé : la relance doit le mode fenêtre même sans
     *    passer par LSEnvironment. conf.environment est un REMPLACEMENT aux clés qu'il dit, mais l'environ
     * HÉRITÉ par LaunchServices part de celui de CETTE instance : il faut aussi déposer les clés interdites
     * ici même, sinon le successeur les reçoit quand même (mesuré : SC2_MAC_LANCEUR_CHOIX rejoué → chaîne de
     * relances). Ce processus s'arrête juste après : cet unsetenv n'affecte que la relance. */
    for (int i = 0; k_relance_interdits[i]; i++) unsetenv(k_relance_interdits[i]);
    NSMutableDictionary *e = [NSMutableDictionary dictionaryWithCapacity:64];
    for (char *const *p = origine; *p; p++) {
        const char *eg = strchr(*p, '=');
        if (!eg || eg == *p) continue;
        char cle[128];
        size_t lc = (size_t)(eg - *p);
        if (lc >= sizeof cle) continue;
        memcpy(cle, *p, lc);
        cle[lc] = 0;
        int interdit = 0;
        for (int i = 0; k_relance_interdits[i]; i++)
            if (!strcmp(cle, k_relance_interdits[i])) { interdit = 1; break; }
        if (interdit) continue;
        e[[NSString stringWithUTF8String:cle]] = [NSString stringWithUTF8String:eg + 1];
    }
    e[@"SC2_FENETRE"] = @"1";
    if (inst && *inst) e[@"SC2_INSTALL"] = [NSString stringWithUTF8String:inst];
    NSWorkspaceOpenConfiguration *conf = [NSWorkspaceOpenConfiguration new];
    conf.createsNewApplicationInstance = YES;
    conf.environment = e;
    /* Essai isolé (SC2_MAC_ISOLE = 0 seul : l'app est activée) : le successeur ne vole pas le focus. */
    const char *isole = getenv("SC2_MAC_ISOLE");
    conf.activates = !(isole && !strcmp(isole, "0"));

    /* 4. le successeur part, l'ancienne instance s'arrête proprement (cas 10) — DÈS LA DEMANDE, pas à la fin du
     *    completionHandler : il arrive APRÈS le lancement effectif, et le successeur attend au verrou d'instance
     *    (15 s au plus) pendant que le jeu gare ses threads ; attendre ici ferait passer le verrou de l'attente
     *    à l'abandon (code 6). sc2m_quitter peut ne pas revenir (exit au fil principal si le jeu n'est pas
     *    lancé) : c'est voulu. */
    sc2_log("[MAC] lanceur : %s — sc2.env réécrit, relance de %s", pourquoi, paquet.UTF8String);
    sc2m_quitter("lanceur : relance");
    [[NSWorkspace sharedWorkspace] openApplicationAtURL:[NSURL fileURLWithPath:paquet]
                                          configuration:conf
                                    completionHandler:^(NSRunningApplication *app, NSError *err) {
        if (err)
            sc2_log("[MAC] lanceur : relance refusée par macOS (%s) — sc2.env garde le choix pour le prochain "
                    "lancement", err.localizedDescription.UTF8String);
        else
            sc2_log("[MAC] lanceur : successeur lancé (pid %d) ; sortie demandée avant l'ouverture",
                    app ? (int)app.processIdentifier : 0);
    }];
}

/* ---- Partie ------------------------------------------------------------------------------------------------------- */

- (void)partieContinuer:(id)s        /* « Continuer la campagne » : menu au bar, tous les autres modes retirés */
{
    (void)s;
    sc2m_jeu_cles k;
    if (sc2m_jeu_cles_menu(&k) > 0) [self relancer:&k pourquoi:"continuer la campagne (menu au bar)"];
}

- (void)mission:(NSMenuItem *)s      /* une des 33 cartes de campagne : SC2_ARGS=-run Maps/Campaign/X.SC2Map */
{
    char nom[64];
    snprintf(nom, sizeof nom, "%s",
             s.representedObject ? [(NSString *)s.representedObject UTF8String] : s.title.UTF8String);
    sc2m_jeu_cles k;
    if (sc2m_jeu_cles_carte(&k, nom) > 0) [self relancer:&k pourquoi:"mission (carte)"];
    else sc2_log("[MAC] lanceur : carte « %s » hors catalogue, rien n'est relancé", nom);
}

/* ---- Charger une partie (V5) ------------------------------------------------------------------------------------- */

- (void)charger:(NSMenuItem *)s      /* une sauvegarde : SC2_RUN_SANS_BNET=1 + SC2_SAUVEGARDE=Saves/<relatif> */
{
    char relatif[300];               /* « Saves/ » + sc2m_sauvegarde.relatif (256) */
    snprintf(relatif, sizeof relatif, "Saves/%s",
             s.representedObject ? [(NSString *)s.representedObject UTF8String] : s.title.UTF8String);
    sc2m_jeu_cles k;
    if (sc2m_jeu_cles_sauvegarde(&k, relatif) > 0)
        [self relancer:&k pourquoi:"chargement d'une sauvegarde"];
    else
        sc2_log("[MAC] lanceur : sauvegarde « %s » inutilisable, rien n'est relancé", relatif);
}

/* Le sous-menu est vide en permanence hors ouverture : menuNeedsUpdate: le reconstruit à chaque fois (les
 * sauvegardes changent pendant que le jeu tourne — une sauvegarde vient peut-être d'être écrite). */
- (void)menuNeedsUpdate:(NSMenu *)m
{
    if (m != self.charger) return;
    [m removeAllItems];

    /* <support>/StarCraft II/Saves : le dossier Documents du jeu par défaut (sc2m_configurer, étape 5). */
    char saves[1104];
    const char *s = [self support];
    if (!s || (size_t)snprintf(saves, sizeof saves, "%s/StarCraft II/Saves", s) >= sizeof saves) {
        NSMenuItem *it = [m addItemWithTitle:@"Aucune sauvegarde" action:NULL keyEquivalent:@""];
        it.enabled = NO;
        return;
    }

    sc2m_sauvegarde liste[80];       /* ecranCharger : sauvegardes(saves, 80), dont 50 utilisables affichées */
    int n = sc2m_sauvegardes(saves, liste, 80);
    int affichees = 0, masquees = 0;
    for (int i = 0; i < n; i++) {
        if (!sc2m_sauvegarde_utilisable(&liste[i])) { masquees++; continue; }
        if (affichees >= 50) break;
        /* libelle : « nom — dd/MM HH:mm » (Progression.Sauvegarde.libelle) ; dossier parent en info. */
        const char *slash = strrchr(liste[i].relatif, '/');
        const char *nomF = slash ? slash + 1 : liste[i].relatif;
        size_t ln = strlen(nomF);
        if (ln <= 8) continue;                                   /* .SC2Save : le nom ne peut pas être vide */
        char nom[64];
        snprintf(nom, sizeof nom, "%.*s", (int)(ln - 8), nomF);
        time_t t = (time_t)liste[i].date;
        struct tm tmv;
        char quand[32];
        localtime_r(&t, &tmv);
        strftime(quand, sizeof quand, "%d/%m %H:%M", &tmv);
        NSString *rel = [NSString stringWithUTF8String:liste[i].relatif];
        NSMenuItem *it = [m addItemWithTitle:[NSString stringWithFormat:@"%s — %s", nom, quand]
                                      action:@selector(charger:)
                               keyEquivalent:@""];
        it.target = self;
        it.representedObject = rel;              /* relatif à Saves, sans le préfixe « Saves/ » */
        char tt[320];
        int r = slash ? snprintf(tt, sizeof tt, "%.*s", (int)(slash - liste[i].relatif), liste[i].relatif)
                      : snprintf(tt, sizeof tt, "%s", "racine de Saves");
        if (r > 0) it.toolTip = [NSString stringWithUTF8String:tt];
        affichees++;
    }
    if (affichees == 0) {
        NSMenuItem *it = [m addItemWithTitle:@"Aucune sauvegarde" action:NULL keyEquivalent:@""];
        it.enabled = NO;
    }
    if (masquees > 0) {
        NSMenuItem *it = [m addItemWithTitle:[NSString stringWithFormat:@"%d sauvegarde(s) vide(s) masquée(s)",
                                                       masquees]
                                     action:NULL keyEquivalent:@""];
        it.enabled = NO;
    }
}

/* ---- Escarmouche (panneau : carte mêlée, races, difficulté — l'écran V4.10 d'Android) ----------------------------- */

- (void)escarmoucheFenetre:(id)s
{
    (void)s;
    if (self.escarmouche) { [self.escarmouche makeKeyAndOrderFront:nil]; return; }

    /* présélections : sc2.env actuel, défauts de LanceurActivity (race 0, race IA 3, difficulté 1). */
    char env[1104], v[256];
    const char *senv = [self sc2env:env cap:sizeof env];
    int iCarte = 12;                                 /* SteppesofWar (n° 13) : MELEE_DEFAUT */
    if (senv && sc2m_env_valeur(senv, "SC2_ESCARMOUCHE", v, sizeof v))
        for (int i = 0; i < sc2m_n_cartes_melee; i++)
            if (!strcasecmp(sc2m_cartes_melee[i], v)) { iCarte = i; break; }
    int iRace = 0, iRaceIA = 3, iDiff = 1;
    if (senv) {
        if (sc2m_env_valeur(senv, "SC2_ESCARMOUCHE_RACE", v, sizeof v))
            for (int i = 0; i < sc2m_n_races; i++) if (!strcasecmp(sc2m_races[i][0], v)) { iRace = i; break; }
        if (sc2m_env_valeur(senv, "SC2_ESCARMOUCHE_RACE_IA", v, sizeof v))
            for (int i = 0; i < sc2m_n_races; i++) if (!strcasecmp(sc2m_races[i][0], v)) { iRaceIA = i; break; }
        if (sc2m_env_valeur(senv, "SC2_ESCARMOUCHE_IA", v, sizeof v))
            for (int i = 0; i < sc2m_n_difficultes; i++)
                if (!strcasecmp(sc2m_difficultes[i][0], v)) { iDiff = i; break; }
    }

    /* quatre rangées libellé + menu : Carte / Votre race / Race de l'IA / Difficulté de l'IA. */
    NSMutableArray *cartes = [NSMutableArray arrayWithCapacity:sc2m_n_cartes_melee];
    for (int i = 0; i < sc2m_n_cartes_melee; i++)
        [cartes addObject:[NSString stringWithFormat:@"%s (%d joueurs)", sc2m_cartes_melee[i],
                           sc2m_joueurs_melee[i]]];
    NSMutableArray *races = [NSMutableArray arrayWithCapacity:sc2m_n_races];
    for (int i = 0; i < sc2m_n_races; i++) [races addObject:[NSString stringWithUTF8String:sc2m_races[i][1]]];
    NSMutableArray *diffs = [NSMutableArray arrayWithCapacity:sc2m_n_difficultes];
    for (int i = 0; i < sc2m_n_difficultes; i++)
        [diffs addObject:[NSString stringWithUTF8String:sc2m_difficultes[i][1]]];
    NSString *titres[4] = { @"Carte", @"Votre race", @"Race de l'IA", @"Difficulté de l'IA" };
    NSArray<NSString *> *items[4] = { cartes, races, races, diffs };
    int sel[4] = { iCarte, iRace, iRaceIA, iDiff };

    /* Fenêtre flottante SANS boucle modale : le jeu et ses Present continuent pendant le panneau (M5). */
    NSWindow *fen = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 420, 246)
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable
                                                  backing:NSBackingStoreBuffered defer:NO];
    fen.title = @"Escarmouche contre l'IA";
    fen.releasedWhenClosed = NO;                     /* C-4 : jamais de libération sous le présentateur */
    CGFloat y = 200;
    for (int i = 0; i < 4; i++, y -= 40) {
        NSTextField *l = [NSTextField labelWithString:titres[i]];
        l.frame = NSMakeRect(20, y, 120, 22);
        [fen.contentView addSubview:l];
        NSPopUpButton *p = [[NSPopUpButton alloc] initWithFrame:NSMakeRect(150, y, 250, 26) pullsDown:NO];
        [p addItemsWithTitles:items[i]];
        [p selectItemAtIndex:sel[i]];
        [fen.contentView addSubview:p];
        switch (i) {
            case 0: self.carte = p; break;
            case 1: self.race = p; break;
            case 2: self.raceIA = p; break;
            default: self.difficulte = p; break;
        }
    }
    NSButton *lancer = [[NSButton alloc] initWithFrame:NSMakeRect(305, 16, 95, 28)];
    lancer.title = @"Lancer";
    lancer.bezelStyle = NSBezelStyleRounded;
    lancer.keyEquivalent = @"\r";
    lancer.target = self;
    lancer.action = @selector(escarmoucheLancer:);
    [fen.contentView addSubview:lancer];
    NSButton *annuler = [[NSButton alloc] initWithFrame:NSMakeRect(200, 16, 95, 28)];
    annuler.title = @"Annuler";
    annuler.bezelStyle = NSBezelStyleRounded;
    annuler.target = self;
    annuler.action = @selector(escarmoucheAnnuler:);
    [fen.contentView addSubview:annuler];

    self.escarmouche = fen;
    [fen center];
    [fen makeKeyAndOrderFront:nil];
}

- (void)escarmoucheLancer:(id)s
{
    (void)s;
    /* item n° i → VALEUR colonne 0 (Catalogue.contenuEscarmouche reçoit les valeurs, pas les libellés). */
    NSInteger ic = self.carte.indexOfSelectedItem, ir = self.race.indexOfSelectedItem,
              iri = self.raceIA.indexOfSelectedItem, id_ = self.difficulte.indexOfSelectedItem;
    if (ic < 0) ic = 0;
    if (ir < 0) ir = 0;
    if (iri < 0) iri = 3;
    if (id_ < 0) id_ = 1;
    [self.escarmouche orderOut:nil];
    sc2m_jeu_cles k;
    if (sc2m_jeu_cles_escarmouche(&k, sc2m_cartes_melee[ic], sc2m_races[ir][0], sc2m_races[iri][0],
                                  sc2m_difficultes[id_][0]) > 0)
        [self relancer:&k pourquoi:"escarmouche (carte mêlée)"];
    else
        sc2_log("[MAC] lanceur : escarmouche incomplète, rien n'est relancé");
}

- (void)escarmoucheAnnuler:(id)s { (void)s; [self.escarmouche orderOut:nil]; }

/* ---- Réglages (aucune relance : la clé est lue au prochain lancement) --------------------------------------------- */

- (void)reglageEcran:(NSMenuItem *)s
{
    sc2m_jeu_cles k;
    if (s.tag < sc2m_n_resolutions && sc2m_reglage_ecran(&k, sc2m_resolutions[s.tag][0]) > 0)
        [self appliquerReglage:&k item:s];
}

- (void)reglageOmbres:(NSMenuItem *)s        /* tag 1 = coupées, tag 0 = selon le réglage du jeu */
{
    sc2m_jeu_cles k;
    (void)sc2m_reglage_ombres(&k, s.tag != 0);
    [self appliquerReglage:&k item:s];
}

- (void)reglageParticules:(NSMenuItem *)s    /* tag 1 = coupées, tag 0 = activées */
{
    sc2m_jeu_cles k;
    (void)sc2m_reglage_particules(&k, s.tag != 0);
    [self appliquerReglage:&k item:s];
}

/* écrit la clé (retrait compris : valeur NULL), coche l'item choisi dans son sous-menu. */
- (void)appliquerReglage:(const sc2m_jeu_cles *)k item:(NSMenuItem *)s
{
    char env[1104];
    if (![self sc2env:env cap:sizeof env]) return;
    if (sc2m_env_ecrire(env, k->cles, k->vals, k->n)) {
        sc2_log("[MAC] lanceur : réglage perdu, sc2.env (%s) non réécrit", env);
        return;
    }
    for (int i = 0; i < k->n; i++)
        sc2_log("[MAC] lanceur : réglage %s=%s (au prochain lancement)", k->cles[i],
                k->vals[i] ? k->vals[i] : "(retiré)");
    cocher(s.menu, s.tag);
}

/* ---- essai sans main : SC2_MAC_LANCEUR_CHOIX (diagnostic) -------------------------------------------------------- */

- (void)choixAutomatique:(NSString *)choix
{
    sc2m_jeu_cles k;
    if ([choix isEqualToString:@"menu"]) {
        if (sc2m_jeu_cles_menu(&k) > 0) [self relancer:&k pourquoi:"choix automatique (menu au bar)"];
        return;
    }
    if ([choix hasPrefix:@"carte:"]) {
        char nom[64];
        snprintf(nom, sizeof nom, "%s", [choix substringFromIndex:6].UTF8String);
        if (sc2m_jeu_cles_carte(&k, nom) > 0) { [self relancer:&k pourquoi:"choix automatique (carte)"]; return; }
        sc2_log("[MAC] lanceur : SC2_MAC_LANCEUR_CHOIX : carte « %s » hors catalogue", nom);
        return;
    }
    if ([choix hasPrefix:@"escarmouche:"]) {
        /* escarmouche:<carte>:<race>:<raceIA>:<difficulté> (valeurs, colonne 0 des tableaux). */
        NSArray<NSString *> *c = [[choix substringFromIndex:12] componentsSeparatedByString:@":"];
        if (c.count == 4 && sc2m_jeu_cles_escarmouche(&k, c[0].UTF8String, c[1].UTF8String, c[2].UTF8String,
                                                      c[3].UTF8String) > 0) {
            [self relancer:&k pourquoi:"choix automatique (escarmouche)"];
            return;
        }
        sc2_log("[MAC] lanceur : SC2_MAC_LANCEUR_CHOIX : escarmouche incomplète (%s)", choix.UTF8String);
        return;
    }
    if ([choix hasPrefix:@"sauvegarde:"]) {
        /* sauvegarde:<chemin relatif au dossier « Documents\StarCraft II »> : ex. « sauvegarde:Saves/Campaign/
         * The Outlaws.SC2Save » — le même chemin que la valeur SC2_SAUVEGARDE du choix manuel. */
        char chemin[300];
        snprintf(chemin, sizeof chemin, "%s", [choix substringFromIndex:11].UTF8String);
        if (sc2m_jeu_cles_sauvegarde(&k, chemin) > 0) {
            [self relancer:&k pourquoi:"choix automatique (sauvegarde)"];
            return;
        }
        sc2_log("[MAC] lanceur : SC2_MAC_LANCEUR_CHOIX : sauvegarde « %s » refusée", chemin);
        return;
    }
    sc2_log("[MAC] lanceur : SC2_MAC_LANCEUR_CHOIX : choix « %s » inconnu (menu | carte:<nom> | "
            "escarmouche:<carte>:<race>:<raceIA>:<difficulté> | sauvegarde:<chemin>)", choix.UTF8String);
}

@end

/* ---- la barre de menus -------------------------------------------------------------------------------------------- */

static SC2LanceurCible *g_cible;

void sc2m_lanceur_menu(void *nsmenu_principal)
{
    NSMenu *barre = (__bridge NSMenu *)nsmenu_principal;
    g_cible = [SC2LanceurCible new];

    /* Partie : continuer (menu au bar), escarmouche, chargement (sous-menu reconstruit à l'ouverture, V5). */
    NSMenu *partie = [[NSMenu alloc] initWithTitle:@"Partie"];
    NSMenuItem *it = [partie addItemWithTitle:@"Continuer la campagne" action:@selector(partieContinuer:)
                                keyEquivalent:@""];
    it.target = g_cible;
    [partie addItem:[NSMenuItem separatorItem]];
    it = [partie addItemWithTitle:@"Escarmouche…" action:@selector(escarmoucheFenetre:) keyEquivalent:@""];
    it.target = g_cible;
    [partie addItem:[NSMenuItem separatorItem]];
    NSMenu *charger = [[NSMenu alloc] initWithTitle:@"Charger une partie"];
    g_cible.charger = charger;
    charger.delegate = g_cible;            /* menuNeedsUpdate: reconstruit la liste à chaque ouverture */
    it = [partie addItemWithTitle:@"Charger une partie" action:NULL keyEquivalent:@""];
    it.submenu = charger;
    it = [barre addItemWithTitle:@"Partie" action:NULL keyEquivalent:@""];
    it.submenu = partie;

    /* Missions : les 33 cartes de campagne, dans l'ordre du catalogue (critère 6). */
    NSMenu *missions = [[NSMenu alloc] initWithTitle:@"Missions"];
    for (int i = 0; i < sc2m_n_cartes; i++) {
        NSString *nom = [NSString stringWithUTF8String:sc2m_cartes[i]];
        NSMenuItem *m = [missions addItemWithTitle:nom action:@selector(mission:) keyEquivalent:@""];
        m.target = g_cible;
        m.representedObject = nom;
    }
    it = [barre addItemWithTitle:@"Missions" action:NULL keyEquivalent:@""];
    it.submenu = missions;

    /* Réglages : écran (Options.RESOLUTIONS), ombres, particules — coche = sc2.env actuel. */
    char env[1104], v[256];
    const char *senv = NULL;
    {
        sc2m_dossiers d;
        if (sc2m_dossiers_defaut(&d, [NSBundle mainBundle].bundlePath.UTF8String) == 0) {
            snprintf(env, sizeof env, "%s/sc2.env", d.support);
            senv = env;
        }
    }
    int ecran = 0;                                       /* « auto » par défaut */
    if (senv && sc2m_env_valeur(senv, "SC2_ECRAN", v, sizeof v))
        for (int i = 0; i < sc2m_n_resolutions; i++)
            if (!strcasecmp(sc2m_resolutions[i][0], v)) { ecran = i; break; }

    NSMenu *reglages = [[NSMenu alloc] initWithTitle:@"Réglages"];
    NSMenu *ecranM = [[NSMenu alloc] initWithTitle:@"Écran"];
    for (int i = 0; i < sc2m_n_resolutions; i++) {
        NSMenuItem *m = [ecranM addItemWithTitle:[NSString stringWithUTF8String:sc2m_resolutions[i][1]]
                                         action:@selector(reglageEcran:) keyEquivalent:@""];
        m.target = g_cible;
        m.tag = i;
    }
    cocher(ecranM, ecran);
    it = [reglages addItemWithTitle:@"Écran…" action:NULL keyEquivalent:@""];
    it.submenu = ecranM;

    NSMenu *ombres = [[NSMenu alloc] initWithTitle:@"Ombres"];
    it = [ombres addItemWithTitle:@"Selon le réglage du jeu" action:@selector(reglageOmbres:) keyEquivalent:@""];
    it.target = g_cible;
    it.tag = 0;
    it = [ombres addItemWithTitle:@"Coupées" action:@selector(reglageOmbres:) keyEquivalent:@""];
    it.target = g_cible;
    it.tag = 1;
    cocher(ombres, senv && sc2m_env_valeur(senv, "SC2_OMBRES", v, sizeof v) && !strcmp(v, "0") ? 1 : 0);
    it = [reglages addItemWithTitle:@"Ombres…" action:NULL keyEquivalent:@""];
    it.submenu = ombres;

    NSMenu *particules = [[NSMenu alloc] initWithTitle:@"Particules"];
    it = [particules addItemWithTitle:@"Activées" action:@selector(reglageParticules:) keyEquivalent:@""];
    it.target = g_cible;
    it.tag = 0;
    it = [particules addItemWithTitle:@"Coupées" action:@selector(reglageParticules:) keyEquivalent:@""];
    it.target = g_cible;
    it.tag = 1;
    cocher(particules, senv && sc2m_env_valeur(senv, "SC2_INSTANCES", v, sizeof v) && !strcmp(v, "0") ? 1 : 0);
    it = [reglages addItemWithTitle:@"Particules…" action:NULL keyEquivalent:@""];
    it.submenu = particules;
    it = [barre addItemWithTitle:@"Réglages" action:NULL keyEquivalent:@""];
    it.submenu = reglages;

    /* essai sans main : le choix part après le délai, sur le fil principal (AppKit). */
    const char *choix = getenv("SC2_MAC_LANCEUR_CHOIX");
    if (choix && *choix) {
        const char *delaiS = getenv("SC2_MAC_LANCEUR_DELAI_S");
        int delai = delaiS ? atoi(delaiS) : 5;
        if (delai < 1) delai = 1;
        sc2_log("[MAC] lanceur : SC2_MAC_LANCEUR_CHOIX jouera « %s » dans %d s", choix, delai);
        NSString *c = [NSString stringWithUTF8String:choix];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delai * NSEC_PER_SEC),
                       dispatch_get_main_queue(), ^{ [g_cible choixAutomatique:c]; });
    }
}

/* ---- choix de l'installation (premier lancement sans SC2_INSTALL) ------------------------------------------------- */

int sc2m_lanceur_install_choisir(char *install, size_t cap)
{
    sc2m_dossiers d;
    if (sc2m_dossiers_defaut(&d, [NSBundle mainBundle].bundlePath.UTF8String)) {
        sc2_log("[MAC] choix de l'installation impossible : dossiers introuvables");
        return -1;
    }

    /* essai sans interface : la proposition des essais, contrôlée comme un vrai choix. */
    const char *proposee = getenv("SC2_MAC_INSTALL_PROPOSEE");
    if (proposee && *proposee) {
        char motif[256];
        if (sc2m_install_valider(proposee, d.support, motif, sizeof motif) == 0) {
            snprintf(install, cap, "%s", proposee);
            return 0;
        }
        sc2_log("[MAC] SC2_MAC_INSTALL_PROPOSEE=%s refusée : %s", proposee, motif);
        return -1;
    }

    /* NSOpenPanel jusqu'à un dossier conforme (ou annulation). */
    while (1) {
        NSOpenPanel *p = [NSOpenPanel openPanel];
        p.canChooseDirectories = YES;
        p.canChooseFiles = NO;
        p.allowsMultipleSelection = NO;
        p.title = @"Où est StarCraft II ?";
        p.message = @"Choisissez le dossier de StarCraft II (celui qui contient Versions/Base15405).";
        p.prompt = @"Choisir";
        if ([p runModal] != NSModalResponseOK) {
            sc2_log("[MAC] choix de l'installation annulé — sortie");
            return -1;
        }
        NSString *dossier = p.URL.path;
        char motif[256];
        if (sc2m_install_valider(dossier.UTF8String, d.support, motif, sizeof motif) == 0) {
            snprintf(install, cap, "%s", dossier.UTF8String);
            return 0;
        }
        NSAlert *a = [NSAlert new];
        a.messageText = @"Ce dossier n'est pas une installation de StarCraft II";
        a.informativeText = [NSString stringWithUTF8String:motif];
        [a addButtonWithTitle:@"Choisir un autre dossier"];
        [a runModal];
    }
}
