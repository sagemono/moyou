add_executable(moyou_tests
    unit/test_qrc.cpp
    unit/test_mnu.cpp
    unit/test_image.cpp
    unit/test_rsx.cpp
    unit/test_effects.cpp)
target_link_libraries(moyou_tests PRIVATE moyou_core Catch2::Catch2WithMain)
set_target_properties(moyou_tests PROPERTIES WIN32_EXECUTABLE OFF)
include(Catch)
catch_discover_tests(moyou_tests PROPERTIES LABELS unit)

set(MOYOU_RESEARCH_DIR "${CMAKE_SOURCE_DIR}/.." CACHE PATH "moyou research checkout (contains work/ and tools/)")
find_package(Python3 COMPONENTS Interpreter)

function(moyou_test name)
    add_executable(${name} ${name}.cpp)
    target_link_libraries(${name} PRIVATE moyou_core)
    set_target_properties(${name} PROPERTIES WIN32_EXECUTABLE OFF)
endfunction()

moyou_test(test_transpile)
add_test(NAME transpile COMMAND test_transpile "${MOYOU_RESEARCH_DIR}" "${Python3_EXECUTABLE}")
set_tests_properties(transpile PROPERTIES SKIP_RETURN_CODE 77)

moyou_test(test_particles)
add_test(NAME particles COMMAND test_particles "${MOYOU_RESEARCH_DIR}")
set_tests_properties(particles PROPERTIES SKIP_RETURN_CODE 77)

moyou_test(test_formats)
add_test(NAME formats COMMAND test_formats "${MOYOU_RESEARCH_DIR}" "${Python3_EXECUTABLE}")
set_tests_properties(formats PROPERTIES SKIP_RETURN_CODE 77)

moyou_test(test_asm)
add_test(NAME asm COMMAND test_asm "${MOYOU_RESEARCH_DIR}")
set_tests_properties(asm PROPERTIES SKIP_RETURN_CODE 77)