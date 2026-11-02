// Customer configuration, as the stock dongle's (ground_truth/firmware/gateway_2025-02-08_v1.8, FACTS.md §5):
// forced VDDR HH, LF clock from the 32 kHz crystal, CPU debug port enabled, ROM bootloader backdoor on DIO1 held low at reset.
// DIO1 is the UART line from the CH554, which is how the dongle's field update enters the bootloader: keep it.
#define CCFG_FORCE_VDDR_HH                   1  // for +14 dBm (the stock txPower 0xA73F)
#define SET_CCFG_BL_CONFIG_BOOTLOADER_ENABLE 0xC5
#define SET_CCFG_BL_CONFIG_BL_LEVEL          0x0
#define SET_CCFG_BL_CONFIG_BL_PIN_NUMBER     1
#define SET_CCFG_BL_CONFIG_BL_ENABLE         0xC5

#include <ti/devices/cc13x0/startup_files/ccfg.c>
