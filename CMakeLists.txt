cmake_minimum_required(VERSION 3.20)
set(PSXRECOMP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/psxrecomp" CACHE PATH
    "Path to psxrecomp")
if(NOT EXISTS "${PSXRECOMP_ROOT}/runtime/runtime.cmake")
    message(FATAL_ERROR
        "PSXRecomp runtime is missing: ${PSXRECOMP_ROOT}/runtime/runtime.cmake\n"
        "This project expects the psxrecomp framework checkout at ${PSXRECOMP_ROOT}.\n"
        "Run: git submodule update --init --recursive\n"
        "If psxrecomp/ is not a submodule, set -DPSXRECOMP_ROOT=<path-to-psxrecomp>.")
endif()

project(BugsBunnyLostinTime LANGUAGES C CXX)

# --- Opt-in product surfaces (OFF by default in the framework) -------------
# Scaffold prompts / flags fill these. Without recomp-ui, PSX_RECOMP_UI is OFF.
# The wizard (WIZARD_CMAKE_BLOCK / ENABLE_SETUP_WIZARD) is the first-run disc
# picker; releases are compiled from the committed generated/ C, so it never
# needs to generate or rebuild anything on a player's machine.
# recomp-ui submodule present (PSX_RECOMP_UI defaults ON).
# Full netplay UI — enable after adding recomp-ui + testing:
# if(EXISTS "${PSXRECOMP_ROOT}/lib/recomp-net/CMakeLists.txt")
#     set(PSX_NETPLAY ON CACHE BOOL "…" FORCE)
# endif()
set(PSX_SETUP_WIZARD ON CACHE BOOL
    "Advertise first-run setup wizard + Generate & rebuild in recomp-ui" FORCE)
include("${PSXRECOMP_ROOT}/runtime/runtime.cmake")

# This project's own mod catalog. The FRAMEWORK stages it -- together with its
# builtin psx.* packages -- into <exe-dir>/mods/bundled, and refuses to
# configure if a catalog exists here but is not declared. Never copy it by
# hand: five titles did, and framework 4cc04be3's rename of the staged
# directory broke all five silently (bead beads-eio.3.101).
set(PSX_PRELOADED_MODS "${CMAKE_CURRENT_SOURCE_DIR}/mods/preloaded")
if(NOT IS_DIRECTORY "${PSX_PRELOADED_MODS}")
    set(PSX_PRELOADED_MODS "NONE")
endif()

psxrecomp_add_game_runtime(psx-runtime
    PRELOADED_MODS_DIR "${PSX_PRELOADED_MODS}"
    # ENABLE_NETPLAY_IF_PRESENT
    # NETPLAY_LOBBY_URL "ws://netplay.retcomm.net:8765"
    ENABLE_SETUP_WIZARD
    WINDOW_TITLE "BugsBunnyLostinTime"
    DEFAULT_GAME_CONFIG_PATH "game.toml"
    MAX_PLAYERS 1
    # LAUNCHER_BOXART "${CMAKE_CURRENT_SOURCE_DIR}/launcher_assets/img/boxart.tga"
    APP_ICON "${CMAKE_CURRENT_SOURCE_DIR}/assets/psxrecomp.ico"
    # Committed game C (psxrecomp_cli.py generate; git add generated). Release
    # CI configures with -DPSXRECOMP_REQUIRE_GAME_C=ON so a checkout without
    # it fails instead of building a setup host.
    GEN_MARKER "generated/SLUS_008.38_dispatch.c"
    GEN_FULL_GLOB "generated/SLUS_008.38_full_*.c"
    CODEGEN_SETUP_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/codegen_setup.c"
)

# Bugs Bunny mods: native widescreen (16:9).
target_sources(psx-runtime PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/mods/src/widescreen.c"
)

target_include_directories(psx-runtime PRIVATE
    "${PSXRECOMP_ROOT}/runtime/include"
)

# Stage runtime configs next to the exe for direct build/ launches. A file-level
# rule rather than POST_BUILD, so editing only game.toml restages it without a
# relink (POST_BUILD ran only when the exe was relinked). The exe directory is
# resolved here: $<TARGET_FILE_DIR:psx-runtime> would make this rule depend on
# psx-runtime, a cycle. Only copy files that exist — game_options.toml is
# optional.
get_target_property(_PSX_RUNTIME_DIR psx-runtime RUNTIME_OUTPUT_DIRECTORY)
if(NOT _PSX_RUNTIME_DIR)
    if(CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(_PSX_RUNTIME_DIR "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
    else()
        set(_PSX_RUNTIME_DIR "${CMAKE_CURRENT_BINARY_DIR}")
    endif()
endif()
get_property(_PSX_MULTI_CONFIG GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(_PSX_MULTI_CONFIG)
    string(APPEND _PSX_RUNTIME_DIR "/$<CONFIG>")
endif()
set(_PSX_STAGED_RUNTIME_CONFIGS "")
foreach(_PSX_RUNTIME_CONFIG game.toml game_options.toml)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}")
        set(_PSX_STAGED "${_PSX_RUNTIME_DIR}/${_PSX_RUNTIME_CONFIG}")
        # copy (not copy_if_different) refreshes the output's timestamp, so the
        # rule is satisfied until the source changes again.
        add_custom_command(OUTPUT "${_PSX_STAGED}"
            COMMAND ${CMAKE_COMMAND} -E copy
                "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}"
                "${_PSX_STAGED}"
            DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/${_PSX_RUNTIME_CONFIG}"
            COMMENT "Staging ${_PSX_RUNTIME_CONFIG}"
            VERBATIM)
        list(APPEND _PSX_STAGED_RUNTIME_CONFIGS "${_PSX_STAGED}")
    endif()
endforeach()
add_custom_target(psx-runtime-configs ALL DEPENDS ${_PSX_STAGED_RUNTIME_CONFIGS})
add_dependencies(psx-runtime psx-runtime-configs)
