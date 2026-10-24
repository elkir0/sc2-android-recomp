# tests-hote/v3/rejeu_ns.cmake — V3, lot M7 (24/10) : synthèse AppKit du rejeu SC2_MAC_EVENEMENTS en VOIE directe
# (mac/sc2_mac_evenements_ns.m + le cœur de M4, mac/sc2_mac_evenements.c) contre des bouchons de l'hôte
# (mac/tests/test_rejeu_ns.m) : types, boutons, points de vue, clavier, molette, modificateurs, horodatages-marqueurs.
# Fragment inclus par le bloc final de tests-hote/CMakeLists.txt (M0) : MAC, JNI, RT et sc2_test() viennent de là.
# Mac seulement ; SKIP (77) sans session graphique.
if(APPLE)
    sc2_test(test_rejeu_ns
             SOURCES "${MAC}/tests/test_rejeu_ns.m" "${MAC}/sc2_mac_evenements_ns.m" "${MAC}/sc2_mac_evenements.c"
             INCLUDES "${MAC}" "${JNI}" "${RT}"
             LIBS m "-framework AppKit" "-framework CoreGraphics"
             OPTIONS -Wall $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)
    set_tests_properties(test_rejeu_ns PROPERTIES SKIP_RETURN_CODE 77)
endif()
