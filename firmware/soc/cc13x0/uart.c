#include "uart.h"

#include "clock.h"

#include <string.h>

#include <ti/devices/DeviceFamily.h>
// clang-format off: a TI include path, not a division
#include DeviceFamily_constructPath(driverlib/ioc.h)
#include DeviceFamily_constructPath(driverlib/sys_ctrl.h)
#include DeviceFamily_constructPath(driverlib/uart.h)
#include DeviceFamily_constructPath(inc/hw_ints.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(inc/hw_types.h)
#include DeviceFamily_constructPath(inc/hw_uart.h)
// clang-format on
#include <ti/drivers/Power.h>
#include <ti/drivers/dpl/HwiP.h>
#include <ti/drivers/power/PowerCC26XX.h>

#define RX_RING 256     // power of two
#define TX_SPIN 200000  // FIFO-full polls before a write gives up (several ms; a character takes 87 us)

static uint8_t rx_ring[RX_RING];
static volatile uint32_t rx_head, rx_tail;
static HwiP_Struct uart_hwi;
static volatile bool uart_on;
static bool tx_stalled;

static void uart_isr(uintptr_t arg)
{
    (void)arg;
    UARTIntClear(UART0_BASE, UART_INT_RX | UART_INT_RT | UART_INT_OE | UART_INT_FE);
    while (UARTCharsAvail(UART0_BASE)) {
        uint8_t c = (uint8_t)UARTCharGetNonBlocking(UART0_BASE);
        if (rx_head - rx_tail < RX_RING) {
            rx_ring[rx_head++ % RX_RING] = c;
        }
    }
    clock_wake();
}

void uart_init(int rx_dio, int tx_dio)
{
    HwiP_Params hp;
    if (rx_dio < 0 || tx_dio < 0) {
        return;
    }
    rx_head = 0;
    rx_tail = 0;
    tx_stalled = false;
    uart_on = true;
    Power_setDependency(PowerCC26XX_PERIPH_UART0);
    // Whatever a CPU-only reset (a debugger's, chip_reset) left in the UART: once it was enabled with a baud divisor
    // of 0 and a full TX FIFO, so UARTDisable (in UARTConfigSetExpClk) waited forever. Off, the FIFO flushed (off),
    // a valid divisor latched (by the LCRH write), so a character still in the shifter goes out and BUSY clears.
    HWREG(UART0_BASE + UART_O_CTL) = 0;
    HWREG(UART0_BASE + UART_O_IBRD) = 1;
    HWREG(UART0_BASE + UART_O_FBRD) = 0;
    HWREG(UART0_BASE + UART_O_LCRH) = 0;
    IOCPinTypeUart(UART0_BASE, (uint32_t)rx_dio, (uint32_t)tx_dio, IOID_UNUSED, IOID_UNUSED);
    IOCIOPortPullSet((uint32_t)rx_dio, IOC_IOPULL_UP);
    UARTConfigSetExpClk(UART0_BASE, SysCtrlClockGet(), 115200,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
    UARTFIFOLevelSet(UART0_BASE, UART_FIFO_TX1_8, UART_FIFO_RX4_8);
    UARTFIFOEnable(UART0_BASE);
    HwiP_Params_init(&hp);
    HwiP_construct(&uart_hwi, INT_UART0_COMB, uart_isr, &hp);
    UARTIntEnable(UART0_BASE, UART_INT_RX | UART_INT_RT);
    UARTEnable(UART0_BASE);
}

void uart_write(const uint8_t *data, size_t n)
{
    if (!uart_on) {
        return;
    }
    while (n--) {
        // a UART that does not send never hangs the app: after one wait of TX_SPIN, output is dropped (at once)
        // until the FIFO has room again
        uint32_t spin = tx_stalled ? 1 : TX_SPIN;
        for (;;) {
            uintptr_t key = HwiP_disable();
            if (!uart_on) {
                HwiP_restore(key);
                return;
            }
            bool sent = UARTCharPutNonBlocking(UART0_BASE, *data);
            HwiP_restore(key);
            if (sent) {
                break;
            }
            if (!--spin) {
                tx_stalled = true;
                return;
            }
        }
        tx_stalled = false;
        data++;
    }
}

void uart_flush(void)
{
    uint32_t spin = TX_SPIN;
    while (--spin) {
        uintptr_t key = HwiP_disable();
        bool busy = uart_on && UARTBusy(UART0_BASE);
        HwiP_restore(key);
        if (!busy) {
            break;
        }
    }
}

void uart_sleep(void)
{
    if (!uart_on) {
        return;
    }
    uart_flush();
    uintptr_t key = HwiP_disable();
    if (uart_on) {
        UARTIntDisable(UART0_BASE, UART_INT_RX | UART_INT_RT);
        UARTDisable(UART0_BASE);
        HwiP_destruct(&uart_hwi);
        HwiP_clearInterrupt(INT_UART0_COMB);
        uart_on = false;
        rx_head = 0;
        rx_tail = 0;
        tx_stalled = false;
        Power_releaseDependency(PowerCC26XX_PERIPH_UART0);
    }
    HwiP_restore(key);
}

void uart_puts(const char *s)
{
    uart_write((const uint8_t *)s, strlen(s));
}

bool uart_getc(uint8_t *c)
{
    if (!uart_on || rx_tail == rx_head) {
        return false;
    }
    *c = rx_ring[rx_tail++ % RX_RING];
    return true;
}
