cmake_minimum_required(VERSION 3.16)
project(SonicTheHedgehogRecomp C)

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# Twin-build: a single configure produces BOTH binaries.
#
#   SonicTheHedgehogRecomp.exe         — native (recompiled 68K, stub interp)
#   SonicTheHedgehogRecomp_oracle.exe  — oracle (full clown68000 interpreter,
#                                                 g_hybrid_table_size = 0)
#
# Both link the same clownmdemu-core static lib. The native target adds
# stub_clown68000.c which provides Clown68000_DoCycles/Reset/Interrupt as
# .obj-level symbols; MSVC's first-wins linker rule means those stubs win
# over the static-library interpreter symbols, so we never have a duplicate.
# The oracle target omits the stubs, so the real interpreter from
# clown68000-interpreter.lib gets pulled in.
#
# Default debug-server ports differ per target (4378 native / 4379 oracle)
# so both can run simultaneously for side-by-side ring-buffer comparison.
#
# Runner code is shared with Sonic 2 and lives in the submodule at
# segagenesisrecomp/runner/. Per-game files (sonic1_spec.c, sonic_extras.c,
# sonic1_hybrid_table.c) live alongside the Sonic 1 generated TUs in
# segagenesisrecomp/sonicthehedgehog/.
# ---------------------------------------------------------------------------

set(RECOMP_ROOT "${CMAKE_SOURCE_DIR}/segagenesisrecomp")
set(RUNNER_ROOT "${RECOMP_ROOT}/runner")

option(SONIC_REVERSE_DEBUG
    "Enable Tier-1 reverse-debugger hooks (sgrdb_write*, ring buffer, rdb_* TCP cmds). \
Requires a regen with GenesisRecomp --reverse-debug; mixing flagged/unflagged \
generated C against the other runner variant is a link error by design."
    OFF)

# clownmdemu-core is the mstan fork; our changes are committed in it directly
# (no build-time patch step — see THIRD-PARTY-LICENSES.md).
add_subdirectory("${RECOMP_ROOT}/clownmdemu-core" clownmdemu-core)

list(APPEND CMAKE_PREFIX_PATH
    "${RUNNER_ROOT}/external/SDL2/SDL2-2.28.5/cmake")
find_package(SDL2 REQUIRED)

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
# Shared sources / includes / link libs
# ---------------------------------------------------------------------------

set(SONIC_SHARED_SOURCES
    "${RUNNER_ROOT}/main.c"
    "${RUNNER_ROOT}/audio.c"
    "${RUNNER_ROOT}/cmd_server.c"
    "${RUNNER_ROOT}/frame_snapshots.c"
    "${RUNNER_ROOT}/hybrid_global.c"
    "${RUNNER_ROOT}/glue.c"
    "${RUNNER_ROOT}/crash_report.c"
    "${RUNNER_ROOT}/input_script.c"
    "${RUNNER_ROOT}/gamepad.c"
    # AGPL-free Z80 core (superzazu, MIT). These definitions override
    # clownz80-interpreter.lib via MSVC first-wins linking (same trick as
    # stub_clown68000.c) so clownz80's AGPL interpreter is never linked.
    "${RUNNER_ROOT}/z80_superzazu.c"
    "${RUNNER_ROOT}/external/superzazu/z80.c"
    "${RUNNER_ROOT}/audio/event_queue.c"
    ${FM_SOURCES}
    "${RUNNER_ROOT}/audio/sn76489.c"
    "${RUNNER_ROOT}/audio/mixer.c"
    "${RUNNER_ROOT}/audio/observability.c"
    "${RECOMP_ROOT}/sonicthehedgehog/generated/sonic_full.c"
    "${RECOMP_ROOT}/sonicthehedgehog/generated/sonic_dispatch.c"
    "${RECOMP_ROOT}/sonicthehedgehog/generated/sonic_layout.c"
    "${RECOMP_ROOT}/sonicthehedgehog/sonic1_spec.c"
    "${RECOMP_ROOT}/sonicthehedgehog/sonic_extras.c"
    "${RECOMP_ROOT}/sonicthehedgehog/extras.c"
)

if(SONIC_REVERSE_DEBUG)
    list(APPEND SONIC_SHARED_SOURCES "${RUNNER_ROOT}/reverse_debug.c")
    message(STATUS "SONIC_REVERSE_DEBUG=ON — regen with GenesisRecomp --reverse-debug")
endif()

set(SONIC_SHARED_INCLUDES
    "${RUNNER_ROOT}"
    "${RUNNER_ROOT}/include"
    "${RUNNER_ROOT}/external/ymfm/src"
    "${RUNNER_ROOT}/external/superzazu"
    "${RECOMP_ROOT}/clownmdemu-core/libraries/clownz80/source"
    "${RECOMP_ROOT}/sonicthehedgehog"
    "${RECOMP_ROOT}/clownmdemu-core/source"
    "${RECOMP_ROOT}/clownmdemu-core/libraries/clowncommon"
    "${RECOMP_ROOT}/clownmdemu-core/libraries/clown68000/source/interpreter"
)

set(SONIC_SHARED_LIBS clownmdemu-core SDL2::SDL2 SDL2::SDL2main)
if(WIN32)
    list(APPEND SONIC_SHARED_LIBS ws2_32 comdlg32)
endif()

function(_sonic_finalize_target tgt)
    target_include_directories(${tgt} PRIVATE ${SONIC_SHARED_INCLUDES})
    target_link_libraries(${tgt} PRIVATE ${SONIC_SHARED_LIBS})
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /W3 /WX- /utf-8 /wd4102 /wd4996)
        target_link_options(${tgt} PRIVATE /MAP)
    endif()
    if(WIN32)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RUNNER_ROOT}/external/SDL2/SDL2-2.28.5/lib/x64/SDL2.dll"
                "$<TARGET_FILE_DIR:${tgt}>"
            COMMENT "Copying SDL2.dll for ${tgt}")
        # Annotation CSV used by crash_report.c for symbol resolution.
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RECOMP_ROOT}/sonicthehedgehog/annotations_from_disasm.csv"
                "$<TARGET_FILE_DIR:${tgt}>/annotations_from_disasm.csv"
            COMMENT "Copying Sonic 1 annotations for crash-report symbol resolution")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Native target — recompiled 68K drives the game
# ---------------------------------------------------------------------------

add_executable(SonicTheHedgehogRecomp
    ${SONIC_SHARED_SOURCES}
    "${RUNNER_ROOT}/stub_clown68000.c"
)
target_compile_definitions(SonicTheHedgehogRecomp PRIVATE
    INCLUDE_GENERATED_CODE=1
    ENABLE_RECOMPILED_CODE=1
    DEFAULT_DEBUG_PORT=4378
    $<$<BOOL:${SONIC_REVERSE_DEBUG}>:SONIC_REVERSE_DEBUG=1>
)
_sonic_finalize_target(SonicTheHedgehogRecomp)

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
target_compile_definitions(SonicTheHedgehogRecomp_oracle PRIVATE
    INCLUDE_GENERATED_CODE=1
    HYBRID_RECOMPILED_CODE=1
    SONIC_ORACLE_BUILD=1
    DEFAULT_DEBUG_PORT=4379
    $<$<BOOL:${SONIC_REVERSE_DEBUG}>:SONIC_REVERSE_DEBUG=1>
)
_sonic_finalize_target(SonicTheHedgehogRecomp_oracle)

# ---------------------------------------------------------------------------
# Framework tests (L1 decoder harness). Opt-in via -DBUILD_TESTS=ON.
# ---------------------------------------------------------------------------

option(BUILD_TESTS "Build L1 decoder test harness" OFF)
if(BUILD_TESTS)
    add_subdirectory("${RECOMP_ROOT}/tests" tests)
endif()
