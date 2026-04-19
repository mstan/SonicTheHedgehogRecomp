cmake_minimum_required(VERSION 3.16)
project(SonicTheHedgehogRecomp C)

set(CMAKE_C_STANDARD 11)

# ---------------------------------------------------------------------------
# Build-phase selectors
# ---------------------------------------------------------------------------

option(INCLUDE_GENERATED_CODE "Compile generated code into the binary" OFF)
option(ENABLE_RECOMPILED_CODE "Recompiled 68K drives the game" OFF)
option(HYBRID_RECOMPILED_CODE "Interpreter with native overrides" OFF)

if(ENABLE_RECOMPILED_CODE)
    set(INCLUDE_GENERATED_CODE ON CACHE BOOL "" FORCE)
endif()
if(HYBRID_RECOMPILED_CODE)
    set(INCLUDE_GENERATED_CODE ON CACHE BOOL "" FORCE)
endif()

# ---------------------------------------------------------------------------
# Paths into the segagenesisrecomp submodule
# ---------------------------------------------------------------------------

set(RECOMP_ROOT "${CMAKE_SOURCE_DIR}/segagenesisrecomp")

# ---------------------------------------------------------------------------
# clownmdemu-core (from submodule)
# ---------------------------------------------------------------------------

if(ENABLE_RECOMPILED_CODE)
    set(CLOWNMDEMU_SKIP_CLOWN68000_INTERPRETER ON CACHE BOOL
        "Use stub instead of clown68000-interpreter" FORCE)
endif()

include("${RECOMP_ROOT}/runner/clownmdemu_patches.cmake")

add_subdirectory("${RECOMP_ROOT}/clownmdemu-core" clownmdemu-core)

# ---------------------------------------------------------------------------
# SDL2
# ---------------------------------------------------------------------------

list(APPEND CMAKE_PREFIX_PATH
    "${RECOMP_ROOT}/runner/external/SDL2/SDL2-2.28.5/cmake")

find_package(SDL2 REQUIRED)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------

add_executable(SonicTheHedgehogRecomp
    runner/main.c
    runner/audio.c
    runner/cmd_server.c
    runner/hybrid_global.c
)

if(INCLUDE_GENERATED_CODE)
    target_sources(SonicTheHedgehogRecomp PRIVATE
        runner/glue.c
        "${RECOMP_ROOT}/sonicthehedgehog/generated/sonic_full.c"
        "${RECOMP_ROOT}/sonicthehedgehog/generated/sonic_dispatch.c"
        "${RECOMP_ROOT}/sonicthehedgehog/extras.c"
    )
    target_include_directories(SonicTheHedgehogRecomp PRIVATE
        "${RECOMP_ROOT}/runner/include"
        "${RECOMP_ROOT}/sonicthehedgehog"
    )
endif()

if(ENABLE_RECOMPILED_CODE)
    target_sources(SonicTheHedgehogRecomp PRIVATE
        runner/stub_clown68000.c
    )
    target_compile_definitions(SonicTheHedgehogRecomp PRIVATE
        ENABLE_RECOMPILED_CODE=1
    )
    target_include_directories(SonicTheHedgehogRecomp PRIVATE
        "${RECOMP_ROOT}/clownmdemu-core/libraries/clown68000/source/interpreter"
    )
endif()

if(HYBRID_RECOMPILED_CODE)
    target_sources(SonicTheHedgehogRecomp PRIVATE
        runner/hybrid.c
        runner/hybrid_table.c
        runner/hybrid_jmp.c
    )
    target_compile_definitions(SonicTheHedgehogRecomp PRIVATE
        HYBRID_RECOMPILED_CODE=1
    )
    target_include_directories(SonicTheHedgehogRecomp PRIVATE
        "${RECOMP_ROOT}/clownmdemu-core/libraries/clown68000/source/interpreter"
    )
endif()

# ---------------------------------------------------------------------------
# Include directories
# ---------------------------------------------------------------------------

target_include_directories(SonicTheHedgehogRecomp PRIVATE
    runner/
    "${RECOMP_ROOT}/clownmdemu-core/source"
    "${RECOMP_ROOT}/clownmdemu-core/libraries/clowncommon"
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

if(WIN32)
    target_link_libraries(SonicTheHedgehogRecomp PRIVATE ws2_32 comdlg32)
endif()

if(WIN32)
    add_custom_command(TARGET SonicTheHedgehogRecomp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${RECOMP_ROOT}/runner/external/SDL2/SDL2-2.28.5/lib/x64/SDL2.dll"
            "$<TARGET_FILE_DIR:SonicTheHedgehogRecomp>"
        COMMENT "Copying SDL2.dll"
    )
endif()

if(MSVC)
    target_compile_options(SonicTheHedgehogRecomp PRIVATE
        /W3 /WX- /utf-8
        /wd4102
        /wd4996
    )
    target_link_options(SonicTheHedgehogRecomp PRIVATE /MAP)
endif()

# ---------------------------------------------------------------------------
# Framework tests (L1 decoder harness). Opt-in via -DBUILD_TESTS=ON.
# ---------------------------------------------------------------------------

option(BUILD_TESTS "Build L1 decoder test harness" OFF)
if(BUILD_TESTS)
    add_subdirectory("${RECOMP_ROOT}/tests" tests)
endif()
