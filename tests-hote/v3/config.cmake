# tests-hote/v3/config.cmake — V3, lot M3 (22/10/2026) : configuration de l'app macOS en C pur (mac/sc2_config_mac.c).
# Fragment inclus par le bloc final de tests-hote/CMakeLists.txt (lot M0, glob de tests-hote/v3/*.cmake) : RACINE, RT et
# sc2_test() viennent de là. Mac seulement (CommonCrypto, chemins Darwin). sc2_documents.h (RT) dit si V1.5 est là.
if(APPLE)
    sc2_test(test_config_mac
             SOURCES "${RACINE}/mac/tests/test_config_mac.c" "${RACINE}/mac/sc2_config_mac.c"
             INCLUDES "${RACINE}/mac" "${RT}"
             OPTIONS -Wall -Wextra)
endif()
