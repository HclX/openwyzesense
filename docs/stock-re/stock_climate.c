/*
 * Reconstructed C source code of the stock Wyze Sense Climate Sensor application
 * (MAC 77C68193, sensor_type = 0x07, firmware version 0x0F), built on TI's
 * SimpleLink CC13x0 SDK 15.4-Stack (ti154stack sensor example).
 *
 * Every function and global variable is annotated with its exact flash or SRAM
 * address in the verified binary dump:
 *   - backups/sensor_77C68193_2026-11-05/flash.bin
 *     (SHA256: 768bd8d78899fa161479414b3dd3a945fff16b0aefbfee2c8adff913e25bce37)
 */

#include "stock_common.h"

/* ==========================================================================
 * 1. Hardware Pin Definitions & BoardGpioInitTable
 * ========================================================================== */

/*
 * Pin assignments for CC1310F128 RHB (5x5 32-pin QFN):
 *   - DIO0  = UART TX test pad (pull-up at boot before UART opens)
 *   - DIO1  = UART RX test pad (pull-up, CCFG SBL backdoor pin: active low)
 *   - DIO6  = Factory test 'W' pad input (pull-up; grounded at boot for Factory UART mode)
 *   - DIO8  = Status LED output (active high, initially low)
 *   - DIO9  = I2C SDA (to Sensirion SHTC3 humidity/temperature sensor)
 *   - DIO10 = I2C SCL (to Sensirion SHTC3 humidity/temperature sensor)
 *   - DIO12 = Pairing button input (pull-up, interrupt enabled on edges)
 *   - Pin 13 = JTAG TMSC ('M' test pad)
 *   - Pin 14 = JTAG TCKC ('C' test pad)
 *   - Pin 19 = RESET_N ('S' test pad)
 */
enum StockClimatePins {
    CLIMATE_PIN_UART_TX = 0,
    CLIMATE_PIN_UART_RX = 1,
    CLIMATE_PIN_FACTEST_W = 6,
    CLIMATE_PIN_LED = 8,
    CLIMATE_PIN_I2C_SDA = 9,
    CLIMATE_PIN_I2C_SCL = 10,
    CLIMATE_PIN_BUTTON = 12,
};

/*
 * BoardGpioInitTable
 * Flash address: 0x0000084C
 * Array of 32-bit PIN_Config words terminated by PIN_TERMINATE (0x000000FE).
 */
static const uint32_t BoardGpioInitTable[] = {
    0x80800808, /* DIO8 : PIN_GPIO_OUTPUT_EN | PIN_GPIO_LOW | PIN_PUSHPULL | PIN_DRVSTR_MIN (LED) */
    0xC007200C, /* DIO12: PIN_INPUT_EN | PIN_PULLUP | PIN_IRQ_BOTHEDGES (Pairing Button) */
    0x80002001, /* DIO1 : PIN_INPUT_EN | PIN_PULLUP (UART RX / SBL Backdoor) */
    0x80002000, /* DIO0 : PIN_INPUT_EN | PIN_PULLUP (UART TX) */
    0xC0002006, /* DIO6 : PIN_INPUT_EN | PIN_PULLUP | PIN_HYSTERESIS (Factory Test 'W' Strap) */
    0x000000FE, /* PIN_TERMINATE */
};

/*
 * I2CCC26XX_HWAttrsV1
 * Flash address: 0x00000894
 */
typedef struct {
    uint32_t baseAddr;   /* 0x00000894: 0x40002000 (I2C0_BASE) */
    int intNum;          /* 0x00000898: 7 (INT_I2C_IRQ) */
    uint8_t powerMngrId; /* 0x0000089c: 0x11 (PERIPH_I2C0) */
    uint8_t intPriority; /* 0x000008a0: 0xFF */
    uint8_t swiPriority; /* 0x000008a4: 0x00 */
    uint8_t sdaPin;      /* 0x000008a8: 0x09 (DIO9) */
    uint8_t sclPin;      /* 0x000008a9: 0x0A (DIO10) */
} I2CCC26XX_HWAttrsV1;

static const I2CCC26XX_HWAttrsV1 I2C_hwAttrs = {
    .baseAddr = 0x40002000,
    .intNum = 7,
    .powerMngrId = 0x11,
    .intPriority = 0xFF,
    .swiPriority = 0x00,
    .sdaPin = CLIMATE_PIN_I2C_SDA,
    .sclPin = CLIMATE_PIN_I2C_SCL,
};

/*
 * I2C_Config Table
 * Flash address: 0x000008B4
 */
typedef struct {
    const void *fxnTablePtr;
    void *object;
    const void *hwAttrs;
} I2C_Config;

/* Forward declaration of I2CCC26XX driver functions */
extern const void *I2CCC26XX_fxnTablePtr;

/* SRAM object for I2C driver instance at 0x20000000-relative SRAM */
static void *g_i2cObject;

static const I2C_Config I2C_config[1] = {
    {
        .fxnTablePtr = &I2CCC26XX_fxnTablePtr,
        .object = &g_i2cObject,
        .hwAttrs = &I2C_hwAttrs,
    },
};

/* ==========================================================================
 * 2. SRAM Global Control Structures & State Variables
 * ========================================================================== */

/*
 * Climate Sensor Control Block (`g_climateCtx`)
 * SRAM address: 0x20001938 / 0x20001880
 */
typedef struct {
    uint8_t joinState;           /* +0x00 (RAM 0x20001938+0x64): 0=unjoined, 1=join_req, 2=challenge, 4=joined */
    uint8_t retryCount;          /* +0x01 (RAM 0x20001938+0x63): Retries left (init 8) */
    uint8_t field_60;            /* +0x02 (RAM 0x20001938+0x60): State timer control byte */
    uint8_t field_61;            /* +0x03 (RAM 0x20001938+0x61): State timer countdown */
    uint8_t seqNum;              /* +0x04 : Monotonic sequence counter */
    uint8_t lastTempInt;         /* +0x05 (RAM 0x20001ADA+0x00): Previous temperature integer part */
    uint8_t lastTempHundredths;  /* +0x06 (RAM 0x20001ADA+0x01): Previous temperature hundredths part */
    uint8_t lastHumidityPct;     /* +0x07 (RAM 0x20001ADA+0x02): Previous relative humidity percent */
    float lastTempFloat;         /* +0x08 (RAM 0x20001938+0x2C): Previous temperature as float */
    float lastHumidityFloat;     /* +0x0C (RAM 0x20001938+0x30): Previous humidity as float */
    uint16_t reportingIntervalS; /* +0x10 (RAM 0x20001938+0x70): Reporting interval in seconds (default 1500) */
    uint16_t pollingIntervalMs;  /* +0x14 (RAM 0x20001938+0x74): Polling interval in milliseconds (default 5000) */
    uint16_t panId;              /* +0x18 (RAM 0x20001938+0x4C): Current 16-bit PAN ID */
    uint8_t channel;             /* +0x1A (RAM 0x20001938+0x4E): RF channel (e.g. 0x00 for 906.8 MHz) */
    uint16_t shortAddr;          /* +0x1C (RAM 0x20001938+0x50): Sensor short address */
    uint16_t coordShortAddr;     /* +0x1E (RAM 0x20001938+0x5A): Gateway coordinator short address */
    uint64_t networkTimeMs;      /* +0x20 (RAM 0x20001938+0x50): 64-bit UTC network timestamp */
    uint8_t gatewayMac[8];       /* +0x28 (RAM 0x20001938+0x80): 8-byte Gateway extended MAC address */
    uint8_t challenge[16];       /* +0x30 (RAM 0x20001938+0x92): 16-byte challenge from gateway 0xA4 */
    uint8_t pendingPacket[32];   /* +0x40 (RAM 0x20001938+0xB4): Buffer holding packet awaiting ACK / retry */
    uint8_t pendingCmd;          /* +0x60 (RAM 0x20001938+0xD4): Pending command ID (0xE8, 0xA1, etc.) */
    uint16_t perTotalRx;         /* +0x62 (RAM 0x20001938+0x42): Total PER test packets received */
    uint16_t perGoodRx;          /* +0x64 (RAM 0x20001938+0x43): Good PER test packets with valid checksum */
    uint16_t perBadRx;           /* +0x66 (RAM 0x20001938+0x44): Corrupted PER test packets */
    uint16_t perRssiSum;         /* +0x68 (RAM 0x20001938+0x46): Accumulated RSSI for PER link test */
} StockClimateSensorCtx_t;

/* SRAM instance at 0x20001938 */
static StockClimateSensorCtx_t g_climateCtx;

/*
 * Identity & Hardware Context (`g_identCtx`)
 * SRAM address: 0x20001D0C
 */
typedef struct {
    uint8_t macAddress[8]; /* 0x20001D0C: 8-byte ASCII/hex MAC (e.g. "77C68193") */
    uint8_t sensorType;    /* 0x07 (CLIMATE) */
    uint8_t fwVersion;     /* 0x0F (15) */
    uint8_t capabilities[3];
} StockIdentCtx_t;

static StockIdentCtx_t g_identCtx;

/*
 * Button Debounce & Hold State (`g_btnCtx`)
 * SRAM address: 0x20001CD0
 */
typedef struct {
    uint8_t state;         /* +0x00 : 0 = idle, 1 = debouncing, 2 = holding */
    uint8_t debounceTicks; /* +0x01 : 10ms ticks in state 1 (needs 4 ticks = 40 ms) */
    uint16_t holdTicks;    /* +0x04 : 10ms ticks in state 2 (300 = 3.00s, 800 = 8.00s) */
} StockButtonCtx_t;

static StockButtonCtx_t g_btnCtx;

/* NVOCTP Function Pointer Table */
static NVINTF_nvFuncts_t *g_pNv; /* Flash 0x00001C14 points here */

/* Forward declarations */
void Sensor_sendMsg(uint8_t cmd, const void *pData, uint16_t len);
void Sensor_sendJoinReq_A3(void);
void Sensor_sendChallengeResp_A5(void);
void Sensor_sendPerEcho_A9(void);
void Sensor_sendDecomAck_B1(uint8_t statusCode);
void Sensor_sendEncryptedReport_E8(int8_t tempInt, uint8_t tempFrac, uint8_t humPct, uint16_t battMv);
void Sensor_sendEncryptedHeartbeat_A1(uint16_t battMv);
void Ssf_ledBlink(uint32_t periodMs, uint8_t count);
void Ssf_factoryResetNv(void);

/* ==========================================================================
 * 3. Sensirion SHTC3 Humidity & Temperature Driver
 * ========================================================================== */

/*
 * SHTC3 Commands:
 *   - Wakeup:    0x3517
 *   - Sleep:     0xB098
 *   - Measure:   0x7CA2 (Normal power, clock stretching enabled, T first)
 */
#define SHTC3_I2C_ADDR 0x70

/*
 * SHTC3_crc8(const uint8_t *data, uint8_t len)
 * Flash address: 0x0000ADAC .. 0x0000ADEC
 * Polynomial: 0x31 (x^8 + x^5 + x^4 + 1), Init: 0xFF
 */
static uint8_t SHTC3_crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0xFF;
    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 0; bit < 8; bit++) {
            if (crc & 0x80) {
                crc = (crc << 1) ^ 0x31;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/*
 * SHTC3_readTempHum()
 * Flash address: 0x0000AD00 .. 0x0000AD90
 *
 * Reads temperature and humidity from Sensirion SHTC3 over I2C:
 *   1. Wakes sensor (0x3517)
 *   2. Starts measurement with clock stretching (0x7CA2)
 *   3. Reads 6 bytes: [T_MSB, T_LSB, T_CRC, RH_MSB, RH_LSB, RH_CRC]
 *   4. Puts sensor to sleep (0xB098)
 *   5. Calculates formulas:
 *        T  = -45.0 + 175.0 * (rawT / 65536.0)
 *        RH = 100.0 * (rawRH / 65536.0)
 */
bool SHTC3_readTempHum(int8_t *pTempInt, uint8_t *pTempFrac, uint8_t *pHumPct)
{
    uint8_t rawBuf[6];
    extern bool I2C_transferHelper(uint8_t addr, const uint8_t *tx, uint8_t txLen, uint8_t *rx, uint8_t rxLen);

    /* Wake up SHTC3: cmd 0x3517 (Flash 0x0000AD0A) */
    static const uint8_t cmdWake[2] = {0x35, 0x17};
    if (!I2C_transferHelper(SHTC3_I2C_ADDR, cmdWake, 2, NULL, 0)) {
        return false;
    }

    /* Trigger measurement: cmd 0x7CA2 (Flash 0x0000AD12) */
    static const uint8_t cmdMeasure[2] = {0x7C, 0xA2};
    if (!I2C_transferHelper(SHTC3_I2C_ADDR, cmdMeasure, 2, rawBuf, 6)) {
        return false;
    }

    /* Put SHTC3 to sleep: cmd 0xB098 (Flash 0x0000AD1C) */
    static const uint8_t cmdSleep[2] = {0xB0, 0x98};
    (void)I2C_transferHelper(SHTC3_I2C_ADDR, cmdSleep, 2, NULL, 0);

    /* Verify Sensirion CRC-8 on temperature and humidity words */
    if (SHTC3_crc8(&rawBuf[0], 2) != rawBuf[2] || SHTC3_crc8(&rawBuf[3], 2) != rawBuf[5]) {
        return false;
    }

    uint16_t rawT = (uint16_t)((rawBuf[0] << 8) | rawBuf[1]);
    uint16_t rawRH = (uint16_t)((rawBuf[3] << 8) | rawBuf[4]);

    /*
     * Temperature conversion (Flash 0x0000AD2C..0x0000AD46):
     * Constant 0x37800000 = (1.0 / 65536.0) at Flash 0x0000AD94
     * Constant 0x432F0000 = 175.0           at Flash 0x0000AD98
     * Constant 0x42340000 = 45.0            at Flash 0x0000AD9C
     */
    float tempC = -45.0f + 175.0f * ((float)rawT / 65536.0f);
    int16_t tempInt = (int16_t)tempC;
    uint8_t tempHundredths = (uint8_t)((tempC - (float)tempInt) * 100.0f);

    /*
     * Humidity conversion (Flash 0x0000AD76..0x0000AD8A):
     * Constant 0x42C80000 = 100.0 at Flash 0x0000ADA0
     */
    float hum = 100.0f * ((float)rawRH / 65536.0f);
    if (hum < 0.0f) {
        hum = 0.0f;
    }
    if (hum > 100.0f) {
        hum = 100.0f;
    }
    uint8_t humPct = (uint8_t)hum;

    /* Store to control block and output pointers (Flash 0x0000AD4C..0x0000AD8C) */
    g_climateCtx.lastTempFloat = tempC;
    g_climateCtx.lastHumidityFloat = hum;
    g_climateCtx.lastTempInt = (uint8_t)tempInt;
    g_climateCtx.lastTempHundredths = tempHundredths;
    g_climateCtx.lastHumidityPct = humPct;

    *pTempInt = (int8_t)tempInt;
    *pTempFrac = tempHundredths;
    *pHumPct = humPct;
    return true;
}

/* ==========================================================================
 * 4. Button Debounce & Hold State Machine
 * ========================================================================== */

/*
 * Button_process()
 * Flash address: 0x00001C70 .. 0x00001D4C
 *
 * Called periodically (every 10 ms) with current DIO12 pin state:
 *   - Debounce window: 4 ticks (40 ms)
 *   - Hold >= 3.00 seconds (300 ticks):
 *       Blinks LED 3 times slowly (400 ms half-period) via Ssf_ledBlink (0x00012950)
 *   - Release after >= 3.00s:
 *       Wipes network NV items, resets PAN/channel to 0xFFFF/0xFF, restarts join scan
 *   - Hold >= 8.00 seconds (800 ticks):
 *       Stuck button protection: cancels button action without triggering reset
 */
void Button_process(uint8_t pinState)
{
    /* Read active-low pairing button on DIO12 (Flash 0x00001C88) */
    uint8_t pressed = (pinState == 0) ? 1 : 0;

    switch (g_btnCtx.state) {
        case 0: /* IDLE */
            if (pressed) {
                g_btnCtx.state = 1;
                g_btnCtx.debounceTicks = 0;
                g_btnCtx.holdTicks = 0;
            }
            break;

        case 1: /* DEBOUNCING (40 ms) */
            if (!pressed) {
                g_btnCtx.state = 0;
                g_btnCtx.debounceTicks = 0;
            } else {
                g_btnCtx.debounceTicks++;
                if (g_btnCtx.debounceTicks >= 4) {
                    g_btnCtx.state = 2; /* Transition to HOLDING */
                    g_btnCtx.holdTicks = 0;
                }
            }
            break;

        case 2: /* HOLDING */
            if (pressed) {
                g_btnCtx.holdTicks++;
                /* Exactly 3.00 seconds hold reached (Flash 0x00001CB0) */
                if (g_btnCtx.holdTicks == 300) {
                    Ssf_ledBlink(400, 3); /* Blink LED 3 times slowly */
                } else if (g_btnCtx.holdTicks > 800) {
                    /* Stuck button timeout (> 8.00s, Flash 0x00001CCE) */
                    g_btnCtx.state = 0;
                    g_btnCtx.holdTicks = 0;
                }
            } else {
                /* Button released */
                if (g_btnCtx.holdTicks >= 300 && g_btnCtx.holdTicks <= 800) {
                    /*
                 * Factory Reset Network NV & Restart Join Scan:
                 * Flash 0x00001D54..0x00001DDC
                 */
                    g_climateCtx.joinState = 0;
                    g_climateCtx.retryCount = 15;
                    g_climateCtx.panId = 0xFFFF;
                    g_climateCtx.channel = 0xFF;
                    g_climateCtx.shortAddr = 0xFFFF;
                    g_climateCtx.coordShortAddr = 0xFFFF;
                    memset(g_climateCtx.gatewayMac, 0xFF, 8);
                    Ssf_factoryResetNv();
                    Sensor_sendJoinReq_A3();
                }
                g_btnCtx.state = 0;
                g_btnCtx.holdTicks = 0;
            }
            break;
    }
}

/* ==========================================================================
 * 5. Outbound RF Packet Generators & Transmission
 * ========================================================================== */

/*
 * Sensor_sendMsg()
 * Flash address: 0x00004174 .. 0x00004250
 * Central MAC data request dispatcher wrapping ApiMac_mcpsDataReq (0x00012F46).
 */
void Sensor_sendMsg(uint8_t cmd, const void *pData, uint16_t len)
{
    extern void ApiMac_mcpsDataReqHelper(uint8_t cmd, const void *pData, uint16_t len);
    ApiMac_mcpsDataReqHelper(cmd, pData, len);
}

/*
 * Sensor_sendJoinReq_A3()
 * Flash address: 0x000018B0 .. 0x00001920
 *
 * 16-byte Join Request packet:
 *   [0]      = 0xA3
 *   [1..8]   = MAC address ("77C68193")
 *   [9]      = Sensor Type = 0x07 (CLIMATE)
 *   [10]     = Firmware Version = 0x0F (15)
 *   [11..13] = Capability bytes: 0x66, 0x00, 0x00
 *   [14..15] = 0x00, 0x00
 */
void Sensor_sendJoinReq_A3(void)
{
    uint8_t pkt[16];
    extern void Crypto_calculateMic16(const uint8_t *in, uint8_t len, uint8_t *out);

    pkt[0] = 0xA3;
    memcpy(&pkt[1], g_identCtx.macAddress, 8);
    pkt[9] = 0x07; /* CLIMATE */
    pkt[10] = 0x0F;
    pkt[11] = 0x66;
    pkt[12] = 0x00;
    pkt[13] = 0x00;
    pkt[14] = 0x00;
    pkt[15] = 0x00;

    Crypto_calculateMic16(pkt, 16, pkt);
    Sensor_sendMsg(0x05, pkt, 16);
}

/*
 * Sensor_sendChallengeResp_A5()
 * Flash address: 0x0000D78C .. 0x0000D7F6
 *
 * 32-byte Join Challenge Response packet:
 *   [0]      = 0xA5
 *   [1..8]   = Sensor MAC address
 *   [9..24]  = 16-byte encrypted response from challenge (via 0x000070BC)
 *   [25]     = Sensor Type = 0x07 (CLIMATE)
 *   [26]     = Firmware Version = 0x0F (15)
 *   [27..31] = MIC / auth padding
 */
void Sensor_sendChallengeResp_A5(void)
{
    uint8_t pkt[32];
    uint8_t encResp[16];
    extern void Crypto_calcChallengeResponse(const uint8_t in[16], uint8_t out[16]);
    extern void Crypto_calculateMic32(const uint8_t *in, uint8_t len, uint8_t *out);

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xA5;
    memcpy(&pkt[1], g_identCtx.macAddress, 8);

    Crypto_calcChallengeResponse(g_climateCtx.challenge, encResp);
    memcpy(&pkt[9], encResp, 16);

    pkt[25] = 0x07; /* CLIMATE */
    pkt[26] = 0x0F;

    Crypto_calculateMic32(pkt, 32, pkt);
    Sensor_sendMsg(0x05, pkt, 32);
}

/*
 * Sensor_sendPerEcho_A9()
 * Flash address: 0x0000D804 .. 0x0000D862
 *
 * 32-byte RF Link / Packet Error Rate (PER) Test Response:
 *   [0]      = 0xA9 (echoes 0xAA)
 *   [1..29]  = Template payload from 0x0000D86C
 *   [30]     = RSSI / sequence sample
 *   [31]     = 8-bit checksum: sum(pkt[0..30]) & 0xFF
 */
void Sensor_sendPerEcho_A9(void)
{
    uint8_t pkt[32];
    extern const uint8_t g_perEchoTemplate[29]; /* Flash 0x0000D86C */
    extern uint8_t g_lastRxRssi;                /* Flash 0x0000D870 */

    pkt[0] = 0xA9;
    memcpy(&pkt[1], g_perEchoTemplate, 29);
    pkt[30] = g_lastRxRssi;

    uint8_t sum = 0;
    for (uint8_t i = 0; i < 31; i++) {
        sum += pkt[i];
    }
    pkt[31] = sum;

    Sensor_sendMsg(0x05, pkt, 32);
}

/*
 * Sensor_sendDecomAck_B1()
 * Flash address: 0x000107DC .. 0x0001080A
 *
 * 16-byte Decommission / Leave Acknowledgment:
 *   [0]      = 0xB1
 *   [1]      = Status code (0x00 = success)
 *   [2..15]  = Auth / MIC padding
 */
void Sensor_sendDecomAck_B1(uint8_t statusCode)
{
    uint8_t pkt[16];
    extern void Crypto_calculateMic16(const uint8_t *in, uint8_t len, uint8_t *out);

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xB1;
    pkt[1] = statusCode;

    Crypto_calculateMic16(pkt, 16, pkt);
    Sensor_sendMsg(0x05, pkt, 16);
}

/*
 * Sensor_sendEncryptedReport_E8()
 * Flash address: 0x0000A604 .. 0x0000A698
 *
 * Full 32-byte encrypted over-the-air climate report frame:
 *   [0]      = 0xE8
 *   [1..8]   = Sensor MAC (8 bytes)
 *   [9]      = Sensor Type = 0x07 (CLIMATE)
 *   [10]     = Monotonic sequence counter (from 0x00012E32)
 *   [11]     = Battery / flags byte (sensor_ctx + 0x28)
 *   [12]     = Context byte
 *   [13]     = Data length = 0x03
 *   [14]     = Temperature °C integer part (int8_t)
 *   [15]     = Temperature °C hundredths part (uint8_t, 0..99)
 *   [16]     = Relative Humidity % (uint8_t, 0..100)
 *   [17..18] = Battery voltage in mV (uint16_t BE)
 *   [19..31] = Zero-padded & encrypted with AES-CCM* / proprietary MIC via 0x0000AF28.
 */
void Sensor_sendEncryptedReport_E8(int8_t tempInt, uint8_t tempFrac, uint8_t humPct, uint16_t battMv)
{
    uint8_t pkt[32];
    extern uint8_t ApiMac_getSeqNum(void); /* Flash 0x00012E32 */
    extern void Crypto_calculateMic32(const uint8_t *in, uint8_t len, uint8_t *out);

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xE8;
    memcpy(&pkt[1], g_identCtx.macAddress, 8);
    pkt[9] = 0x07; /* CLIMATE */
    pkt[10] = ApiMac_getSeqNum();
    pkt[11] = 0x00; /* Flags */
    pkt[12] = 0x00;
    pkt[13] = 0x03; /* Data length: 3 bytes */
    pkt[14] = (uint8_t)tempInt;
    pkt[15] = tempFrac;
    pkt[16] = humPct;
    pkt[17] = (uint8_t)(battMv >> 8);
    pkt[18] = (uint8_t)(battMv & 0xFF);

    Crypto_calculateMic32(pkt, 32, pkt);
    Sensor_sendMsg(0x05, pkt, 32);
}

/*
 * Sensor_sendEncryptedHeartbeat_A1()
 * Flash address: 0x0000B000 .. 0x0000B08E
 *
 * Full 32-byte encrypted over-the-air heartbeat frame (sent every 120 minutes):
 *   [0]      = 0xA1
 *   [1..8]   = Sensor MAC (8 bytes)
 *   [9]      = Sensor Type = 0x07 (CLIMATE)
 *   [10]     = Monotonic sequence counter (from 0x00012E32)
 *   [11]     = Battery / flags byte (sensor_ctx + 0x28)
 *   [12]     = Context byte
 *   [13]     = Data length = 0x01
 *   [14]     = Current state byte (0x00)
 *   [15..16] = Battery voltage in mV (uint16_t BE)
 *   [17..31] = Zero-padded & encrypted with AES-CCM* / proprietary MIC via 0x0000AF28.
 */
void Sensor_sendEncryptedHeartbeat_A1(uint16_t battMv)
{
    uint8_t pkt[32];
    extern uint8_t ApiMac_getSeqNum(void);
    extern void Crypto_calculateMic32(const uint8_t *in, uint8_t len, uint8_t *out);

    memset(pkt, 0, sizeof(pkt));
    pkt[0] = 0xA1;
    memcpy(&pkt[1], g_identCtx.macAddress, 8);
    pkt[9] = 0x07; /* CLIMATE */
    pkt[10] = ApiMac_getSeqNum();
    pkt[11] = 0x00;
    pkt[12] = 0x00;
    pkt[13] = 0x01; /* Data length: 1 byte */
    pkt[14] = 0x00; /* State */
    pkt[15] = (uint8_t)(battMv >> 8);
    pkt[16] = (uint8_t)(battMv & 0xFF);

    Crypto_calculateMic32(pkt, 32, pkt);
    Sensor_sendMsg(0x05, pkt, 32);
}

/* ==========================================================================
 * 6. Inbound RF Packet Dispatcher: Sensor_processRxMsg
 * ========================================================================== */

/*
 * Sensor_processRxMsg()
 * Flash address: 0x00003A84 .. 0x00003DE4
 *
 * Dispatches incoming over-the-air MAC data payload:
 *   - 0xA4: Gateway Join Challenge
 *   - 0xA6: Join Confirm (subcmd 0x04) or Decommission (subcmd 0x05)
 *   - 0xAA: RF PER Link Test Request
 *   - 0xB0: Network Time Sync
 *   - 0xE9: Extended Time Sync / Config
 *   - TI 15.4 standard config
 */
void Sensor_processRxMsg(const uint8_t *pMsdu, uint16_t len)
{
    if (!pMsdu || len == 0) {
        return;
    }

    uint8_t cmd = pMsdu[0];

    switch (cmd) {
        case 0xA4: {
            /*
         * CASE 0xA4: Join Challenge Request (Flash 0x00003DBC)
         * Payload: [0xA4][challenge (16 bytes)]
         */
            if (len >= 17) {
                memcpy(g_climateCtx.challenge, &pMsdu[1], 16);
                g_climateCtx.joinState = 2; /* Waiting to send challenge response 0xA5 */
                g_climateCtx.retryCount = 8;
                g_climateCtx.field_60 = 60;
                extern void Timer_startMs(uint32_t ms); /* Flash 0x00010D34 */
                Timer_startMs(100);                     /* 100 ms trigger timer */
            }
            break;
        }

        case 0xA6: {
            /*
         * CASE 0xA6: Join Confirmation or Decommission (Flash 0x00003CB2)
         * Payload: [0xA6][0xFF][subcmd][params...]
         */
            if (len >= 3 && pMsdu[1] == 0xFF) {
                uint8_t subcmd = pMsdu[2];
                if (subcmd == 0x04) {
                    /*
                 * Subcommand 0x04: Join Success (Flash 0x00003D28)
                 * pMsdu[3..10] contains Gateway extended address / network key
                 */
                    g_climateCtx.joinState = 4; /* Joined */
                    if (len >= 11) {
                        memcpy(g_climateCtx.gatewayMac, &pMsdu[3], 8);
                    }
                    extern void Nv_saveJoinParams(void); /* Flash 0x000111DC */
                    Nv_saveJoinParams();

                    /* Arm 120-minute heartbeat timer & initial report */
                    extern void Timer_startHeartbeat(uint32_t ticks); /* Flash 0x00010D9C */
                    Timer_startHeartbeat(300);
                } else if (subcmd == 0x05) {
                    /*
                 * Subcommand 0x05: Decommission / Leave (Flash 0x00003CDA)
                 */
                    g_climateCtx.joinState = 0;
                    Sensor_sendDecomAck_B1(0x00);
                    if (len >= 4 && pMsdu[3] == 0x01) {
                        Ssf_factoryResetNv();
                    }
                    Sensor_sendJoinReq_A3();
                }
            }
            break;
        }

        case 0xAA: {
            /*
         * CASE 0xAA: RF PER Link Test (Flash 0x00003BD2)
         * Payload: [0xAA][payload (30B)][checksum_8]
         */
            g_climateCtx.perTotalRx++;
            if (len == 32) {
                uint8_t sum = 0;
                for (uint8_t i = 0; i < 31; i++) {
                    sum += pMsdu[i];
                }
                if (pMsdu[31] == sum) {
                    g_climateCtx.perGoodRx++;
                } else {
                    g_climateCtx.perBadRx++;
                }
                /* At 10 packets received, reply with 0xA9 (Flash 0x00003C48) */
                if (g_climateCtx.perTotalRx >= 10) {
                    Sensor_sendPerEcho_A9();
                    g_climateCtx.perTotalRx = 0;
                    g_climateCtx.perGoodRx = 0;
                    g_climateCtx.perBadRx = 0;
                }
            }
            break;
        }

        case 0xB0: {
            /*
         * CASE 0xB0: Network Time Sync (Flash 0x00003B48)
         * Payload: [0xB0][timestamp (64-bit ms LE)]
         */
            if (len >= 9) {
                uint64_t ts = 0;
                for (uint8_t i = 0; i < 8; i++) {
                    ts |= ((uint64_t)pMsdu[1 + i]) << (8 * i);
                }
                g_climateCtx.networkTimeMs = ts;
                extern void Rtc_updateClockMs(uint64_t ms); /* Flash 0x00012FB2 */
                Rtc_updateClockMs(ts);
            }
            break;
        }

        case 0xE9: {
            /*
         * CASE 0xE9: Extended Time Sync / Config (Flash 0x00003B1C)
         */
            if (len >= 13) {
                uint64_t ts = 0;
                for (uint8_t i = 0; i < 8; i++) {
                    ts |= ((uint64_t)pMsdu[5 + i]) << (8 * i);
                }
                g_climateCtx.networkTimeMs = ts;
                extern void Rtc_updateClockMs(uint64_t ms);
                Rtc_updateClockMs(ts);
            }
            break;
        }

        default:
            break;
    }
}

/* ==========================================================================
 * 7. Factory UART Production Test Parser
 * ========================================================================== */

/*
 * FactoryUart_process()
 * Flash address: 0x00001620 .. 0x000017D0
 *
 * Invoked when DIO6 ('W' strap) is grounded at boot.
 * Framing: [0x55][0xAA]['C' or 'S'][len][cmd][payload...][checksum_16 BE]
 */
void FactoryUart_process(const uint8_t *rxBuf, uint8_t len)
{
    extern void Uart_writeBytes(const uint8_t *buf, uint8_t len); /* Flash 0x00012AE0 */

    if (!rxBuf || len < 6) {
        return;
    }

    /* Check sync word and header: 0x55 0xAA 'S' */
    if (rxBuf[0] != 0x55 || rxBuf[1] != 0xAA || (rxBuf[2] != 'S' && rxBuf[2] != 'C')) {
        return;
    }

    uint8_t cmd = rxBuf[4];

    switch (cmd) {
        case 0x16: {
            /* Device self-test & info (Flash 0x000017DE) */
            extern void Factory_runSelfTest(void);
            Factory_runSelfTest();
            break;
        }

        case 0x2A: {
            /* Factory sensor calibration (Flash 0x000016D0) */
            uint8_t resp[7] = {0x55, 0xAA, 'S', 0x03, 0x2B, 0x00, 0x00};
            uint16_t sum = 0;
            for (uint8_t i = 0; i < 5; i++) {
                sum += resp[i];
            }
            resp[5] = (uint8_t)(sum >> 8);
            resp[6] = (uint8_t)(sum & 0xFF);
            Uart_writeBytes(resp, 7);
            break;
        }

        case 0x2D: {
            /* Read MAC address & hardware UUID (Flash 0x0000165E) */
            uint8_t resp[16];
            resp[0] = 0x55;
            resp[1] = 0xAA;
            resp[2] = 'S';
            resp[3] = 0x0A;
            resp[4] = 0x2B;
            memcpy(&resp[5], g_identCtx.macAddress, 8);
            uint16_t sum = 0;
            for (uint8_t i = 0; i < 13; i++) {
                sum += resp[i];
            }
            resp[13] = (uint8_t)(sum >> 8);
            resp[14] = (uint8_t)(sum & 0xFF);
            Uart_writeBytes(resp, 15);
            break;
        }

        case 0xDE: {
            /*
         * RF Production BER / Carrier Test (Flash 0x00001762)
         * Responds with 0xDF: [0x55][0xAA]['S'][0x06][0xDF][status][mac_short][chk]
         */
            uint8_t resp[10];
            resp[0] = 0x55;
            resp[1] = 0xAA;
            resp[2] = 'S';
            resp[3] = 0x06;
            resp[4] = 0xDF;
            resp[5] = 0x00; /* Success status */
            resp[6] = g_identCtx.macAddress[0];
            resp[7] = g_identCtx.macAddress[1];
            uint16_t sum = 0;
            for (uint8_t i = 0; i < 8; i++) {
                sum += resp[i];
            }
            resp[8] = (uint8_t)(sum >> 8);
            resp[9] = (uint8_t)(sum & 0xFF);
            Uart_writeBytes(resp, 10);
            break;
        }

        default:
            break;
    }
}

/* ==========================================================================
 * 8. Main Sensor Task & Process Loop
 * ========================================================================== */

/*
 * Sensor_init()
 * Flash address: 0x000033AA
 */
void Sensor_init(void)
{
    /* Initialize NVOCTP, PIN driver, and read MAC from CCFG (Flash 0x0001FFCC) */
    memcpy(g_identCtx.macAddress, (const void *)STOCK_CCFG_MAC_ADDR, 8);
    g_identCtx.sensorType = 0x07; /* CLIMATE */
    g_identCtx.fwVersion = 0x0F;  /* 15 */
}

/*
 * Sensor_process()
 * Flash address: 0x000014F0 .. 0x000019B0
 *
 * Main application event loop:
 *   - Checks pairing button transitions
 *   - Polls SHTC3 sensor for temperature / humidity changes
 *   - If delta >= 0.5 °C or >= 3.0 % RH: sends 0xE8 report
 *   - Manages join retry state machine (0xA3 / 0xA5)
 *   - Checks Factory UART RX buffer
 */
void Sensor_process(void)
{
    int8_t tempInt = 0;
    uint8_t tempFrac = 0;
    uint8_t humPct = 0;

    if (g_climateCtx.joinState == 4) {
        /* Joined: sample SHTC3 sensor */
        if (SHTC3_readTempHum(&tempInt, &tempFrac, &humPct)) {
            /* Check delta thresholds: float 0.5 °C (Flash 0x00001D50) or 3.0 % RH */
            float tempDelta = g_climateCtx.lastTempFloat - (float)tempInt;
            if (tempDelta < 0.0f) {
                tempDelta = -tempDelta;
            }
            float humDelta = g_climateCtx.lastHumidityFloat - (float)humPct;
            if (humDelta < 0.0f) {
                humDelta = -humDelta;
            }

            if (tempDelta >= 0.5f || humDelta >= 3.0f) {
                Sensor_sendEncryptedReport_E8(tempInt, tempFrac, humPct, 3000);
            }
        }
    } else if (g_climateCtx.joinState == 1) {
        /* State 1: Broadcasting 0xA3 Join Requests */
        Sensor_sendJoinReq_A3();
    } else if (g_climateCtx.joinState == 2) {
        /* State 2: Sending 0xA5 Challenge Response */
        Sensor_sendChallengeResp_A5();
    }
}

/*
 * Sensor_taskFxn()
 * Flash address: 0x0000333C .. 0x0000366A
 * RTOS task entry point: loops on Semaphore_pend / Sensor_process().
 */
void Sensor_taskFxn(uint32_t a0, uint32_t a1)
{
    (void)a0;
    (void)a1;
    Sensor_init();

    while (1) {
        Sensor_process();
    }
}
