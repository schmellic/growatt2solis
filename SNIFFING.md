# Sniffing your live GBLI 6532 ↔ Growatt SPH6000 link

Having the batteries currently on a Growatt inverter is the single most useful
thing about your situation. A working Growatt pair on the bench answers four
questions that no datasheet does:

1. **Does the GBLI actually speak the documented `0x311`-series protocol?**
   The Growatt LV BMS CAN spec is public, but I could not find a single published
   capture taken from a **GBLI 6532** specifically. Everything in this firmware's
   battery-side decoding is inferred from the spec plus a capture of the ARK
   pack. Your bus settles it.

2. **Is `0x313` scaled 0.01 V or 0.1 V?** Growatt's own document says 0.01 V; the
   `growattArkCAN` project observed 0.1 V. Both fit in an int16 for a 48 V pack.

3. **What is in `0x301`?** This is the keepalive the inverter sends *to* the
   battery, once per second. Growatt's spec gives an example payload
   (`11 22 33 44 55 66 77 88`) and no semantics at all. Your SPH6000 is sending
   the real thing right now. If the GBLI needs it to stay awake and talking, this
   capture is the difference between the gateway working and the battery going
   silent after 30 seconds.

4. **What do the WAKE pins actually do?** You can measure pins 7/8 on a working
   link instead of guessing.

---

## Before you start

The bus you're tapping is keeping a real battery and a real inverter talking. Two
rules:

- **Listen-only.** The sniffer firmware puts the CAN controller in
  `TWAI_MODE_LISTEN_ONLY`. It never sends an ACK bit, never transmits, never
  sends an error frame. Electrically it's a passive stub. Use the `sniffer`
  build, not `SNIFF_ONLY` in the translator build — the latter still has an
  active controller that ACKs.
- **No third terminator.** Remove the 120 Ω resistor from your SN65HVD230 module
  (see `PARTS.md`). Adding it to an already-terminated bus can break the working
  link between your battery and inverter.

---

## Wiring the tap

The GBLI's **PCS** port is the one that goes to the SPH6000 — not Link-In or
Link-Out, which carry a different bus on different pins.

```
  GBLI PCS ──────── existing cable ──────── SPH6000 battery port
                         │
                    RJ45 splitter        ← parallel-wired, all 8 pins
                         │
                    ≤ 30 cm stub
                         │
              RJ45 breakout
                    pin 4 (blue)      → SN65HVD230  CAN_H
                    pin 5 (blue/white)→ SN65HVD230  CAN_L
                                        SN65HVD230  3V3 → ESP32 3V3
                                        SN65HVD230  GND → ESP32 GND
                                        SN65HVD230  CTX → ESP32 GPIO 5
                                        SN65HVD230  CRX → ESP32 GPIO 4
```

Don't connect pins 7/8 (WAKE) — you're not the inverter and shouldn't act like
one. Don't connect the RS485 pair either.

Before plugging in: multimeter across the breakout's pins 4 and 5 with the stub
connected but the ESP32 unpowered. You should read the bus's normal ~60 Ω. If you
read ~40 Ω, you've left a terminator on your module.

---

## Running it

```bash
pio run -e sniffer-devkit -t upload   # plain ESP32 DevKit + SN65HVD230 (phase 1 hardware)
pio device monitor
```

(`sniffer` without `-devkit` targets the ESP32-S3 CAN-X2 board instead — use
`-devkit` for the phase 1 sniffer parts in `PARTS.md`.)

Commands (type the letter, then Enter):

| Key | Does |
|---|---|
| `r` | toggle raw frame logging |
| `d` | toggle decoded Growatt lines |
| `s` | print the statistics table now |
| `c` | clear statistics |

A statistics table prints automatically every 15 s.

---

## What you're looking for

### Expected — the good outcome

```
  ID     count   period(ms)  DLC  last payload             changing
  ----------------------------------------------------------------------
  0x301     120    1000 (998-1002)  8   11 22 33 44 55 66 77 88   ........
  0x311     240     500 (498-503)   8   02 38 02 58 03 84 00 62   ....*..*
  0x312     120    1000 (997-1004)  8   00 00 00 00 01 47 57 10   ........
  0x313     240     500 (498-502)   8   14 82 00 C8 00 EA 3E 64   ***** *.
  0x314     120    1000 (996-1003)  8   1A F8 1C 20 00 0C 00 2B   *.*...**
  0x319     120    1000 (999-1001)  8   60 0D 0A 0D 04 03 08 00   .****...
```

The `changing` column marks bytes that have ever changed value — a quick way to
see which fields are live telemetry and which are constants.

### The three things to write down

**1. The `0x313` voltage scaling.** The decode line prints both interpretations:

```
        0x313  Vraw=5250  -> 52.50V if 0.01V  /  525.0V if 0.1V  |  I=+20.0A ...
```

Compare against the pack voltage in the Growatt ShinePhone app. One of the two
will obviously be right. Then set `GROWATT_0x313_VOLT_SCALE` in `config.h` to
`VS_0V01` or `VS_0V1` rather than leaving it on `VS_AUTO`.

**2. The real `0x301` payload.** The sniffer flags this one specially. If your
SPH6000 sends something other than `11 22 33 44 55 66 77 88`, copy the actual
bytes into `sendGrowattKeepalive()` in `src/main.cpp`. Watch it for a minute —
if any byte changes over time (a counter? a checksum? a command?), that matters,
and it's worth capturing a longer log. A forum user trying the same GBLI6532
pairing with a non-Growatt inverter found that replaying a single static
`0x301` payload (`0B 16 21 2C 37 42 4D 58`, ~10 Hz) was **not** enough on its
own to get the BMS to report charge/discharge-enable — so don't stop at one
captured frame; capture several continuous seconds and watch for it changing.

**2b. Which byte of `0x311` actually carries charge/discharge-enable.** This
project's `translate.h` currently reads them from byte 7 (the low byte of the
big-endian pair) — found via this project's own host tests, correcting an
earlier assumption of byte 6. That same forum thread independently claims
byte 6 (bits 4/5/6 = wake/discharge/charge). Check both bytes in your capture
against what the pack is actually doing (charging vs idle vs discharging) to
settle this for real, rather than trusting either secondhand source.

**3. Anything unexpected.** Frames at IDs this firmware doesn't handle, unusual
periods, or `0x312` reporting a pack count other than 1. Save the whole serial
log — `pio device monitor` can tee to a file:

```bash
pio device monitor | tee growatt-capture-$(date +%F).log
```

### If you see nothing at all

In order of likelihood:

1. CAN_H and CAN_L swapped — try the other way round, you can't damage anything.
2. You tapped Link-In/Link-Out instead of PCS.
3. Termination — you left the resistor on your module, or the splitter has a long
   tail.
4. Wiring standard — check your patch lead is T568B and that pin 4 really is the
   solid blue conductor.

If you see nothing and the battery↔inverter link has *also* stopped working,
unplug immediately: that's the third-terminator failure mode.

### If you see completely different CAN IDs

Then the GBLI 6532 isn't speaking the protocol this firmware assumes, and the
whole battery-side decode needs revisiting. Save the log — that's a solvable
problem, just a different one, and the raw capture is everything needed to solve
it.

---

## Bonus: capture a cold boot

A steady-state capture can't answer two of the open questions: the `0x301`/
`0x311` handshake sequence (open question 3 in `CLAUDE.md`), and whether
WAKE+ toggles at a particular point in that sequence (open question 4). Only
a capture that starts *before* power-on can show that.

- **Start logging before power-on, not after.** Flash and start
  `pio device monitor | tee ...` while the GBLI/inverter link is still cold,
  so `t=0` in the log is genuinely before anything happens. Seeing the
  sniffer's own "30 s with no frames" warning while you get things powered
  up is expected, not a fault.
- **The valuable part is the first ~30 seconds**, not a long run. That's
  where you'd see whether WAKE+ appears before or after the first `0x301`,
  whether `0x311`'s status word/enable bits start at 0 and flip after some
  exchange or come up already set, and whether `0x301`'s payload differs in
  its first few frames versus later (a boot-only counter or command). A
  longer tail afterwards is free insurance for periodicity and catching
  anything infrequent (e.g. `0x320`'s mfr/hw/sw info might only ever send
  once) — worth leaving running for several minutes, but that part isn't
  where the interesting data is.
- **Name this capture distinctly** from a steady-state one, e.g.
  `2026-08-21-gbli6532-sph6000-coldboot.log` — see the naming convention in
  `captures/README.md`.
- **One caution, not a blocker:** if the SPH6000 currently powers anything in
  the house, a deliberate restart briefly interrupts that (whether it fails
  over to grid bypass depends on the setup) — plan around it rather than
  trigger it by surprise.
- If you can, note the wall-clock time of the actual power-on moment
  alongside the log — makes it much easier to find "frame 1 of the real
  sequence" versus noise later.

## Bonus: measure the WAKE pins

While you have a working link in front of you, put a meter on PCS pins 7 and 8
(`PCS-WAKE−` and `PCS-WAKE+`) relative to pin 3 or 6 (`GND-ISO`).

Whatever the SPH6000 is doing there, your gateway will probably need to do the
same, and this is the only easy chance to find out. Note whether it's a voltage,
a short, or open circuit.

This one matters more than it might look. A forum user pairing a GBLI6532
with a non-Growatt inverter reports that driving WAKE+ (pin 8) to +5 V
relative to WAKE− (pin 7/GND), through a series resistor, is enough to wake a
real pack and close its relays — their own experimental value (100 Ω), not a
confirmed spec figure. If that's right, **this project's gateway will need to
actively supply that voltage once the GBLI is no longer connected to a real
Growatt inverter** — nothing in `config.h`/`main.cpp` does that today. Get an
exact reading (voltage, and whether it's constant or pulsed) on the real
SPH6000 link if you can; it would settle both this project's Phase 2 hardware
list and directly answer the open question on that forum thread (still
unanswered as of 2026-08-21) — worth posting back if you get a clean reading.
