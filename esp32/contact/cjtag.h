// 2-pin IEEE 1149.7 cJTAG (OScan1) + TI ICEPick-C + ARM CoreSight JTAG-DP/AHB-AP + CC1310 flash engine (issue #69).
// Operates over two GPIOs only (M = TMSC, C = TCKC; no RESET_N or W strap needed), with auto-polarity detection on
// the two probe pins and page-by-page dump/flash support so hand-held probes can slip and resume without restarting.
// Hard rule (CLAUDE.md / AGENTS.md): page 31 (0x0001F000..0x0001FFFF, CCFG + factory MAC) is NEVER erased or written.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define CJTAG_PAGE           0x1000u
#define CJTAG_TOTAL_PAGES    32u
#define CJTAG_FLASH_SIZE     (CJTAG_PAGE * CJTAG_TOTAL_PAGES)  // 128 KB
#define CJTAG_APP_PAGES      30u                               // pages 0-29 (0x00000..0x1DFFF)
#define CJTAG_APP_END        (CJTAG_PAGE * CJTAG_APP_PAGES)    // 0x1E000
#define CJTAG_NV_PAGE_IDX    30u                               // page 30 (0x1E000)
#define CJTAG_CCFG_PAGE_IDX  31u                               // page 31 (0x1F000): never erased or written
#define CJTAG_CCFG_ADDR      0x0001FFA8u
#define CJTAG_FCFG1_ADDR     0x50001000u
#define CJTAG_FCFG1_SIZE     0x00000400u  // 1 KB factory configuration
#define CJTAG_ICEPICK_IDCODE 0x0B9BE02Fu  // CC13x0 ICEPick-C IDCODE (ignoring 4-bit version)
#define CJTAG_DAP_IDCODE     0x4BA00477u  // ARM Cortex-M3 CoreSight JTAG-DP IDCODE

typedef struct {
    void (*set_pins)(void *ctx, bool tck, bool tms);
    void (*set_tms_dir)(void *ctx, bool output);
    bool (*get_tms)(void *ctx);
    void (*delay_us)(void *ctx, uint32_t us);
    void *ctx;
} cjtag_io_t;

typedef struct {
    cjtag_io_t io;
    bool dap_at_tdo;     // true if CS_DAP is closest to TDO, false if ICEPick-C is closest to TDO
    uint8_t cur_dap_ir;  // cached 4-bit JTAG-DP IR (0xFF when unknown)
    uint32_t ap_csw;     // cached AHB-AP CSW register
    uint32_t icepick_id;
    uint32_t dap_id;
    uint32_t user_id;     // FCFG1 USER_ID (0x50001294)
    uint64_t ieee_mac;    // FCFG1 64-bit IEEE 802.15.4 MAC (0x500012F0)
    char mac[9];          // 8-char uppercase hex sensor MAC + NUL
    bool has_ccfg_mac;    // true if page 31 CCFG + 0x24 holds a valid 8-char ASCII hex MAC
    uint32_t bl_config;   // CCFG BL_CONFIG word at 0x0001FFD8
    uint32_t page31_crc;  // IEEE CRC32 of page 31 read at connect time
    char error[96];       // human-readable reason for the last error
} cjtag_t;

void cjtag_init(cjtag_t *c, const cjtag_io_t *io);

// Reset the target's IEEE 1149.7 adapter back to offline JScan0 mode (>=8 TMSC toggles while TCKC=1).
void cjtag_reset_escape(cjtag_t *c);

// Enter 2-pin IEEE 1149.7 OScan1 mode using the exact XDS110 non-adaptive OScan1 sequence, unlock ICEPick-C,
// link the Cortex-M3 CS_DAP into the scan chain, power up the debug domain, and halt the CPU.
// Fast-fails if no CC1310 is on the pins (used for 150 ms hands-free polling and auto-polarity detection).
bool cjtag_connect(cjtag_t *c, bool halt_cpu);

// Read/write a single 32-bit aligned memory word through the Cortex-M3 AHB-AP.
bool cjtag_mem_read32(cjtag_t *c, uint32_t addr, uint32_t *val);
bool cjtag_mem_write32(cjtag_t *c, uint32_t addr, uint32_t val);

// Read/write a block of 32-bit aligned bytes through AHB-AP auto-increment (len must be a multiple of 4).
bool cjtag_mem_read_block(cjtag_t *c, uint32_t addr, uint8_t *out, size_t len);
bool cjtag_mem_write_block(cjtag_t *c, uint32_t addr, const uint8_t *data, size_t len);

// Read a 4 KB flash page (0..31) twice and verify both passes match byte-for-byte so a hand-held probe glitch
// can never corrupt a dump page.
bool cjtag_dump_page(cjtag_t *c, uint32_t page_idx, uint8_t *out_4k);

// Read the 1 KB FCFG1 factory configuration region (0x50001000..0x500013FF) twice and verify both passes match.
bool cjtag_dump_fcfg1(cjtag_t *c, uint8_t *out_1k);

// Erase and program a single 4 KB flash page via the CC1310 ROM Hard-API in SRAM, then read back and verify all
// 4096 bytes and confirm page 31's CRC is unchanged.
// Hard rule: page_idx >= 31 is unconditionally refused; page_idx == 30 requires allow_nv == true.
bool cjtag_flash_page(cjtag_t *c, uint32_t page_idx, const uint8_t *data_4k, bool allow_nv);

// Release the debug domain, trigger a Cortex-M3 VECTRESET so the chip boots cleanly without hanging in ROM
// Halt-In-Boot (HIB), and return the cJTAG adapter to offline mode.
void cjtag_release_and_reset(cjtag_t *c);
