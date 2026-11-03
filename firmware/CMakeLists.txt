# OpenWyzeSense firmware. Configured two ways (see CMakePresets.json at the repo root):
#   cross (toolchain file cmake/arm-none-eabi.cmake): the products in products.cmake, apps (apps/) on boards
#     (boards/), for the CC1310
#   native: unit tests of the portable code (common/), and the apps on the host board (boards/host/), with ctest
cmake_minimum_required(VERSION 3.20)
project(openwyzesense C ASM)

get_filename_component(OWS_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

add_library(ows_common STATIC common/mac154.c common/host_proto.c common/wyze_app.c common/gateway.c common/sensor.c common/nvstore.c common/ccfg_check.c common/boot_slot.c)
target_include_directories(ows_common PUBLIC common)
target_compile_options(ows_common PRIVATE -Wall -Wextra -Werror)

if(NOT CMAKE_CROSSCOMPILING)
    enable_testing()
    add_executable(test_common tests/test_main.c tests/aes_ref.c)
    target_link_libraries(test_common ows_common)
    target_compile_options(test_common PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
    target_link_options(test_common PRIVATE -fsanitize=address,undefined)
    add_test(NAME common COMMAND test_common "${OWS_ROOT}/reference/tests/vectors/frames.txt")
    add_executable(test_gateway tests/test_gateway.c tests/aes_ref.c)
    target_link_libraries(test_gateway ows_common)
    target_compile_options(test_gateway PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
    target_link_options(test_gateway PRIVATE -fsanitize=address,undefined)
    add_test(NAME gateway COMMAND test_gateway "${OWS_ROOT}/reference/tests/vectors/gateway_replay.txt")
    add_executable(test_sensor tests/test_sensor.c tests/aes_ref.c)
    target_link_libraries(test_sensor ows_common)
    target_compile_options(test_sensor PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
    target_link_options(test_sensor PRIVATE -fsanitize=address,undefined)
    add_test(NAME sensor COMMAND test_sensor "${OWS_ROOT}/reference/tests/vectors/frames.txt")
    add_executable(test_roam tests/test_roam.c tests/aes_ref.c)
    target_link_libraries(test_roam ows_common)
    target_compile_options(test_roam PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
    target_link_options(test_roam PRIVATE -fsanitize=address,undefined)
    add_test(NAME roam COMMAND test_roam)
    add_executable(test_nvstore tests/test_nvstore.c tests/aes_ref.c)
    target_link_libraries(test_nvstore ows_common)
    target_compile_options(test_nvstore PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
    target_link_options(test_nvstore PRIVATE -fsanitize=address,undefined)
    # the stock dongle dumps are not published (ground_truth/README.md): OWS_GROUND_TRUTH=<full copy> runs that part
    if(DEFINED ENV{OWS_GROUND_TRUTH})
        set(OWS_GROUND_TRUTH "$ENV{OWS_GROUND_TRUTH}")
    else()
        set(OWS_GROUND_TRUTH "${OWS_ROOT}/ground_truth")
    endif()
    add_test(NAME nvstore COMMAND test_nvstore "${OWS_GROUND_TRUTH}/firmware/gateway_2025-02-02_v1.4/FLASH.bin"
             "${OWS_GROUND_TRUTH}/firmware/gateway_2025-02-08_v1.8/FLASH.bin")

    add_executable(test_boot tests/test_boot.c)
    target_link_libraries(test_boot ows_common)
    target_compile_options(test_boot PRIVATE -Wall -Wextra -Werror -fsanitize=address,undefined)
    target_link_options(test_boot PRIVATE -fsanitize=address,undefined)
    add_test(NAME boot COMMAND test_boot)

    # The apps, unchanged, on the host board: the hardware interface (hal/) simulated (the simulated radio refuses
    # the house PAN as the real one does), with the debug host command.
    set(host_flags -Wall -Wextra -Werror -fsanitize=address,undefined)
    add_library(board_host STATIC boards/host/host_board.c boards/host/build_info_host.c)
    target_include_directories(board_host PUBLIC hal boards/host)
    target_link_libraries(board_host PUBLIC ows_common)
    target_compile_options(board_host PRIVATE ${host_flags})
    add_library(apps_host STATIC apps/gateway/gateway_app.c apps/sensor/sensor_app.c apps/sniffer/sniffer_app.c)
    target_include_directories(apps_host PUBLIC apps/gateway apps/sensor apps/sniffer)
    target_link_libraries(apps_host PUBLIC board_host)
    target_compile_definitions(apps_host PRIVATE OWS_DEBUG=1 OWS_VERSION="host")
    target_compile_options(apps_host PRIVATE ${host_flags})
    add_executable(test_apps tests/test_apps.c tests/aes_ref.c)
    target_link_libraries(test_apps apps_host board_host)
    target_compile_options(test_apps PRIVATE ${host_flags})
    target_link_options(test_apps PRIVATE -fsanitize=address,undefined)
    add_test(NAME apps COMMAND test_apps)

    # The apps as programs on the host board, in real time, sharing an air over local UDP (boards/host/host_main.c)
    foreach(app gateway sensor sniffer)
        add_executable(ows-host-${app} boards/host/host_main.c tests/aes_ref.c)
        target_link_libraries(ows-host-${app} apps_host board_host)
        target_compile_definitions(ows-host-${app} PRIVATE OWS_HOST_APP=${app})
        target_compile_options(ows-host-${app} PRIVATE ${host_flags})
        target_link_options(ows-host-${app} PRIVATE -fsanitize=address,undefined)
    endforeach()
    return()
endif()

# --- firmware ---

option(OWS_DEBUG "Debug host commands (gateway: inject a received frame)" OFF)
option(OWS_PASSIVE "Tests only: omit radio_tx() so an image can never transmit (passive HIL)" OFF)

include(cmake/ti_sdk.cmake)

# The hardware interface (hal/) on the CC13x0: everything but the board-specific part (soc/cc13x0/board.c)
add_library(soc_cc13x0 STATIC soc/cc13x0/radio.c soc/cc13x0/phy_settings.c soc/cc13x0/uart.c soc/cc13x0/nv_flash.c
            soc/cc13x0/clock.c soc/cc13x0/chip.c soc/cc13x0/input.c soc/cc13x0/ccfg_flash.c soc/cc13x0/boot_hal.c)
target_include_directories(soc_cc13x0 PUBLIC hal soc/cc13x0)
target_link_libraries(soc_cc13x0 PUBLIC ows_common ti_sdk)
target_compile_options(soc_cc13x0 PRIVATE -Wall -Wextra -Werror)

find_package(Git QUIET)
execute_process(COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags WORKING_DIRECTORY "${OWS_ROOT}"
                OUTPUT_VARIABLE OWS_VERSION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
# for tests: another version string (it must not change an image's ID, #76)
set(OWS_VERSION_OVERRIDE "" CACHE STRING "the version string built in instead of git describe")
if(NOT OWS_VERSION_OVERRIDE STREQUAL "")
    set(OWS_VERSION "${OWS_VERSION_OVERRIDE}")
endif()
string(SUBSTRING "${OWS_VERSION}" 0 39 OWS_VERSION)  # build_info_t.version holds 39 and a NUL

set(OWS_PAN 0 CACHE STRING "gateway PAN ID (0: derive from the chip's IEEE address); sensor: join only this PAN")
set(OWS_SHORT 0 CACHE STRING "gateway short address (0: derive from the chip's IEEE address)")
set(OWS_BL_PIN "" CACHE STRING "LaunchPad only, for tests: the ROM bootloader backdoor pin (default 13, BTN-1)")
set(OWS_SENSOR_MAC "" CACHE STRING "the sensor's ASCII MAC, for boards that keep it in the CCFG (e.g. 777A9A46)")
set(OWS_BACKUP_DIR "" CACHE PATH "uniflash flash-<product>: directory holding this chip's flash.bin backup")

# Flashing and debugging through an XDS110 (the LaunchPad's) with the pinned OpenOCD
file(STRINGS "${OWS_ROOT}/tools/toolchain.lock" _ocd_dir REGEX "^OPENOCD_DIR=")
string(REPLACE "OPENOCD_DIR=" "" _ocd_dir "${_ocd_dir}")
set(OPENOCD "${OWS_ROOT}/.toolchain/${_ocd_dir}/bin/openocd")
set(OPENOCD_ARGS -f interface/xds110.cfg -c "transport select jtag" -c "adapter speed 2500"
                 -f target/ti_cc13x0.cfg)
add_custom_target(gdbserver COMMAND "${OPENOCD}" ${OPENOCD_ARGS} USES_TERMINAL)

# A board: its board_config.h and the CC13x0 board code (soc/cc13x0/board.c), built once for all its products
set(OWS_LOW_POWER "" CACHE STRING "Tests only: force BOARD_LOW_POWER (0 or 1) on every board")
option(OWS_NO_CONSOLE "Tests only: no UART console on any board (as a Wyze sensor)" OFF)
set(OWS_DEBUG_WINDOW_MS "" CACHE STRING "Tests only: a battery board's no-standby time after power-up (default 10000; 0: none)")

function(ows_board board)
    if(TARGET board_${board})
        return()
    endif()
    set(dir "${CMAKE_CURRENT_SOURCE_DIR}/boards/${board}")
    add_library(board_${board} STATIC soc/cc13x0/board.c)
    if(NOT OWS_LOW_POWER STREQUAL "")
        target_compile_definitions(board_${board} PRIVATE OWS_LOW_POWER=${OWS_LOW_POWER})
    endif()
    if(NOT OWS_DEBUG_WINDOW_MS STREQUAL "")
        target_compile_definitions(board_${board} PRIVATE OWS_DEBUG_WINDOW_MS=${OWS_DEBUG_WINDOW_MS})
    endif()
    if(OWS_NO_CONSOLE)
        target_compile_definitions(board_${board} PRIVATE OWS_NO_CONSOLE=1)
    endif()
    target_include_directories(board_${board} PUBLIC "${dir}")
    target_link_libraries(board_${board} PUBLIC soc_cc13x0)
    target_compile_options(board_${board} PRIVATE -Wall -Wextra -Werror)
endfunction()

# 8 ASCII characters -> two little-endian words, as the CCFG stores the stock sensors' MACs
function(ows_mac_words mac out0 out1)
    set(words "")
    foreach(start 0 4)
        set(word "")
        foreach(k 3 2 1 0)
            math(EXPR at "${start} + ${k}")
            string(SUBSTRING "${mac}" ${at} 1 ch)
            string(HEX "${ch}" hex)
            string(APPEND word "${hex}")
        endforeach()
        list(APPEND words "0x${word}")
    endforeach()
    list(GET words 0 w0)
    list(GET words 1 w1)
    set(${out0} ${w0} PARENT_SCOPE)
    set(${out1} ${w1} PARENT_SCOPE)
endfunction()

set(OWS_IMAGE_VERSION "" CACHE STRING "OTA slot images: their version (default: from the release tag, image_version.cmake)")
if(OWS_IMAGE_VERSION STREQUAL "")
    # the real git describe (not OWS_VERSION_OVERRIDE, a test's string for the build info block)
    execute_process(COMMAND "${GIT_EXECUTABLE}" describe --always --dirty --tags WORKING_DIRECTORY "${OWS_ROOT}"
                    OUTPUT_VARIABLE describe_for_image OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND "${GIT_EXECUTABLE}" rev-list --count HEAD WORKING_DIRECTORY "${OWS_ROOT}"
                    OUTPUT_VARIABLE commit_count OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    include(${CMAKE_CURRENT_LIST_DIR}/image_version.cmake)
    ows_image_version("${describe_for_image}" "${commit_count}" image_version)
else()
    set(image_version ${OWS_IMAGE_VERSION})
endif()
math(EXPR image_version_hex "${image_version}" OUTPUT_FORMAT HEXADECIMAL)
message(STATUS "OTA slot image version: ${image_version} (${image_version_hex})")
find_package(Python3 COMPONENTS Interpreter REQUIRED)

# BOARD_NAME from a board's board_config.h (the boot selector and slot images use it for the board ID)
function(ows_board_name board out)
    file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/boards/${board}/board_config.h" line REGEX "^#define BOARD_NAME")
    string(REGEX REPLACE ".*\"(.*)\".*" "\\1" name "${line}")
    set(${out} "${name}" PARENT_SCOPE)
endfunction()

# The boot selector for a board (firmware/boot/selector.c): flash page 0, starts the newest OTA slot image
function(ows_boot board)
    set(dir "${CMAKE_CURRENT_SOURCE_DIR}/boards/${board}")
    include("${dir}/board.cmake")
    set(name boot-${board})
    add_executable(${name}.elf boot/selector.c)
    target_include_directories(${name}.elf PRIVATE "${dir}")
    target_link_libraries(${name}.elf ows_common)
    target_compile_options(${name}.elf PRIVATE -Wall -Wextra -Werror -Os)
    set(ld "${CMAKE_CURRENT_SOURCE_DIR}/chips/${BOARD_CHIP}/${BOARD_CHIP}_boot.ld")
    target_link_options(${name}.elf PRIVATE -nostartfiles -T "${ld}" -L "${CMAKE_CURRENT_SOURCE_DIR}/chips/${BOARD_CHIP}"
                        -Wl,-Map=${name}.map -Wl,--print-memory-usage)
    set_target_properties(${name}.elf PROPERTIES LINK_DEPENDS "${ld}")
    add_custom_command(TARGET ${name}.elf POST_BUILD
        COMMAND "${CMAKE_OBJCOPY}" -O binary ${name}.elf ${name}.bin
        BYPRODUCTS ${name}.bin)
endfunction()

function(ows_product name app board)
    cmake_parse_arguments(P "SLOTS" "SENSOR_TYPE" "" ${ARGN})
    set(dir "${CMAKE_CURRENT_SOURCE_DIR}/boards/${board}")
    set(BOARD_MAC_IN_CCFG FALSE)
    include("${dir}/board.cmake")
    set(defs OWS_VERSION="${OWS_VERSION}" OWS_PAN=${OWS_PAN} OWS_SHORT=${OWS_SHORT} OWS_DEBUG=$<BOOL:${OWS_DEBUG}>
             OWS_PASSIVE=$<BOOL:${OWS_PASSIVE}>
             OWS_HAS_TX=$<IF:$<OR:$<BOOL:${OWS_PASSIVE}>,$<STREQUAL:${app},sniffer>>,0,1>
             OWS_PRODUCT="${name}" OWS_APP="${app}")
    if(P_SENSOR_TYPE)
        list(APPEND defs OWS_SENSOR_TYPE=${P_SENSOR_TYPE})
    endif()
    set(ccfg_defs "")
    string(LENGTH "${OWS_SENSOR_MAC}" mac_len)
    if(BOARD_MAC_IN_CCFG AND mac_len EQUAL 8)
        ows_mac_words("${OWS_SENSOR_MAC}" w0 w1)
        list(APPEND ccfg_defs OWS_MAC_W0=${w0} OWS_MAC_W1=${w1})
    endif()
    if(NOT OWS_BL_PIN STREQUAL "" AND board STREQUAL "launchxl_cc1310")
        list(APPEND ccfg_defs OWS_BL_PIN=${OWS_BL_PIN})
    endif()
    ows_board(${board})
    set(ld "${CMAKE_CURRENT_SOURCE_DIR}/chips/${BOARD_CHIP}/${BOARD_CHIP}.ld")
    # ccfg.c and the startup code are linked as objects, not from a library: nothing references them
    # ccfg_builtin.c: the same CCFG again as data (hal/ccfg.h), for ccfg_restore()
    add_executable(${name}.elf apps/${app}/main.c apps/${app}/${app}_app.c soc/cc13x0/aes_cc13x0.c "${dir}/ccfg.c"
                   soc/cc13x0/ccfg_builtin.c soc/cc13x0/build_info.c
                   "${TI_SDK}/kernel/nortos/startup/startup_cc13xx_cc26xx_gcc.c")
    target_include_directories(${name}.elf PRIVATE apps/${app})
    target_link_libraries(${name}.elf board_${board} soc_cc13x0 board_${board})
    target_compile_definitions(${name}.elf PRIVATE ${defs} ${ccfg_defs})
    set_source_files_properties(apps/${app}/main.c apps/${app}/${app}_app.c soc/cc13x0/aes_cc13x0.c
                                soc/cc13x0/build_info.c soc/cc13x0/slot_header.c
                                PROPERTIES COMPILE_OPTIONS "-Wall;-Wextra;-Werror")
    set_source_files_properties("${dir}/ccfg.c" soc/cc13x0/ccfg_builtin.c
                                "${TI_SDK}/kernel/nortos/startup/startup_cc13xx_cc26xx_gcc.c"
                                PROPERTIES COMPILE_OPTIONS "-w")
    target_link_options(${name}.elf PRIVATE -nostartfiles -T "${ld}" -L "${CMAKE_CURRENT_SOURCE_DIR}/chips/${BOARD_CHIP}"
                        -Wl,-Map=${name}.map -Wl,--print-memory-usage)
    set_target_properties(${name}.elf PROPERTIES LINK_DEPENDS "${ld}")
    add_custom_command(TARGET ${name}.elf POST_BUILD
        # the image ID into the build info block first (#76): every output below carries it
        COMMAND "${Python3_EXECUTABLE}" "${OWS_ROOT}/tools/image_id.py" stamp ${name}.elf --objcopy "${CMAKE_OBJCOPY}"
        COMMAND "${CMAKE_OBJCOPY}" -O ihex ${name}.elf ${name}.hex
        COMMAND "${CMAKE_OBJCOPY}" -O binary ${name}.elf ${name}.bin
        # without the CCFG (flash 0x1FFA8): what ows-flash writes on the dongle, to pages 0-29 only
        COMMAND "${CMAKE_OBJCOPY}" -O binary -R .ccfg ${name}.elf ${name}-app.bin
        BYPRODUCTS ${name}.hex ${name}.bin ${name}-app.bin)
    if(P_SLOTS)
        # OTA slot images (docs/ota.md): the same app linked for slot A and slot B, no CCFG, header filled in after
        ows_board_name(${board} board_name)
        foreach(slot a b)
            set(sname ${name}-slot-${slot})
            set(sld "${CMAKE_CURRENT_SOURCE_DIR}/chips/${BOARD_CHIP}/${BOARD_CHIP}_slot_${slot}.ld")
            add_executable(${sname}.elf apps/${app}/main.c apps/${app}/${app}_app.c soc/cc13x0/aes_cc13x0.c
                           soc/cc13x0/slot_header.c soc/cc13x0/ccfg_builtin.c soc/cc13x0/build_info.c
                           "${TI_SDK}/kernel/nortos/startup/startup_cc13xx_cc26xx_gcc.c")
            target_include_directories(${sname}.elf PRIVATE apps/${app})
            target_link_libraries(${sname}.elf board_${board} soc_cc13x0 board_${board})
            target_compile_definitions(${sname}.elf PRIVATE ${defs} ${ccfg_defs})
            target_link_options(${sname}.elf PRIVATE -nostartfiles -T "${sld}"
                                -L "${CMAKE_CURRENT_SOURCE_DIR}/chips/${BOARD_CHIP}" -Wl,-Map=${sname}.map)
            set_target_properties(${sname}.elf PROPERTIES LINK_DEPENDS "${sld}")
            add_custom_command(TARGET ${sname}.elf POST_BUILD
                COMMAND "${Python3_EXECUTABLE}" "${OWS_ROOT}/tools/image_id.py" stamp ${sname}.elf
                        --objcopy "${CMAKE_OBJCOPY}"
                COMMAND "${CMAKE_OBJCOPY}" -O binary ${sname}.elf ${sname}.raw
                COMMAND "${Python3_EXECUTABLE}" "${OWS_ROOT}/tools/slot_image.py" ${sname}.raw ${sname}.img
                        --board-name "${board_name}" --version ${image_version} --slot ${slot}
                BYPRODUCTS ${sname}.raw ${sname}.img)
        endforeach()
        if(NOT TARGET boot-${board}.elf)
            ows_boot(${board})
        endif()
    endif()
    if(BOARD_FLASH STREQUAL openocd)
        # The reset after programming now and then fails ("JTAG scan chain interrogation failed") and leaves
        # the CPU halted in OpenOCD's flash helper in RAM: reset again in a second session.
        # A board that keeps the unit's MAC in its CCFG gets only the image below it (<product>-app.bin, from 0x0:
        # OpenOCD erases just the pages it covers), so the unit's CCFG, MAC included, stays as it is.
        if(BOARD_MAC_IN_CCFG)
            set(program "program ${name}-app.bin 0x0 verify reset exit")
        else()
            set(program "program ${name}.elf verify reset exit")
        endif()
        add_custom_target(flash-${name}
            COMMAND "${OPENOCD}" ${OPENOCD_ARGS} -c "${program}"
            COMMAND "${OPENOCD}" ${OPENOCD_ARGS} -c "init" -c "reset run" -c "shutdown"
            DEPENDS ${name}.elf USES_TERMINAL)
    endif()
    if(BOARD_FLASH STREQUAL uniflash)
        # tools/uniflash.py (TI UniFlash's DSLite over 2-pin cJTAG, .toolchain/uniflash): pages 0-29 only (selector +
        # slot A for SLOTS products, else <product>-app.bin at 0x0; page 30 NV and page 31 CCFG with MAC stay),
        # guarded by --backup ${OWS_BACKUP_DIR}, verified by read-back, then board reset with debug power domain off
        if(P_SLOTS)
            set(flash_deps boot-${board}.elf ${name}-slot-a.elf)
            set(flash_args slots boot-${board}.bin ${name}-slot-a.img)
        else()
            set(flash_deps ${name}.elf)
            set(flash_args flash ${name}-app.bin)
        endif()
        if(OWS_BACKUP_DIR STREQUAL "")
            add_custom_target(flash-${name}
                COMMAND "${CMAKE_COMMAND}" -E echo
                        "error: OWS_BACKUP_DIR is empty; set -DOWS_BACKUP_DIR=<backup-dir> before running flash-${name}"
                COMMAND "${CMAKE_COMMAND}" -E false
                DEPENDS ${flash_deps} USES_TERMINAL VERBATIM)
        else()
            add_custom_target(flash-${name}
                COMMAND "${Python3_EXECUTABLE}" "${OWS_ROOT}/tools/uniflash.py" --config "${dir}/xds110_2pin.ccxml"
                        ${flash_args} --backup "${OWS_BACKUP_DIR}"
                DEPENDS ${flash_deps} USES_TERMINAL VERBATIM)
        endif()
    endif()
endfunction()

include(products.cmake)
