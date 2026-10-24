# tests-hote/v3/v2-5a-ddraw.cmake — V2.5a (23/10/2026) : décisions de l'IDirectDraw7 factice (recomp-sc2/runtime/
# sc2_ddraw.h, appelé par sc2_iat_p4.c) : garde du canon (historique → DirectDraw refusé), garde-fou Android, SC2_DDRAW,
# SC2_VRAM_MO, octets de GetAvailableVidMem et paliers de textures observés au banc. Chantier de la vague 2 rangé ici
# pour profiter du glob des fragments (bloc final de tests-hote/CMakeLists.txt) sans éditer le fichier commun.
# Logique pure, en-tête seul : toutes plateformes.
sc2_test(test_ddraw SOURCES "${TR}/test_ddraw.c" INCLUDES "${RT}" OPTIONS -Wall -Wextra)
