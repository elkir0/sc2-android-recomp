/* mac/sc2_config_mac.h — configuration de l'app macOS, en C pur (V3, lot M3, 22/10/2026).
 *
 * Interface d'A §2.6 (docs de conception du 22/10), avec la correction de la critique §3.4 M3 : Variables.txt n'est
 * plus semé par l'hôte. L'hôte sauve celui du lancement précédent (sauver_vars d'Android) puis pose
 * SC2_VARIABLES=<support>/Variables.txt ; le runtime de V1.5 (sc2_variables_depuis_env, src/main.c) le sème dans le
 * C:\ neuf. Tant que V1.5 n'est pas dans l'arbre (SC2_VARIABLES_MAX absent de sc2_documents.h), le journal porte
 * « [MAC] Variables.txt non semé (V1.5 absente) ».
 *
 * Fils : P = fil principal, AVANT tout autre fil (setenv n'est pas sûr dès qu'un autre fil lit l'environnement).
 * Journal : lignes « [MAC] … » (et « [VARS] … » pour la sauvegarde de Variables.txt) sur stdout, comme sc2_log.
 * Rien ici n'appelle AppKit ni le runtime : testé sur l'hôte par mac/tests/test_config_mac.c
 * (tests-hote/v3/config.cmake).
 *
 * Disposition de <support> (SC2_MAC_SUPPORT, défaut ~/Library/Application Support/SC2Partout) :
 *   sc2.env             réglages NOM=VALEUR (# en tête = commentaire) ; l'ENVIRONNEMENT DU PROCESSUS L'EMPORTE
 *   install/            install par défaut (Versions/Base15405/SC2.exe), si SC2_INSTALL n'en désigne pas d'autre
 *   Variables.txt       réglages du jeu sauvés au lancement précédent (semés par SC2_VARIABLES)
 *   sc2-c/              C:\ émulé (SC2_ROOT, imposé) : effacé à chaque lancement, sans suivre les liens
 *   StarCraft II/       Banks/ et Saves/ persistants (SC2_DOCUMENTS)
 *   vk-pipelines.bin    cache de pipelines Vulkan (SC2_VK_CACHE_PIPELINES)
 *   frames/             captures (SC2_FRAMES_DIR ; SC2_FRAMES=0 par défaut)
 *   sc2-spirv.pak       pack SPIR-V, s'il n'est pas à côté de l'install
 *   verifications.txt   cache des sha256 : « sha \t taille \t mtime \t chemin », relu tant que taille et mtime tiennent
 *   .instance           verrou d'instance (lockf) et pid du détenteur
 *   Logs/               journal par défaut quand SC2_MAC_SUPPORT est posé (essais isolés)
 */
#ifndef SC2_CONFIG_MAC_H
#define SC2_CONFIG_MAC_H
#include <stddef.h>

#define SC2M_EXE_RELATIF   "Versions/Base15405/SC2.exe"
#define SC2M_EXE_TAILLE    16449536LL
/* Références : le sha256 de VOTRE copie (SC2.exe de la build 15405) et celui de VOTRE pack SPIR-V (produit
 * localement), à donner au build : -DSC2M_EXE_SHA256='"<64 hex>"' -DSC2M_PACK_SHA256='"<64 hex>"'
 * (shasum -a 256 <install>/Versions/Base15405/SC2.exe). Le dépôt public ne publie aucune empreinte du jeu ni
 * de ses dérivés : sans ces définitions, toute install est refusée « NON conforme » (motif journalisé). */
#ifndef SC2M_EXE_SHA256
#define SC2M_EXE_SHA256    "(sha256 de votre copie)"
#endif
#ifndef SC2M_PACK_SHA256
#define SC2M_PACK_SHA256   "(sha256 de votre pack)"
#endif
#define SC2M_VERROU_ATTENTE_S 15   /* relance (M9) : la nouvelle instance attend l'ancienne au plus 15 s */

typedef struct {
    char support[1024];     /* $SC2_MAC_SUPPORT (absolu) ou ~/Library/Application Support/SC2Partout */
    char journaux[1024];    /* ~/Library/Logs/SC2Partout ; <support>/Logs si SC2_MAC_SUPPORT est posé */
    char ressources[1024];  /* <app>/Contents/Resources (polices/, inits-stricts.txt) ; vide : hors paquet */
    char frameworks[1024];  /* <app>/Contents/Frameworks ; vide : aucun MoltenVK embarqué */
} sc2m_dossiers;

typedef struct {
    int  install_a_choisir; /* 1 : SC2_INSTALL absente ou non conforme (motif) — NSOpenPanel, puis second appel */
    char install[1024];     /* install contrôlée (ou candidate refusée) */
    char exe[1100];         /* <install>/Versions/Base15405/SC2.exe : l'argv[1] de sc2_poc_main */
    char motif[256];        /* raison du refus ou de l'erreur, en clair */
} sc2m_config;

/* P ; remplit les dossiers. paquet : chemin du .app ([NSBundle mainBundle].bundlePath) ou NULL ; Resources et
 * Frameworks ne sont retenus que s'ils existent. 0 = prêt, -1 = ni HOME ni SC2_MAC_SUPPORT, ou chemin trop long. */
int  sc2m_dossiers_defaut(sc2m_dossiers *d, const char *paquet);

/* T ; chemin du journal : $SC2_MAC_JOURNAL s'il est posé et non vide (« - » compris), sinon
 * <journaux>/sc2-partout.log. Rend o. */
const char *sc2m_journal_chemin(const sc2m_dossiers *d, char *o, size_t cap);

/* P ; « - » ou vide : stdout inchangé, 0. Sinon : dossier créé, ancien journal renommé en <chemin>.1, stdout ET stderr
 * redirigés vers le fichier (dup2), stdout tamponné par ligne. 0 = redirigé, -1 = échec (stdout inchangé, journalisé). */
int  sc2m_journal_ouvrir(const char *chemin);

/* P ; verrou d'instance : lockf sur <support>/.instance, essayé toutes les 50 ms pendant attente_s secondes au plus.
 * 0 = tenu (pid écrit dans le fichier ; réentrant : un second appel du même processus rend 0), 1 = une autre instance
 * le tient encore au délai, -1 = erreur (dossier ou fichier). Le descripteur reste ouvert jusqu'à la sortie : ne
 * jamais ouvrir puis fermer .instance ailleurs dans le processus (un close relâche les verrous POSIX du fichier). */
int  sc2m_verrou_instance(const char *support, int attente_s);

/* P, AVANT TOUT FIL ; tous les setenv de l'app, puis chdir(<support>). Dans l'ordre :
 *   1. <support>/sc2.env (sc2m_env_lire) : l'environnement du processus l'emporte ;
 *   2. SC2_INSTALL : environnement ou sc2.env (relative : sous <support>), sinon <support>/install ; contrôle
 *      sc2m_install_valider ; refus → install_a_choisir = 1 (et la suite sans les étapes qui en dépendent) ;
 *   3. Variables.txt de <support>/sc2-c/Users/sc2/Documents/StarCraft II/ sauvé vers <support>/Variables.txt AVANT
 *      l'effacement (règles de sauver_vars d'Android : SC2_VARIABLES_FIGE=1 → rien ; 1 o à 64 Kio ; « GraphicsOption » ;
 *      .tmp puis rename) ;
 *   4. <support>/sc2-c effacé (nftw FTW_DEPTH|FTW_PHYS : les liens Banks, Saves et polices sont retirés, jamais suivis) ;
 *      SC2_ROOT=<support>/sc2-c IMPOSÉ (jamais d'effacement d'un autre dossier) ; SC2_VARIABLES=<support>/Variables.txt
 *      si elle est absente et que le fichier est acceptable (1 o à 64 Kio) ;
 *   5. défauts, posés seulement s'ils manquent : SC2_DOCUMENTS=<support>/StarCraft II (une valeur vide reste vide : pas
 *      de persistance), SC2_VK_CACHE_PIPELINES=<support>/vk-pipelines.bin, SC2_FRAMES=0, SC2_FRAMES_DIR=<support>/frames,
 *      SC2_SWAPCHAIN=1, SC2_ECRAN=auto (résolu par l'hôte Cocoa), SC2_RUN_SANS_BNET=1 (« Campagne (au bar) »),
 *      SC2_MANQUE_CONTINUE=1 (décision du mainteneur), SC2_EXE=<install>/Versions/Base15405/SC2.exe (install conforme),
 *      SC2_POLICES=<ressources>/polices et SC2_STRICT_INITS=<ressources>/inits-stricts.txt s'ils existent (hors paquet :
 *      sous SC2_MAC_DEPOT, si cette macro de compilation désigne le dépôt) ;
 *   6. SC2_VK_LIB=<frameworks>/libMoltenVK.dylib si ce fichier existe et que la variable est absente ;
 *   7. pack SPIR-V (install conforme) : SC2_SPIRV_PACK s'il est posé, sinon <parent de l'install>/sc2-spirv.pak (que le
 *      runtime trouve seul), sinon <support>/sc2-spirv.pak (→ SC2_SPIRV_PACK) ; sha256 contrôlé (cache) et journalisé,
 *      jamais bloquant ;
 *   8. chdir(<support>) ; ligne de résumé « [MAC] SC2_INSTALL=… SC2_ROOT=… ».
 * Rend 0 = prêt (c->exe à passer au jeu), 1 = install à choisir (c->motif), < 0 = erreur (c->motif). Rappelable :
 * après le choix de l'install (setenv("SC2_INSTALL", …, 1)), un second appel termine (rend 0), sans effet de bord
 * nouveau puisque le jeu n'est pas lancé. */
int  sc2m_configurer(const sc2m_dossiers *d, sc2m_config *c);

/* P ; <install>/Versions/Base15405/SC2.exe = 16 449 536 o ET sha256 de référence (CC_SHA256). Le sha est mis en cache
 * dans <support>/verifications.txt (support NULL ou vide : pas de cache), relu tant que la taille et la mtime (ns) du
 * fichier n'ont pas changé. Journal : « [MAC] SC2.exe 16449536 o, sha256 <8 premiers>… conforme (cache | calculé en N ms) ».
 * 0 = conforme, 1 = absent ou illisible, 2 = taille ou sha différents (motif rempli dans tous les refus). */
int  sc2m_install_valider(const char *install, const char *support, char *motif, size_t cap);

/* P ; lecture de sc2.env : une affectation NOM=VALEUR par ligne (grammaire de lire_env d'Android, sc2_android_main.c
 * l. 170-184 : la valeur est prise telle quelle jusqu'à la fin de ligne, « = » et espaces compris, sans guillemets) ;
 * blancs en tête ignorés, lignes vides et « # … » ignorées, NOM = [A-Za-z_][A-Za-z0-9_]* (sinon ligne ignorée et
 * journalisée), CR final retiré. setenv(…, 0) : une variable déjà dans l'environnement n'est pas remplacée.
 * Rend le nombre de variables posées (0 si le fichier manque), -1 si le fichier existe mais ne s'ouvre pas. */
int  sc2m_env_lire(const char *fichier);

/* P ; FUSION dans sc2.env : chaque clé cles[i] prend la valeur vals[i] à la place de sa première occurrence (les
 * suivantes disparaissent), ou s'ajoute à la fin ; vals[i] == NULL : la clé est retirée. Toutes les autres lignes
 * (commentaires, lignes vides, autres clés) restent telles quelles, dans l'ordre. Écriture dans <fichier>.tmp puis
 * rename : un lecteur ne voit jamais un fichier à moitié écrit. 0 = écrit, -1 = refus (clé invalide, valeur avec un
 * saut de ligne, n hors de 0..64) ou échec d'E/S (fichier d'origine intact). */
int  sc2m_env_ecrire(const char *fichier, const char *const *cles, const char *const *vals, int n);

/* T ; sha256 d'un fichier en hexadécimal minuscule (hex[64] = 0). 0 = calculé, -1 = illisible. */
int  sc2m_sha256_fichier(const char *chemin, char hex[65]);

/* P ; efface un arbre sans jamais suivre un lien (nftw FTW_DEPTH|FTW_PHYS, comme sc2_android_main.c:332) : un lien,
 * même vers un dossier, est retiré lui-même. Les « / » finaux de la racine sont retirés d'abord (« lien/ » retire le
 * lien, pas sa cible) ; seuls les composants intermédiaires sont résolus. Racine absente : 0. Racine relative, vide,
 * « / » (« // » compris) ou dont le dernier composant est « . » ou « .. » : refus (-1, EINVAL). Rend le nombre
 * d'entrées non effacées (0 = tout effacé), -1 si le parcours échoue. */
int  sc2m_effacer_arbre(const char *racine);

/* Tests seulement : remplace la référence sha256 de SC2.exe (64 caractères hex ; NULL = référence d'origine).
 * 0 = posée, -1 = refusée (pas 64 caractères hex). */
int  sc2m_install_reference(const char *sha256_hex);
/* Tests seulement : nombre de sha256 calculés depuis le début du processus (un succès du cache n'en ajoute pas). */
int  sc2m_sha_calculs(void);

#endif
