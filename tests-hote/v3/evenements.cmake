# tests-hote/v3/evenements.cmake — V3, lot M4 (22/10) : rejeu SC2_MAC_EVENEMENTS, cœur en C pur (mac/sc2_mac_evenements.c :
# grammaire, erreurs nommées, ordonnancement sur horloge simulée, suivi du journal, conversion j → points de vue).
# Inclus par le bloc final de tests-hote/CMakeLists.txt (M0, glob de tests-hote/v3/*.cmake). C pur et POSIX, sans AppKit :
# pas de garde APPLE (le test tourne aussi sous Linux). Ses fichiers temporaires vont dans $TMPDIR (défaut /tmp), effacés.
sc2_test(test_mac_evenements SOURCES "${RACINE}/mac/tests/test_mac_evenements.c" "${RACINE}/mac/sc2_mac_evenements.c"
         INCLUDES "${RACINE}/mac" LIBS m OPTIONS -Wall -Wextra)
