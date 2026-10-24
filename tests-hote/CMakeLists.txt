# =============================================================================
# tests-hote — tests hôtes de SC2 Partout, SANS le code lifté (V0.2, 22/10)
#
# Projet INDÉPENDANT de recomp-sc2/CMakeLists.txt (qui reste le build du jeu) : il ne
# compile que le runtime écrit à la main et les en-têtes « pur calcul » d'Android, en -O0,
# pour la porte de vérification scripts/verifier.sh (< 2 min, cache chaud ou froid) :
#   cmake -S tests-hote -B build-tests -G Ninja && cmake --build build-tests -j8
#   ctest --test-dir build-tests --output-on-failure
# Sources référencées par chemin relatif à la racine du dépôt (aucune copie). Ni StormLib,
# ni Theora, ni lift, ni sc2_shaders_spv.h : rien de privé n'est lu.
#
# Repris de recomp-sc2/CMakeLists.txt (mêmes ordres de compilation) : test_win32_compat,
# cpu_selftest, test_d3d9v (SKIP 77 sans Vulkan), test_documents, test_spv_pack,
# test_entrees_banc, test_attentes. Orphelins jusqu'ici (recette « cc » en tête de fichier) :
# test_d3dx_surface, test_horloge_9j, test_gestes, test_barre, test_entrees_8h, plus le
# nouveau test_geo (aller-retour sc2_geo ↔ écran, barre de raccourcis, recentrage).
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(tests-hote LANGUAGES C)

set(CMAKE_C_STANDARD 17)
set(CMAKE_C_EXTENSIONS ON)                      # gnu17 : recettes des tests Android (-std=gnu17)
# -O0 imposé (porte rapide, pas de mesure) : la configuration ne dépend pas de CMAKE_BUILD_TYPE.
set(CMAKE_C_FLAGS_DEBUG "")
set(CMAKE_C_FLAGS_RELEASE "")
add_compile_options(-O0 -g)

get_filename_component(RACINE "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(RT "${RACINE}/runtime")             # privé : recomp-sc2/runtime
set(TR "${RACINE}/runtime/tests")       # privé : recomp-sc2/tests
set(JNI "${RACINE}/android/jni")

find_package(Threads REQUIRED)
enable_testing()

# --- en-têtes Vulkan (dlopen à l'exécution : seuls les en-têtes sont requis) --------------------
# macOS : Homebrew (vulkan-headers) ou SDK LunarG, comme recomp-sc2 ; Linux : /usr/include.
find_path(VK_INC vulkan/vulkan.h HINTS /opt/homebrew/include $ENV{VULKAN_SDK}/include /usr/include)

# sc2_d3d9v.c appelle SC2_ATTENTES par références FAIBLES (module absent de ces tests) ; Mach-O refuse un symbole
# faible non défini à l'édition des liens : -U les laisse se résoudre à NULL à l'exécution (cf. recomp-sc2, 21/10).
function(sc2_faibles_attentes cible)
    if(APPLE)
        target_link_options(${cible} PRIVATE "LINKER:-U,_sc2_att_debut" "LINKER:-U,_sc2_att_fin" "LINKER:-U,_sc2_att_present")
    endif()
endfunction()

# Test hôte d'une seule unité (+ sources) : nom, includes, sources ; sortie 0 = conforme.
function(sc2_test nom)
    cmake_parse_arguments(T "SANS_TEST" "" "SOURCES;INCLUDES;LIBS;ARGS;OPTIONS" ${ARGN})
    add_executable(${nom} ${T_SOURCES})
    target_include_directories(${nom} PRIVATE ${T_INCLUDES})
    target_compile_options(${nom} PRIVATE ${T_OPTIONS})
    target_link_libraries(${nom} PRIVATE ${T_LIBS})
    if(NOT T_SANS_TEST)
        add_test(NAME ${nom} COMMAND ${nom} ${T_ARGS})
        set_tests_properties(${nom} PROPERTIES TIMEOUT 60)
    endif()
endfunction()

# =============================================================================
# recomp-sc2 : socle Win32, CPU, backend Vulkan, documents, pack SPIR-V, entrées, attentes
# =============================================================================
add_library(sc2_win32_hote STATIC "${RT}/win32_compat.c" "${RT}/stubs_sc2.c")
target_include_directories(sc2_win32_hote PUBLIC "${RT}")
target_compile_options(sc2_win32_hote PRIVATE -Wall -Wextra $<$<PLATFORM_ID:Darwin>:-Wno-deprecated-declarations>)   # ucontext : déprécié sur macOS, voulu
target_link_libraries(sc2_win32_hote PUBLIC Threads::Threads)

if(NOT APPLE)
    sc2_test(test_win32_compat SOURCES "${TR}/test_win32_compat.c" LIBS sc2_win32_hote OPTIONS -Wall -Wextra)
else()
    # macOS : 4 échecs CONNUS, nommés ici un par un → SKIP (77) par echecs-connus.sh ; sans échec le test PASSE ; tout
    # AUTRE échec reste un ÉCHEC, de même qu'un plantage (signal, abort : code ≠ 0 et 1) ou un bilan « RÉSULTAT » absent
    # ou incohérent (plantage après les échecs connus).
    #  - non applicables : « pile de la fibre sous 4 Go », « vue sous 4 Go » — __PAGEZERO couvre les 4 premiers Go sur
    #    arm64, aucune allocation hôte n'y tombe ; le runtime donne aux fibres une pile invitée dans l'arène
    #    (h_CreateFiberEx) et reloge la vue (h_MapViewOfFile, sc2_iat.c) ;
    #  - BOGUE du runtime (22/10, à corriger dans stubs_sc2.c puis à retirer d'ici) : « auto-reset : SetEvent … » et
    #    « le signal a été consommé » — WaitForSingleObject calcule l'échéance en CLOCK_MONOTONIC alors que la condvar,
    #    sans pthread_condattr_setclock sur Apple, attend en CLOCK_REALTIME : toute attente bornée expire aussitôt.
    set(W32_CONNUS
        "pile de la fibre sous 4 Go" "vue sous 4 Go"
        "auto-reset : SetEvent depuis un autre thread réveille Wait(2000)" "auto-reset : le signal a été consommé")
    sc2_test(test_win32_compat SANS_TEST SOURCES "${TR}/test_win32_compat.c" LIBS sc2_win32_hote OPTIONS -Wall -Wextra)
    add_test(NAME test_win32_compat COMMAND sh "${CMAKE_CURRENT_SOURCE_DIR}/echecs-connus.sh" $<TARGET_FILE:test_win32_compat>
        "macOS — sous 4 Go non applicables ; auto-reset = bogue stubs_sc2.c (CLOCK_MONOTONIC)" ${W32_CONNUS})
    set_tests_properties(test_win32_compat PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()
sc2_test(cpu_selftest      SOURCES "${RT}/cpu_selftest.c" INCLUDES "${RT}")
sc2_test(test_documents    SOURCES "${TR}/test_documents.c" "${RT}/sc2_documents.c" INCLUDES "${RT}" OPTIONS -Wall -Wextra)
sc2_test(test_spv_pack     SOURCES "${TR}/test_spv_pack.c" "${RT}/sc2_spv_pack.c" INCLUDES "${RT}"
         LIBS Threads::Threads OPTIONS -Wall -Wextra)
# V1.1 : un test fait des rappels depuis un second fil (boîte aux lettres des déclencheurs) ; la copie verbatim du
# format 1 et les scénarios de scripts/entrees/ sont trouvés par __FILE__ (ou SC2_DEPOT_TEST).
sc2_test(test_entrees_banc SOURCES "${TR}/test_entrees_banc.c" LIBS Threads::Threads OPTIONS -Wall -Wextra)
sc2_test(test_attentes     SOURCES "${TR}/test_attentes.c" INCLUDES "${RT}" LIBS Threads::Threads OPTIONS -Wall)
# Passe 9j : horloge GetTickCount / MSG.time (en-tête seul)
sc2_test(test_horloge_9j   SOURCES "${TR}/test_horloge_9j.c" INCLUDES "${RT}" OPTIONS -Wall -Wextra)
# V2.2 : plan d'instanciation (sc2_d3d9_inst.h, en-tête pur) contre une référence D3D9, 200 000 cas tirés
sc2_test(test_instanciation SOURCES "${TR}/test_instanciation.c" INCLUDES "${RT}" OPTIONS -Wall -Wextra)
# V5.1 (24/10) : file de libération des ressources D3D9 (runtime/sc2_d3d9_file.h), pure
sc2_test(test_d3d9_file SOURCES "${TR}/test_d3d9_file.c" INCLUDES "${RT}" OPTIONS -Wall -Wextra)

if(VK_INC)
    # Backend D3D9 → Vulkan : MoltenVK (ou loader) présent → vrai rendu ; absent, ou SC2_VK_LIB vers un chemin
    # inexistant → SKIP (77). Le test écrit test_d3d9v.ppm dans le dossier de build.
    sc2_test(test_d3d9v SOURCES "${TR}/test_d3d9v.c" "${RT}/sc2_d3d9v.c" INCLUDES "${RT}" "${VK_INC}"
             LIBS Threads::Threads ${CMAKE_DL_LIBS} OPTIONS -Wall -Wextra)
    sc2_faibles_attentes(test_d3d9v)
    set_tests_properties(test_d3d9v PROPERTIES SKIP_RETURN_CODE 77)
    # Passe 9h : D3DXLoadSurfaceFromMemory (sc2_d3dx_surface.h, lié au backend pour ses conversions)
    sc2_test(test_d3dx_surface SOURCES "${TR}/test_d3dx_surface.c" "${RT}/sc2_d3d9v.c" INCLUDES "${RT}" "${VK_INC}"
             LIBS Threads::Threads ${CMAKE_DL_LIBS} OPTIONS -Wall -Wextra)
    sc2_faibles_attentes(test_d3dx_surface)
else()
    # Sans en-têtes Vulkan, les deux tests ne se compilent pas : SKIP déclaré (77), jamais un vert silencieux.
    message(STATUS "vulkan/vulkan.h introuvable : test_d3d9v et test_d3dx_surface en SKIP (brew install vulkan-headers)")
    foreach(t test_d3d9v test_d3dx_surface)
        add_test(NAME ${t} COMMAND sh -c "echo 'SKIP  en-têtes Vulkan absents à la configuration'; exit 77")
        set_tests_properties(${t} PROPERTIES SKIP_RETURN_CODE 77)
    endforeach()
endif()

# =============================================================================
# android/jni : gestes tactiles, barre de raccourcis, géométrie, entrées physiques (pur calcul)
# =============================================================================
sc2_test(test_gestes     SOURCES "${JNI}/tests/test_gestes.c" INCLUDES "${JNI}" OPTIONS -Wall -Wextra)
# 23/10 : règle de sauvegarde du Variables.txt de /sdcard (runs de test jamais recopiés dessus)
sc2_test(test_vars_regle SOURCES "${JNI}/tests/test_vars_regle.c" INCLUDES "${JNI}" OPTIONS -Wall -Wextra)
# Les deux PPM de contrôle (barre-fold.ppm, barre-lettres.ppm) vont dans le dossier de build, jamais dans le dépôt.
sc2_test(test_barre      SOURCES "${JNI}/tests/test_barre.c" INCLUDES "${JNI}" OPTIONS -Wall -Wextra
         ARGS "${CMAKE_CURRENT_BINARY_DIR}")
sc2_test(test_geo        SOURCES "${JNI}/tests/test_geo.c" INCLUDES "${JNI}" LIBS m OPTIONS -Wall -Wextra)
# Passe 8h : souris + clavier Android → glue sc2a_* (inclut ../jni/sc2_entrees_bt.c)
sc2_test(test_entrees_8h SOURCES "${RACINE}/android/outils/test-entrees-8h.c" INCLUDES "${JNI}" OPTIONS -Wall)

# =============================================================================
# V3 (22/10, lot M0) : fragments de tests des lots de la vague 3 — un fichier tests-hote/v3/<lot>.cmake par lot, jamais
# d'édition croisée de ce fichier. Chaque fragment appelle sc2_test(…) (plus haut) et se garde lui-même (if(APPLE)
# pour un test Mac) ; un test qui exige MoltenVK ou une session graphique rend 77 (SKIP) sans eux. Visibles : RACINE,
# RT, TR, JNI, VK_INC, sc2_faibles_attentes(), et MAC (mac/ du dépôt). OBJC activé sur macOS pour les tests en .m.
# =============================================================================
if(APPLE)
    enable_language(OBJC)
    set(CMAKE_OBJC_FLAGS_DEBUG "")
    set(CMAKE_OBJC_FLAGS_RELEASE "")
endif()
set(MAC "${RACINE}/mac")
file(GLOB SC2_FRAGMENTS_V3 CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/v3/*.cmake")
list(SORT SC2_FRAGMENTS_V3)
foreach(f ${SC2_FRAGMENTS_V3})
    include(${f})
endforeach()
