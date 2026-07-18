cmake_minimum_required(VERSION 3.16)
project(SonicTheHedgehogRecomp C)

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# Twin-build: a single configure produces BOTH binaries.
#
#   SonicTheHedgehogRecomp.exe         — native: recompiled 68K + the clean-room
#                                        OWN BACKEND (VDP+bus+Z80 scheduler).
#                                        Links ZERO clownmdemu — AGPL-free, this
#                                        is what ships.
#   SonicTheHedgehogRecomp_oracle.exe  — oracle: full clown68000 interpreter +
#                                        clownmdemu VDP/bus, the verification
#                                        ground truth. Links clownmdemu-core
#                                        (AGPL); dev-only, never shipped.
#
# Only the oracle links clownmdemu-core. The native target adds stub_clown68000.c
# (Clown68000_DoCycles/Reset/Interrupt as harmless no-ops; nothing calls them now
# that the lib is unlinked). The oracle omits the stubs, so the real interpreter
# from clown68000-interpreter.lib is pulled in.
#
# Default debug-server ports differ per target (4378 native / 4379 oracle)
# so both can run simultaneously for side-by-side ring-buffer comparison.
#
# Runner code is shared with Sonic 2 and lives in the submodule at
# segagenesisrecomp/runner/. Per-game files (sonic1_spec.c, sonic_extras.c,
# sonic1_hybrid_table.c) live alongside the Sonic 1 generated TUs in
# segagenesisrecomp/sonicthehedgehog/.
# ---------------------------------------------------------------------------

set(GENESIS_RECOMP_ROOT "" CACHE PATH
    "Optional shared Genesis framework checkout (defaults to the submodule)")
if(GENESIS_RECOMP_ROOT)
    set(RECOMP_ROOT "${GENESIS_RECOMP_ROOT}")
else()
    set(RECOMP_ROOT "${CMAKE_SOURCE_DIR}/segagenesisrecomp")
endif()
set(RUNNER_ROOT "${RECOMP_ROOT}/runner")

option(GENESIS_Z80_RECOMP
    "Run the Genesis sound Z80 through a flat statically-recompiled backend" OFF)
set(GENESIS_Z80_CORE_ROOT "" CACHE PATH
    "SMS/GG Z80 semantic-core checkout used by GENESIS_Z80_RECOMP")
set(GENESIS_Z80_AOT_SOURCE "" CACHE FILEPATH
    "Generated <game>_step.c emitted by SmsRecomp --flat-step")
if(GENESIS_Z80_RECOMP)
    if(NOT EXISTS "${GENESIS_Z80_CORE_ROOT}/runner/include/sms_runtime.h")
        message(FATAL_ERROR "GENESIS_Z80_CORE_ROOT must point at an smsggrecomp checkout")
    endif()
    if(NOT EXISTS "${GENESIS_Z80_AOT_SOURCE}")
        message(FATAL_ERROR "GENESIS_Z80_AOT_SOURCE does not exist: ${GENESIS_Z80_AOT_SOURCE}")
    endif()
    get_filename_component(GENESIS_Z80_AOT_DIR "${GENESIS_Z80_AOT_SOURCE}" DIRECTORY)
endif()

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

# clownmdemu-core is the mstan fork; our changes are committed in it directly
# (no build-time patch step — see THIRD-PARTY-LICENSES.md).
add_subdirectory("${RECOMP_ROOT}/clownmdemu-core" clownmdemu-core)

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

# ---------------------------------------------------------------------------
# Clean-room VDP (genesis_vdp) — AGPL-free runtime teardown item #4. While
# PERMISSIVE_VDP is ON it runs as a SHADOW (mirrors VDP-port writes, substitutes
# its rendered scanlines for display) so the renderer can be validated against
# clownmdemu before clownmdemu's VDP is dropped. Default OFF. See LICENSING.md.
# ---------------------------------------------------------------------------
option(PERMISSIVE_VDP "Shadow the clean-room genesis_vdp for validation." OFF)
set(VDP_SOURCES "")
if(PERMISSIVE_VDP)
    list(APPEND VDP_SOURCES
        "${RUNNER_ROOT}/video/genesis_vdp.c"
        "${RUNNER_ROOT}/video/vdp_integration.c")
    add_compile_definitions(PERMISSIVE_VDP=1)
    message(STATUS "PERMISSIVE_VDP=ON — clean-room VDP shadow build")
endif()

# Backend split (teardown #4/#5/#10, validated on Sonic 1):
#   - Native target  = the clean-room own backend (VDP+bus+Z80 scheduler). It is
#     THE Sonic 1 runtime — not a toggle — and links ZERO clownmdemu (AGPL-free).
#   - Oracle target  = clownmdemu (clown68000 interpreter + clownmdemu VDP/bus),
#     the dev-only verification reference; never shipped.
# OWN_BACKEND is therefore defined intrinsically on the native target below, not
# as a user option. (S2/S3 still build on clownmdemu until their own backend is
# validated; the engine keeps the OWN_BACKEND conditional until they migrate.)
set(OWN_BACKEND_SOURCES
    "${RUNNER_ROOT}/video/genesis_vdp.c"
    "${RUNNER_ROOT}/video/genesis_bus.c"
    "${RUNNER_ROOT}/video/genesis_machine.c"
    "${RUNNER_ROOT}/external/superzazu/z80.c")   # MIT Z80 core, embedded by genesis_machine
if(GENESIS_Z80_RECOMP)
    list(APPEND OWN_BACKEND_SOURCES
        "${RUNNER_ROOT}/z80_recomp.c"
        "${GENESIS_Z80_AOT_SOURCE}")
endif()

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
    "${RUNNER_ROOT}/hybrid_global.c"
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

# Include paths. THE NATIVE LIST MUST CONTAIN NO clownmdemu-core PATHS —
# that is the enforcement that no AGPL header can be compiled into a release
# binary (clowncommon is ISC-permissive and vendored at
# runner/external/clowncommon). The oracle keeps the full clownmdemu paths.
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
if(GENESIS_Z80_RECOMP)
    list(APPEND SONIC_INCLUDES_NATIVE
        "${GENESIS_Z80_CORE_ROOT}/runner/include"
        "${GENESIS_Z80_AOT_DIR}")
endif()
set(SONIC_INCLUDES_ORACLE
    "${RUNNER_ROOT}"
    "${RUNNER_ROOT}/include"
    "${RUNNER_ROOT}/external/ymfm/src"
    "${RUNNER_ROOT}/external/superzazu"
    "${RUNNER_ROOT}/video"
    "${RECOMP_ROOT}/recompiler/src"    # m68k_decoder.h / rom_parser.h (floor symbol in glue.c)
    "${RECOMP_ROOT}/clownmdemu-core/libraries/clownz80/source"
    "${RECOMP_ROOT}/sonicthehedgehog"
    "${RECOMP_ROOT}/clownmdemu-core/source"
    "${RECOMP_ROOT}/clownmdemu-core/libraries/clowncommon"
    "${RECOMP_ROOT}/clownmdemu-core/libraries/clown68000/source/interpreter"
    ${SDL2_PARENT_INCLUDE_DIRS}
)

set(SONIC_SHARED_LIBS SDL2::SDL2 SDL2::SDL2main)
if(WIN32)
    list(APPEND SONIC_SHARED_LIBS ws2_32 comdlg32)
endif()
# clownmdemu-core (AGPL) is linked ONLY by the oracle target (see below). The
# native target is the own backend and links zero clownmdemu code.

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
    $<$<BOOL:${GENESIS_Z80_RECOMP}>:GENESIS_Z80_RECOMP=1>
    $<$<BOOL:${GENESIS_Z80_RECOMP}>:SMS_RUNTIME_NO_CALL_BY_ADDRESS=1>
    $<$<BOOL:${SONIC_REVERSE_DEBUG}>:SONIC_REVERSE_DEBUG=1>
    $<$<BOOL:${GEN_DEV_TRACE}>:GEN_DEV_TRACE>
)
_sonic_finalize_target(SonicTheHedgehogRecomp SONIC_INCLUDES_NATIVE)

# Shared recomp-ui (Dear ImGui) pre-boot launcher — native (shipped) target
# only. Adds the launcher sources + vendored ImGui (all permissive), defines
# RECOMP_LAUNCHER (which un-gates the seam in the runner's main.c), links the
# system GL, and stages assets/ next to the exe. Replaces the old RmlUi launcher
# (launcher.cmake / GENESIS_LAUNCHER). The dev-only _oracle target below
# deliberately does NOT get the launcher.
include(${CMAKE_SOURCE_DIR}/recomp-ui/recomp_ui.cmake)
recomp_target_launcher_ui(SonicTheHedgehogRecomp BOXART ${CMAKE_SOURCE_DIR}/boxart.tga)

# ---------------------------------------------------------------------------
# Oracle target — clown68000 interpreter executes everything
# ---------------------------------------------------------------------------

set(SONIC_ORACLE_EXTRA_SOURCES
    "${RUNNER_ROOT}/hybrid.c"
    "${RECOMP_ROOT}/sonicthehedgehog/sonic1_hybrid_table.c"  # per-game (Sonic 1's verified-clean overrides)
    "${RUNNER_ROOT}/hybrid_jmp.c"
)
if(SONIC_REVERSE_DEBUG)
    # Tier 3: per-instruction capture on the oracle side. Oracle-only
    # because native uses stub_clown68000.c and never runs the
    # interpreter.
    list(APPEND SONIC_ORACLE_EXTRA_SOURCES "${RUNNER_ROOT}/oracle_trace.c")
endif()

add_executable(SonicTheHedgehogRecomp_oracle
    ${SONIC_SHARED_SOURCES}
    ${SONIC_ORACLE_EXTRA_SOURCES}
)
add_dependencies(SonicTheHedgehogRecomp_oracle ${SONIC_GENERATED_SOURCES_TARGET})
target_compile_definitions(SonicTheHedgehogRecomp_oracle PRIVATE
    INCLUDE_GENERATED_CODE=1
    HYBRID_RECOMPILED_CODE=1
    SONIC_ORACLE_BUILD=1
    DEFAULT_DEBUG_PORT=4379
    $<$<BOOL:${SONIC_REVERSE_DEBUG}>:SONIC_REVERSE_DEBUG=1>
    $<$<BOOL:${GEN_DEV_TRACE}>:GEN_DEV_TRACE>
)
_sonic_finalize_target(SonicTheHedgehogRecomp_oracle SONIC_INCLUDES_ORACLE)
# The oracle runs clownmdemu's clown68000 interpreter + VDP/bus as ground truth,
# so it (and only it) links the AGPL core. Dev-only; never shipped.
target_link_libraries(SonicTheHedgehogRecomp_oracle PRIVATE clownmdemu-core)

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
        "${RUNNER_ROOT}/cosim_visible.c"                      # pairing #2 A-side (visible surface)
        "${RUNNER_ROOT}/cosim_cycles.c"                       # cross-backend work-cycle ruler
        "${RUNNER_ROOT}/game_cycles.c"                        # cost-table lookup
        "${SONIC_GENERATED_SOURCES_CYCLES}"                  # clown-measured costs
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

    # ----- Pairing #2: ORACLE cosim (clownmdemu, guest-visible-surface) -----
    # The independent full-machine oracle. Uses cosim_visible.c (NOT cosim_state.c,
    # which reads own-backend globals). Hashes the guest-visible surface only.
    add_executable(SonicTheHedgehogRecomp_oracle_cosim
        ${SONIC_SHARED_SOURCES}
        ${SONIC_ORACLE_EXTRA_SOURCES}
        "${RUNNER_ROOT}/cosim.c"
        "${RUNNER_ROOT}/cosim_visible.c"
        "${RUNNER_ROOT}/cosim_cycles.c"                             # cross-backend work-cycle ruler
        "${RUNNER_ROOT}/game_cycles.c"                              # m68k_interp cost lookup
        "${SONIC_GENERATED_SOURCES_CYCLES}"
    )
    add_dependencies(SonicTheHedgehogRecomp_oracle_cosim ${SONIC_GENERATED_SOURCES_TARGET})
    target_compile_definitions(SonicTheHedgehogRecomp_oracle_cosim PRIVATE
        INCLUDE_GENERATED_CODE=1
        HYBRID_RECOMPILED_CODE=1
        SONIC_ORACLE_BUILD=1
        GENESIS_COSIM=1
        DEFAULT_DEBUG_PORT=4392
        $<$<BOOL:${SONIC_REVERSE_DEBUG}>:SONIC_REVERSE_DEBUG=1>
    )
    _sonic_finalize_target(SonicTheHedgehogRecomp_oracle_cosim SONIC_INCLUDES_ORACLE)
    target_link_libraries(SonicTheHedgehogRecomp_oracle_cosim PRIVATE clownmdemu-core)
    message(STATUS "  + oracle cosim target (pairing #2) enabled")
endif()
