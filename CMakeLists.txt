cmake_minimum_required(VERSION 3.16)
project(SonicTheHedgehogRecomp C)

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# Twin-build: a single configure produces BOTH binaries.
#
# Single build: SonicTheHedgehogRecomp.exe — recompiled 68K on the clean-room
# OWN BACKEND (VDP + bus + Z80 scheduler). There is one backend and it is ours;
# no emulator core is compiled or linked anywhere in this project.
#
# The paired _oracle target is gone. It ran the AGPL clown68000 interpreter as
# a verification reference, went stale, and forced a private submodule into
# every clone. The surviving cross-check is the cosim harness below, which
# diffs the recompiled code against our own Tier-3 interpreter.
#
# Runner code is shared with Sonic 2 and lives in the submodule at
# segagenesisrecomp/runner/. Per-game files (sonic1_spec.c, sonic_extras.c)
# live alongside the Sonic 1 generated TUs in
# segagenesisrecomp/sonicthehedgehog/.
# ---------------------------------------------------------------------------

set(RECOMP_ROOT "${CMAKE_SOURCE_DIR}/segagenesisrecomp")
set(RUNNER_ROOT "${RECOMP_ROOT}/runner")

option(SONIC_REVERSE_DEBUG
    "Enable Tier-1 reverse-debugger hooks (sgrdb_write*, ring buffer, rdb_* TCP cmds). \
Requires a regen with GenesisRecomp --reverse-debug; mixing flagged/unflagged \
generated C against the other runner variant is a link error by design."
    OFF)
option(GEN_DEV_TRACE
    "Compile the dev chip/snd trace rings + dumps (NEVER in release builds)" OFF)
# Prod-vs-debug TCP strip. ON (default) keeps the always-on dev tooling so the
# normal dev workflow is unchanged: the cmd_server TCP listener + frame_snapshots
# ring. OFF (release: tools/build-linux.sh --config prod) swaps cmd_server.c for
# its no-op stub and drops frame_snapshots.c (referenced only by cmd_server), so
# the shipped native binary opens no port and carries no query ring. Combined
# with GEN_DEV_TRACE=OFF (which makes chip_trace.c self-stub) the release build
# is fully tooling-free.
option(GEN_ENABLE_TRACE
    "Build the TCP cmd server + observability rings (frame_snapshots). OFF for release." ON)
if(GEN_ENABLE_TRACE)
    set(GEN_TRACE_SOURCES
        "${RUNNER_ROOT}/cmd_server.c"
        "${RUNNER_ROOT}/frame_snapshots.c")
    add_compile_definitions(GEN_ENABLE_TRACE=1)
else()
    set(GEN_TRACE_SOURCES "${RUNNER_ROOT}/cmd_server_stub.c")
endif()

# SDL2 discovery. Windows uses the bundled VC devel package; macOS/Linux
# use the system/Homebrew SDL2 (on Apple Silicon that's /opt/homebrew).
if(WIN32)
    list(APPEND CMAKE_PREFIX_PATH
        "${RUNNER_ROOT}/external/SDL2/SDL2-2.28.5/cmake")
elseif(APPLE)
    find_program(BREW_EXECUTABLE brew)
    if(BREW_EXECUTABLE)
        execute_process(COMMAND "${BREW_EXECUTABLE}" --prefix sdl2
            OUTPUT_VARIABLE _brew_sdl2_prefix
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET RESULT_VARIABLE _brew_sdl2_rc)
        if(_brew_sdl2_rc EQUAL 0 AND _brew_sdl2_prefix)
            list(APPEND CMAKE_PREFIX_PATH "${_brew_sdl2_prefix}")
        endif()
    endif()
endif()
find_package(SDL2 REQUIRED)

# The runner includes SDL as <SDL2/SDL.h> (the layout of the bundled Windows
# devel package, whose include root holds an SDL2/ subdir). Homebrew's
# SDL2::SDL2 instead points the include dir *at* .../include/SDL2, so add its
# parent so <SDL2/SDL.h> resolves the same way on every platform.
set(SDL2_PARENT_INCLUDE_DIRS "")
foreach(_inc IN LISTS SDL2_INCLUDE_DIRS)
    get_filename_component(_inc_name "${_inc}" NAME)
    if(_inc_name STREQUAL "SDL2")
        get_filename_component(_inc_parent "${_inc}" DIRECTORY)
        list(APPEND SDL2_PARENT_INCLUDE_DIRS "${_inc_parent}")
    endif()
endforeach()

# ---------------------------------------------------------------------------
# YM2612 FM = vendored ymfm (Aaron Giles, BSD-3) behind the stable ym2612_*
# API (runner/audio/ym2612.h). Replaced clownmdemu's FM (AGPL) 2026-06-05 as
# part of the AGPL-free runtime initiative — see segagenesisrecomp/LICENSING.md
# (teardown item #2). enable_language(CXX) because ymfm is C++.
# ---------------------------------------------------------------------------
enable_language(CXX)
set(CMAKE_CXX_STANDARD 17)
set(FM_SOURCES
    "${RUNNER_ROOT}/audio/ym2612_ymfm.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_opn.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_ssg.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_adpcm.cpp"
    "${RUNNER_ROOT}/external/ymfm/src/ymfm_pcm.cpp"
)

# Backend split (teardown #4/#5/#10, validated on Sonic 1):
#   - Native target  = the clean-room own backend (VDP+bus+Z80 scheduler). It is
#     THE Sonic 1 runtime — not a toggle — and links ZERO clownmdemu (AGPL-free).
# It is the only backend: the OWN_BACKEND conditionals were removed from the
# runner once every game migrated, so there is nothing left to select.
set(OWN_BACKEND_SOURCES
    "${RUNNER_ROOT}/video/genesis_vdp.c"
    "${RUNNER_ROOT}/video/genesis_bus.c"
    "${RUNNER_ROOT}/video/genesis_machine.c"
    "${RUNNER_ROOT}/external/superzazu/z80.c")   # MIT Z80 core, embedded by genesis_machine

# Tier-3 floor + differential co-sim: the clean-room 68000 interpreter and the
# recompiler's decoder. Engine master's glue.c references m68k_interp_run_framed
# unconditionally, so every target links these (game master predates the floor).
set(NATIVE_FLOOR_SOURCES
    "${RUNNER_ROOT}/m68k_interp.c"
    "${RECOMP_ROOT}/recompiler/src/m68k_decoder.c"
    "${RECOMP_ROOT}/recompiler/src/rom_parser.c")

# ---------------------------------------------------------------------------
# Shared sources / includes / link libs
# ---------------------------------------------------------------------------

include("${RECOMP_ROOT}/cmake/GenesisRecompGenerated.cmake")
genesisrecomp_add_generated_sources(
    SONIC_GENERATED_SOURCES sonic sonicthehedgehog sonic.bin
    REVERSE_DEBUG "${SONIC_REVERSE_DEBUG}")

set(SONIC_SHARED_SOURCES
    "${RUNNER_ROOT}/main.c"
    "${RUNNER_ROOT}/audio.c"
    "${RUNNER_ROOT}/chip_trace.c"   # self-stubs unless GEN_DEV_TRACE=ON (no-op in release)
    ${GEN_TRACE_SOURCES}            # cmd_server.c + frame_snapshots.c, or cmd_server_stub.c (GEN_ENABLE_TRACE=OFF)
    "${RUNNER_ROOT}/glue.c"
    "${RUNNER_ROOT}/fiber_compat.c"   # cross-platform fiber shim (Win32 Fibers / POSIX ucontext) — glue.c depends on it
    "${RUNNER_ROOT}/crash_report.c"
    "${RUNNER_ROOT}/input_script.c"
    "${RUNNER_ROOT}/gamepad.c"
    "${RUNNER_ROOT}/input_map.c"      # per-player Genesis button bindings (kbd+pad, 3/6-btn)
    "${RUNNER_ROOT}/app_config.c"     # settings.ini + rom.cfg load/save
    "${RUNNER_ROOT}/audio/event_queue.c"
    ${FM_SOURCES}
    "${RUNNER_ROOT}/audio/sn76489.c"
    "${RUNNER_ROOT}/audio/mixer.c"
    "${RUNNER_ROOT}/audio/observability.c"
    # Verified-enhancement shadow layer (opt-in via env, default OFF; links no
    # clownmdemu, safe to ship). See segagenesisrecomp/docs/SHADOW_ENHANCEMENTS.md.
    "${RUNNER_ROOT}/audio/audio_shadow.c"   # envelope-correlation verifier
    "${RUNNER_ROOT}/audio/fm_shadow.cpp"    # parallel ymfm chip (cleaner highs / ym3438 ladder-off)
    "${RUNNER_ROOT}/video/color_lut.c"      # present-time 9-bit CRT/composite LUT
    ${SONIC_GENERATED_SOURCES}
    "${RECOMP_ROOT}/sonicthehedgehog/sonic1_spec.c"
    "${RECOMP_ROOT}/sonicthehedgehog/sonic_extras.c"
    "${RECOMP_ROOT}/sonicthehedgehog/extras.c"
    ${NATIVE_FLOOR_SOURCES}         # m68k_interp + decoder (engine-master glue.c needs it)
)

if(SONIC_REVERSE_DEBUG)
    list(APPEND SONIC_SHARED_SOURCES "${RUNNER_ROOT}/reverse_debug.c")
    message(STATUS "SONIC_REVERSE_DEBUG=ON — regen with GenesisRecomp --reverse-debug")
endif()

# Include paths. clowncommon is ISC-permissive and vendored at
# runner/external/clowncommon; nothing else third-party is on this list.
set(SONIC_INCLUDES_NATIVE
    "${RUNNER_ROOT}"
    "${RUNNER_ROOT}/include"
    "${RUNNER_ROOT}/external/ymfm/src"
    "${RUNNER_ROOT}/external/superzazu"
    "${RUNNER_ROOT}/external/clowncommon"
    "${RUNNER_ROOT}/video"
    "${RECOMP_ROOT}/recompiler/src"    # m68k_decoder.h / rom_parser.h (Tier-3 floor + cosim)
    "${RECOMP_ROOT}/sonicthehedgehog"
)

set(SONIC_SHARED_LIBS SDL2::SDL2 SDL2::SDL2main)
if(WIN32)
    list(APPEND SONIC_SHARED_LIBS ws2_32 comdlg32)
endif()

function(_sonic_finalize_target tgt includes)
    target_include_directories(${tgt} PRIVATE ${${includes}})
    target_link_libraries(${tgt} PRIVATE ${SONIC_SHARED_LIBS})
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W3 /WX- /utf-8 /wd4102 /wd4996)
        target_link_options(${tgt} PRIVATE /MAP)
    endif()
    if(WIN32)
        # SDL2 is a DLL on Windows; macOS/Linux link the shared lib via rpath.
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RUNNER_ROOT}/external/SDL2/SDL2-2.28.5/lib/x64/SDL2.dll"
                "$<TARGET_FILE_DIR:${tgt}>"
            COMMENT "Copying SDL2.dll for ${tgt}")
    endif()
    # Annotation CSV used by crash_report.c for symbol resolution (all platforms).
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RECOMP_ROOT}/sonicthehedgehog/annotations_from_disasm.csv"
            "$<TARGET_FILE_DIR:${tgt}>/annotations_from_disasm.csv"
        COMMENT "Copying Sonic 1 annotations for crash-report symbol resolution")
endfunction()

# ---------------------------------------------------------------------------
# Native target — recompiled 68K drives the game
# ---------------------------------------------------------------------------

add_executable(SonicTheHedgehogRecomp
    ${SONIC_SHARED_SOURCES}
    ${OWN_BACKEND_SOURCES}          # clean-room VDP+bus+Z80 scheduler (native = own backend)
)
add_dependencies(SonicTheHedgehogRecomp ${SONIC_GENERATED_SOURCES_TARGET})
target_compile_definitions(SonicTheHedgehogRecomp PRIVATE
    INCLUDE_GENERATED_CODE=1
    ENABLE_RECOMPILED_CODE=1
    OWN_BACKEND=1                   # the own backend IS the Sonic 1 runtime (no clownmdemu link)
    DEFAULT_DEBUG_PORT=4378
    $<$<BOOL:${SONIC_REVERSE_DEBUG}>:SONIC_REVERSE_DEBUG=1>
    $<$<BOOL:${GEN_DEV_TRACE}>:GEN_DEV_TRACE>
)
_sonic_finalize_target(SonicTheHedgehogRecomp SONIC_INCLUDES_NATIVE)

# Shared recomp-ui (Dear ImGui) pre-boot launcher — native (shipped) target
# only. Adds the launcher sources + vendored ImGui (all permissive), defines
# RECOMP_LAUNCHER (which un-gates the seam in the runner's main.c), links the
# system GL, and stages assets/ next to the exe. The cosim target below
# deliberately does NOT get the launcher.
include(${CMAKE_SOURCE_DIR}/recomp-ui/recomp_ui.cmake)
recomp_target_launcher_ui(SonicTheHedgehogRecomp BOXART ${CMAKE_SOURCE_DIR}/boxart.tga)

# ---------------------------------------------------------------------------
# Framework tests (L1 decoder harness). Opt-in via -DBUILD_TESTS=ON.
# ---------------------------------------------------------------------------

option(BUILD_TESTS "Build L1 decoder test harness" OFF)
if(BUILD_TESTS)
    set(GENESIS_SONIC_GENERATED_DIR "${SONIC_GENERATED_SOURCES_DIR}")
    add_subdirectory("${RECOMP_ROOT}/tests" tests)
endif()

# ---------------------------------------------------------------------------
# Differential co-simulation target — dev-only, never shipped.
#
# SonicTheHedgehogRecomp_cosim.exe: the OWN-BACKEND recomp runtime plus the
# #ifdef GENESIS_COSIM full-state hash module (cosim_state.c) + lockstep TCP
# server (cosim.c). Two instances driven by tools/genesis_cosim.py compare full
# architectural state at aligned checkpoints and halt at the first divergence.
# GENESIS_FORCE_INTERP (env, added later) selects the m68k_interp backend for
# pairing #1 (recomp-vs-interp). Links no clownmdemu.
# ---------------------------------------------------------------------------
option(GENESIS_BUILD_COSIM "Build the differential co-simulation target (dev-only)." OFF)
if(GENESIS_BUILD_COSIM)
    add_executable(SonicTheHedgehogRecomp_cosim
        ${SONIC_SHARED_SOURCES}
        ${OWN_BACKEND_SOURCES}
        "${RUNNER_ROOT}/cosim.c"
        "${RUNNER_ROOT}/cosim_state.c"
        "${RUNNER_ROOT}/cosim_visible.c"                      # visible-surface hashes + memchunks localizer
        "${RUNNER_ROOT}/cosim_cycles.c"                       # cross-backend work-cycle ruler
        "${RUNNER_ROOT}/game_cycles.c"                        # cost-table lookup
        "${SONIC_GENERATED_SOURCES_CYCLES}"                  # model-stamped costs
    )
    add_dependencies(SonicTheHedgehogRecomp_cosim ${SONIC_GENERATED_SOURCES_TARGET})
    target_compile_definitions(SonicTheHedgehogRecomp_cosim PRIVATE
        INCLUDE_GENERATED_CODE=1
        ENABLE_RECOMPILED_CODE=1
        OWN_BACKEND=1
        GENESIS_COSIM=1
        DEFAULT_DEBUG_PORT=4390
        $<$<BOOL:${SONIC_REVERSE_DEBUG}>:SONIC_REVERSE_DEBUG=1>
    )
    _sonic_finalize_target(SonicTheHedgehogRecomp_cosim SONIC_INCLUDES_NATIVE)
    # No launcher (headless/deterministic); the coordinator drives it over TCP.
    message(STATUS "GENESIS_BUILD_COSIM=ON — differential co-sim target enabled")

endif()
