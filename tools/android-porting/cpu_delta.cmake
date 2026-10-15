# cpu_delta.cmake — copie de recomp-sc2/runtime/cpu.h dont les atomiques respectent g_hote_delta
# (passe 7a). Le fichier d'origine n'est PAS modifié (zone de la passe 5d) : la copie est générée
# dans le dossier de build et incluse de force APRÈS sc2_lifted.h ; sa garde PCRECOMP_CPU_H rend
# l'#include "cpu.h" des sources sans effet.
#
# Défaut corrigé : atomic_xadd32 / atomic_cmpxchg32 / atomic_xchg32 (lock xadd, lock cmpxchg, xchg)
# déréférencent la VA invitée BRUTE — (volatile uint32_t *)(uintptr_t)a — au lieu de GPTR(a).
# Juste tant que g_hote_delta = 0 (Linux, arène à adresse fixe) ; faux dès que la zone invitée est
# décalée (macOS, Android). Quand la passe 5d l'aura corrigé en amont, 0 remplacement : copie
# identique, rien à retirer.
function(sc2_cpu_delta R SORTIE)
    set(src "${R}/runtime/cpu.h")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${src}")
    file(READ "${src}" t)
    string(REPLACE "(volatile uint32_t *)(uintptr_t)a" "(volatile uint32_t *)GPTR(a)" p "${t}")
    string(LENGTH "${t}" l0)
    string(REGEX MATCHALL "volatile uint32_t \\*\\)GPTR\\(a\\)" n "${p}")
    list(LENGTH n nb)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/cpu-delta")
    file(WRITE "${CMAKE_BINARY_DIR}/cpu-delta/cpu.h.tmp" "/* GÉNÉRÉ par android/cmake/cpu_delta.cmake depuis ${src} — ne pas éditer */\n${p}")
    file(COPY_FILE "${CMAKE_BINARY_DIR}/cpu-delta/cpu.h.tmp" "${CMAKE_BINARY_DIR}/cpu-delta/cpu.h" ONLY_IF_DIFFERENT)
    message(STATUS "cpu.h delta-propre : ${nb} atomique(s) sur GPTR (attendu 3) → ${CMAKE_BINARY_DIR}/cpu-delta/cpu.h")
    if(NOT nb EQUAL 3)
        message(WARNING "cpu.h : ${nb} atomiques GPTR au lieu de 3 — vérifier runtime/cpu.h (forme changée ?)")
    endif()
    set(${SORTIE} "${CMAKE_BINARY_DIR}/cpu-delta/cpu.h" PARENT_SCOPE)
endfunction()
