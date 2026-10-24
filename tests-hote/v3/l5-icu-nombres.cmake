# tests-hote/v3/l5-icu-nombres.cmake — L5 (24/10/2026) : NumberFormat d'ICU 4.4 en_US factice (recomp-sc2/runtime/
# sc2_icu_nombres.h, servi par sc2_iat_p4.c à NumberFormat::createInstance / createPercentInstance) : motifs
# « #,##0.### » et « #,##0% », 15 chiffres significatifs, arrondi au pair, séquences de réglages des appelants du jeu.
# Logique pure, en-tête seul : toutes plateformes.
sc2_test(test_icu_nombres SOURCES "${TR}/test_icu_nombres.c" INCLUDES "${RT}" LIBS m OPTIONS -Wall -Wextra)
