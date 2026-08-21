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
