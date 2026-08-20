# Project context for Claude Code

Read this first. It carries the context from the session where this project was
designed, since conversation history does not transfer between Claude surfaces.

## What this is

An ESP32 CAN gateway. It sits between a **Growatt GBLI 6532** 48 V battery and a
**Solis S6-EH1P8K-L-PLUS** single-phase hybrid inverter, decodes Growatt's
low-voltage BMS CAN protocol, and re-emits it as the Pylontech LV CAN protocol
the Solis understands.

Owner: Paul. Location UK. The batteries are currently connected to a Growatt
SPH6000, which is being replaced by the Solis.

## Why it's needed

Growatt LV BMS CAN and Pylontech LV CAN are different protocols. Same bit rate
(500 kbit/s) and same ID width (11-bit), but different IDs, different field
layouts and **opposite byte order** (Growatt big-endian, Pylontech little-endian).
There is no DIP switch on the GBLI that changes its protocol — Growatt does
protocol selection on the inverter, and there's no Growatt inverter here.

`dalathegreat/Battery-Emulator` does not cover this pairing: its Growatt support
is for the **ARK HV** pack, not the GBLI LV. Its `PYLON-LV-CAN` inverter output
*is* the right output side, though — see "Future direction" below.

## Layout

```
src/config.h      all tunables: board pinout, safety clamps, protocol quirks
src/translate.h   pure protocol logic, no hardware deps, host-compilable
src/main.cpp      translator firmware (TWAI battery bus + MCP2515 inverter bus)
src/sniffer.cpp   listen-only sniffer, separate build env
test/             host-side test suite for translate.h
PARTS.md          hardware shopping list, two phases
SNIFFING.md       procedure for tapping the live Growatt link
README.md         wiring, commissioning, protocol reference
```

## Build and test

```bash
pio run -e sniffer    -t upload     # listen-only sniffer
pio run -e translator -t upload     # the gateway itself
pio device monitor

cd test && g++ -std=c++17 -I../src -o test_translate test_translate.cpp && ./test_translate
```

**Run the host tests after any change to `translate.h`.** They cover byte-level
encoding, the endianness flip, flag remapping, clamping and the stale-link
derate path, and they already caught one real bug (0x311 status bits are in the
low byte of the big-endian pair, i.e. byte 7, not byte 6).

Hardware target defaults to `BOARD_ESP32_CAN_X2` in `config.h`. `BOARD_ESP32_DEVKIT`
is the other supported option.

## Frame mapping

| Growatt (big-endian) | → | Pylontech (little-endian) |
|---|---|---|
| `0x311` CVL / CCL / DCL | → | `0x351` — both current fields stay **positive magnitudes** (Solis reads them into unsigned Modbus registers) |
| `0x313` SOC, SOH | → | `0x355` |
| `0x313` V / I / T | → | `0x356` — V normalised to 0.01 V; **current sign is identical on both sides (+ = charging), no flip** |
| `0x312` protection + warning | → | `0x359` — bit positions differ, explicitly remapped |
| `0x319` enables + force-charge | → | `0x35C` — bit positions differ |
| — | → | `0x35E` fixed `"PYLON   "` |
| `0x301` ← generated toward the battery | | `0x305` ← received from Solis, liveness only |

Solis inverter menu must be set to battery profile **`PYLON_LV`**.

## Open questions — resolve these from a real capture

These are the things that are inferred rather than confirmed. Do not treat them
as settled:

1. **Does the GBLI 6532 actually emit the `0x311`-series protocol?** Inferred
   from Growatt's LV BMS spec plus a capture of the sibling ARK pack. No
   published GBLI-specific capture was found.
2. **`0x313` bytes 0–1 scaling: 0.01 V or 0.1 V?** Growatt's spec says 0.01 V;
   `growattArkCAN` observed 0.1 V. Currently auto-detected by magnitude
   (`VS_AUTO`), which is unambiguous for a 48 V pack. Pin it once known.
3. **The real `0x301` payload** from a genuine Growatt inverter. Undocumented.
   Currently sends the example bytes from the spec.
4. **The WAKE pins** (PCS 7/8). Electrically undocumented. The GBLI may not wake
   and transmit without whatever the Growatt inverter does here.

`SNIFFING.md` is the procedure for answering all four from the existing
GBLI ↔ SPH6000 link.

## Safety posture

This sits in the control path of a 48 V ~100 A battery. The design principle
throughout: **when in doubt, ask the inverter for less, and fail toward
disconnection rather than toward uncontrolled current.**

- Battery silent > 5 s → limits zeroed, enables cleared, `0x359` system-error set
- Battery silent > 30 s → stop transmitting, Solis faults and disconnects
- All forwarded values clamped to the ceilings in `config.h`

Keep that posture. Any change that could make the gateway request *more* current
or a *higher* voltage than the BMS asked for needs a test in `test/` before it
lands.

Also relevant: the Solis appears to partly ignore the CAN charge-voltage limit
and use its own float setting. `0x351` CVL is not the only over-charge
protection, and shouldn't be treated as such.

## Future direction

The better long-term home for this is a `GROWATT-LV-BATTERY` decoder contributed
to `dalathegreat/Battery-Emulator`, whose `PYLON-LV-CAN` inverter side is already
mature and field-proven. `translate.h` is essentially the decoder that would need
porting. Doing so inherits its webserver, MQTT/Home Assistant integration, OTA and
event log.

One thing to know if you go there: Battery-Emulator's `PYLON-LV-CAN.cpp` `0x359`
over-current logic appears to have a sign bug — it treats positive current as
discharge, contradicting its own datalayer convention. This project drives those
bits from the BMS's protection flags instead.

## Protocol references

- Growatt BMS CAN-Bus protocol, low voltage V1.04 — https://www.amosplanet.org/wp-content/uploads/2022/04/Growatt-BMS-CAN-Bus-protocol-low-voltage-V1.04-1.pdf
- PYLON CANBUS protocol V1.2 — https://akkudoktor.net/uploads/short-url/oLZIl9bFdMC1doN4OnIvXbazHMl.pdf
- Solis Modbus register map (33141–33146 mirror the BMS CAN frames; used to cross-check scalings and the current sign) — https://akkudoktor.net/uploads/short-url/kbwIjN8pl8idrCP04UstaxtSmlC.pdf
- GBLI 6532 quick installation guide (PCS/Link pinouts) — https://solsol.eu/file/view/1805/growatt_instman_gbli-6532-quick-guide_en.pdf
- Solis S6-EH1P8K-L-PLUS manual — https://139708663.fs1.hubspotusercontent-eu1.net/hubfs/139708663/Fiches%20techniques/Solis/Solis_Manual_S6-EH1P8K-L-PLUS_EUR_V1,2(20251030).pdf
- martc55/Jbd2Solis — Pylon emulator verified against a Solis LV hybrid
- edibg/growattArkCAN — independent capture of Growatt `0x311`/`0x313`
