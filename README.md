# OpenWyzeSense

Open-source firmware for Wyze Sense sensors (v1 and v2) and the Wyze Sense USB dongle, now discontinued.
- **To use it** (flash a dongle, revive or flash a v1 contact, build a Wi-Fi gateway), with only a
  [release](https://github.com/HclX/openwyzesense/releases)'s files: [the user guide](docs/guide/README.md)
- **To build, test or change it**: [docs/development.md](docs/development.md)
- **System architecture & design**: [docs/design/DESIGN.md](docs/design/DESIGN.md)

Goals: keep the hardware working, fix stock bugs (first: v1 contact sensors losing their MAC on low
battery), and add features, while staying compatible with stock devices.

## Layout

| Path | What |
|---|---|
| `docs/` | the documentation, mapped in [docs/README.md](docs/README.md): `guide/` (using it, from a release), `design/` (how ours works: [DESIGN.md](docs/design/DESIGN.md) and per-subsystem notes), `development.md`, `TODO.md` (the one task list), and the stock firmware's knowledge (`FACTS.md`, `known-issues.md`, `stock-unknowns.md`, `stock-re/`) |
| `ground_truth/` | raw evidence: RF captures (IQ), the bridge's decoded-state logs, firmware dumps. Never edited. |
| `firmware/` | the firmware (C, CMake). `products.cmake` lists what gets built: an app (`apps/`: gateway, sensor, sniffer) on a board (`boards/`: LaunchPad, Wyze dongle, Wyze v1 motion; placeholders for the boards we have no firmware for), e.g. `gateway-launchpad`, `gateway-dongle`, `sensor-motion-v1`. `common/`: the portable protocol code (gateway and sensor state machines); `hal/`: the hardware interface the apps use; `soc/cc13x0/`: it on the CC1310 (radio, UART, AES, flash, clock); `boards/host/`: it simulated, so native tests run the apps unchanged (`tests/test_apps.c`: gateway, sensor and sniffer on one simulated air) |
| `third_party/ti_coresdk/` | the subset of TI's SimpleLink core SDK the firmware builds from (BSD-3-Clause), vendored by `tools/vendor_coresdk.sh` |
| `crates/` | Rust host tools (#42): `ows-host`, the library wyzesense2mqtt-rs uses to back up, flash and restore a Wyze dongle and carry its sensor table into our NV format (a port of `dongle_sbl.py` and `nv_json.py`, same checks, held to the same golden vectors); `ows-flash`, its command line (in each release) |
| `tools/` | `toolchain.sh` (fetches the pinned compiler and OpenOCD into `.toolchain/`), `host_link.py` (a host for the dongle protocol over serial or hidraw), `ws_bridge.py` (our gateway's UART to wyzesense2mqtt-rs over its WebSocket dongle bridge), `sbl_common.py` (the ROM bootloader's framing and guarded write path, for `uart_sbl.py`; the dongle itself: `ows-flash`), `decode_stream.py`, `image_id.py` (an image's build info block and its image ID, #76), udev rules for the LaunchPad and the ESP boards' USB serial chips |
| `reference/` | Python reference implementation (`wyzesense_ref`: IQ front end, PHY, framing, payload, host protocol, and a gateway model that replays the captures byte for byte) and the tests that tie FACTS.md to `ground_truth/` |

## History

The project was developed in a private repository before this one was published, so this repository starts with a
single commit. Issue and PR numbers (`#90`) and commit hashes (`fdcaad9`) cited in the docs and in `ground_truth/` refer
to that earlier history, not to this repository's issues.

## License

MIT, see [LICENSE](LICENSE).
