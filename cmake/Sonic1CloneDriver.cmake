# Optional, build-bound sound image. No upstream music/sample data is in Git.
set(SONIC_CLONE_DRIVER_PROBE_DIR "" CACHE PATH "Sound-only probe built by tools/clone_driver_probe/build.py")
set(SONIC_GENERATION_GAME "${CMAKE_SOURCE_DIR}/game")
if(SONIC_CLONE_DRIVER_PROBE_DIR)
    find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
    set(SONIC_GENERATION_GAME "${CMAKE_BINARY_DIR}/clone-input")
    file(GLOB _s1_clone_configs CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/game/*.toml" "${CMAKE_SOURCE_DIR}/game/*.csv")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        ${_s1_clone_configs} "${CMAKE_SOURCE_DIR}/game/sonic.bin"
        "${CMAKE_SOURCE_DIR}/tools/clone_driver_probe/prepare_native.py"
        "${SONIC_CLONE_DRIVER_PROBE_DIR}/probe.json"
        "${SONIC_CLONE_DRIVER_PROBE_DIR}/driver.lst"
        "${SONIC_CLONE_DRIVER_PROBE_DIR}/clone-driver.bin")
    execute_process(COMMAND "${Python3_EXECUTABLE}"
        "${CMAKE_SOURCE_DIR}/tools/clone_driver_probe/prepare_native.py"
        --probe "${SONIC_CLONE_DRIVER_PROBE_DIR}" --game "${CMAKE_SOURCE_DIR}/game"
        --engine "${RECOMP_ROOT}" --output "${SONIC_GENERATION_GAME}"
        COMMAND_ERROR_IS_FATAL ANY)
    include_directories("${SONIC_GENERATION_GAME}")
    add_compile_definitions(SONIC1_CLONE_DRIVER=1)
endif()
