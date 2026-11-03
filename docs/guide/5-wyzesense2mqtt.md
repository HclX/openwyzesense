# Level 5: wyzesense2mqtt-rs with the OWS features

[wyzesense2mqtt-rs](https://github.com/HclX/wyzesense2mqtt-rs) bridges Wyze Sense gateways to MQTT and Home
Assistant. It works with stock dongles and ours alike, and when it finds our firmware on a gateway it can turn on more.
Its own README is the reference; this page says what is there and where.

**Setting it up:** [Quick Start](https://github.com/HclX/wyzesense2mqtt-rs#%EF%B8%8F-quick-start) (Docker Compose or a
plain install) and [Configuration](https://github.com/HclX/wyzesense2mqtt-rs#%EF%B8%8F-configuration-profile-configyaml).
A USB dongle is found by itself. Wi-Fi gateways ([level 4](4-wifi-gateway.md)) connect to it over the
[Remote Dongle Bridge](https://github.com/HclX/wyzesense2mqtt-rs#-remote-dongle-bridge) (`bridge.enabled: true`).

## What it adds with our gateways

All of these are in its README under
[OpenWyzeSense Gateways & Roaming](https://github.com/HclX/wyzesense2mqtt-rs#%EF%B8%8F-openwyzesense-gateways--roaming).
They are off unless you turn them on in the `openwyzesense` section of its config (for all gateways or one), except
roaming, which turns on by itself when it is needed.

| Feature | What it does | Config |
|---|---|---|
| Deduplication | The gateway reports a sensor's event once, even when the sensor resends it | `dedup` |
| Zero-MAC fixing | A report with an all-zero MAC from a **stock** v1 contact gets the MAC that the gateway's table holds for that sensor's address. It only helps while the table still has the real MAC; a unit that lost its pairing needs [level 2](2-revive-contact.md) | `fix_zero_mac` |
| Capability announcement | When a sensor pairs, the gateway tells it that it is ours, and which features it has | `OWS_ANNOUNCE` (environment) |
| Roaming | Several gateways share one network: each sensor is answered by one gateway, heard by all, and handed over when another hears it better | automatic; `roam` to force it on or off, `roam_hysteresis_db` |

The dashboard also shows each of our gateways' firmware version and features.

## Firmware from the dashboard

wyzesense2mqtt-rs takes firmware from the OWS release it is pinned to, and checks it against the release's SHA-256.

- **A USB dongle:** install our gateway, update it, or put stock back: the dongle's **Actions** dialog, **Firmware**
  section. Same steps as [level 1](1-dongle.md), with a review of the sensor list before anything is written.
  ([README](https://github.com/HclX/wyzesense2mqtt-rs#installing-openwyzesense-on-a-usb-dongle))
- **A Wi-Fi gateway:** **Update relay** in its Actions dialog.
  ([README](https://github.com/HclX/wyzesense2mqtt-rs#updating-an-openwyzesense-relay))
- **Sensors with our firmware:** updates over the air from a gateway's Actions dialog; then press each sensor's button
  briefly, as in [level 3](3-contact-sensor.md#updating-over-the-air).

## Several gateways, one network

To cover a large house, run several of our gateways (dongles and Wi-Fi gateways) on **one** network: import the same
`nv.json` into each ([level 4](4-wifi-gateway.md#moving-sensors-from-a-dongle)). wyzesense2mqtt-rs then turns roaming
on, merges the copies of each event, and moves each sensor to the gateway that hears it best. Only one gateway of the
network pairs at a time. `GET /api/roaming` shows who hears whom and how well.
