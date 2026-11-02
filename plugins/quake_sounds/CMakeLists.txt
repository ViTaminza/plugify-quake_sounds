# quake_sounds — Plugify C++ port of the Metamod:Source plugin cs2-quake-sounds
# (Quake announcer sounds for kill streaks, special kills, round events and the C4 countdown).
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat and no helper library. The production
# quake_sounds.ini / quake_sounds.phrases.txt KeyValues files are read by the plugin-local KV1
# reader (src/kv1.cpp); the map gate, client/event/schema checks, native menus, commands, cookies
# and the C4 countdown live in src/plugin.cpp.
#
#   quake_sounds_core        engine-free core (KV1 reader, config/phrase files, kill logic, colour
#                            tags, sound bookkeeping; also used by tests/quake_core_tests.cpp)
#   plugify-quake_sounds     the plugin shared library (entry "bin/quake_sounds")
#   quake_sounds_tests       engine-free core tests
#   quake_sounds_plugin_tests  the real src/plugin.cpp compiled against recording s2sdk bindings

set(QUAKE_SOUNDS_VERSION "1.2.0")

# The plugin is published as a standalone repository too: the root CMakeLists.txt defines these
# targets, but a build of this directory alone must still work. Every fallback carries a
# plugin-unique target name (a generic one would collide in a combined build).
if(TARGET plugify_sdk)
    set(QUAKE_SOUNDS_PLUGIFY_SDK plugify_sdk)
else()
    add_library(quake_sounds_plugify_sdk INTERFACE)
    target_include_directories(quake_sounds_plugify_sdk INTERFACE "${CMAKE_SOURCE_DIR}/sdk/include")
    set(QUAKE_SOUNDS_PLUGIFY_SDK quake_sounds_plugify_sdk)
endif()

if(TARGET s2sdk_bindings)
    set(QUAKE_SOUNDS_S2SDK_BINDINGS s2sdk_bindings)
else()
    add_library(quake_sounds_s2sdk_bindings_obj OBJECT "${CMAKE_SOURCE_DIR}/sdk/include/s2sdk/s2sdk.cpp")
    target_include_directories(quake_sounds_s2sdk_bindings_obj SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(quake_sounds_s2sdk_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(QUAKE_SOUNDS_S2SDK_BINDINGS quake_sounds_s2sdk_bindings_obj)
endif()

if(TARGET permissions_bindings)
    set(QUAKE_SOUNDS_PERMISSIONS_BINDINGS permissions_bindings)
else()
    add_library(quake_sounds_permissions_bindings_obj OBJECT
            "${CMAKE_SOURCE_DIR}/sdk/include/permissions/permissions.cpp")
    target_include_directories(quake_sounds_permissions_bindings_obj SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(quake_sounds_permissions_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(QUAKE_SOUNDS_PERMISSIONS_BINDINGS quake_sounds_permissions_bindings_obj)
endif()

# hudmenu is an *optional* API dependency (an existing Plugify plugin, not a wrapper library): its
# GetClientMenuType() is the only source of the player's own "!menu" choice. Without it the plugin
# falls back to the old wrapper's HUD_LAYOUT default ("hud" when registered, else s2sdk "button").
if(TARGET hudmenu_bindings)
    set(QUAKE_SOUNDS_HUDMENU_BINDINGS hudmenu_bindings)
elseif(EXISTS "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    add_library(quake_sounds_hudmenu_bindings_obj OBJECT "${CMAKE_SOURCE_DIR}/sdk/include/hudmenu/hudmenu.cpp")
    target_include_directories(quake_sounds_hudmenu_bindings_obj SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
    target_compile_definitions(quake_sounds_hudmenu_bindings_obj PUBLIC _GLIBCXX_USE_CXX11_ABI=1)
    set(QUAKE_SOUNDS_HUDMENU_BINDINGS quake_sounds_hudmenu_bindings_obj)
else()
    message(FATAL_ERROR "quake_sounds needs the generated hudmenu consumer bindings "
            "(sdk/include/hudmenu/hudmenu.cpp)")
endif()

# --- core ---------------------------------------------------------------------
add_library(quake_sounds_core STATIC
        src/quake_core.cpp
        src/quake_config.cpp
        src/kv1.cpp)
target_include_directories(quake_sounds_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
# Engine-free: the KV1 reader, the config/phrase model and the kill logic include no plugify or
# s2sdk header, so the tests link this target alone.
target_compile_options(quake_sounds_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(quake_sounds_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-quake_sounds SHARED
        src/plugin.cpp
        src/sound_volume.cpp
        # Binding globals (__s2sdk_* / __permissions_* / __hudmenu_*) filled by the plugify
        # module from the plugins declared as dependencies.
        $<TARGET_OBJECTS:${QUAKE_SOUNDS_S2SDK_BINDINGS}>
        $<TARGET_OBJECTS:${QUAKE_SOUNDS_PERMISSIONS_BINDINGS}>
        $<TARGET_OBJECTS:${QUAKE_SOUNDS_HUDMENU_BINDINGS}>
)
target_include_directories(plugify-quake_sounds PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/permissions/plg headers must not fail -Werror on our
# sources.
target_include_directories(plugify-quake_sounds SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-quake_sounds PRIVATE
        quake_sounds_core
        ${QUAKE_SOUNDS_PLUGIFY_SDK}
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
# The top-level CMakeLists.txt declares the PLUGIFY_PORT_TESTS option *after* the plugin
# directories, so on a first configure it is not defined here yet: default it to the same ON. An
# explicit -DPLUGIFY_PORT_TESTS=OFF is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
    # Engine-free tests of the core against the real config/phrases files.
    add_executable(quake_sounds_tests tests/quake_core_tests.cpp)
    target_link_libraries(quake_sounds_tests PRIVATE quake_sounds_core -static-libstdc++ -static-libgcc)
    target_compile_options(quake_sounds_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(quake_sounds_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            QS_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data")
    # Config/resource parity against the live Metamod reference when this checkout has it; the
    # standalone repository simply skips those checks.
    if(EXISTS "${CMAKE_SOURCE_DIR}/ref/prod-27016/addons/configs/quake_sounds.ini")
        target_compile_definitions(quake_sounds_tests PRIVATE
                QS_PROD_CONFIG_PATH="${CMAKE_SOURCE_DIR}/ref/prod-27016/addons/configs/quake_sounds.ini")
    endif()
    if(EXISTS "${CMAKE_SOURCE_DIR}/ref/prod-27016/addons/translations/quake_sounds.phrases.txt")
        target_compile_definitions(quake_sounds_tests PRIVATE
                QS_PROD_PHRASES_PATH="${CMAKE_SOURCE_DIR}/ref/prod-27016/addons/translations/quake_sounds.phrases.txt")
    endif()
    set_target_properties(quake_sounds_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

    # The real plugin TU is compiled into the test; every __s2sdk_* / __permissions_* binding slot
    # is a recording fake (see tests/quake_sounds_plugin_tests.cpp).
    add_executable(quake_sounds_plugin_tests
            tests/quake_sounds_plugin_tests.cpp
            src/sound_volume.cpp
            $<TARGET_OBJECTS:${QUAKE_SOUNDS_S2SDK_BINDINGS}>
            $<TARGET_OBJECTS:${QUAKE_SOUNDS_PERMISSIONS_BINDINGS}>
            $<TARGET_OBJECTS:${QUAKE_SOUNDS_HUDMENU_BINDINGS}>
    )
    target_include_directories(quake_sounds_plugin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_include_directories(quake_sounds_plugin_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
    target_link_libraries(quake_sounds_plugin_tests PRIVATE
            quake_sounds_core
            ${QUAKE_SOUNDS_PLUGIFY_SDK}
            -static-libstdc++ -static-libgcc)
    target_compile_options(quake_sounds_plugin_tests PRIVATE -Wall -Wextra -Werror)
    target_compile_definitions(quake_sounds_plugin_tests PRIVATE
            _GLIBCXX_USE_CXX11_ABI=1
            PLUGIFY_MODULE="quake_sounds"
            PLUGIFY_IS_DEBUG=0
            PLUGIFY_IS_RELEASE=1
            QUAKE_SOUNDS_TEST_CONFIG="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/configs/quake_sounds.ini"
            QUAKE_SOUNDS_TEST_PHRASES="${CMAKE_CURRENT_SOURCE_DIR}/tests/data/translations/quake_sounds.phrases.txt")
    set_target_properties(quake_sounds_plugin_tests PROPERTIES
            RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
