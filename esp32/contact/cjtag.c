#include "cjtag.h"

#include <stdio.h>
#include <string.h>

// ICEPick-C 6-bit IR instructions (ti/icepick.cfg)
#define ICEPICK_IR_ROUTER  0x02u
#define ICEPICK_IR_CONNECT 0x07u
#define ICEPICK_IR_BYPASS  0x3Fu

// ARM CoreSight JTAG-DP 4-bit IR instructions
#define DAP_IR_ABORT  0x08u
#define DAP_IR_DPACC  0x0Au
#define DAP_IR_APACC  0x0Bu
#define DAP_IR_IDCODE 0x0Eu

// JTAG-DP 3-bit ACK responses (shifted out in bits [2:0] of 35-bit DPACC/APACC DR)
#define DAP_ACK_WAIT 0x01u
#define DAP_ACK_OK   0x02u

// DP register addresses (A[3:2])
#define DP_CTRL_STAT 0x04u
#define DP_SELECT    0x08u
#define DP_RDBUFF    0x0Cu

// AHB-AP register addresses (bank 0, A[3:2])
#define AP_CSW 0x00u
#define AP_TAR 0x04u
#define AP_DRW 0x0Cu

// AHB-AP CSW values: 32-bit access, master debug, auto-increment off (0x00) or single (0x10)
#define AP_CSW_32BIT_NOINC 0x23000002u
#define AP_CSW_32BIT_INC   0x23000012u

// Cortex-M3 SCS debug registers
#define CM3_DHCSR   0xE000EDF0u
#define CM3_DCRSR   0xE000EDF4u
#define CM3_DCRDR   0xE000EDF8u
#define CM3_DEMCR   0xE000EDFCu
#define CM3_AIRCR   0xE000ED0Cu
#define DHCSR_HALT  0xA05F0003u  // DBGKEY | C_HALT | C_DEBUGEN
#define DHCSR_RUN   0xA05F0001u  // DBGKEY | C_DEBUGEN (C_HALT cleared)
#define DHCSR_SHALT 0x00020000u  // S_HALT bit
#define DHCSR_SRDY  0x00010000u  // S_REGRDY bit

// CC1310 memory map for flash & ROM Hard-API
#define CC1310_ROM_HAPI_CRC32 0x10000048u  // HARD_API->Crc32
#define CC1310_ROM_HAPI_ERASE 0x10000054u  // HARD_API->ReservedLocation1 (HapiSectorErase)
#define CC1310_ROM_HAPI_PROG  0x1000005Cu  // HARD_API->ReservedLocation3 (HapiProgramFlash)
#define CC1310_VIMS_CTL       0x40034004u
#define CC1310_VIMS_STAT      0x40034000u
#define CC1310_FLASH_CFG      0x40030024u
#define CC1310_FLASH_FWLOCK   0x4003003Cu
#define CC1310_FLASH_FWFLAG   0x40030040u
#define CC1310_SRAM_TRAMP     0x20000000u  // bkpt #0 trampoline
#define CC1310_SRAM_BUF       0x20001000u  // 4 KB page buffer in SRAM
#define CC1310_SRAM_SP        0x20004000u  // stack pointer for ROM Hard-API calls

static bool fail(cjtag_t *c, const char *why)
{
    snprintf(c->error, sizeof c->error, "%s", why);
    return false;
}

static uint32_t crc32_ieee(const uint8_t *p, size_t n)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        crc ^= p[i];
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

void cjtag_init(cjtag_t *c, const cjtag_io_t *io)
{
    memset(c, 0, sizeof *c);
    c->io = *io;
    c->cur_dap_ir = 0xFFu;
}

// --- Layer 1: IEEE 1149.7 OScan1 physical signaling (decoded from XDS110 firmware_3.0.0.45.bin) ---

// Online-to-offline escape (0x151ec in XDS110): >=8 TMSC transitions while TCKC is held high resets the
// IEEE 1149.7 TAP adapter back to standard JScan0 mode.
void cjtag_reset_escape(cjtag_t *c)
{
    c->io.set_tms_dir(c->io.ctx, true);
    c->io.set_pins(c->io.ctx, true, false);
    c->io.delay_us(c->io.ctx, 2);
    for (int i = 0; i < 10; i++) {
        c->io.set_pins(c->io.ctx, true, true);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, false);
        c->io.delay_us(c->io.ctx, 1);
    }
    c->io.delay_us(c->io.ctx, 5);
}

// Standard 2-pin JScan0 TMS clock sequence (0xf184 in XDS110): 1 TCKC clock per TMS bit (LSB-first).
static void jscan0_tms(cjtag_t *c, uint64_t bits, unsigned count)
{
    c->io.set_tms_dir(c->io.ctx, true);
    for (unsigned i = 0; i < count; i++) {
        bool tms = ((bits >> i) & 1u) != 0;
        c->io.set_pins(c->io.ctx, false, tms);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, tms);
        c->io.delay_us(c->io.ctx, 1);
    }
}

// OScan1 TMS-only navigation packet (0xf1c8 in XDS110, mode=7): 3 TCKC clocks per bit (!TDI=0, TMS, TDO).
static void oscan1_tms(cjtag_t *c, uint32_t bits, unsigned count)
{
    for (unsigned i = 0; i < count; i++) {
        bool tms = ((bits >> i) & 1u) != 0;
        // Clock 1: !TDI = 0 (TDI = 1)
        c->io.set_tms_dir(c->io.ctx, true);
        c->io.set_pins(c->io.ctx, false, false);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, false);
        c->io.delay_us(c->io.ctx, 1);
        // Clock 2: TMS
        c->io.set_pins(c->io.ctx, false, tms);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, tms);
        c->io.delay_us(c->io.ctx, 1);
        // Clock 3: TDO (TMSC tri-stated)
        c->io.set_tms_dir(c->io.ctx, false);
        c->io.set_pins(c->io.ctx, false, false);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, false);
        c->io.delay_us(c->io.ctx, 1);
    }
    c->io.set_tms_dir(c->io.ctx, true);
}

// OScan1 Shift-IR / Shift-DR packet (0xfd70 in XDS110): shifts count bits (1..64) LSB-first.
// When exit_tms is true, TMS=1 on the final bit to transition Shift-xR -> Exit1-xR.
static uint64_t oscan1_shift(cjtag_t *c, uint64_t tdi_bits, unsigned count, bool exit_tms)
{
    uint64_t rx = 0;
    for (unsigned i = 0; i < count; i++) {
        bool ntdi = ((tdi_bits >> i) & 1u) == 0;
        bool tms = (i + 1 == count) && exit_tms;
        // Clock 1: !TDI (inverted TDI)
        c->io.set_tms_dir(c->io.ctx, true);
        c->io.set_pins(c->io.ctx, false, ntdi);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, ntdi);
        c->io.delay_us(c->io.ctx, 1);
        // Clock 2: TMS
        c->io.set_pins(c->io.ctx, false, tms);
        c->io.delay_us(c->io.ctx, 1);
        c->io.set_pins(c->io.ctx, true, tms);
        c->io.delay_us(c->io.ctx, 1);
        // Clock 3: TDO (target drives TMSC while TCKC is low; sample before rising edge)
        c->io.set_tms_dir(c->io.ctx, false);
        c->io.set_pins(c->io.ctx, false, false);
        c->io.delay_us(c->io.ctx, 1);
        if (c->io.get_tms(c->io.ctx)) {
            rx |= (1ULL << i);
        }
        c->io.set_pins(c->io.ctx, true, false);
        c->io.delay_us(c->io.ctx, 1);
    }
    c->io.set_tms_dir(c->io.ctx, true);
    return rx;
}

// Exact XDS110 CJTAG_Connect(7) OScan1 activation sequence (0x10db2..0x10fa4 in firmware_3.0.0.45.bin).
static void oscan1_enter(cjtag_t *c)
{
    cjtag_reset_escape(c);

    // Step B: JScan0 Zero-Bit Scan (ZBS) command level 2 unlock and OScan1 mode selection
    jscan0_tms(c, 0xFFu, 8);   // Test-Logic-Reset (8 clocks TMS=1)
    jscan0_tms(c, 0x00u, 1);   // Run-Test/Idle
    jscan0_tms(c, 0x03u, 3);   // Select-DR, Select-IR, Capture-IR
    jscan0_tms(c, 0x00u, 64);  // Shift-IR 64 clocks (internal TDI=1 pull-up -> BYPASS)
    jscan0_tms(c, 0x03u, 3);   // Exit1-IR, Update-IR, Run-Test/Idle
    jscan0_tms(c, 0x05u, 4);   // ZBS #1
    jscan0_tms(c, 0x03u, 2);   // Select-DR, Select-IR (Command Level 1)
    jscan0_tms(c, 0x05u, 4);   // ZBS #2
    jscan0_tms(c, 0x03u, 2);   // Select-DR, Select-IR (Command Level 2)
    jscan0_tms(c, 0x05u, 4);   // ZBS #3 (lock Command Level 2)
    jscan0_tms(c, 0x0Du, 4);   // 1-bit DR scan #1
    jscan0_tms(c, 0x05u, 4);
    jscan0_tms(c, 0x0Du, 4);  // 1-bit DR scan #2
    jscan0_tms(c, 0x05u, 4);
    jscan0_tms(c, 0x0Du, 4);  // 1-bit DR scan #3
    jscan0_tms(c, 0x05u, 4);
    jscan0_tms(c, 0x31u, 6);  // 3-bit DR scan
    jscan0_tms(c, 0x05u, 4);
    jscan0_tms(c, 0x1C01u, 13);  // 8-bit DR scan (OScan1 format = mode 7)
    jscan0_tms(c, 0x00u, 4);     // 4 clocks TMS=0

    // Step C: OScan1 control register finalization (0x10eee..0x10f9a)
    oscan1_tms(c, 0x02u, 3);
    oscan1_tms(c, 0x03u, 2);
    oscan1_tms(c, 0x15u, 6);
    oscan1_tms(c, 0x08u, 4);
    oscan1_tms(c, 0x01u, 2);
    jscan0_tms(c, 0x00u, 7);
    oscan1_tms(c, 0x05u, 4);
    oscan1_tms(c, 0x01u, 2);
    oscan1_tms(c, 0x40u, 7);
    oscan1_tms(c, 0x01u, 1);
    oscan1_tms(c, 0x05u, 4);
    oscan1_tms(c, 0x03u, 3);
    jscan0_tms(c, 0x00u, 7);
    oscan1_tms(c, 0x05u, 4);
    oscan1_tms(c, 0x03u, 2);
    oscan1_tms(c, 0x05u, 4);
    oscan1_tms(c, 0x01u, 2);
    oscan1_tms(c, 0x01u, 1);
    oscan1_tms(c, 0x01u, 2);
    jscan0_tms(c, 0x00u, 7);
}

// --- Layer 2: TI ICEPick-C Router Unlock ---

// Shift 6-bit IR and N-bit DR on ICEPick-C alone without entering Run-Test/Idle between router commands
// (as required by OpenOCD target/ti/icepick.cfg lines 103-105).
static void icepick_ir_dr_no_idle(cjtag_t *c, bool from_idle, uint8_t ir6, uint32_t dr, unsigned dr_bits)
{
    // Enter Shift-IR from either Run-Test/Idle (1,1,0,0) or Update-DR (1,1,0,0)
    (void)from_idle;
    oscan1_tms(c, 0x03u, 4);
    oscan1_shift(c, ir6 & 0x3Fu, 6, true);
    // Exit1-IR -> Pause-IR (0) -> Exit2-IR (1) -> Update-IR (1) -> Select-DR (1) -> Capture-DR (0) -> Shift-DR (0)
    oscan1_tms(c, 0x0Eu, 6);
    oscan1_shift(c, dr, dr_bits, true);
    // Exit1-DR -> Pause-DR (0) -> Exit2-DR (1) -> Update-DR (1)
    oscan1_tms(c, 0x06u, 3);
}

// Shift 10-bit combined IR (CS_DAP 4 bits + ICEPick-C 6 bits in BYPASS) from Run-Test/Idle -> Run-Test/Idle.
static void chain_shift_ir(cjtag_t *c, uint8_t dap_ir)
{
    if (c->cur_dap_ir == dap_ir) {
        return;
    }
    uint32_t tx_ir = c->dap_at_tdo ? ((ICEPICK_IR_BYPASS << 4) | (dap_ir & 0x0Fu))
                                   : (((uint32_t)(dap_ir & 0x0Fu) << 6) | ICEPICK_IR_BYPASS);
    oscan1_tms(c, 0x03u, 4);  // Idle -> Select-DR (1), Select-IR (1), Capture-IR (0), Shift-IR (0)
    oscan1_shift(c, tx_ir, 10, true);
    oscan1_tms(c, 0x01u, 2);  // Exit1-IR -> Update-IR (1), Run-Test/Idle (0)
    c->cur_dap_ir = dap_ir;
}

// Shift (dap_bits + 1) combined DR (CS_DAP + 1-bit ICEPick-C bypass) from Run-Test/Idle -> Run-Test/Idle.
static uint64_t chain_shift_dr(cjtag_t *c, uint64_t dap_tx, unsigned dap_bits)
{
    uint64_t tx_dr = c->dap_at_tdo ? dap_tx : (dap_tx << 1);
    oscan1_tms(c, 0x01u, 3);  // Idle -> Select-DR (1), Capture-DR (0), Shift-DR (0)
    uint64_t rx_dr = oscan1_shift(c, tx_dr, dap_bits + 1, true);
    oscan1_tms(c, 0x01u, 2);  // Exit1-DR -> Update-DR (1), Run-Test/Idle (0)
    uint64_t mask = (dap_bits == 64) ? ~0ULL : ((1ULL << dap_bits) - 1ULL);
    return c->dap_at_tdo ? (rx_dr & mask) : ((rx_dr >> 1) & mask);
}

static bool icepick_connect_dap(cjtag_t *c)
{
    // Read 32-bit ICEPick-C IDCODE (default DR in Run-Test/Idle right after OScan1 entry)
    oscan1_tms(c, 0x01u, 3);  // Idle -> Select-DR, Capture-DR, Shift-DR
    uint32_t idcode = (uint32_t)oscan1_shift(c, 0, 32, true);
    oscan1_tms(c, 0x01u, 2);  // Exit1-DR -> Update-DR, Idle

    c->icepick_id = idcode;
    if ((idcode & 0x0FFFFFFFu) != CJTAG_ICEPICK_IDCODE) {
        return fail(c, "no CC1310 ICEPick-C IDCODE on probe pins");
    }

    // Unlock ICEPick-C and link Debug TAP 0 (Cortex-M3 CS_DAP) without touching Run-Test/Idle on the way
    icepick_ir_dr_no_idle(c, true, ICEPICK_IR_CONNECT, 0x89u, 8);
    icepick_ir_dr_no_idle(c, false, ICEPICK_IR_ROUTER, 0x81002100u, 32);  // SysCtrl: force power/clock active
    icepick_ir_dr_no_idle(c, false, ICEPICK_IR_ROUTER, 0xA0110048u, 32);  // TAP 0: power & clock enable
    icepick_ir_dr_no_idle(c, false, ICEPICK_IR_ROUTER, 0xA0112048u, 32);  // TAP 0: debug enable
    icepick_ir_dr_no_idle(c, false, ICEPICK_IR_ROUTER, 0xA0112148u, 32);  // TAP 0: select & link into chain

    // From Update-DR, load ICEPick BYPASS (0x3F) and enter Run-Test/Idle for 10 clocks
    oscan1_tms(c, 0x03u, 4);  // Update-DR -> Select-DR (1), Select-IR (1), Capture-IR (0), Shift-IR (0)
    oscan1_shift(c, ICEPICK_IR_BYPASS, 6, true);
    oscan1_tms(c, 0x01u, 2);   // Exit1-IR -> Update-IR (1), Run-Test/Idle (0)
    oscan1_tms(c, 0x00u, 10);  // 10 Run-Test/Idle clocks

    // Auto-detect whether CS_DAP (32-bit IDCODE = 0x4BA00477) or ICEPick-C (1-bit bypass) is closest to TDO
    oscan1_tms(c, 0x01u, 3);
    uint64_t raw33 = oscan1_shift(c, 0, 33, true);
    oscan1_tms(c, 0x01u, 2);

    if ((uint32_t)((raw33 >> 1) & 0xFFFFFFFFULL) == CJTAG_DAP_IDCODE) {
        c->dap_at_tdo = false;
        c->dap_id = CJTAG_DAP_IDCODE;
        c->cur_dap_ir = DAP_IR_IDCODE;
        return true;
    }
    if ((uint32_t)(raw33 & 0xFFFFFFFFULL) == CJTAG_DAP_IDCODE) {
        c->dap_at_tdo = true;
        c->dap_id = CJTAG_DAP_IDCODE;
        c->cur_dap_ir = DAP_IR_IDCODE;
        return true;
    }

    // Try explicit DAP_IR_IDCODE under both chain orders
    for (int order = 0; order < 2; order++) {
        c->dap_at_tdo = (order == 1);
        c->cur_dap_ir = 0xFFu;
        chain_shift_ir(c, DAP_IR_IDCODE);
        uint32_t id = (uint32_t)chain_shift_dr(c, 0, 32);
        if (id == CJTAG_DAP_IDCODE) {
            c->dap_id = id;
            return true;
        }
    }
    return fail(c, "ICEPick connected, but ARM CS_DAP IDCODE (0x4BA00477) not found");
}

// --- Layer 3: ARM CoreSight JTAG-DP & AHB-AP ---

// Perform one 35-bit DPACC or APACC scan with WAIT retry.
// addr is byte address (bits [3:2] placed in DR[2:1], rnw in DR[0], 32-bit data in DR[34:3]).
static bool dap_scan35(cjtag_t *c, uint8_t ir, uint8_t addr, bool rnw, uint32_t tx_data, uint32_t *rx_data)
{
    chain_shift_ir(c, ir);
    uint64_t req = ((uint64_t)tx_data << 3) | (uint64_t)(addr & 0x0Cu) >> 1 | (rnw ? 1ULL : 0ULL);
    for (int tries = 0; tries < 64; tries++) {
        uint64_t resp = chain_shift_dr(c, req, 35);
        uint8_t ack = (uint8_t)(resp & 0x07u);
        if (ack == DAP_ACK_OK) {
            if (rx_data) {
                *rx_data = (uint32_t)(resp >> 3);
            }
            return true;
        }
        if (ack != DAP_ACK_WAIT) {
            // Clear sticky error bits via ABORT (DAPABORT=1, STKERRCLR=1)
            chain_shift_ir(c, DAP_IR_ABORT);
            chain_shift_dr(c, 0x04u, 35);
            return fail(c, "CoreSight JTAG-DP ACK fault");
        }
    }
    return fail(c, "CoreSight JTAG-DP WAIT timeout");
}

static bool dp_write(cjtag_t *c, uint8_t reg_addr, uint32_t val)
{
    return dap_scan35(c, DAP_IR_DPACC, reg_addr, false, val, NULL);
}

static bool dp_read(cjtag_t *c, uint8_t reg_addr, uint32_t *val)
{
    // Pipelined JTAG-DP read: first scan initiates the read, second scan (RDBUFF) shifts out the result
    return dap_scan35(c, DAP_IR_DPACC, reg_addr, true, 0, NULL) && dap_scan35(c, DAP_IR_DPACC, DP_RDBUFF, true, 0, val);
}

static bool ap_write(cjtag_t *c, uint8_t reg_addr, uint32_t val)
{
    return dap_scan35(c, DAP_IR_APACC, reg_addr, false, val, NULL);
}

static bool ap_read(cjtag_t *c, uint8_t reg_addr, uint32_t *val)
{
    return dap_scan35(c, DAP_IR_APACC, reg_addr, true, 0, NULL) && dap_scan35(c, DAP_IR_DPACC, DP_RDBUFF, true, 0, val);
}

static bool set_ap_csw(cjtag_t *c, uint32_t csw)
{
    if (c->ap_csw == csw) {
        return true;
    }
    if (!ap_write(c, AP_CSW, csw)) {
        return false;
    }
    c->ap_csw = csw;
    return true;
}

static bool dap_power_up(cjtag_t *c)
{
    // Clear any sticky errors in CTRL/STAT
    chain_shift_ir(c, DAP_IR_ABORT);
    chain_shift_dr(c, 0x1Eu, 35);

    // Request debug and system power-up (CDBGPWRUPREQ | CSYSPWRUPREQ = 0x50000000)
    if (!dp_write(c, DP_CTRL_STAT, 0x50000000u)) {
        return false;
    }
    uint32_t stat = 0;
    for (int i = 0; i < 50; i++) {
        if (!dp_read(c, DP_CTRL_STAT, &stat)) {
            return false;
        }
        if ((stat & 0xF0000000u) == 0xF0000000u) {
            break;
        }
        c->io.delay_us(c->io.ctx, 100);
    }
    if ((stat & 0xF0000000u) != 0xF0000000u) {
        return fail(c, "JTAG-DP debug/system power-up ACK timeout");
    }

    // Select AP 0 (Cortex-M3 AHB-AP), bank 0
    if (!dp_write(c, DP_SELECT, 0x00000000u)) {
        return false;
    }
    c->ap_csw = 0;
    return set_ap_csw(c, AP_CSW_32BIT_NOINC);
}

bool cjtag_mem_read32(cjtag_t *c, uint32_t addr, uint32_t *val)
{
    return set_ap_csw(c, AP_CSW_32BIT_NOINC) && ap_write(c, AP_TAR, addr) && ap_read(c, AP_DRW, val);
}

bool cjtag_mem_write32(cjtag_t *c, uint32_t addr, uint32_t val)
{
    // Follow AP_DRW write with a dummy DP_RDBUFF read to ensure the AHB write completes before returning
    uint32_t dummy = 0;
    return set_ap_csw(c, AP_CSW_32BIT_NOINC) && ap_write(c, AP_TAR, addr) && ap_write(c, AP_DRW, val) &&
           dap_scan35(c, DAP_IR_DPACC, DP_RDBUFF, true, 0, &dummy);
}

// Pipelined block read using AHB-AP single auto-increment (wraps at 1 KB TAR boundaries).
bool cjtag_mem_read_block(cjtag_t *c, uint32_t addr, uint8_t *out, size_t len)
{
    if ((addr & 3u) != 0 || (len & 3u) != 0) {
        return fail(c, "unaligned block read");
    }
    if (!set_ap_csw(c, AP_CSW_32BIT_INC)) {
        return false;
    }
    size_t off = 0;
    while (off < len) {
        uint32_t cur = addr + (uint32_t)off;
        // Do not cross a 1 KB (0x400) TAR auto-increment boundary in a single burst
        size_t chunk_max = 0x400u - (cur & 0x3FFu);
        size_t chunk = (len - off < chunk_max) ? (len - off) : chunk_max;
        size_t words = chunk / 4u;

        if (!ap_write(c, AP_TAR, cur)) {
            return false;
        }
        // Prime the pipeline with the first AP_DRW read
        if (!dap_scan35(c, DAP_IR_APACC, AP_DRW, true, 0, NULL)) {
            return false;
        }
        for (size_t w = 0; w < words; w++) {
            uint32_t word = 0;
            bool ok = (w + 1 < words) ? dap_scan35(c, DAP_IR_APACC, AP_DRW, true, 0, &word)
                                      : dap_scan35(c, DAP_IR_DPACC, DP_RDBUFF, true, 0, &word);
            if (!ok) {
                return false;
            }
            out[off + w * 4 + 0] = (uint8_t)word;
            out[off + w * 4 + 1] = (uint8_t)(word >> 8);
            out[off + w * 4 + 2] = (uint8_t)(word >> 16);
            out[off + w * 4 + 3] = (uint8_t)(word >> 24);
        }
        off += chunk;
    }
    return true;
}

bool cjtag_mem_write_block(cjtag_t *c, uint32_t addr, const uint8_t *data, size_t len)
{
    if ((addr & 3u) != 0 || (len & 3u) != 0) {
        return fail(c, "unaligned block write");
    }
    if (!set_ap_csw(c, AP_CSW_32BIT_INC)) {
        return false;
    }
    size_t off = 0;
    while (off < len) {
        uint32_t cur = addr + (uint32_t)off;
        size_t chunk_max = 0x400u - (cur & 0x3FFu);
        size_t chunk = (len - off < chunk_max) ? (len - off) : chunk_max;
        size_t words = chunk / 4u;

        if (!ap_write(c, AP_TAR, cur)) {
            return false;
        }
        for (size_t w = 0; w < words; w++) {
            const uint8_t *p = data + off + w * 4;
            uint32_t word = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
            if (!ap_write(c, AP_DRW, word)) {
                return false;
            }
        }
        uint32_t dummy = 0;
        if (!dap_scan35(c, DAP_IR_DPACC, DP_RDBUFF, true, 0, &dummy)) {
            return false;
        }
        off += chunk;
    }
    return true;
}

// --- Layer 4: Cortex-M3 Halt, Core Registers & CC1310 Flash Engine ---

static bool cpu_halt(cjtag_t *c)
{
    if (!cjtag_mem_write32(c, CM3_DHCSR, DHCSR_HALT)) {
        return false;
    }
    for (int i = 0; i < 50; i++) {
        uint32_t dhcsr = 0;
        if (!cjtag_mem_read32(c, CM3_DHCSR, &dhcsr)) {
            return false;
        }
        if ((dhcsr & DHCSR_SHALT) != 0) {
            return true;
        }
        c->io.delay_us(c->io.ctx, 50);
    }
    return fail(c, "Cortex-M3 failed to halt");
}

static bool cpu_write_reg(cjtag_t *c, uint8_t reg, uint32_t val)
{
    if (!cjtag_mem_write32(c, CM3_DCRDR, val) || !cjtag_mem_write32(c, CM3_DCRSR, 0x00010000u | reg)) {
        return false;
    }
    for (int i = 0; i < 50; i++) {
        uint32_t dhcsr = 0;
        if (!cjtag_mem_read32(c, CM3_DHCSR, &dhcsr)) {
            return false;
        }
        if ((dhcsr & DHCSR_SRDY) != 0) {
            return true;
        }
    }
    return fail(c, "Cortex-M3 DCRSR write timeout");
}

static bool cpu_read_reg(cjtag_t *c, uint8_t reg, uint32_t *val)
{
    if (!cjtag_mem_write32(c, CM3_DCRSR, reg)) {
        return false;
    }
    for (int i = 0; i < 50; i++) {
        uint32_t dhcsr = 0;
        if (!cjtag_mem_read32(c, CM3_DHCSR, &dhcsr)) {
            return false;
        }
        if ((dhcsr & DHCSR_SRDY) != 0) {
            return cjtag_mem_read32(c, CM3_DCRDR, val);
        }
    }
    return fail(c, "Cortex-M3 DCRSR read timeout");
}

// Call a CC1310 ROM Hard-API function on the halted Cortex-M3 with interrupts disabled and a bkpt #0 trampoline
// at 0x20000000 in SRAM, then wait for the CPU to hit the breakpoint and return R0.
static bool rom_hapi_call(cjtag_t *c, uint32_t ptr_addr, uint32_t r0, uint32_t r1, uint32_t r2, uint32_t timeout_ms,
                          uint32_t *ret_r0)
{
    uint32_t func_addr = 0;
    if (!cjtag_mem_read32(c, ptr_addr, &func_addr) || (func_addr & 0xFFF00001u) != 0x10000001u) {
        return fail(c, "invalid CC1310 ROM Hard-API pointer");
    }
    // Write `bkpt #0; nop` (0xBE00, 0xBF00 -> 0xBF00BE00 little-endian) at 0x20000000
    if (!cjtag_mem_write32(c, CC1310_SRAM_TRAMP, 0xBF00BE00u)) {
        return false;
    }
    // Set PRIMASK=1 (reg 20), SP, LR=0x20000001 (bkpt #0 in Thumb), xPSR=0x01000000 (Thumb), R0..R2, PC=func_addr
    if (!cpu_write_reg(c, 20, 0x00000001u) || !cpu_write_reg(c, 13, CC1310_SRAM_SP) ||
        !cpu_write_reg(c, 14, CC1310_SRAM_TRAMP | 1u) || !cpu_write_reg(c, 16, 0x01000000u) ||
        !cpu_write_reg(c, 0, r0) || !cpu_write_reg(c, 1, r1) || !cpu_write_reg(c, 2, r2) ||
        !cpu_write_reg(c, 15, func_addr)) {
        return false;
    }
    // Resume Cortex-M3 (C_DEBUGEN=1, C_HALT=0)
    if (!cjtag_mem_write32(c, CM3_DHCSR, DHCSR_RUN)) {
        return false;
    }
    uint32_t loops = timeout_ms * 10u;
    for (uint32_t i = 0; i <= loops; i++) {
        uint32_t dhcsr = 0;
        if (!cjtag_mem_read32(c, CM3_DHCSR, &dhcsr)) {
            return false;
        }
        if ((dhcsr & DHCSR_SHALT) != 0) {
            return cpu_read_reg(c, 0, ret_r0);
        }
        c->io.delay_us(c->io.ctx, 100);
    }
    (void)cpu_halt(c);
    return fail(c, "CC1310 ROM Hard-API call timed out");
}

static bool is_ascii_hex_mac(const uint8_t *p)
{
    for (int i = 0; i < 8; i++) {
        if (!((p[i] >= '0' && p[i] <= '9') || (p[i] >= 'A' && p[i] <= 'F'))) {
            return false;
        }
    }
    return memcmp(p, "00000000", 8) != 0;
}

bool cjtag_connect(cjtag_t *c, bool halt_cpu)
{
    c->error[0] = 0;
    c->cur_dap_ir = 0xFFu;
    c->ap_csw = 0;
    oscan1_enter(c);
    if (!icepick_connect_dap(c) || !dap_power_up(c)) {
        return false;
    }
    if (halt_cpu && !cpu_halt(c)) {
        return false;
    }

    // Read FCFG1 USER_ID (0x50001294) and 64-bit IEEE 802.15.4 MAC (0x500012F0..0x500012F4)
    uint32_t mac_lo = 0, mac_hi = 0;
    if (!cjtag_mem_read32(c, 0x50001294u, &c->user_id) || !cjtag_mem_read32(c, 0x500012F0u, &mac_lo) ||
        !cjtag_mem_read32(c, 0x500012F4u, &mac_hi)) {
        return fail(c, "failed to read FCFG1 identification registers");
    }
    c->ieee_mac = ((uint64_t)mac_hi << 32) | mac_lo;

    // Read Page 31 (CCFG) to capture its baseline CRC32, BL_CONFIG, and CCFG ASCII MAC
    uint8_t page31[CJTAG_PAGE] = {0};
    if (!cjtag_mem_read_block(c, CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, page31, sizeof page31)) {
        return fail(c, "failed to read page 31 (CCFG)");
    }
    c->page31_crc = crc32_ieee(page31, sizeof page31);

    const uint8_t *ccfg = page31 + (CJTAG_CCFG_ADDR - (CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE));
    const uint8_t *bl = ccfg + 0x30;
    c->bl_config = (uint32_t)bl[0] | ((uint32_t)bl[1] << 8) | ((uint32_t)bl[2] << 16) | ((uint32_t)bl[3] << 24);

    const uint8_t *ccfg_mac = ccfg + 0x24;  // 0x0001FFCC
    if (is_ascii_hex_mac(ccfg_mac)) {
        memcpy(c->mac, ccfg_mac, 8);
        c->mac[8] = 0;
        c->has_ccfg_mac = true;
    } else {
        c->has_ccfg_mac = false;
        // Stock v2 climate/motion sensors store their 8-char ASCII MAC at 0x0001D000
        uint8_t p29_mac[8];
        if (cjtag_mem_read_block(c, 0x0001D000u, p29_mac, sizeof p29_mac) && is_ascii_hex_mac(p29_mac)) {
            memcpy(c->mac, p29_mac, 8);
            c->mac[8] = 0;
        } else {
            snprintf(c->mac, sizeof c->mac, "%08X", (unsigned)mac_lo);
        }
    }
    return true;
}

bool cjtag_dump_page(cjtag_t *c, uint32_t page_idx, uint8_t *out_4k)
{
    if (page_idx >= CJTAG_TOTAL_PAGES) {
        return fail(c, "page index out of range (0..31)");
    }
    uint8_t verify_buf[CJTAG_PAGE] = {0};
    uint32_t addr = page_idx * CJTAG_PAGE;
    if (!cjtag_mem_read_block(c, addr, out_4k, CJTAG_PAGE) || !cjtag_mem_read_block(c, addr, verify_buf, CJTAG_PAGE)) {
        return false;
    }
    if (memcmp(out_4k, verify_buf, CJTAG_PAGE) != 0) {
        return fail(c, "page read mismatch between pass 1 and pass 2 (probe glitch)");
    }
    return true;
}

bool cjtag_dump_fcfg1(cjtag_t *c, uint8_t *out_1k)
{
    uint8_t verify_buf[CJTAG_FCFG1_SIZE] = {0};
    if (!cjtag_mem_read_block(c, CJTAG_FCFG1_ADDR, out_1k, CJTAG_FCFG1_SIZE) ||
        !cjtag_mem_read_block(c, CJTAG_FCFG1_ADDR, verify_buf, CJTAG_FCFG1_SIZE)) {
        return false;
    }
    if (memcmp(out_1k, verify_buf, CJTAG_FCFG1_SIZE) != 0) {
        return fail(c, "FCFG1 read mismatch between pass 1 and pass 2");
    }
    return true;
}

static bool vector_table_ok(const uint8_t *p)
{
    uint32_t sp = (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
    uint32_t reset = (uint32_t)p[4] | ((uint32_t)p[5] << 8) | ((uint32_t)p[6] << 16) | ((uint32_t)p[7] << 24);
    return sp > 0x20000000u && sp <= 0x20005000u && (reset & 1u) != 0 && reset < CJTAG_APP_END;
}

static bool is_all_ff(const uint8_t *p, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != 0xFFu) {
            return false;
        }
    }
    return true;
}

bool cjtag_flash_page(cjtag_t *c, uint32_t page_idx, const uint8_t *data_4k, bool allow_nv)
{
    // Hard safety rule: NEVER erase or write page 31 (CCFG + factory MAC)
    if (page_idx >= CJTAG_CCFG_PAGE_IDX) {
        return fail(c, "refused: page 31 (CCFG) may never be erased or written");
    }
    if (page_idx == CJTAG_NV_PAGE_IDX && !allow_nv) {
        return fail(c, "refused: page 30 (NV) requires allow_nv");
    }
    if (!c->has_ccfg_mac) {
        return fail(c, "no valid ASCII MAC in CCFG (page 31): refusing to flash");
    }
    if (page_idx == 0 && !vector_table_ok(data_4k)) {
        return fail(c, "page 0 does not start with a valid CC1310 Cortex-M3 vector table");
    }
    if (!cpu_halt(c)) {
        return false;
    }

    // Disable VIMS flash cache so flash reads and ROM flash operations see raw flash state
    if (!cjtag_mem_write32(c, CC1310_VIMS_CTL, 0x00000001u)) {  // MODE = OFF (0x1) or GPRAM (0x0)
        return false;
    }

    // Force flash trim before erase/program in case the chip was halted early in boot (cc26x0_xds.gel)
    uint32_t fwflag = 0;
    if (!cjtag_mem_write32(c, CC1310_FLASH_FWLOCK, 0x00000005u) || !cjtag_mem_read32(c, CC1310_FLASH_FWFLAG, &fwflag) ||
        !cjtag_mem_write32(c, CC1310_FLASH_FWFLAG, fwflag & ~1u) ||
        !cjtag_mem_write32(c, CC1310_FLASH_FWLOCK, 0x00000000u)) {
        return false;
    }

    uint32_t sector_addr = page_idx * CJTAG_PAGE;
    uint32_t status = 0xFFFFFFFFu;
    if (!rom_hapi_call(c, CC1310_ROM_HAPI_ERASE, sector_addr, 0, 0, 500, &status) || status != 0) {
        return fail(c, "CC1310 HapiSectorErase failed");
    }

    if (!is_all_ff(data_4k, CJTAG_PAGE)) {
        if (!cjtag_mem_write_block(c, CC1310_SRAM_BUF, data_4k, CJTAG_PAGE)) {
            return fail(c, "failed to load 4 KB page buffer into CC1310 SRAM");
        }
        status = 0xFFFFFFFFu;
        if (!rom_hapi_call(c, CC1310_ROM_HAPI_PROG, CC1310_SRAM_BUF, sector_addr, CJTAG_PAGE, 500, &status) ||
            status != 0) {
            return fail(c, "CC1310 HapiProgramFlash failed");
        }
    }

    // Clear FLASH.CFG.DIS_STANDBY (bit 1) as driverlib FlashSectorErase/FlashProgram do
    uint32_t fcfg = 0;
    if (cjtag_mem_read32(c, CC1310_FLASH_CFG, &fcfg)) {
        (void)cjtag_mem_write32(c, CC1310_FLASH_CFG, fcfg & ~0x02u);
    }
    // Re-enable VIMS cache
    (void)cjtag_mem_write32(c, CC1310_VIMS_CTL, 0x00000000u);

    // Read back the 4 KB page and verify every byte matches
    uint8_t verify_buf[CJTAG_PAGE] = {0};
    if (!cjtag_mem_read_block(c, sector_addr, verify_buf, CJTAG_PAGE) || memcmp(verify_buf, data_4k, CJTAG_PAGE) != 0) {
        return fail(c, "flash readback verification failed");
    }

    // Verify page 31 (CCFG) is completely untouched
    if (!cjtag_mem_read_block(c, CJTAG_CCFG_PAGE_IDX * CJTAG_PAGE, verify_buf, CJTAG_PAGE) ||
        crc32_ieee(verify_buf, CJTAG_PAGE) != c->page31_crc) {
        return fail(c, "page 31 CRC changed!");
    }
    return true;
}

void cjtag_release_and_reset(cjtag_t *c)
{
    // Clear DEMCR vector catch, unhalt CPU, and trigger Cortex-M3 VECTRESET (0x05FA0001) so the ROM boot code
    // does not enter the Halt-In-Boot (HIB) wait loop (chip.c line 84 / ti_cc26x0.cfg line 57).
    (void)cjtag_mem_write32(c, CM3_DEMCR, 0x00000000u);
    (void)cjtag_mem_write32(c, CM3_DHCSR, DHCSR_RUN);
    (void)cjtag_mem_write32(c, CM3_AIRCR, 0x05FA0001u);
    (void)dp_write(c, DP_CTRL_STAT, 0x00000000u);
    cjtag_reset_escape(c);
}
