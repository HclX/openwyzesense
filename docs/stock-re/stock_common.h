/*
 * Reconstructed TI SimpleLink CC13x0 SDK (ti154stack) definitions, NVOCTP
 * structures, RF protocol payloads, and cryptographic primitives from the
 * stock Wyze Sense v1 sensor and USB dongle firmware binaries.
 *
 * Firmware binaries referenced:
 *   - contact_v1 : ground_truth/firmware/contact_v1_777B42DF_2026-10-24/flash.bin
 *   - motion_v1  : ground_truth/firmware/motion_v1_777A9A46/dump.bin
 *   - gateway_v18: ground_truth/firmware/gateway_2025-02-08_v1.8/FLASH.bin (+ RAM.bin)
 */

#ifndef STOCK_COMMON_H
#define STOCK_COMMON_H

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

/* ==========================================================================
 * 1. Hardware & Memory-Map Constants (CC1310F128)
 * ========================================================================== */

/* CCFG user record holding the 8-byte ASCII MAC on sensors (flash page 31) */
#define STOCK_CCFG_MAC_ADDR 0x0001FFCCu

/* NVOCTP flash page (flash page 30, 4 KB) */
#define STOCK_NV_PAGE_ADDR 0x0001E000u
#define STOCK_NV_PAGE_SIZE 0x00001000u

/* TI NVOCTP System ID used by ti154stack application layer (ssf.c / csf.c) */
#define NVINTF_SYSID_APP 6u

/* Default 16-byte gateway ENR XXTEA key in SRAM `.data` (`0x20001374`) before `0x430A` (`SET_ENR`):
 *   - gateway_v18 SRAM: 0x20001374 (`"WYZE098765432101"`)
 */
static const uint8_t g_stockDefaultGatewayEnrKey[16] = {
    'W', 'Y', 'Z', 'E', '0', '9', '8', '7', '6', '5', '4', '3', '2', '1', '0', '1',
};

/* ==========================================================================
 * 2. TI SimpleLink 15.4-Stack (ti154stack) Primitives & NV Structures
 * ========================================================================== */

/*
 * NVINTF_itemID_t (ti154stack/common/inc/nvintf.h)
 * Passed to NVOCTP readItem / writeItem / deleteItem callbacks.
 */
typedef struct {
    uint8_t systemID; /* [0]   NVINTF_SYSID_APP = 6 */
    uint16_t itemID;  /* [2..3] Item ID (1..11) */
    uint16_t subID;   /* [4..5] Sub ID (0, or 0..219 for device list) */
} NVINTF_itemID_t;

/*
 * NVINTF_nvFuncts_t (ti154stack/common/inc/nvintf.h)
 * Function pointer table populated by NVOCTP_loadApiPtrs(&sNvFns):
 *   - contact_v1  RAM: 0x20000904 (pointer at 0x2000083c)
 *   - motion_v1   RAM: 0x20000d5c (pointer at 0x20000c94)
 *   - gateway_v18 RAM: 0x200008fc (pointer at 0x20000958)
 */
typedef struct {
    uint8_t (*initNV)(void *param);                                                 /* +0x00 */
    uint8_t (*compactNV)(uint16_t minAvail);                                        /* +0x04 */
    uint8_t (*createItem)(NVINTF_itemID_t id, uint32_t len, const void *buf);       /* +0x08 */
    uint8_t (*updateItem)(NVINTF_itemID_t id, uint32_t len, const void *buf);       /* +0x0C */
    uint8_t (*readItem)(NVINTF_itemID_t id, uint16_t ofs, uint16_t len, void *buf); /* +0x10 */
    uint8_t (*writeItem)(NVINTF_itemID_t id, uint16_t len, const void *buf);        /* +0x14 */
    uint8_t (*deleteItem)(NVINTF_itemID_t id);                                      /* +0x18 */
    uint32_t (*getItemLen)(NVINTF_itemID_t id);                                     /* +0x1C */
} NVINTF_nvFuncts_t;

/*
 * Sensor NVOCTP Item IDs (sys = 6, sub = 0 unless noted):
 *   - Item 1 (7B) : Network info (Llc_netInfo_t: channel, panID, shortAddr, state)
 *   - Item 2 (4B) : Sensor configuration (poll/heartbeat/PIR config)
 *   - Item 3 (12B): Offline event log ring-buffer entries (sub = 0..499)
 *   - Item 4 (4B) : Sequence number base (incremented by +25 on every boot & every 25 events)
 *   - Item 5 (8B) : Saved 64-bit millisecond epoch timestamp
 *   - Item 6 (2B) : Boot counter (incremented by +1 on every single boot!)
 *   - Item 8 (25B): Factory identity ({ uint8_t xxtea_key[16]; uint8_t mac[8]; uint8_t valid; })
 *   - Item 9 (1B) : Joined state flag (1 = joined, deleted on button reset)
 *   - Item 10 (1B): Factory test passed flag (1 = passed)
 *   - Item 11 (1B): Low-battery / decommissioned latch flag (0 = normal, 1 = latched)
 */
enum StockSensorNvItem {
    SENSOR_NV_NETWORK_INFO = 1,
    SENSOR_NV_CONFIG = 2,
    SENSOR_NV_HISTORY_LOG = 3,
    SENSOR_NV_SEQ_COUNTER = 4,
    SENSOR_NV_TIME_SYNC = 5,
    SENSOR_NV_BOOT_COUNTER = 6,
    SENSOR_NV_MAC_AND_KEY = 8,
    SENSOR_NV_JOINED_FLAG = 9,
    SENSOR_NV_FACTEST_FLAG = 10,
    SENSOR_NV_LOWBAT_LATCH = 11,
};

/*
 * Sensor NV Item 8 layout (25 bytes, sys=6, item=8, sub=0)
 * Read at boot by Ssf_readMacAndKey() (contact_v1 0x11c50 / motion_v1 0x11ec8)
 * into RAM (contact_v1 0x20000aac / motion_v1 0x20000f08).
 */
typedef struct __attribute__((packed)) {
    uint8_t xxteaKey[16]; /* [0..15]  Per-sensor 16-byte key for A5/A6 XXTEA-19 */
    uint8_t mac[8];       /* [16..23] 8-byte ASCII MAC (e.g. "777B42DF") */
    uint8_t keyValidFlag; /* [24]     0x01 when factory-provisioned via UART cmd 0x0A */
} StockSensorNv8Record_t;

/*
 * Gateway NVOCTP Item IDs (sys = 6, ti154stack Collector csf.c):
 *   - Item 1 (7B) : CSF_NV_NETWORK_INFO_ID (packed Llc_netInfo_t)
 *   - Item 4 (2B) : CSF_NV_DEVICELIST_ENTRIES_ID (uint16_t numEntries)
 *   - Item 5 (24B): CSF_NV_DEVICELIST_ID (sub = 0..219, Llc_deviceListItem_t)
 *   - Item 6 (8B) : Gateway's own 8-byte ASCII MAC (written by 0x4308, read by 0x430A)
 */
enum StockGatewayNvItem {
    CSF_NV_NETWORK_INFO_ID = 1,
    CSF_NV_DEVICELIST_ENTRIES_ID = 4,
    CSF_NV_DEVICELIST_ID = 5,
    CSF_NV_GATEWAY_MAC_ID = 6,
};

/*
 * NV Item 1 (7 bytes): Packed Llc_netInfo_t (ti154stack/common/inc/llc.h)
 * Decodes unknown D7 (gateway_v18 0x13358).
 */
typedef struct __attribute__((packed)) {
    uint8_t isFh;          /* [0] 0x01 (mode flag) */
    uint8_t channel;       /* [1] 0x39 (or 0..24 channel index) */
    uint16_t panID;        /* [2..3] Coordinator PAN ID (e.g. 0x845B) */
    uint16_t shortAddress; /* [4..5] Coordinator short address (e.g. 0xAA55) */
    uint8_t state;         /* [6] Cllc_states_t coordinator state */
} StockLlcNetInfo_t;

/*
 * NV Item 5 (24 bytes, sub = 0..219): Standard TI ti154stack Llc_deviceListItem_t
 * (ti154stack/common/inc/llc.h & api_mac.h).
 * Decodes unknown D7: explains why every NV item 5 record in flash page 30 ends with
 * `00 00 00 00 00 01 00 00 00 00 00 00`!
 *   - Byte 17 is `capInfo.allocAddr = true (0x01)` from the 802.15.4 Association Request
 *     capability byte (0x80).
 *   - Bytes 12..16 are the other 5 false capability booleans.
 *   - Bytes 18..19 and 22..23 are compiler 4-byte struct alignment padding.
 *   - Bytes 20..21 are `uint16_t msa = 0`.
 */
typedef struct {
    /* ApiMac_deviceDescriptor_t devInfo (12 bytes, offset 0..11) */
    uint16_t panID;        /* [0..1]   PAN ID (little-endian, e.g. 0x845B) */
    uint16_t shortAddress; /* [2..3]   Assigned short address (e.g. 0x0001) */
    uint8_t extAddress[8]; /* [4..11]  8-byte ASCII MAC (e.g. "777B42DF") */

    /* ApiMac_capabilityInfo_t capInfo (8 bytes, offset 12..19) */
    bool panCoord;     /* [12] 0x00 */
    bool ffd;          /* [13] 0x00 (Reduced Function Device) */
    bool mainsPower;   /* [14] 0x00 (Battery-powered) */
    bool rxOnWhenIdle; /* [15] 0x00 (Sleepy end device) */
    bool security;     /* [16] 0x00 (MAC-layer security disabled) */
    bool allocAddr;    /* [17] 0x01 (Requested short address in Assoc Req) */
    uint8_t _pad1[2];  /* [18..19] 0x00, 0x00 (struct alignment padding) */

    uint16_t msa;     /* [20..21] 0x0000 */
    uint8_t _pad2[2]; /* [22..23] 0x00, 0x00 (struct alignment padding) */
} StockLlcDeviceListItem_t;

/* ==========================================================================
 * 3. Over-the-Air Application Packet Structures (Plaintext before AES-128-ECB)
 * ========================================================================== */

/*
 * 0xA1 (Heartbeat) and 0xA2 (Event) Report (17 bytes plaintext -> padded to 32B AES)
 * Built by:
 *   - contact_v1: Sensor_sendHeartbeat_A1 (0x0c2f8), Sensor_sendEvent_A2 (0x0b738)
 *   - motion_v1 : Sensor_sendHeartbeat_A1 (0x0be78), Sensor_sendEvent_A2 (0x0b250)
 */
typedef struct __attribute__((packed)) {
    uint8_t cmd;         /* [0]     0xA1 (heartbeat) or 0xA2 (state change event) */
    uint8_t mac[8];      /* [1..8]  8-byte ASCII MAC from NV item 8 (or 00..00 if lost) */
    uint8_t sensorType;  /* [9]     0x01 = contact_v1, 0x02 = motion_v1 */
    int8_t tempDegC;     /* [10]    AONBatMonTemperatureGetDegC() (signed integer C) */
    uint8_t batV32;      /* [11]    Cached (AONBatMonBatteryVoltageGet() >> 3) = V * 32 */
    uint8_t lowBatLatch; /* [12]    0x00 normal; 0x01 if post-TX batV32 < 0x54 3x in a row */
    uint8_t dataLenLo;   /* [13]    Always 0x01 */
    uint8_t state;       /* [14]    Live GPIO state (contact DIO7, motion DIO9) */
    uint16_t seqNumBe;   /* [15..16] Big-endian 16-bit sequence number */
    uint8_t padding[15]; /* [17..31] Zero-padded to 32 bytes (2 AES blocks) */
} StockReportA1A2_t;

/*
 * 0xAB Motion 2-Minute Activity Summary (23 bytes plaintext -> padded to 32B AES)
 * Built by motion_v1 `Sensor_sendPirSummary_AB` (0x0cedc) at the end of twelve
 * 10-second windows (120 s total) following motion trigger.
 */
typedef struct __attribute__((packed)) {
    uint8_t cmd;            /* [0]      0xAB */
    uint8_t mac[8];         /* [1..8]   8-byte ASCII MAC */
    uint8_t sensorType;     /* [9]      0x02 (motion_v1) */
    uint8_t pirBuckets[12]; /* [10..21] PIR pulse count in each of the twelve 10s windows */
    uint8_t tailMarker;     /* [22]     Always 0x01 */
    uint8_t padding[9];     /* [23..31] Zero-padded to 32 bytes */
} StockMotionSummaryAB_t;

/*
 * 0xAD Offline History Log Record (22 bytes plaintext -> padded to 32B AES)
 * Built by `Sensor_sendHistoryLog_AD` (contact_v1 0x08280 / motion_v1 0x07fa8).
 */
typedef struct __attribute__((packed)) {
    uint8_t cmd;          /* [0]      0xAD */
    uint8_t mac[8];       /* [1..8]   8-byte ASCII MAC */
    uint8_t sensorType;   /* [9]      0x01 or 0x02 */
    uint8_t timestamp[8]; /* [10..17] 64-bit millisecond epoch timestamp (big-endian) */
    uint8_t eventType;    /* [18]     0xA2 */
    uint8_t state;        /* [19]     Event state (0 or 1) */
    uint16_t seqNumBe;    /* [20..21] Event sequence number (big-endian) */
    uint8_t padding[10];  /* [22..31] Zero-padded to 32 bytes */
} StockHistoryLogAD_t;

/*
 * 0xA3 Join Request (16 bytes plaintext -> 16B AES)
 * Built by `Sensor_sendJoinReq_A3` (contact_v1 0x0da30 / motion_v1 0x0d968).
 */
typedef struct __attribute__((packed)) {
    uint8_t cmd;            /* [0]    0xA3 */
    uint8_t mac[8];         /* [1..8] 8-byte ASCII MAC */
    uint8_t sensorType;     /* [9]    0x01 (contact_v1) or 0x02 (motion_v1) */
    uint8_t fwVersion;      /* [10]   0x16 (contact_v1) or 0x18/0x19 (motion_v1) */
    uint8_t bootBatV32;     /* [11]   Boot battery reading (V * 32) */
    uint8_t keyValidFlag;   /* [12]   NV item 8 byte 24 (1 = factory key valid) */
    uint8_t facTestFlag;    /* [13]   NV item 10 (1 = factory test passed) */
    uint8_t resetCauseFlag; /* [14]   Boot reset cause flag */
    uint8_t pad;            /* [15]   0x00 */
} StockJoinReqA3_t;

/*
 * 0xB0 Time Sync Downlink (16 bytes plaintext -> 16B AES)
 * Built by gateway `Cllc_sendTimeSync_B0` (gateway_v18 0x0e88c).
 */
typedef struct __attribute__((packed)) {
    uint8_t cmd;          /* [0]     0xB0 */
    uint8_t timestamp[8]; /* [1..8]  64-bit Unix ms clock (big-endian; top 2 bytes 0) */
    uint8_t magic28;      /* [9]     Hardcoded literal 0x28 (ignored by stock sensors) */
    uint8_t padding[6];   /* [10..15] 0x00 */
} StockTimeSyncB0_t;

/* ==========================================================================
 * 4. Shared Cryptographic Primitives (XXTEA-19 & AES-128-ECB)
 * ========================================================================== */

/*
 * Static 16-Byte AES-128-ECB Key (`g_stockAesKey`)
 * SRAM `.data` addresses (initialized from `.cinit` LZSS at boot, never modified):
 *   - contact_v1 : 0x20001944 (0x20001748 + 0x1fc; keyLocation = 0 at 0x20001954)
 *   - motion_v1  : 0x20001988 (0x20001728 + 0x260; keyLocation = 0 at 0x20001998)
 *   - gateway_v18: 0x200014e0 (0x200012f8 + 0x1e8; keyLocation = 0 at 0x200014f0)
 *
 * Value: NIST FIPS-197 Appendix B AES-128 example key from TI's CryptoCC26XX.h.
 */
static const uint8_t g_stockAesKey[16] = {
    0x2Bu, 0x7Eu, 0x15u, 0x16u, 0x28u, 0xAEu, 0xD2u, 0xA6u, 0xABu, 0xF7u, 0x15u, 0x88u, 0x09u, 0xCFu, 0x4Fu, 0x3Cu,
};

/*
 * Stock_xxtea19_encrypt()
 * Flash addresses:
 *   - contact_v1 : 0x00007f98 (called by `Sensor_sendChallengeRsp_A5` at 0x0ea98,
 *                              using 16B key at 0x20000acc from NV item 8 bytes 0..15)
 *   - motion_v1  : 0x0000842c (called by `Sensor_sendChallengeRsp_A5` at 0x0f76a,
 *                              using 16B key at 0x20000f28 from NV item 8 bytes 0..15)
 *   - gateway_v18: 0x00008b2c (called ONLY at 0x039bc for UART `0x4302 -> 0x4303`
 *                              `GET_ENR`, using 16B key at 0x20001374 written by `0x430A`
 *                              at 0x12fa8; never called for RF `0xA4`/`0xA5`/`0xA6`)
 *
 * Note: Neither the sensor nor the gateway binary contains an `XXTEA_decrypt` routine.
 *
 * Encrypts a 16-byte buffer (4 big-endian uint32_t words) in-place using
 * 19-round Corrected Block TEA (XXTEA) with a 16-byte key (4 big-endian words).
 */
static inline void Stock_xxtea19_encrypt(uint8_t buf[16], const uint8_t keyBytes[16])
{
    uint32_t v[4];
    uint32_t k[4];
    for (int i = 0; i < 4; i++) {
        v[i] = ((uint32_t)buf[4 * i] << 24) | ((uint32_t)buf[4 * i + 1] << 16) | ((uint32_t)buf[4 * i + 2] << 8) |
               (uint32_t)buf[4 * i + 3];
        k[i] = ((uint32_t)keyBytes[4 * i] << 24) | ((uint32_t)keyBytes[4 * i + 1] << 16) |
               ((uint32_t)keyBytes[4 * i + 2] << 8) | (uint32_t)keyBytes[4 * i + 3];
    }

    uint32_t sum = 0;
    uint32_t z = v[3];
    for (int round = 0; round < 19; round++) {
        sum -= 0x61C88647u; /* Equivalent to sum += 0x9E3779B9u */
        uint32_t e = (sum >> 2) & 3u;
        for (uint32_t p = 0; p < 4; p++) {
            uint32_t y = v[(p + 1) & 3u];
            uint32_t mx = (((z >> 5) ^ (y << 2)) + ((y >> 3) ^ (z << 4))) ^ ((sum ^ y) + (k[(p & 3u) ^ e] ^ z));
            v[p] += mx;
            z = v[p];
        }
    }

    for (int i = 0; i < 4; i++) {
        buf[4 * i + 0] = (uint8_t)(v[i] >> 24);
        buf[4 * i + 1] = (uint8_t)(v[i] >> 16);
        buf[4 * i + 2] = (uint8_t)(v[i] >> 8);
        buf[4 * i + 3] = (uint8_t)(v[i]);
    }
}

/*
 * Stock_aes128_ecb_encryptBuffer()
 * Flash addresses:
 *   - contact_v1 : 0x0000c5f0 (calls `AES_encryptBlock16` at 0x00010994; outBuf = 0x2000187c)
 *   - motion_v1  : 0x0000c7c0 (calls `AES_encryptBlock16` at 0x00010bbc; outBuf = 0x200018c0)
 *   - gateway_v18: 0x0000d5a4 (calls `AES_encryptBlock16` at 0x000127a0; outBuf = 0x200013d0)
 *
 * Each `AES_encryptBlock16()` invocation executes:
 *   1. `CryptoCC26XX_Transac_init(&trans, CRYPTOCC26XX_OP_AES_ECB_NOCRYPT = 4)`
 *   2. `keyIndex = CryptoCC26XX_allocateKey(cryptoHandle, CRYPTOCC26XX_KEY_0, g_stockAesKey)`
 *   3. `CryptoCC26XX_transact(cryptoHandle, &trans)`
 *   4. `CryptoCC26XX_releaseKey(cryptoHandle, &keyIndex)`
 *
 * BUG IN ALL STOCK BINARIES (`known-issues.md` §3):
 *   The `if (len <= 16)` branch at `0x0c5d0` / `0x0c7ea` / `0x0d5ce` is missing an
 *   `else` / `return`, so 16-byte inputs (`0xA3`, `0xA6`, `0xA7`, `0xB0`, `0xC0`,
 *   `0xD0`, `0xD2`, `0xE0`, `0xE2`) fall straight through into the 32-byte path,
 *   running 3 hardware AES operations instead of 1 and reading 16 bytes out-of-bounds
 *   from `inBuf[16..31]` into `outBuf[16..31]`.
 */
static inline void Stock_aes128_ecb_encryptBuffer(const uint8_t *inBuf, int len, uint8_t outBuf[32],
                                                  void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]))
{
    uint8_t blk0[16] = {0};
    uint8_t blk1[16] = {0};

    if (len <= 16) {
        aes128EcbBlock16(inBuf, outBuf);
        /* BUG: Missing return in stock firmware! Falls through to 32-byte path below. */
    }

    aes128EcbBlock16(inBuf, blk0);
    aes128EcbBlock16(inBuf + 16, blk1);
    memcpy(outBuf, blk0, 16);
    memcpy(outBuf + 16, blk1, 16);
}

/*
 * Stock_aes128_ecb_decryptBuffer()
 * Flash addresses:
 *   - contact_v1 : 0x000086bc (calls `AES_decryptBlock16` at 0x00010940; outBuf = 0x200018bc)
 *   - motion_v1  : 0x000089e0 (calls `AES_decryptBlock16` at 0x00010b68; outBuf = 0x20001900)
 *   - gateway_v18: 0x0000924c (calls `AES_decryptBlock16` at 0x000112a8; outBuf = 0x20001410)
 *
 * Each `AES_decryptBlock16()` invocation executes `CryptoCC26XX` with
 * `CRYPTOCC26XX_OP_AES_ECB_NOCRYPT_DEC = 5` and `keySrc = g_stockAesKey`.
 *
 * TWO BUGS IN ALL STOCK BINARIES (`known-issues.md` §3):
 *   1. Missing `else` after `if (len <= 16)` (`0x086e8` / `0x09278`): falls through
 *      into the 32-byte path and reads 16 bytes out-of-bounds (`inBuf + 16`).
 *   2. Broken `if (len > 32)` branch (`0x08770..0x0880a` / `0x09306..0x0939a`):
 *      decrypts `inBuf + 16` twice instead of `inBuf + 32` and copies both blocks
 *      to `outBuf + 16` without advancing the output pointer.
 */
static inline void Stock_aes128_ecb_decryptBuffer(const uint8_t *inBuf, int len, uint8_t outBuf[48],
                                                  void (*aes128EcbDecBlock16)(const uint8_t in[16], uint8_t out[16]))
{
    uint8_t blk0[16] = {0};
    uint8_t blk1[16] = {0};
    uint8_t blk2[16] = {0};
    (void)blk2;

    if (len <= 16) {
        aes128EcbDecBlock16(inBuf, outBuf);
        /* BUG: Missing return in stock firmware! Falls through to 32-byte path below. */
    }

    aes128EcbDecBlock16(inBuf, blk0);
    aes128EcbDecBlock16(inBuf + 16, blk1);
    memcpy(outBuf, blk0, 16);
    memcpy(outBuf + 16, blk1, 16);

    if (len > 32) {
        /* BUG (0x08770 / 0x09306): Decrypts (inBuf + 16) twice and overwrites (outBuf + 16)! */
        aes128EcbDecBlock16(inBuf, blk0);
        aes128EcbDecBlock16(inBuf + 16, blk1);
        aes128EcbDecBlock16(inBuf + 16, blk2);
        memcpy(outBuf, blk0, 16);
        memcpy(outBuf + 16, blk1, 16);
        memcpy(outBuf + 16, blk2, 16);
    }
}

#endif /* STOCK_COMMON_H */
