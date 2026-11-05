# =============================================================================
# android/CMakeLists.txt — libSC2Partout.so : recomp-sc2 pour aarch64-linux-android (passe 7a)
#
#   cmake -S android -B android/build -DCMAKE_TOOLCHAIN_FILE=$NDK/build/cmake/android.toolchain.cmake \
#         -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static
#   (scripts/build-apk.sh le fait)
#
# Les sources du POC sont prises TELLES QUELLES dans ../recomp-sc2 (runtime/, src/main.c,
# src/lifted/) : aucune n'est modifiée. Les écarts Android passent par :
#   - jni/compat/sc2_android_compat.h (-include) + sc2_ucontext* : getcontext/makecontext/
#     swapcontext absents de bionic (fibres de win32_compat.c) ;
#   - -Dmain=sc2_poc_main sur src/main.c : le main() du POC devient une fonction de la .so ;
#   - --wrap au lien (jni/sc2_android_hooks.c) : mmap (zone invitée décalée, le tas d'ART occupe
#     le bas des 4 Go), dlopen (libvulkan.so), d3dv_present (frames → écran, sortie propre) ;
#   - entrées (passe 7d) : runtime/sc2_input_sim.c lié, le tactile y est injecté par son API
#     (sc2_input.h) — plus de --wrap=sc2_iat_p4_find ;
#   - passe 9s : --wrap=L_<va> de 5 fonctions liftées (libtheora de SC2.exe) → runtime/video_amc.c,
#     cinématiques H.264 par AMediaCodec derrière SC2_VIDEO_AMC=1 (option SC2_VIDEO_AMC ci-dessous).
# Écarts au CMake Linux (recomp-sc2/CMakeLists.txt), imposés par une .so Android :
#   - PIC partout, pas de -no-pie ni de -Ttext-segment (le chargeur place la .so où il veut ;
#     le modèle mémoire invité passe par g_hote_delta) ;
#   - pas de -z stacksize : le fil du jeu est créé avec 16 Mo de pile (la fibre de boot en a 64).
# =============================================================================

cmake_minimum_required(VERSION 3.24)   # V0.3 : $<LINK_LIBRARY:WHOLE_ARCHIVE,…> (3.24)
project(sc2-partout-android VERSION 0.7.0 LANGUAGES C CXX ASM)

set(CMAKE_C_STANDARD 17)
set(R ${CMAKE_CURRENT_SOURCE_DIR}/../recomp-sc2)
set(J ${CMAKE_CURRENT_SOURCE_DIR}/jni)

if(NOT ANDROID)
    message(FATAL_ERROR "android/CMakeLists.txt se configure avec le toolchain du NDK (scripts/build-apk.sh)")
endif()

# Sémantique x86 de `char` (celle du binaire d'origine), comme le build ARM64 Linux.
add_compile_options(-fsigned-char)

# ---- sources du POC (inchangées) ---------------------------------------------------------
set(RUNTIME_SOURCES
    ${R}/src/main.c
    ${R}/runtime/image_base.c
    ${R}/runtime/sc2_runtime.c
    ${R}/runtime/sc2_iat.c
    ${R}/runtime/sc2_mpq.c
    ${R}/runtime/sc2_iat_p4.c
    ${R}/runtime/sc2_d3d9.c
    ${R}/runtime/sc2_d3d9v.c
    ${R}/runtime/sc2_spv_pack.c     # passe 10t : pack SPIR-V externe sc2-spirv.pak (mmap), consulté si la table embarquée manque un shader
    ${R}/runtime/win32_compat.c
    ${R}/runtime/stubs_sc2.c
    ${R}/runtime/sc2_input_sim.c     # passe 7d : entrées (API 5f) — le tactile y est injecté
    ${R}/runtime/sc2_documents.c     # passe 10t : Banks/ et Saves/ persistants (SC2_DOCUMENTS)
    ${R}/runtime/sc2_escarmouche.c   # V4.10 : escarmouche contre l'IA (SC2_ESCARMOUCHE), carte mêlée dérivée (StormLib)
    # passe 7e : FMOD Ex logiciel du runtime (5e/5g) — sans lui, le jeu reboucle sur
    # FMOD_System_Create → FMOD_ERR_OUTPUT_INIT (logcat émulateur M4, retest 7c)
    ${R}/runtime/sc2_audio_fmod.c
    ${R}/runtime/sc2_audio_mix.c
    ${R}/runtime/sc2_audio_stb_vorbis.c
    ${R}/runtime/sc2_audio_mpq.c
    ${R}/runtime/sc2_audio_sortie.c
    ${R}/runtime/sc2_audio_sortie_compteur.c
)
# passe 7x2 : vidéos .ogv (runtime/sc2_video.c, 7w3) — libogg + libtheoradec statiques aarch64 construites HORS
# dépôt par scripts/theora-android.sh (~/tools/theora/android-arm64). Absentes : sc2_video.c n'est pas compilé
# (même règle que le build Linux) et l'APK est celui d'avant la 7w3.
set(THEORA_ANDROID "$ENV{HOME}/tools/theora/android-arm64" CACHE PATH "libogg.a + libtheoradec.a aarch64 (scripts/theora-android.sh)")
if(EXISTS "${THEORA_ANDROID}/lib/libtheoradec.a" AND EXISTS "${THEORA_ANDROID}/lib/libogg.a")
    set(SC2_VIDEO_OGV ON)
    list(APPEND RUNTIME_SOURCES ${R}/runtime/sc2_video.c)
    message(STATUS "vidéo .ogv (passe 7x2) : sc2_video.c + libtheoradec/libogg de ${THEORA_ANDROID}")
else()
    set(SC2_VIDEO_OGV OFF)
    message(WARNING "vidéo .ogv (passe 7x2) : ${THEORA_ANDROID}/lib/lib{theoradec,ogg}.a absentes (scripts/theora-android.sh) — sc2_video.c non compilé")
endif()
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/cpu_delta.cmake)
sc2_cpu_delta(${R} CPU_DELTA)

# Passe 10r : réécritures optionnelles du lift dans le dossier de build (SC2_APPELS_DIRECTS…), cmake/passes_lift.cmake.
# V0.3 : objets du lift dans la bibliothèque statique sc2_lifted (${R}/cmake/lift_drapeaux.cmake : SC2_LIFTED_OPT, -O0 par
# défaut comme le build Linux ; -include sc2_lifted.h puis cpu.h delta-propre ; -fsigned-char ; PIC), compilée ici ou
# prise dans le cache Android partagé (-DSC2_LIFTED_PREBUILT=<entrée>/libsc2_lifted.a : scripts/cache-lift.sh construire
# android, que android/build-apk-mac.sh consomme), contrôlée par scripts/garde-lift.sh cache.
include(${R}/cmake/lift_objets.cmake)
sc2_lift_objets(${R} ${CMAKE_CURRENT_SOURCE_DIR}/.. PLATEFORME android CPU_DELTA ${CPU_DELTA})
# Runtime écrit à la main : bionic sans ucontext complet → déclarations forcées.
# -O2 explicite (le build Linux de référence est à -O0 ; -O3 de Release est évité).
set(SC2_RUNTIME_OPT "-O2" CACHE STRING "Optimisation du runtime écrit à la main")
set_source_files_properties(${RUNTIME_SOURCES} PROPERTIES COMPILE_OPTIONS
    "${SC2_RUNTIME_OPT};-include;${J}/compat/sc2_android_compat.h;-include;${R}/runtime/sc2_lifted.h;-include;${CPU_DELTA};-Wno-unused-parameter")
set_source_files_properties(${R}/src/main.c PROPERTIES COMPILE_DEFINITIONS "main=sc2_poc_main")

# ---- StormLib (comme en passe 6a : construit ici pour la cible, dépendances embarquées) -----
set(STORMLIB_DIR "$ENV{HOME}/tools/StormLib" CACHE PATH "Checkout StormLib")
set(STORM_USE_BUNDLED_LIBRARIES ON CACHE BOOL "" FORCE)
set(STORM_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(STORM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
add_subdirectory(${STORMLIB_DIR} stormlib EXCLUDE_FROM_ALL)
target_compile_options(storm PRIVATE -w)
set_property(SOURCE ${R}/runtime/sc2_mpq.c ${R}/runtime/sc2_audio_mpq.c ${R}/runtime/sc2_video.c ${R}/runtime/sc2_escarmouche.c
             APPEND PROPERTY INCLUDE_DIRECTORIES "${STORMLIB_DIR}/src")
# décodeur Ogg Vorbis tiers (stb_vorbis) : avertissements coupés, comme dans le build Linux
set_property(SOURCE ${R}/runtime/sc2_audio_stb_vorbis.c APPEND PROPERTY COMPILE_OPTIONS "-w")

# ---- glue NativeActivity du NDK ------------------------------------------------------------
add_library(native_app_glue STATIC ${ANDROID_NDK}/sources/android/native_app_glue/android_native_app_glue.c)
target_include_directories(native_app_glue PUBLIC ${ANDROID_NDK}/sources/android/native_app_glue)
target_compile_options(native_app_glue PRIVATE -w)

# ---- libSC2Partout.so ----------------------------------------------------------------------
add_library(SC2Partout SHARED
    ${J}/sc2_android_main.c
    ${J}/sc2_android_hooks.c ${J}/sc2_trace_present.c ${J}/sc2_entrees_bt.c
    ${J}/sc2_ucontext.c
    ${J}/sc2_ucontext_aarch64.S
    ${J}/sc2_audio_android.c         # passe 7e : sortie AAudio (anneau + rappel)
    ${RUNTIME_SOURCES}
)
# src/shaders : table SPIR-V des shaders du jeu (passe 5d, générée hors git) ; absente → draws « lumière ».
target_include_directories(SC2Partout PRIVATE ${J} ${R}/src ${R}/runtime ${R}/src/lifted ${R}/src/shaders)
if(NOT EXISTS "${R}/src/shaders/sc2_shaders_spv.h")
    message(WARNING "${R}/src/shaders/sc2_shaders_spv.h absente : l'APK n'aura que les draws « lumière » (cf. src/shaders/README.md)")
endif()
target_compile_options(SC2Partout PRIVATE $<$<COMPILE_LANGUAGE:C>:-Wall>)
# V0.3 : archive du lift liée EN ENTIER (--whole-archive : mêmes objets qu'avant, dans l'ordre des sources)
target_link_libraries(SC2Partout PRIVATE "$<LINK_LIBRARY:WHOLE_ARCHIVE,sc2_lifted>")
target_link_libraries(SC2Partout PRIVATE native_app_glue StormLib::storm android log dl m aaudio)   # aaudio : API 26+
if(SC2_VIDEO_OGV)   # passe 7x2 : ordre theoradec puis ogg (theoradec appelle oggpack_*)
    target_include_directories(SC2Partout PRIVATE ${THEORA_ANDROID}/include)
    target_link_libraries(SC2Partout PRIVATE ${THEORA_ANDROID}/lib/libtheoradec.a ${THEORA_ANDROID}/lib/libogg.a)
endif()
target_link_options(SC2Partout PRIVATE
    -u ANativeActivity_onCreate                    # point d'entrée de la glue (sinon éliminé)
    -Wl,--wrap=mmap
    -Wl,--wrap=dlopen
    -Wl,--wrap=d3dv_present
    -Wl,--wrap=d3dv_cible                          # passe 9d : taille des cibles (relecture CPU bornée)
    -Wl,-z,max-page-size=16384                     # appareils à pages de 16 Ko (Android 15+)
    -Wl,--no-undefined
)
# V0.3 : édition de liens de la .so sous le verrou edl (2 jetons, scripts/verrou.sh), recomp-sc2/cmake/verrous.cmake.
include(${R}/cmake/verrous.cmake)
sc2_verrou_edl(${CMAKE_CURRENT_SOURCE_DIR}/.. SC2Partout)

# ---- passe 9s : cinématiques H.264 matérielles (runtime/video_amc.c, AMediaCodec), option ---------------------
# ON par défaut : compilé et lié, INACTIF tant que sc2.env ne pose pas SC2_VIDEO_AMC=1 (chaque accroche appelle alors
# l'original, rien d'autre). Exige que le lift contienne les 5 fonctions visées (libtheora 1.0 de SC2.exe, toujours
# présentes dans la clôture du menu) ; OFF : build d'avant la 9s.
option(SC2_VIDEO_AMC "Cinématiques H.264 décodées par AMediaCodec (SC2_VIDEO_AMC=1)" ON)
if(SC2_VIDEO_AMC)
    set(VIDEO_AMC_FONCTIONS L_00C73940 L_01093B70 L_01093C60 L_01093CB0 L_01093810)
    foreach(f ${VIDEO_AMC_FONCTIONS})
        # SC2_LIFTED_SOURCES (cmake/lift_objets.cmake) : lift transformé, ou canonique en mode précompilé
        execute_process(COMMAND grep -lq "^void ${f}(CPU [*]c)" ${SC2_LIFTED_SOURCES} RESULT_VARIABLE r OUTPUT_QUIET ERROR_QUIET)
        if(NOT r EQUAL 0)
            message(FATAL_ERROR "SC2_VIDEO_AMC : ${f} absente du lift (${R}/src/lifted) — relifter ou -DSC2_VIDEO_AMC=OFF")
        endif()
        target_link_options(SC2Partout PRIVATE -Wl,--wrap=${f})
    endforeach()
    target_sources(SC2Partout PRIVATE ${R}/runtime/video_amc.c)
    set_source_files_properties(${R}/runtime/video_amc.c PROPERTIES COMPILE_OPTIONS
        "${SC2_RUNTIME_OPT};-include;${J}/compat/sc2_android_compat.h;-include;${R}/runtime/sc2_lifted.h;-include;${CPU_DELTA};-Wno-unused-parameter")
    target_link_libraries(SC2Partout PRIVATE mediandk)
    message(STATUS "SC2_VIDEO_AMC=ON : video_amc.c + --wrap ${VIDEO_AMC_FONCTIONS} (actif avec SC2_VIDEO_AMC=1)")
endif()

# ---- passe 7b : présentation GPU par swapchain (jni/sc2_vk_presente.c), option ----------------
# OFF par défaut : la copie CPU (7a) reste le chemin de l'APK tant que la swapchain n'est pas
# validée sur le Fold 8. ON : compilée, puis activée à l'exécution par SC2_SWAPCHAIN=1.
option(SC2_SWAPCHAIN "Présentation par swapchain Vulkan (VK_KHR_android_surface ; banc : VK_KHR_xcb_surface sur Xvfb)" OFF)
if(SC2_SWAPCHAIN)
    target_sources(SC2Partout PRIVATE ${J}/sc2_vk_presente.c)
    set_source_files_properties(${J}/sc2_android_hooks.c ${J}/sc2_vk_presente.c
        ${J}/sc2_android_main.c
        PROPERTIES COMPILE_DEFINITIONS SC2_SWAPCHAIN=1)
    target_link_options(SC2Partout PRIVATE -Wl,--wrap=dlsym)   # --wrap=d3dv_cible : toujours posé (passe 9d)
    message(STATUS "SC2_SWAPCHAIN=ON : présentation par swapchain compilée (active avec SC2_SWAPCHAIN=1)")
endif()
