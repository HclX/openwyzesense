#include "sniffer_app.h"

#include <stdio.h>
#include <string.h>

#include "board.h"
#include "radio.h"
#include "uart.h"
#include "wyze_app.h"

#ifndef OWS_VERSION
#define OWS_VERSION "unknown"
#endif

// AES self-test on the crypto engine: the E8 climate report in reference/tests/vectors/frames.txt
static bool aes_selftest(void)
{
    static const uint8_t ct[16] = {0x60, 0x19, 0x7d, 0xf5, 0xc5, 0xfd, 0xbd, 0xe9,
                                   0x50, 0x20, 0x4c, 0x70, 0xc5, 0x2d, 0x69, 0x73};
    uint8_t pt[16], back[16];
    aes128_ecb_decrypt(app_key, ct, pt, 16);
    aes128_ecb_encrypt(app_key, pt, back, 16);
    return pt[0] == APP_CLIMATE && !memcmp(pt + 1, "77C68217", 8) && !memcmp(back, ct, 16);
}

static void print_frame(const radio_frame_t *f)
{
    static const char hex[] = "0123456789abcdef";
    char line[32 + 2 * RADIO_MAX_FRAME + 2];
    int n = snprintf(line, 32, "RX %d %lu ", f->rssi, (unsigned long)f->rat_time);
    if (n < 0) {
        n = 0;
    } else if (n > 31) {
        n = 31;
    }
    uint8_t len = f->len <= RADIO_MAX_FRAME ? f->len : RADIO_MAX_FRAME;
    for (uint8_t i = 0; i < len; i++) {
        line[n++] = hex[f->frame[i] >> 4];
        line[n++] = hex[f->frame[i] & 15];
    }
    line[n++] = '\r';
    line[n++] = '\n';
    uart_write((const uint8_t *)line, (size_t)n);
}

void sniffer_app_init(sniffer_app_t *a)
{
    a->halted = false;
    uart_puts("\r\nOpenWyzeSense sniffer " OWS_VERSION " on " BOARD_NAME ", RX only\r\n");
    uart_puts(aes_selftest() ? "AES ok\r\n" : "AES FAILED\r\n");
    if (!radio_init() || !radio_rx_start()) {
        uart_puts("radio init FAILED\r\n");
        board_led(BOARD_LED_ERROR, 1);
        a->halted = true;
        return;
    }
    uart_puts("RX on 906.8 MHz\r\n");
    board_led(BOARD_LED_OK, 1);
}

uint32_t sniffer_app_step(sniffer_app_t *a)
{
    radio_frame_t f;
    if (a->halted) {
        return UINT32_MAX;
    }
    while (radio_poll(&f)) {
        print_frame(&f);
    }
    return UINT32_MAX;  // a frame wakes it
}
