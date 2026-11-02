# Cross toolchain: the pinned Arm GNU Toolchain that tools/toolchain.sh unpacks into .toolchain/.
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

get_filename_component(OWS_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
file(STRINGS "${OWS_ROOT}/tools/toolchain.lock" _gcc_dir REGEX "^ARM_GCC_DIR=")
string(REPLACE "ARM_GCC_DIR=" "" _gcc_dir "${_gcc_dir}")
set(_bin "${OWS_ROOT}/.toolchain/${_gcc_dir}/bin")
if(NOT EXISTS "${_bin}/arm-none-eabi-gcc")
    message(FATAL_ERROR "arm-none-eabi-gcc not found in ${_bin}: run tools/toolchain.sh")
endif()

set(CMAKE_C_COMPILER "${_bin}/arm-none-eabi-gcc")
set(CMAKE_ASM_COMPILER "${_bin}/arm-none-eabi-gcc")
set(CMAKE_OBJCOPY "${_bin}/arm-none-eabi-objcopy" CACHE FILEPATH "")
set(CMAKE_SIZE "${_bin}/arm-none-eabi-size" CACHE FILEPATH "")
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m3 -mthumb -ffunction-sections -fdata-sections")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-mcpu=cortex-m3 -mthumb -specs=nano.specs -specs=nosys.specs -Wl,--gc-sections")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
