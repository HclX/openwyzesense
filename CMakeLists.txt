# Entry point for CMakePresets.json: firmware/, and esp32/ (the relay) on host builds.
cmake_minimum_required(VERSION 3.20)
project(openwyzesense-root NONE)
enable_testing()
add_subdirectory(firmware)
if(NOT CMAKE_CROSSCOMPILING)
    enable_language(C)
    add_subdirectory(esp32)  # the relay core and ows-relay, host builds only
endif()
