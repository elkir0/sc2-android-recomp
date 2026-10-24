# tests-hote/v3/catalogue.cmake — V3, lot M9 (24/10/2026) : catalogue du lanceur macOS (mac/sc2_catalogue_mac.c).
# Fragment inclus par le bloc final de tests-hote/CMakeLists.txt (lot M0, glob de tests-hote/v3/*.cmake) : RACINE et
# sc2_test() viennent de là. Mac seulement. Le test RELIT android/src/fr/sc2partout/Catalogue.java et Options.java
# (données de référence d'Android, aucune donnée privée) : leur chemin passe en ARGS, mode --cartes en dehors de ctest.
if(APPLE)
    sc2_test(test_catalogue_mac
             SOURCES "${RACINE}/mac/tests/test_catalogue_mac.c" "${RACINE}/mac/sc2_catalogue_mac.c"
             INCLUDES "${RACINE}/mac"
             ARGS "${RACINE}/android/src/fr/sc2partout/Catalogue.java" "${RACINE}/android/src/fr/sc2partout/Options.java"
             OPTIONS -Wall -Wextra)
endif()
