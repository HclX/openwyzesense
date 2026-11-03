// Gateway coordinator application (common/gateway.c) on the hardware interface (hal/).
#include "gateway_app.h"

#include <string.h>

#include "board.h"
#include "build_info.h"
#include "ccfg.h"
#include "chip.h"
#include "clock.h"
#include "nv_page.h"
#include "radio.h"
#include "uart.h"

// Identity. 0 = derive from the chip's IEEE address. Set at build time with -DOWS_PAN=... -DOWS_SHORT=...
#ifndef OWS_PAN
#define OWS_PAN 0
#endif
#ifndef OWS_SHORT
#define OWS_SHORT 0
#endif
#ifndef OWS_PASSIVE
#define OWS_PASSIVE 0
#endif
#if OWS_PAN == 0x845B && !OWS_PASSIVE
#error "never transmit on the house network (PAN 0x845B): see CLAUDE.md"
#endif

#ifndef OWS_DEBUG
#define OWS_DEBUG 0
#endif
// Debug builds only (not a stock command): a MAC frame (no FCS) handed to the gateway as if received, to make
// it transmit on demand. Payload: rssi (int8, dBm), frame.
#define HOST_DEBUG_INJECT 0x43F0

// A mains-powered gateway: frames and host bytes wake it; this is only for gw_tick's own timers
#define TICK_MS          10
#define GW_SCAN_BLINK_MS 300  // stock ledBlinkToggle period during pairing scan mode (stock_gateway.c:37)
#define GW_RX_PULSE_MS   50   // brief activity dip on valid RF frame reception

static void radio_send(void *ctx, const uint8_t *frame, size_t len, uint32_t gap_us)
{
    (void)ctx;
#if OWS_PASSIVE
    (void)frame, (void)len, (void)gap_us;
#else
    radio_tx(frame, len, gap_us);
#endif
}

static void host_send(void *ctx, const uint8_t *pkt, size_t len)
{
    (void)ctx;
    uart_write(pkt, len);
}

// OWS_SUB_CCFG (gateway.h): the chip's CCFG against the one built in (hal/ccfg.h)
static int ccfg(void *ctx, bool write)
{
    (void)ctx;
    uint8_t want[CCFG_SIZE];
    if (write) {
        return ccfg_restore();
    }
    ccfg_merge(ccfg_builtin(), ccfg_current(), want);
    if (!ccfg_is_safe(want, NULL)) {
        return CCFG_NOT_SAFE;
    }
    return memcmp(ccfg_current(), want, CCFG_SIZE) ? 0 : CCFG_ALREADY_RIGHT;
}

static bool sbl(void *ctx, bool enter)
{
    gateway_app_t *a = ctx;
    if (board_usb_hid() || !chip_can_enter_sbl()) {
        return false;
    }
    if (!enter) {
        return true;
    }
    uart_flush();
    a->halted = chip_enter_sbl();
    return a->halted;
}

static void init_gateway(gateway_app_t *a)
{
    static const char hex[] = "0123456789ABCDEF";
    gw_io_t io = {radio_send, host_send, ccfg, a, sbl};
    uint64_t ieee = chip_ieee();
    uint8_t mac[8] = {'7', '7'};  // stock MACs start with 77
    uint16_t pan = OWS_PAN ? OWS_PAN : (uint16_t)(ieee ^ ieee >> 16);
    uint16_t addr = OWS_SHORT ? OWS_SHORT : (uint16_t)(ieee >> 32 ^ ieee >> 48);
    if (pan == 0xFFFF) {
        pan = 0x0001;  // 0xFFFF is broadcast
    }
    for (int i = 0; i < 6; i++) {
        mac[2 + i] = (uint8_t)hex[(ieee >> (20 - 4 * i)) & 15];
    }
    board_mac(mac);  // a Wyze sensor board keeps its own MAC in its CCFG: that one
    gw_init(&a->gw, pan, addr, ieee, mac, &io);
    a->gw.usb_hid = board_usb_hid();
    a->gw.build_info = &build_info;  // what OWS_SUB_BUILD_INFO answers (#76)
    a->gw.build_info_len = sizeof build_info;
    // the network and sensor table from the NV page (our format: docs/design/nv-format.md; a stock dongle's is carried over
    // by tools/nv_json.py or tools/gwsync.py); else ours, saved there.
    // A build-time identity (OWS_PAN) wins over the page.
    bool have = nv_page_open(&a->nv) && gw_load(&a->gw, &a->nv);
    if (!have || OWS_PAN) {
        a->gw.pan = pan, a->gw.short_addr = addr, a->gw.ext = ieee;
        if (pan != 0x845B) {
            gw_save_network(&a->gw);
        }
    }
}

void gateway_app_init(gateway_app_t *a)
{
    memset(a, 0, sizeof *a);
    host_rx_init(&a->host_rx);
    init_gateway(a);
    board_keep_awake();  // the host link and the receiver never sleep
    a->now_ms = clock_now_ms();
    // never transmit on the house network (CLAUDE.md): a gateway that finds the house PAN in flash stops here
    if ((!OWS_PASSIVE && a->gw.pan == 0x845B) || !radio_init() || !radio_rx_start()) {
        a->halted = true;
        board_led(BOARD_LED_ERROR, 1);
        return;
    }
    board_led(BOARD_LED_OK, 1);
}

uint32_t gateway_app_step(gateway_app_t *a)
{
    radio_frame_t f;
    host_pkt_t pkt;
    uint8_t c;

    if (a->halted) {
        return UINT32_MAX;
    }
    bool got_rf = false;
    while (radio_poll(&f)) {
        gw_rx(&a->gw, f.frame, f.len, f.rssi);
        got_rf = true;
    }
    while (!a->halted && uart_getc(&c)) {
        if (!host_rx_feed(&a->host_rx, c, &pkt)) {
            continue;
        }
        if (OWS_DEBUG && pkt.cmd == HOST_DEBUG_INJECT && pkt.len > 1) {
            uint8_t frame[RADIO_MAX_FRAME];
            size_t n = pkt.len - 1 < sizeof frame ? pkt.len - 1 : sizeof frame;
            int8_t rssi = (int8_t)pkt.payload[0];
            memcpy(frame, pkt.payload + 1, n);  // pkt points into host_rx, which gw_rx may not touch
            gw_rx(&a->gw, frame, n, rssi);
            continue;
        }
        // the packet where it starts: host_rx_feed finds it after stray bytes too (#39); header 5 + checksum 2
        gw_host(&a->gw, pkt.payload - 5, pkt.len + 7);
    }
    if (a->halted) {
        return UINT32_MAX;
    }
    // the dongle clock runs on from the host's last time sync (0 until then, like the stock dongle)
    uint32_t now = clock_now_ms();
    if (a->gw.clock_ms) {
        a->gw.clock_ms += now - a->now_ms;
    }
    a->now_ms = now;
    gw_tick(&a->gw, now);
    if (got_rf) {
        a->rx_pulsing = true;
        a->rx_pulse_until = now + GW_RX_PULSE_MS;
    }
    if (a->rx_pulsing && (int32_t)(now - a->rx_pulse_until) >= 0) {
        a->rx_pulsing = false;
    }
    if (a->gw.permit) {
        if (!a->scan_was_on) {
            a->scan_was_on = true;
            a->scan_led_on = false;
            a->scan_next_ms = now + GW_SCAN_BLINK_MS;
        } else {
            while ((int32_t)(now - a->scan_next_ms) >= 0) {
                a->scan_led_on = !a->scan_led_on;
                a->scan_next_ms += GW_SCAN_BLINK_MS;
            }
        }
        board_led(BOARD_LED_OK, a->rx_pulsing ? !a->scan_led_on : a->scan_led_on);
    } else {
        a->scan_was_on = false;
        board_led(BOARD_LED_OK, !a->rx_pulsing);
    }
    return TICK_MS;
}
