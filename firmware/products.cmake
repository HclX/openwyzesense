# What gets built: each product is one app on one board, as <product>.elf/.hex/.bin, with flash-<product> when
# the board can be flashed from here. Boards without firmware we could check against are placeholders
# (boards/<board>/README.md) with no products: Wyze hub, v2 contact, v2 motion, climate, leak.
#
#   ows_product(<product> <app> <board> [SENSOR_TYPE <hex>] [SLOTS])

# Gateways (apps/gateway: the stock dongle's host protocol on the board's UART)
ows_product(gateway-launchpad   gateway  launchxl_cc1310)
ows_product(gateway-dongle      gateway  wyze_dongle)
ows_product(gateway-contact-v1  gateway  wyze_contact_v1)   # the host link on its RX/TX pads (an ESP32: #15)

# Sensors (apps/sensor)
ows_product(sensor-launchpad    sensor   launchxl_cc1310  SENSOR_TYPE 0x0E SLOTS)    # any type: "type" command
ows_product(sensor-motion-v1    sensor   wyze_motion_v1   SENSOR_TYPE 0x02 SLOTS)    # its MAC: the unit's CCFG
ows_product(sensor-contact-v1   sensor   wyze_contact_v1  SENSOR_TYPE 0x01 SLOTS)    # its MAC: the unit's CCFG

# Tools
ows_product(sniffer-launchpad   sniffer  launchxl_cc1310)
