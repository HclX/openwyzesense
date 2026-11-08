#include "radio.h"

#include "clock.h"

#include <string.h>

#include "chip.h"
#include "phy_settings.h"
// clang-format off: a TI include path, not a division
#include DeviceFamily_constructPath(driverlib/rf_data_entry.h)
#include DeviceFamily_constructPath(driverlib/aon_batmon.h)
#include DeviceFamily_constructPath(inc/hw_aon_batmon.h)
// clang-format on
#include <ti/drivers/dpl/HwiP.h>

// RX data entries: [2-byte length][PHR 2][MAC frame][RSSI 1][timestamp 4] per element (rxConf in phy_settings.c)
#define RX_ENTRIES    4
#define RX_ENTRY_DATA (2 + 2 + RADIO_MAX_FRAME + 1 + 4)
#define RX_ENTRY_SIZE ((sizeof(rfc_dataEntryGeneral_t) - 1 + RX_ENTRY_DATA + 3) & ~3u)
#define FRAME_FIFO    8  // frames waiting for the main loop (power of two)

static RF_Object rf_object;
static RF_Handle rf;
static uint32_t tx_min_raw = UINT32_MAX;  // BATMON's lowest reading during the last frame (3.8 fixed-point V)
static RF_CmdHandle rx_cmd;
static bool rx_on;
static uint8_t rx_buf[RX_ENTRIES * RX_ENTRY_SIZE] __attribute__((aligned(4)));
static dataQueue_t rx_queue;
static rfc_dataEntryGeneral_t *rx_current;
static rfc_propRxOutput_t rx_stats;

static radio_frame_t fifo[FRAME_FIFO];
static volatile uint32_t fifo_head, fifo_tail, overruns;

// The stock settings send and store packets MSB first (formatConf.bMsbFirst, which the PHR needs), but the
// 802.15.4g PSDU goes LSB first: every PSDU byte reaches us bit-reversed, and has to leave that way.
static void reverse_bits(uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        uint32_t v = b[i];
        __asm__("rbit %0, %1" : "=r"(v) : "r"(v));
        b[i] = (uint8_t)(v >> 24);
    }
}

static void rx_queue_init(void)
{
    for (int i = 0; i < RX_ENTRIES; i++) {
        rfc_dataEntryGeneral_t *e = (rfc_dataEntryGeneral_t *)(rx_buf + i * RX_ENTRY_SIZE);
        e->pNextEntry = rx_buf + ((i + 1) % RX_ENTRIES) * RX_ENTRY_SIZE;
        e->status = DATA_ENTRY_PENDING;
        e->config.type = DATA_ENTRY_TYPE_GEN;
        e->config.lenSz = 2;
        e->length = RX_ENTRY_DATA;
    }
    rx_queue.pCurrEntry = rx_buf;
    rx_queue.pLastEntry = NULL;  // circular
    rx_current = (rfc_dataEntryGeneral_t *)rx_buf;
}

// RF driver callback (software interrupt context): move finished entries into the FIFO.
static void rx_callback(RF_Handle h, RF_CmdHandle ch, RF_EventMask e)
{
    (void)h, (void)ch;
    if (!(e & RF_EventRxEntryDone)) {
        return;
    }
    while (rx_current->status == DATA_ENTRY_FINISHED) {
        const uint8_t *d = &rx_current->data;
        uint16_t n = (uint16_t)(d[0] | d[1] << 8);  // element length: PHR + frame + RSSI + timestamp
        if (n >= 2 + 1 + 4 && n - 2 - 1 - 4 <= RADIO_MAX_FRAME) {
            if (fifo_head - fifo_tail < FRAME_FIFO) {
                radio_frame_t *f = &fifo[fifo_head % FRAME_FIFO];
                f->len = (uint8_t)(n - 2 - 1 - 4);
                memcpy(f->frame, d + 2 + 2, f->len);
                reverse_bits(f->frame, f->len);
                f->rssi = (int8_t)d[2 + 2 + f->len];
                memcpy(&f->rat_time, d + 2 + 2 + f->len + 1, 4);
                fifo_head++;
                clock_wake();
            } else {
                overruns++;
            }
        }
        rx_current->status = DATA_ENTRY_PENDING;
        rx_current = (rfc_dataEntryGeneral_t *)rx_current->pNextEntry;
    }
}

bool radio_init(void)
{
    RF_Params params;
    RF_Params_init(&params);
    params.nInactivityTimeout = 2000;  // µs: with nothing to do (a sensor between events), the RF core
                                       // powers down; a gateway never stops receiving, so never
    rf = RF_open(&rf_object, &phy_mode, (RF_RadioSetup *)&phy_cmd_setup, &params);
    if (!rf) {
        return false;
    }
    return RF_runCmd(rf, (RF_Op *)&phy_cmd_fs, RF_PriorityNormal, NULL, 0) & RF_EventLastCmdDone;
}

bool radio_rx_start(void)
{
    if (rx_on) {
        return true;
    }
    rx_queue_init();
    phy_cmd_rx.pQueue = &rx_queue;
    phy_cmd_rx.pOutput = (uint8_t *)&rx_stats;
    phy_cmd_rx.pktConf.bRepeatOk = 1;  // keep receiving after each frame
    phy_cmd_rx.pktConf.bRepeatNok = 1;
    phy_cmd_rx.endTrigger.triggerType = TRIG_NEVER;
    rx_cmd = RF_postCmd(rf, (RF_Op *)&phy_cmd_rx, RF_PriorityNormal, rx_callback, RF_EventRxEntryDone);
    rx_on = rx_cmd >= 0;
    return rx_on;
}

void radio_rx_stop(void)
{
    if (!rx_on) {
        return;
    }
    RF_cancelCmd(rf, rx_cmd, 0);
    rx_on = false;
}


bool radio_poll(radio_frame_t *out)
{
    if (fifo_tail == fifo_head) {
        return false;
    }
    *out = fifo[fifo_tail % FRAME_FIFO];
    fifo_tail++;
    return true;
}

uint32_t radio_overruns(void)
{
    return overruns;
}

bool radio_tx(const uint8_t *frame, size_t len, uint32_t gap_us)
{
    static uint32_t last_start;  // radio timer (4 MHz) at the start of the previous frame
    // PHR (FACTS.md §2): 4-byte FCS, whitening on, length incl. FCS; the radio appends the FCS. The radio
    // sends the 16-bit header low byte first, so it goes in little-endian (found with the SDR, test_hw_tx.py).
    static uint8_t tx_buf[2 + RADIO_MAX_FRAME];
    uint16_t phr = (uint16_t)(1u << 11 | (len + 4));
    bool ok;
    if (len > RADIO_MAX_FRAME) {
        return false;
    }
    tx_buf[0] = (uint8_t)phr;
    tx_buf[1] = (uint8_t)(phr >> 8);
    memcpy(tx_buf + 2, frame, len);
    reverse_bits(tx_buf + 2, len);
    phy_cmd_tx.pktLen = (uint16_t)(2 + len);
    phy_cmd_tx.pPkt = tx_buf;
    radio_rx_stop();  // the receiver: stopped for the frame, on again after it
    if (gap_us) {
        last_start += gap_us * 4;
        phy_cmd_tx.startTrigger.triggerType = TRIG_ABSTIME;
        phy_cmd_tx.startTrigger.pastTrig = 1;  // already late: start now
        phy_cmd_tx.startTime = last_start;
    } else {
        phy_cmd_tx.startTrigger.triggerType = TRIG_NOW;
        last_start = RF_getCurrentTime();
    }
    // EXPERIMENT (#85): the radio_tx of before the battery-under-load change (c642c0c): run the command, no BATMON
    ok = RF_runCmd(rf, (RF_Op *)&phy_cmd_tx, RF_PriorityNormal, NULL, 0) & RF_EventLastCmdDone;
    return radio_rx_start() && ok;
}

uint32_t radio_tx_min_mv(void)
{
    return tx_min_raw == UINT32_MAX ? 0 : (tx_min_raw * 1000u) >> 8;  // 3.8 fixed-point volts
}
