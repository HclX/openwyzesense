# OTA updates for our sensors: design (built; verified over the air on the LaunchPad)

Updating our own sensor firmware over the air, after the first install by wire. Between our gateway and our sensors
only; off unless the host asks for it (R3). Stock sensors are never offered anything.

## Who may update a sensor: its own gateway over the secured link, or whoever has just pressed its button

**With the secured link** ([security.md](security.md), prototype): the offer and the blocks come sealed (AES-CCM) from
the gateway the sensor shares a session with, so the sensor takes the offer without a button press, at its next
check-in (its A1 heartbeat, then a poll). A staged wave updates unattended. The rest of this section is the rule
without the secured link (a gateway without the network key):

No signatures (the user's decision, 2026-10-24): a sensor accepts an update only in a **window opened by a short
press of its own pairing button** (5 minutes, closed by the update or a reset), the way pairing itself needs a hand
on the sensor. A long press (held `>= 3.04 s` while held, matching stock `Ssf_buttonClockCb`) pairs, as on stock; the
short press (`< 1 s`; stock ignores short presses) is ours, so an update never re-pairs a sensor. A remote attacker would have to be in radio range at that moment. The gateway also offers an update only
when its host has asked for one for that sensor (a host command, next to scan/verify).

**Integrity** (not security) is still checked: the offer carries the image's size and CRC32; the sensor checks the
CRC32 of what it wrote before it ever runs it. Each frame already has its own CRC-32 (FCS) and each block its offset.

## Flash layout (CC1310F128, 32 pages of 4 KB)

| Pages | Address | What | Written by |
|---|---|---|---|
| 0 | 0x00000 | **boot selector** (vector table at 0x0 for the ROM): picks a slot and jumps to it | wire only, never OTA |
| 1-14 | 0x01000 | **slot A** (56 KB) | wire (first install) or OTA |
| 15-28 | 0x0F000 | **slot B** (56 KB) | OTA |
| 29 | 0x1D000 | spare | - |
| 30 | 0x1E000 | NV page: the sensor's join (and a stock table where there is one) | the app |
| 31 | 0x1F000 | CCFG (and stock code on Wyze units): **never written** | nobody |

Our sensor image is ~32 KB: a 56 KB slot leaves room to grow.

Each slot starts with a 256-byte header region (`slot_header_t`, 80 bytes at `+0x00` in `firmware/common/boot_slot.h`,
padded with `0xFF` to `+0x100`): `0x00` magic (`"OWSI"`), `0x04` board ID (CRC-32 of the board name), `0x08` version,
`0x0C` size, `0x10` CRC-32 of the image, and a 6th word at `0x14` **`slot`** (`0` = Slot A, `1` = Slot B, `0xFFFFFFFF` =
any, filled in after the link by `tools/slot_image.py`), followed by `0x18..0x3F` `reserved[10]`, then words left
all-ones in the image and programmed to 0 later, never erased: `0x40` **confirmed**, and `0x44..0x4F` three
**attempts**. The image itself starts at `+0x100` (its Cortex-M3 vector table). Both `boot_slot_valid()` and
`boot_written_ok()` verify the `slot` word and check that the vector table's reset handler falls strictly inside
`[slot_base + 0x100, slot_base + SLOT_SIZE)`, so a Slot A build can never be booted or confirmed in Slot B. The **boot
selector** (`firmware/boot/selector.c`, ~500 bytes) only reads:
it runs the intact (CRC) slot for its board with the higher version among those still usable, sets VTOR and jumps;
as a last resort an intact image out of attempts, never a broken one. The **app** does the writing, after its own
start-up (flash writes need the device trimmed): first thing, an unconfirmed image uses up an attempt; it confirms
itself after 60 s of running and, if it was on a network, once its gateway has ACKed a frame. An image that fails three boots in a row is dropped for the other slot. Images are
linked for their slot (`cc1310f128_slot_{a,b}.ld`); a product built with `SLOTS` gives `<product>-slot-{a,b}.img`
and `boot-<board>.bin`. Verified on the LaunchPad: `reference/tests/test_hw_boot.py` (probation, the fallback after
three boots, confirmation, a slot with one wrong byte skipped).

### The version number (#57)

The header's version is a `uint32_t`, compared unsigned ("newer" = higher, by the sensor and the boot selector). It is
encoded from the release tag (`firmware/image_version.cmake`, from `git describe`):

    major << 24 | minor << 16 | patch << 8 | commits since the tag

so v0.2.2 is `0x00020200`, three commits after it `0x00020203`. Each release field is one byte (a tag above 255 stops
the build); the commit count saturates at 255 (a warning), so it never carries into the patch byte. Every release is
above every build numbered the old way (the commit count: the archive repository's builds reach a few hundred, the
public repository's first releases 5-21), so those sensors take a release. A pre-release tag (`v0.3.0-rc1`) would
sort equal to its release and stops the build; `-DOWS_IMAGE_VERSION=<n>` sets any number by hand. Without a tag (a
clone without tags) a build is `0.0.0` + the commit count (saturated), below every release.

## On the air

Our own application messages `4F 10`–`13` (`firmware/common/ota.h`; they were C0-C3, which collide with the stock
gateway's `0xC0` command), sealed in a secured link's frame when there is one, else encrypted as all the others, on
the existing poll /
pending-frame mechanism, so the sensor drives everything and its receiver is on only around its own frames.
Nothing a stock peer sees changes (R3): our sensor joins exactly as before (an early version flagged itself in an
A3 byte, which a stock dongle would have seen: dropped), and our gateway offers an update only with `GW_FLAG_OTA`
on and only to the sensor its host names.

1. The host (which has the update's two builds, for slot A and slot B) stages it via `0x53F0` sub-ID `0x10`
   (`OWS_SUB_OTA_OFFER`) for up to **8 explicit sensor MACs** at once (`GW_MAX_OTA_TARGETS = 8`), or wildcard
   `"********"` for all paired sensors of that board (`ows-flash ota`, crates/ows-host's `ota` module, and `tools/host_link.py --ota` default to `--seconds 360`,
   dynamically extending while blocks advance, and snapshots + restores active `gw->flags` on exit rather than clearing
   them).
2. The user gives each target (paired) sensor's button a short press: its 5-minute window opens and it **checks in**:
   an A1 (a stock message), then ten polls; in the window it also polls every 10 s, so an update staged after the press
   — or queued behind another sensor currently receiving blocks — is picked up automatically without pressing the
   button again. Our gateway answers a poll of the active sensor with **C0**: board ID, version, size and CRC of each
   build (and again after each A1 from it, so a sensor reset half-way gets it again on its next press).
3. The sensor takes it if: in its window, a slot image, same board ID, newer version, battery >= 2.5 V (read just
   after a transmission). Else it answers **C3** "refused" (for example, a different board type or already on that
   version during a wildcard wave). It writes the build for its free slot.
4. It asks for each block with **C1** (slot, offset); the gateway asks its host (`0x53F1` sub-ID `0x11`
   `OWS_SUB_OTA_NEED`), collects the block in `<= 48`-byte pieces (`0x53F0` sub-ID `0x12` `OWS_SUB_OTA_DATA`, fitting
   comfortably inside the dongle's 63-byte USB-HID reports), and sends it as **C2** (96 bytes) on the sensor's next poll
   (60 ms later, then every 150 ms). The sensor re-checks battery >= 2.5 V before erasing each 4 KB page of the slot as
   the data reaches it, aborting cleanly if voltage sags. A lost block comes again from the gateway's buffer; a lost
   request is sent again by the MAC; 12 requests in a row without a block: it gives up.
5. At the end: the slot's CRC, header (board, version, `slot` index), and vector-table slot address range are checked;
   **C3** with the result (forwarded to the host as `0x53F1` sub-ID `0x13` `OWS_SUB_OTA_DONE`); good -> reset into the
   new image, on probation. It sends an A1 at start-up, and confirms itself once its gateway has ACKed a frame and it
   has run 60 s.

The gateway keeps no image (the dongle has 20 KB of RAM): one 96-byte block buffer, one active stream at a time, while
up to 8 queued sensor MACs (`GW_MAX_OTA_TARGETS = 8`, or `"********"` fleet wildcard) wait in their 5-minute poll
windows and automatically begin one after another.

Simulated (`firmware/tests/test_apps.c`, host board, the air at 5 kbps as measured): a 9 KB build in 43 s from the
button press (~0.44 s per block); staged 4 s after the press, found by the window's next poll; the same with frames lost every 1.5 s; a reset half-way (version 10 keeps running,
a new press starts over); a byte wrong (CRC: refused, no switch); battery low at the offer and half-way; not newer;
no window; flag off; a sensor that is no slot image. A second update goes from slot B back to slot A.

## Cost

Per block: request (~55 ms on the air) + ACK + data request + ACK + 33.5 ms gap + data frame (15.4 ms + 1.6 ms/byte,
~210 ms) + ACK: **~0.44 s for 96 bytes** (simulated). A 32 KB image: ~340 blocks, **~2.5 minutes**. Charge, from CC1310 datasheet currents (not
yet measured on a board): receiving ~5.5 mA most of that time (~0.17 mAh) plus ~370 short transmissions at +14 dBm
(~0.14 mAh): **~0.3 mAh, about 0.15 % of a CR2032** (to be measured on a board). Erasing and programming 8-9 pages: negligible. Other sensors
keep reporting meanwhile; the gateway serves them between blocks.

## Safety rules

- Nothing over the air writes page 0 (boot selector), 30 (NV) or 31 (CCFG and the sensor's MAC).
- No erase or write when the battery is weak under load (the zero-MAC failure mode is a flash write at low battery).
- The new image runs only after its CRC32 checks out; it stays on probation until it confirms itself.
- Off unless the host enables it; tested on test PANs only.

## Steps

1. Done: the boot selector, two-slot link scripts, the app's attempt / confirm; `test_boot` (native) and
   `test_hw_boot.py` (LaunchPad).
2. Done: the protocol in `common/` (sensor.c, gateway.c), the app's policy (sensor_app.c), the host board's slots;
   tests as above.
3. Done on the LaunchPad: `HostLink.ota_update()` (`tools/host_link.py`) serves the two builds; `test_hw_ota.py`
   (transmits, test PAN): the sensor in slot A pairs by its button with our gateway on the spare dongle, takes
   version N+1 (36,304 bytes, the slot-B build) in **194 s from the button press** (~0.5 s per 96-byte block),
   restarts into slot B, reports, confirms itself, and stays on B across a reset.
4. Done on a Wyze board: the v1 contact 777B42DF (`test_hw_contact_ota.py`, 2026-10-24): 37 KB in 196 s, slot B,
   confirmed; `chip_reset` through the selector works there too. Left: the charge of an update, on a battery.

The restart after an update: a software **system** reset on the LaunchPad (its debugger attached since power-up) left
the ROM boot code waiting in WFI every other time (FACTS.md §5), so `chip_reset` restarts the way a debugger's reset
does: interrupts off and cleared, VTOR 0, the selector's stack pointer and reset vector. The app's start-up already
copes with what survives that (board.c: RF core, HF crystal; uart.c: the UART).
