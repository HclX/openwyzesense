/*
 * Reconstructed C source code of the stock Wyze Sense v1 Sensor application
 * (`contact_v1` and `motion_v1`), built on TI's SimpleLink CC13x0 SDK
 * 15.4-Stack (`ti154stack` sensor example).
 *
 * Every function and global variable is annotated with its exact flash or SRAM
 * address in both ground-truth binaries:
 *   - contact_v1 : ground_truth/firmware/contact_v1_777B42DF_2026-10-24/flash.bin
 *   - motion_v1  : ground_truth/firmware/motion_v1_777A9A46/dump.bin
 */

#include "stock_common.h"

/* ==========================================================================
 * 1. Global Data Structures & SRAM Variable Map
 * ========================================================================== */

/*
 * Board GPIO PIN_Config Table
 * Flash / SRAM addresses:
 *   - contact_v1:
 *       * Flash 0x0001532c (`BoardGpioInitTable`):
 *           DIO9  = LED output (active high, initially low)
 *           DIO1  = UART RX pad (input, pull-up)
 *           DIO0  = UART TX pad (input, pull-up at boot before UART opens)
 *           DIO10 = Factory test 'W' pad input (`0xC000200A`: pull-up, hysteresis)
 *       * SRAM 0x20002258 (LZSS-compressed `.data` at flash `0x000153f8`, opened in `Sensor_init` at `0x02950`):
 *           DIO7  = Reed switch input (`0x80000007`)
 *           DIO11 = Pairing button input (`0x8005000B`)
 *   - motion_v1 :
 *       * Flash 0x000156b0 (`BoardGpioInitTable`):
 *           DIO12 = LED output (active high, initially low)
 *           DIO1  = UART RX pad (input, pull-up)
 *           DIO0  = UART TX pad (input, pull-up)
 *           DIO2  = Factory test 'W' pad input (`0x80002002`: pull-up)
 *       * PIR primary input = DIO9 / DIO10; pairing button input = DIO13
 */
enum StockSensorPins {
    CONTACT_PIN_REED = 7,
    CONTACT_PIN_LED = 9,
    CONTACT_PIN_FACTEST_W = 10,
    CONTACT_PIN_BUTTON = 11,

    MOTION_PIN_FACTEST_W = 2,
    MOTION_PIN_PIR = 9,
    MOTION_PIN_PIR_ALT = 10,
    MOTION_PIN_LED = 12,
    MOTION_PIN_BUTTON = 13,
};

/*
 * Sensor Core Control Block (`g_sensorCtx`)
 * SRAM addresses:
 *   - contact_v1: 0x20000a74 / 0x200012c0 / 0x20001748
 *   - motion_v1 : 0x20000ed8 / 0x20001728
 */
typedef struct {
    uint16_t sensorEvents;            /* +0x00 (RAM 0x20001754 / 0x2000173c): Sensor_events bitmask */
    uint8_t reedHighCount;            /* +0x02 (contact_v1 0x20001748+0x00): Consecutive DIO7==1 debounce samples */
    uint8_t reedLowCount;             /* +0x03 (contact_v1 0x20001748+0x01): Consecutive DIO7==0 debounce samples */
    uint8_t historyTiebreaker;        /* +0x04 (0x20001748+0x03): +1 ms offset when consecutive 0xAD timestamps tie */
    uint8_t historyEmptyFlag;         /* +0x07 (0x20001748+0x07 / 0x20001728+0x06): 1 when 0xAD log is empty */
    uint8_t txFailRetryStage;         /* +0x08 (0x20001748+0x08 / 0x20001728+0x07): 0=initial, 1=80ms, 2=130ms */
    uint8_t buttonAction;             /* +0x09 : 0=none, 1=long_press_3s, 4=short_press, 5=stuck_8s */
    uint8_t pirPulseCount10s;         /* +0x0a (motion_v1 0x20001728+0x08): PIR pulses in current 10s window */
    uint8_t gpioSelfTestTriggerCount; /* +0x0b (motion_v1 0x20001728+0x09): Pulses during 20s 0x53 0xD2 self-test */
    uint8_t pir10sTimerRestartFlag;   /* +0x0c (motion_v1 0x20001728+0x0b): 1 to start 10s bucket timer */
    uint8_t pir120sTimerRestartFlag;  /* +0x0d (motion_v1 0x20001728+0x0c): 1 to start 120s clear timer */
    uint8_t pirMotionIdleArmed;  /* +0x0e (motion_v1 0x20001728+0x0d): 1 when idle (send 0xA2 state=1 on next PIR) */
    uint8_t gpioSelfTestActive;  /* +0x0f (motion_v1 0x20001728+0x10): 1 during 20s 0x53 0xD2 self-test */
    uint8_t pirLedTestArm;       /* +0x10 : (motion_v1) 1 = blink LED on next PIR trigger */
    uint8_t lastSentReedState;   /* +0x11 (contact_v1 0x20000a82): Last debounced reed state sent via 0xA2 */
    uint8_t unackedReportCount;  /* +0x12 : Consecutive reports without 0xB0 ACK */
    uint8_t factoryTestActive;   /* +0x13 (0x20000a74): 1 when W pad (DIO10/DIO2) was low at boot */
    uint8_t txInProgress;        /* +0x14 : 1 while waiting for MAC data confirm / poll */
    uint8_t pollRetriesLeft;     /* +0x15 (0x20000a7a / 0x20000eda): Remaining DataReq polls (10 contact / 5 motion) */
    uint8_t historyCount;        /* +0x16 (0x20000a89 / 0x20000ee7): Queued 12B entries in g_historyLog (0..50) */
    uint16_t historyWriteOffset; /* +0x18 (0x20001756 / 0x20001740): Byte write offset in g_historyLog (0..588) */
    uint16_t historyReadOffset;  /* +0x1a (0x20001758 / 0x20001742): Byte read offset in g_historyLog (0..588) */
    uint8_t a3RetriesLeft;       /* +0x1c : Remaining 0xA3 join broadcast attempts (init 8) */
    uint8_t a5RetriesLeft;       /* +0x1d (0x20000a9c / 0x20000efa): Remaining 0xA5/0xA3 attempts (init 8) */
    uint8_t joinState;           /* +0x1e (0x20000a80 / 0x20000ee0): 0=sending_A3, 1=sending_A5 */
    uint8_t joinedNvFlag;        /* +0x1f (0x20000a7f / 0x20000edf): Cached NV item (6, 9, 0) joined state */
    uint8_t nv8KeyValidFlag;     /* +0x20 : Cached NV item (6, 8, 0) byte 24 */
    uint8_t nv10FacTestFlag;     /* +0x21 : Cached NV item (6, 10, 0) factory test flag */
    uint8_t nv11LowBatLatch;     /* +0x22 : Cached NV item (6, 11, 0) low-battery/decomm latch */
    uint8_t nv12Flag;            /* +0x23 (motion_v1 0x20000ee6): Cached NV item (6, 12, 0) flag */
    uint8_t lowBatBootFlag;      /* +0x24 : 1 if boot battery voltage <= 0x53 (2.59 V) */
    uint8_t joinCyclesRemaining; /* +0x25 (0x20000a76 / 0x20000eda): Remaining join scan cycles (init 30) */
    uint8_t ledToggleEdgesLeft;  /* +0x26 (0x20000a93 / 0x20000ef1): Remaining LED toggle edges */
    uint8_t lowBatTxStreak;      /* +0x27 : Consecutive post-TX battery readings < 0x54 (2.625 V) */
    uint16_t heartbeat5MinTicks; /* +0x28 (0x20000a96 / 0x20000ef4): 5-min ticks toward 48 (4h 0xA1) */
    uint8_t currentReedOrPirState;   /* +0x2a (0x20000a81): Latest state byte (0 or 1) */
    uint8_t rfTestChannel;           /* +0x2b (0x20000a75 / 0x20000ed9): Channel selected by 0x53 0x2A */
    uint8_t rfTestModeActive;        /* +0x2c (0x20000a7d / 0x20000edd): 1 during 50-pkt 0xAA/0xA9 PER test */
    uint8_t rfTestRxTotal;           /* +0x2d (0x20001748+0x00): Total 0xAA packets received (0..50) */
    uint8_t rfTestRxGood;            /* +0x2e (0x20001748+0x01): 0xAA packets with valid checksum */
    uint8_t rfTestRxBad;             /* +0x2f (0x20001748+0x02): 0xAA packets with bad checksum */
    int8_t rfTestLastRxRssi;         /* +0x30 (0x20000a8c / 0x20000ee8): RSSI of latest 0xAA packet */
    int32_t rfTestRssiSum;           /* +0x34 (0x20001748+0x20): Accumulated RSSI over 50 0xAA packets */
    uint8_t bootBatV32;              /* +0x38 : Boot battery voltage (V * 32) */
    uint8_t lastTxBatV32;            /* +0x39 (0x20001764 / 0x20001760): Latest post-TX battery voltage (V * 32) */
    int8_t lastTempDegC;             /* +0x3a : Cached AONBatMonTemperatureGetDegC() */
    uint32_t seqNum;                 /* +0x44 (0x20000a9a / 0x20000ef8): 16/32-bit event sequence counter */
    uint32_t lastSavedSeqNum;        /* +0x48 : Last sequence counter written to NV item (6, 4, 0) */
    uint16_t bootCounter;            /* +0x4c : Cached NV item (6, 6, 0) boot counter */
    uint32_t epochSyncRtcTick;       /* +0x50 (0x2000177c / 0x20001764): Clock_getTicks() when 0xB0/0xA6 arrived */
    uint64_t epochMsBase;            /* +0x58 (0x20001790 / 0x20001780): 64-bit Unix ms synced from 0xA6/0xB0 */
    uint64_t historyLastTimestampMs; /* +0x60 (0x20001798 / 0x20001788): Timestamp of previous 0xAD packet */
    uint32_t pollingIntervalMs;   /* +0x80 (0x20001f48 / 0x200019c0): 400 ms if W pad low & unjoined, else 700/800 ms */
    uint8_t pirWindowIndex;       /* +0x84 (motion_v1 0x20001732): Current 10s bucket index (0..11) */
    uint8_t pirBuckets[12];       /* +0x88 (motion_v1 0x200017b0): 12 x 10s PIR pulse histogram for 0xAB */
    uint8_t lastTxPlaintext[32];  /* +0xa0 (0x20000b14 / 0x20000f53): Plaintext of last transmitted packet */
    uint8_t lastTxCiphertext[32]; /* +0xc0 (0x20001808 / 0x20001804): Ciphertext saved for 80ms/130ms retries */
} StockSensorCtx_t;

/*
 * SRAM instance of `g_sensorCtx`:
 *   - contact_v1: 0x20000a74 / 0x20001748
 *   - motion_v1 : 0x20000ed8 / 0x20001728
 */
static StockSensorCtx_t g_sensorCtx;

/*
 * Identity, Factory Test & Crypto Context (`g_identCtx`)
 * SRAM addresses:
 *   - contact_v1: 0x20000a74 (offsets +0x38 MAC, +0x48 key, +0x9e challenge, +0x112 UART rxBuf)
 *   - motion_v1 : 0x20000ed8 (offsets +0x30 MAC, +0x40 key, +0x76 challenge, +0xee UART rxBuf)
 */
typedef struct {
    uint8_t macAddress[8];      /* 0x20000aac (contact_v1) / 0x20000f08 (motion_v1): 8-byte ASCII MAC */
    uint8_t xxteaKey[16];       /* 0x20000abc (contact_v1) / 0x20000f18 (motion_v1): 16-byte key from NV item 8 */
    uint8_t joinChallengeR[16]; /* 0x20000b12 (contact_v1) / 0x20000f4e (motion_v1): 16-byte token from 0xA4 */
    uint8_t facUartType;        /* 0x20000b86 (contact_v1) / 0x20000fc6 (motion_v1): rxBuf[2] (0x43 or 0x53) */
    uint8_t facUartLen;         /* 0x20000b87 (contact_v1) / 0x20000fc7 (motion_v1): rxBuf[3] */
    uint8_t facUartCmdId;       /* 0x20000b88 (contact_v1) / 0x20000fc8 (motion_v1): rxBuf[4] command byte */
    uint8_t facUartPayload[32]; /* 0x20000b89 (contact_v1) / 0x20000fc9 (motion_v1): rxBuf[5..] payload */
} StockIdentCtx_t;

/*
 * SRAM instance of `g_identCtx`:
 *   - contact_v1: 0x20000a80
 *   - motion_v1 : 0x20000ed8
 */
static StockIdentCtx_t g_identCtx;

/*
 * Button Debounce & Hold State (`g_btnCtx`)
 * SRAM addresses:
 *   - contact_v1: 0x20000a78 (state) / 0x20000a98 (ticks)
 *   - motion_v1 : 0x20000edc (state) / 0x20000efc (ticks)
 */
typedef struct {
    uint8_t state;         /* +0x00 : 0 = idle, 1 = 40ms debounce, 2 = holding */
    uint8_t debounceTicks; /* +0x01 : 10ms ticks in state 1 (needs 4 ticks = 40 ms) */
    uint16_t holdTicks;    /* +0x02 : 10ms ticks in state 2 (300 = 3.00s, 800 = 8.00s) */
} StockButtonCtx_t;

static StockButtonCtx_t g_btnCtx;

/*
 * Offline History Ring Buffer (`g_historyLog`, 50 entries x 12 bytes = 600 bytes = 0x258)
 * SRAM addresses:
 *   - contact_v1: 0x20000c84 (`0x20000a74 + 0x210`)
 *   - motion_v1 : 0x200010c4 (`0x20000ed8 + 0x1ec`)
 */
typedef struct __attribute__((packed)) {
    uint8_t timestampBe[8]; /* 64-bit big-endian millisecond epoch timestamp */
    uint8_t eventType;      /* 0xA2 */
    uint8_t dataMarker;     /* 0x01 (lastTxPlaintext[14]) */
    uint8_t state;          /* 0x00 or 0x01 (lastTxPlaintext[15]) */
    uint8_t seqNumLo;       /* Low byte of seqNum (lastTxPlaintext[16]) */
} StockHistoryEntry_t;

static StockHistoryEntry_t g_historyLog[50];

/*
 * NVOCTP Function Pointer Table Pointer (`g_pNv`)
 * SRAM addresses:
 *   - contact_v1: 0x200019d4 (points to NVINTF_nvFuncts_t)
 *   - motion_v1 : 0x20001978 (points to NVINTF_nvFuncts_t)
 */
static NVINTF_nvFuncts_t *g_pNv;

/* Forward declarations of TI SDK & helper primitives */
void Ssf_ledBlink(uint32_t halfPeriodMs, uint8_t blinkCount);
void Ssf_factoryResetNv(void);
void Sensor_sendHeartbeat_A1(uint8_t stateByte, void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]));
void Sensor_sendEvent_A2(uint8_t stateByte, void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]));
void Sensor_sendJoinReq_A3(void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]));
void Sensor_sendJoinResp_A5(bool isMotionSensor, void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]));
void Sensor_sendFactoryTestEcho_A9(const uint8_t rxPlain[32], int8_t rxRssi,
                                   void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]));
void Sensor_sendPirSummary_AB(void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]));
void Sensor_sendHistoryLog_AD(void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]),
                              void (*startHistoryTimerMs)(uint32_t ms));
void Sensor_sendDecommAck_B1(uint8_t subCode, void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]));

/* ==========================================================================
 * 2. NVOCTP Persistence Helpers & Boot Sequence (Root Cause of Zero-MAC Bug)
 * ========================================================================== */

/*
 * Ssf_incrementBootCounter()
 * Flash addresses:
 *   - contact_v1: 0x0001216c
 *   - motion_v1 : 0x000123ec
 *
 * BUG (Z1): Called unconditionally at the start of Sensor_init() BEFORE checking
 * battery voltage. Reads 2-byte NV item (6, 6, 0), adds 1, and writes it back to
 * flash page 30 on every single power-on or brownout reset.
 */
void Ssf_incrementBootCounter(void)
{
    NVINTF_itemID_t id = {NVINTF_SYSID_APP, SENSOR_NV_BOOT_COUNTER, 0};
    uint16_t cnt = 0;

    if (g_pNv && g_pNv->readItem) {
        if (g_pNv->readItem(id, 0, sizeof(cnt), &cnt) == 0 && cnt < 0xFFF0u) {
            g_sensorCtx.bootCounter = cnt + 1;
        } else {
            g_sensorCtx.bootCounter = 1;
        }
        g_pNv->updateItem(id, sizeof(uint16_t), &g_sensorCtx.bootCounter);
    }
}

/*
 * Ssf_initSeqCounter()
 * Flash addresses:
 *   - contact_v1: 0x00010278
 *   - motion_v1 : 0x00010558
 *
 * Reads 4-byte sequence counter base from NV item (6, 4, 0), advances it by +25
 * (wear-leveling stride so runtime events only write flash every 25 increments),
 * and immediately writes the new base back to flash page 30.
 *
 * BUG (Z1): Also runs unconditionally on every boot BEFORE battery voltage check!
 */
void Ssf_initSeqCounter(void)
{
    NVINTF_itemID_t id = {NVINTF_SYSID_APP, SENSOR_NV_SEQ_COUNTER, 0};
    uint32_t savedSeq = 0;

    if (g_pNv && g_pNv->readItem(id, 0, sizeof(savedSeq), &savedSeq) == 0 && savedSeq < 0xFFFFFFE5u) {
        g_sensorCtx.seqNum = savedSeq + 25u;
    } else {
        g_sensorCtx.seqNum = 0;
    }
    g_sensorCtx.lastSavedSeqNum = g_sensorCtx.seqNum;
    g_pNv->updateItem(id, sizeof(uint32_t), &g_sensorCtx.seqNum);
}

/*
 * Ssf_readMacAndKey()
 * Flash addresses:
 *   - contact_v1: 0x000115b8 / 0x00011c50
 *   - motion_v1 : 0x00011870 / 0x00011ec8
 *
 * BUG (Z1): Reads the sensor's 8-byte ASCII MAC and 16-byte XXTEA key ONLY from
 * NVOCTP item (6, 8, 0) in flash page 30 (0x1E000). It NEVER reads CCFG 0x0001FFCC!
 * When page 30 is erased by an NVOCTP compaction brownout, `readItem` returns non-zero
 * and `g_identCtx.macAddress` remains all-zeros (`00 00 00 00 00 00 00 00`).
 */
bool Ssf_readMacAndKey(void)
{
    NVINTF_itemID_t id = {NVINTF_SYSID_APP, SENSOR_NV_MAC_AND_KEY, 0};
    StockSensorNv8Record_t rec;
    memset(&rec, 0, sizeof(rec));

    if (g_pNv && g_pNv->readItem(id, 0, sizeof(rec), &rec) == 0) {
        memcpy(g_identCtx.xxteaKey, rec.xxteaKey, 16);
        memcpy(g_identCtx.macAddress, rec.mac, 8);
        g_sensorCtx.nv8KeyValidFlag = rec.keyValidFlag;
        return true;
    }
    /* CCFG 0x0001FFCC is never consulted here; g_identCtx.macAddress stays 0! */
    return false;
}

/*
 * Ssf_writeMacAndKey()
 * Flash addresses:
 *   - contact_v1: 0x00012734
 *   - motion_v1 : 0x00012a88
 *
 * Writes the 25-byte `{xxteaKey[16], mac[8], keyValidFlag}` record to NV item (6, 8, 0).
 */
bool Ssf_writeMacAndKey(const uint8_t key[16], const uint8_t mac[8], uint8_t validFlag)
{
    NVINTF_itemID_t id = {NVINTF_SYSID_APP, SENSOR_NV_MAC_AND_KEY, 0};
    StockSensorNv8Record_t rec;
    memcpy(rec.xxteaKey, key, 16);
    memcpy(rec.mac, mac, 8);
    rec.keyValidFlag = validFlag;
    return (g_pNv->updateItem(id, sizeof(rec), &rec) == 0);
}

/*
 * Ssf_readNvStatusFlags()
 * Flash addresses:
 *   - contact_v1: 0x0001104c (item 10) and 0x00011094 (item 11)
 *   - motion_v1 : 0x00010f8c (item 10), 0x00011308 (item 11), and 0x000112c0 (item 12)
 *
 * Reads the 1-byte NVOCTP factory/status flags into `g_sensorCtx`.
 */
void Ssf_readNvStatusFlags(bool isMotionSensor)
{
    if (!g_pNv || !g_pNv->readItem) {
        return;
    }
    NVINTF_itemID_t id9 = {NVINTF_SYSID_APP, SENSOR_NV_JOINED_FLAG, 0};
    NVINTF_itemID_t id10 = {NVINTF_SYSID_APP, SENSOR_NV_FACTEST_FLAG, 0};
    NVINTF_itemID_t id11 = {NVINTF_SYSID_APP, SENSOR_NV_LOWBAT_LATCH, 0};

    (void)g_pNv->readItem(id9, 0, 1, &g_sensorCtx.joinedNvFlag);
    (void)g_pNv->readItem(id10, 0, 1, &g_sensorCtx.nv10FacTestFlag);
    (void)g_pNv->readItem(id11, 0, 1, &g_sensorCtx.nv11LowBatLatch);

    if (isMotionSensor) {
        NVINTF_itemID_t id12 = {NVINTF_SYSID_APP, 12, 0};
        (void)g_pNv->readItem(id12, 0, 1, &g_sensorCtx.nv12Flag);
    }
}

/*
 * Ssf_checkBootBattery()
 * Flash addresses:
 *   - contact_v1: 0x00011bc0
 *   - motion_v1 : 0x00011e3c
 *
 * Samples `AONBatMonBatteryVoltageGet() >> 3` (1/32 V scale) 6 times at 200 ms
 * intervals (1.2 s total). If any reading is <= 0x53 (83/32 = 2.59375 V), latches
 * `g_sensorCtx.lowBatBootFlag = 1`.
 */
void Ssf_checkBootBattery(uint8_t (*readBatV32)(void), void (*sleepMs)(uint32_t))
{
    for (int i = 0; i < 6; i++) {
        sleepMs(200);
        uint8_t v32 = readBatV32();
        g_sensorCtx.bootBatV32 = v32;
        g_sensorCtx.lastTxBatV32 = v32;
        if (v32 <= 0x53u) { /* <= 2.59375 V */
            g_sensorCtx.lowBatBootFlag = 1;
        }
    }
}

/*
 * Ssf_sampleBatteryPostTx()
 * Flash addresses:
 *   - contact_v1: 0x00011c14
 *   - motion_v1 : 0x00011e8c
 *
 * Called immediately after every `ApiMac_mcpsDataReq()` radio transmission to
 * measure battery voltage under load. If `v32 < 0x54` (2.625 V) on 3 consecutive
 * transmissions, latches `nv11LowBatLatch = 1` in NV item (6, 11, 0) (`pt[12]`).
 */
void Ssf_sampleBatteryPostTx(uint8_t (*readBatV32)(void))
{
    uint8_t v32 = readBatV32();
    g_sensorCtx.lastTxBatV32 = v32;
    if (v32 < 0x54u) {
        g_sensorCtx.lowBatTxStreak++;
        if (g_sensorCtx.lowBatTxStreak >= 3u) {
            g_sensorCtx.nv11LowBatLatch = 1;
            NVINTF_itemID_t id = {NVINTF_SYSID_APP, SENSOR_NV_LOWBAT_LATCH, 0};
            g_pNv->updateItem(id, 1, &g_sensorCtx.nv11LowBatLatch);
        }
    } else {
        g_sensorCtx.lowBatTxStreak = 0;
    }
}

/*
 * Sensor_init()
 * Flash addresses:
 *   - contact_v1: 0x000027dc (with NV/battery helpers at 0x00007c18)
 *   - motion_v1 : 0x000080ac
 *
 * Initializes sensor clocks, checks the `W` pad (`DIO10` on contact_v1) for the
 * default unjoined poll interval (400 ms vs 700 ms), writes NV items 6 and 4,
 * checks battery voltage, loads MAC/key from NV item 8, and either:
 *   - Normal boot (`factoryTestActive == 0`): blinks LED 1x (300 ms) if provisioned
 *     or 2x (300 ms) if unprovisioned (`0x02ba8..0x02bc4`), and posts `0x0001`
 *     (`SENSOR_START_EVT`) at `0x02c86..0x02c94`.
 *   - Factory mode (`factoryTestActive == 1`, `W` pad held low at boot): turns the
 *     red LED solid ON (`Ssf_setLed(1, 1)` at `0x02bd4`) and skips posting
 *     `SENSOR_START_EVT` (`0x02c86..0x02c8c`).
 */
void Sensor_init(uint8_t (*readBatV32)(void), void (*sleepMs)(uint32_t), uint32_t (*readDio)(uint32_t dio))
{
    /* 0. Check DIO10 (W pad) at 0x027f6..0x0281e: 400 ms unjoined poll if W is low, else 700 ms */
    uint32_t pollMs = (readDio(10) != 0) ? 700u : 400u;
    if (g_sensorCtx.joinedNvFlag != 0) {
        pollMs = 700u;
    }
    g_sensorCtx.pollingIntervalMs = pollMs;

    /* 1. Unconditional flash writes BEFORE battery check (causes brownout NVOCTP wipe!) */
    Ssf_incrementBootCounter();
    Ssf_initSeqCounter();

    /* 2. Sample battery 6 times over 1.2 s (threshold 0x53 = 2.59 V) */
    Ssf_checkBootBattery(readBatV32, sleepMs);

    if (g_sensorCtx.lowBatBootFlag == 0) {
        Ssf_readMacAndKey();
        if (g_sensorCtx.factoryTestActive != 0) {
            /* 0x02bd4: W pad was held LOW at boot -> turn red LED solid ON and stay idle for UART jig */
            Ssf_ledBlink(0, 0xFF);
        } else {
            /* 0x02ba8..0x02bc4: Normal boot -> 1x 300 ms blink if provisioned, 2x 300 ms if unprovisioned */
            bool provisioned = (g_sensorCtx.nv8KeyValidFlag == 1) && (g_sensorCtx.nv10FacTestFlag == 1);
            Ssf_ledBlink(300, provisioned ? 1 : 2);
        }
    } else {
        /* Low-battery boot: skips Ssf_readMacAndKey() and blinks 100 ms */
        Ssf_ledBlink(100, 1);
    }
}

/* ==========================================================================
 * 3. Boot Strap (`main`) & Factory UART Task (`W` Pad / `DIO10` Held Low)
 * ========================================================================== */

/*
 * BoardGpioInitTable
 * Flash addresses:
 *   - contact_v1: 0x0001532c
 *   - motion_v1 : 0x000156b0
 */
static const uint32_t g_contactV1BoardGpioInitTable[5] = {
    0x80800809u, /* 0x1532c: DIO9  (LED)     | PIN_GPIO_OUTPUT_EN | PIN_GPIO_LOW | PIN_PUSHPULL | PIN_DRVSTR_MAX */
    0x80002001u, /* 0x15330: DIO1  (RX pad)  | PIN_INPUT_EN | PIN_PULLUP */
    0x80002000u, /* 0x15334: DIO0  (TX pad)  | PIN_INPUT_EN | PIN_PULLUP */
    0xC000200Au, /* 0x15338: DIO10 (W pad)   | PIN_INPUT_EN | PIN_HYSTERESIS | PIN_PULLUP */
    0x000000FEu, /* 0x1533c: PIN_TERMINATE */
};

/*
 * main()
 * Flash addresses:
 *   - contact_v1: 0x0000994c
 *   - motion_v1 : 0x0000a568
 *
 * Initializes board power and `BoardGpioInitTable`, then samples `DIO0` (`TX`) and
 * `DIO10` (`W` pad on contact_v1; `DIO2` on motion_v1) twice via `GPIO_readDio()`
 * (`0x000140ac` in contact_v1, `0x0001440c` in motion_v1).
 *
 *   - Normal boot (`W` HIGH): `g_sensorCtx.factoryTestActive` (`0x20000a74`) stays 0;
 *     `FactoryUart_taskFxn` (`0x0000b044`) is NEVER constructed and `UART0` is never opened.
 *   - Factory mode (`W` held LOW to GND at reset): sets `0x20000a74 = 1` and
 *     `0x20000a8b = 1`, and constructs `FactoryUart_taskFxn` (`0x0000b044`, pri 2,
 *     stack `0x3c0`) so the CC1310 stays awake listening on `UART0` (115200 8N1).
 */
void StockSensor_main(uint32_t (*readDio)(uint32_t dio), void (*constructSensorTask)(void),
                      void (*constructFactoryUartTask)(void), void (*biosStart)(void))
{
    (void)g_contactV1BoardGpioInitTable;
    g_sensorCtx.factoryTestActive = 0;

    /* 0x099c6..0x099e8: Double-read debounce of DIO0 (TX pad) and DIO10 (W pad) */
    if ((readDio(0) == 0 || readDio(10) == 0) && (readDio(0) == 0 || readDio(10) == 0)) {
        g_sensorCtx.factoryTestActive = 1; /* 0x20000a74 = 1, 0x20000a8b = 1 */
    }

    /* 0x09a06..0x09a10: Task 1 (pri 1, stack 0x3c0) - Sensor_taskFxn (0x00002678) */
    constructSensorTask();

    /* 0x09a14..0x09a3c: Task 2 (pri 2, stack 0x3c0) - FactoryUart_taskFxn (0x0000b044) */
    if (g_sensorCtx.factoryTestActive != 0) {
        constructFactoryUartTask();
    }

    /* 0x09a40: BIOS_start() */
    biosStart();
}

/*
 * UartFactory_open() and FactoryUart_taskFxn()
 * Flash addresses:
 *   - contact_v1: UartFactory_open = 0x0000f120, FactoryUart_taskFxn = 0x0000b044
 *   - motion_v1 : UartFactory_open = 0x0000fd74, FactoryUart_taskFxn = 0x0000c7e8
 *
 * Opens UART0 (`DIO0` TX, `DIO1` RX) at 115200 8N1 (`0x1c200` at `0x0f154`) and
 * loops reading bytes (`UART_read(handle, &ch, 1)` at `0x0b070`). When a valid
 * `AA 55 <type> <len> <cmd> ... <csum_hi> <csum_lo>` frame arrives, copies payload
 * bytes starting at `<type>` into `g_identCtx` (`0x20000b86` / `0x20000fc6`) and
 * sets event `0x0200` on `Sensor_events` (`0x0b108`).
 */
void FactoryUart_taskFxn(int (*uartReadByte)(uint8_t *outByte), void (*postSensorEvent)(uint16_t evt))
{
    uint8_t rxBuf[36];
    uint8_t idx = 0;
    uint8_t expectedLen = 8;
    bool inFrame = false;

    for (;;) {
        uint8_t b = 0;
        if (uartReadByte(&b) <= 0) {
            continue;
        }
        if (!inFrame) {
            if (b == 0xAAu) {
                inFrame = true;
                idx = 0;
                expectedLen = 8;
                rxBuf[idx++] = b;
            }
            continue;
        }
        if (idx < sizeof(rxBuf)) {
            rxBuf[idx++] = b;
        }
        if (idx == 4u) {
            expectedLen = (uint8_t)(rxBuf[3] + 4u);
        }
        if (idx >= expectedLen) {
            inFrame = false;
            g_identCtx.facUartType = rxBuf[2];
            g_identCtx.facUartLen = rxBuf[3];
            g_identCtx.facUartCmdId = rxBuf[4];
            memcpy(g_identCtx.facUartPayload, &rxBuf[5], sizeof(g_identCtx.facUartPayload));
            postSensorEvent(0x0200u);
        }
    }
}

/*
 * FactoryTest_sendReply43()
 * Flash addresses:
 *   - contact_v1: 0x00009124
 *   - motion_v1 : 0x00009448
 *
 * Builds and sends the UART response frame (`AA 55 43 <len> <replyCmd> ... <csum_hi> <csum_lo>`)
 * for all five class-`0x43` factory commands (`0x04`, `0x06`, `0x08`, `0x0A`, `0xD0`):
 *   - `0x04` (Read 8B MAC)     -> `0x05` (`len=11`, 8-byte MAC from NV item 8)
 *   - `0x06` (Read 16B Key)    -> `0x07` (`len=19`, 16-byte XXTEA key from NV item 8)
 *   - `0x08` (Write 8B MAC)    -> `0x09` (`len=4`, `0xFF` if `ok==1` else `0x00`)
 *   - `0x0A` (Write 16B Key)   -> `0x0B` (`len=4`, `0xFF` if `ok==1` else `0x00`)
 *   - `0xD0` (Read NV Flags)   -> `0xD1` (`len=6` on contact_v1: `{joined, facTest, lowBat}`;
 *                                         `len=7` on motion_v1 : `{joined, facTest, nv12, lowBat}`)
 */
void FactoryTest_sendReply43(uint8_t ok, bool isMotionSensor, void (*uartWrite)(const uint8_t *buf, uint32_t len))
{
    uint8_t tx[128];
    memset(tx, 0, sizeof(tx));
    tx[0] = 0xAAu;
    tx[1] = 0x55u;
    tx[2] = 0x43u;

    uint8_t cmd = g_identCtx.facUartCmdId;
    uint8_t lenField = 0;
    uint8_t replyCmd = 0;

    if (cmd == 0x04u) {
        /* 0x091d8 / 0x09504: Read 8-byte MAC from NV item (6, 8, 0) */
        Ssf_readMacAndKey();
        lenField = 11u; /* 0x0B */
        replyCmd = 0x05u;
        memcpy(&tx[5], g_identCtx.macAddress, 8);
    } else if (cmd == 0x06u) {
        /* 0x0919c / 0x094c8: Read 16-byte XXTEA key from NV item (6, 8, 0) */
        Ssf_readMacAndKey();
        lenField = 19u; /* 0x13 */
        replyCmd = 0x07u;
        memcpy(&tx[5], g_identCtx.xxteaKey, 16);
    } else if (cmd == 0x08u) {
        /* 0x09186 / 0x094b2: Write 8-byte MAC response */
        lenField = 4u;
        replyCmd = 0x09u;
        tx[5] = (ok == 1u) ? 0xFFu : 0x00u;
    } else if (cmd == 0x0Au) {
        /* 0x09182 / 0x094ae: Write 16-byte XXTEA key response */
        lenField = 4u;
        replyCmd = 0x0Bu;
        tx[5] = (ok == 1u) ? 0xFFu : 0x00u;
    } else if (cmd == 0xD0u) {
        /* 0x09166 / 0x09488: Query NVOCTP status flags */
        Ssf_readNvStatusFlags(isMotionSensor);
        replyCmd = 0xD1u;
        tx[5] = g_sensorCtx.joinedNvFlag;
        tx[6] = g_sensorCtx.nv10FacTestFlag;
        if (isMotionSensor) {
            lenField = 7u;
            tx[7] = g_sensorCtx.nv12Flag;
            tx[8] = g_sensorCtx.nv11LowBatLatch;
        } else {
            lenField = 6u;
            tx[7] = g_sensorCtx.nv11LowBatLatch;
        }
    }

    tx[3] = lenField;
    tx[4] = replyCmd;

    uint16_t sum = 0;
    uint8_t sumBytes = (uint8_t)(lenField + 2u);
    for (uint8_t i = 0; i < sumBytes; i++) {
        sum = (uint16_t)(sum + tx[i]);
    }
    tx[sumBytes] = (uint8_t)(sum >> 8);
    tx[sumBytes + 1u] = (uint8_t)(sum & 0xFFu);

    uartWrite(tx, (uint32_t)(lenField + 4u));
}

/*
 * FactoryTest_process43()
 * Flash addresses:
 *   - contact_v1: 0x0000ca44
 *   - motion_v1 : 0x0000cc14
 *
 * Handles class-`0x43` factory provisioning commands (`0x04`, `0x06`, `0x08`, `0x0A`, `0xD0`)
 * and then invokes `FactoryTest_sendReply43()` (`0x09124` / `0x09448`).
 *
 * Notice command `0x08` ("Write MAC" at `0x0caa6` / `0x0cc76`): this is the ONLY code
 * path in the entire firmware that writes CCFG `0x0001FFCC` (`0x13908` / `0x13c7c`)!
 */
void FactoryTest_process43(bool isMotionSensor,
                           void (*programFlash)(const uint8_t *src, uint32_t dstAddr, uint32_t len),
                           void (*uartWrite)(const uint8_t *buf, uint32_t len))
{
    uint8_t cmd = g_identCtx.facUartCmdId;
    uint8_t ok = 0;

    if (cmd == 0x08u) {
        /* 0x0caa6 / 0x0cc76: Write 8-byte ASCII MAC to CCFG 0x0001FFCC and NVOCTP item (6, 8, 0) */
        memcpy(g_identCtx.macAddress, g_identCtx.facUartPayload, 8);
        programFlash(g_identCtx.macAddress, STOCK_CCFG_MAC_ADDR, 8);
        Ssf_writeMacAndKey(g_identCtx.xxteaKey, g_identCtx.macAddress, 1u);
        ok = 1u;
    } else if (cmd == 0x0Au) {
        /* 0x0ca72 / 0x0cc42: Write 16-byte sensor XXTEA key to NVOCTP item (6, 8, 0) */
        memcpy(g_identCtx.xxteaKey, g_identCtx.facUartPayload, 16);
        g_sensorCtx.nv8KeyValidFlag = 1u;
        Ssf_writeMacAndKey(g_identCtx.xxteaKey, g_identCtx.macAddress, 1u);
        ok = 1u;
    } else if (cmd == 0xD0u || cmd == 0x04u || cmd == 0x06u) {
        ok = 1u;
    }

    FactoryTest_sendReply43(ok, isMotionSensor, uartWrite);
}

/*
 * FactoryTest_sendVersion_5317()
 * Flash addresses:
 *   - contact_v1: 0x0000eb00
 *   - motion_v1 : 0x0000ec6c
 *
 * Responds to `AA 55 53 03 16 <csum>` with `55 AA 53 <len> 17 <version_str> <csum>`.
 */
void FactoryTest_sendVersion_5317(const char *versionStr, uint8_t versionLen,
                                  void (*uartWrite)(const uint8_t *buf, uint32_t len))
{
    uint8_t tx[32];
    tx[0] = 0x55u;
    tx[1] = 0xAAu;
    tx[2] = 0x53u;
    tx[3] = (uint8_t)(versionLen + 3u);
    tx[4] = 0x17u;
    memcpy(&tx[5], versionStr, versionLen);

    uint16_t sum = 0;
    uint8_t sumBytes = (uint8_t)(versionLen + 5u);
    for (uint8_t i = 0; i < sumBytes; i++) {
        sum = (uint16_t)(sum + tx[i]);
    }
    tx[sumBytes] = (uint8_t)(sum >> 8);
    tx[sumBytes + 1u] = (uint8_t)(sum & 0xFFu);

    uartWrite(tx, (uint32_t)(versionLen + 7u));
}

/*
 * FactoryTest_process53()
 * Flash addresses:
 *   - contact_v1: 0x00001ac0..0x00001bd6 (inside Sensor_process event 0x0200)
 *   - motion_v1 : 0x00001cd6..0x00001e64 (inside Sensor_process event 0x0200)
 *
 * Handles class-`0x53` factory commands:
 *   - `0x16`: Send firmware version (`0x53 0x17`) via `FactoryTest_sendVersion_5317()`.
 *   - `0x2A`: Start RF Factory PER Test (`0x53 0x2B` ACK) on channel index `1..16`, mapped
 *             via the 16-entry `TBB` table at `0x01afc` / `0x01d6c` to 802.15.4 channel
 *             numbers `{19..25, 32..34, 38..40, 48..50}`, then posts `SENSOR_START_EVT` (`0x0001`).
 *   - `0x2D`: Factory Test Finish — if `payload[0] == 0xFF`, writes `1` to NVOCTP item
 *             `(6, 10, 0)` (`SENSOR_NV_FACTEST_FLAG`) and blinks LED `3x 400 ms`.
 *   - `0xD2` (`motion_v1` only at `0x01e02`): Start 20-second PIR Hardware Self-Test
 *             (`20,000 ms` timer at `0x125e0`; sends `0x53 0xD3` in event `0x8000`).
 *   - `0xD4` (`motion_v1` only at `0x01cfe`): If `payload[0] == 0xFF`, writes `1` to
 *             NVOCTP item `(6, 12, 0)`.
 */
void FactoryTest_process53(bool isMotionSensor, void (*uartWrite)(const uint8_t *buf, uint32_t len),
                           void (*startGpioSelfTestTimerMs)(uint32_t ms))
{
    static const uint8_t kFacTestChannelMap[16] = {
        0x13u, 0x14u, 0x15u, 0x16u, 0x17u, 0x18u, 0x19u, /* indices 1..7  -> ch 19..25 (rfTestModeActive = 1) */
        0x20u, 0x21u, 0x22u,                             /* indices 8..10 -> ch 32..34 (rfTestModeActive = 1) */
        0x26u, 0x27u, 0x28u,                             /* indices 11..13 -> ch 38..40 */
        0x30u, 0x31u, 0x32u                              /* indices 14..16 -> ch 48..50 */
    };

    uint8_t cmd = g_identCtx.facUartCmdId;
    uint8_t arg = g_identCtx.facUartPayload[0];

    if (cmd == 0x16u) {
        /* 0x01b94 / 0x01e20: Get Firmware Version */
        const char *ver = isMotionSensor ? "1.8.0.0" : "1.6.0.0";
        FactoryTest_sendVersion_5317(ver, 7u, uartWrite);
    } else if (cmd == 0x2Au) {
        /* 0x01ad6 / 0x01d40: Start RF Factory Test on channel index 1..16 */
        Ssf_factoryResetNv();
        if (arg >= 1u && arg <= 16u) {
            g_sensorCtx.rfTestChannel = kFacTestChannelMap[arg - 1u];
            if (arg <= 10u) {
                g_sensorCtx.rfTestModeActive = 1u;
            }
        }
        uint8_t ack[7] = {0xAAu, 0x55u, 0x53u, 0x03u, 0x2Bu, 0x01u, 0x80u};
        uartWrite(ack, sizeof(ack));
        g_sensorCtx.joinCyclesRemaining = 30u;
        g_sensorCtx.sensorEvents |= 0x0001u; /* SENSOR_START_EVT */
    } else if (cmd == 0x2Du) {
        /* 0x01b98 / 0x01e24: Factory Test Finish -> write NV item (6, 10, 0) = 1 and blink 3x 400 ms */
        if (arg == 0xFFu) {
            g_sensorCtx.nv10FacTestFlag = 1u;
            if (g_pNv && g_pNv->updateItem) {
                NVINTF_itemID_t id10 = {NVINTF_SYSID_APP, SENSOR_NV_FACTEST_FLAG, 0};
                g_pNv->updateItem(id10, 1, &g_sensorCtx.nv10FacTestFlag);
            }
            Ssf_ledBlink(400, 3);
        }
    } else if (isMotionSensor && cmd == 0xD2u) {
        /* 0x01e02: Start 20-second PIR Hardware Self-Test (replies with 0x53 0xD3 on event 0x8000) */
        g_sensorCtx.gpioSelfTestActive = 1u;
        g_sensorCtx.gpioSelfTestTriggerCount = 0u;
        if (startGpioSelfTestTimerMs) {
            startGpioSelfTestTimerMs(20000u); /* 0x4E20 = 20,000 ms */
        }
    } else if (isMotionSensor && cmd == 0xD4u) {
        /* 0x01cfe: Write 1 to NVOCTP item (6, 12, 0) */
        if (arg == 0xFFu && g_pNv && g_pNv->updateItem) {
            g_sensorCtx.nv12Flag = 1u;
            NVINTF_itemID_t id12 = {NVINTF_SYSID_APP, 12, 0};
            g_pNv->updateItem(id12, 1, &g_sensorCtx.nv12Flag);
        }
    }
}

/* ==========================================================================
 * 4. Button State Machine & LED Blink Driver (Unknowns B1–B5)
 * ========================================================================== */

/*
 * Ssf_ledBlink() and Ssf_ledClockCb()
 * Flash addresses:
 *   - contact_v1: Ssf_ledBlink = 0x000140fc, Ssf_ledClockCb = 0x00012054 (event 0x0400 at 0x01a56)
 *   - motion_v1 : Ssf_ledBlink = 0x000140f8, Ssf_ledClockCb = 0x000122bc (event 0x0400 at 0x01c6c)
 *
 * Blinks the sensor LED (`DIO9` on contact_v1, `DIO12` on motion_v1) `blinkCount`
 * times with `halfPeriodMs` ON and `halfPeriodMs` OFF.
 */
void Ssf_ledBlink(uint32_t halfPeriodMs, uint8_t blinkCount)
{
    (void)halfPeriodMs;
    g_sensorCtx.ledToggleEdgesLeft = (uint8_t)(blinkCount * 2u);
    /* Starts TI-RTOS clock `ledClkStruct` with initial & periodic timeout = halfPeriodMs */
}

/*
 * Ssf_buttonClockCb()
 * Flash addresses:
 *   - contact_v1: 0x000017e0..0x000018a0 (inside Sensor_process event 0x0080)
 *   - motion_v1 : 0x000019a0..0x00001a60 (inside Sensor_process event 0x0080)
 *
 * Invoked every 10 ms by `buttonClkStruct` (`event 0x0080`) after a falling-edge
 * interrupt on the pairing button (`DIO11` on contact_v1, `DIO13` on motion_v1).
 *
 * State machine:
 *   - State 1 (Debounce): Requires 4 consecutive 10 ms ticks (40 ms) held LOW.
 *   - State 2 (Hold timer):
 *       * At tick == 300 (3.04 s total): Calls `Ssf_readNvStatusFlags()` and blinks
 *         LED 3x (400 ms) if provisioned & battery OK, or 5x (200 ms) if unprovisioned
 *         (Zero-MAC) / low battery (`0x019d2..0x019f0`).
 *       * At tick > 800 (> 8.04 s total): Stuck-button timeout (`action = 0`).
 *       * Released at < 200 ticks (< 2.04 s): Short press (`action = 3`).
 *       * Released at 200..300 ticks (2.04 s .. 3.04 s): Ignored (`action = 0`).
 *       * Released at 301..800 ticks (3.05 s .. 8.04 s): Long press join (`action = 4`).
 */
void Ssf_buttonClockCb(bool isButtonPinLow)
{
    if (g_btnCtx.state == 1) {
        if (isButtonPinLow) {
            g_btnCtx.debounceTicks++;
            if (g_btnCtx.debounceTicks >= 4u) { /* 40 ms debounce complete */
                g_btnCtx.debounceTicks = 0;
                g_btnCtx.holdTicks = 0;
                g_btnCtx.state = 2;
            }
        } else {
            g_btnCtx.debounceTicks = 0;
            g_btnCtx.state = 0;
        }
        return;
    }

    if (g_btnCtx.state == 2) {
        if (isButtonPinLow) {
            g_btnCtx.holdTicks++;
            if (g_btnCtx.holdTicks == 300u) { /* 3.00 s after debounce = 3.04 s total */
                bool provisioned = (g_sensorCtx.nv8KeyValidFlag == 1) && (g_sensorCtx.nv10FacTestFlag == 1) &&
                                   (g_sensorCtx.nv11LowBatLatch == 0) && (g_sensorCtx.lowBatBootFlag == 0);
                if (provisioned) {
                    Ssf_ledBlink(400, 3); /* 3 slow blinks: ready to pair on release */
                } else {
                    Ssf_ledBlink(200, 5); /* 5 fast blinks: error / unprovisioned */
                }
            } else if (g_btnCtx.holdTicks > 800u) { /* > 8.04 s: stuck button timeout */
                g_btnCtx.holdTicks = 0;
                g_btnCtx.state = 0;
                g_sensorCtx.buttonAction = 0;
            }
        } else {
            /* Button released during State 2 */
            if (g_btnCtx.holdTicks < 200u) {
                g_sensorCtx.buttonAction = 3; /* Short press (< 2.04 s) */
            } else if (g_btnCtx.holdTicks <= 300u) {
                g_sensorCtx.buttonAction = 0; /* Dead zone (2.04 s .. 3.04 s): ignored */
            } else {
                g_sensorCtx.buttonAction = 4; /* Long press (3.05 s .. 8.04 s): join */
            }
            g_btnCtx.holdTicks = 0;
            g_btnCtx.state = 0;
        }
    }
}

/*
 * Ssf_factoryResetNv()
 * Flash addresses:
 *   - contact_v1: 0x00012c88 / 0x000140e8 (inlined NV item deletion at 0x01908..0x019d0)
 *   - motion_v1 : 0x00013004 (inlined NV item deletion at 0x01b1c..0x01bd0)
 *
 * Wipes all network and session NVOCTP items (items 2, 3[0..499], 4, 5, 6, 9) and
 * zeroes the 600-byte `g_historyLog` circular buffer (`0x258` bytes), leaving only
 * NV items 8 (MAC+key), 10 (factory test flag), and 11 (low-battery latch) intact.
 */
void Ssf_factoryResetNv(void)
{
    static const uint16_t itemsToDelete[] = {
        SENSOR_NV_CONFIG,       /* item 2 */
        SENSOR_NV_SEQ_COUNTER,  /* item 4 */
        SENSOR_NV_TIME_SYNC,    /* item 5 */
        SENSOR_NV_BOOT_COUNTER, /* item 6 */
        SENSOR_NV_JOINED_FLAG,  /* item 9 */
    };
    if (g_pNv && g_pNv->deleteItem) {
        for (unsigned i = 0; i < sizeof(itemsToDelete) / sizeof(itemsToDelete[0]); i++) {
            NVINTF_itemID_t id = {NVINTF_SYSID_APP, itemsToDelete[i], 0};
            g_pNv->deleteItem(id);
        }
        for (uint16_t sub = 0; sub < 500u; sub++) {
            NVINTF_itemID_t logId = {NVINTF_SYSID_APP, SENSOR_NV_HISTORY_LOG, sub};
            g_pNv->deleteItem(logId);
        }
    }
    memset(g_historyLog, 0, sizeof(g_historyLog));
    g_sensorCtx.historyCount = 0;
    g_sensorCtx.historyWriteOffset = 0;
    g_sensorCtx.historyReadOffset = 0;
    g_sensorCtx.historyEmptyFlag = 1;
    g_sensorCtx.joinedNvFlag = 0;
    g_sensorCtx.seqNum = 0;
    g_sensorCtx.lastSavedSeqNum = 0;
}

/*
 * Sensor_handleKeys()
 * Flash addresses:
 *   - contact_v1: 0x000018a0..0x000019fc (inside Sensor_process event 0x0080)
 *   - motion_v1 : 0x00001a4e..0x00001c00 (inside Sensor_process event 0x0080)
 *
 * Processes `g_sensorCtx.buttonAction` set by `Ssf_buttonClockCb()`.
 */
void Sensor_handleKeys(bool isMotionSensor)
{
    if (g_sensorCtx.buttonAction == 4u) {
        /* 3-second hold released: reset network state and start 30-cycle (or 50-cycle) join scan */
        Ssf_factoryResetNv();
        g_sensorCtx.joinCyclesRemaining = isMotionSensor ? 50u : 30u; /* 0x32 motion_v1, 0x1E contact_v1 */
        g_sensorCtx.joinState = 0u;
        g_sensorCtx.sensorEvents |= 0x0001u; /* SENSOR_START_EVT */
    } else if (g_sensorCtx.buttonAction == 3u) {
        /* Short press (< 2.04 s): no-op on contact_v1; arms 1-shot PIR LED on motion_v1 (0x01bee) */
        if (isMotionSensor) {
            g_sensorCtx.pirLedTestArm = 1u;
        }
    }
    g_sensorCtx.buttonAction = 0;
}

/* ==========================================================================
 * 5. Reporting, Heartbeat, Retry & History Log Logic (Unknowns R1–R8)
 * ========================================================================== */

/*
 * Sensor_sendHeartbeat_A1()
 * Flash addresses:
 *   - contact_v1: 0x0000c2f8
 *   - motion_v1 : 0x0000be78
 *
 * Builds a 32-byte AES-128-ECB encrypted `0xA1` heartbeat report, saves the 17-byte
 * plaintext in `g_sensorCtx.lastTxPlaintext` and the 32-byte ciphertext in
 * `g_sensorCtx.lastTxCiphertext`, transmits via `Sensor_sendMsg` (`0x0acc4` / `0x0ae90`),
 * and samples post-TX battery voltage (`AONBatMonBatteryVoltageGet() >> 3`).
 * Note: Does NOT increment `g_sensorCtx.seqNum`!
 */
void Sensor_sendHeartbeat_A1(uint8_t stateByte, void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]))
{
    StockReportA1A2_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.cmd = 0xA1u;
    memcpy(pkt.mac, g_identCtx.macAddress, 8);
    pkt.sensorType = 0x01u; /* 0x01 on contact_v1 (0x0c330), 0x02 on motion_v1 (0x0bea4) */
    pkt.tempDegC = g_sensorCtx.lastTempDegC;
    pkt.batV32 = g_sensorCtx.lastTxBatV32;
    pkt.lowBatLatch = g_sensorCtx.nv11LowBatLatch;
    pkt.dataLenLo = 0x01u;
    pkt.state = stateByte;
    pkt.seqNumBe = (uint16_t)((g_sensorCtx.seqNum >> 8) | (g_sensorCtx.seqNum << 8));

    memset(g_sensorCtx.lastTxPlaintext, 0, sizeof(g_sensorCtx.lastTxPlaintext));
    memcpy(g_sensorCtx.lastTxPlaintext, &pkt, 17u);
    Stock_aes128_ecb_encryptBuffer((const uint8_t *)&pkt, sizeof(pkt), g_sensorCtx.lastTxCiphertext, aes128EcbBlock16);
}

/*
 * Sensor_sendEvent_A2()
 * Flash addresses:
 *   - contact_v1: 0x0000c238
 *   - motion_v1 : 0x0000bdac
 *
 * Builds a 32-byte AES-128-ECB encrypted `0xA2` state-change report, saves the
 * 17-byte plaintext in `g_sensorCtx.lastTxPlaintext` (used by `Sensor_dataCnfCb`
 * to construct a 12-byte `0xAD` history record if MAC ACK + both retries fail)
 * and the 32-byte ciphertext in `g_sensorCtx.lastTxCiphertext` (used for the
 * 80 ms and 130 ms retries), transmits via `Sensor_sendMsg`, and updates the
 * current timestamp (`epochMsBase + elapsedMs`) for potential offline logging.
 */
void Sensor_sendEvent_A2(uint8_t stateByte, void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]))
{
    StockReportA1A2_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.cmd = 0xA2u;
    memcpy(pkt.mac, g_identCtx.macAddress, 8);
    pkt.sensorType = 0x01u; /* 0x01 on contact_v1 (0x0c26c), 0x02 on motion_v1 (0x0bdd8) */
    pkt.tempDegC = g_sensorCtx.lastTempDegC;
    pkt.batV32 = g_sensorCtx.lastTxBatV32;
    pkt.lowBatLatch = g_sensorCtx.nv11LowBatLatch;
    pkt.dataLenLo = 0x01u;
    pkt.state = stateByte;
    pkt.seqNumBe = (uint16_t)((g_sensorCtx.seqNum >> 8) | (g_sensorCtx.seqNum << 8));

    memset(g_sensorCtx.lastTxPlaintext, 0, sizeof(g_sensorCtx.lastTxPlaintext));
    memcpy(g_sensorCtx.lastTxPlaintext, &pkt, 17u);
    Stock_aes128_ecb_encryptBuffer((const uint8_t *)&pkt, sizeof(pkt), g_sensorCtx.lastTxCiphertext, aes128EcbBlock16);
}

/*
 * Sensor_sendPirSummary_AB() (`motion_v1` only)
 * Flash address:
 *   - motion_v1 : 0x0000cedc (10s window clock at 0x12578, 120s summary clock at 0x125ac)
 *
 * Transmitted every 120 seconds (`0x1D4C0` ms) while motion is active (`event 0x0008`).
 * Copies the twelve 10-second PIR pulse buckets (`g_sensorCtx.pirBuckets[0..11]`) into
 * bytes `10..21` of a 32-byte `0xAB` packet, encrypts with `AES_encryptBuffer` (`0x0c7c0`),
 * and transmits via `Sensor_sendMsg` (`0x0ae90`).
 */
void Sensor_sendPirSummary_AB(void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]))
{
    StockMotionSummaryAB_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.cmd = 0xABu;
    memcpy(pkt.mac, g_identCtx.macAddress, 8);
    pkt.sensorType = 0x02u;
    memcpy(pkt.pirBuckets, g_sensorCtx.pirBuckets, 12);

    memset(g_sensorCtx.lastTxPlaintext, 0, sizeof(g_sensorCtx.lastTxPlaintext));
    memcpy(g_sensorCtx.lastTxPlaintext, &pkt, 22u);
    Stock_aes128_ecb_encryptBuffer((const uint8_t *)&pkt, sizeof(pkt), g_sensorCtx.lastTxCiphertext, aes128EcbBlock16);
}

/*
 * Sensor_sendHistoryLog_AD()
 * Flash addresses:
 *   - contact_v1: 0x00008280
 *   - motion_v1 : 0x00007fa8
 *
 * Drains one 12-byte offline event record from the 50-entry (600-byte = `0x258`)
 * circular buffer `g_historyLog`:
 *   1. If `g_sensorCtx.historyCount == 0` (`0x0828e`), zeroes `g_historyLog` (`0x258` bytes),
 *      sets `historyEmptyFlag = 1`, resets `historyWriteOffset = 0` and `historyReadOffset = 0`,
 *      and restarts the 200 ms timer one last time (`0x08290..0x082a8`).
 *   2. Otherwise, reads the 12-byte entry at `historyReadOffset` and advances
 *      `historyReadOffset = (historyReadOffset + 12 > 588) ? 0 : (historyReadOffset + 12)`.
 *   3. Checks if the 64-bit big-endian timestamp in `entry.timestampBe[0..7]` equals
 *      `historyLastTimestampMs` (`0x0834e..0x08390`): if two consecutive history records
 *      share the exact same millisecond timestamp, increments `historyTiebreaker` (`+1 ms`,
 *      `+2 ms`, ...) and adds it to the 64-bit timestamp so every `0xAD` record has a
 *      strictly unique timestamp!
 *   4. Builds the 32-byte `0xAD` packet (`[0]=0xAD, [1..8]=mac, [9]=sensorType, [10..21]=entry`),
 *      encrypts with `AES_encryptBuffer`, sends via `Sensor_sendMsg`, decrements `historyCount`,
 *      and schedules the next `0xAD` transmission in `200 ms` (`0xC8` at `0x083da -> 0x12258`).
 */
void Sensor_sendHistoryLog_AD(void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]),
                              void (*startHistoryTimerMs)(uint32_t ms))
{
    if (g_sensorCtx.historyCount == 0u) {
        memset(g_historyLog, 0, sizeof(g_historyLog));
        g_sensorCtx.historyEmptyFlag = 1u;
        g_sensorCtx.historyWriteOffset = 0u;
        g_sensorCtx.historyReadOffset = 0u;
        if (startHistoryTimerMs) {
            startHistoryTimerMs(200u);
        }
        return;
    }

    uint8_t plain[32];
    memset(plain, 0, sizeof(plain));
    plain[0] = 0xADu;
    memcpy(&plain[1], g_identCtx.macAddress, 8);
    plain[9] = 0x01u; /* 0x01 on contact_v1 (0x082d0), 0x02 on motion_v1 */

    uint16_t idx = (uint16_t)(g_sensorCtx.historyReadOffset / sizeof(StockHistoryEntry_t));
    memcpy(&plain[10], &g_historyLog[idx], sizeof(StockHistoryEntry_t));

    uint16_t nextOff = (uint16_t)(g_sensorCtx.historyReadOffset + sizeof(StockHistoryEntry_t));
    if (nextOff > 0x024Cu) { /* 588 = 49 * 12 */
        nextOff = 0u;
    }
    g_sensorCtx.historyReadOffset = nextOff;

    /* 0x082fe..0x08390: 64-bit big-endian timestamp tiebreaker check */
    uint64_t ts = ((uint64_t)plain[10] << 56) | ((uint64_t)plain[11] << 48) | ((uint64_t)plain[12] << 40) |
                  ((uint64_t)plain[13] << 32) | ((uint64_t)plain[14] << 24) | ((uint64_t)plain[15] << 16) |
                  ((uint64_t)plain[16] << 8) | (uint64_t)plain[17];

    if (ts == g_sensorCtx.historyLastTimestampMs) {
        g_sensorCtx.historyTiebreaker++;
        uint64_t adjusted = ts + g_sensorCtx.historyTiebreaker;
        plain[10] = (uint8_t)(adjusted >> 56);
        plain[11] = (uint8_t)(adjusted >> 48);
        plain[12] = (uint8_t)(adjusted >> 40);
        plain[13] = (uint8_t)(adjusted >> 32);
        plain[14] = (uint8_t)(adjusted >> 24);
        plain[15] = (uint8_t)(adjusted >> 16);
        plain[16] = (uint8_t)(adjusted >> 8);
        plain[17] = (uint8_t)(adjusted & 0xFFu);
    } else {
        g_sensorCtx.historyTiebreaker = 0u;
    }
    g_sensorCtx.historyLastTimestampMs = ts;

    memset(g_sensorCtx.lastTxPlaintext, 0, sizeof(g_sensorCtx.lastTxPlaintext));
    memcpy(g_sensorCtx.lastTxPlaintext, plain, 22u);
    Stock_aes128_ecb_encryptBuffer(plain, sizeof(plain), g_sensorCtx.lastTxCiphertext, aes128EcbBlock16);

    g_sensorCtx.historyCount--;
    if (startHistoryTimerMs) {
        startHistoryTimerMs(200u); /* 0xC8 = 200 ms between 0xAD packets */
    }
}

/*
 * Sensor_sendFactoryTestEcho_A9()
 * Flash addresses:
 *   - contact_v1: 0x0000ec74
 *   - motion_v1 : 0x0000ee60
 *
 * Called during the 50-packet RF Factory Link-Quality / PER Test (`0xAA`) for
 * packets `1..49`. Copies bytes `1..29` from the incoming `0xAA` packet, sets
 * `plain[0] = 0xA9`, writes `g_sensorCtx.rfTestLastRxRssi` at `plain[30]`, writes
 * `sum8(plain[0..30])` at `plain[31]`, encrypts 32 bytes with `AES_encryptBuffer`,
 * and transmits to the coordinator via `Sensor_sendMsg`.
 */
void Sensor_sendFactoryTestEcho_A9(const uint8_t rxPlain[32], int8_t rxRssi,
                                   void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]))
{
    uint8_t plain[32];
    memcpy(plain, rxPlain, sizeof(plain));
    plain[0] = 0xA9u;
    plain[30] = (uint8_t)rxRssi;

    uint8_t sum = 0;
    for (int i = 0; i < 31; i++) {
        sum = (uint8_t)(sum + plain[i]);
    }
    plain[31] = sum;

    Stock_aes128_ecb_encryptBuffer(plain, sizeof(plain), g_sensorCtx.lastTxCiphertext, aes128EcbBlock16);
}

/*
 * Sensor_sendDecommAck_B1() (`motion_v1` only)
 * Flash address:
 *   - motion_v1 : 0x000122a4 (called from handleDownlinkRf at 0x00004672)
 *
 * When `motion_v1` receives a `0xA6` packet with `pt[1] == 0xFF && pt[2] == 0x05`
 * (Decommission / Unpair command from gateway), it builds a 16-byte `0xB1` ACK
 * (`plain[0] = 0xB1`, `plain[1] = pt[3]`), encrypts it with `AES_encryptBuffer`
 * (`0x0c7c0`), and transmits it before calling `Ssf_factoryResetNv()`.
 */
void Sensor_sendDecommAck_B1(uint8_t subCode, void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]))
{
    uint8_t plain[32];
    memset(plain, 0, sizeof(plain));
    plain[0] = 0xB1u;
    plain[1] = subCode;

    Stock_aes128_ecb_encryptBuffer(plain, 16u, g_sensorCtx.lastTxCiphertext, aes128EcbBlock16);
}

/*
 * Sensor_dataCnfCb()
 * Flash addresses:
 *   - contact_v1: 0x000057e0
 *   - motion_v1 : 0x00005820
 *
 * 802.15.4 MAC `McpsDataCnf` callback invoked after every `Sensor_sendMsg()`:
 *   1. If `status == 0` (`ApiMac_status_success`, MAC ACK received from gateway):
 *      - Resets `txFailRetryStage = 0`.
 *      - Persists `frameCounter` to NVOCTP item `(6, 4, 0)` every 25 frames (`0x05804..0x05828`).
 *      - If `lastTxPlaintext[0] == 0xA2` and `historyEmptyFlag == 0` (offline `0xAD` records
 *        are waiting in `g_historyLog`), starts a **200 ms (`0xC8`) timer** (`0x12258`) to
 *        post `SENSOR_HISTORY_EVT` (`0x0004`) and drain `g_historyLog` via `0xAD`!
 *   2. If `status != 0` (No MAC ACK from gateway):
 *      - Sets `historyEmptyFlag = 0`.
 *      - Stage 0 (`txFailRetryStage == 0`): starts an **80 ms (`0x50`)** retry timer (`0x123f8`)
 *        to post `SENSOR_A2_RETRY_EVT` (`0x0002`) for retry #1.
 *      - Stage 1 (`txFailRetryStage == 1`): starts a **130 ms (`0x82`)** retry timer (`0x123f8`)
 *        to post `SENSOR_A2_RETRY_EVT` (`0x0002`) for retry #2.
 *      - Stage 2 (`txFailRetryStage >= 2`, all 3 attempts failed):
 *        Resets `txFailRetryStage = 0`. If `lastTxPlaintext[0] == 0xA2`, pushes a 12-byte
 *        record (`{epochMsBe[8], 0xA2, 0x01, state, seqLo}`) into `g_historyLog` at
 *        `historyWriteOffset` (up to 50 entries) and zeroes `lastTxPlaintext`!
 */
void Sensor_dataCnfCb(uint8_t macStatus, uint32_t frameCntr, void (*startRetryTimerMs)(uint32_t ms),
                      void (*startHistoryTimerMs)(uint32_t ms))
{
    if (macStatus == 0u) {
        /* 0x057f2: MAC ACK received */
        g_sensorCtx.txFailRetryStage = 0u;
        if ((frameCntr - g_sensorCtx.lastSavedSeqNum) >= 25u && g_pNv && g_pNv->updateItem) {
            NVINTF_itemID_t id4 = {NVINTF_SYSID_APP, SENSOR_NV_SEQ_COUNTER, 0};
            if (g_pNv->updateItem(id4, sizeof(uint32_t), &frameCntr) == 0) {
                g_sensorCtx.lastSavedSeqNum = frameCntr;
            }
        }
        /* 0x0582a..0x05844: If an 0xA2 report succeeded and offline history exists, drain in 200 ms */
        if (g_sensorCtx.lastTxPlaintext[0] == 0xA2u && g_sensorCtx.historyEmptyFlag == 0u) {
            if (startHistoryTimerMs) {
                startHistoryTimerMs(200u);
            }
        }
        return;
    }

    /* 0x05846: MAC transmission failed (no ACK) */
    g_sensorCtx.historyEmptyFlag = 0u;
    if (g_sensorCtx.txFailRetryStage == 0u) {
        /* 0x059a2: Schedule Retry #1 in 80 ms (0x50) */
        if (startRetryTimerMs) {
            startRetryTimerMs(80u);
        }
        return;
    }
    if (g_sensorCtx.txFailRetryStage == 1u) {
        /* 0x0599a: Schedule Retry #2 in 130 ms (0x82) */
        if (startRetryTimerMs) {
            startRetryTimerMs(130u);
        }
        return;
    }

    /* 0x05858..0x05998: Both retries failed -> push un-ACKed 0xA2 event to 50-entry g_historyLog */
    g_sensorCtx.txFailRetryStage = 0u;
    if (g_sensorCtx.lastTxPlaintext[0] == 0xA2u) {
        if (g_sensorCtx.historyCount < 50u) {
            g_sensorCtx.historyCount++;
        }
        if (g_sensorCtx.historyWriteOffset >= 0x0258u) { /* Wrap at 600 bytes (50 * 12) */
            g_sensorCtx.historyWriteOffset = 0u;
        }
        uint16_t idx = (uint16_t)(g_sensorCtx.historyWriteOffset / sizeof(StockHistoryEntry_t));
        StockHistoryEntry_t *dst = &g_historyLog[idx];
        uint64_t ts = g_sensorCtx.epochMsBase;
        dst->timestampBe[0] = (uint8_t)(ts >> 56);
        dst->timestampBe[1] = (uint8_t)(ts >> 48);
        dst->timestampBe[2] = (uint8_t)(ts >> 40);
        dst->timestampBe[3] = (uint8_t)(ts >> 32);
        dst->timestampBe[4] = (uint8_t)(ts >> 24);
        dst->timestampBe[5] = (uint8_t)(ts >> 16);
        dst->timestampBe[6] = (uint8_t)(ts >> 8);
        dst->timestampBe[7] = (uint8_t)(ts & 0xFFu);
        dst->eventType = g_sensorCtx.lastTxPlaintext[0];   /* 0xA2 */
        dst->dataMarker = g_sensorCtx.lastTxPlaintext[14]; /* 0x01 */
        dst->state = g_sensorCtx.lastTxPlaintext[15];      /* state (0 or 1) */
        dst->seqNumLo = g_sensorCtx.lastTxPlaintext[16];   /* seqNum low byte */

        g_sensorCtx.historyWriteOffset = (uint16_t)(g_sensorCtx.historyWriteOffset + sizeof(StockHistoryEntry_t));
        memset(g_sensorCtx.lastTxPlaintext, 0, sizeof(g_sensorCtx.lastTxPlaintext));
    }
}

/* ==========================================================================
 * 6. Join Handshake (`0xA3 -> 0xA4 -> 0xA5 -> 0xA6`) & Downlink RF Handler
 * ========================================================================== */

/*
 * Sensor_sendJoinReq_A3()
 * Flash addresses:
 *   - contact_v1: 0x00001714..0x00001786 (inlined in Sensor_process event 0x2000) / 0x0000da30
 *   - motion_v1 : 0x00001738..0x000017b8 (inlined in Sensor_process event 0x2000) / 0x0000d968
 *
 * Sent every 3,000 ms (`0xBB8`) up to 8 times per channel scan (polling every 300 ms).
 */
void Sensor_sendJoinReq_A3(void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]))
{
    StockJoinReqA3_t pkt;
    memset(&pkt, 0, sizeof(pkt));

    pkt.cmd = 0xA3u;
    memcpy(pkt.mac, g_identCtx.macAddress, 8);
    pkt.sensorType = 0x01u; /* 0x01 contact_v1, 0x02 motion_v1 */
    pkt.fwVersion = 0x16u;  /* 0x16 contact_v1, 0x19 motion_v1 (0x01766) */
    pkt.bootBatV32 = g_sensorCtx.bootBatV32;
    pkt.keyValidFlag = g_sensorCtx.nv8KeyValidFlag;
    pkt.facTestFlag = g_sensorCtx.nv10FacTestFlag;
    pkt.resetCauseFlag = g_sensorCtx.lowBatBootFlag;
    pkt.pad = g_sensorCtx.nv11LowBatLatch;

    Stock_aes128_ecb_encryptBuffer((const uint8_t *)&pkt, sizeof(pkt), g_sensorCtx.lastTxCiphertext, aes128EcbBlock16);
}

/*
 * Sensor_sendJoinResp_A5()
 * Flash addresses:
 *   - contact_v1: 0x0000ea84 (called from Sensor_process event 0x2000 at 0x016ea)
 *   - motion_v1 : 0x0000f764 (called from Sensor_process event 0x2000 at 0x0170c)
 *
 * Encrypts the 16-byte challenge token from `0xA4` in-place with 19-round XXTEA
 * (`Stock_xxtea19_encrypt`: contact_v1 `0x07f98`, motion_v1 `0x0842c`) using
 * `g_identCtx.xxteaKey`, places it at offset 9..24 of `0xA5`, and encrypts the
 * 32-byte packet with `Stock_aes128_ecb_encryptBuffer` (`0x0c5f0` / `0x0c7c0`).
 * Retried every 5,000 ms (`0x1388`) up to 8 times.
 */
void Sensor_sendJoinResp_A5(bool isMotionSensor, void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]))
{
    uint8_t a5Plain[32];
    uint8_t xxteaBlock[16];

    if (!isMotionSensor) {
        /* 0x0ea90: present in contact_v1, omitted in motion_v1 (0x0f764)! */
        memset(a5Plain, 0, sizeof(a5Plain));
    }

    memcpy(xxteaBlock, g_identCtx.joinChallengeR, 16);
    Stock_xxtea19_encrypt(xxteaBlock, g_identCtx.xxteaKey);

    a5Plain[0] = 0xA5u;
    memcpy(&a5Plain[1], g_identCtx.macAddress, 8);
    memcpy(&a5Plain[9], xxteaBlock, 16);
    a5Plain[25] = isMotionSensor ? 0x02u : 0x01u; /* 0x01 contact_v1, 0x02 motion_v1 */
    a5Plain[26] = isMotionSensor ? 0x19u : 0x16u; /* 0x16 contact_v1, 0x19 motion_v1 */

    Stock_aes128_ecb_encryptBuffer(a5Plain, sizeof(a5Plain), g_sensorCtx.lastTxCiphertext, aes128EcbBlock16);
}

/*
 * Sensor_dataIndCb()
 * Flash addresses:
 *   - contact_v1: 0x00003d28..0x0000407c (calls AES_decryptBuffer at 0x086bc -> 0x200018bc)
 *   - motion_v1 : 0x00006414 (decrypt wrapper) -> 0x00004368..0x000046a2 (handleDownlinkRf)
 *
 * Decrypts incoming 802.15.4 frames from the coordinator via `AES_decryptBuffer`
 * and dispatches all four downlink RF opcodes (`0xB0`, `0xA4`, `0xA6`, `0xAA`):
 *   - `0xB0` (`0x03d90` / `0x043c0`): Stops the poll timer (`0x123c4(0)` / `0x1274c(0)`),
 *     parses `pt[1..8]` as a 64-bit big-endian ms timestamp into `epochMsBase`, and records
 *     the current RTC tick (`Clock_getTicks()`).
 *   - `0xA4` (`0x0405a` / `0x04680`): Copies 16-byte challenge `pt[1..16]` into
 *     `g_identCtx.joinChallengeR`, sets `joinState = 1`, `a5RetriesLeft = 8`,
 *     `pollRetriesLeft = 60` (`0x3C`), and starts a 100 ms (`0x64`) timer (`0x12764` / `0x12ab8`)
 *     to trigger `Sensor_sendJoinResp_A5()`.
 *   - `0xA6` (`0x03f0c` / `0x04542`):
 *     * If `pt[1] == 0xFF && pt[2] == 0x04` (Join Approved):
 *       Writes `1` to NVOCTP item `(6, 9, 0)` (`SENSOR_NV_JOINED_FLAG`), sets
 *       `heartbeat5MinTicks = 47` (`0x2F`) and starts the 5-minute heartbeat clock with
 *       10 polls (`0x0A`) so the first `0xA1` heartbeat is sent after 5 minutes (and every
 *       4 hours thereafter), samples battery voltage, on `contact_v1` immediately reads
 *       `DIO7`, increments `seqNum`, and sends an initial `0xA2` event (`0x03fe0..0x03ff6`),
 *       and syncs `epochMsBase` from `pt[3..10]`.
 *     * If `pt[1] == 0xFF && pt[2] == 0x05` (Decommission / Unpair):
 *       Stops the poll timer; if `pt[3] == 0x01`, writes `1` to NVOCTP item `(6, 11, 0)`
 *       (`SENSOR_NV_LOWBAT_LATCH`); on `motion_v1` sends a 16-byte `0xB1` ACK via
 *       `Sensor_sendDecommAck_B1(pt[3])` (`0x122a4`); and calls `Ssf_factoryResetNv()`.
 *   - `0xAA` (`0x03e38` / `0x04478`):
 *     * 50-packet RF Factory Link-Quality / PER Test: verifies `sum8(pt[0..30]) == pt[31]`,
 *       increments `rfTestRxTotal` (`1..50`), `rfTestRxGood` / `rfTestRxBad`, accumulates
 *       RSSI, calls `Sensor_sendFactoryTestEcho_A9()` for packets `1..49`, and on packet 50
 *       computes `avgRssi = rfTestRssiSum / 50` and (if `factoryTestActive != 0`) sends
 *       `AA 55 53 06 2C <rxGood> <rxBad> <avgRssi> <csum>` (`0x53 0x2C`) over UART!
 */
void Sensor_dataIndCb(const uint8_t *pt, uint16_t len, int8_t rxRssi, bool isMotionSensor,
                      uint32_t (*readDio)(uint32_t dio),
                      void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]),
                      void (*setPollTimerMs)(uint32_t ms), void (*startJoinTxTimerMs)(uint32_t ms),
                      void (*uartWrite)(const uint8_t *buf, uint32_t len))
{
    if (len < 16u) {
        return;
    }
    uint8_t opcode = pt[0];

    if (opcode == 0xB0u) {
        /* 0x03d90 / 0x043c0: Time Sync ACK */
        if (setPollTimerMs) {
            setPollTimerMs(0u);
        }
        g_sensorCtx.pollRetriesLeft = 0u;
        g_sensorCtx.txInProgress = 0u;
        g_sensorCtx.unackedReportCount = 0u;
        g_sensorCtx.epochMsBase = ((uint64_t)pt[1] << 56) | ((uint64_t)pt[2] << 48) | ((uint64_t)pt[3] << 40) |
                                  ((uint64_t)pt[4] << 32) | ((uint64_t)pt[5] << 24) | ((uint64_t)pt[6] << 16) |
                                  ((uint64_t)pt[7] << 8) | (uint64_t)pt[8];
    } else if (opcode == 0xA4u) {
        /* 0x0405a / 0x04680: Join Challenge Token */
        memcpy(g_identCtx.joinChallengeR, &pt[1], 16);
        g_sensorCtx.joinState = 1u;
        g_sensorCtx.a5RetriesLeft = 8u;
        g_sensorCtx.pollRetriesLeft = 60u; /* 0x3C */
        if (startJoinTxTimerMs) {
            startJoinTxTimerMs(100u); /* 0x64 = 100 ms -> event 0x2000 */
        }
    } else if (opcode == 0xA6u) {
        /* 0x03f0c / 0x04542: Join Complete (0x04) or Decommission (0x05) */
        g_sensorCtx.a5RetriesLeft = 0u;
        if (startJoinTxTimerMs) {
            startJoinTxTimerMs(0u);
        }
        if (pt[1] != 0xFFu) {
            return;
        }
        if (pt[2] == 0x04u) {
            /* 0x03f90 / 0x0457a: Join Approved */
            g_sensorCtx.joinedNvFlag = 1u;
            g_sensorCtx.joinState = 0u;
            if (g_pNv && g_pNv->updateItem) {
                NVINTF_itemID_t id9 = {NVINTF_SYSID_APP, SENSOR_NV_JOINED_FLAG, 0};
                g_pNv->updateItem(id9, 1, &g_sensorCtx.joinedNvFlag);
            }
            /* 0x03fcc: Set heartbeat5MinTicks = 47 (0x2F) so first 0xA1 fires in 5 minutes! */
            g_sensorCtx.heartbeat5MinTicks = 47u;
            g_sensorCtx.pollRetriesLeft = 10u;

            if (!isMotionSensor && readDio) {
                /* 0x03fe0..0x03ff6: contact_v1 immediately sends initial 0xA2 state report */
                uint8_t reedState = (readDio(CONTACT_PIN_REED) != 0u) ? 1u : 0u;
                g_sensorCtx.currentReedOrPirState = reedState;
                g_sensorCtx.lastSentReedState = reedState;
                g_sensorCtx.seqNum++;
                Sensor_sendEvent_A2(reedState, aes128EcbBlock16);
            }

            /* 0x0401a..0x04050: Sync 64-bit ms timestamp from pt[3..10] */
            g_sensorCtx.epochMsBase = ((uint64_t)pt[3] << 56) | ((uint64_t)pt[4] << 48) | ((uint64_t)pt[5] << 40) |
                                      ((uint64_t)pt[6] << 32) | ((uint64_t)pt[7] << 24) | ((uint64_t)pt[8] << 16) |
                                      ((uint64_t)pt[9] << 8) | (uint64_t)pt[10];
        } else if (pt[2] == 0x05u) {
            /* 0x03f34 / 0x04624: Decommission / Unpair command from gateway */
            if (setPollTimerMs) {
                setPollTimerMs(0u);
            }
            if (pt[3] == 0x01u) {
                g_sensorCtx.nv11LowBatLatch = 1u;
                if (g_pNv && g_pNv->updateItem) {
                    NVINTF_itemID_t id11 = {NVINTF_SYSID_APP, SENSOR_NV_LOWBAT_LATCH, 0};
                    g_pNv->updateItem(id11, 1, &g_sensorCtx.nv11LowBatLatch);
                }
            }
            if (isMotionSensor) {
                /* 0x04672: motion_v1 sends 16-byte encrypted 0xB1 ACK before wiping NV */
                Sensor_sendDecommAck_B1(pt[3], aes128EcbBlock16);
            }
            Ssf_factoryResetNv();
        }
    } else if (opcode == 0xAAu && len >= 32u) {
        /* 0x03e38 / 0x04478: 50-packet RF Factory Link-Quality / PER Test */
        g_sensorCtx.rfTestLastRxRssi = rxRssi;
        g_sensorCtx.rfTestRxTotal++;
        g_sensorCtx.rfTestRssiSum += rxRssi;

        uint8_t sum = 0;
        for (int i = 0; i < 31; i++) {
            sum = (uint8_t)(sum + pt[i]);
        }
        if (sum == pt[31]) {
            g_sensorCtx.rfTestRxGood++;
        } else {
            g_sensorCtx.rfTestRxBad++;
        }

        if (g_sensorCtx.rfTestRxTotal < 50u) {
            /* Packets 1..49: echo back 32-byte 0xA9 with rxRssi at byte 30 */
            Sensor_sendFactoryTestEcho_A9(pt, rxRssi, aes128EcbBlock16);
        } else {
            /* Packet 50 (0x32): compute average RSSI and report 0x53 0x2C on UART if in factory mode */
            if (setPollTimerMs) {
                setPollTimerMs(0u);
            }
            uint8_t avgRssi = (uint8_t)(g_sensorCtx.rfTestRssiSum / 50);
            if (g_sensorCtx.factoryTestActive != 0u && uartWrite) {
                uint8_t tx[10];
                tx[0] = 0xAAu;
                tx[1] = 0x55u;
                tx[2] = 0x53u;
                tx[3] = 0x06u;
                tx[4] = 0x2Cu;
                tx[5] = g_sensorCtx.rfTestRxGood;
                tx[6] = g_sensorCtx.rfTestRxBad;
                tx[7] = avgRssi;
                uint16_t csum = 0;
                for (int i = 0; i < 8; i++) {
                    csum = (uint16_t)(csum + tx[i]);
                }
                tx[8] = (uint8_t)(csum >> 8);
                tx[9] = (uint8_t)(csum & 0xFFu);
                uartWrite(tx, sizeof(tx));
            }
            g_sensorCtx.rfTestRxTotal = 0u;
            g_sensorCtx.rfTestRxGood = 0u;
            g_sensorCtx.rfTestRxBad = 0u;
            g_sensorCtx.rfTestRssiSum = 0;
        }
    }
}

/* ==========================================================================
 * 7. Main Application Event Loop (`Sensor_process`)
 * ========================================================================== */

/*
 * Sensor_process()
 * Flash addresses:
 *   - contact_v1: 0x00001658..0x00001db0
 *   - motion_v1 : 0x00001658..0x00001f12
 *
 * Processes all application event bits in `g_sensorCtx.sensorEvents`:
 *   - `0x0001` (`SENSOR_START_EVT`): Starts Jdllc join / re-join and arms `a5RetriesLeft = 8` (contact)
 *     or `10` (motion).
 *   - `0x0002` (`SENSOR_A2_RETRY_EVT`): Re-transmits `g_sensorCtx.lastTxCiphertext` up to 2 times
 *     (`txFailRetryStage < 2`, triggered after 80 ms and 130 ms by `Sensor_dataCnfCb`).
 *   - `0x0004`:
 *       * `contact_v1` (`0x01c66`): `SENSOR_HISTORY_EVT` — calls `Sensor_sendHistoryLog_AD()`.
 *       * `motion_v1` (`0x0190a`): `SENSOR_PIR_10S_EVT` — restarts 10,000 ms (`0x2710`) bucket timer,
 *         stores `pirPulseCount10s` into `pirBuckets[pirWindowIndex]`, resets `pirPulseCount10s = 0`,
 *         and advances `pirWindowIndex = (pirWindowIndex + 1) % 12`.
 *   - `0x0008` (`motion_v1` `0x0193e`): `SENSOR_PIR_120S_SUMMARY_EVT` — restarts 120,000 ms (`0x1D4C0`)
 *     timer and calls `Sensor_sendPirSummary_AB()`.
 *   - `0x0010`: Button falling-edge interrupt — enables debounce and starts 10 ms button clock.
 *   - `0x0020` / `0x0040`:
 *       * `contact_v1` (`0x01796`): Reed switch `DIO7` edge interrupt — starts reed debounce clock
 *         (`0x127f4`, which posts `0x4000`).
 *       * `motion_v1` (`0x017ca`, `0x01802`): PIR edge interrupt — if `gpioSelfTestActive`, increments
 *         `gpioSelfTestTriggerCount` and blinks LED `1x 150 ms`; if joined, increments `pirPulseCount10s`
 *         and starts 50 ms (`0x32`) PIR settle timer (`0x12b18`, which posts `0x0800`).
 *   - `0x0080`: 10 ms button clock tick (`Ssf_buttonClockCb` + `Sensor_handleKeys`).
 *   - `0x0100`: 5-minute (`300,000 ms` = `0x493E0`) heartbeat clock tick — after 48 ticks (4 hours),
 *     reads live GPIO (`DIO7` on contact_v1, `DIO9 | DIO10` on motion_v1), calls `Sensor_sendHeartbeat_A1`,
 *     and starts DataReq polling (`700 ms` x 10 on contact_v1, `800 ms` x 5 on motion_v1).
 *   - `0x0200`: Factory UART frame received (`FactoryTest_process43` or `FactoryTest_process53`).
 *   - `0x0400`: LED blink clock tick (`Ssf_ledClockCb` — toggles LED GPIO and decrements `ledToggleEdgesLeft`).
 *   - `0x0800` (`motion_v1` `0x01840`): PIR 50 ms settle complete — if `pirMotionIdleArmed != 0`,
 *     increments `seqNum`, blinks LED `1x 300 ms` (if `pirLedTestArm`), calls `Sensor_sendEvent_A2(1)`,
 *     clears `pirMotionIdleArmed = 0`, and starts the 10s bucket timer and 120s motion-clear timer.
 *   - `0x1000` (`motion_v1` `0x018de`): PIR 120s motion-clear timeout — increments `seqNum`, calls
 *     `Sensor_sendEvent_A2(0)`, stops the 10s bucket timer, and re-arms `pirMotionIdleArmed = 1`.
 *   - `0x2000` (`contact_v1` `0x016ba`, `motion_v1` `0x016da`): Join `0xA3` / `0xA5` transmit timer —
 *     sends `0xA5` (every 5,000 ms, up to 8 retries) if `joinState == 1`, or `0xA3` (every 3,000 ms,
 *     up to 8/10 retries) if `joinState == 0`.
 *   - `0x4000` (`contact_v1` `0x01bea`): Reed switch debounce sample tick — reads `DIO7`, counts
 *     consecutive high (`reedHighCount`) or low (`reedLowCount`) samples until `> 4` (5 consecutive
 *     samples), and if the debounced state differs from `lastSentReedState`, increments `seqNum`,
 *     calls `Sensor_sendEvent_A2(reedState)`, and updates `lastSentReedState`.
 *   - `0x8000` (`motion_v1` `0x01e78`): 20-second PIR Hardware Self-Test timeout (`0x53 0xD2` completion) —
 *     sends `AA 55 53 04 D3 <gpioSelfTestTriggerCount> <csum>` (`0x53 0xD3`) over UART and clears
 *     `gpioSelfTestActive = 0`.
 */
void Sensor_process(bool isMotionSensor, uint32_t (*readDio)(uint32_t dio),
                    void (*aes128EcbBlock16)(const uint8_t in[16], uint8_t out[16]),
                    void (*sendRawCiphertext)(const uint8_t *ct, uint16_t len),
                    void (*programFlash)(const uint8_t *src, uint32_t dstAddr, uint32_t len),
                    void (*uartWrite)(const uint8_t *buf, uint32_t len), void (*setPollTimerMs)(uint32_t ms),
                    void (*startJoinTxTimerMs)(uint32_t ms), void (*startReedDebounceTimer)(void),
                    void (*startPirSettleTimerMs)(uint32_t ms), void (*setPir10sTimerMs)(uint32_t ms),
                    void (*setPir120sTimerMs)(uint32_t ms), void (*startHistoryTimerMs)(uint32_t ms))
{
    uint16_t ev = g_sensorCtx.sensorEvents;

    /* 1. 0x0001: SENSOR_START_EVT (0x01658) */
    if (ev & 0x0001u) {
        g_sensorCtx.a5RetriesLeft = isMotionSensor ? 10u : 8u;
        g_sensorCtx.sensorEvents &= (uint16_t)~0x0001u;
    }

    /* 2. 0x0002: SENSOR_A2_RETRY_EVT (0x01680 / 0x01686) */
    if (ev & 0x0002u) {
        if (g_sensorCtx.txFailRetryStage < 2u) {
            g_sensorCtx.txFailRetryStage++;
            if (sendRawCiphertext) {
                sendRawCiphertext(g_sensorCtx.lastTxCiphertext, 32u);
            }
        }
        g_sensorCtx.sensorEvents &= (uint16_t)~0x0002u;
    }

    /* 3. 0x2000: Join 0xA3 / 0xA5 periodic transmit timer (0x016ba / 0x016da) */
    if (ev & 0x2000u) {
        if (g_sensorCtx.joinedNvFlag == 0u && g_sensorCtx.a5RetriesLeft > 0u) {
            if (g_sensorCtx.joinState == 1u) {
                /* 0x016d6 / 0x016f6: Send 0xA5 Join Response and retry in 5,000 ms (0x1388) */
                if (startJoinTxTimerMs) {
                    startJoinTxTimerMs(5000u);
                }
                Sensor_sendJoinResp_A5(isMotionSensor, aes128EcbBlock16);
            } else {
                /* 0x01714 / 0x01738: Send 0xA3 Join Request and retry in 3,000 ms (0xBB8) */
                if (startJoinTxTimerMs) {
                    startJoinTxTimerMs(3000u);
                }
                Sensor_sendJoinReq_A3(aes128EcbBlock16);
            }
            g_sensorCtx.a5RetriesLeft--;
            if (g_sensorCtx.a5RetriesLeft == 0u && setPollTimerMs) {
                setPollTimerMs(0u);
            }
        }
        g_sensorCtx.sensorEvents &= (uint16_t)~0x2000u;
    }

    /* 4. 0x0020 / 0x0040: Reed switch edge (contact_v1) or PIR pulse edge (motion_v1) */
    if (ev & (0x0020u | 0x0040u)) {
        if (!isMotionSensor) {
            /* 0x01796: contact_v1 starts reed switch debounce timer (posts 0x4000) */
            g_sensorCtx.reedHighCount = 0u;
            g_sensorCtx.reedLowCount = 0u;
            if (startReedDebounceTimer) {
                startReedDebounceTimer();
            }
        } else {
            /* 0x017ca / 0x01802: motion_v1 PIR edge */
            if (g_sensorCtx.gpioSelfTestActive != 0u) {
                g_sensorCtx.gpioSelfTestTriggerCount++;
                Ssf_ledBlink(150, 1); /* 0x96 = 150 ms */
            }
            if (g_sensorCtx.joinedNvFlag != 0u) {
                g_sensorCtx.pirPulseCount10s++;
                if (startPirSettleTimerMs) {
                    startPirSettleTimerMs(50u); /* 0x32 = 50 ms -> posts 0x0800 */
                }
            }
        }
        g_sensorCtx.sensorEvents &= (uint16_t)~(0x0020u | 0x0040u);
    }

    /* 5. 0x0800: motion_v1 PIR 50 ms settle complete (0x01840) */
    if (isMotionSensor && (ev & 0x0800u)) {
        if (g_sensorCtx.pirMotionIdleArmed != 0u && g_sensorCtx.joinedNvFlag != 0u) {
            g_sensorCtx.seqNum++;
            if (g_sensorCtx.pirLedTestArm != 0u) {
                Ssf_ledBlink(300, 1);
                g_sensorCtx.pirLedTestArm = 0u;
            }
            Sensor_sendEvent_A2(1u, aes128EcbBlock16);
            g_sensorCtx.pirMotionIdleArmed = 0u;
        }
        if (g_sensorCtx.pir10sTimerRestartFlag != 0u && g_sensorCtx.joinedNvFlag != 0u) {
            g_sensorCtx.pir10sTimerRestartFlag = 0u;
            if (setPir10sTimerMs) {
                setPir10sTimerMs(10000u); /* 0x2710 = 10,000 ms */
            }
        }
        if (g_sensorCtx.pir120sTimerRestartFlag != 0u && g_sensorCtx.joinedNvFlag != 0u) {
            g_sensorCtx.pir120sTimerRestartFlag = 0u;
            if (setPir120sTimerMs) {
                setPir120sTimerMs(120000u); /* 0x1D4C0 = 120,000 ms */
            }
        }
        g_sensorCtx.sensorEvents &= (uint16_t)~0x0800u;
    }

    /* 6. 0x1000: motion_v1 PIR 120s motion-clear timeout (0x018de) */
    if (isMotionSensor && (ev & 0x1000u)) {
        g_sensorCtx.seqNum++;
        Sensor_sendEvent_A2(0u, aes128EcbBlock16);
        if (setPir10sTimerMs) {
            setPir10sTimerMs(0u);
        }
        g_sensorCtx.pir10sTimerRestartFlag = 1u;
        g_sensorCtx.pirMotionIdleArmed = 1u;
        g_sensorCtx.sensorEvents &= (uint16_t)~0x1000u;
    }

    /* 7. 0x0004: contact_v1 0xAD history drain (0x01c66) OR motion_v1 10s PIR bucket rotation (0x0190a) */
    if (ev & 0x0004u) {
        if (!isMotionSensor) {
            Sensor_sendHistoryLog_AD(aes128EcbBlock16, startHistoryTimerMs);
        } else {
            if (setPir10sTimerMs) {
                setPir10sTimerMs(10000u);
            }
            g_sensorCtx.pirBuckets[g_sensorCtx.pirWindowIndex] = g_sensorCtx.pirPulseCount10s;
            g_sensorCtx.pirPulseCount10s = 0u;
            g_sensorCtx.pirWindowIndex = (g_sensorCtx.pirWindowIndex < 11u) ? (g_sensorCtx.pirWindowIndex + 1u) : 0u;
        }
        g_sensorCtx.sensorEvents &= (uint16_t)~0x0004u;
    }

    /* 8. 0x0008: motion_v1 120s PIR summary 0xAB (0x0193e) */
    if (isMotionSensor && (ev & 0x0008u)) {
        if (setPir120sTimerMs) {
            setPir120sTimerMs(120000u);
        }
        Sensor_sendPirSummary_AB(aes128EcbBlock16);
        g_sensorCtx.sensorEvents &= (uint16_t)~0x0008u;
    }

    /* 9. 0x0010: Button falling-edge interrupt (0x017c8 / 0x01978) */
    if (ev & 0x0010u) {
        if (g_btnCtx.state == 0u) {
            g_btnCtx.state = 1u;
        }
        g_sensorCtx.sensorEvents &= (uint16_t)~0x0010u;
    }

    /* 10. 0x0080: 10 ms button clock tick (0x017e0 / 0x019a0) */
    if (ev & 0x0080u) {
        uint32_t pin = isMotionSensor ? MOTION_PIN_BUTTON : CONTACT_PIN_BUTTON;
        bool isLow = (readDio && readDio(pin) == 0u);
        Ssf_buttonClockCb(isLow);
        Sensor_handleKeys(isMotionSensor);
        g_sensorCtx.sensorEvents &= (uint16_t)~0x0080u;
    }

    /* 11. 0x0100: 5-minute (300,000 ms) heartbeat clock tick (0x01a08 / 0x01c0c) */
    if (ev & 0x0100u) {
        if (g_sensorCtx.joinedNvFlag != 0u) {
            g_sensorCtx.heartbeat5MinTicks++;
            if (g_sensorCtx.heartbeat5MinTicks >= 48u) { /* 48 * 5 min = 4 hours */
                g_sensorCtx.heartbeat5MinTicks = 0u;
                g_sensorCtx.pollRetriesLeft = isMotionSensor ? 5u : 10u;
                uint8_t state = 0u;
                if (readDio) {
                    if (isMotionSensor) {
                        state = (readDio(MOTION_PIN_PIR) != 0u || readDio(MOTION_PIN_PIR_ALT) != 0u) ? 1u : 0u;
                    } else {
                        state = (readDio(CONTACT_PIN_REED) != 0u) ? 1u : 0u;
                    }
                }
                g_sensorCtx.currentReedOrPirState = state;
                Sensor_sendHeartbeat_A1(state, aes128EcbBlock16);
                if (setPollTimerMs) {
                    setPollTimerMs(isMotionSensor ? 800u : 700u); /* 0x320 motion, 0x2BC contact */
                }
            }
        }
        g_sensorCtx.sensorEvents &= (uint16_t)~0x0100u;
    }

    /* 12. 0x0400: LED blink clock tick (0x01a56 / 0x01c6c) */
    if (ev & 0x0400u) {
        if (g_sensorCtx.ledToggleEdgesLeft > 0u) {
            g_sensorCtx.ledToggleEdgesLeft--;
        }
        g_sensorCtx.sensorEvents &= (uint16_t)~0x0400u;
    }

    /* 13. 0x0200: Factory UART frame dispatch (0x01ab0 / 0x01cca) */
    if (ev & 0x0200u) {
        if (g_identCtx.facUartType == 0x43u) {
            FactoryTest_process43(isMotionSensor, programFlash, uartWrite);
        } else if (g_identCtx.facUartType == 0x53u) {
            FactoryTest_process53(isMotionSensor, uartWrite, NULL);
        }
        g_sensorCtx.sensorEvents &= (uint16_t)~0x0200u;
    }

    /* 14. 0x4000: contact_v1 5-sample reed switch debounce tick (0x01bea..0x01c62) */
    if (!isMotionSensor && (ev & 0x4000u)) {
        uint8_t reedNow = (readDio && readDio(CONTACT_PIN_REED) != 0u) ? 1u : 0u;
        g_sensorCtx.currentReedOrPirState = reedNow;
        if (reedNow == 1u) {
            g_sensorCtx.reedHighCount++;
        } else {
            g_sensorCtx.reedLowCount++;
        }

        if (g_sensorCtx.reedHighCount > 4u || g_sensorCtx.reedLowCount > 4u) {
            /* 5 consecutive matching samples -> debounced! */
            g_sensorCtx.reedHighCount = 0u;
            g_sensorCtx.reedLowCount = 0u;
            if (g_sensorCtx.joinedNvFlag != 0u && reedNow != g_sensorCtx.lastSentReedState) {
                g_sensorCtx.seqNum++;
                Sensor_sendEvent_A2(reedNow, aes128EcbBlock16);
                g_sensorCtx.lastSentReedState = reedNow;
            }
        } else if (startReedDebounceTimer) {
            /* Keep sampling until 5 samples match */
            startReedDebounceTimer();
        }
        g_sensorCtx.sensorEvents &= (uint16_t)~0x4000u;
    }

    /* 15. 0x8000: motion_v1 20-second PIR Hardware Self-Test completion (0x01e78..0x01ede) */
    if (isMotionSensor && (ev & 0x8000u)) {
        if (g_sensorCtx.gpioSelfTestActive == 1u) {
            Ssf_factoryResetNv();
            if (g_sensorCtx.factoryTestActive != 0u && uartWrite) {
                uint8_t tx[8];
                tx[0] = 0xAAu;
                tx[1] = 0x55u;
                tx[2] = 0x53u;
                tx[3] = 0x04u;
                tx[4] = 0xD3u;
                tx[5] = g_sensorCtx.gpioSelfTestTriggerCount;
                uint16_t csum = 0;
                for (int i = 0; i < 6; i++) {
                    csum = (uint16_t)(csum + tx[i]);
                }
                tx[6] = (uint8_t)(csum >> 8);
                tx[7] = (uint8_t)(csum & 0xFFu);
                uartWrite(tx, sizeof(tx));
            }
            if (setPollTimerMs) {
                setPollTimerMs(0u);
            }
            g_sensorCtx.gpioSelfTestActive = 0u;
            g_sensorCtx.gpioSelfTestTriggerCount = 0u;
        }
        g_sensorCtx.sensorEvents &= (uint16_t)~0x8000u;
    }
}
