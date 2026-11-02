// Values copied from the stock gateway's RAM (ground_truth/firmware/gateway_2025-02-08_v1.8/RAM.bin):
// CMD_PROP_TX_ADV 0x2628, CMD_PROP_RADIO_DIV_SETUP 0x2648, CMD_PROP_RX_ADV 0x2670, overrides 0x2728, CMD_FS 0x25D8.
// The MCE and RFE patches are TI's sl_longrange ones, identical to the stock images (FACTS.md §1).
#include "phy_settings.h"

// clang-format off: a TI include path, not a division
#include DeviceFamily_constructPath(rf_patches/rf_patch_cpe_sl_longrange.h)
#include DeviceFamily_constructPath(rf_patches/rf_patch_mce_sl_longrange.h)
#include DeviceFamily_constructPath(rf_patches/rf_patch_rfe_sl_longrange.h)
// clang-format on

#define SYNC_WORD 0x5555904E  // what the MCE patch is given; the 64-symbol sync on air is FACTS.md §1

RF_Mode phy_mode = {
    .rfMode = RF_MODE_PROPRIETARY_SUB_1,
    .cpePatchFxn = &rf_patch_cpe_sl_longrange,
    .mcePatchFxn = &rf_patch_mce_sl_longrange,
    .rfePatchFxn = &rf_patch_rfe_sl_longrange,
};

uint32_t phy_overrides[] = {
    0x00001107, 0x00374038, 0x000684A3, 0x7F004020, 0x00404064, 0xB1070503, 0x05330523, 0x40410583, 0x32CC0603,
    0x00010623, 0x849F0002, 0xC7440002, 0x02010403, 0x00108463, 0x04B00243, 0x00038883, 0x35F16084, 0x41146088,
    0x8213608C, 0x0100505C, 0x00FB88A3, 0x0CF80002, 0x00000943, 0x00000963, 0xFFFFFFFF,
};

rfc_CMD_PROP_RADIO_DIV_SETUP_t phy_cmd_setup = {
    .commandNo = CMD_PROP_RADIO_DIV_SETUP,
    .startTrigger.triggerType = TRIG_NOW,
    .condition.rule = COND_NEVER,
    .modulation = {.modType = 1, .deviation = 20},       // GFSK, +-5 kHz
    .symbolRate = {.preScale = 15, .rateWord = 0x3333},  // 20 ksym/s
    .rxBw = 0x21,
    .preamConf = {.nPreamBytes = 2},
    .formatConf = {.nSwBits = 32, .bMsbFirst = 1, .whitenMode = 7},  // 802.15.4g whitening, dynamic CRC
    .config = {.biasMode = 1, .analogCfgMode = 0x2D},
    .txPower = 0xA73F,
    .pRegOverride = phy_overrides,
    .centerFreq = 915,
    .intFreq = (int16_t)0x8000,  // default IF
    .loDivider = 5,
};

rfc_CMD_FS_t phy_cmd_fs = {
    .commandNo = CMD_FS,
    .startTrigger.triggerType = TRIG_NOW,
    .condition.rule = COND_NEVER,
    .frequency = 906,
    .fractFreq = 0xCCCC,  // + 0.8 MHz
};

rfc_CMD_PROP_RX_ADV_t phy_cmd_rx = {
    .commandNo = CMD_PROP_RX_ADV,
    .startTrigger = {.triggerType = TRIG_NOW, .pastTrig = 1},
    .condition.rule = COND_NEVER,
    .pktConf = {.bUseCrc = 1, .filterOp = 1},
    .rxConf = {.bAutoFlushCrcErr = 1, .bIncludeHdr = 1, .bAppendRssi = 1, .bAppendTimestamp = 1},
    .syncWord0 = SYNC_WORD,
    .maxPktLen = 1000,
    .hdrConf = {.numHdrBits = 16, .numLenBits = 11},  // the 802.15.4g PHR
    .lenOffset = -4,                                  // its length counts the 4-byte FCS
    .endTrigger.triggerType = TRIG_REL_START,
    .endTime = 0x6422C400,  // 420 s at 4 MHz; then restarted
};

rfc_CMD_PROP_TX_ADV_t phy_cmd_tx = {
    .commandNo = CMD_PROP_TX_ADV,
    .startTrigger.triggerType = TRIG_NOW,
    .condition.rule = COND_NEVER,
    .pktConf.bUseCrc = 1,
    .numHdrBits = 16,
    .preTrigger = {.triggerType = TRIG_REL_START, .pastTrig = 1},
    .syncWord = SYNC_WORD,
};
