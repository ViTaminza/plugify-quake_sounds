# quake_sounds — Plugify C++ port of the Metamod:Source plugin cs2-quake-sounds
# (Quake announcer sounds for kill streaks, special kills, round events and the C4 countdown).
#
#   quake_sounds_core       engine-free core (config, phrases, kill logic; also used by
#                           test/quake_core_tests.cpp)
#   plugify-quake_sounds    the plugin shared library (entry "bin/quake_sounds")

set(QUAKE_SOUNDS_VERSION "1.0.1")

# --- core ---------------------------------------------------------------------
add_library(quake_sounds_core STATIC src/quake_core.cpp)
target_include_directories(quake_sounds_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# pisexcompat brings the KeyValues parser and, through its own PUBLIC link, the s2sdk binding
# globals (they stay null in the tests, which never call into s2sdk).
target_link_libraries(quake_sounds_core PUBLIC pisexcompat)
target_compile_options(quake_sounds_core PRIVATE -Wall -Wextra -Werror)
# SYSTEM: warnings from the generated s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(quake_sounds_core SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_definitions(quake_sounds_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-quake_sounds SHARED
        src/plugin.cpp
        src/sound_volume.cpp
        # Binding globals (__s2sdk_* / __permissions_* / __hudmenu_*) filled by the plugify
        # module from the plugins declared as dependencies.
        $<TARGET_OBJECTS:s2sdk_bindings>
        $<TARGET_OBJECTS:permissions_bindings>
)
target_include_directories(plugify-quake_sounds PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/permissions/plg/pisexcompat headers must not fail
# -Werror on our sources.
target_include_directories(plugify-quake_sounds SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-quake_sounds PRIVATE
        quake_sounds_core
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-quake_sounds PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-quake_sounds PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="quake_sounds"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-quake_sounds PROPERTIES OUTPUT_NAME quake_sounds)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-quake_sounds PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/quake_sounds.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/quake_sounds.pplugin"
)

# --- tests ---------------------------------------------------------------------
# Engine-free tests of the core against the real config/phrases files.
add_executable(quake_sounds_tests test/quake_core_tests.cpp)
target_link_libraries(quake_sounds_tests PRIVATE quake_sounds_core -static-libstdc++ -static-libgcc)
target_compile_options(quake_sounds_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(quake_sounds_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        QS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/test/data")
set_target_properties(quake_sounds_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
