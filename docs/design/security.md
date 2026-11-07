# Securing the radio link: design proposal

**Status: proposal for discussion (#28); partly decided (the user, 2026-11-06, on PR #77), nothing built.** Each
decision area lists the alternatives with their pros and cons and a recommendation; the user picks, and this file then
becomes the design.

**Decided so far:**

| Area | Decision |
|---|---|
| Threat model | An attacker in radio range **while the user is pairing** is an accepted risk |
| 4.1 Key | **F1**: per-sensor keys derived from one network key, the sensor's key sent to it **in clear** at pairing ("just works", the pairing window an accepted risk) |
| 4.2 Messages | **A**: AES-CCM; confidentiality is nice to have |
| 4.3 Freshness | **A**: a session per boot |
| 4.4 OTA | **A**: the authenticated channel is enough; **the button goes** |
| 4.5 Enabling | **B: on by default between our sensor and our gateway.** The user's general rule (R3 amended, [requirements.md](requirements.md)): once both have verified the other is ours, every feature both support is on. So the A6 "ours" mark goes on by default too (decided), after TASK-22 (v2 leak and v2 motion with the mark) |
| 4.6 Keys | **D**: the host holds `K_net` and pushes it to the gateway's RAM at every start (no gateway flash writes); `nv.json` may carry keys in clear. Recovery after a gateway restart: §4.6.1 |
| 4.7 Stock sensors | **C**: nothing |

## 1. The problem

On the air today (FACTS.md §4):

- Every payload is AES-128-ECB under one **public** key (the FIPS-197 example key), the same on every network.
- There is no integrity check beyond the radio CRC, no IV, and no replay check: the stock dongle ignores the message
  counter, and so does our gateway.
- So anyone in radio range with a CC1310 board or an SDR with a transmitter can **read** every report, **forge** any
  report ("door closed" while it opens), **replay** an old one, or **send commands** to a sensor (B0 time, and for our
  sensors the OTA messages C0–C2, which today only the button press guards: [ota.md](ota.md)).

## 2. Goals, non-goals, threat model

**Goals.** For our sensors with our gateway:

- G1. A forged or altered frame is rejected.
- G2. A replayed frame is rejected.
- G3. Gateway → sensor commands (time sync, OTA offers and blocks) are authenticated, so OTA no longer needs a button.
- G4. Report contents are confidential. (Lower priority: the timing of frames already shows that *something* happened.)

**Non-goals.**

- Jamming. Nothing at this layer stops it. A host can notice a silent sensor (missed heartbeats), as today.
- Traffic analysis: who sent a frame, and when.
- Protecting **stock sensors**. Their firmware speaks the stock scheme forever (R1); see §4.7 for what can still be
  done for them.

**Attacker.**

| Attacker | In scope? |
|---|---|
| Anyone in radio range: listens, records, transmits, at any time | yes: the main one |
| Someone in radio range *while the user is pairing a sensor* (a few seconds) | **accepted risk** (the user) |
| Physical access to one sensor (JTAG readout of its flash) | partly: it must not expose *other* sensors' keys |
| Physical access to the gateway, or control of the host (wyzesense2mqtt-rs) | no: they are trusted. §4.6 is about not letting that grow |

## 3. Constraints

- **R1, both ways.** A stock sensor keeps working with our gateway, and our sensor keeps working with a stock dongle.
  The stock dongle must see nothing new from our sensor.
- **R3.** Features that change what goes over the air are off unless the host turns them on. Is this one an exception?
  §4.5 asks.
- **No flash writes in normal sensor operation** (R2, #38/#59). The sensor's message counter lives in RAM and starts
  from 0 at each boot. That rules out the simplest replay check (§4.3).
- **CC1310 resources.**
  - Hardware: AES-128 with ECB, CBC-MAC, CTR and **CCM** in the crypto engine (driverlib `crypto.h`), and a TRNG.
  - No public-key or SHA hardware.
  - 128 KB flash: our sensor image is ~32 KB in a 56 KB slot.
  - 20 KB RAM.
  - A coin cell.
- **Frames.** An 802.15.4 frame is at most 127 bytes; stock payloads are 16 or 32 bytes. The dongle's host link is
  63-byte HID reports, of which `0x53F0` sub-commands use at most 56 bytes.
- **Several gateways, one network.** Roaming shares the sensor table across gateways ([roaming.md](roaming.md)), and
  migration moves it through `nv.json` ([nv-format.md](nv-format.md)). Anything stored per sensor must move with it.
- **Testable on the host board.** Everything must run in `firmware/tests` on the host board, so the crypto needs a
  portable implementation next to the CC1310 one, as AES-ECB has today.

## 4. Decision areas

### 4.1 How a sensor and the gateway get a shared key

| | Alternative | Pros | Cons |
|---|---|---|---|
| A | **X25519 key exchange at pairing**, unauthenticated: during the 3 s pairing, after the A6 (whose "OW" mark says the gateway supports it), sensor and gateway exchange public keys in two new messages and derive a per-sensor key | needs nothing from the user beyond today's pairing; a passive listener, even one present during pairing, learns nothing; a fresh key at every re-pair; no key database to keep | an *active* attacker present during those seconds can sit in the middle (Bluetooth's "just works" weakness); software X25519 on the CC1310: roughly 5–10 KB of flash and ~1 s of CPU at pairing (estimates, to measure) |
| B | **X25519 at pairing, authenticated by provisioning.** We wire-provision every one of our sensors anyway (`ows-flash v1 provision`). At that moment, the sensor's key pair is generated and its public key recorded by the host. At pairing, the host gives the gateway the expected public key | resists the man in the middle at pairing; still no symmetric secrets in a database (public keys are not secret) | the host must keep the provisioning record (a lost record means falling back to A); a sensor provisioned elsewhere has no record; more host plumbing (provision → w2m → gateway) |
| C | **A pre-shared key per sensor, written at wire provisioning**, the host keeps it and loads it into the gateway | no public-key code on the sensor (smallest, fastest); no man in the middle | the host's key file is now a database of secrets that must never leak *or* be lost; every gateway (roaming) needs it; nothing at all without the record |
| D | **One network key** for all sensors of a network, sent to each sensor at pairing (in clear, or under a key from A) | simplest table (no per-sensor key); roaming gateways share one secret | reading out one sensor exposes every sensor of the network; replacing the key means re-pairing everything |
| E | **The key sent in clear at pairing** (the gateway picks it and sends it in the A6) | trivial to build; far better than today | anyone recording during pairing has it, forever; a recorded pairing is enough, no active attack needed |

| F | **Per-sensor keys derived from one network key** (the user's suggestion): `K_sensor = AES-CMAC(K_net, sensor MAC)`. The sensor receives only its own `K_sensor` at pairing; the gateway (and host) keep only `K_net` and derive any sensor's key when needed, on the AES hardware | reading out one sensor exposes that sensor only (unlike D); the gateway and host store and move **one** 16-byte secret, not one per sensor, so roaming, migration and pushing keys to RAM (4.6 D) become trivial; no per-sensor key in the gateway's RAM or flash | compromising the gateway or host exposes every sensor (they are trusted anyway, §2); a re-pair gives the same key (fine, or add a pairing counter to the derivation); replacing `K_net` means re-pairing everything; how `K_sensor` reaches the sensor is a sub-choice (below) |

F's key reaches the sensor at pairing either:

- **F1, in clear** (in a new message after the A6): nothing added to the sensor. But anyone who merely *recorded* that
  pairing, without being active, can forge that sensor until `K_net` changes.
- **F2, wrapped by an X25519 exchange** (A's exchange, used only to carry `K_sensor`): a recording is useless.
  Roughly 5–10 KB of sensor flash and ~1 s more pairing (estimates).

**Recommendation: F2, or F1 if flash or pairing time turns out tight.** With the pairing window an accepted risk, F
gives A's per-sensor isolation while keeping the gateway's and host's key handling to one secret. F2 also covers
someone who records a pairing and attacks months later; F1 does not. (Earlier recommendation, before F: A now, B
possibly later. A and B remain possible, but store a key per sensor. C and D trade a safe key exchange for secrets to
look after. E is too weak to be worth a format change.)

### 4.2 Protecting each message

| | Alternative | Pros | Cons |
|---|---|---|---|
| A | **AES-CCM over the application payload**, our own format for our sensor with our gateway: a header (message type, key/session ID, counter), ciphertext, a tag of 4 or 8 bytes | standard authenticated encryption, in the CC1310's hardware; gives G1, G2 (with §4.3), G3 and G4; one mechanism for reports, commands and OTA | a new payload format (our gateway must recognise it next to the stock one); +8–12 bytes per frame (counter + tag) |
| B | **The stock ECB payload, plus an AES-CMAC tag** appended | keeps the stock layout, so tooling and captures still decode the plaintext; small change | no confidentiality (G4); CMAC is CBC-MAC with extra steps, no simpler than CCM; still a format change (the tag), so no stock compatibility is gained |
| C | **802.15.4 MAC-layer security** (the standard's auxiliary security header and CCM*) | the standard's own answer; covers MAC commands too | our `mac154.c` would need the aux header and frame counters; the frame counter must be persistent, which needs flash writes (R2); TI's stock dongle would see frames it cannot parse; more bytes per frame |

**Recommendation: A with an 8-byte tag.** CCM is in hardware. A 4-byte tag (one forgery in about 4 billion tries)
would also do for door events, but byte count matters little here: our frames are far from 127 bytes, and air time
grows by ~1.6 ms per byte at the measured rate (so ~13–19 ms per frame).

### 4.3 Freshness: rejecting replays

| | Alternative | Pros | Cons |
|---|---|---|---|
| A | **A per-boot session:** the sensor's first frame after a boot is a "hello"; the gateway answers with a random nonce; both derive a session key from the pairing key, the gateway's nonce and the sensor's own nonce; counters start at 0 in each session and must only go up | no flash writes on the sensor (R2 kept); a replay from an old session fails (other session key); standard pattern | one extra round trip before the first report after a boot (~100 ms on the air); a gateway restart loses the sessions, so each sensor re-handshakes on its next frame (one extra round trip per sensor) |
| B | **A persistent counter on the sensor:** reserve blocks of counters in flash, write one record per block of 256 or 1024 frames | no handshake; the counter alone gives freshness | flash writes in normal operation, which R2 forbids (#38: they killed the stock sensors); a lost block skips ahead (harmless) |
| C | **Timestamps:** the gateway's B0 time sync gives the sensor the time; reports carry it; the gateway accepts a window | no extra messages after the first sync | a replay inside the window still works; the sensor's clock drifts while it sleeps; the first report after a boot has no time yet |
| D | **The gateway persists the last counter per sensor**, the sensor's counter in RAM | no sensor flash writes | a sensor that rebooted starts from 0 and would be rejected until it gets past its old counter: needs A anyway |

**Recommendation: A.** It keeps R2 and closes replays across reboots. The extra round trip happens only after a sensor
or gateway restart, and those are rare.

### 4.4 Gateway → sensor traffic (time sync, OTA)

The same session key protects downlink frames (B0, C0–C2, A6 after the handshake), with the gateway's own counter.
There are no real alternatives once 4.2/A and 4.3/A are chosen, but two choices remain for OTA:

| | Alternative | Pros | Cons |
|---|---|---|---|
| A | **The authenticated channel is enough:** an offer from the gateway the sensor paired with is trusted; the button is no longer needed | 100 sensors update unattended, as each checks in (§6) | trusts the gateway and the host fully: a compromised host can push any image |
| B | **A + signed images:** release images signed with an Ed25519 key (CI secret or the user's); the public key is in our sensor firmware; the offer carries the signature, checked before any block moves | a compromised host or gateway still cannot push foreign firmware; works even without 4.1 | Ed25519 verify + SHA-256 in software (roughly 5–15 KB, ~1 s per update: estimates); key management; people who build their own firmware need a fallback (the button, or their own key added at provisioning) |
| C | **Keep the button** in any case | no change | the problem we started from |

**Recommendation: A first, B as a later layer.** The button stays as a fallback for unsigned and development builds.

### 4.5 Turning it on, compatibility, downgrades

| | Alternative | Pros | Cons |
|---|---|---|---|
| A | **Behind a gateway flag, off by default** (R3, like OTA and roaming): only with the flag on does the gateway advertise it in the A6 | follows R3; nothing changes for anyone who does not ask | users must know to turn it on; until then our sensors are as forgeable as stock ones |
| B | **On by default between our sensor and our gateway** (the feature is advertised in the A6 mark, which only our sensor reads) | secure without configuration; a stock dongle still sees nothing new (the sensor only asks after an "OW" A6) | an exception to R3: the user decides |

Either way:

- **Downgrade protection:** once a sensor has paired securely, the gateway's table marks it, and it then rejects
  stock-format frames claiming that sensor's address or MAC. Without this, an attacker would simply send old-style
  frames.
- **Our sensor with a stock dongle:** no "OW" mark in the A6, so it stays on the stock format, as today (R1).
- **Stock sensors with our gateway:** unchanged.

### 4.6 Where the keys live: roaming and migration

| | Alternative | Pros | Cons |
|---|---|---|---|
| A | **Each gateway verifies; keys travel with the table:** per-sensor keys (16 B) in the gateway's NV page; `nv.json` and roaming handovers carry them | the gateway forwards only verified, decrypted reports, so the host protocol barely changes; works with a plain stock-protocol host | `nv.json` becomes secret material (a leaked export lets someone forge until re-pairing); every roaming gateway needs every key; NV page space (~100 sensors × 16 B, fits) |
| B | **The host verifies:** gateways forward ciphertext (as they forward overheard frames today); keys live in wyzesense2mqtt-rs | keys in one place; a stolen or replaced gateway holds nothing; roaming needs no key sharing | the gateway cannot ACK by authenticity or answer commands securely without the key; the host protocol grows; a stock-protocol host gets nothing |
| C | **A, plus keys exported only wrapped:** `nv.json` carries keys encrypted under a key the user sets (or the gateway's own wrapping key, for moves between our gateways) | A's simplicity with a safer export | one more secret to manage; a wrapping key derived from a passphrase on a CH554-limited host link is fiddly |

| D | **The host holds the keys and pushes them to the gateway's RAM** at every gateway start (the user's preference: no gateway flash writes for keys) | keys never in gateway flash; a stolen or replaced gateway holds nothing once unpowered; with 4.1 F, the host pushes a single 16-byte `K_net` | after a restart, the gateway cannot verify or answer secure sensors until its host has connected and pushed the key: a gateway without its host (a relay whose wyzesense2mqtt-rs is down) drops their frames meanwhile; RAM (below) |

**RAM (D).** The gateway holds at most 64 sensors (`GW_MAX_SENSORS`). (Correction: `arm-none-eabi-size` counts the
fixed 6 KB stack block at the top of RAM as `.bss`, which made RAM look full. The linker map shows the gateway's data and
`.bss` end at ~8.2 KB and the stack starts at 14 KB: **~6 KB free**; the sensor has ~8.6 KB free.) The prototype keeps
16 sessions of 32 B (key, counters) and a `secure` flag per sensor: under 1 KB; 64 full sessions would be ~2 KB, which
also fits.

**Recommendation: D with 4.1 F** (the user's preference, and F makes the push one key). A remains the fallback if the
gap after a gateway restart proves a problem. Either way, `nv.json` may carry keys in clear (decided).

#### 4.6.1 Recovery after a gateway restart (with D)

A gateway restart loses every session, and until its host pushes `K_net`, the key too. The sensor must recover by
itself. Today it does not: after `MAX_TRIES` (8) unanswered tries it drops the report (`sensor.c`). Two cases:

- **Key present, session unknown** (the host has pushed `K_net`). A secured frame's header (session ID, counter) is in
  clear, so the gateway sees "unknown session" before any decryption and in time for the ACK. It ACKs with frame
  pending. On the sensor's poll it answers **"new session"**; the sensor redoes the hello / nonce exchange and resends.
  One extra round trip (~100 ms); nothing lost.
- **No key yet** (the host has not connected). The gateway cannot verify anything, so it **does not ACK** secured
  frames (an ACK would tell the sensor its report was delivered). The sensor, instead of dropping a report after its
  tries, **keeps it and retries with backoff** (e.g. 1, 5, 15 min, then with each heartbeat; the queue holds 4 reports,
  keeping the latest state per type). The first retry after the key arrives goes through the first case. Reports are
  delayed, not lost, and the backoff bounds the battery cost.

How long "no key" lasts:

- **USB dongle:** a dongle restart is a replug or a host restart, so it lasts until wyzesense2mqtt-rs reconnects
  (seconds). The host pushes the key right after its handshake.
- **ESP relay gateway:** the relay is on the host side. It can keep `K_net` in its own flash and push it whenever the
  contact board starts, so the gap disappears even while wyzesense2mqtt-rs is down. The contact board itself still
  never stores the key.

### 4.7 Stock sensors

They cannot be fixed, but two cheap partial defences exist, both host-visible and opt-in:

| | Alternative | Pros | Cons |
|---|---|---|---|
| A | **Replay check on the stock counter:** stock sensors keep their counter in flash and add 25 at each boot (FACTS.md §4, known-issues.md §1), so it only rises; the gateway drops a report whose counter went backwards (persisting the last value per sensor in RAM, re-learned after a gateway restart) | stops plain replays | not forgeries (the attacker just picks a higher counter, and then the real sensor's reports look like replays: a forged jump could silence it); a counter reset (re-pair, NV wipe) needs care |
| B | **Anomaly flags for the host:** RSSI far from the sensor's usual, two copies of one counter with different contents, etc. | detection, no false drops | heuristics; no prevention |
| C | Nothing | | stock sensors stay as forgeable as on a stock dongle |

**Recommendation: decide after the rest.** A has a nasty failure mode (a forged counter jump can silence a real
sensor), so if anything, B is safer.

## 5. A strawman combination

As decided: 4.1 F1 (derived keys, sent in clear at pairing) · 4.2 A (AES-CCM, 8-byte tag) · 4.3 A (per-boot session) ·
4.4 A (no button) · 4.5 B (on by default between our devices) · 4.6 D (host-held key in gateway RAM, §4.6.1) · 4.7 C
(nothing for stock sensors).

Messages, all in our own range, only between our sensor and our gateway after an "OW" A6 that advertises the feature:

| Step | Direction | Content |
|---|---|---|
| Pairing, after A6 | gateway → sensor | its key `K_sensor = AES-CMAC(K_net, MAC)` (16 B, in clear: 4.1 F1) |
| | sensor → gateway | a confirmation tag under `K_sensor` (proves it has the key); both mark the pairing secure |
| After every boot (sensor), or on "new session" from the gateway (§4.6.1) | sensor → gateway | hello: sensor nonce |
| | gateway → sensor | gateway nonce; both derive the session key from `K_sensor` and the two nonces |
| Every later frame | both | type, counter, AES-CCM ciphertext and 8-byte tag |

**Phases**, each one usable on its own:

0. TASK-22 (v2 leak and v2 motion take the A6 "ours" mark), then the mark on by default (4.5).
1. `K_net` from the host into the gateway's RAM (4.6 D); the key at pairing and the table's "secure" mark; reports
   authenticated and encrypted (4.1 F1, 4.2, downgrade protection).
2. Sessions and the replay check (4.3), and recovery after a gateway restart: "new session", the sensor's backoff
   (4.6.1); the ESP relay keeps `K_net` for its contact board.
3. OTA over the channel, without the button (4.4 A).
4. Optional, later: signed images (4.4 B).

## 6. Costs (estimates, to measure on a board)

| What | Cost |
|---|---|
| Sensor flash | small: no public-key code (F1); CCM and CMAC use the hardware (a few hundred bytes of glue) |
| Pairing | one more exchange (the key and its confirmation) |
| Each frame | +8–12 bytes, ~13–19 ms more on the air (~1.6 ms per byte, measured) |
| After a boot | one round trip (hello / nonce) before the first report |
| Gateway | nothing in flash; `K_net` (16 B) and ~12–16 B of session state per sensor in RAM (~1 KB for 64), from the stack reservation once measured |
| Host | keeps `K_net` (and pushes it at every gateway start); `nv.json` may carry it in clear |
| Unattended OTA of 100 sensors | each starts at its next check-in (≤ 4 h for v1 contacts); one at a time, ~2.5 min each, so ~4–5 h in all |

## 7. Prototype (branch `issue-28-secure-link-prototype`, draft)

Built on the host board and tested end to end (`test_secure_link` in `firmware/tests/test_apps.c`), and on hardware
(2026-11-07: the LaunchPad as the sensor, our gateway on the spare dongle, test PAN 0x65FE):
- `test_hw_secure.py`: the key pushed, the sensor paired, `secure session`, an event through it to the host; the gateway
  restarted (its key gone), the key pushed again, the next event through at once (the session renewed) (19 s).
- `test_hw_ota.py[secured]`: the sealed update of 45 KB started by a check-in (an A1), no button, in 299 s (plain: 265 s);
  the new image in slot B reported and confirmed itself. `[plain]` (no key) still passes, and `test_hw_sensor.py` (all
  six types, no key) too.
- On air (`ground_truth/rf/2026-11-07_1146_906.7M_250k_g0_secure_ota`, during that update): all 564 data frames between
  the two, 282 each way, are `4F 04` sealed frames once the stock layer is removed; none of the sensor's outside them.

- **Our messages share one first byte, `0x4F`, then a sub-ID** (the user's suggestion): no stock message uses `0x4F`.
  Stock gateways send `0xC0`, `0xD0`, `0xD2`, `0xE0` and `0xE2` to sensors (FACTS.md §9, host commands 0x5370–0x5393),
  so our OTA's `C0`–`C3` collided with one of them; OTA is now `4F 10`–`13` (no compatibility with earlier images
  needed: the user reflashes them by JTAG). Sub-IDs: `01` key, `02` hello, `03` session, `04` sealed
  frame, `05` new session (`firmware/common/seclink.h`).
- **Crypto** (`firmware/common/seclink.c`): AES-CMAC and AES-CCM built on the platform's AES-ECB (the CC1310's
  engine; `tests/aes_ref.c` on the host), checked against RFC 4493 and RFC 3610 vectors; `platform_random()` from the
  CC1310's TRNG.
- **Gateway:** the A6 mark on by default; `OWS_SUB_NET_KEY` (`0x53F0 0x07`) puts `K_net` in RAM; with it, A6 advertises
  `A6_OURS_SECURE` and `4F 01` carries the sensor's key; sessions in RAM; a secured sensor's stock-format reports are
  dropped; an SL_FRAME is ACKed only with the key **and** a session for that sensor, so a report is never lost to a
  gateway that cannot open it.
- **Sensor:** the key in NV item `0x22`; a hello before its first report after a start or a resync (idle, it sends
  nothing, as the stock sensors); reports sealed; nobody answering a hello
  or a sealed report: reports kept, a hello at once, then after 60 s, 5 min, 15 min, then with each heartbeat.
- **Flash cost:** ~2.6 KB on the gateway and on the sensor.
- **OTA** moved to `4F 10`–`13`, sealed for a secured sensor, which then takes the offer **without the button** at its
  next check-in (`test_secure_ota_without_button`: staged, no press, an A1, updated). Blocks shrink to 80 bytes so a
  sealed block fits one frame (a 9 KB update: 48 s instead of ~43 s in the simulation).
- **Host library:** `HostLink.net_key` (`tools/host_link.py`) and `host::Link::net_key` (`crates/ows-host`) query or
  push `K_net` over `OWS_SUB_NET_KEY`.
- **Not yet:** the `secure` flag persisted in the gateway's table (now RAM only: after a gateway restart, a
  forged stock-format report is accepted until the sensor's next hello); a plain B0 still accepted by a secured
  sensor; wyzesense2mqtt-rs pushing `K_net` at every start; the ESP relay keeping `K_net`.

## 8. Questions for the user

All six answered on PR #77 (see the table at the top). Still to settle before phase 0: nothing from the user; TASK-22 on
the bench.
