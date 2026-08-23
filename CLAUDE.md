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
pio run -e sniffer    -t upload     # listen-only sniffer (ESP32-CAN-X2)
pio run -e translator -t upload     # the gateway itself (ESP32-CAN-X2, default)
pio device monitor

cd test && g++ -std=c++17 -I../src -o test_translate test_translate.cpp && ./test_translate
```

**Run the host tests after any change to `translate.h`.** They cover byte-level
encoding, the endianness flip, flag remapping, clamping and the stale-link
derate path, and they already caught one real bug (0x311 status bits are in the
low byte of the big-endian pair, i.e. byte 7, not byte 6).

Three `BOARD` options in `config.h`: `BOARD_ESP32_CAN_X2` (default, Autosport
Labs ESP32-CAN-X2), `BOARD_ESP32_DEVKIT` (budget, plain ESP32 + SN65HVD230 +
MCP2515), `BOARD_LILYGO_T2CAN` (LilyGo T-2Can, non-FD). The latter two have
their own PlatformIO envs (`translator-devkit`/`sniffer-devkit`,
`translator-t2can`) that pass `-D BOARD=...` at compile time — no `config.h`
edit needed, just pick the matching `-e`. T-2Can's TWAI pins match the CAN-X2's,
so plain `sniffer` covers it too; DevKit needs `sniffer-devkit`. CI builds all
of these on every push.

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

## Open questions

Resolved ones are kept here rather than deleted, so the evidence trail (and
the fact that this was inferred/secondhand before) doesn't get lost.

1. **RESOLVED — the GBLI 6532 does emit the `0x311`-series protocol.**
   Confirmed directly against a real capture (2026-08-23,
   `captures/2026-08-23-gbli6532-sph6000-coldboot.log`): `0x311`–`0x320` all
   present, plus five IDs not currently decoded by this project at all —
   `0x322`, `0x323`, `0x324`, `0x329`, `0x330`. Worth a look at some point,
   not urgent.
2. **RESOLVED — `0x313` bytes 0–1 are 0.01 V.** Confirmed against the same
   capture: raw values read 53.20–53.40 V through an idle → discharge →
   charge cycle, which is sane for this pack; the 0.1 V interpretation would
   be ~530 V. `config.h` is now pinned to `VS_0V01` (was `VS_AUTO`).
3. **Mostly resolved — the real `0x301` payload.** Confirmed static
   (`0B 16 21 2C 37 42 4D 58`) across the entire real capture above — no
   counter, no checksum, no change through boot, idle, discharge, or charge.
   Matches what's already hardcoded in `sendGrowattKeepalive()`. A forum
   thread (see Protocol references) reported that replaying this same
   payload statically wasn't enough to get charge/discharge-enable from a
   GBLI6532 paired with a *non-Growatt* inverter — but a real Growatt
   inverter sending it evidently works fine, so whatever that poster was
   missing likely isn't the `0x301` payload itself. Still not captured: the
   very first cold power-on moment (see item 4).
4. **Narrowed down, still not fully confirmed — the WAKE pins (PCS 7/8).**
   The GBLI6532's own quick installation guide (see Protocol references)
   settles the mechanism question, even without pinning down the electrical
   details: **§5.1 "Power on Battery" lists two documented methods side by
   side — pressing the physical POWER button on the unit, or "PCS voltage
   signal activates battery."** The latter is almost certainly what
   PCS-WAKE+/WAKE− (pins 7/8) are for. So this isn't just a forum workaround
   - it's a real, designed feature, and this project's gateway will likely
   need to drive it eventually for the battery to wake without someone
   pressing the button by hand. Still open: the exact voltage/timing (a
   sustained level vs a pulse), since a multimeter check on the real
   SPH6000 link read ~0 V in steady state (consistent with either a
   power-on-only pulse, or WAKE simply not being used by this particular
   inverter pairing - the manual doesn't say it's mandatory, only that it's
   *a* way to activate the battery). **The GBLI6532 also has a documented
   power button** (§5.2): hold it 2 seconds to turn the pack on or off - the
   correct way to power-cycle it, safer than disconnecting DC terminals
   directly. It also self-powers-off automatically 25 minutes after losing
   its CAN link to whatever's on its PCS port, independent of anything this
   project's firmware does.
5. **RESOLVED — `0x311` byte 7 (not byte 6) carries charge/discharge-enable,
   confirmed against real operating state.** `translate.h`'s existing byte-7
   read was cross-checked against the pack's actual state (idle/charging/
   discharging) in the real capture and tracked it correctly throughout; byte
   6 stayed constant. The secondhand claim of byte 6 (from the same forum
   thread) doesn't hold up against real data.
6. **RESOLVED (new finding, not one of the original four) — `0x319`'s own
   enable bits don't track live state.** The real capture showed `0x319`
   byte 0 reading a constant `0xC0` (discharge-enable bit clear) through an
   *actual* ~10 A discharge, while `0x311`'s enable bits and status/mode
   field tracked it correctly. `translate.h` trusted `0x319` over `0x311`
   whenever both were present, which would have meant **the gateway never
   telling the Solis discharge was allowed, even during a real discharge.**
   Fixed: `0x311` is now the authoritative source for both enable bits;
   `0x319` is used only as a fallback before the first `0x311` arrives
   (`ENABLE_0x319_FALLBACK` in `config.h`, renamed from
   `ENABLE_0x311_FALLBACK` since its meaning flipped). Covered by test [9] in
   `test/test_translate.cpp`.

`SNIFFING.md` is the procedure for answering all of these from the existing
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
- [DIY Solar Forum: GBLI6532 wake/handshake with non-Growatt inverter](https://diysolarforum.com/threads/gbli6532-wake-handshake-with-non%E2%80%91growatt-inverter-can-0x301-0x311-behaviour.115292/) —
  someone hitting this exact problem (GBLI6532 → GoodWe). Unanswered as of
  2026-08-21. Their own WAKE-pin/byte6 findings are folded into "Open
  questions" above with appropriate caveats — they're independently useful
  but not a substitute for our own capture.
