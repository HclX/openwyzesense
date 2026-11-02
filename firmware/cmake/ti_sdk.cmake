# The vendored TI core SDK subset (third_party/ti_coresdk) as one static library: CC13x0 driverlib, the
# single-client RF driver, the PIN and Power drivers and the NoRTOS kernel.
set(TI_SDK "${OWS_ROOT}/third_party/ti_coresdk")

file(GLOB TI_DRIVERLIB_SRC "${TI_SDK}/source/ti/devices/cc13x0/driverlib/*.c")
set(TI_NORTOS_DPL ClockPTimer DebugP HwiPCC26XX MutexP PowerCC26XX QueueP SemaphoreP SwiP SystemP TimerPCC26XX)
list(TRANSFORM TI_NORTOS_DPL PREPEND "${TI_SDK}/kernel/nortos/dpl/")
list(TRANSFORM TI_NORTOS_DPL APPEND "_nortos.c")

add_library(ti_sdk STATIC
    ${TI_DRIVERLIB_SRC}
    ${TI_NORTOS_DPL}
    "${TI_SDK}/kernel/nortos/NoRTOS.c"
    "${TI_SDK}/source/ti/drivers/rf/RFCC26XX_singleMode.c"
    "${TI_SDK}/source/ti/drivers/pin/PINCC26XX.c"
    "${TI_SDK}/source/ti/drivers/power/PowerCC26XX.c"
    "${TI_SDK}/source/ti/drivers/power/PowerCC26XX_calibrateRCOSC.c"
    "${TI_SDK}/source/ti/drivers/utils/List.c"
)
target_include_directories(ti_sdk SYSTEM PUBLIC "${TI_SDK}/source" "${TI_SDK}/kernel/nortos")
target_compile_definitions(ti_sdk PUBLIC DeviceFamily_CC13X0 RF_SINGLEMODE)
target_compile_options(ti_sdk PRIVATE -Os -w)   # vendor code, kept as shipped
