# V3, lot M2 (22/10) : clavier et souris macOS en C pur (mac/sc2_clavier_mac.c, mac/sc2_souris_mac.c).
# Fragment inclus par le bloc final de tests-hote/CMakeLists.txt (M0 : glob de tests-hote/v3/*.cmake) ; sc2_test et
# RACINE viennent de ce fichier. Sans AppKit ni Carbon à la compilation : les deux tests tournent aussi hors macOS
# (sc2m_iso* y rend 0). test_souris_mac inclut android/jni/sc2_geo.h par chemin relatif (aller-retour vue → client).
sc2_test(test_clavier_mac SOURCES "${RACINE}/mac/tests/test_clavier_mac.c" "${RACINE}/mac/sc2_clavier_mac.c"
         INCLUDES "${RACINE}/mac" LIBS ${CMAKE_DL_LIBS} OPTIONS -Wall -Wextra)
sc2_test(test_souris_mac  SOURCES "${RACINE}/mac/tests/test_souris_mac.c" "${RACINE}/mac/sc2_souris_mac.c"
         INCLUDES "${RACINE}/mac" LIBS m OPTIONS -Wall -Wextra)
