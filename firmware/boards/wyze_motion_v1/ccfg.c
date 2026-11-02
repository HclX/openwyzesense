// Customer configuration, as the stock v1 motion sensor's (ground_truth/firmware/motion_v1_777A9A46): no DC/DC,
// LF clock from the RC oscillator (no 32 kHz crystal), CPU debug port enabled, ROM bootloader backdoor on DIO1
// held low.
// The sensor's MAC (8 ASCII characters) lives here too, at 0x1FFCC: each unit's own, so the image normally leaves
// it erased and the unit's CCFG is not written at all (flash-<product> writes <product>-app.bin; a CCFG restore
// keeps the chip's MAC fields). Built with -DOWS_SENSOR_MAC=77xxxxxx (OWS_MAC_W0/W1 from firmware/CMakeLists.txt),
// the image carries that MAC, for a unit whose MAC has to be provisioned anew.
#define CCFG_FORCE_VDDR_HH                   1    // for +14 dBm (the stock txPower 0xA73F)
#define SET_CCFG_MODE_CONF_DCDC_RECHARGE     0x1  // no DC/DC: the board has none (stock CCFG)
#define SET_CCFG_MODE_CONF_DCDC_ACTIVE       0x1
#define SET_CCFG_MODE_CONF_SCLK_LF_OPTION    0x3
#define SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE 0xC5
#define SET_CCFG_BL_CONFIG_BL_LEVEL          0x0
#define SET_CCFG_BL_CONFIG_BL_PIN_NUMBER     1
#define SET_CCFG_BL_CONFIG_BL_ENABLE         0xC5
#ifdef OWS_MAC_W0
#define SET_CCFG_IEEE_MAC_1 OWS_MAC_W0  // MAC characters 0-3
#define SET_CCFG_IEEE_BLE_0 OWS_MAC_W1  // characters 4-7
#endif

#include <ti/devices/cc13x0/startup_files/ccfg.c>
