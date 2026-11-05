# tests-hote/v3/plateforme.cmake — V3, lot M7 (24/10) : ordre du crochet de Present de la couche plate-forme
# (android/jni/sc2_android_hooks.c compilée TELLE QUELLE, renommages de mac/mac.cmake, SC2_SWAPCHAIN=1) contre des
# bouchons (mac/tests/test_plateforme_ordre.c). Fragment inclus par le bloc final de tests-hote/CMakeLists.txt (M0, glob
# de tests-hote/v3/*.cmake) : RACINE, RT, JNI et sc2_test() viennent de là. POSIX seulement, mais la couche n'exclut ses
# l. 38-108 (mmap, dlopen) que sous __APPLE__ : test Mac. test_plateforme_ordre_temoin rejoue le comparateur sur un
# ordre faux injecté : il doit ÉCHOUER (test à sortie inversée).
if(APPLE)
    set(PLATEFORME_DEFS SC2_SWAPCHAIN=1 __wrap_d3dv_present=d3dv_present __real_d3dv_present=d3dv_present_reel
        __wrap_d3dv_cible=d3dv_cible __real_d3dv_cible=d3dv_cible_reel)
    add_library(pm7_plateforme OBJECT "${JNI}/sc2_android_hooks.c")
    target_include_directories(pm7_plateforme PRIVATE "${JNI}" "${RT}")
    target_compile_definitions(pm7_plateforme PRIVATE ${PLATEFORME_DEFS})
    target_compile_options(pm7_plateforme PRIVATE -Wall)
    sc2_test(test_plateforme_ordre
             SOURCES "${RACINE}/mac/tests/test_plateforme_ordre.c" $<TARGET_OBJECTS:pm7_plateforme>
             INCLUDES "${JNI}" "${RT}" LIBS Threads::Threads OPTIONS -Wall -Wextra)
    # témoin : le binaire doit exister, rendre ≠ 0 ET nommer l'échec de l'ordre faux (pas un plantage quelconque)
    add_test(NAME test_plateforme_ordre_temoin COMMAND sh -c
             "o=$(SC2_ORDRE_TEMOIN=1 \"$0\"); r=$?; echo \"$o\"; [ $r -ne 0 ] && echo \"$o\" | grep -q '^FAIL  TÉMOIN'"
             $<TARGET_FILE:test_plateforme_ordre>)
    set_tests_properties(test_plateforme_ordre_temoin PROPERTIES TIMEOUT 60)
endif()
