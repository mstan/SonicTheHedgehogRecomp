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

set(RECOMP_ROOT "${CMAKE_SOURCE_DIR}/segagenesisrecomp")
set(RUNNER_ROOT "${RECOMP_ROOT}/runner")

option(SONIC_REVERSE_DEBUG
    "Enable Tier-1 reverse-debugger hooks (sgrdb_write*, ring buffer, rdb_* TCP cmds). \
Requires a regen with GenesisRecomp --reverse-debug; mixing flagged/unflagged \
generated C against the other runner variant is a link error by design."
    OFF)
option(GEN_DEV_TRACE
    "Compile the dev chip/snd trace rings + dumps (NEVER in release builds)" OFF)

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

# ---------------------------------------------------------------------------
# Shared sources / includes / link libs
# ---------------------------------------------------------------------------

set(SONIC_SHARED_SOURCES
    "${RUNNER_ROOT}/main.c"
    "${RUNNER_ROOT}/audio.c"
    "${RUNNER_ROOT}/chip_trace.c"   # dev-only [CHIP-TRACE] shared FM/PSG stream ring (strip before commit)
    "${RUNNER_ROOT}/cmd_server.c"
    "${RUNNER_ROOT}/frame_snapshots.c"
    "${RUNNER_ROOT}/hybrid_global.c"
    "${RUNNER_ROOT}/glue.c"
    "${RUNNER_ROOT}/fiber_compat.c"   # cross-platform fiber shim (Win32 Fibers / POSIX ucontext) — glue.c depends on it
    "${RUNNER_ROOT}/crash_report.c"
    "${RUNNER_ROOT}/input_script.c"
    "${RUNNER_ROOT}/gamepad.c"
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
    "${RECOMP_ROOT}/sonicthehedgehog"
)
set(SONIC_INCLUDES_ORACLE
    "${RUNNER_ROOT}"
    "${RUNNER_ROOT}/include"
    "${RUNNER_ROOT}/external/ymfm/src"
    "${RUNNER_ROOT}/external/superzazu"
    "${RUNNER_ROOT}/video"
    "${RECOMP_ROOT}/clownmdemu-core/libraries/clownz80/source"
    "${RECOMP_ROOT}/sonicthehedgehog"
    "${RECOMP_ROOT}/clownmdemu-core/source"
    "${RECOMP_ROOT}/clownmdemu-core/libraries/clowncommon"
    "${RECOMP_ROOT}/clownmdemu-core/libraries/clown68000/source/interpreter"
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
    ${OWN_BACKEND_SOURCES}          # clean-room VDP+bus+Z80 scheduler (native = own backend)
)
target_compile_definitions(SonicTheHedgehogRecomp PRIVATE
    INCLUDE_GENERATED_CODE=1
    ENABLE_RECOMPILED_CODE=1
    OWN_BACKEND=1                   # the own backend IS the Sonic 1 runtime (no clownmdemu link)
    DEFAULT_DEBUG_PORT=4378
    $<$<BOOL:${SONIC_REVERSE_DEBUG}>:SONIC_REVERSE_DEBUG=1>
    $<$<BOOL:${GEN_DEV_TRACE}>:GEN_DEV_TRACE>
)
_sonic_finalize_target(SonicTheHedgehogRecomp SONIC_INCLUDES_NATIVE)

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
    add_subdirectory("${RECOMP_ROOT}/tests" tests)
endif()
