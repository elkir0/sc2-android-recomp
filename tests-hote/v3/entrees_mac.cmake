# tests-hote/v3/entrees_mac.cmake — V3, lot M8 (24/10) : couche d'entrées Cocoa (mac/sc2_entrees_mac.m) contre le
# vrai M2 (mac/sc2_clavier_mac.c + mac/sc2_souris_mac.c) et des bouchons de l'hôte (mac/tests/test_entrees_mac.m) :
# conversion de coordonnées, Ctrl+clic mémorisé, rejeu du keyUp, caractères (Cmd → Ctrl+A, Ctrl physique muet),
# AZERTY, F10, flagsChanged, molette par axe, abandon sans géométrie, bilan sans balise.
# Fragment inclus par le bloc final de tests-hote/CMakeLists.txt (M0) : MAC, JNI, RT et sc2_test() viennent de là.
# Mac seulement ; SKIP (77) sans session graphique.
if(APPLE)
    sc2_test(test_entrees_mac
             SOURCES "${MAC}/tests/test_entrees_mac.m" "${MAC}/sc2_entrees_mac.m" "${MAC}/sc2_clavier_mac.c"
                     "${MAC}/sc2_souris_mac.c"
             INCLUDES "${MAC}" "${JNI}" "${RT}"
             LIBS m "-framework AppKit" "-framework CoreGraphics"
             OPTIONS -Wall $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
    set_tests_properties(test_entrees_mac PROPERTIES SKIP_RETURN_CODE 77)
endif()
