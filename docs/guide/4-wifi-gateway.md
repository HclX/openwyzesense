# Level 4: build a Wi-Fi gateway (v1 contact board + ESP)

A v1 contact board running our gateway firmware, wired to an ESP32, ESP32-S3 or ESP8266 board that relays it to
wyzesense2mqtt-rs over Wi-Fi. It works like a dongle, but goes anywhere with Wi-Fi and USB power, and you can have
several of them. Dead (zero-MAC) contacts are ideal for this, because a gateway does not need the sensor's MAC to be
working.

The ESP side, its firmware, wiring and this level's step-by-step guide are now in their own repository:
**[HclX/wyzesense_relay](https://github.com/HclX/wyzesense_relay)** (`docs/guide/4-wifi-gateway.md` there). Its relay
images carry this project's gateway image for the contact board, `ows-gateway-contact-v1-<version>.bin` (in this
project's releases), and flash it onto the board themselves.

Without a relay, the contact board can also be provisioned as a gateway from the bench, wired to a LaunchPad as in
level 2: `ows-flash v1 provision --gateway --app ows-gateway-contact-v1-<version>.bin`.
