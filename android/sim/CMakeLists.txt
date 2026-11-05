# =============================================================================
# android/sim — banc Linux/qemu de la plate-forme Android (passe 7a) : sc2-sim
#
#   cmake -S android/sim -B android/build-sim \
#         -DCMAKE_TOOLCHAIN_FILE=$PWD/recomp-sc2/cmake/toolchain-aarch64.cmake
#   make -C android/build-sim -j3 sc2-sim
#
# Mêmes sources et mêmes --wrap que l'APK (android/CMakeLists.txt), glibc au lieu de bionic :
# accroches, fibres asm (getcontext/makecontext/swapcontext définis ici l'emportent sur ceux de la
# glibc), zone invitée décalée, cpu.h delta-propre (cmake/cpu_delta.cmake), exécutable PIE comme
# une .so Android.
# =============================================================================

cmake_minimum_required(VERSION 3.20)
project(sc2-sim VERSION 0.7.0 LANGUAGES C CXX ASM)
set(CMAKE_C_STANDARD 17)
# passe 8d : -DSC2_R=<arbre recomp-sc2> pour compiler le banc contre un arbre dont src/lifted/ est généré (hors git)
if(DEFINED SC2_R)
  set(R ${SC2_R})
else()
  set(R ${CMAKE_CURRENT_SOURCE_DIR}/../../recomp-sc2)
endif()
set(J ${CMAKE_CURRENT_SOURCE_DIR}/../jni)

include(${CMAKE_CURRENT_SOURCE_DIR}/../cmake/cpu_delta.cmake)
sc2_cpu_delta(${R} CPU_DELTA)

set(RUNTIME_SOURCES
    ${R}/src/main.c ${R}/runtime/image_base.c ${R}/runtime/sc2_runtime.c ${R}/runtime/sc2_iat.c
    ${R}/runtime/sc2_mpq.c ${R}/runtime/sc2_iat_p4.c ${R}/runtime/sc2_d3d9.c ${R}/runtime/sc2_d3d9v.c
    ${R}/runtime/win32_compat.c ${R}/runtime/stubs_sc2.c
    ${R}/runtime/sc2_input_sim.c    # passe 7d : entrées runtime (5f)
    ${R}/runtime/sc2_audio_fmod.c ${R}/runtime/sc2_audio_mix.c ${R}/runtime/sc2_audio_stb_vorbis.c   # passe 7e
    ${R}/runtime/sc2_audio_mpq.c ${R}/runtime/sc2_audio_sortie.c ${R}/runtime/sc2_audio_sortie_compteur.c)
set_source_files_properties(${RUNTIME_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-O2;-include;${J}/compat/sc2_android_compat.h;-include;${R}/runtime/sc2_lifted.h;-include;${CPU_DELTA}")
set_source_files_properties(${R}/src/main.c PROPERTIES COMPILE_DEFINITIONS "main=sc2_poc_main")

# Le code lifté est recompilé ici : il doit voir le cpu.h delta-propre (atomiques).
file(GLOB LIFTES "${R}/src/lifted/recomp_*.c")
set_source_files_properties(${LIFTES} PROPERTIES COMPILE_OPTIONS "-w;-O0;-include;${R}/runtime/sc2_lifted.h;-include;${CPU_DELTA}")
list(LENGTH LIFTES n)
message(STATUS "code lifté : ${n} fichiers")

set(STORMLIB_DIR "$ENV{HOME}/tools/StormLib" CACHE PATH "Checkout StormLib")
set(STORM_USE_BUNDLED_LIBRARIES ON CACHE BOOL "" FORCE)
set(STORM_SKIP_INSTALL ON CACHE BOOL "" FORCE)
set(STORM_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${STORMLIB_DIR} stormlib EXCLUDE_FROM_ALL)
target_compile_options(storm PRIVATE -w)
set_property(SOURCE ${R}/runtime/sc2_mpq.c ${R}/runtime/sc2_audio_mpq.c APPEND PROPERTY INCLUDE_DIRECTORIES "${STORMLIB_DIR}/src")
set_property(SOURCE ${R}/runtime/sc2_audio_stb_vorbis.c APPEND PROPERTY COMPILE_OPTIONS "-w")

# En-têtes Vulkan (le chargeur est ouvert par dlopen) : comme recomp-sc2/CMakeLists.txt.
set(VK_INC "${CMAKE_BINARY_DIR}/vk-entetes")
file(MAKE_DIRECTORY "${VK_INC}")
foreach(d vulkan vk_video)
    if(EXISTS "/usr/include/${d}" AND NOT EXISTS "${VK_INC}/${d}")
        file(CREATE_LINK "/usr/include/${d}" "${VK_INC}/${d}" SYMBOLIC)
    endif()
endforeach()

find_package(Threads REQUIRED)
add_executable(sc2-sim
    sc2_sim_main.c
    ${J}/sc2_android_hooks.c ${J}/sc2_trace_present.c ${J}/sc2_ucontext.c ${J}/sc2_ucontext_aarch64.S ${J}/sc2_audio_android.c
    ${RUNTIME_SOURCES} ${LIFTES})
target_include_directories(sc2-sim PRIVATE ${J} ${R}/src ${R}/runtime ${R}/src/lifted ${R}/src/shaders ${VK_INC})
set_target_properties(sc2-sim PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_link_options(sc2-sim PRIVATE -pie
    -Wl,--wrap=mmap -Wl,--wrap=dlopen -Wl,--wrap=d3dv_present)
target_link_libraries(sc2-sim PRIVATE StormLib::storm stdc++ Threads::Threads m ${CMAKE_DL_LIBS})

# ---- passe 7b : présentation GPU par swapchain (jni/sc2_vk_presente.c), option ----------------
# OFF par défaut : la copie CPU (7a) reste le chemin de l'APK tant que la swapchain n'est pas
# validée sur le Fold 8. ON : compilée, puis activée à l'exécution par SC2_SWAPCHAIN=1.
option(SC2_SWAPCHAIN "Présentation par swapchain Vulkan (VK_KHR_android_surface ; banc : VK_KHR_xcb_surface sur Xvfb)" OFF)
if(SC2_SWAPCHAIN)
    target_sources(sc2-sim PRIVATE ${J}/sc2_vk_presente.c)
    set_source_files_properties(${J}/sc2_android_hooks.c ${J}/sc2_vk_presente.c
        ${CMAKE_CURRENT_SOURCE_DIR}/sc2_sim_main.c
        PROPERTIES COMPILE_DEFINITIONS SC2_SWAPCHAIN=1)
    target_link_options(sc2-sim PRIVATE -Wl,--wrap=dlsym -Wl,--wrap=d3dv_cible)
    message(STATUS "SC2_SWAPCHAIN=ON : présentation par swapchain compilée (active avec SC2_SWAPCHAIN=1)")
endif()
