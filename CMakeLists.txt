cmake_minimum_required(VERSION 3.21)

project(Open_Editor VERSION 3.0.0 LANGUAGES C CXX)
if (WIN32)
    enable_language(RC)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Concurrent Multimedia Svg OpenGL Network HttpServer)

qt_standard_project_setup(REQUIRES 6.10)

# Engine: no widgets, shared by the app and the headless tests.
file(GLOB OE_CORE CONFIGURE_DEPENDS src/core/*.cpp src/core/*.h src/engine/*.cpp src/engine/*.h)
qt_add_library(oe_core STATIC ${OE_CORE})
target_include_directories(oe_core PUBLIC src)
target_link_libraries(oe_core PUBLIC Qt6::Core Qt6::Gui Qt6::Concurrent Qt6::Multimedia Qt6::OpenGL Qt6::Network)
if (WIN32)
    target_link_libraries(oe_core PUBLIC dbghelp)
endif()

file(GLOB OE_UI CONFIGURE_DEPENDS src/ui/*.cpp src/ui/*.h)
if (OE_UI)
    # Window and Explorer icon. MinGW's windres cannot take include paths with spaces, which this
    # project has, so the resource is compiled from src without any.
    set(OE_WIN_RC "")
    if (WIN32 AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/app.ico)
        if (MINGW)
            set(OE_WIN_RC ${CMAKE_CURRENT_BINARY_DIR}/app_icon.o)
            add_custom_command(OUTPUT ${OE_WIN_RC}
                COMMAND ${CMAKE_RC_COMPILER} -O coff app.rc ${OE_WIN_RC}
                WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/src
                DEPENDS src/app.rc src/app.ico VERBATIM)
            set_source_files_properties(${OE_WIN_RC} PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        else()
            set(OE_WIN_RC src/app.rc)
        endif()
    endif()
    qt_add_executable(appOpen_Editor src/main.cpp ${OE_UI} ${OE_WIN_RC})
    target_compile_definitions(appOpen_Editor PRIVATE OE_VERSION="${PROJECT_VERSION}")
    set_target_properties(appOpen_Editor PROPERTIES OUTPUT_NAME OpenEditor WIN32_EXECUTABLE TRUE)
    target_link_libraries(appOpen_Editor PRIVATE oe_core Qt6::Widgets Qt6::Svg Qt6::HttpServer)
    if (WIN32)
        target_link_libraries(appOpen_Editor PRIVATE crypt32)
    endif()
    # Icon library: Tabler Icons (MIT, see src/ui/icons/LICENSE-tabler.txt)
    file(GLOB OE_ICONS CONFIGURE_DEPENDS RELATIVE ${CMAKE_CURRENT_SOURCE_DIR} src/ui/icons/*.svg)
    qt_add_resources(appOpen_Editor "icons" PREFIX "/" BASE "src/ui" FILES ${OE_ICONS})
    install(TARGETS appOpen_Editor
        BUNDLE DESTINATION .
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

# Example effect plugin, built into the folder the editor scans.
add_library(oe_plugin_duotone MODULE examples/plugin_duotone/duotone.c)
target_include_directories(oe_plugin_duotone PRIVATE src/plugin_api)
set_target_properties(oe_plugin_duotone PROPERTIES PREFIX "" OUTPUT_NAME duotone LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/oe-plugins)
target_link_options(oe_plugin_duotone PRIVATE -static-libgcc)

# stdio bridge for MCP clients that launch a local command.
qt_add_executable(oe-mcp tools/oe_mcp_bridge.cpp)
set_target_properties(oe-mcp PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(oe-mcp PRIVATE Qt6::Core Qt6::Network)

# Headless engine tests: ctest, or run oe_engine_test directly.
enable_testing()
qt_add_executable(oe_engine_test tests/engine_test.cpp)
set_target_properties(oe_engine_test PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(oe_engine_test PRIVATE oe_core)
add_test(NAME engine COMMAND oe_engine_test)
qt_add_executable(oe_features_test tests/features_test.cpp)
set_target_properties(oe_features_test PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(oe_features_test PRIVATE oe_core)
add_dependencies(oe_features_test oe_plugin_duotone)
add_test(NAME features COMMAND oe_features_test)
qt_add_executable(oe_v2_test tests/v2_test.cpp)
set_target_properties(oe_v2_test PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(oe_v2_test PRIVATE oe_core)
add_test(NAME v2 COMMAND oe_v2_test)
qt_add_executable(oe_text_test tests/text_test.cpp)
set_target_properties(oe_text_test PROPERTIES WIN32_EXECUTABLE FALSE)
target_compile_definitions(oe_text_test PRIVATE OE_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(oe_text_test PRIVATE oe_core)
add_test(NAME text COMMAND oe_text_test)
qt_add_executable(oe_v3_test tests/v3_test.cpp)
set_target_properties(oe_v3_test PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(oe_v3_test PRIVATE oe_core)
add_test(NAME v3 COMMAND oe_v3_test)
qt_add_executable(oe_align_eval tests/align_eval.cpp)
set_target_properties(oe_align_eval PROPERTIES WIN32_EXECUTABLE FALSE)
target_link_libraries(oe_align_eval PRIVATE oe_core)
if (TARGET appOpen_Editor)
    add_dependencies(appOpen_Editor oe-mcp oe_plugin_duotone)
endif()
