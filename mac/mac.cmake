# =============================================================================
# mac/mac.cmake — V3 (22/10, lot M0 ; v3-critique.md §3.4, v3-conception-A.md §3.2) : l'app macOS EST l'exécutable
# recomp-sc2. Inclus à la fin de recomp-sc2/CMakeLists.txt sous APPLE AND SC2_APP_MAC (la cible recomp-sc2 et
# SC2_VK_INC existent ; les chemins relatifs le sont à recomp-sc2/).
#
# RÈGLES :
#  (a) aucune option de RÉPERTOIRE (add_compile_options, add_definitions, CMAKE_OSX_DEPLOYMENT_TARGET…) : les objets du
#      lift (cache SC2_LIFTED_PREBUILT, empreinte de scripts/garde-lift.sh) et des autres cibles ne bougent pas ;
#  (b) les renommages sont des définitions de CIBLE sur des bibliothèques OBJECT, jamais des propriétés de source (portée
#      de dossier : test_d3d9v, runner-difftest et banc-video-7w3 compilent aussi runtime/sc2_d3d9v.c) ;
#  (c) mac/*.c et mac/*.m par GLOB (non récursif : mac/tests/ n'y entre pas) : un lot ajoute un fichier, pas une ligne
#      ici. Seule exception réservée : la ligne SC2_MAC_SON_COREAUDIO, que M6 passe à ON.
#
# Les --wrap d'Android deviennent des noms publics (A §2.1) :
#   backend    d3dv_present → d3dv_present_reel, d3dv_cible → d3dv_cible_reel, dlsym → sc2v_dlsym, dladdr → sc2v_dladdr ;
#   couche     __wrap_X → X, __real_X → X_reel (__real_dlsym → dlsym) ; mmap et dlopen sans objet (zone_hote_init,
#              charger_vk_apple ; sc2_android_hooks.c les exclut sous __APPLE__).
# =============================================================================
set(SC2M_MAC "${CMAKE_CURRENT_LIST_DIR}")
get_filename_component(SC2M_DEPOT "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(SC2M_JNI "${SC2M_DEPOT}/android/jni")
# mêmes chemins, dans le même ordre, que la cible recomp-sc2 (ses autres -I ne servent qu'à StormLib et Theora)
set(SC2M_INC_RUNTIME src runtime src/lifted "${SC2_VK_INC}" src/shaders)

# 1. backend Vulkan « crocheté » : le même fichier, les mêmes drapeaux que dans recomp-sc2, plus quatre renommages
add_library(sc2_d3d9v_mac OBJECT runtime/sc2_d3d9v.c)
target_include_directories(sc2_d3d9v_mac PRIVATE ${SC2M_INC_RUNTIME})
target_compile_definitions(sc2_d3d9v_mac PRIVATE
    d3dv_present=d3dv_present_reel d3dv_cible=d3dv_cible_reel dlsym=sc2v_dlsym dladdr=sc2v_dladdr)

# 2. main() du banc renommé (précédent : android/CMakeLists.txt) ; le main() du binaire est mac/sc2_mac_main.m
add_library(sc2_main_poc OBJECT src/main.c)
target_include_directories(sc2_main_poc PRIVATE ${SC2M_INC_RUNTIME})
target_compile_definitions(sc2_main_poc PRIVATE main=sc2_poc_main)

# 3. couche plate-forme d'Android compilée EN PLACE + hôte Mac
# Sortie son CoreAudio (android/jni/sc2_audio_android.c, branche __APPLE__) : OFF tant que M6 ne l'a pas écrite (talons
# faibles sc2aa_* de mac/sc2_mac_hote.c). Variable ordinaire, pas une option de cache : son passage à ON par M6 vaut
# aussi pour un dossier de build déjà configuré.
set(SC2_MAC_SON_COREAUDIO ON)
file(GLOB SC2M_HOTE CONFIGURE_DEPENDS "${SC2M_MAC}/*.c" "${SC2M_MAC}/*.m")
set(SC2M_PLATEFORME "${SC2M_JNI}/sc2_android_hooks.c" "${SC2M_JNI}/sc2_vk_presente.c" "${SC2M_JNI}/sc2_trace_present.c")
if(SC2_MAC_SON_COREAUDIO)
    list(APPEND SC2M_PLATEFORME "${SC2M_JNI}/sc2_audio_android.c")
endif()
add_library(sc2_plateforme_mac OBJECT ${SC2M_PLATEFORME} ${SC2M_HOTE})
target_include_directories(sc2_plateforme_mac PRIVATE "${SC2M_MAC}" "${SC2M_JNI}" ${SC2M_INC_RUNTIME})
target_compile_definitions(sc2_plateforme_mac PRIVATE SC2_SWAPCHAIN=1
    __wrap_d3dv_present=d3dv_present __real_d3dv_present=d3dv_present_reel
    __wrap_d3dv_cible=d3dv_cible     __real_d3dv_cible=d3dv_cible_reel
    __wrap_dlsym=sc2v_dlsym          __real_dlsym=dlsym)
target_compile_options(sc2_plateforme_mac PRIVATE -Wall $<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>)

# Tous les cadriciels dès M0 : aucun lot n'a à éditer cette ligne (AppKit est déjà chargé par MoltenVK au banc).
target_link_libraries(recomp-sc2 PRIVATE sc2_d3d9v_mac sc2_main_poc sc2_plateforme_mac
    "-framework AppKit" "-framework QuartzCore" "-framework Metal" "-framework CoreGraphics"
    "-framework AudioToolbox" "-framework CoreAudio")

# 4. paquet de DÉVELOPPEMENT : SC2Partout.app autour d'un CLONE APFS du binaire (même contenu, aucun second lien ;
#    l'éditeur de liens a déjà signé ad hoc l'exécutable arm64). Hors de ALL et sans lsregister (A-6) : à demander,
#    « ninja -C <build> sc2partout-app ». Le paquet distribuable est fait par scripts/paquet-mac.sh (M9).
set(SC2M_APP "${CMAKE_BINARY_DIR}/SC2Partout.app")
execute_process(COMMAND git -C "${SC2M_DEPOT}" describe --always --dirty
                OUTPUT_VARIABLE SC2M_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT SC2M_REVISION)
    set(SC2M_REVISION "inconnue")
endif()
# LSMinimumSystemVersion (M9) : la version de l'hôte qui fait le paquet — le binaire n'est pas fait pour plus vieux
# (lift du banc, MoltenVK embarqué). Pas de cible de déploiement plus ancienne : le paquet est pour les Mac du mainteneur.
execute_process(COMMAND sw_vers -productVersion
                OUTPUT_VARIABLE SC2M_OS_MINIMUM OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
if(NOT SC2M_OS_MINIMUM)
    set(SC2M_OS_MINIMUM "15.0")
endif()
configure_file("${SC2M_MAC}/Info.plist.in" "${CMAKE_BINARY_DIR}/SC2Partout-Info.plist" @ONLY)
set(SC2M_EXE "${SC2M_APP}/Contents/MacOS/recomp-sc2")
add_custom_target(sc2partout-app
    COMMAND mkdir -p "${SC2M_APP}/Contents/MacOS" "${SC2M_APP}/Contents/Resources" "${SC2M_APP}/Contents/Frameworks"
    # copie puis renommage : un exécutable en cours d'exécution n'est jamais réécrit en place
    COMMAND sh -c "rm -f '${SC2M_EXE}.tmp' && { cp -c '$<TARGET_FILE:recomp-sc2>' '${SC2M_EXE}.tmp' 2>/dev/null || cp '$<TARGET_FILE:recomp-sc2>' '${SC2M_EXE}.tmp'; } && mv -f '${SC2M_EXE}.tmp' '${SC2M_EXE}'"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_BINARY_DIR}/SC2Partout-Info.plist" "${SC2M_APP}/Contents/Info.plist"
    COMMAND ln -sfn "${SC2M_DEPOT}/android/assets/polices" "${SC2M_APP}/Contents/Resources/polices"
    COMMAND ln -sfn "${CMAKE_CURRENT_SOURCE_DIR}/inits-stricts.txt" "${SC2M_APP}/Contents/Resources/inits-stricts.txt"
    DEPENDS recomp-sc2 "${CMAKE_BINARY_DIR}/SC2Partout-Info.plist"
    COMMENT "SC2Partout.app autour d'un clone APFS de recomp-sc2"
    VERBATIM)
