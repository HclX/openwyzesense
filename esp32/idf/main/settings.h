// The relay's settings, kept in NVS (namespace "ows"): set from the setup page (web.c).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define ROLE_BOTH    0
#define ROLE_RELAY   1
#define ROLE_STATION 2

typedef struct {
    char ssid[33], pass[65];  // Wi-Fi; an empty SSID: not set up yet (the setup access point)
    char url[128];            // wyzesense2mqtt-rs's bridge: ws://host:port/ws/bridge
    char token[64];           // its bridge token, or empty
    char admin_pass[64];      // HTTP admin password for mutating endpoints (falls back to token when empty)
    bool skip_gwsync;         // pause startup CC1310 auto-reflash (safe mode / custom radio builds)
    bool rig;                 // the battery rig (#95): CONFIG_OWS_RIG_SUPPLY_GPIO supplies the contact (rig_task.c)
    uint8_t contact_flash;    // how gwsync reflashes the contact (#81): GWSYNC_VIA_AUTO / _CJTAG / _SBL
    uint8_t role;             // 0 = both (default), 1 = relay only, 2 = station only
} settings_t;

extern settings_t settings;

void settings_load(void);
bool settings_save(void);
