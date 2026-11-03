# Troubleshooting

| Problem | What to try |
|---|---|
| `Permission denied` on `/dev/hidraw*`, `/dev/ttyACM*` or `/dev/ttyUSB*` | Install the udev rule for the device ([Getting a release](README.md#getting-a-release)), then unplug and replug it. |
| `ows-flash` finds no dongle, or the wrong one | Pass `--device /dev/hidrawN`. The dongle is USB `1a86:e024` (`lsusb`). |
| The dongle does not come back after flashing | Unplug it and plug it back in. If it still does not work, flash again with the same backup, or restore stock (below). |
| I want stock firmware back on a dongle | `./ows-flash restore --backup backups/dongle-1 --yes` |
| `ows-flash v1` says TI UniFlash is missing | `./ows-flash fetch-uniflash` once (or `--uniflash DIR` if you keep it elsewhere). |
| `ows-flash v1` cannot connect to the sensor | Check the wiring: `M` and `C` swapped? Power on `V+` / `V−`? The LaunchPad's TMS, TCK and RST jumpers pulled, and the wires on the XDS110 side? Pogo pins straight and firm? |
| It stopped half-way, and the sensor no longer answers | `./ows-flash v1 reset` (needs `S` wired), then run the same command again; it continues from its backup. Without `S`: take all wires off for a few seconds (power alone is not enough while `R` / `T` are wired). |
| "no MAC" for a sensor | Its chip lost its factory MAC, which our tools refuse to work around. Open an issue. |
| The sensor does not pair | Pairing on at the gateway? Hold the button until the LED blinks 3 times slowly, while still holding. Try close to the gateway. A sensor paired as another type: `leave` on its console first. |
| No reports from far away | Check the sensor's signal strength in wyzesense2mqtt-rs or on the relay's page; below about -85 dBm reports get lost. Move the gateway, or add one ([level 5](5-wyzesense2mqtt.md#several-gateways-one-network)). |
| A sensor's update does not start | Briefly press its button after the update is staged; its battery must be above 2.5 V; it must be paired with our gateway, not a stock dongle. |
| I want stock firmware back on a v1 contact | Not supported yet: the backup has it, but the tools never write the part of the chip that holds the MAC. The stock firmware also has the bug that killed these sensors. |
| esptool cannot connect to the ESP | Hold its BOOT button while it starts; try another USB cable (some carry power only). |
| The relay's access point does not appear | It only opens when no Wi-Fi is set, or after about 2 minutes without the configured Wi-Fi. Reflash the first-flash image to clear the settings. |
| The relay is online but wyzesense2mqtt-rs shows no gateway | `bridge.enabled: true` in wyzesense2mqtt-rs, the bridge address `ws://<host>:8080/ws/bridge` and the token match, and the relay's page shows the contact board answering. |
| Two gateways and every report comes twice or gets lost | Both carry the same network: unplug one, or let wyzesense2mqtt-rs run roaming ([level 5](5-wyzesense2mqtt.md#several-gateways-one-network)). |

Bugs and questions: [open an issue](https://github.com/HclX/openwyzesense/issues).
