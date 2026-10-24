# tests-hote/v3/mpq-alias.cmake — 24/10/2026 : alias « X.wav » → « X.wav_ » des tables de hachage MPQ servies au moteur
# (recomp-sc2/runtime/sc2_mpq_alias.h, inclus par sc2_mpq.c ; port runtime de la passe 7z2). Logique pure, en-tête seul,
# sans StormLib : toutes plateformes.
sc2_test(test_mpq_alias SOURCES "${TR}/test_mpq_alias.c" INCLUDES "${RT}" OPTIONS -Wall -Wextra -Wno-unused-function)
