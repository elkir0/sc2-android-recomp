/* sc2_documents.h — passe 10t : Banks/ et Saves/ de StarCraft II persistants (SC2_DOCUMENTS) ;
 * V1.5 (22/10) : semis de Variables.txt (SC2_VARIABLES, SC2_VARIABLES_FIGE), commun au banc Mac et à Android.
 *
 * Le C:\ émulé ($SC2_ROOT, « sc2-c ») est effacé à chaque lancement sur Android (passe 6a) :
 * la progression de campagne (Banks/, ex. TCampaign.SC2Bank) et les sauvegardes (Saves/)
 * seraient perdues. SC2_DOCUMENTS désigne un dossier hôte persistant — cf. sc2_documents.c.
 */
#ifndef RECOMP_SC2_DOCUMENTS_H
#define RECOMP_SC2_DOCUMENTS_H

/* Au démarrage du runtime, avant tout code du jeu : pose racine/Users/sc2/Documents/
 * StarCraft II/{Banks,Saves} comme liens symboliques vers persistant/{Banks,Saves}
 * (créés s'ils manquent). Un vrai dossier préexistant (ancien sc2-c non effacé, ex. banc
 * Mac) est d'abord fusionné dans le persistant sans écraser un fichier plus récent
 * (mtime : le plus récent gagne), puis remplacé par le lien ; un lien vers ailleurs est
 * remplacé. Idempotent. 0 = tout est en place, -1 = échec (raison journalisée ; le jeu
 * continue sans persistance). */
int sc2_documents_preparer(const char *racine, const char *persistant);

/* V1.5 : Variables.txt, les réglages du jeu (déplacé de semer_vars(), android/jni/sc2_android_main.c).
 * Le jeu le lit et l'écrit dans C:\Users\sc2\Documents\StarCraft II\Variables.txt, soit
 * racine/Users/sc2/Documents/StarCraft II/Variables.txt (hpath() de sc2_iat.c ; racine = $SC2_ROOT, « sc2-c »
 * par défaut). Une source n'est acceptée que si c'est un fichier ordinaire de 1 octet à 64 Kio (bornes de
 * l'ancien semer_vars). */
#define SC2_VARIABLES_MAX (64 * 1024)

/* DeviceId de la carte annoncée au jeu (GetAdapterIdentifier, recomp-sc2/runtime/sc2_d3d9.c) selon SC2_GPU_PROFIL :
 * dx10 → 0x05E3 (1507, GeForce GTX 285), sinon 0x0402 (1026, GeForce 8600 GT, profil historique). Le jeu
 * RE-DÉTECTE ses réglages, et écrase ceux semés, quand le lastDeviceId de Variables.txt manque ou diffère de ce
 * DeviceId (constaté au banc le 22/10). sc2_gpu_device_id : définition FAIBLE dans sc2_documents.c (copie de la table
 * de sc2_d3d9.c, même repli) ; une définition forte dans sc2_d3d9.c (gpu_profil_courant()->device) la remplace. */
unsigned sc2_gpu_device_id(void);
unsigned sc2_variables_device_id(void);

/* Sème la source (fichier) : copiée si Variables.txt est ABSENT de la racine, ou toujours si fige (réglages
 * réécrits à chaque lancement : banc, canon épinglé). Écriture par fichier temporaire + rename, dossiers créés.
 * lastDeviceId est ALIGNÉ sur sc2_variables_device_id() : une source sans la ligne (préréglages de
 * scripts/prereglages/, « fold ») la reçoit en fin de fichier ; une source qui l'a avec une autre valeur
 * (Variables.txt écrit par le jeu sous un autre profil, ex. /sdcard du Fold écrit sous la 8600 GT) la voit
 * remplacée, journalisé « [VARS] lastDeviceId 1026 → 1507 : réglages conservés » ; avec la bonne valeur (canon.txt
 * en historique), elle est copiée octet pour octet. 1 = semé, 0 = rien à faire (Variables.txt présent et
 * conservé), -1 = source refusée ou échec (journalisé). */
int sc2_variables_semer(const char *racine, const char *source, int fige);

/* Même chose depuis un texte en mémoire (préréglage intégré, ex. « fold » côté Android) ; origine : nom pour le
 * journal. Mêmes retours. */
int sc2_variables_semer_texte(const char *racine, const char *texte, const char *origine, int fige);

/* Point d'appel commun (src/main.c, avant tout code du jeu) : SC2_VARIABLES=<fichier> sème Variables.txt s'il
 * est absent, SC2_VARIABLES_FIGE=1 le réécrit à chaque lancement ; sans SC2_VARIABLES (vide ou « 0 ») : rien.
 * Une seule fois par processus : si l'hôte a déjà semé (configurer() d'Android, au même point qu'avant), rien
 * n'est refait. Retours de sc2_variables_semer ; 0 quand il n'y a rien à faire. */
int sc2_variables_depuis_env(const char *racine);

#endif
