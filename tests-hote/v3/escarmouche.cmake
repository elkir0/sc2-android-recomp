# tests-hote/v3/escarmouche.cmake — V4.10 (23/10) : escarmouche contre l'IA, partie « pur calcul » de
# recomp-sc2/runtime/sc2_escarmouche.c (MapInfo 0x18 synthétique, insertion dans MapScript.galaxy, noms de cartes,
# races, difficultés, options). Le test INCLUT le module avec SC2_ESCARMOUCHE_SANS_MPQ : ni StormLib ni données du jeu
# (la partie StormLib se vérifie sur les 63 vraies cartes par SC2_ESCARMOUCHE=verifier). Fragment inclus par le bloc
# final de tests-hote/CMakeLists.txt (glob de tests-hote/v3/*.cmake) : TR, RT et sc2_test() viennent de là.
sc2_test(test_escarmouche SOURCES "${TR}/test_escarmouche.c" INCLUDES "${RT}" OPTIONS -Wall -Wextra)
