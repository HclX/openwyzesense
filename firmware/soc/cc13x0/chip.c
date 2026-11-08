#include "chip.h"

#include <stdbool.h>

#include "radio.h"
#include "uart.h"

#include <ti/devices/DeviceFamily.h>
// clang-format off: a TI include path, not a division
#include DeviceFamily_constructPath(driverlib/aon_batmon.h)
#include DeviceFamily_constructPath(driverlib/ioc.h)
#include DeviceFamily_constructPath(driverlib/prcm.h)
#include DeviceFamily_constructPath(driverlib/rfc.h)
#include DeviceFamily_constructPath(driverlib/watchdog.h)
#include DeviceFamily_constructPath(inc/hw_aon_batmon.h)
#include DeviceFamily_constructPath(inc/hw_ccfg.h)
#include DeviceFamily_constructPath(inc/hw_fcfg1.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_uart.h)
// clang-format on

#define ROM_VTOR       0x10000000u
#define ROM_SBL_ENTRY  0x10003c67u  // Thumb address of 0x10003c66 (DESIGN.md §6.6)
#define SBL_WDT_RELOAD 11250000u    // 7.5 s at 1.5 MHz -> hardware reset on 2nd timeout (~15 s)

uint64_t chip_ieee(void)
{
    return (uint64_t)HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_1) << 32 | HWREG(FCFG1_BASE + FCFG1_O_MAC_15_4_0);
}

static void batmon_on(void)
{
    static bool on;
    if (!on) {
        AONBatMonEnable(), on = true;
    }
}

int chip_temperature_c(void)
{
    batmon_on();
    return (int)AONBatMonTemperatureGetDegC();
}

uint32_t chip_battery_mv(void)
{
    batmon_on();
    return (AONBatMonBatteryVoltageGet() * 1000u) >> 8;  // 3.8 fixed-point volts
}

// A flash operation runs in ROM with interrupts off, so it cannot be sampled while it runs: BATMON measures
// continuously meanwhile, and its latest reading, taken the moment the operation returns, is from its end
static uint32_t flash_meascfg, flash_raw = UINT32_MAX, flash_min_raw = UINT32_MAX;

void chip_flash_begin(void)
{
    batmon_on();
    flash_meascfg = HWREG(AON_BATMON_BASE + AON_BATMON_O_MEASCFG);
    HWREG(AON_BATMON_BASE + AON_BATMON_O_MEASCFG) = AON_BATMON_MEASCFG_PER_CONT;
}

void chip_flash_end(void)
{
    flash_raw = AONBatMonBatteryVoltageGet();
    HWREG(AON_BATMON_BASE + AON_BATMON_O_MEASCFG) = flash_meascfg;
    if (flash_raw < flash_min_raw) {
        flash_min_raw = flash_raw;
    }
}

uint32_t chip_flash_mv(void)
{
    return flash_raw == UINT32_MAX ? 0 : (flash_raw * 1000u) >> 8;
}

uint32_t chip_flash_min_mv(void)
{
    return flash_min_raw == UINT32_MAX ? 0 : (flash_min_raw * 1000u) >> 8;
}

// A restart through the boot selector, as a debugger's reset does it (CPU only, VECTRESET): the selector, then the
// app's start-up, which already undoes what survives it (board.c: the RF core, the HF crystal). Not a system
// reset: SysCtrlSystemReset (and HapiResetDevice) on the LaunchPad, its debugger keeping the JTAG power domain on,
// left the ROM boot code waiting for a pin wake-up (always from a running app; 2026-10-24, test_hw_ota.py).
void chip_reset(void)
{
    volatile uint32_t *const icer = (volatile uint32_t *)0xE000E180, *const icpr = (volatile uint32_t *)0xE000E280;
    uintptr_t zero;
    __asm__ volatile("cpsid i");
    for (int i = 0; i < 2; i++) {
        icer[i] = 0xFFFFFFFFu, icpr[i] = 0xFFFFFFFFu;  // every interrupt off, none pending
    }
    *(volatile uint32_t *)0xE000E010 = 0;                        // SysTick off
    *(volatile uint32_t *)0xE000ED04 = (1u << 27) | (1u << 25);  // PendSV, SysTick not pending
    *(volatile uint32_t *)0xE000ED08 = 0;                        // VTOR: the selector's table
    __asm__ volatile("" : "=r"(zero) : "0"(0u));                 // flash starts at address 0: not a C null pointer
    const uint32_t *vt = (const uint32_t *)zero;
    __asm__ volatile("dsb\n\tisb\n\tmsr msp, %0\n\tcpsie i\n\tbx %1" : : "r"(vt[0]), "r"(vt[1]) : "memory");
    for (;;) {
    }
}

void chip_watchdog_start(uint32_t timeout_ms)
{
    WatchdogUnlock();
    WatchdogReloadSet(timeout_ms * 1500u);  // the watchdog clock: 48 MHz / 32
    WatchdogStallEnable();                  // paused while a debugger halts the CPU
    WatchdogResetEnable();                  // the second timeout resets the chip
    WatchdogEnable();
    WatchdogLock();
}

void chip_watchdog_feed(void)
{
    WatchdogUnlock();
    WatchdogIntClear();  // reloads the counter
    WatchdogLock();
}

bool chip_can_enter_sbl(void)
{
    return (HWREG(CCFG_BASE + CCFG_O_BL_CONFIG) >> 24) == 0xC5u;
}

bool chip_enter_sbl(void)
{
    if (!chip_can_enter_sbl()) {
        return false;
    }
    uart_flush();
    radio_rx_stop();
    if (PRCMPowerDomainStatus(PRCM_DOMAIN_RFCORE) != PRCM_DOMAIN_POWER_OFF) {
        RFCClockDisable();
        PRCMPowerDomainOff(PRCM_DOMAIN_RFCORE);
        while (PRCMPowerDomainStatus(PRCM_DOMAIN_RFCORE) != PRCM_DOMAIN_POWER_OFF) {
        }
    }
    WatchdogUnlock();
    WatchdogReloadSet(SBL_WDT_RELOAD);
    WatchdogResetEnable();
    WatchdogEnable();
    WatchdogLock();

    volatile uint32_t *const icer = (volatile uint32_t *)0xE000E180;
    volatile uint32_t *const icpr = (volatile uint32_t *)0xE000E280;
    __asm__ volatile("cpsid i");
    *(volatile uint32_t *)0xE000E010 = 0;                        // SysTick off
    *(volatile uint32_t *)0xE000ED04 = (1u << 27) | (1u << 25);  // PendSV, SysTick not pending
    for (int i = 0; i < 2; i++) {
        icer[i] = 0xFFFFFFFFu;
        icpr[i] = 0xFFFFFFFFu;
    }
    HWREG(UART0_BASE + UART_O_CTL) = 0;
    for (uint32_t dio = 0; dio < 32; dio++) {
        uint32_t port = IOCPortConfigureGet(dio) & IOC_IOID_MASK;
        if (port == IOC_PORT_MCU_UART0_RX || port == IOC_PORT_MCU_UART0_TX) {
            IOCPortConfigureSet(dio, IOC_PORT_GPIO, IOC_STD_INPUT);
        }
    }
    *(volatile uint32_t *)0xE000ED08 = ROM_VTOR;
    uint32_t ctrl = 0;
    uintptr_t entry = ROM_SBL_ENTRY;
    __asm__ volatile("msr control, %0\n\tdsb\n\tisb\n\tcpsie i\n\tbx %1" : : "r"(ctrl), "r"(entry) : "memory");
    for (;;) {
    }
}
