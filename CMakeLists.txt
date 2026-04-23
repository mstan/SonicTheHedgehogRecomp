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
# ---------------------------------------------------------------------------

set(RECOMP_ROOT "${CMAKE_SOURCE_DIR}/segagenesisrecomp")

option(SONIC_REVERSE_DEBUG
    "Enable Tier-1 reverse-debugger hooks (sgrdb_write*, ring buffer, rdb_* TCP cmds). \
Requires a regen with GenesisRecomp --reverse-debug; mixing flagged/unflagged \
generated C against the other runner variant is a link error by design."
    OFF)

include("${RECOMP_ROOT}/runner/clownmdemu_patches.cmake")
add_subdirectory("${RECOMP_ROOT}/clownmdemu-core" clownmdemu-core)

list(APPEND CMAKE_PREFIX_PATH
    "${RECOMP_ROOT}/runner/external/SDL2/SDL2-2.28.5/cmake")
find_package(SDL2 REQUIRED)

# ---------------------------------------------------------------------------
# Shared sources / includes / link libs
# ---------------------------------------------------------------------------

set(SONIC_SHARED_SOURCES
    runner/main.c
    runner/audio.c
    runner/cmd_server.c
    runner/frame_snapshots.c
    runner/sonic_extras.c
    runner/hybrid_global.c
    runner/glue.c
    runner/audio/event_queue.c
    runner/audio/ym2612.c
    runner/audio/sn76489.c
    runner/audio/mixer.c
    runner/audio/observability.c
    "${RECOMP_ROOT}/sonicthehedgehog/generated/sonic_full.c"
    "${RECOMP_ROOT}/sonicthehedgehog/generated/sonic_dispatch.c"
    "${RECOMP_ROOT}/sonicthehedgehog/extras.c"
)

if(SONIC_REVERSE_DEBUG)
    list(APPEND SONIC_SHARED_SOURCES runner/reverse_debug.c)
    message(STATUS "SONIC_REVERSE_DEBUG=ON — regen with GenesisRecomp --reverse-debug")
endif()

set(SONIC_SHARED_INCLUDES
    runner/
    "${RECOMP_ROOT}/runner/include"
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
                "${RECOMP_ROOT}/runner/external/SDL2/SDL2-2.28.5/lib/x64/SDL2.dll"
                "$<TARGET_FILE_DIR:${tgt}>"
            COMMENT "Copying SDL2.dll for ${tgt}")
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Native target — recompiled 68K drives the game
# ---------------------------------------------------------------------------

add_executable(SonicTheHedgehogRecomp
    ${SONIC_SHARED_SOURCES}
    runner/stub_clown68000.c
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
    runner/hybrid.c
    runner/hybrid_table.c
    runner/hybrid_jmp.c
)
if(SONIC_REVERSE_DEBUG)
    # Tier 3: per-instruction capture on the oracle side. Oracle-only
    # because native uses stub_clown68000.c and never runs the
    # interpreter.
    list(APPEND SONIC_ORACLE_EXTRA_SOURCES runner/oracle_trace.c)
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
