# M1 (V3, 22/10) — API d'entrées de bureau du runtime : sc2_input_touche_plateforme, sc2_input_caractere,
# sc2_input_activation, sc2_input_tout_relacher. Le test INCLUT recomp-sc2/runtime/sc2_input_sim.c (états static), donc
# sc2_entrees_banc.c aussi. Ses références FAIBLES de V1.1 (sc2_d3d9_taille, sc2_glue_masque, sc2_frames_presentees,
# définies dans recomp-sc2 et absentes ici) : Mach-O refuse un symbole faible non défini à l'édition des liens, -U les
# laisse se résoudre à NULL (comme sc2_faibles_attentes) ; sans effet tant qu'elles ne sont pas référencées.
sc2_test(test_input_sim SOURCES "${TR}/test_input_sim.c" INCLUDES "${RT}" LIBS Threads::Threads OPTIONS -Wall -Wextra)
if(APPLE)
    target_link_options(test_input_sim PRIVATE
        "LINKER:-U,_sc2_d3d9_taille" "LINKER:-U,_sc2_glue_masque" "LINKER:-U,_sc2_frames_presentees")
endif()
