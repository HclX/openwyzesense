// Radio settings: TI SimpleLink Long Range ("SL LR", 20 ksym/s, DSSS 2, FEC) on 906.8 MHz, as the stock dongle
// runs it (docs/FACTS.md §1). tests/test_firmware.py compares these, field by field, with the stock gateway's RAM.
#pragma once

#include <ti/devices/DeviceFamily.h>
// clang-format off: a TI include path, not a division
#include DeviceFamily_constructPath(driverlib/rf_common_cmd.h)
#include DeviceFamily_constructPath(driverlib/rf_prop_cmd.h)
// clang-format on
#include <ti/drivers/rf/RF.h>

extern RF_Mode phy_mode;
extern rfc_CMD_PROP_RADIO_DIV_SETUP_t phy_cmd_setup;
extern rfc_CMD_FS_t phy_cmd_fs;
extern rfc_CMD_PROP_RX_ADV_t phy_cmd_rx;
extern uint32_t phy_overrides[];
extern rfc_CMD_PROP_TX_ADV_t phy_cmd_tx;
