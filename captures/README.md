# CAN captures

Raw serial output from `pio run -e sniffer`, committed so the protocol decoding
can be checked against real traffic rather than against a datasheet.

**This is the point of the repo.** Growatt's LV BMS CAN protocol is documented,
but I could find no published capture from a **GBLI 6532** specifically — every
byte layout in `src/translate.h` is inferred from the spec plus a capture of the
sibling ARK pack. A capture here turns inference into fact.

## Capturing

```bash
pio run -e sniffer-devkit -t upload   # plain ESP32 DevKit + SN65HVD230 (phase 1 hardware)
pio device monitor | tee captures/$(date +%F)-gbli6532-sph6000.log
```

Let it run for at least a few minutes, ideally spanning a charge→discharge
transition so the signed current field is exercised in both directions.

**Also worth a separate capture: a cold boot.** Start logging *before*
powering the GBLI/inverter link on, not after — a steady-state capture can't
show the `0x301`/`0x311` handshake or when WAKE+ toggles relative to it,
since both only happen once at power-on. See "Bonus: capture a cold boot" in
`SNIFFING.md` for the detailed procedure. The interesting part is the first
~30 seconds; a longer tail afterwards is free insurance but not where the
handshake data is.

## Naming

```
YYYY-MM-DD-<battery>-<inverter>-<what-was-happening>.log
```

e.g. `2026-08-24-gbli6532-sph6000-idle.log`,
`2026-08-24-gbli6532-sph6000-charging-20A.log`,
`2026-08-24-gbli6532-sph6000-coldboot.log`

## What to note alongside each capture

Add a line to the table below. The cross-checks are what make a capture useful —
a log with no reference readings can't resolve a scaling question.

| Capture | Pack V (app/meter) | SOC (app) | Current (app) | Notes |
|---|---|---|---|---|
| _example_ | 52.5 V | 62 % | +20 A charging | |
| `2026-08-23-gbli6532-sph6000-coldboot.log` | not cross-checked | not cross-checked | not cross-checked | Real cold boot into a full idle → discharge (to −9.8 A) → charge (to +7.9 A) cycle, via manually toggling solar off/on. Resolved most of `CLAUDE.md`'s open questions 1, 2, 3, 5, 6. CAN-reported: 53.20–53.40 V, SOC 99%. |
| `2026-08-23-gbli6532-sph6000-battery-only.log` | not cross-checked | not cross-checked | not cross-checked | Battery powered (via its POWER button) with the inverter left off. Shows the battery transmits `0x311` etc. on its own but stays in a disabled `mode=0` state (`CCL=DCL=0.0A`) with no inverter present. CAN-reported: ~53.1 V, SOC 99-100%. |
| `2026-08-23-gbli6532-sph6000-wake-test.log` | not cross-checked | not cross-checked | not cross-checked | Inverter powered first (alone for a while, `0x301` only), then battery POWER button pressed with the exact moment noted live. Shows `mode=0 → mode=1` (fully enabled) within 2-4 seconds of the battery's first frame. Resolved `CLAUDE.md` open question 4 (WAKE not needed). |
| `2026-08-31-gbli6532-t2can-translator-dryrun.log` | not cross-checked | 96% | +0.0A idle | **First-ever capture from the real translator firmware (`translator-t2can`), not the sniffer** - README's commissioning Step 3, battery bus only, inverter bus unconnected. `chg=0 dis=0` on reconnect → `chg=1 dis=1, CCL=DCL=25.6A` within 4s, matching the wake-test pattern exactly. Clean throughout: SOH=99%, packs=2, no faults, `inv0x305=no` correctly reflecting no inverter connected. |
| `2026-08-31-gbli6532-t2can-solis-live-nodc.log` | not cross-checked | 87% | +0.0A idle | First `SNIFF_ONLY` off run against the real Solis, but with DC power deliberately left disconnected - confirms the entire CAN/data path (real Pylon frames, real Solis interpreting them, `inv0x305=yes`) with zero possibility of real current flow. Solis's own display showed `CCL`/`DCL=208.4A` exactly matching the datasheet-derived ceiling, SOC/voltage matching. Solis itself correctly reported "No Battery" as its overall status with DC disconnected - sensible behavior, distinguishing "valid BMS comms" from "usable connected battery". |
| `2026-08-31-gbli6532-t2can-solis-live-charging.log` | not cross-checked | 86-87% | -47.2A to +4.8A | **First fully live run** - `SNIFF_ONLY` off, real Solis attached, DC connected, README Step 5. Confirmed `CCL=DCL=208.4A` on the Solis's own display exactly matching the datasheet-derived 2-pack ceiling. Also captured a real event: the T-2Can was physically disconnected mid-discharge (to move it to a laptop) - battery responded with a protective `chg=0 dis=0` disable and its ALM light/relays opening (t=116s), recovering cleanly within ~4.5 minutes of reconnection (t=390s) with no repeat. `prot=`/`warn=` stayed `0000` throughout the whole event - whatever triggers this protective response isn't visible in the `0x312` bytes this project currently decodes. See `CLAUDE.md`'s safety posture notes. |
| `2026-08-31-gbli6532-t2can-laptop-power-10min.log` | 53.0-53.2V | 84% | -32.4A to +0.0A | Raw serial capture (`cat /dev/ttyACM0`, not `pio device monitor` - that needs a real TTY and can't run backgrounded/unattended) while diagnosing a reported "disconnects after ~10 minutes" issue. Gateway itself was rock solid throughout - `inv0x305=yes` held, no reboot, keepalive never missed a beat - but the battery's own `0x311` enable bits flipped `chg=1 dis=1` → `chg=0 dis=0` at t=648s (10m48s) and stayed disabled for the rest of the 16+ minute capture, no self-recovery. `prot=`/`warn=` stayed `0000`. First two clean measurements of the exact 594 s enable-to-disable interval (t=58s→652s and t=1090s→1684s, identical to the second). See `CLAUDE.md` open question 8 - unresolved. |
| `2026-08-31-gbli6532-sph6000-t2can-sniffer-594s-test.log` | not cross-checked | not cross-checked | +0.0A to -57.1A | Real SPH6000 reconnected directly to the battery (one shared bus, same RJ45-splitter tap technique as the original `SNIFFING.md` procedure), T-2Can as a passive listen-only tap (`sniffer-t2can` env, new - the plain `sniffer` env is missing the `ARDUINO_USB_CDC_ON_BOOT` flag the T-2Can needs). Ran 14m51s with **zero disables** - `chg_en=1 dis_en=1` the whole time, including through a sustained -56.6A real discharge. The decisive test for open question 8: a real Growatt inverter does not hit the 594s wall this gateway does. Also confirmed the real `0x301` payload is byte-for-byte identical to what this gateway sends. |
| `2026-08-31-gbli6532-t2can-batteryonly-594s-clean.log` | ~53.1V | 72% | +0.0A idle | Battery ↔ T-2Can only - no Solis, no SPH6000, cleanest possible topology, running the newly-instrumented `translator-t2can` (`VERBOSE_STATS` added to `main.cpp`/`config.h`). Confirmed the 594s interval a third time, independently: enable at t=68s (after a manual power cycle triggered by a brief comms gap from reflashing), disable at t=664s = 596s, within the 2s print-resolution margin of the first two measurements. Rules out the Solis or SPH6000 having anything to do with the timer - it's purely between this gateway's replay and the battery. |

## Open questions these should answer

1. Does the GBLI 6532 emit `0x311`/`0x312`/`0x313` at all, or something else?
2. Is `0x313` bytes 0–1 scaled **0.01 V** (Growatt spec) or **0.1 V**
   (`growattArkCAN` observation)?
3. What is the real `0x301` payload from a Growatt inverter, and does any byte
   change over time — especially during the power-on handshake, not just in
   steady state?
4. What are the actual transmit periods for each ID? Only `0x301`'s ~1 s cadence
   is documented.
5. What voltage/pattern does the inverter apply to the WAKE pins (PCS 7/8),
   and when relative to the `0x301`/`0x311` handshake? See `CLAUDE.md`'s open
   questions for why this one might mean new hardware, not just new config.
6. Does `0x311` byte 6 or byte 7 carry the charge/discharge-enable bits? This
   project's own tests concluded byte 7; a secondhand report elsewhere claims
   byte 6. See `CLAUDE.md`.

See `SNIFFING.md` for the wiring and the safety notes — in particular, remove the
120 Ω terminator from your transceiver module before tapping a live bus.

## Note

Captures are battery telemetry — voltages, currents, SOC. Nothing personally
identifying, so they're safe to publish. Do check before committing anything
you've hand-annotated, though.
