// The host board: the hardware interface (hal/) simulated, so native tests run the apps as they run on a chip.
// A world of devices on one virtual clock, each running one app and having its own radio, console, NV page,
// battery and LEDs. The air: a frame takes 15.4 ms + 1.6 ms a byte (FACTS.md §3), and reaches every other device
// whose receiver was on from its start to its end; frames that overlap at a receiver are both lost.
//
//   static gateway_app_t g;
//   host_dev_t *gw = host_add("gateway", &g, HOST_APP(gateway));
//   host_boot(gw);                         // power on: the app's init
//   host_console_send(gw, pkt, n);         // what a host would write to its UART
//   host_run(100);                         // 100 ms of virtual time: every app steps when it asked to
//   n = host_console_take(gw, out, sizeof out);
//
// Realtime, one device per process (host_main.c: the ows-host-<app> programs): the clock is set from outside
// (host_set_now_us), host_poll() does what is due, and frames reach other processes through host_on_tx /
// host_air_receive.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ccfg_check.h"
#include "input.h"
#include "nvstore.h"
#include "radio.h"

#define HOST_MAX_DEVICES     8
#define HOST_PAGE_SIZE       4096
#define HOST_AIR_LOG         1024
#define HOST_FLASH_SLOTS_END 0x1D000  // the end of slot B (SLOT_B_BASE + SLOT_SIZE)

typedef struct {
    void (*init)(void *app);
    uint32_t (*step)(void *app);
    size_t size;
} host_app_t;

// The init/step pair of an app named <name>_app_init/_step on a <name>_app_t
#define HOST_APP(name) \
    ((host_app_t){(void (*)(void *))name##_app_init, (uint32_t (*)(void *))name##_app_step, sizeof(name##_app_t)})

typedef struct {
    const char *name;
    void *app;
    host_app_t fn;
    bool booted;
    uint64_t boot_us, wake_us;
    bool woken;  // a frame or a console byte arrived

    // the hardware; tests may set these before host_boot()
    uint64_t ieee;  // chip_ieee()
    bool has_mac;   // board_mac(): mac
    bool battery;   // board_battery()
    bool usb_hid;   // board_usb_hid() (Wyze USB dongle CH554 bridge)
    uint8_t mac[8];
    int temperature_c;
    uint32_t battery_mv;
    uint32_t tx_sag_mv;     // a weak cell: how far the supply falls while a frame is on the air (radio_tx_min_mv)
    uint32_t tx_min_mv;     // what radio_tx_min_mv() returns: battery_mv - tx_sag_mv at the last frame
    uint32_t flash_sag_mv;  // the same under a flash erase / program (chip_flash_mv)
    uint32_t flash_mv, flash_min_mv;  // what chip_flash_mv() / chip_flash_min_mv() return
    bool radio_broken;                // radio_init() fails
    bool no_input[INPUT_COUNT];       // the board lacks the input (host_input() sets the others)
    int8_t rssi;                      // what receivers measure for this device's frames
    unsigned miss_frames;             // the next this many frames it would receive are lost (e.g. an ACK)

    // what the device did
    bool led[2];
    bool kept_awake;
    uint64_t console_until_us;  // battery console awake window deadline (soc/cc13x0/board.c)
    bool input[INPUT_COUNT];    // logical levels (host_input)
    bool event_invert;          // input_configure_event(): invert INPUT_EVENT active level
    int event_pull;             // input_configure_event(): BOARD_PULL_* or INPUT_PULL_DEFAULT (-1)
    uint32_t dio_levels;        // raw pin mask returned by board_pin_levels()
    bool radio_ready, rx_on;
    uint64_t rx_since_us, rx_on_us;  // receiver: on since, total on-time
    uint64_t tx_free_us;             // its transmitter is busy until then
    uint64_t last_start_us;          // start of its previous frame (radio_tx's gap_us)
    unsigned tx_frames;
    radio_frame_t rxq[16];
    unsigned rx_head, rx_tail;
    uint32_t overruns;

    uint8_t con_in[2048];  // console: to the device
    size_t in_head, in_tail;
    uint8_t con_out[65536];  // console: from the device, until host_console_take()
    size_t n_out;

    uint8_t page[HOST_PAGE_SIZE];  // the NV page; kept across host_boot()
    unsigned nv_bad_writes;        // programs that would have needed an erase (flash only clears bits)
    bool nv_dirty;                 // the page changed (host_main.c saves it); the caller clears it

    uint8_t ccfg_page[HOST_PAGE_SIZE];  // flash page 31: the CCFG at its end (CCFG_OFF), whatever before it
    uint8_t ccfg_builtin[CCFG_SIZE];    // the firmware's (hal/ccfg.h ccfg_builtin)
    unsigned ccfg_rewrites;             // ccfg_restore() erases of page 31
    bool ccfg_fail;                     // the next rewrite fails (verify)

    // The two-slot boot (hal/boot.h, docs/design/ota.md): flash pages 0-28. With slots set (before host_boot), the device
    // boots as the boot selector decides (common/boot_slot.c), and does not run at all if no slot is usable; else
    // it is an image at 0 (boot_running_slot() -1), as a device without the selector.
    bool slots;
    uint8_t flash[HOST_FLASH_SLOTS_END] __attribute__((aligned(8)));
    int slot;         // the slot running, -1 if none
    unsigned resets;  // chip_reset() calls (each a host_boot after the step)
    bool reset_pending;
    bool in_sbl;           // chip_enter_sbl() jumped to ROM SBL; stays there until host_boot()
    unsigned sbl_entries;  // chip_enter_sbl() calls that succeeded
    unsigned flash_erases, flash_programs;
} host_dev_t;

typedef struct {
    int from;  // device index (HOST_REMOTE + n: a device in another process)
    uint64_t start_us, end_us;
    int8_t rssi;  // what receivers measure
    uint8_t len;
    uint8_t frame[RADIO_MAX_FRAME];
} host_air_t;

#define HOST_CCFG_OFF (HOST_PAGE_SIZE - CCFG_SIZE)

// A safe CCFG, as the stock dongle's in what it checks (ccfg_is_safe): every device's built-in and current one
// at host_add()
extern const uint8_t host_safe_ccfg[CCFG_SIZE];

// Forgets every device; the clock goes back to 0.
void host_reset(void);

// A new device running app (state in *app); its page is erased flash, its IEEE address from its index.
host_dev_t *host_add(const char *name, void *app, host_app_t fn);

// Power on, or reset: the app's state is cleared and its init runs; the NV page is kept.
void host_boot(host_dev_t *d);

// Puts a slot image (header and image, as tools/slot_image.py makes it) in a slot of a device with slots set.
void host_install_slot(host_dev_t *d, int slot, const uint8_t *img, size_t len);

// Lets ms of virtual time pass.
void host_run(uint32_t ms);

// Runs until cond(arg) holds, checking after every step, for at most ms; returns whether it held.
bool host_run_until(bool (*cond)(void *arg), void *arg, uint32_t ms);

uint64_t host_now_us(void);

// An input changes level (logical: true = open / motion / pressed); wakes the device, as the pin interrupt does.
void host_input(host_dev_t *d, int input, bool level);

void host_console_send(host_dev_t *d, const void *data, size_t n);
size_t host_console_take(host_dev_t *d, uint8_t *out, size_t max);
bool host_console_awake(const host_dev_t *d);

// The device's NV page as an nvstore (to look at or prepare it from a test).
void host_nv(host_dev_t *d, nvstore_t *nv);

// --- realtime ---

#define HOST_REMOTE 1000

// Sets the clock (it only goes forward). Realtime: a monotonic clock shared by all the processes.
void host_set_now_us(uint64_t t);

// Delivers the frames that have ended and steps the devices that are due; returns when something is next due.
uint64_t host_poll(void);

// Called for every frame a device here sends (to pass it to the other processes); may be NULL.
extern void (*host_on_tx)(const host_air_t *f);

// A frame from another process, on the air from f->start_us to f->end_us (f->from >= HOST_REMOTE).
void host_air_receive(const host_air_t *f);

// Every frame sent so far (the last HOST_AIR_LOG).
extern host_air_t host_air[HOST_AIR_LOG];
extern size_t host_n_air;

// The receiver's on-time so far, up to now.
uint64_t host_rx_on_us(const host_dev_t *d);
