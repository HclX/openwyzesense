set(BOARD_CHIP cc1310f128)
set(BOARD_FLASH uniflash)               # 2-pin cJTAG (TMS, TCK, RESET_N) through TI UniFlash's DSLite
set(BOARD_MAC_IN_CCFG TRUE)             # each unit's MAC is in its CCFG: never write page 31; a MAC can be
                                        # built in for provisioning with -DOWS_SENSOR_MAC=77xxxxxx
