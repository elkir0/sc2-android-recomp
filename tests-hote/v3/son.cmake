# V3, lot M6 (23/10/2026) : anneau SPSC et purge du son Mac (branche __APPLE__ de android/jni/sc2_audio_android.c).
# Fragment inclus par le bloc final de tests-hote/CMakeLists.txt (M0 : glob de tests-hote/v3/*.cmake) ; sc2_test,
# RACINE, RT et JNI viennent de là. Le test INCLUT le .c (même unité : ses static — produire, consommer, purger,
# rappel_mac — deviennent atteignables) mais ne crée JAMAIS d'unité CoreAudio : pure arithmétique d'anneau.
# test_anneau_bogue compile le même test avec -DSC2_ANNEAU_BOGUE=1 (producteur qui publie l'index AVANT la copie,
# bogue injecté exigé par le lot) : ce binaire doit ÉCHOUER — d'où le test à sortie inversée (sh -c '!').
if(APPLE)
    sc2_test(test_son_anneau
             SOURCES "${RACINE}/mac/tests/test_son_anneau.c" "${RT}/sc2_audio_sortie.c"
             INCLUDES "${JNI}" "${RT}"
             LIBS m "-framework AudioToolbox" "-framework CoreAudio"
             OPTIONS -Wall -Wextra -Wno-unused-function)
    sc2_test(test_anneau_bogue SANS_TEST
             SOURCES "${RACINE}/mac/tests/test_son_anneau.c" "${RT}/sc2_audio_sortie.c"
             INCLUDES "${JNI}" "${RT}"
             LIBS m "-framework AudioToolbox" "-framework CoreAudio"
             OPTIONS -Wall -Wextra -Wno-unused-function -DSC2_ANNEAU_BOGUE=1)
    add_test(NAME test_anneau_bogue COMMAND sh -c "! $<TARGET_FILE:test_anneau_bogue>")
    set_tests_properties(test_anneau_bogue PROPERTIES TIMEOUT 60)
endif()
