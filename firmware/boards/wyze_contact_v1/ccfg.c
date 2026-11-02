// Customer configuration, matching the stock v1 contact sensor's power/clock/debug settings
// (ground_truth/firmware/contact_v1_777B42DF_2026-10-24: DC/DC on, VDDR not forced to HH, LF RCOSC, CPU DAP enabled),
// EXCEPT with the ROM Serial Bootloader enabled on the W pad (DIO10, active low: BL_CONFIG 0xC5FE0AC5) so that
// gateway-contact-v1 (and provisioned units) can always be updated over UART/SBL without 2-pin cJTAG recovery.
// Sensor slot images (.img) and flash-<product> (<product>-app.bin) never touch Page 31; a gateway CCFG restore
// preserves the unit's 8-char ASCII MAC at 0x1FFCC. Built with -DOWS_SENSOR_MAC=77xxxxxx, the full image carries that
// MAC for provisioning a blank/recovered chip.
#define SET_CCFG_MODE_CONF_DCDC_RECHARGE     0x0  // DC/DC in use (stock CCFG)
#define SET_CCFG_MODE_CONF_DCDC_ACTIVE       0x0
#define SET_CCFG_MODE_CONF_SCLK_LF_OPTION    0x3
#define SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE 0xC5  // ROM SBL enabled, active-low backdoor on W pad (DIO10)
#define SET_CCFG_BL_CONFIG_BL_LEVEL          0x0
#define SET_CCFG_BL_CONFIG_BL_PIN_NUMBER     10
#define SET_CCFG_BL_CONFIG_BL_ENABLE         0xC5
#ifdef OWS_MAC_W0
#define SET_CCFG_IEEE_MAC_1 OWS_MAC_W0  // MAC characters 0-3
#define SET_CCFG_IEEE_BLE_0 OWS_MAC_W1  // characters 4-7
#endif

#include <ti/devices/cc13x0/startup_files/ccfg.c>
