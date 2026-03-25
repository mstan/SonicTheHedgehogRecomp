cmake_minimum_required(VERSION 3.16)
project(SonicTheHedgehogRecomp C)

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# Directory layout
#
#   SonicTheHedgehogRecomp/         <- this project (CMAKE_SOURCE_DIR)
#     runner/
#     segagenesisrecomp/            <- submodule
#       clownmdemu-core/            <- nested submodule
#       sonicthehedgehog/generated/
#       runner/include/             <- shared headers (genesis_runtime.h)
#     CMakeLists.txt (this file)
# ---------------------------------------------------------------------------

set(V1_ROOT "${CMAKE_SOURCE_DIR}/segagenesisrecomp")

# ---------------------------------------------------------------------------
# clownmdemu-core
#
# We replace clown68000-interpreter with our own stub (stub_clown68000.c).
# Clown68000_DoCycles dispatches to the game fiber; Clown68000_Interrupt is
# a no-op. clownmdemu-core/CMakeLists.txt honours this flag.
# ---------------------------------------------------------------------------

set(CLOWNMDEMU_SKIP_CLOWN68000_INTERPRETER ON CACHE BOOL
    "Use stub instead of clown68000-interpreter" FORCE)

add_subdirectory("${V1_ROOT}/clownmdemu-core" clownmdemu-core)

# ---------------------------------------------------------------------------
# SDL2
# ---------------------------------------------------------------------------

list(APPEND CMAKE_PREFIX_PATH
    "${V1_ROOT}/runner/external/SDL2/SDL2-2.28.5/cmake")

find_package(SDL2 REQUIRED)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

add_executable(SonicTheHedgehogRecomp
    runner/main.c
    runner/audio.c
    runner/glue.c
    runner/stub_clown68000.c
    # Generated code (READ-ONLY — fix codegen bugs in the recompiler, not here)
    "${V1_ROOT}/sonicthehedgehog/generated/sonic_full.c"
    "${V1_ROOT}/sonicthehedgehog/generated/sonic_dispatch.c"
    # Game-specific hooks
    "${V1_ROOT}/sonicthehedgehog/extras.c"
)

target_compile_definitions(SonicTheHedgehogRecomp PRIVATE
    ENABLE_RECOMPILED_CODE=1
)

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------

target_include_directories(SonicTheHedgehogRecomp PRIVATE
    runner/
    # clownmdemu headers
    "${V1_ROOT}/clownmdemu-core/source"
    "${V1_ROOT}/clownmdemu-core/libraries/clowncommon"
    # genesis_runtime.h, game_extras.h
    "${V1_ROOT}/runner/include"
    # extras.h (declares func_000206 etc.)
    "${V1_ROOT}/sonicthehedgehog"
    # clown68000.h (needed by stub_clown68000.c)
    "${V1_ROOT}/clownmdemu-core/libraries/clown68000/source/interpreter"
)

# ---------------------------------------------------------------------------
# Link
# ---------------------------------------------------------------------------

target_link_libraries(SonicTheHedgehogRecomp
    PRIVATE
        clownmdemu-core
        SDL2::SDL2
        SDL2::SDL2main
)

# ---------------------------------------------------------------------------
# Windows: copy SDL2.dll next to the executable
# ---------------------------------------------------------------------------

if(WIN32)
    add_custom_command(TARGET SonicTheHedgehogRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${V1_ROOT}/runner/external/SDL2/SDL2-2.28.5/lib/x64/SDL2.dll"
            "$<TARGET_FILE_DIR:SonicTheHedgehogRecomp>"
        COMMENT "Copying SDL2.dll"
    )
endif()

# ---------------------------------------------------------------------------
# Compiler options
# ---------------------------------------------------------------------------

if(MSVC)
    target_compile_options(SonicTheHedgehogRecomp PRIVATE
        /W3 /WX- /utf-8
        /wd4102   # unreferenced label (common in generated dispatch code)
        /wd4996   # 'deprecated' CRT functions
    )
    target_link_options(SonicTheHedgehogRecomp PRIVATE /MAP)
endif()
