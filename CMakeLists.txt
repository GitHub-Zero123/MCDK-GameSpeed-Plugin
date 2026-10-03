cmake_minimum_required(VERSION 3.24)
project(McdkGameSpeed VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(MSVC AND NOT DEFINED CMAKE_MSVC_RUNTIME_LIBRARY)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()
if(MSVC)
    add_compile_options(/utf-8)
endif()
option(GAMESPEED_BUILD_INJECTED "Build the injected Windows x64 DLL and RmlUi overlay" ${WIN32})
option(GAMESPEED_BUILD_PLUGIN "Build the MCDevTool host plugin" ${WIN32})
option(GAMESPEED_BUILD_TOOLS "Build the standalone injector/controller" ${WIN32})
option(GAMESPEED_BUILD_TESTS "Build clock and isolated injection tests" ON)
set(GAMESPEED_PACKAGE_DIR "${CMAKE_BINARY_DIR}/plugins/game-speed/$<CONFIG>"
    CACHE STRING "Plugin package output directory (supports configuration generator expressions)")

function(gamespeed_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

add_library(gamespeed_clock STATIC src/core/VirtualClock.cpp)
target_include_directories(gamespeed_clock PUBLIC include)
gamespeed_warnings(gamespeed_clock)

if(GAMESPEED_BUILD_INJECTED OR GAMESPEED_BUILD_PLUGIN OR GAMESPEED_BUILD_TOOLS)
    if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
        message(FATAL_ERROR "The game integration requires Windows x64. For clock-only builds disable GAMESPEED_BUILD_INJECTED, GAMESPEED_BUILD_PLUGIN and GAMESPEED_BUILD_TOOLS.")
    endif()
    add_library(gamespeed_client STATIC src/client/Client.cpp)
    target_include_directories(gamespeed_client PUBLIC include)
    target_compile_definitions(gamespeed_client PUBLIC WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
    gamespeed_warnings(gamespeed_client)
endif()

if(GAMESPEED_BUILD_INJECTED)
    foreach(dep RmlUi minhook freetype)
        if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/${dep}/CMakeLists.txt")
            message(FATAL_ERROR "Missing external/${dep}. Run: git submodule update --init --recursive")
        endif()
    endforeach()
    set(BUILD_SHARED_LIBS OFF)
    foreach(dep ZLIB BZIP2 PNG HARFBUZZ BROTLI)
        set(FT_DISABLE_${dep} ON CACHE BOOL "Build FreeType without optional ${dep}" FORCE)
    endforeach()
    add_subdirectory(external/freetype EXCLUDE_FROM_ALL)
    add_library(Freetype::Freetype ALIAS freetype)
    # RmlUi's find module may probe the machine; the pinned target above is authoritative.
    set(CMAKE_DISABLE_FIND_PACKAGE_Freetype TRUE)
    set(RMLUI_SAMPLES OFF CACHE BOOL "" FORCE)
    set(RMLUI_LUA_BINDINGS OFF CACHE BOOL "" FORCE)
    set(RMLUI_FONT_ENGINE freetype CACHE STRING "" FORCE)
    add_subdirectory(external/RmlUi EXCLUDE_FROM_ALL)
    set(CMAKE_POLICY_DEFAULT_CMP0077 NEW)
    add_subdirectory(external/minhook EXCLUDE_FROM_ALL)
    # The game's WGL context is OpenGL 3.2 / GLSL 1.50. The pinned GL3
    # renderer uses no 3.3-only shader syntax; adapt its version declaration
    # in a generated build copy and leave the submodule checkout unchanged.
    set(_gamespeed_gl3_source "${CMAKE_CURRENT_SOURCE_DIR}/external/RmlUi/Backends/RmlUi_Renderer_GL3.cpp")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_gamespeed_gl3_source}")
    file(READ "${_gamespeed_gl3_source}" _gamespeed_gl3_code)
    if(NOT _gamespeed_gl3_code MATCHES "#version 330")
        message(FATAL_ERROR "The pinned RmlUi shader version changed; review the OpenGL 3.2 adapter.")
    endif()
    string(REPLACE "#version 330" "#version 150" _gamespeed_gl3_code "${_gamespeed_gl3_code}")
    file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/generated/RmlUi_Renderer_GL32.cpp" "${_gamespeed_gl3_code}")
    add_library(gamespeed_hook SHARED
        src/injected/DllMain.cpp
        src/injected/ClockHook.cpp
        src/injected/Overlay.cpp
        src/injected/PipeServer.cpp
        external/RmlUi/Backends/RmlUi_Platform_Win32.cpp
        "${CMAKE_CURRENT_BINARY_DIR}/generated/RmlUi_Renderer_GL32.cpp")
    target_include_directories(gamespeed_hook PRIVATE include src/injected external/RmlUi/Backends)
    target_compile_definitions(gamespeed_hook PRIVATE NOMINMAX UNICODE= _UNICODE=)
    target_link_libraries(gamespeed_hook PRIVATE gamespeed_clock RmlUi::Core minhook opengl32 gdi32 user32 imm32 advapi32 comctl32)
    set_target_properties(gamespeed_hook PROPERTIES
        PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${GAMESPEED_PACKAGE_DIR}")
    gamespeed_warnings(gamespeed_hook)
    add_custom_target(gamespeed_assets
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GAMESPEED_PACKAGE_DIR}/assets"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GAMESPEED_PACKAGE_DIR}/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/assets/speed.rml" "${GAMESPEED_PACKAGE_DIR}/assets/speed.rml"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/external/RmlUi/Samples/assets/LatoLatin-Regular.ttf" "${GAMESPEED_PACKAGE_DIR}/assets/LatoLatin-Regular.ttf"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/external/RmlUi/Samples/assets/LICENSE.txt" "${GAMESPEED_PACKAGE_DIR}/assets/FONT-LICENSE.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/external/RmlUi/LICENSE.txt" "${GAMESPEED_PACKAGE_DIR}/licenses/RMLUI.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/external/minhook/LICENSE.txt" "${GAMESPEED_PACKAGE_DIR}/licenses/MINHOOK.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/external/freetype/docs/FTL.TXT" "${GAMESPEED_PACKAGE_DIR}/licenses/FREETYPE-FTL.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/external/freetype/docs/GPLv2.TXT" "${GAMESPEED_PACKAGE_DIR}/licenses/FREETYPE-GPLv2.txt"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/README.md" "${GAMESPEED_PACKAGE_DIR}/README.md"
        VERBATIM)
    add_dependencies(gamespeed_hook gamespeed_assets)
endif()

if(GAMESPEED_BUILD_PLUGIN)
    if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/external/MCDevTool/sdk/plugin-sdk/CMakeLists.txt")
        message(FATAL_ERROR "Missing MCDevTool SDK. Run: git submodule update --init --recursive")
    endif()
    set(MCDK_PLUGIN_SDK_BUILD_CHECKS OFF CACHE BOOL "" FORCE)
    set(MCDK_PLUGIN_SDK_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    add_subdirectory(external/MCDevTool/sdk/plugin-sdk mcdevtool-sdk EXCLUDE_FROM_ALL)
    mcdk_add_plugin(game_speed_plugin
        ID com.github-zero123.game-speed VERSION ${PROJECT_VERSION}
        SOURCES plugins/game-speed/Plugin.cpp
        OUTPUT_DIRECTORY "${GAMESPEED_PACKAGE_DIR}")
    target_link_libraries(game_speed_plugin PRIVATE gamespeed_client)
    target_include_directories(game_speed_plugin PRIVATE external/MCDevTool/libs/nlohmann)
    gamespeed_warnings(game_speed_plugin)
    if(TARGET gamespeed_hook)
        add_dependencies(game_speed_plugin gamespeed_hook)
    endif()
    add_custom_target(gamespeed_manifest
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GAMESPEED_PACKAGE_DIR}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/plugins/game-speed/plugin.json" "${GAMESPEED_PACKAGE_DIR}/plugin.json"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${GAMESPEED_PACKAGE_DIR}/licenses"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/external/MCDevTool/LICENSE" "${GAMESPEED_PACKAGE_DIR}/licenses/MCDEVTOOL.txt"
        VERBATIM)
    add_dependencies(game_speed_plugin gamespeed_manifest)
endif()

if(GAMESPEED_BUILD_TOOLS)
    add_executable(gamespeed tools/main.cpp)
    target_link_libraries(gamespeed PRIVATE gamespeed_client)
    set_target_properties(gamespeed PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin/$<CONFIG>")
    gamespeed_warnings(gamespeed)
    if(TARGET minhook)
        add_library(gamespeed_gl_probe SHARED EXCLUDE_FROM_ALL tools/GlVersionProbe.cpp)
        target_link_libraries(gamespeed_gl_probe PRIVATE minhook opengl32 gdi32)
        set_target_properties(gamespeed_gl_probe PROPERTIES
            PREFIX "" RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/probe/$<CONFIG>")
        gamespeed_warnings(gamespeed_gl_probe)
    endif()
endif()

if(GAMESPEED_BUILD_TESTS)
    enable_testing()
    find_package(Threads REQUIRED)
    add_executable(gamespeed_clock_tests tests/ClockTests.cpp)
    target_link_libraries(gamespeed_clock_tests PRIVATE gamespeed_clock Threads::Threads)
    gamespeed_warnings(gamespeed_clock_tests)
    add_test(NAME clock COMMAND gamespeed_clock_tests)
    if(TARGET game_speed_plugin)
        add_executable(gamespeed_host_plugin_tests tests/HostPluginTests.cpp)
        target_link_libraries(gamespeed_host_plugin_tests PRIVATE mcdk::plugin-abi)
        target_include_directories(gamespeed_host_plugin_tests PRIVATE external/MCDevTool/libs/nlohmann)
        add_dependencies(gamespeed_host_plugin_tests game_speed_plugin)
        gamespeed_warnings(gamespeed_host_plugin_tests)
        add_test(NAME host_plugin_abi COMMAND gamespeed_host_plugin_tests "$<TARGET_FILE:game_speed_plugin>")
    endif()
    if(TARGET gamespeed_hook)
        add_executable(gamespeed_loader_tests tests/LoaderReadinessTests.cpp)
        target_link_libraries(gamespeed_loader_tests PRIVATE gamespeed_client Threads::Threads)
        add_dependencies(gamespeed_loader_tests gamespeed_hook)
        gamespeed_warnings(gamespeed_loader_tests)
        add_test(NAME target_loader_readiness COMMAND gamespeed_loader_tests "$<TARGET_FILE:gamespeed_hook>")
        set_tests_properties(target_loader_readiness PROPERTIES TIMEOUT 20)
        add_executable(gamespeed_integration_tests tests/HookIntegration.cpp)
        target_link_libraries(gamespeed_integration_tests PRIVATE gamespeed_client opengl32 gdi32 user32 Threads::Threads)
        gamespeed_warnings(gamespeed_integration_tests)
        add_test(NAME injected_clock_and_ui COMMAND gamespeed_integration_tests "$<TARGET_FILE:gamespeed_hook>")
        set_tests_properties(injected_clock_and_ui PROPERTIES TIMEOUT 40)
        add_test(NAME compact_ui COMMAND gamespeed_integration_tests "$<TARGET_FILE:gamespeed_hook>" --compact)
        set_tests_properties(compact_ui PROPERTIES TIMEOUT 40)
    endif()
endif()
