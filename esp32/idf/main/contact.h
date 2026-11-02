// The v1 contact board the relay serves (docs/v1-contact.md; wiring: esp32/README.md): its UART (the gateway's host
// link, and the ROM bootloader's), its S pad (RESET_N) and W pad (DIO10: low at reset = the ROM bootloader, backdoor
// `w`, #14), both open-drain.
#pragma once

#include "gwnv.h"
#include "gwsync.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void contact_init(void);

// A reset into its firmware (W released)
void contact_reset(void);

// A reset into the ROM bootloader (W low across the reset, then released: the ROM looks at it at the reset only)
void contact_enter_bootloader(void);

// The UART is shared: the relay uses it unless a bootloader session holds it
bool contact_uart_take(uint32_t wait_ms);
void contact_uart_give(void);

size_t contact_uart_read(uint8_t *buf, size_t cap, uint32_t timeout_ms);
bool contact_uart_write(const uint8_t *data, size_t len);
void contact_uart_flush_input(void);
void contact_uart_counts(uint32_t *tx, uint32_t *rx);

// Reflash the contact (pages 0-29: image, len) through its ROM bootloader (esp32/contact/sbl.c): the UART taken from
// the relay, W + S into the bootloader, the CCFG checked (expect_mac if not NULL), written and verified, page 30
// wiped if wipe_nv, then a reset into the new firmware and the UART given back. msg: what happened.
bool contact_flash(const uint8_t *image, size_t len, const char *expect_mac, bool wipe_nv, char *msg, size_t cap);

// Streaming variant of contact_flash() (4 KB page buffer, zero 120 KB heap allocation): begin, feed chunks, then end.
bool contact_flash_begin(const char *expect_mac, bool wipe_nv, char *msg, size_t cap);
bool contact_flash_write(const uint8_t *data, size_t len);
bool contact_flash_end(bool rx_ok, char *msg, size_t cap);

// At the start, before anything is relayed: the contact on the gateway image this firmware carries (#76,
// esp32/contact/gwsync.c): asked for its image ID, reflashed if it runs another image. Takes the UART meanwhile.
void contact_gwsync(void);

// What it found (NULL before it ran)
const gwsync_report_t *contact_gwsync_report(void);

// The check's last steps ("<ms since start> <step>"), oldest first, a line each: while it runs, too
size_t contact_gwsync_steps(char *out, size_t cap);

// Runtime NV export / zero-ladder migration over the contact's UART (OWS_SUB_NETWORK + OWS_SUB_NV_IMPORT, TASK-14).
bool contact_nv_export(char *json_out, size_t json_cap, char *msg, size_t msg_cap);
bool contact_nv_import(const gwnv_doc_t *doc, char *msg, size_t msg_cap);
