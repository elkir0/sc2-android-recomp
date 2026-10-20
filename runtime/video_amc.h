/* video_amc.h — passe 9s : cinématiques H.264 décodées par le matériel (AMediaCodec) à la place du Theora lifté.
 *
 * Le jeu décode ses .ogv avec le libtheora 1.0 LIÉ DANS SC2.exe (code lifté, ~2-3 images/s sur le Fold) et en
 * téléverse les plans Y/Cb/Cr dans des textures L8/A8L8 (TRACE-PASSE7W5). video_amc.c s'intercale, par --wrap au
 * lien (aucun fichier du runtime ni du lift modifié), sur l'API historique de cette libtheora :
 *
 *   0x00C73940  ouverture d'une vidéo par le jeu (this = ecx, [esp+4] = nom)  → nom retenu pour la session
 *   0x01093B70  theora_decode_init(ecx = theora_state*, edx = theora_info*)   → session (mp4 frère trouvé ?)
 *   0x01093C60  theora_decode_packetin(ecx = état, edx = ogg_packet*)         → en mode matériel : le paquet passe
 *               par la branche « image dupliquée » (0 octet) du vrai décodeur : granulepos tenu, AUCUN décodage
 *   0x01093CB0  theora_decode_YUVout(ecx = état, edx = yuv_buffer*)           → plans remplis par le décodeur matériel
 *   0x01093810  theora_clear(ecx = état)                                      → session fermée, bilan
 *
 * Le son et l'horloge restent ceux du jeu (vorbis du .ogv, DSP FMOD) : seule l'IMAGE vient du .mp4.
 *
 * Knobs (sc2.env ; sans SC2_VIDEO_AMC=1, chaque accroche appelle l'original et rien d'autre) :
 *   SC2_VIDEO_AMC=1          active ; .mp4 frère trouvé → décodage matériel, sinon Theora lifté inchangé
 *   SC2_VIDEO_AMC_DIR=<dir>  racine des .mp4 (défaut $SC2_INSTALL, puis $SC2_ROOT) : <dir>/<chemin interne>.mp4
 *                            puis <dir>/<nom>.mp4 (« \ » du jeu → « / », extension .ogv → .mp4)
 *   SC2_VIDEO_AMC_MIRE=1     banc sans .mp4 ni AMediaCodec : mire calculée (valide l'accroche sur x86)
 *   SC2_VIDEO_AMC_VOIE=tampon  sortie du décodeur en ByteBuffer au lieu de l'AImageReader YUV_420_888 (défaut,
 *                            repli automatique sur tampon si le lecteur échoue)
 *   SC2_VIDEO_SAUT=0         désactive le saut par toucher (défaut 1 quand SC2_VIDEO_AMC=1) ; le saut = VK_ESCAPE
 *                            injectée par l'API d'entrées du runtime (geste natif du jeu, qui ferme la cinématique)
 *   SC2_VIDEO_SAUT_MIN=<px>  largeur minimale d'une vidéo « plein écran » sautable (défaut 1024 : pas les portraits)
 *   SC2_VIDEO_SECOURS=<s>    sortie de secours : au-delà de <s> s de lecture, Échap injectée (relancée toutes les 5 s
 *                            tant que la vidéo vit ; défaut : en mode matériel 2 × durée du .mp4 + 30 s ; en mode
 *                            Theora, seulement si la valeur est posée)
 */
#ifndef RECOMP_SC2_VIDEO_AMC_H
#define RECOMP_SC2_VIDEO_AMC_H

#include <stddef.h>

/* Toucher reçu par la plate-forme : si une vidéo plein écran est EN LECTURE (paquet lu il y a moins de 0,5 s), injecte
 * VK_ESCAPE et rend 1 — l'appelant garde alors ce toucher pour lui (sinon le clic tomberait sur l'écran suivant).
 * 0 sinon (knob absent, rien en lecture) : le toucher suit son chemin habituel. La touche retour donne déjà Échap. */
int  video_amc_entree(void);
/* 1 si une vidéo plein écran est en lecture. */
int  video_amc_en_lecture(void);
/* Une ligne de bilan (sessions ouvertes, images matérielles, sauts). */
void video_amc_bilan(char *o, size_t cap);

#endif
