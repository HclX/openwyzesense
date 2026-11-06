# Securing the radio link: design proposal

**Status: proposal for discussion (#28). Nothing here is decided or built.** Each decision area lists the
alternatives with their pros and cons and a recommendation; the user picks, and this file then becomes the design.

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
| Someone in radio range *while the user is pairing a sensor* (a few seconds) | yes, see §4.1 for which alternative resists it |
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

**Recommendation: A now, B possibly later as an upgrade.** A is the best protection for no extra user effort, and B
can be added on top without changing the message format: only the gateway checks one more thing. C and D trade a
safe key exchange for secrets to look after. E is too weak to be worth a format change.

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

**Recommendation: A, with C's wrapping as an option for the export.** The gateway must be able to answer sensors on
its own (ACKs, time sync, OTA), so it needs the keys; B fits poorly with that.

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

4.1 A (X25519 at pairing) · 4.2 A (AES-CCM, 8-byte tag) · 4.3 A (per-boot session) · 4.4 A, then B · 4.5 to decide ·
4.6 A (+ optional wrapping) · 4.7 to decide.

Messages, all in our own range, only between our sensor and our gateway after an "OW" A6 that advertises the feature:

| Step | Direction | Content |
|---|---|---|
| Pairing, after A6 | sensor → gateway | its X25519 public key (32 B) |
| | gateway → sensor | its X25519 public key (32 B), then a confirmation tag under the derived key |
| After every boot (sensor) or gateway restart | sensor → gateway | hello: key ID, sensor nonce |
| | gateway → sensor | gateway nonce; both derive the session key |
| Every later frame | both | type, counter, AES-CCM ciphertext and 8-byte tag |

**Phases**, each one usable on its own:

1. Pairing key exchange and the table's "secure" mark; reports authenticated and encrypted (4.1, 4.2, 4.5,
   downgrade protection).
2. Sessions and the replay check (4.3).
3. OTA over the channel, without the button (4.4 A); `nv.json` and roaming carry keys (4.6).
4. Optional: signed images (4.4 B), provisioning-authenticated pairing (4.1 B), stock-sensor flags (4.7).

## 6. Costs (estimates, to measure on a board)

| What | Cost |
|---|---|
| Sensor flash | X25519 ~5–10 KB; CCM uses the hardware (a few hundred bytes of glue) |
| Pairing | ~1 s more (X25519 on both sides, in software) |
| Each frame | +8–12 bytes, ~13–19 ms more on the air (~1.6 ms per byte, measured) |
| After a boot | one round trip (hello / nonce) before the first report |
| Gateway | 16 B per sensor in NV; a session key and two counters per sensor in RAM (~40 B × sensors) |
| Host | `nv.json` carries keys; wyzesense2mqtt-rs must store and move them carefully |
| Unattended OTA of 100 sensors | each starts at its next check-in (≤ 4 h for v1 contacts); one at a time, ~2.5 min each, so ~4–5 h in all |

## 7. Questions for the user

1. 4.1: is "just works" pairing (A) acceptable, or is provisioning-authenticated pairing (B) wanted from the start?
2. 4.2: is confidentiality (G4) wanted, or authentication only?
3. 4.4: is the authenticated channel enough to drop the button for OTA, or do we want signed images too, and where
   should the signing key live?
4. 4.5: off by default behind a flag (R3), or on by default between our devices?
5. 4.6: may `nv.json` carry keys in clear for a trusted host, or wrapped only?
6. 4.7: anything for stock sensors (replay check, anomaly flags), or leave them?
