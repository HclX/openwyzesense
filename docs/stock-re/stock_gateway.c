/*
 * Reconstructed C source code of the stock Wyze Sense USB Dongle (Gateway)
 * application (`gateway_2025-02-08_v1.8/FLASH.bin` + `RAM.bin`), built on TI's
 * SimpleLink CC13x0 SDK 15.4-Stack (`ti154stack` collector example).
 *
 * Every function and global variable is annotated with its exact flash or SRAM
 * address in `gateway_2025-02-08_v1.8`.
 */

#include "stock_common.h"

/* ==========================================================================
 * 1. Global Data Structures & SRAM Variable Map (`gateway_v1.8`)
 * ========================================================================== */

/*
 * Board GPIO & UART0 Hardware Attributes
 * Flash addresses:
 *   - UARTCC26XX_hwAttrs : 0x00017328 (base 0x40001000, int 0x15, TX=DIO0, RX=DIO1, 115200 8N1)
 *   - BoardGpioInitTable : 0x000174c0 (DIO10 & DIO13 = LED outputs; DIO1 = UART RX / SBL backdoor)
 *   - Firmware version   : 0x00016ec8 ("1.8.0.0\0") / RAM 0x2000184c ("0.0.0.47 V1.8 Gateway GW3U")
 */

/*
 * Collector State & Downlink Control Block (`g_collectorCtx`)
 * SRAM base address: 0x200012f8 (register `r5` in `Collector_process` / `Uart_processCommands`)
 */
typedef struct {
    uint8_t rfTestMode;          /* +0x00 (0x200012f8): Non-zero enables 0x53F2 RF echo */
    uint8_t g_joinPollState;     /* +0x03 (0x200012fb): 1..12 TBB dispatch on sensor poll */
    uint8_t rfTestChannel;       /* +0x04 (0x200012fc): Channel set by 0x532A */
    uint8_t txRetryCount;        /* +0x05 (0x200012fd): Un-ACKed UART TX retry counter (0..3) */
    uint8_t c0ArgByte;           /* +0x06 (0x200012fe): 1B arg from 0x5380 for RF 0xC0 */
    uint8_t scanInclusionEnable; /* +0x07 (0x200012ff): 0xFF when inclusion enabled via 0x533D */
    uint8_t e0ArgByte;           /* +0x08 (0x20001300): 1B arg from 0x5390 for RF 0xE0 */
    uint8_t e2ArgByte;           /* +0x09 (0x20001301): 1B arg from 0x5392 for RF 0xE2 */
    uint8_t ledBlinkToggle;      /* +0x0a (0x20001302): Toggled by 300ms clock on event 0x0020 */
    uint8_t permitJoinActive;    /* +0x0b (0x20001303): 1 while 60s scan timer is active */
    uint8_t timeSyncMinuteTicks; /* +0x0c (0x20001304): Counts 0..10 (11 min) between 0x5332 reqs */
    uint8_t cllcState;           /* +0x0e (0x20001306): Cllc_states_t (>= 3 when network is up) */
    uint16_t collectorEvents;    /* +0x16 (0x2000130e): Event bitmask for Collector_process */
    uint16_t diagLogDumpCount;   /* +0x18 (0x20001310): Remaining 0x5335 log chunks to send */
    uint16_t offlineA2Count;     /* +0x1c (0x20001314): Queued 22B offline A2 events (max 30) */
    uint16_t offlineA2WriteOfs;  /* +0x26 (0x2000131e): Byte write offset (0..660) in 0x20001b98 */
    uint8_t d0ArgBytes[3];       /* +0x28 (0x20001320): 3B args from 0x5370/0x5372 for RF 0xD0/0xD2 */
    uint64_t gatewayMsClock;     /* +0x48 (0x20001340): 64-bit Unix ms clock (set by 0x5333, +10ms tick) */
    uint8_t gatewayMac[8];       /* +0x50 (0x20001348): Gateway 8B ASCII MAC (NV item 0x20) */
    uint8_t gwEnrXxteaKey[16];   /* +0x7c (0x20001374): Gateway 16B ENR XXTEA key (NV item 0x30) */
} StockCollectorCtx_t;

/* SRAM at 0x200012f8 */
static StockCollectorCtx_t g_collectorCtx;

/*
 * Gateway UART & Protocol State Block (`g_gwCtx`)
 * SRAM base address: 0x200017c8 (register `r4` in `Uart_processCommands` / `Uart_buildAndSendReply`)
 */
typedef struct {
    uint8_t chimeActiveFlag;           /* +0x001 (0x200017c9): Set to 1 by 0x53E9 (50s noise monitor) */
    uint8_t rfTestModeFlag;            /* +0x002 (0x200017ca): Checked by 0x5318 RF test RX reply */
    uint8_t lastTxFrameLen;            /* +0x003 (0x200017cb): Total byte length of lastTxFrame */
    uint8_t rxLenOrLastCmd;            /* +0x004 (0x200017cc): RX b2 length or last sent UART cmd */
    uint8_t pendingReplyCmd;           /* +0x006 (0x200017ce): Reply opcode for Uart_buildAndSendReply */
    uint8_t txCsumOffset;              /* +0x007 (0x200017cf): Offset of checksum high byte in uartTxBuf */
    uint8_t txPayloadPlus2Len;         /* +0x008 (0x200017d0): b2 - 1 */
    uint8_t txTotalLen;                /* +0x009 (0x200017d1): Total UART frame length (b2 + 6) */
    uint8_t joinApprovedFlag;          /* +0x00a (0x200017d2): 1 when 0x5323 approves sensor join */
    uint8_t verifyTimeoutFlag;         /* +0x00b (0x200017d3): 1 when awaiting 0xA6 completion */
    uint8_t finishAuthDone;            /* +0x00c (0x200017d4): 1 after host sends 0x5314 with 0xFF */
    uint8_t diagLogReadOfs;            /* +0x00d (0x200017d5): Current byte offset in diagLogBuf */
    uint8_t uartRxState;               /* +0x00f (0x200017d7): 0=wait AA, 1=wait 55, 2=wait 53, 3=payload */
    uint8_t joinVerifyRsp[2];          /* +0x010 (0x200017d8): 2B status from 0x5323 (e.g. 0xFF, 0x04) */
    uint8_t txCsumHi;                  /* +0x014 (0x200017dc): Computed TX checksum MSB */
    uint8_t txCsumLo;                  /* +0x015 (0x200017dd): Computed TX checksum LSB */
    uint16_t targetShortAddr;          /* +0x016 (0x200017de): Short address of sensor targeted by UART cmd */
    uint16_t sameTimestampCounter;     /* +0x018 (0x200017e0): Sub-ms tiebreaker for 0xAD history logs */
    uint16_t offlineA2ReadOfs;         /* +0x01a (0x200017e2): Byte read offset (0..638) in 0x20001b98 */
    uint16_t lastA2SeqNum;             /* +0x01c (0x200017e4): Last 0xA2 sequence number (dedup check) */
    uint16_t diagLogTotalOfs;          /* +0x01e (0x200017e6): Total written bytes in diagLogBuf */
    uint8_t cmdTargetMac[8];           /* +0x030 (0x200017f8): 8B MAC of sensor targeted by UART cmd */
    uint8_t timeSyncPayload[8];        /* +0x038 (0x20001800): Raw 8B big-endian ms from 0x5333 */
    uint64_t lastHistoryBaseMs;        /* +0x040 (0x20001808): Base timestamp for 0xAD log conversion */
    uint64_t lastA2GatewayMs;          /* +0x048 (0x20001810): Gateway ms timestamp of last 0xA2 */
    uint8_t lastA2Mac[8];              /* +0x050 (0x20001818): 8B MAC of last 0xA2 (dedup check) */
    uint8_t deleteAllArg[9];           /* +0x058 (0x20001820): 9B payload saved by 0x533A */
    uint8_t joinChallenge[16];         /* +0x060 (0x20001828): 16B challenge from 0x5321 for RF 0xA4 */
    uint8_t e9PayloadBuf[20];          /* +0x070 (0x20001838): Variable payload from 0x5353 for RF 0xE9 */
    char versionString[27];            /* +0x084 (0x2000184c): "0.0.0.47 V1.8 Gateway GW3U" */
    uint8_t diagLogBuf[30];            /* +0x09f (0x20001867): Diagnostic log buffer for 0x5335 */
    uint8_t lastReplyCmdCopy;          /* +0x0a0 (0x20001868): Copy of pendingReplyCmd */
    uint8_t pendingCmdCopy[40];        /* +0x0bd (0x20001885): Saved [len+1, cmd, payload...] until ACK */
    uint8_t unackedA2AlarmPayload[40]; /* +0x0e5 (0x200018ad): Saved 0xA2 payload until 0x5319 ACKed */
    uint8_t lastTxFrame[40];           /* +0x10d (0x200018d5): Last TX frame for 150ms x 4 retransmit */
    uint8_t b3b5PayloadBuf[130];       /* +0x149 (0x20001911): 36B (0x535A->0xB3) or 130B (0x535C->0xB5) */
    uint16_t pairedShortAddrs[110];    /* +0x1cc (0x20001994): In-RAM cache of 110 paired short addresses */
    uint8_t uartTxBuf[256];            /* +0x2a8 (0x20001a70): UART TX frame buffer */
    uint8_t uartRxBuf[40];             /* +0x3a8 (0x20001b70): Parsed UART RX command + payload */
    uint8_t offlineA2Ring[660];        /* +0x3d0 (0x20001b98): 30 x 22B offline 0xA2 records for 0x5337 */
} StockGatewayCtx_t;

/* SRAM at 0x200017c8 */
static StockGatewayCtx_t g_gwCtx;

/*
 * Per-Sensor `0xB4` Sequence Number Dedup Cache (`g_lastB4SeqNum`)
 * SRAM base address: 0x20000b78 (100 entries x 6 bytes)
 */
static uint16_t g_lastB4SeqNum[110];

/*
 * Offline `0xA2` Event Record Layout (22 bytes, stored in `g_gwCtx.offlineA2Ring` at `0x20001b98`)
 * Drained via `0x5337` (`Collector_process` event `0x0200` at `0x00001f42`).
 */
typedef struct __attribute__((packed)) {
    uint8_t a2Cmd;        /* [0]      0xA2 */
    uint8_t mac[8];       /* [1..8]   8-byte ASCII MAC */
    uint8_t sensorType;   /* [9]      Sensor type */
    uint8_t timestamp[8]; /* [10..17] 64-bit gateway ms clock (big-endian) */
    uint8_t eventType;    /* [18]     0xA2 */
    uint8_t state;        /* [19]     0x00 or 0x01 */
    uint16_t seqNumBe;    /* [20..21] Big-endian sequence counter */
} StockOfflineA2Record_t;

/*
 * Other Key Gateway SRAM Globals:
 *   - 0x200008fc: NVINTF_nvFuncts_t sNvFns (pointer at 0x20000958)
 *   - 0x200013d0: uint8_t  g_aesEncOutBuf[32]  (output buffer of `AES_encryptBuffer` at 0x0d5a4)
 *   - 0x20001410: uint8_t  g_aesDecOutBuf[48]  (output buffer of `AES_decryptBuffer` at 0x0924c)
 *   - 0x200014e0: uint8_t  g_runtimeAesKey[16] (static 16-byte AES-128-ECB key, never modified)
 *   - 0x20003e18: uint8_t  g_uartRxRing[512]   (interrupt RX ring buffer for UART0)
 */
static NVINTF_nvFuncts_t *g_gwNv;

/* Forward declarations for UART & NV helpers */
void Uart_send53(uint8_t cmd, const void *payload, uint8_t len);
void Uart_buildAndSendReply(const uint8_t *srcBuf);
void Uart_sendTimeSyncReq_5332(void);
void Csf_sendSensorList_5331(void);

/* ==========================================================================
 * 2. NVOCTP Collector Device List & Custom NV Helpers (`csf.c`) (Unknown D7 & Bug §2)
 * ========================================================================== */

/*
 * Csf_writeCustomNvItem()
 * Flash address: 0x00012cb0
 *
 * Writes or creates a custom gateway NVOCTP item under `systemID = 6`, `subID = 0`
 * (used for `itemID = 0x10..0x13` factory/inclusion flags, `0x20` gateway MAC,
 * and `0x30` gateway 16-byte ENR XXTEA key).
 */
void Csf_writeCustomNvItem(const uint8_t *recWithIdPrefix, uint8_t totalLen)
{
    if (!g_gwNv || !recWithIdPrefix || totalLen < 2u) {
        return;
    }
    NVINTF_itemID_t id = {NVINTF_SYSID_APP, recWithIdPrefix[0], 0};
    uint32_t existingLen = g_gwNv->getItemLen(id);
    uint8_t payloadLen = (uint8_t)(totalLen - 1u);
    if (existingLen == 0u) {
        g_gwNv->createItem(id, payloadLen, &recWithIdPrefix[1]);
    } else {
        g_gwNv->writeItem(id, payloadLen, &recWithIdPrefix[1]);
    }
}

/*
 * Csf_readCustomNvItem()
 * Flash address: 0x00012944
 *
 * Reads `len` bytes from custom gateway NVOCTP item `(6, itemId, 0)`.
 */
bool Csf_readCustomNvItem(uint8_t itemId, uint8_t len, void *outBuf)
{
    if (!g_gwNv || !outBuf) {
        return false;
    }
    NVINTF_itemID_t id = {NVINTF_SYSID_APP, itemId, 0};
    return (g_gwNv->readItem(id, 0, len, outBuf) == 0u);
}

/*
 * Csf_writeGatewayEnrKey()
 * Flash address: 0x00012fa8
 *
 * Called by UART command `0x0A` (`WRITE_KEY`, `0x0326e`). Copies the 16-byte key
 * into `g_collectorCtx.gwEnrXxteaKey` (`0x20001374`), persists `{0x30, key[0..15]}`
 * to NVOCTP item `(6, 0x30, 0)`, and sends `0x530B` (`55 AA 53 04 0B FF <csum>`).
 */
void Csf_writeGatewayEnrKey(const uint8_t key[16])
{
    uint8_t nvRec[17];
    nvRec[0] = 0x30u;
    memcpy(&nvRec[1], key, 16);
    memcpy(g_collectorCtx.gwEnrXxteaKey, key, 16);
    Csf_writeCustomNvItem(nvRec, sizeof(nvRec));
    g_gwCtx.pendingReplyCmd = 0x0Bu;
    Uart_buildAndSendReply(NULL);
}

/*
 * Csf_getNumDeviceListEntries()
 * Flash address: 0x00013088
 *
 * Reads the 2-byte `uint16_t` count of paired sensors from NVOCTP item (6, 4, 0)
 * (`CSF_NV_DEVICELIST_ENTRIES_ID`).
 */
uint16_t Csf_getNumDeviceListEntries(void)
{
    NVINTF_itemID_t id = {NVINTF_SYSID_APP, CSF_NV_DEVICELIST_ENTRIES_ID, 0};
    uint16_t count = 0;

    if (g_gwNv && g_gwNv->readItem(id, 0, sizeof(count), &count) != 0) {
        count = 0;
    }
    return count;
}

/*
 * Csf_getDevice()
 * Flash address: 0x0000e56c
 *
 * Searches NVOCTP item (6, 5, 0..219) (`CSF_NV_DEVICELIST_ID`, 24-byte
 * `StockLlcDeviceListItem_t`) by either short address (`addrMode == 2`, offset +2)
 * or 8-byte extended ASCII MAC address (`addrMode == 3`, offset +4..+11).
 */
bool Csf_getDevice(uint8_t addrMode, uint16_t shortAddr, const uint8_t extMac[8], StockLlcDeviceListItem_t *outItem)
{
    uint16_t targetCount = Csf_getNumDeviceListEntries();
    if (!g_gwNv || !outItem || targetCount == 0) {
        return false;
    }

    uint16_t seenLive = 0;
    for (uint16_t sub = 0; sub < 220u && seenLive < targetCount; sub++) {
        NVINTF_itemID_t id = {NVINTF_SYSID_APP, CSF_NV_DEVICELIST_ID, sub};
        StockLlcDeviceListItem_t item;
        if (g_gwNv->readItem(id, 0, sizeof(item), &item) == 0) {
            if (addrMode == 2u && item.shortAddress == shortAddr) {
                *outItem = item;
                return true;
            }
            if (addrMode == 3u && memcmp(item.extAddress, extMac, 8) == 0) {
                *outItem = item;
                return true;
            }
            seenLive++;
        }
    }
    return false;
}

/*
 * Csf_getDeviceShortByMac()
 * Flash address: 0x00014acc
 *
 * Wrapper around `Csf_getDevice(3, ...)` that returns the sensor's `shortAddress`
 * if its 8-byte ASCII MAC is in NV item 5, or `0xFFFF` if not found.
 */
uint16_t Csf_getDeviceShortByMac(const uint8_t extMac[8])
{
    StockLlcDeviceListItem_t item;
    if (Csf_getDevice(3u, 0, extMac, &item)) {
        return item.shortAddress;
    }
    return 0xFFFFu;
}

/*
 * Csf_findDeviceByMac()
 * Flash address: 0x00010460
 *
 * Searches NV item (6, 5, 0..219) for an 8-byte ASCII MAC and returns its NV
 * `subID` (`0..219`), or `-1` if the MAC is not paired.
 */
int Csf_findDeviceByMac(const uint8_t extMac[8])
{
    uint16_t targetCount = Csf_getNumDeviceListEntries();
    if (!g_gwNv || !extMac || targetCount == 0) {
        return -1;
    }

    uint16_t seenLive = 0;
    for (uint16_t sub = 0; sub < 220u && seenLive < targetCount; sub++) {
        NVINTF_itemID_t id = {NVINTF_SYSID_APP, CSF_NV_DEVICELIST_ID, sub};
        StockLlcDeviceListItem_t item;
        if (g_gwNv->readItem(id, 0, sizeof(item), &item) == 0) {
            if (memcmp(item.extAddress, extMac, 8) == 0) {
                return (int)sub;
            }
            seenLive++;
        }
    }
    return -1;
}

/*
 * Csf_removeDevice()
 * Flash address: 0x0001064c
 *
 * Deletes the paired sensor matching `extMac` from NVOCTP item `(6, 5, sub)`
 * and decrements the sensor count in NVOCTP item `(6, 4, 0)`.
 */
bool Csf_removeDevice(const uint8_t extMac[8])
{
    int sub = Csf_findDeviceByMac(extMac);
    if (!g_gwNv || sub < 0) {
        return false;
    }
    NVINTF_itemID_t devId = {NVINTF_SYSID_APP, CSF_NV_DEVICELIST_ID, (uint16_t)sub};
    if (g_gwNv->deleteItem(devId) == 0u) {
        uint16_t count = Csf_getNumDeviceListEntries();
        if (count > 0u) {
            count--;
            NVINTF_itemID_t cntId = {NVINTF_SYSID_APP, CSF_NV_DEVICELIST_ENTRIES_ID, 0};
            g_gwNv->writeItem(cntId, sizeof(count), &count);
        }
        return true;
    }
    return false;
}

/*
 * Csf_sendSensorList_5331()
 * Flash address: 0x0000dc24
 *
 * Called when `pendingReplyCmd == 0x31` (`GET_SENSOR_LIST`, `0x038e0`).
 * Iterates through all paired sensors (`0..count-1`) via `Csf_getDeviceItem`
 * (`0x102ec`), skips all-`0xFF` or all-`0x00` MACs, and sends one 15-byte
 * `55 AA 53 0B 31 <8B MAC> <csum_hi> <csum_lo>` (`0x5331`) frame per sensor,
 * delaying 10 ms (`0x16a68(10000)`) between frames.
 */
void Csf_sendSensorList_5331(void)
{
    uint16_t count = Csf_getNumDeviceListEntries();
    uint16_t seenLive = 0;

    for (uint16_t sub = 0; sub < 220u && seenLive < count; sub++) {
        NVINTF_itemID_t id = {NVINTF_SYSID_APP, CSF_NV_DEVICELIST_ID, sub};
        StockLlcDeviceListItem_t item;
        if (g_gwNv && g_gwNv->readItem(id, 0, sizeof(item), &item) == 0) {
            seenLive++;
            static const uint8_t kAllFf[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
            static const uint8_t kAll00[8] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
            if (memcmp(item.extAddress, kAllFf, 8) != 0 && memcmp(item.extAddress, kAll00, 8) != 0) {
                Uart_send53(0x31u, item.extAddress, 8);
            }
        }
    }
}

/*
 * Csf_deleteAllAndReboot_5340()
 * Flash address: 0x00011310
 *
 * Called when `pendingReplyCmd == 0x40` (reply to `0x533F` `DELETE_ALL` at `0x038ca`).
 * Sends `55 AA 53 04 40 FF <csum>`, waits 10 ms, disassociates all sensors (`0x14b84`),
 * wipes the in-RAM device tables (`0x20000dd0`, `0x20000b78`, `0x20001994`) with `0xFF`,
 * and triggers a hardware reboot via `SysCtrlSystemReset()` (`0x16184`).
 */
void Csf_deleteAllAndReboot_5340(void (*sysCtrlSystemReset)(void))
{
    uint8_t status = 0xFFu;
    Uart_send53(0x40u, &status, 1);
    memset(g_gwCtx.pairedShortAddrs, 0xFF, sizeof(g_gwCtx.pairedShortAddrs));
    memset(g_lastB4SeqNum, 0xFF, sizeof(g_lastB4SeqNum));
    if (sysCtrlSystemReset) {
        sysCtrlSystemReset();
    }
}

/* ==========================================================================
 * 3. IEEE 802.15.4 Association & Short Address Assignment Bug (`known-issues.md` §2)
 * ========================================================================== */

/*
 * Cllc_assocIndCb()
 * Flash address: 0x0000e60c
 *
 * Handles an incoming 802.15.4 MLME-ASSOCIATE.indication from a joining sensor.
 *
 * ROOT CAUSE OF DUPLICATE SHORT ADDRESS BUG (`docs/known-issues.md` §2):
 *   1. Calls `Csf_getDeviceShortByMac(extAddress)` (`0x14acc`).
 *   2. If the sensor MAC is already in NV item 5 (`shortAddr != 0xFFFF`), it
 *      reuses that sensor's existing `shortAddr`.
 *   3. If the sensor MAC is NOT in NV item 5 (`shortAddr == 0xFFFF`), lines
 *      `0x0e638..0x0e640` assign:
 *          `newShortAddr = Csf_getNumDeviceListEntries() + 1;`
 *      Because `Csf_getNumDeviceListEntries()` is the *current* count of active
 *      sensors rather than a monotonic counter or free-address search, deleting
 *      a sensor from the middle of the table decrements `count`, causing the next
 *      newly paired sensor to be assigned `count + 1` — colliding with an existing
 *      sensor that already holds that short address!
 */
uint16_t Cllc_assocIndCb(const uint8_t extAddress[8])
{
    uint16_t shortAddr = Csf_getDeviceShortByMac(extAddress);
    if (shortAddr == 0xFFFFu) {
        /* BUG (0x0e638): Collides whenever a lower-numbered sensor was deleted! */
        uint16_t currentCount = Csf_getNumDeviceListEntries();
        shortAddr = (uint16_t)(currentCount + 1u);
    }
    return shortAddr;
}

/* ==========================================================================
 * 4. Downlink RF Packet Builders (`g_joinPollState` 1..12) (Unknowns D1, D2, D5)
 * ========================================================================== */

/*
 * Cllc_sendDataReq()
 * Flash address: 0x0000d2dc
 *
 * Queues an 802.15.4 indirect MAC data frame (`ApiMac_mcpsDataReq`).
 *
 * RESOLVES UNKNOWN D2 (`0x535E` with payload `0xF1`):
 *   At `0x0000d35e`, if `ApiMac_mcpsDataReq()` returns a non-zero status (such as
 *   `0xF1` = `ApiMac_status_transactionOverflow` when the indirect TX queue is full),
 *   the gateway immediately sends `55 AA 53 04 5E <macStatus> <csum>` (`0x535E`)
 *   to the host!
 */
void Cllc_sendDataReq(uint16_t dstShortAddr, const uint8_t *payload, uint8_t len,
                      uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    if (!apiMacDataReq) {
        return;
    }
    uint8_t status = apiMacDataReq(dstShortAddr, payload, len);
    if (status != 0u) {
        /* 0x0000d35e: Sends 0x535E with 1-byte ApiMac_status_t (e.g. 0xF1) */
        Uart_send53(0x5Eu, &status, 1);
    }
}

/*
 * Cllc_sendTimeSync_B0()
 * Flash address: 0x0000e88c
 *
 * Builds a 16-byte `0xB0` time-sync packet, encrypts it with `AES_encryptBuffer`
 * (`0x0d5a4`), and queues it for `dstShortAddr`.
 *
 * RESOLVES UNKNOWN D1:
 *   At `0x0000e8fa` (`movs r3, #0x28; strb.w r3, [sp, #0x25]`), byte 9 of `0xB0`
 *   is a hardcoded literal constant `0x28` (40).
 */
void Cllc_sendTimeSync_B0(uint16_t dstShortAddr, uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    StockTimeSyncB0_t b0;
    memset(&b0, 0, sizeof(b0));

    b0.cmd = 0xB0u;
    for (int i = 0; i < 8; i++) {
        b0.timestamp[i] = (uint8_t)(g_collectorCtx.gatewayMsClock >> (56 - 8 * i));
    }
    b0.magic28 = 0x28u; /* 0x0000e8fa: Hardcoded literal 0x28 */

    Cllc_sendDataReq(dstShortAddr, (const uint8_t *)&b0, sizeof(b0), apiMacDataReq);
}

/*
 * Cllc_sendChallenge_A4()
 * Flash address: 0x0000f878
 *
 * Called when `g_joinPollState == 1` (`0x01cda`, queued by host UART `0x5321`).
 * Wraps `[0xA4, g_joinChallenge[0..15], 0...]` (32 bytes) in AES-128-ECB via
 * `AES_encryptBuffer` (`0x0d5a4`) — NO XXTEA is performed on `0xA4` — and queues
 * it for `dstShortAddr` via `Cllc_sendDataReq` (`0x0d2b0`).
 */
void Cllc_sendChallenge_A4(uint16_t dstShortAddr, const uint8_t challenge[16],
                           uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    uint8_t a4[32];
    memset(a4, 0, sizeof(a4));
    a4[0] = 0xA4u;
    memcpy(&a4[1], challenge, 16);

    Cllc_sendDataReq(dstShortAddr, a4, sizeof(a4), apiMacDataReq);
}

/*
 * Cllc_sendVerifyRsp_A6()
 * Flash address: 0x0000c608
 *
 * Called when `g_joinPollState == 2` (`0x01cc8`, queued by host UART `0x5323`).
 * Builds a 16-byte `0xA6` packet (`[0]=0xA6, [1..2]=verifyRsp` (`0xFF 0x04`),
 * `[3..10]=big-endian 64-bit gateway ms clock`, `[11..15]=0x00`) and encrypts it
 * with `AES_encryptBuffer(buf, 16)` (`0x0d5a4`) — NO XXTEA is used on `0xA6`.
 */
void Cllc_sendVerifyRsp_A6(uint16_t dstShortAddr, const uint8_t verifyRsp[2],
                           uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    if (verifyRsp[0] == 0u || verifyRsp[1] == 0u) {
        return;
    }

    uint8_t a6[16];
    memset(a6, 0, sizeof(a6));
    a6[0] = 0xA6u;
    a6[1] = verifyRsp[0]; /* 0xFF */
    a6[2] = verifyRsp[1]; /* 0x04 (join ok) or 0x05 (decommission) */
    for (int i = 0; i < 8; i++) {
        a6[3 + i] = (uint8_t)(g_collectorCtx.gatewayMsClock >> (56 - 8 * i));
    }

    Cllc_sendDataReq(dstShortAddr, a6, sizeof(a6), apiMacDataReq);
}

/*
 * Cllc_sendRfCmd_C0()
 * Flash address: 0x00011f7c
 *
 * Called when `g_joinPollState == 5` (`0x01ca2`, queued by host UART `0x5380`).
 * Builds 16-byte plaintext `[0]=0xC0, [1..8]=mac, [9]=arg1, [10]=c0ArgByte`,
 * encrypts with `AES_encryptBuffer(buf, 16)` (`0x0d5a4`), and queues via `Cllc_sendDataReq`.
 */
void Cllc_sendRfCmd_C0(uint16_t dstShortAddr, uint8_t arg1, const uint8_t mac[8],
                       uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    uint8_t pkt[16];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xC0u;
    memcpy(&pkt[1], mac, 8);
    pkt[9] = arg1;
    pkt[10] = g_collectorCtx.c0ArgByte;
    if (g_collectorCtx.rfTestMode != 0u) {
        g_gwCtx.pendingReplyCmd = 0xF2u;
        Uart_buildAndSendReply(pkt);
    }
    Cllc_sendDataReq(dstShortAddr, pkt, sizeof(pkt), apiMacDataReq);
}

/*
 * Cllc_sendRfCmd_D0()
 * Flash address: 0x00011650
 *
 * Called when `g_joinPollState == 6` (`0x01c92`, queued by host UART `0x5370`).
 * Builds 16-byte plaintext `[0]=0xD0, [1..8]=mac, [9..11]=d0ArgBytes[0..2]`
 * (updates `Sensor_processConfig_D0` on the sensor: heartbeat interval & config)
 * and queues via `Cllc_sendDataReq`.
 */
void Cllc_sendRfCmd_D0(uint16_t dstShortAddr, const uint8_t d0Args[3], const uint8_t mac[8],
                       uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    uint8_t pkt[16];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xD0u;
    memcpy(&pkt[1], mac, 8);
    memcpy(&pkt[9], d0Args, 3);
    if (g_collectorCtx.rfTestMode != 0u) {
        g_gwCtx.pendingReplyCmd = 0xF2u;
        Uart_buildAndSendReply(pkt);
    }
    Cllc_sendDataReq(dstShortAddr, pkt, sizeof(pkt), apiMacDataReq);
}

/*
 * Cllc_sendRfCmd_E0()
 * Flash address: 0x00011fdc
 *
 * Called when `g_joinPollState == 7` (`0x01c82`, queued by host UART `0x5390`).
 * Builds 16-byte plaintext `[0]=0xE0, [1..8]=mac, [9]=e0ArgByte` and queues via
 * `Cllc_sendDataReq`.
 */
void Cllc_sendRfCmd_E0(uint16_t dstShortAddr, uint8_t e0Arg, const uint8_t mac[8],
                       uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    uint8_t pkt[16];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xE0u;
    memcpy(&pkt[1], mac, 8);
    pkt[9] = e0Arg;
    if (g_collectorCtx.rfTestMode != 0u) {
        g_gwCtx.pendingReplyCmd = 0xF2u;
        Uart_buildAndSendReply(pkt);
    }
    Cllc_sendDataReq(dstShortAddr, pkt, sizeof(pkt), apiMacDataReq);
}

/*
 * Cllc_sendRfCmd_D2()
 * Flash address: 0x00011f1c
 *
 * Called when `g_joinPollState == 8` (`0x01c74`, queued by host UART `0x5372`).
 * Builds 16-byte plaintext `[0]=0xD2, [1..8]=mac, [9..11]=d0ArgBytes[0..2]` and
 * queues via `Cllc_sendDataReq`.
 */
void Cllc_sendRfCmd_D2(uint16_t dstShortAddr, const uint8_t d2Args[3], const uint8_t mac[8],
                       uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    uint8_t pkt[16];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xD2u;
    memcpy(&pkt[1], mac, 8);
    memcpy(&pkt[9], d2Args, 3);
    if (g_collectorCtx.rfTestMode != 0u) {
        g_gwCtx.pendingReplyCmd = 0xF2u;
        Uart_buildAndSendReply(pkt);
    }
    Cllc_sendDataReq(dstShortAddr, pkt, sizeof(pkt), apiMacDataReq);
}

/*
 * Cllc_sendRfCmd_E2()
 * Flash address: 0x0001203c
 *
 * Called when `g_joinPollState == 9` (`0x01c64`, queued by host UART `0x5392`).
 * Builds 16-byte plaintext `[0]=0xE2, [1..8]=mac, [9]=e2ArgByte` and queues via
 * `Cllc_sendDataReq`.
 */
void Cllc_sendRfCmd_E2(uint16_t dstShortAddr, uint8_t e2Arg, const uint8_t mac[8],
                       uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    uint8_t pkt[16];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xE2u;
    memcpy(&pkt[1], mac, 8);
    pkt[9] = e2Arg;
    if (g_collectorCtx.rfTestMode != 0u) {
        g_gwCtx.pendingReplyCmd = 0xF2u;
        Uart_buildAndSendReply(pkt);
    }
    Cllc_sendDataReq(dstShortAddr, pkt, sizeof(pkt), apiMacDataReq);
}

/*
 * Cllc_sendRfCmd_E9()
 * Flash address: 0x00010d9c
 *
 * Called when `g_joinPollState == 10` (`0x01c54`, queued by host UART `0x5353`).
 * Builds 32-byte plaintext `[0]=0xE9, [1..8]=mac, [9]=0x0E, [10..]=e9PayloadBuf[0..N]`
 * (encrypted with 32-byte `AES_encryptBuffer` at `0x0d5a4`) and queues via `Cllc_sendDataReq`.
 */
void Cllc_sendRfCmd_E9(uint16_t dstShortAddr, const uint8_t *e9Payload, const uint8_t mac[8],
                       uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    uint8_t pkt[32];
    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xE9u;
    memcpy(&pkt[1], mac, 8);
    pkt[9] = 0x0Eu;
    uint8_t copyLen = (uint8_t)(e9Payload[0] + 1u);
    if (copyLen <= 20u) {
        memcpy(&pkt[10], e9Payload, copyLen);
    }
    Cllc_sendDataReq(dstShortAddr, pkt, sizeof(pkt), apiMacDataReq);
}

/*
 * Cllc_sendRfCmd_B3()
 * Flash address: 0x000128f0
 *
 * Called when `g_joinPollState == 11` (`0x01c34`, queued by host UART `0x535A`).
 * Builds 37-byte (`0x25`) unencrypted frame `[0]=0xB3, [1..36]=b3Payload[0..35]`
 * and queues via `Cllc_sendDataReq`.
 */
void Cllc_sendRfCmd_B3(uint16_t dstShortAddr, const uint8_t b3Payload[36],
                       uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    uint8_t pkt[37];
    pkt[0] = 0xB3u;
    memcpy(&pkt[1], b3Payload, 36);
    Cllc_sendDataReq(dstShortAddr, pkt, sizeof(pkt), apiMacDataReq);
}

/*
 * Cllc_sendRfCmd_B5()
 * Flash address: 0x00012324
 *
 * Called when `g_joinPollState == 12` (`0x01c1c`, queued by host UART `0x535C`).
 * Builds 131-byte (`0x83`) unencrypted frame `[0]=0xB5, [1..130]=b5Payload[0..129]`
 * and queues via `Cllc_sendDataReq`.
 */
void Cllc_sendRfCmd_B5(uint16_t dstShortAddr, const uint8_t b5Payload[130],
                       uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    uint8_t pkt[131];
    pkt[0] = 0xB5u;
    memcpy(&pkt[1], b5Payload, 130);
    Cllc_sendDataReq(dstShortAddr, pkt, sizeof(pkt), apiMacDataReq);
}

/* ==========================================================================
 * 5. Incoming RF Data Indication Handler (`dataIndCb`) (Unknowns D1, D5, D8, J4)
 * ========================================================================== */

/*
 * Collector_dataIndCb()
 * Flash addresses:
 *   - Collector_dataIndCb   : 0x00003ec8 .. 0x00004318
 *   - Uart_buildAndSendReply: 0x00003300 .. 0x00003a32
 *   - Collector_handleReport: 0x0000e7ec .. 0x0000e934
 *
 * Processes all incoming 802.15.4 data frames from sensors:
 *   1. IN-PLACE AES-128-ECB DECRYPTION (`0x03ee4..0x03ef4`):
 *      - Immediately calls `AES_decryptBuffer(pDataInd->msdu.p, pDataInd->msdu.len)`
 *        (`0x0924c`) using the static key at `0x200014e0` and copies the decrypted
 *        plaintext from `0x20001410` back over `pDataInd->msdu.p` in-place before
 *        inspecting `opcode = msdu.p[0]` (`0x03efa`).
 *   2. UNKNOWN SENSOR FILTERING (D8):
 *      - At `0x03efc..0x03f46`, checks if `srcShortAddr` is in `g_gwCtx.pairedShortAddrs[]`
 *        (`0x20001994`). If not found, allows only `opcode == 0xA3 || opcode == 0xA5`
 *        (`0x03f3a..0x03f40`) and drops all other frames immediately.
 *      - In `Uart_buildAndSendReply` (`0x035ca..0x035dc`), calls `Csf_findDeviceByMac(&pt[1])`
 *        (`0x10460`). If it returns `-1` (MAC not in NV item 5), **silently drops**
 *        the report without sending `0xB0` or forwarding `0x5319` to the host.
 *   3. THE DISPATCH (`0x03f4c..0x03fda`, re-checked against the binary 2026-11-06, #28): a compare / subtract
 *      chain on `opcode`, every case below and nothing else. v1.4 (`gateway_2025-02-02_v1.4`, `0x06c72`) has only
 *      the cases marked [v1.4]; the rest came with v1.8.
 *        0x02 [v1.4] (len 13) -> 0x040ea  TI Collector example's config response (Smsgs_cmdIds_configRsp)
 *        0x04 [v1.4] (len 1)  -> 0x040b0  TI Collector example's tracking response
 *        0x08 [v1.4]          -> 0x0430e  TI Collector example's ramp data: only counts ([r4, #0x17c]++)
 *        0xA1 [v1.4]          -> 0x0e7ec  report -> 0x5319 (Collector_handleReport)
 *        0xA2 [v1.4]          -> 0x07b10  event -> 0x5319
 *        0xA3 [v1.4]          -> 0x042b4  join announce -> 0x5320 (11 bytes, only while pairing)
 *        0xA5 [v1.4]          -> 0x0f114  join response -> 0x5322 (XXTEA not checked here)
 *        0xA9 [v1.4]          -> 0x09f5c  factory link-quality test reply
 *        0xAB [v1.4]          -> 0x1226c  v1 motion PIR summary
 *        0xAD [v1.4]          -> 0x0ad0c  v1 offline log record -> 0x5319 as an A2
 *        0xAE [v1.4]          -> 0x0428c  forwarded to the host as 0x5338
 *        0xAF [v1.4]          -> 0x12bc4  raw host packet 55 AA 53 0E 3B + pt[1..11] (template at 0x17104): 0x533B
 *        0xB2                 -> 0x0406e  -> 0x5359 (17 bytes + -RSSI)
 *        0xB4                 -> 0x03fdc  duplicate seq -> 0x535E (2-byte seq), else -> 0x535B
 *        0xB6                 -> 0x0425c  -> 0x535D (18 bytes + -RSSI)
 *        0xD1                 -> 0x041ee  a compact alarm (type, MAC, sensor type, pt[13..15]) -> 0x5319, MAC in NV only
 *        0xD3, 0xE1, 0xE3     -> 0x041a0  a compact alarm (type, MAC, sensor type, pt[12..14]) -> 0x5319, MAC in NV only
 *        0xE8                 -> 0x0424a  climate -> 0x5319 (as the captures show)
 *        0xEA                 -> 0x04166  -> 0x5355: plaintext pt[0 .. 10 + pt[10]] + -RSSI (keypad: PR #76)
 *      Not handled at all (an earlier reconstruction claimed them): 0xA7 (no `#0xa7` anywhere in the binary; the
 *      cited 0x0ef20 / 0x03504 are unrelated), 0xAA, 0xDF, 0xE6. (0xE8 -> 0x5355 and 0xEA -> 0x5354 were swapped
 *      readings: 0xEA is the one forwarded as 0x5355.)
 *   4. TIME-SYNC `0xB0` GATING (D1):
 *      - At `0x0e848..0x0e882`, queues `0xB0` after a sensor report ONLY when
 *        `sensorType != 0x0E && sensorType != 0x0F`.
 */
void Collector_dataIndCb(uint16_t srcShortAddr, int8_t rssi, const uint8_t *pt, uint8_t len,
                         uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    if (len < 16u) {
        return;
    }
    uint8_t opcode = pt[0];
    /* unknown short address: only A3 / A5 go on (0x03f2a..0x03f40) */
    switch (opcode) {
        case 0xA3u: /* 0x042b4 */
            g_gwCtx.pendingReplyCmd = 0x20u;
            Uart_buildAndSendReply(pt);
            return;
        case 0xA5u: /* 0x0f114 */
            g_gwCtx.pendingReplyCmd = 0x22u;
            Uart_buildAndSendReply(pt);
            return;
        case 0xEAu: { /* 0x04166: -> 0x5355, pt[0 .. 10 + pt[10]] then -RSSI */
            uint8_t out[40];
            uint8_t n = (uint8_t)(pt[10] + 11u);
            memcpy(out, pt, n);
            out[n] = (uint8_t)(-rssi);
            g_gwCtx.pendingReplyCmd = 0x55u;
            Uart_buildAndSendReply(out);
            return;
        }
        case 0xA1u: /* 0x0e7ec */
        case 0xA2u: /* 0x07b10 */
        case 0xABu: /* 0x1226c */
        case 0xE8u: /* 0x0424a */
            break;
        default: /* 0x02, 0x04, 0x08, 0xA9, 0xAD, 0xAE, 0xAF, 0xB2, 0xB4, 0xB6, 0xD1, 0xD3, 0xE1, 0xE3: see above */
            return;
    }

    /* the report path (D8: the MAC must be in NV item 5, 0x035ca -> Csf_findDeviceByMac at 0x10460) */
    int nvSlot = Csf_findDeviceByMac(&pt[1]);
    if (nvSlot < 0) {
        return;
    }
    uint8_t sensorType = pt[9];
    if (sensorType != 0x0Eu && sensorType != 0x0Fu) {
        /* 0x0e848: Queue 0xB0 time sync for all types EXCEPT 0x0E and 0x0F (D1) */
        Cllc_sendTimeSync_B0(srcShortAddr, apiMacDataReq);
    }

    uint8_t reportWithRssi[32];
    memset(reportWithRssi, 0, sizeof(reportWithRssi));
    uint8_t copyLen = (opcode == 0xABu) ? 22u : 16u;
    memcpy(reportWithRssi, pt, copyLen);
    reportWithRssi[copyLen] = (uint8_t)(-rssi);

    g_gwCtx.pendingReplyCmd = 0x19u;
    Uart_buildAndSendReply(reportWithRssi);
}

/* ==========================================================================
 * 6. UART Framing, Checksum, Ring Buffer & Reply Builder (`0x071e8`, `0x03300`)
 * ========================================================================== */

/*
 * Uart_computeFrameCsum()
 * Flash address: 0x00012d94
 *
 * Computes the 16-bit big-endian sum of all bytes in `g_gwCtx.uartTxBuf[0 .. txCsumOffset-1]`
 * and stores the high byte in `txCsumHi` (`0x200017dc`) and low byte in `txCsumLo` (`0x200017dd`).
 */
void Uart_computeFrameCsum(void)
{
    uint16_t sum = 0;
    for (uint8_t i = 0; i < g_gwCtx.txCsumOffset; i++) {
        sum = (uint16_t)(sum + g_gwCtx.uartTxBuf[i]);
    }
    g_gwCtx.txCsumHi = (uint8_t)(sum >> 8);
    g_gwCtx.txCsumLo = (uint8_t)(sum & 0xFFu);
}

/*
 * Uart_finalizeAndSend()
 * Flash address: 0x00012ad4
 *
 * Appends the 16-bit big-endian checksum to `g_gwCtx.uartTxBuf`, sends the frame
 * over UART0 (`UART_write`), saves a copy in `g_gwCtx.lastTxFrame` (`0x200018d5`)
 * for retransmission, and starts the 150 ms (`0x96`) retry timer (`0x14238`).
 */
void Uart_finalizeAndSend(void)
{
    g_gwCtx.txCsumOffset = (uint8_t)(g_gwCtx.txPayloadPlus2Len + 4u);
    g_gwCtx.txTotalLen = (uint8_t)(g_gwCtx.txPayloadPlus2Len + 6u);
    Uart_computeFrameCsum();

    g_gwCtx.uartTxBuf[g_gwCtx.txCsumOffset] = g_gwCtx.txCsumHi;
    g_gwCtx.uartTxBuf[g_gwCtx.txCsumOffset + 1u] = g_gwCtx.txCsumLo;

    if (g_gwCtx.txTotalLen <= sizeof(g_gwCtx.lastTxFrame)) {
        memcpy(g_gwCtx.lastTxFrame, g_gwCtx.uartTxBuf, g_gwCtx.txTotalLen);
        g_gwCtx.lastTxFrameLen = g_gwCtx.txTotalLen;
    }
    g_gwCtx.rxLenOrLastCmd = g_gwCtx.uartTxBuf[4];
}

/*
 * Uart_sendImmediate53() / Uart_send53()
 * Flash address: 0x0000e9cc
 *
 * Builds and immediately transmits a `55 AA 53 <b2> <cmd> <payload...> <csum_hi> <csum_lo>`
 * frame where `b2 = len + 3`.
 */
void Uart_send53(uint8_t cmd, const void *payload, uint8_t len)
{
    g_gwCtx.uartTxBuf[0] = 0x55u;
    g_gwCtx.uartTxBuf[1] = 0xAAu;
    g_gwCtx.uartTxBuf[2] = 0x53u;
    g_gwCtx.uartTxBuf[3] = (uint8_t)(len + 3u);
    g_gwCtx.uartTxBuf[4] = cmd;
    if (len > 0u && payload != NULL) {
        memcpy(&g_gwCtx.uartTxBuf[5], payload, len);
    }
    g_gwCtx.txPayloadPlus2Len = (uint8_t)(len + 2u);
    Uart_finalizeAndSend();
}

/*
 * Uart_sendTimeSyncReq_5332()
 * Flash address: 0x00013e90
 *
 * Sends `55 AA 53 03 32 01 86` (`0x5332`, `HOST_TIME_SYNC_REQ`) to ask the host
 * to reply with `0x5333` (`HOST_TIME_SYNC` containing the 8-byte ms epoch).
 * Called after `0x5315` (`FINISH_AUTH`) is ACKed (`0x07380`), after `0x5330 -> 0x5331`
 * (`GET_SENSOR_LIST`, `0x038e6`), and every 11 minutes (`Collector_process` `0x02070`).
 */
void Uart_sendTimeSyncReq_5332(void)
{
    g_gwCtx.pendingReplyCmd = 0x32u;
    Uart_buildAndSendReply(NULL);
}

/*
 * Uart_handleHistoryLog_AD()
 * Flash address: 0x00008e10
 *
 * Called by `Uart_buildAndSendReply` (`0x038d0`) when an offline history log record
 * (`opcode == 0xAD`) arrives from a sensor. Converts the sensor's 8-byte timestamp
 * into a 64-bit gateway millisecond epoch, rewrites the inner event opcode to `0xA2`,
 * and sends a 27-byte (`b2 = 0x1B`) `0x5319` frame to the host.
 */
void Uart_handleHistoryLog_AD(const uint8_t *pt)
{
    uint64_t recMs = 0;
    for (int i = 0; i < 8; i++) {
        recMs = (recMs << 8) | pt[10 + i];
    }
    uint64_t gwMs = (g_collectorCtx.gatewayMsClock >= recMs) ? (g_collectorCtx.gatewayMsClock - recMs) : 0u;
    if (gwMs == g_gwCtx.lastHistoryBaseMs) {
        g_gwCtx.sameTimestampCounter++;
        gwMs += g_gwCtx.sameTimestampCounter;
    } else {
        g_gwCtx.sameTimestampCounter = 0;
        g_gwCtx.lastHistoryBaseMs = gwMs;
    }

    g_gwCtx.uartTxBuf[0] = 0x55u;
    g_gwCtx.uartTxBuf[1] = 0xAAu;
    g_gwCtx.uartTxBuf[2] = 0x53u;
    g_gwCtx.uartTxBuf[3] = 0x1Bu;
    g_gwCtx.uartTxBuf[4] = 0x19u;
    for (int i = 0; i < 8; i++) {
        g_gwCtx.uartTxBuf[5 + i] = (uint8_t)(gwMs >> (56 - 8 * i));
    }
    g_gwCtx.uartTxBuf[13] = 0xA2u;
    memcpy(&g_gwCtx.uartTxBuf[14], &pt[1], 9); /* 8B MAC + sensorType */
    g_gwCtx.uartTxBuf[23] = 0x00u;
    g_gwCtx.uartTxBuf[24] = 0x00u;
    g_gwCtx.uartTxBuf[25] = 0x00u;
    g_gwCtx.uartTxBuf[26] = 0x01u;
    g_gwCtx.uartTxBuf[27] = pt[19]; /* state */
    g_gwCtx.uartTxBuf[28] = pt[20]; /* seq_hi */
    g_gwCtx.uartTxBuf[29] = pt[21]; /* seq_lo */
    g_gwCtx.uartTxBuf[30] = 0x00u;  /* rssi placeholder */
    g_gwCtx.txPayloadPlus2Len = 0x1Au;
    Uart_finalizeAndSend();
}

/*
 * Uart_rxRingRead()
 * Flash address: 0x000071e8 .. 0x000073d8
 *
 * State-machine parser for incoming host UART frames (`AA 55 53 <b2> <cmd> ...`):
 *   - State 0 (`0x07200`): Wait for `0xAA`
 *   - State 1 (`0x0721a`): Wait for `0x55`
 *   - State 2 (`0x07232`): Wait for `0x53` (`0x073a6`)
 *   - State 3 (`0x0724c`): Read `b2` (`rxLen`) and `b2 - 1` bytes of `[cmd, payload..., csum_hi, csum_lo]`
 *   - Checksum verification (`0x0727a..0x072b0`):
 *     - On mismatch (`0x07300`), sends `55 AA 53 03 5E 00 5F` (`0x535E` with 0-byte payload, D2)!
 *   - On valid checksum (`0x072b0`):
 *     - If `cmd == 0x00` (`0x07312`): Retransmits `g_gwCtx.lastTxFrame` (`0x200018d5`)!
 *     - If `cmd == 0xFF` (`0x0732a`, `HOST_ACK` `AA 55 53 03 FF <acked_cmd> ...`):
 *       - Stops the 150 ms TX retry clock (`0x14238(0)`) and resets `txRetryCount = 0`.
 *       - If `acked_cmd == 0x19`: Clears `g_gwCtx.unackedA2AlarmPayload`.
 *       - If `acked_cmd == 0x15` (`FINISH_AUTH` reply): Immediately calls `Uart_sendTimeSyncReq_5332()` (`0x13e90`)!
 *       - If `acked_cmd == 0x37`: Starts 1000 ms timer (`0x1419c(1000)`) to send next offline `0xA2` record!
 *     - Otherwise (`0x072ba`):
 *       - Sends immediate `HOST_ACK` `55 AA 53 03 FF <cmd> <csum_hi> <csum_lo>` (`0x072c4..0x072f8`)
 *       - Saves `[b2 - 2, cmd, payload...]` into `g_gwCtx.pendingCmdCopy` (`0x20001885`)
 *       - Posts semaphore `g_uartSem` (`0x200017f0`) so `Uart_processCommands()` (`0x02bc0`) executes!
 */
bool Uart_rxRingRead(const uint8_t *rawFrame, uint8_t rawLen)
{
    if (!rawFrame || rawLen < 7u) {
        return false;
    }
    if (rawFrame[0] != 0xAAu || rawFrame[1] != 0x55u || rawFrame[2] != 0x53u) {
        return false;
    }

    uint8_t b2 = rawFrame[3];
    if ((uint8_t)(b2 + 3u) > rawLen || b2 < 3u) {
        return false;
    }

    uint16_t calcSum = (uint16_t)(0xAAu + 0x55u + 0x53u + b2);
    uint8_t payloadPlusCmdLen = (uint8_t)(b2 - 2u);
    for (uint8_t i = 0; i < payloadPlusCmdLen; i++) {
        calcSum = (uint16_t)(calcSum + rawFrame[4u + i]);
    }
    uint16_t rxSum = (uint16_t)((rawFrame[4u + payloadPlusCmdLen] << 8) | rawFrame[5u + payloadPlusCmdLen]);
    if (calcSum != rxSum) {
        /* 0x0000730a: Checksum error -> send 0x535E with 0-byte payload (D2) */
        Uart_send53(0x5Eu, NULL, 0);
        return false;
    }

    uint8_t cmd = rawFrame[4];
    if (cmd == 0x00u) {
        /* 0x00007312: Host NACK / retransmit request -> resend g_gwCtx.lastTxFrame */
        return false;
    }
    if (cmd == 0xFFu) {
        /* 0x0000732a: Host ACK frame (rawFrame[5] = acked_cmd) */
        g_collectorCtx.txRetryCount = 0;
        uint8_t ackedCmd = rawFrame[5];
        if (ackedCmd == 0x19u) {
            memset(g_gwCtx.unackedA2AlarmPayload, 0, sizeof(g_gwCtx.unackedA2AlarmPayload));
        } else if (ackedCmd == 0x15u) {
            /* 0x00007380: After host ACKs 0x5315 (FINISH_AUTH), request host time sync (0x5332) */
            Uart_sendTimeSyncReq_5332();
        }
        return false;
    }

    /* 0x000072c4: Copy command + payload into g_gwCtx.uartRxBuf and g_gwCtx.pendingCmdCopy */
    memcpy(g_gwCtx.uartRxBuf, &rawFrame[4], payloadPlusCmdLen);
    g_gwCtx.rxLenOrLastCmd = b2;
    g_gwCtx.pendingCmdCopy[0] = (uint8_t)(payloadPlusCmdLen + 1u);
    memcpy(&g_gwCtx.pendingCmdCopy[1], &rawFrame[4], payloadPlusCmdLen);
    return true;
}

/*
 * Uart_buildAndSendReply()
 * Flash address: 0x00003300 .. 0x00003a32
 *
 * Central reply and async event frame builder, dispatching on `g_gwCtx.pendingReplyCmd`
 * (`0x200017ce`):
 *   - 0x03 (`0x039ba`): `GET_ENR` reply (`55 AA 53 13 03 <16B XXTEA(challenge, gwEnrXxteaKey)> <csum>`)
 *   - 0x05 (`0x039ac`): `GET_MAC` reply (`55 AA 53 0B 05 <8B gatewayMac> <csum>`)
 *   - 0x07 (`0x03996`): `GET_KEY` reply (`55 AA 53 13 07 <16B gwEnrXxteaKey> <csum>`)
 *   - 0x09 (`0x03966`): `WRITE_MAC` reply (`55 AA 53 04 09 FF <csum>`, sets NV item `0x11 = {0x00, 0x99}`)
 *   - 0x0B (`0x038c6`): `WRITE_KEY` reply (`55 AA 53 04 0B FF <csum>`)
 *   - 0x15 (`0x03950`): `FINISH_AUTH` reply (`55 AA 53 03 15 <csum>`)
 *   - 0x17 (`0x0392c`): `GET_VERSION` reply (`55 AA 53 1D 17 "0.0.0.47 V1.8 Gateway GW3U" <csum>`)
 *   - 0x18 (`0x03888`): `RF_TEST_RX` reply (`AA 55 53 0F 18 <12B data> <csum>`)
 *   - 0x19 (`0x0359e`): `ASYNC_EVENT` for RF `0xA1`, `0xA2`, `0xAB`, `0xAD` (as an A2), `0xE8`, and compact alarms for `0xD1`, `0xD3`, `0xE1`, `0xE3`
 *   - 0x1D (`0x038bc`): `SET_SCAN` reply (`55 AA 53 03 1D <csum>`)
 *   - 0x20 (`0x03542`): `SEND_ENROLL` (`0xA3` join req -> `55 AA 53 0F 20 <8B MAC> <type> <ver> <bat> 00 <csum>`)
 *   - 0x22 (`0x03512`): `VERIFY_R1` (`0xA5` join rsp -> `55 AA 53 1E 22 <8B MAC> <16B R1'> <2B rnd> FF <csum>`)
 *   - 0x24 (`0x038bc`): `VERIFY_SENSOR` reply (`55 AA 53 03 24 <csum>`)
 *   - 0x26 (`0x034d2`): `DELETE_SENSOR` reply (`55 AA 53 0C 26 <8B MAC> <0xFF|0x00> <csum>`)
 *   - 0x28 (`0x03922`): `INQUIRY` reply (`55 AA 53 04 28 01 <csum>`)
 *   - (no 0x29 / RF 0xA7: an earlier reading; the binary has no 0xA7 case)
 *   - 0x2B (`0x03918`): `RF_FACTORY_TEST` reply (`AA 55 53 03 2B <csum>`)
 *   - 0x2C (`0x03904`): `RF_FACTORY_DATA` (`AA 55 53 0D 2C <10B data> <csum>`)
 *   - 0x2F (`0x038ee`): `GET_SENSOR_COUNT` reply (`55 AA 53 04 2F <count> <csum>`)
 *   - 0x31 (`0x038e0`): `GET_SENSOR_LIST` (`Csf_sendSensorList_5331` + `Uart_sendTimeSyncReq_5332`)
 *   - 0x32 (`0x038bc`): `HOST_TIME_SYNC_REQ` (`55 AA 53 03 32 <csum>`)
 *   - 0x35 (`0x038d6`): `DUMP_DIAG_LOG` timer start (`0x14204(1000)`)
 *   - 0x3B (`0x0348e`): `PREPARE_DELETE_ALL` reply (`55 AA 53 04 3B <0xFF|0x00> <csum>`)
 *   - 0x3E (`0x03464`): `SET_INCLUSION_MODE` reply (`55 AA 53 04 3E <0xFF|0x00|0x77> <csum>`)
 *   - 0x40 (`0x038ca`): `DELETE_ALL` (`Csf_deleteAllAndReboot_5340`)
 *   - 0x42 (`0x03456`): `READ_GPIO_TEST` reply (`55 AA 53 04 42 <0x00|0xFF> <csum>`)
 *   - 0x54 (`0x038c6`): `SEND_RF_E9` completion reply (`55 AA 53 04 54 FF <csum>`)
 *   - 0x71 (`0x038bc`): `SEND_RF_D0` reply (`55 AA 53 03 71 <csum>`)
 *   - 0x73 (`0x038bc`): `SEND_RF_D2` reply (`55 AA 53 03 73 <csum>`)
 *   - 0x81 (`0x038c6`): `SEND_RF_C0` reply (`55 AA 53 04 81 FF <csum>`)
 *   - 0x91 (`0x038bc`): `SEND_RF_E0` reply (`55 AA 53 03 91 <csum>`)
 *   - 0x93 (`0x038bc`): `SEND_RF_E2` reply (`55 AA 53 03 93 <csum>`)
 *   - 0xD1 (`0x0340e`): `GET_PROVISIONING_FLAGS` reply (`55 AA 53 06 D1 <b0> <b1> <b2> <csum>`)
 *   - 0xF2 (`0x0389c`): `RF_CMD_ECHO` (`55 AA 53 12 F2 <15B RF cmd> <csum>`)
 */
void Uart_buildAndSendReply(const uint8_t *srcBuf)
{
    uint8_t replyCmd = g_gwCtx.pendingReplyCmd;
    g_gwCtx.lastReplyCmdCopy = replyCmd;

    switch (replyCmd) {
        case 0x03u: { /* 0x039ba: GET_ENR reply (16B XXTEA-19 with g_collectorCtx.gwEnrXxteaKey) */
            uint8_t block[16];
            if (srcBuf) {
                memcpy(block, srcBuf, 16);
            } else {
                memset(block, 0, 16);
            }
            Stock_xxtea19_encrypt(block, g_collectorCtx.gwEnrXxteaKey);
            Uart_send53(0x03u, block, 16);
            break;
        }

        case 0x05u: /* 0x039ac: GET_MAC reply (8B g_collectorCtx.gatewayMac) */
            Uart_send53(0x05u, g_collectorCtx.gatewayMac, 8);
            break;

        case 0x07u: /* 0x03996: GET_KEY reply (16B g_collectorCtx.gwEnrXxteaKey) */
            Uart_send53(0x07u, g_collectorCtx.gwEnrXxteaKey, 16);
            break;

        case 0x09u: { /* 0x03966: WRITE_MAC reply (writes {0x11, 0x00, 0x99} to NV item 0x11) */
            uint8_t nv11[3] = {0x11u, 0x00u, 0x99u};
            Csf_writeCustomNvItem(nv11, sizeof(nv11));
            uint8_t status = 0xFFu;
            Uart_send53(0x09u, &status, 1);
            break;
        }

        case 0x0Bu: /* 0x038c6: WRITE_KEY reply */
        case 0x54u: /* 0x038c6: SEND_RF_E9 completion reply */
        case 0x81u: /* 0x038c6: SEND_RF_C0 reply */
        {
            uint8_t status = 0xFFu;
            Uart_send53(replyCmd, &status, 1);
            break;
        }

        case 0x15u: /* 0x03950: FINISH_AUTH reply */
        case 0x1Du: /* 0x038bc: SET_SCAN reply */
        case 0x24u: /* 0x038bc: VERIFY_SENSOR reply */
        case 0x32u: /* 0x038bc: HOST_TIME_SYNC_REQ */
        case 0x71u: /* 0x038bc: SEND_RF_D0 reply */
        case 0x73u: /* 0x038bc: SEND_RF_D2 reply */
        case 0x91u: /* 0x038bc: SEND_RF_E0 reply */
        case 0x93u: /* 0x038bc: SEND_RF_E2 reply */
            Uart_send53(replyCmd, NULL, 0);
            break;

        case 0x17u: { /* 0x0392c: GET_VERSION reply ("0.0.0.47 V1.8 Gateway GW3U") */
            uint8_t verLen = (uint8_t)strlen(g_gwCtx.versionString);
            Uart_send53(0x17u, g_gwCtx.versionString, verLen);
            break;
        }

        case 0x18u: /* 0x03888: RF_TEST_RX reply (12B payload) */
            if (g_gwCtx.rfTestModeFlag == 1u && srcBuf != NULL) {
                Uart_send53(0x18u, srcBuf, 12);
            }
            break;

        case 0x19u: { /* 0x0359e: ASYNC_EVENT (sensor RF report -> 0x5319) */
            if (!srcBuf || Csf_findDeviceByMac(&srcBuf[1]) < 0) {
                break;
            }
            uint8_t rfOp = srcBuf[0];
            if (rfOp == 0xADu) {
                /* 0x038d0: Offline history log record -> Uart_handleHistoryLog_AD */
                Uart_handleHistoryLog_AD(srcBuf);
                break;
            }
            uint8_t out[32];
            for (int i = 0; i < 8; i++) {
                out[i] = (uint8_t)(g_collectorCtx.gatewayMsClock >> (56 - 8 * i));
            }
            if (rfOp == 0xA2u) {
                /* 0x03674: Deduplicate (mac, seqNum) and save in unackedA2AlarmPayload */
                uint16_t seq = (uint16_t)((srcBuf[15] << 8) | srcBuf[16]);
                if (memcmp(&srcBuf[1], g_gwCtx.lastA2Mac, 8) == 0 && seq == g_gwCtx.lastA2SeqNum) {
                    break;
                }
                memcpy(g_gwCtx.lastA2Mac, &srcBuf[1], 8);
                g_gwCtx.lastA2SeqNum = seq;
                g_gwCtx.lastA2GatewayMs = g_collectorCtx.gatewayMsClock;
                memcpy(&out[8], srcBuf, 17);
                Uart_send53(0x19u, out, 25);
                memcpy(g_gwCtx.unackedA2AlarmPayload, &g_gwCtx.uartTxBuf[5], 25);
            } else {
                /* the compare chain at 0x035f0 (re-checked 2026-11-06): plaintext bytes copied per type, then RSSI */
                uint8_t n = rfOp == 0xA1u   ? 17u
                            : rfOp == 0xABu ? 23u
                            : rfOp == 0xD1u ? 18u
                            : rfOp == 0xD3u ? 15u
                            : rfOp == 0xE1u ? 21u
                            : (rfOp == 0xE3u || rfOp == 0xE8u) ? 19u
                                                              : 0u;
                if (n) {
                    memcpy(&out[8], srcBuf, n);
                    Uart_send53(0x19u, out, (uint8_t)(8u + n));
                }
            }
            break;
        }

        case 0x20u: { /* 0x03542: SEND_ENROLL (0xA3 join request -> 12B payload) */
            if (g_collectorCtx.scanInclusionEnable == 0xFFu && g_collectorCtx.permitJoinActive == 1u && srcBuf) {
                uint8_t enroll[12];
                memcpy(enroll, &srcBuf[1], 11);
                enroll[11] = 0x00u;
                Uart_send53(0x20u, enroll, sizeof(enroll));
            }
            break;
        }

        case 0x22u: { /* 0x03512: VERIFY_R1 (0xA5 challenge response -> 27B payload) */
            if (g_collectorCtx.scanInclusionEnable == 0xFFu && g_collectorCtx.permitJoinActive == 1u && srcBuf) {
                uint8_t verify[27];
                memcpy(verify, &srcBuf[1], 26);
                verify[26] = 0xFFu;
                Uart_send53(0x22u, verify, sizeof(verify));
            }
            break;
        }

        case 0x26u: { /* 0x034d2: DELETE_SENSOR reply (<8B MAC> <0xFF if deleted, 0x00 if still present>) */
            uint8_t delRsp[9];
            memcpy(delRsp, g_gwCtx.cmdTargetMac, 8);
            delRsp[8] = (Csf_findDeviceByMac(g_gwCtx.cmdTargetMac) < 0) ? 0xFFu : 0x00u;
            Uart_send53(0x26u, delRsp, sizeof(delRsp));
            break;
        }

        case 0x28u: { /* 0x03922: INQUIRY reply (payload = 0x01) */
            uint8_t status = 0x01u;
            Uart_send53(0x28u, &status, 1);
            break;
        }

        case 0x2Bu: /* 0x03918: RF_FACTORY_TEST reply */
            Uart_send53(0x2Bu, NULL, 0);
            break;

        case 0x2Cu: /* 0x03904: RF_FACTORY_DATA (10B payload) */
            if (srcBuf) {
                Uart_send53(0x2Cu, srcBuf, 10);
            }
            break;

        case 0x2Fu: { /* 0x038ee: GET_SENSOR_COUNT reply (1B count) */
            uint8_t count = (uint8_t)Csf_getNumDeviceListEntries();
            Uart_send53(0x2Fu, &count, 1);
            break;
        }

        case 0x31u: /* 0x038e0: GET_SENSOR_LIST -> sends 0x5331 per sensor, then 0x5332 time-sync req */
            Csf_sendSensorList_5331();
            Uart_sendTimeSyncReq_5332();
            break;

        case 0x35u: /* 0x038d6: Starts 1000ms timer to dump g_gwCtx.diagLogBuf via 0x5335 */
            g_collectorCtx.collectorEvents |= 0x0100u;
            break;

        case 0x3Bu: { /* 0x0348e: PREPARE_DELETE_ALL reply */
            uint8_t status = (srcBuf && srcBuf[0] == 0xFFu) ? 0xFFu : 0x00u;
            Uart_send53(0x3Bu, &status, 1);
            break;
        }

        case 0x3Eu: { /* 0x03464: SET_INCLUSION_MODE reply (0xFF, 0x00, or 0x77 if unauthenticated) */
            uint8_t status = 0x77u;
            if (srcBuf) {
                if (srcBuf[0] == 0xFFu) {
                    status = 0xFFu;
                } else if (srcBuf[0] == 0x00u) {
                    status = 0x00u;
                }
            }
            Uart_send53(0x3Eu, &status, 1);
            break;
        }

        case 0x40u: /* 0x038ca: DELETE_ALL -> sends 0x5340, wipes RAM tables, and reboots CC1310 */
            Csf_deleteAllAndReboot_5340(NULL);
            break;

        case 0x42u: { /* 0x03456: READ_GPIO_TEST reply (GPIO_readDio(0xFF) -> 0x00 or 0xFF) */
            uint8_t dioStatus = 0xFFu;
            Uart_send53(0x42u, &dioStatus, 1);
            break;
        }

        case 0xD1u: { /* 0x0340e: GET_PROVISIONING_FLAGS reply (reads 3B from NV item 0x10) */
            uint8_t nv10[3] = {0, 0, 0};
            Csf_readCustomNvItem(0x10u, 3, nv10);
            uint8_t flags[3] = {
                (nv10[0] == 0x99u) ? 1u : 0u,
                (nv10[1] == 0x88u) ? 1u : 0u,
                (nv10[2] == 0x77u) ? 1u : 0u,
            };
            Uart_send53(0xD1u, flags, sizeof(flags));
            break;
        }

        case 0xF2u: /* 0x0389c: RF_CMD_ECHO (15B RF command echo when rfTestMode != 0) */
            if (srcBuf) {
                Uart_send53(0xF2u, srcBuf, 15);
            }
            break;

        default:
            break;
    }
}

/* ==========================================================================
 * 7. Complete Host -> Dongle UART Command Dispatcher (`0x02bc0 .. 0x032ea`)
 * ========================================================================== */

/*
 * Uart_processCommands()
 * Flash address: 0x00002bc0 .. 0x000032ea
 *
 * Complete command switch table for all 30 host commands (`cmd = g_gwCtx.uartRxBuf[0]`):
 *   - 0x02 (`0x032ba`): `GET_ENR` (16B challenge) -> XXTEA-19 with `gwEnrXxteaKey` -> `0x5303`
 *   - 0x04 (`0x032b6`): `GET_MAC` -> returns 8B `gatewayMac` (`0x20001348`) -> `0x5305`
 *   - 0x06 (`0x032b2`): `GET_KEY` -> returns 16B `gwEnrXxteaKey` (`0x20001374`) -> `0x5307`
 *   - 0x08 (`0x03276`): `WRITE_MAC` (8B MAC) -> writes NV item `0x20` & `0x11` -> `0x5309`
 *   - 0x0A (`0x0326e`): `WRITE_KEY` (16B ENR key) -> `Csf_writeGatewayEnrKey` (NV item `0x30`) -> `0x530B`
 *   - 0x14 (`0x0315e`): `FINISH_AUTH` (`0xFF`=ok, `0x00`=fail) -> sets `finishAuthDone`, LEDs -> `0x5315`
 *   - 0x16 (`0x03262`): `GET_VERSION` -> starts 300ms LED blink, returns `"0.0.0.47 V1.8 Gateway GW3U"` -> `0x5317`
 *   - 0x1C (`0x0312c`): `SET_SCAN` (`1`=enable permit-join `state=3`, `0`=disable `state=4`) -> `0x531D`
 *   - 0x21 (`0x030d0`): `GET_R1` (`[8B MAC][16B challenge]`, `b2-2 == 25`) -> queues RF `0xA4` (`state=1`)
 *   - 0x23 (`0x0306a`): `VERIFY_SENSOR` (`[8B MAC][2B status]`) -> queues RF `0xA6` (`state=2`) -> `0x5324`
 *   - 0x25 (`0x02fee`): `DELETE_SENSOR` (`[8B MAC]`) -> `Csf_removeDevice` -> `0x5326`
 *   - 0x27 (`0x0325e`): `INQUIRY` -> replies `0x5328` (`0x01`)
 *   - 0x2A (`0x03224`): `RF_FACTORY_TEST` (`[1B channel]`) -> disassociates all, sets `rfTestChannel` -> `0x532B`
 *   - 0x2D (`0x02fb8`): `FACTORY_FINISH` (`0xFF` writes `{0x10, 0x00, 0x88}` to NV item `0x10`)
 *   - 0x2E (`0x03220`): `GET_SENSOR_COUNT` -> replies `0x532F` (`1B count`)
 *   - 0x30 (`0x0321c`): `GET_SENSOR_LIST` -> sends `0x5331` per sensor + `0x5332` time-sync req
 *   - 0x33 (`0x02f4c`): `HOST_TIME_SYNC` (`[8B BE ms]`) -> sets `gatewayMsClock` (`0x20001340`), starts 10ms tick
 *   - 0x34 (`0x03218`): `DUMP_DIAG_LOG` -> triggers `0x5335` diagnostic log dump (D4)
 *   - 0x36 (`0x03214`): `DRAIN_OFFLINE_A2` -> triggers `0x5337` offline `0xA2` queue drain (D6)
 *   - 0x3A (`0x03206`): `PREPARE_DELETE_ALL` (`[9B arg]`) -> saves `deleteAllArg` (`0x20001820`)
 *   - 0x3C (`0x02f0a`): `SET_NV12_FLAG` (`1` writes `{0x12, 0x00, 0x77}`, `0` writes `{0x12, 0x00, 0x00}`)
 *   - 0x3D (`0x02eb2`): `SET_INCLUSION_MODE` (`0xFF`=on, `0x00`=off; requires `finishAuthDone == 1`) -> `0x533E`
 *   - 0x3F (`0x03202`): `DELETE_ALL` / Factory Reset -> replies `0x5340`, wipes RAM tables, reboots CC1310 (D3)
 *   - 0x41 (`0x031fe`): `READ_GPIO_TEST` -> reads `GPIO_readDio(0xFF)` -> replies `0x5342` (D3)
 *   - 0x53 (`0x02e80`): `SEND_RF_E9` (`[8B MAC][rsvd][len][payload...]`) -> queues RF `0xE9` (`state=10`)
 *   - 0x57 (`0x031c4`): `GET_RF_NOISE_DBM` -> reads noise floor (`0x11bbc`) -> replies `0x5358` (`4B BE`)
 *   - 0x5A (`0x02e58`): `SEND_RF_B3` (`[8B MAC][36B payload]`) -> queues RF `0xB3` (`state=11`)
 *   - 0x5C (`0x02e26`): `SEND_RF_B5` (`[8B MAC][130B payload]`) -> queues RF `0xB5` (`state=12`)
 *   - 0x70 (`0x02df0`): `SEND_RF_D0` (`[8B MAC][3B d0Args]`) -> queues RF `0xD0` (`state=6`) -> `0x5371`
 *   - 0x72 (`0x02dba`): `SEND_RF_D2` (`[8B MAC][3B d2Args]`) -> queues RF `0xD2` (`state=8`) -> `0x5373`
 *   - 0x80 (`0x02d8c`): `SEND_RF_C0` (`[8B MAC][2B rsvd][1B c0Arg]`) -> queues RF `0xC0` (`state=5`) -> `0x5381`
 *   - 0x90 (`0x02d5a`): `SEND_RF_E0` (`[8B MAC][1B e0Arg]`) -> queues RF `0xE0` (`state=7`) -> `0x5391`
 *   - 0x92 (`0x02d28`): `SEND_RF_E2` (`[8B MAC][1B e2Arg]`) -> queues RF `0xE2` (`state=9`) -> `0x5393`
 *   - 0xD0 (`0x031c0`): `GET_PROVISIONING_FLAGS` -> reads NV item `0x10` -> replies `0x53D1` (`3B flags`)
 *   - 0xE9 (`0x031ac`): `START_NOISE_MONITOR_50S` -> sets `chimeActiveFlag = 1`, starts 50s timer -> `0x53EA`
 */
void Uart_processCommands(uint8_t cmd, const uint8_t *payload, uint8_t len)
{
    switch (cmd) {
        case 0x02u: /* 0x032ba: GET_ENR -> 0x5303 (XXTEA-19 encrypt 16B challenge) */
            g_gwCtx.pendingReplyCmd = 0x03u;
            Uart_buildAndSendReply(payload);
            break;

        case 0x04u: /* 0x032b6: GET_MAC -> 0x5305 */
            g_gwCtx.pendingReplyCmd = 0x05u;
            Uart_buildAndSendReply(payload);
            break;

        case 0x06u: /* 0x032b2: GET_KEY -> 0x5307 */
            g_gwCtx.pendingReplyCmd = 0x07u;
            Uart_buildAndSendReply(payload);
            break;

        case 0x08u: { /* 0x03276: WRITE_MAC -> writes NV item 0x20, replies 0x5309 */
            uint8_t nv20[9];
            nv20[0] = 0x20u;
            memcpy(&nv20[1], payload, 8);
            memcpy(g_collectorCtx.gatewayMac, payload, 8);
            Csf_writeCustomNvItem(nv20, sizeof(nv20));
            g_gwCtx.pendingReplyCmd = 0x09u;
            Uart_buildAndSendReply(payload);
            break;
        }

        case 0x0Au: /* 0x0326e: WRITE_KEY -> writes 16B ENR key to NV item 0x30, replies 0x530B */
            Csf_writeGatewayEnrKey(payload);
            break;

        case 0x14u: /* 0x0315e: FINISH_AUTH -> sets finishAuthDone, replies 0x5315 */
            if (payload[0] == 0xFFu) {
                g_gwCtx.finishAuthDone = 1u;
                g_gwCtx.pendingReplyCmd = 0x15u;
                Uart_buildAndSendReply(payload);
            } else if (payload[0] == 0x00u) {
                g_gwCtx.finishAuthDone = 0u;
            }
            break;

        case 0x16u: /* 0x03262: GET_VERSION -> starts 300ms LED blink, replies 0x5317 */
            g_gwCtx.pendingReplyCmd = 0x17u;
            Uart_buildAndSendReply(payload);
            break;

        case 0x1Cu: /* 0x0312c: SET_SCAN -> sets g_joinPollState = 3 (on) or 4 (off), replies 0x531D */
            if (payload[0] == 1u) {
                g_collectorCtx.g_joinPollState = 3u;
                g_gwCtx.pendingReplyCmd = 0x1Du;
                Uart_buildAndSendReply(payload);
            } else if (payload[0] == 0u) {
                g_collectorCtx.g_joinPollState = 4u;
                g_gwCtx.pendingReplyCmd = 0x1Du;
                Uart_buildAndSendReply(payload);
            }
            break;

        case 0x21u: /* 0x030d0: GET_R1 ([8B MAC][16B challenge]) -> queues RF 0xA4 (g_joinPollState = 1) */
            if (len == 24u) {
                memcpy(g_gwCtx.cmdTargetMac, payload, 8);
                if (Csf_findDeviceByMac(g_gwCtx.cmdTargetMac) >= 0) {
                    g_gwCtx.targetShortAddr = Csf_getDeviceShortByMac(g_gwCtx.cmdTargetMac);
                    memcpy(g_gwCtx.joinChallenge, &payload[8], 16);
                    g_collectorCtx.g_joinPollState = 1u;
                }
            }
            break;

        case 0x23u: /* 0x0306a: VERIFY_SENSOR ([8B MAC][2B status]) -> queues RF 0xA6 (state = 2), replies 0x5324 */
            memcpy(g_gwCtx.joinVerifyRsp, &payload[8], 2);
            memcpy(g_gwCtx.cmdTargetMac, payload, 8);
            if (Csf_findDeviceByMac(g_gwCtx.cmdTargetMac) >= 0) {
                g_gwCtx.targetShortAddr = Csf_getDeviceShortByMac(g_gwCtx.cmdTargetMac);
                g_collectorCtx.g_joinPollState = 2u;
                if (g_gwCtx.joinVerifyRsp[0] == 0xFFu) {
                    g_gwCtx.joinApprovedFlag = 1u;
                    g_gwCtx.verifyTimeoutFlag = 1u;
                } else if (g_gwCtx.joinVerifyRsp[0] == 0x00u) {
                    g_gwCtx.verifyTimeoutFlag = 1u;
                    Csf_removeDevice(g_gwCtx.cmdTargetMac);
                }
                g_gwCtx.pendingReplyCmd = 0x24u;
                Uart_buildAndSendReply(payload);
            }
            break;

        case 0x25u: { /* 0x02fee: DELETE_SENSOR ([8B MAC]) -> Csf_removeDevice, replies 0x5326 */
            memcpy(g_gwCtx.cmdTargetMac, payload, 8);
            uint16_t shortAddr = Csf_getDeviceShortByMac(g_gwCtx.cmdTargetMac);
            if (shortAddr != 0xFFFFu) {
                for (int i = 0; i < 110; i++) {
                    if (g_gwCtx.pairedShortAddrs[i] == shortAddr) {
                        g_gwCtx.pairedShortAddrs[i] = 0xFFFFu;
                        break;
                    }
                }
                Csf_removeDevice(g_gwCtx.cmdTargetMac);
            }
            g_gwCtx.pendingReplyCmd = 0x26u;
            Uart_buildAndSendReply(payload);
            break;
        }

        case 0x27u: /* 0x0325e: INQUIRY -> replies 0x5328 (0x01) */
            g_gwCtx.pendingReplyCmd = 0x28u;
            Uart_buildAndSendReply(payload);
            break;

        case 0x2Au: /* 0x03224: RF_FACTORY_TEST ([1B channel]) -> replies 0x532B */
            g_collectorCtx.rfTestChannel = payload[0];
            g_collectorCtx.g_joinPollState = 3u;
            g_gwCtx.pendingReplyCmd = 0x2Bu;
            Uart_buildAndSendReply(payload);
            break;

        case 0x2Du: /* 0x02fb8: FACTORY_FINISH -> writes {0x10, 0x00, 0x88} to NV item 0x10 */
            if (payload[0] == 0xFFu) {
                uint8_t nv10[3] = {0x10u, 0x00u, 0x88u};
                Csf_writeCustomNvItem(nv10, sizeof(nv10));
            } else {
                g_collectorCtx.g_joinPollState = 4u;
            }
            break;

        case 0x2Eu: /* 0x03220: GET_SENSOR_COUNT -> replies 0x532F */
            g_gwCtx.pendingReplyCmd = 0x2Fu;
            Uart_buildAndSendReply(payload);
            break;

        case 0x30u: /* 0x0321c: GET_SENSOR_LIST -> sends 0x5331 per sensor + 0x5332 time-sync req */
            g_gwCtx.pendingReplyCmd = 0x31u;
            Uart_buildAndSendReply(payload);
            break;

        case 0x33u: { /* 0x02f4c: HOST_TIME_SYNC ([8B BE ms]) -> updates gatewayMsClock */
            memcpy(g_gwCtx.timeSyncPayload, payload, 8);
            uint64_t ms = 0;
            for (int i = 0; i < 8; i++) {
                ms = (ms << 8) | payload[i];
            }
            g_collectorCtx.gatewayMsClock = ms;
            break;
        }

        case 0x34u: /* 0x03218: DUMP_DIAG_LOG -> triggers 0x5335 diagnostic log dump (D4) */
            g_gwCtx.pendingReplyCmd = 0x35u;
            Uart_buildAndSendReply(payload);
            break;

        case 0x36u: /* 0x03214: DRAIN_OFFLINE_A2 -> triggers 0x5337 offline A2 queue drain (D6) */
            g_collectorCtx.collectorEvents |= 0x0200u;
            break;

        case 0x3Au: /* 0x03206: PREPARE_DELETE_ALL -> saves 9B argument in g_gwCtx.deleteAllArg */
            memcpy(g_gwCtx.deleteAllArg, payload, 9);
            break;

        case 0x3Cu: { /* 0x02f0a: SET_NV12_FLAG -> writes NV item 0x12 and disassociates all */
            if (payload[0] == 1u) {
                uint8_t nv12[3] = {0x12u, 0x00u, 0x77u};
                Csf_writeCustomNvItem(nv12, sizeof(nv12));
            } else if (payload[0] == 0u) {
                uint8_t nv12[3] = {0x12u, 0x00u, 0x00u};
                Csf_writeCustomNvItem(nv12, sizeof(nv12));
            }
            break;
        }

        case 0x3Du: { /* 0x02eb2: SET_INCLUSION_MODE -> requires finishAuthDone == 1, replies 0x533E */
            uint8_t statusByte = 0x77u;
            if (g_gwCtx.finishAuthDone == 1u) {
                statusByte = payload[0];
                if (statusByte == 0xFFu) {
                    uint8_t nv13[3] = {0x13u, 0x00u, 0x01u};
                    Csf_writeCustomNvItem(nv13, sizeof(nv13));
                    g_collectorCtx.scanInclusionEnable = 0xFFu;
                } else if (statusByte == 0x00u) {
                    uint8_t nv13[3] = {0x13u, 0x00u, 0x00u};
                    Csf_writeCustomNvItem(nv13, sizeof(nv13));
                    g_collectorCtx.scanInclusionEnable = 0x00u;
                }
            }
            g_gwCtx.pendingReplyCmd = 0x3Eu;
            Uart_buildAndSendReply(&statusByte);
            break;
        }

        case 0x3Fu: /* 0x03202: DELETE_ALL / Factory Reset -> replies 0x5340 and reboots (D3) */
            g_gwCtx.pendingReplyCmd = 0x40u;
            Uart_buildAndSendReply(payload);
            break;

        case 0x41u: /* 0x031fe: READ_GPIO_TEST -> replies 0x5342 (D3) */
            g_gwCtx.pendingReplyCmd = 0x42u;
            Uart_buildAndSendReply(payload);
            break;

        case 0x53u: /* 0x02e80: SEND_RF_E9 ([8B MAC][rsvd][len][payload...]) -> queues RF 0xE9 (state = 10) */
            memcpy(g_gwCtx.cmdTargetMac, payload, 8);
            if (Csf_findDeviceByMac(g_gwCtx.cmdTargetMac) >= 0) {
                g_collectorCtx.g_joinPollState = 10u;
                g_gwCtx.targetShortAddr = Csf_getDeviceShortByMac(g_gwCtx.cmdTargetMac);
                uint8_t copyLen = (uint8_t)(payload[9] + 1u);
                if (copyLen <= sizeof(g_gwCtx.e9PayloadBuf)) {
                    memcpy(g_gwCtx.e9PayloadBuf, &payload[9], copyLen);
                }
            }
            break;

        case 0x57u: { /* 0x031c4: GET_RF_NOISE_DBM -> sends 0x5358 with 4B big-endian value */
            uint8_t noiseBe[4] = {0, 0, 0, 0};
            Uart_send53(0x58u, noiseBe, sizeof(noiseBe));
            break;
        }

        case 0x5Au: /* 0x02e58: SEND_RF_B3 ([8B MAC][36B payload]) -> queues RF 0xB3 (state = 11) */
            memcpy(g_gwCtx.cmdTargetMac, payload, 8);
            if (Csf_findDeviceByMac(g_gwCtx.cmdTargetMac) >= 0) {
                g_collectorCtx.g_joinPollState = 11u;
                g_gwCtx.targetShortAddr = Csf_getDeviceShortByMac(g_gwCtx.cmdTargetMac);
                memcpy(g_gwCtx.b3b5PayloadBuf, &payload[8], 36);
            }
            break;

        case 0x5Cu: /* 0x02e26: SEND_RF_B5 ([8B MAC][130B payload]) -> queues RF 0xB5 (state = 12) */
            memcpy(g_gwCtx.cmdTargetMac, payload, 8);
            if (Csf_findDeviceByMac(g_gwCtx.cmdTargetMac) >= 0) {
                g_collectorCtx.g_joinPollState = 12u;
                g_gwCtx.targetShortAddr = Csf_getDeviceShortByMac(g_gwCtx.cmdTargetMac);
                memcpy(g_gwCtx.b3b5PayloadBuf, &payload[8], 130);
            }
            break;

        case 0x70u: /* 0x02df0: SEND_RF_D0 ([8B MAC][3B d0Args]) -> queues RF 0xD0 (state = 6), replies 0x5371 */
            memcpy(g_gwCtx.cmdTargetMac, payload, 8);
            if (Csf_findDeviceByMac(g_gwCtx.cmdTargetMac) >= 0) {
                memcpy(g_collectorCtx.d0ArgBytes, &payload[8], 3);
                g_gwCtx.targetShortAddr = Csf_getDeviceShortByMac(g_gwCtx.cmdTargetMac);
                g_collectorCtx.g_joinPollState = 6u;
                g_gwCtx.pendingReplyCmd = 0x71u;
                Uart_buildAndSendReply(payload);
            }
            break;

        case 0x72u: /* 0x02dba: SEND_RF_D2 ([8B MAC][3B d2Args]) -> queues RF 0xD2 (state = 8), replies 0x5373 */
            memcpy(g_gwCtx.cmdTargetMac, payload, 8);
            if (Csf_findDeviceByMac(g_gwCtx.cmdTargetMac) >= 0) {
                memcpy(g_collectorCtx.d0ArgBytes, &payload[8], 3);
                g_gwCtx.targetShortAddr = Csf_getDeviceShortByMac(g_gwCtx.cmdTargetMac);
                g_collectorCtx.g_joinPollState = 8u;
                g_gwCtx.pendingReplyCmd = 0x73u;
                Uart_buildAndSendReply(payload);
            }
            break;

        case 0x80u: /* 0x02d8c: SEND_RF_C0 ([8B MAC][2B rsvd][1B c0Arg]) -> queues RF 0xC0 (state = 5), replies 0x5381 */
            memcpy(g_gwCtx.cmdTargetMac, payload, 8);
            if (Csf_findDeviceByMac(g_gwCtx.cmdTargetMac) >= 0) {
                g_collectorCtx.c0ArgByte = payload[10];
                g_gwCtx.targetShortAddr = Csf_getDeviceShortByMac(g_gwCtx.cmdTargetMac);
                g_collectorCtx.g_joinPollState = 5u;
                g_gwCtx.pendingReplyCmd = 0x81u;
                Uart_buildAndSendReply(payload);
            }
            break;

        case 0x90u: /* 0x02d5a: SEND_RF_E0 ([8B MAC][1B e0Arg]) -> queues RF 0xE0 (state = 7), replies 0x5391 */
            memcpy(g_gwCtx.cmdTargetMac, payload, 8);
            if (Csf_findDeviceByMac(g_gwCtx.cmdTargetMac) >= 0) {
                g_collectorCtx.e0ArgByte = payload[8];
                g_gwCtx.targetShortAddr = Csf_getDeviceShortByMac(g_gwCtx.cmdTargetMac);
                g_collectorCtx.g_joinPollState = 7u;
                g_gwCtx.pendingReplyCmd = 0x91u;
                Uart_buildAndSendReply(payload);
            }
            break;

        case 0x92u: /* 0x02d28: SEND_RF_E2 ([8B MAC][1B e2Arg]) -> queues RF 0xE2 (state = 9), replies 0x5393 */
            memcpy(g_gwCtx.cmdTargetMac, payload, 8);
            if (Csf_findDeviceByMac(g_gwCtx.cmdTargetMac) >= 0) {
                g_collectorCtx.e2ArgByte = payload[8];
                g_gwCtx.targetShortAddr = Csf_getDeviceShortByMac(g_gwCtx.cmdTargetMac);
                g_collectorCtx.g_joinPollState = 9u;
                g_gwCtx.pendingReplyCmd = 0x93u;
                Uart_buildAndSendReply(payload);
            }
            break;

        case 0xD0u: /* 0x031c0: GET_PROVISIONING_FLAGS -> reads NV item 0x10, replies 0x53D1 */
            g_gwCtx.pendingReplyCmd = 0xD1u;
            Uart_buildAndSendReply(payload);
            break;

        case 0xE9u: /* 0x031ac: START_NOISE_MONITOR_50S -> sets chimeActiveFlag = 1, replies 0x53EA (D2) */
            g_gwCtx.chimeActiveFlag = 1u;
            Uart_send53(0xEAu, NULL, 0);
            break;

        default:
            break;
    }
}

/* ==========================================================================
 * 8. Collector Main Event Loop (`Collector_process`, `0x00001b48 .. 0x00002120`)
 * ========================================================================== */

/*
 * Collector_process()
 * Flash address: 0x00001b48 .. 0x00002120
 *
 * Processes asynchronous events in `g_collectorCtx.collectorEvents` (`0x2000130e`):
 *   - Event `0x0001` (`0x01b52`): Starts coordinator network (`Cllc_startNetwork`)
 *   - Event `0x0010` (`0x01b92`): Triggered when a sensor polls (`pollIndCb` at `0x0ea48`).
 *     Dispatches `g_collectorCtx.g_joinPollState` (`1..12`) via `TBB` table at `0x01bae`:
 *       - State 1  (`0x01cda`): `Cllc_sendChallenge_A4` (`0x0f878`)
 *       - State 2  (`0x01cc8`): `Cllc_sendVerifyRsp_A6` (`0x0c608`)
 *       - State 3  (`0x01c02`): `ApiMac_mlmeSetFhReqUint8(0x41, 1)` (permit join ON for 60s)
 *       - State 4  (`0x01cae`): `ApiMac_mlmeSetFhReqUint8(0x41, 0)` (permit join OFF)
 *       - State 5  (`0x01ca2`): `Cllc_sendRfCmd_C0` (`0x11f7c`)
 *       - State 6  (`0x01c92`): `Cllc_sendRfCmd_D0` (`0x11650`)
 *       - State 7  (`0x01c82`): `Cllc_sendRfCmd_E0` (`0x11fdc`)
 *       - State 8  (`0x01c74`): `Cllc_sendRfCmd_D2` (`0x11f1c`)
 *       - State 9  (`0x01c64`): `Cllc_sendRfCmd_E2` (`0x1203c`)
 *       - State 10 (`0x01c54`): `Cllc_sendRfCmd_E9` (`0x10d9c`)
 *       - State 11 (`0x01c34`): `Cllc_sendRfCmd_B3` (`0x128f0`)
 *       - State 12 (`0x01c1c`): `Cllc_sendRfCmd_B5` (`0x12324`)
 *   - Event `0x0020` (`0x01d2e`): 300 ms LED blink toggle (`0x0f98c`)
 *   - Event `0x0040` (`0x01eca`): 1000 ms un-ACKed `0x5319` timeout: pushes `unackedA2AlarmPayload`
 *     into the 30-entry (660-byte) `offlineA2Ring` (`0x20001b98`)!
 *   - Event `0x0080` (`0x01e42`): 10 ms clock tick: increments `g_collectorCtx.gatewayMsClock += 10`!
 *   - Event `0x0100` (`0x01fc2`): Sends next chunk of `diagLogBuf` (`0x20001867`) via `0x5335` (D4)
 *   - Event `0x0200` (`0x01f42`): Drains next 22-byte offline `0xA2` record from `0x20001b98` via `0x5337` (D6)
 *   - Event `0x0400` (`0x02060`): 60-second tick: after 11 ticks (11 minutes), calls `Uart_sendTimeSyncReq_5332()`
 *   - Event `0x0800` (`0x0209e`): 150 ms UART TX retry timer: resends `lastTxFrame` up to 3 additional times (4 total)
 */
void Collector_process(uint8_t (*apiMacDataReq)(uint16_t, const uint8_t *, uint8_t))
{
    uint16_t ev = g_collectorCtx.collectorEvents;

    /* Event 0x0010 (0x01b92): Sensor poll / downlink state machine (1..12) */
    if (ev & 0x0010u) {
        switch (g_collectorCtx.g_joinPollState) {
            case 1u:
                Cllc_sendChallenge_A4(g_gwCtx.targetShortAddr, g_gwCtx.joinChallenge, apiMacDataReq);
                break;
            case 2u:
                Cllc_sendVerifyRsp_A6(g_gwCtx.targetShortAddr, g_gwCtx.joinVerifyRsp, apiMacDataReq);
                break;
            case 3u:
                g_collectorCtx.permitJoinActive = 1u;
                break;
            case 4u:
                g_collectorCtx.permitJoinActive = 0u;
                break;
            case 5u:
                Cllc_sendRfCmd_C0(g_gwCtx.targetShortAddr, 1u, g_gwCtx.cmdTargetMac, apiMacDataReq);
                break;
            case 6u:
                Cllc_sendRfCmd_D0(g_gwCtx.targetShortAddr, g_collectorCtx.d0ArgBytes, g_gwCtx.cmdTargetMac,
                                  apiMacDataReq);
                break;
            case 7u:
                Cllc_sendRfCmd_E0(g_gwCtx.targetShortAddr, g_collectorCtx.e0ArgByte, g_gwCtx.cmdTargetMac,
                                  apiMacDataReq);
                break;
            case 8u:
                Cllc_sendRfCmd_D2(g_gwCtx.targetShortAddr, g_collectorCtx.d0ArgBytes, g_gwCtx.cmdTargetMac,
                                  apiMacDataReq);
                break;
            case 9u:
                Cllc_sendRfCmd_E2(g_gwCtx.targetShortAddr, g_collectorCtx.e2ArgByte, g_gwCtx.cmdTargetMac,
                                  apiMacDataReq);
                break;
            case 10u:
                Cllc_sendRfCmd_E9(g_gwCtx.targetShortAddr, g_gwCtx.e9PayloadBuf, g_gwCtx.cmdTargetMac, apiMacDataReq);
                break;
            case 11u:
                Cllc_sendRfCmd_B3(g_gwCtx.targetShortAddr, g_gwCtx.b3b5PayloadBuf, apiMacDataReq);
                break;
            case 12u:
                Cllc_sendRfCmd_B5(g_gwCtx.targetShortAddr, g_gwCtx.b3b5PayloadBuf, apiMacDataReq);
                break;
            default:
                break;
        }
        g_collectorCtx.g_joinPollState = 0u;
        g_collectorCtx.collectorEvents &= (uint16_t)~0x0010u;
    }

    /* Event 0x0040 (0x01eca): Un-ACKed 0x5319 A2 alarm -> push 22B record into offlineA2Ring (0x20001b98) */
    if (ev & 0x0040u) {
        if (g_gwCtx.unackedA2AlarmPayload[8] == 0xA2u && g_collectorCtx.offlineA2Count < 30u) {
            uint16_t ofs = g_collectorCtx.offlineA2WriteOfs;
            if (ofs + 22u <= sizeof(g_gwCtx.offlineA2Ring)) {
                uint8_t *dst = &g_gwCtx.offlineA2Ring[ofs];
                dst[0] = 0xA2u;
                memcpy(&dst[1], &g_gwCtx.unackedA2AlarmPayload[9], 9);  /* 8B MAC + sensorType */
                memcpy(&dst[10], &g_gwCtx.unackedA2AlarmPayload[0], 8); /* 8B BE timestamp */
                dst[18] = 0xA2u;
                memcpy(&dst[19], &g_gwCtx.unackedA2AlarmPayload[22], 3); /* state + 2B seq */
                g_collectorCtx.offlineA2WriteOfs = (uint16_t)(ofs + 22u);
                g_collectorCtx.offlineA2Count++;
            }
            memset(g_gwCtx.unackedA2AlarmPayload, 0, sizeof(g_gwCtx.unackedA2AlarmPayload));
        }
        g_collectorCtx.collectorEvents &= (uint16_t)~0x0040u;
    }

    /* Event 0x0080 (0x01e42): 10 ms clock tick -> advance 64-bit gatewayMsClock by +10 ms */
    if (ev & 0x0080u) {
        g_collectorCtx.gatewayMsClock += 10u;
        g_collectorCtx.collectorEvents &= (uint16_t)~0x0080u;
    }

    /* Event 0x0100 (0x01fc2): Diagnostic log dump via 0x5335 (D4) */
    if (ev & 0x0100u) {
        if (g_collectorCtx.diagLogDumpCount > 0u) {
            uint8_t chunkLen = g_gwCtx.diagLogBuf[g_gwCtx.diagLogReadOfs];
            if (chunkLen > 0u && (uint16_t)(g_gwCtx.diagLogReadOfs + 1u + chunkLen) <= sizeof(g_gwCtx.diagLogBuf)) {
                Uart_send53(0x35u, &g_gwCtx.diagLogBuf[g_gwCtx.diagLogReadOfs + 1u], chunkLen);
                g_gwCtx.diagLogReadOfs = (uint8_t)(g_gwCtx.diagLogReadOfs + 1u + chunkLen);
            }
            g_collectorCtx.diagLogDumpCount--;
        }
        g_collectorCtx.collectorEvents &= (uint16_t)~0x0100u;
    }

    /* Event 0x0200 (0x01f42): Offline 0xA2 queue drain via 0x5337 (22-byte records, D6) */
    if (ev & 0x0200u) {
        if (g_collectorCtx.offlineA2Count > 0u && g_gwCtx.offlineA2ReadOfs <= 0x027Eu) {
            Uart_send53(0x37u, &g_gwCtx.offlineA2Ring[g_gwCtx.offlineA2ReadOfs], 22);
            g_gwCtx.offlineA2ReadOfs = (uint16_t)(g_gwCtx.offlineA2ReadOfs + 22u);
            g_collectorCtx.offlineA2Count--;
        } else {
            g_collectorCtx.offlineA2Count = 0;
            g_collectorCtx.offlineA2WriteOfs = 0;
            g_gwCtx.offlineA2ReadOfs = 0;
        }
        g_collectorCtx.collectorEvents &= (uint16_t)~0x0200u;
    }

    /* Event 0x0400 (0x02060): 60s tick -> request host time sync (0x5332) every 11 minutes */
    if (ev & 0x0400u) {
        g_collectorCtx.timeSyncMinuteTicks++;
        if (g_collectorCtx.timeSyncMinuteTicks >= 11u) {
            Uart_sendTimeSyncReq_5332();
            g_collectorCtx.timeSyncMinuteTicks = 0;
        }
        g_collectorCtx.collectorEvents &= (uint16_t)~0x0400u;
    }

    /* Event 0x0800 (0x0209e): 150 ms un-ACKed UART TX retry timer (up to 3 retries = 4 total sends) */
    if (ev & 0x0800u) {
        if (g_collectorCtx.txRetryCount < 3u) {
            g_collectorCtx.txRetryCount++;
        } else {
            g_collectorCtx.txRetryCount = 0;
        }
        g_collectorCtx.collectorEvents &= (uint16_t)~0x0800u;
    }
}
