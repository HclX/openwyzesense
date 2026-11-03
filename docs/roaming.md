# Roaming: several gateways, one network (#90, `TASK-23`)

Stock sensors know one coordinator: a PAN, a short address and an extended address. Several of our gateways can all
carry that one identity (each migrated from the same stock dongle, or `OWS_SUB_NETWORK` set alike), so a sensor is
heard wherever one of them is in range, without being re-paired. The radio is one channel and every Wyze network
shares one payload key, so every gateway in range hears and can decrypt every report.

What must not happen is two gateways answering the same frame: their ACKs start within the same few ms and collide
(DESIGN.md §11.1, the carrier beat), and the sensor resends or gives up. So with roaming on, **each sensor has one
owner**: the gateway whose table holds it. The host (wyzesense2mqtt-rs, #96) decides who owns what.

## The gateway (`GW_FLAG_ROAM`, flag bit 4, off by default)

With the flag on (`OWS_SUB_FLAGS`; like every flag, not kept across a reset):

| Frame from a sensor | Owner (in its table, or joining it) | Any other gateway |
|---|---|---|
| Report (data frame, ACK requested) | ACKs it, forwards it as `0x5319` (as now) | **no ACK**; forwards A1 / A2 / E8 / AB as `OWS_SUB_OVERHEARD` |
| Data request (poll) | ACKs it, sends what is pending (or the B0 time) | **silent** |
| Association request | the gateway that is pairing (`0x531C` on) ACKs and answers it | **silent** |
| Beacon request | every gateway answers (after its random delay, #84) | (same) |

Whatever the flag, a gateway reports a sensor's join (A3 -> `0x5320`) only while it is pairing, as stock does (#60).
With roaming off, two gateways on one PAN both ACK every frame, so a PAN shared by several gateways needs the flag on
all of them. Pairing is one gateway at a time: every gateway shares one identity, so a sensor cannot choose between
them, and several pairing at once would ACK one association request together and each hand out an address. The best
gateway is the host's choice after the join, by handover on the RSSIs it gets from the owner and from
`OWS_SUB_OVERHEARD` (the user's decision, 2026-11-02).

`OWS_SUB_OVERHEARD` (`0x53F1` sub-ID `0x20`, gateway → host): `[0x20, short_le16, <the 0x5319 payload>]`, i.e. the
sensor's short address, then exactly what the owner would have sent (gateway clock, the plaintext up to its counter,
the RSSI as -dBm). It is sent once and not kept for resending (it would crowd out the gateway's own async packets), so
it may be lost; the host ACKs it as any async packet. With `GW_FLAG_DEDUP` on, a sensor's resends are forwarded once.
The short address is there for the v1 contacts whose reports carry an all-zero MAC (known-issues.md §1): only the host
can tell which sensor that is, from its combined table.

Moving a sensor (a handover) uses commands that exist:

- **drop** at the old owner: `0x5325` Delete Sensor (stock command). Ours sends nothing to the sensor; it only forgets
  it, with whatever was queued or learned for its address.
- **add** at the new owner: `OWS_SUB_NV_IMPORT` `ADD_SENSOR` (`0x53F0 0x06 0x03`: MAC, short address, type), with the
  sensor's short address unchanged. The type lets the new owner send the B0 time on its next poll at once.

Drop first, then add, so that two gateways never own it at once. The sensor notices nothing: same coordinator, same
address.

New sensors: only one gateway pairs at a time, and each hands out addresses from its own range, so two gateways never
give the same address to two sensors. The host sets the range right before it turns pairing on at a gateway:
`[highest address in the combined table + 1, 0xFFFD]` (the user's decision, 2026-11-01, #96), so nothing per gateway
has to be configured or kept: `OWS_SUB_ADDR_RANGE` (`0x53F0` sub-ID `0x23`): `[]` asks, `[first_le16,
last_le16]` sets (`0x0001 <= first <= last <= 0xFFFD`); reply `[0x23, status, first_le16, last_le16]`, status `0x00`
or `0xE0` (refused). Default: the whole range `0x0001-0xFFFD`. Not kept across a reset. Migrated sensors keep their
addresses, inside any range.

## What it does not do (yet)

- **Two sensors on one short address** (a stock defect, known-issues.md §2): never carried over. Ownership is by
  short address, since the ACK must go out before the report is decrypted, so every address must be one sensor's.
  The export keeps the lowest sub-ID and lists the other under `dropped` with its MAC and "re-pair ... after the
  migration" (the flashing tools print it); the gateway and the import tools refuse a second MAC on a taken address
  (the user's decision, 2026-11-01). Until it is re-paired, the dropped sensor still sends from the shared address and
  its owner ACKs and forwards its reports under its own MAC, but the host does not know it and `GW_FLAG_FIX_ZERO_MAC`
  would give a zero-MAC report from it the kept sensor's MAC. Re-pairing it (a long press with pairing on) gives it a
  new address, since its old one is taken.
- **Beacons**: every gateway with the identity answers a beacon request. With #84's random delays, a pairing gateway
  answers first; the others' beacons follow and carry permit off. A sensor scanning for a network takes any of them.
- **Who owns what** is all the host's: signal strengths, missed reports, hysteresis, a gateway that went offline. The
  request for wyzesense2mqtt-rs is #96.

## Tests

- `firmware/tests/test_roam.c` (ctest `roam`): gateways and `sensor.c` sensors on one virtual air where two gateways
  sending at once collide: both ACK without the flag; with it, the owner alone ACKs and the others overhear; a
  handover; the owner out of range; nobody's sensor; pairing with address ranges; the range command.
- `reference/tests/test_host_roam.py`: the real host binaries (two `ows-host-gateway`, one `ows-host-sensor`) on one
  UDP air, a small host on each pty: one identity, ranges, pairing on one gateway, the other overhearing, a handover
  and the reports after it, with no re-pairing.
- On hardware (2026-11-01, test PAN `0x65FE`, gateway 40f8584): relay B15C's contact gateway and the spare dongle,
  one identity (migrated from the stock dongle), `GW_FLAG_ROAM` on both, no dedup. Stock v2 contact 77BD514D: 6
  events and a heartbeat with B15C as owner, a handover (`0x5325` at B15C, `ADD_SENSOR` at the dongle), then 8 events
  with the dongle as owner. Each event was reported once by the owner and overheard once by the other; the counters
  ran on without a repeat (no sensor resends) and the sensor never joined again. The user's gateway 9A78 stayed out.
