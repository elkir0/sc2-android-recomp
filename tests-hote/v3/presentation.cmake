# tests-hote/v3/presentation.cmake — V3, lot M5 (22/10) : branche __APPLE__ du présentateur (android/jni/sc2_vk_presente.c)
# contre MoltenVK, surface sans écran (android/jni/tests/test_presentation_mac.c). Fragment inclus par le bloc final de
# tests-hote/CMakeLists.txt (lot M0, glob de tests-hote/v3/*.cmake) : RACINE, RT, JNI, VK_INC, sc2_test() et
# sc2_faibles_attentes() viennent de là. Mac seulement ; MoltenVK absent à l'exécution : SKIP (77).
#
# Même assemblage que recomp-sc2 (mac/mac.cmake, M0) : les renommages sont des définitions de CIBLE sur des
# bibliothèques OBJECT, jamais des propriétés de source (portée de dossier : test_d3d9v et test_d3dx_surface compilent
# aussi sc2_d3d9v.c ici, sans renommage).
if(APPLE AND VK_INC)
    add_library(pm5_d3d9v OBJECT "${RT}/sc2_d3d9v.c")
    target_include_directories(pm5_d3d9v PRIVATE "${RT}" "${VK_INC}")
    target_compile_definitions(pm5_d3d9v PRIVATE
        d3dv_present=d3dv_present_reel d3dv_cible=d3dv_cible_reel dlsym=sc2v_dlsym dladdr=sc2v_dladdr)
    add_library(pm5_presentateur OBJECT "${JNI}/sc2_vk_presente.c" "${JNI}/sc2_trace_present.c")
    target_include_directories(pm5_presentateur PRIVATE "${JNI}" "${RT}" "${VK_INC}")
    target_compile_definitions(pm5_presentateur PRIVATE SC2_SWAPCHAIN=1
        __wrap_d3dv_cible=d3dv_cible     __real_d3dv_cible=d3dv_cible_reel
        __wrap_dlsym=sc2v_dlsym          __real_dlsym=dlsym)
    target_compile_options(pm5_presentateur PRIVATE -Wall)
    sc2_test(test_presentation_mac
             SOURCES "${JNI}/tests/test_presentation_mac.c"
                     $<TARGET_OBJECTS:pm5_d3d9v> $<TARGET_OBJECTS:pm5_presentateur>
             INCLUDES "${JNI}" "${RT}" "${VK_INC}"
             LIBS Threads::Threads ${CMAKE_DL_LIBS} m objc
             OPTIONS -Wall -Wextra)
    sc2_faibles_attentes(test_presentation_mac)
    set_tests_properties(test_presentation_mac PROPERTIES SKIP_RETURN_CODE 77)
elseif(APPLE)
    add_test(NAME test_presentation_mac COMMAND sh -c "echo 'SKIP  en-têtes Vulkan absents à la configuration'; exit 77")
    set_tests_properties(test_presentation_mac PROPERTIES SKIP_RETURN_CODE 77)
endif()
