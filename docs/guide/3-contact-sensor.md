# Level 3: flash a working v1 contact sensor

A working stock v1 contact can run our firmware too. What it gets:

- **No zero-MAC death.** Our firmware reads the MAC from the part of the chip the bug never touches, and does not
  write to flash during normal use, so a dying battery cannot wipe anything.
- **Updates over the air**, through our gateway, without opening the sensor again.
- **DIY sensor types**: the same board as a water-leak sensor, a dry contact (float switch, pressure mat, relay), or
  an external PIR motion sensor.
- More accurate battery reports (measured under load, as the battery actually behaves when it sends).

It still pairs with a stock dongle, but updates and the extras above need our gateway ([level 1](1-dongle.md) or
[level 4](4-wifi-gateway.md)).

> **One way, for now.** The tools keep the sensor's stock firmware in its backup, but cannot put it back yet. The
> stock firmware has the bug that kills these sensors anyway.

## Flashing

Exactly as for a dead unit: [level 2](2-revive-contact.md), steps 1 to 3, with the same files and the same command.
Then put the battery in and **pair it again** (level 2, step 4). Our firmware starts unpaired, and the sensor
comes back under the same MAC.

## Using the sensor

| What you do | What the LED does |
|---|---|
| Put the battery in | blinks once (¼ s); 3 times (¼ s) on the first start after an update; 5 quick blinks: low battery |
| Hold the button **3 seconds** | at 3 seconds, while you still hold it: 3 slow blinks, pairing starts. Then 3 slow blinks: paired; 3 quick: no gateway answered |
| Press the button **briefly** | one short blink; the sensor checks in and opens a **5-minute update window** (below) |
| Move the magnet away / back | nothing: it reports open / closed with the LED off, to save the battery |

Between events the sensor sleeps. It wakes for the magnet, the button and its regular check-ins.

## Updating over the air

Sensors with our firmware update through **our** gateway (not a stock dongle). Each release has two images for that,
`ows-sensor-contact-v1-slot-a-<version>.img` and `-slot-b-<version>.img`. The sensor takes the one for the slot it is
not running from, and keeps the old one as a fallback until the new one has started properly.

From wyzesense2mqtt-rs: its dashboard offers sensor updates in a gateway's **Actions** dialog ([level 5](5-wyzesense2mqtt.md)).

From the command line, with our gateway on a USB dongle on this PC (stop wyzesense2mqtt-rs while it runs, if it uses
the same dongle):

```
./ows-flash ota 777B42DF ows-sensor-contact-v1-slot-a-<version>.img ows-sensor-contact-v1-slot-b-<version>.img
```

`777B42DF` is the sensor's MAC. Several MACs go comma-separated (`777B42DF,7788CCF6`), and `all` stands for every
paired sensor of that board type. When the tool says the update is staged, **briefly press each sensor's button**: that
opens its 5-minute update window, and the gateway updates the sensors one at a time, about 3 minutes each. A sensor
only accepts an update after a press of its own button, with a battery above 2.5 V, and only a newer version for its
board. The LED blinks twice when it takes an update, and 5 times quickly when it refuses one for a weak battery or when
an update failed.

## DIY sensor types

The board's input is the glass reed switch: it connects the chip's input `DIO7` to ground when a magnet is near. Cut
or unsolder one of its legs and wire your own probe or switch across the same two points (the input and `V−`), and the
same firmware reports it as another sensor type. The setting lives in the sensor's settings area, which updates never
touch.

| DIY sensor | Wiring | Console command | Reports |
|---|---|---|---|
| Water leak | two stainless probes, one on the input, one on `V−` | `profile leak` | wet when water bridges the probes for 100 ms or more, as a Wyze leak sensor |
| Normally-open switch (button, float switch, pressure mat, relay contact) | across the input and `V−` | `profile contact-no` | open (active) while the switch is closed |
| Normally-closed loop (wired door contact, tamper loop) | across the input and `V−` | `profile contact` | open when the loop opens |
| PIR motion module (3.3 V, push-pull output, such as an AM312) | output to the input, power from `V+` / `V−` | `profile motion` | motion, cleared 2 minutes after the last pulse |

To set it, connect the sensor's console: wire `T` to the LaunchPad's TXD and `R` to its RXD as well (the dashed lines in
the [level 2 wiring](2-revive-contact.md#2-wire-it-to-the-launchpad)), and open the LaunchPad's first serial port at
115200 baud, for example:

```
picocom -b 115200 /dev/ttyACM0
```

The console only listens for 10 seconds after a start, to save the battery. In a second terminal run
`./ows-flash v1 reset`, then press Enter in the console within those 10 seconds. It stays awake while you type, until
5 minutes of silence. Then:

```
profile leak        # or: profile contact-no | profile contact | profile motion | profile default
cfg                 # shows it: cfg type 03 invert 1 pull up debounce 100
sleep               # back to sleep now
```

A sensor that is already paired as another type has to leave and pair again: type `leave` before the `profile`
command, then pair it as in level 2, step 4. The fields can also be set one by one (`type`, `invert 0|1`,
`pull up|none|down`, `debounce <ms>`), and `status` shows the sensor's state.

## The v1 motion sensor

The release has the same three images for the v1 motion sensor (`ows-sensor-motion-v1-*`), provisioned with
`--board wyze_motion_v1` added to the `provision` command. Its board and pads differ from the contact's and are not
covered by this guide yet, and the firmware is built and tested in simulation only, not yet on hardware. Try it only
on a unit you can spare, and tell us how it went.
