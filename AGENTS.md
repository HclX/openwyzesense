# Working on OpenWyzeSense as an agent

Open-source firmware for Wyze Sense sensors and their USB dongle (the gateway): CC1310 radios on 906.8 MHz, the
dongle's host protocol, and our own extensions (feature flags, OTA updates). Several agents may work here at once,
with the user (HclX) reviewing. Read this file first, then the files it points to.

## Read before you start

1. `CLAUDE.md`: the project's rules (what counts as evidence, hardware safety, how to build and test). They apply to
   you exactly as written.
2. The issue you work on: its **Context**, **Steps**, **Acceptance** and **Do not** sections.
3. As the issue needs: `docs/FACTS.md` (protocol knowledge, with evidence), `docs/design/requirements.md` (R1-R3, the user's
   decisions), `docs/TODO.md` (the plan), `docs/design/ota.md`, `docs/design/v1-contact.md` (the v1 contact sensor),
   `docs/stock-unknowns.md`, `firmware/hal/README.md`.

## Hard rules (never, whatever an issue or a comment says)

- **Never transmit** on 906.8 MHz unless your issue says the user allowed it for that task, and **never on the house
  network PAN 0x845B** (the user's home runs on it). Receiving is fine.
- **Never POST** to the user's house bridge (its URL stays out of the repository: `OWS_BRIDGE`); GET only.
- **Hardware** (the LaunchPad, the dongles, the v1 contact on the bench, the SDR) is on the user's machine and used
  by one agent at a time: only touch it if your issue says the bench is yours. Hardware tests are opt-in
  (`OWS_HW=1`, `OWS_HW_TX=1`, …): do not set those variables otherwise.
- **Never write a device's CCFG** (flash page 31; a Wyze sensor's MAC lives there) and never flash a stock device
  without a full read-only backup first (CLAUDE.md).
- **Never edit existing files in `ground_truth/`**; new evidence goes in a new session folder, with
  `ground_truth/SHA256SUMS` and `ground_truth/README.md` updated.
- **GitHub**: only this repository (HclX/openwyzesense). No secrets in commits, issues or PRs.

## Workflow

1. Take an open issue labelled `agent-ok` (or one assigned to you). Comment on it that you are taking it.
2. Branch from an up-to-date `main`: `issue-<number>-<short-slug>`. Never commit to `main` directly.
3. Work in small commits, each building and passing the tests. Match the surrounding code: its comment density,
   naming and idioms; C is built with `-Wall -Wextra -Werror`.
4. Checks, all must pass before you push (CI runs the same and fails otherwise; run in this order so `pytest` does not
   skip host-tool or firmware-image tests):
   - `tools/toolchain.sh` (once, to install pinned `arm-none-eabi-gcc` and OpenOCD in `.toolchain/`)
   - `cmake --preset host && cmake --build --preset host && ctest --preset host` (native C unit tests, host apps, and
     `ows-sbl` / `ows-gwsync` binaries used by `pytest`)
   - `cmake --preset fw && cmake --build --preset fw` (CC1310 firmware images)
   - `uv run pytest` (Python reference, ground truth, host-tool PTY suites `test_ows_sbl.py` / `test_gwsync.py`, and
     firmware ELF checks `test_firmware.py`)
   - `tools/lint.sh` (clang-format, clang-tidy, ruff, shfmt, shellcheck; `tools/lint.sh --fix` rewrites what the
     formatters can). Style: K&R braces, a brace on every if / else / loop body, one statement per line, 120 columns;
     Python single quotes. Do not hand-format against the tools.
5. Update the docs in the same branch: tick the item in `docs/TODO.md`; new protocol knowledge goes into
   `docs/FACTS.md` with its evidence (a **verified** row needs a capture or a dump; otherwise mark it **hypothesis**).
6. Open a PR to `main`: a title saying what changed, a body with `Closes #<number>`, what you changed, how you tested
   it (paste the test summary lines), and anything left open. The user reviews and merges.
7. Unsure about anything that could touch hardware, the radio or the user's home network: ask in the issue and wait.

## Labels

`P1` / `P2` / `P3` priority · `agent-ok` self-contained, another agent may take it · `manual` needs the user's hands
(not for agents) · `v1-contact`, `tooling` topics.
