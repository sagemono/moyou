cmake_minimum_required(VERSION 3.25)

project(moyou
    VERSION 1.0.0
    DESCRIPTION "Theming tool for the PS3 XMB wave (lines.qrc)"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8 /fp:precise /Zc:__cplusplus /wd4100)
else()
    add_compile_options(-Wall -Wextra -ffp-contract=off)
endif()

find_package(Qt6 6.5 REQUIRED COMPONENTS Core Gui Widgets OpenGL OpenGLWidgets)
find_package(ZLIB REQUIRED)

qt_standard_project_setup()

add_library(moyou_core STATIC
    src/formats/qrc.cpp
    src/formats/mnu.cpp
    src/formats/image.cpp
    src/rsx/cgbinary.cpp
    src/rsx/glsl.cpp
    src/rsx/asm.cpp
    src/render/effects.cpp
    src/sim/params.cpp
    src/sim/wave.cpp
    src/particles/particles.cpp
    src/render/gl.cpp
    src/render/renderer.cpp
)
target_include_directories(moyou_core PUBLIC src)
target_link_libraries(moyou_core PUBLIC Qt6::Core Qt6::Gui Qt6::OpenGL ZLIB::ZLIB)
target_compile_definitions(moyou_core PUBLIC MOYOU_VERSION="${PROJECT_VERSION}")   # About box, the note in saved files

qt_add_resources(moyou_core "moyou_core_data"
    PREFIX "/moyou"
    BASE src
    FILES
        src/render/linescontroller.asm
        src/render/rsx_lib.glsl
        src/render/quad_vs.glsl
        src/render/wave_pre.glsl
        src/render/wave_main.glsl
        src/render/present_fs.glsl
)

qt_add_executable(moyou_app WIN32 MACOSX_BUNDLE
    src/main.cpp
    src/app/mainwindow.cpp
    src/app/mainwindow.h
    src/app/waveview.cpp
    src/app/waveview.h
    src/app/controlpanel.cpp
    src/app/controlpanel.h
    src/app/snapshot.cpp
    src/app/snapshot.h
    src/app/icon.cpp
    src/app/icon.h
    src/app/shadereditor.cpp
    src/app/shadereditor.h
    src/app/effectspanel.cpp
    src/app/effectspanel.h
)
target_link_libraries(moyou_app PRIVATE moyou_core Qt6::Widgets Qt6::OpenGLWidgets)

# Window icon: PNGs rendered from resources/icons/moyou.svg by tools/make_icons.py (no Qt SVG module needed).
set(MOYOU_ICON_PNGS)
foreach(size 16 20 24 32 40 48 64 128 256)
    list(APPEND MOYOU_ICON_PNGS resources/icons/moyou-${size}.png)
endforeach()
qt_add_resources(moyou_app "moyou_app_icon" PREFIX "/moyou" BASE resources FILES ${MOYOU_ICON_PNGS})
if(WIN32)
    target_sources(moyou_app PRIVATE resources/windows/moyou.rc)   # the .exe icon
endif()

if(WIN32)
    if(TARGET Qt6::windeployqt)
        set(MOYOU_WINDEPLOYQT "$<TARGET_FILE:Qt6::windeployqt>")
    else()
        find_program(MOYOU_WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../tools/Qt6/bin" "${Qt6_DIR}/../../../tools/Qt6/bin")
    endif()
    if(MOYOU_WINDEPLOYQT)
        add_custom_command(TARGET moyou_app POST_BUILD
            COMMAND "${MOYOU_WINDEPLOYQT}" --no-translations --no-system-d3d-compiler --no-opengl-sw --no-compiler-runtime
                    --verbose 0 "$<TARGET_FILE:moyou_app>"
            COMMENT "windeployqt: Qt plugins next to moyou.exe")
    endif()
endif()
set_target_properties(moyou_app PROPERTIES OUTPUT_NAME moyou)

qt_add_executable(moyou_cli src/cli.cpp)
target_link_libraries(moyou_cli PRIVATE moyou_core)
set_target_properties(moyou_cli PROPERTIES OUTPUT_NAME moyou-cli WIN32_EXECUTABLE OFF)

include(CTest)

option(MOYOU_FETCH_CATCH2 "Download Catch2 3 when the system has no compatible version" ON)

if(BUILD_TESTING)
    find_package(Catch2 3 CONFIG QUIET)
    if(NOT Catch2_FOUND)
        if(NOT MOYOU_FETCH_CATCH2)
            message(FATAL_ERROR "Catch2 3 not found and MOYOU_FETCH_CATCH2 is OFF. Install Catch2 3, re-run with -DMOYOU_FETCH_CATCH2=ON, or build without tests (-DBUILD_TESTING=OFF).")
        endif()
        message(STATUS "Catch2 3 not found on the system - fetching it")
        include(FetchContent)
        FetchContent_Declare(Catch2
            GIT_REPOSITORY https://github.com/catchorg/Catch2.git
            GIT_TAG v3.9.1
            GIT_SHALLOW TRUE)
        FetchContent_MakeAvailable(Catch2)
        list(APPEND CMAKE_MODULE_PATH "${catch2_SOURCE_DIR}/extras")
    endif()
    add_subdirectory(tests)
endif()

include(GNUInstallDirs)
install(TARGETS moyou_app moyou_cli
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    BUNDLE DESTINATION .)
