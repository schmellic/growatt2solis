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
and it's worth capturing a longer log.

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

## Bonus: measure the WAKE pins

While you have a working link in front of you, put a meter on PCS pins 7 and 8
(`PCS-WAKE−` and `PCS-WAKE+`) relative to pin 3 or 6 (`GND-ISO`).

Whatever the SPH6000 is doing there, your gateway will probably need to do the
same, and this is the only easy chance to find out. Note whether it's a voltage,
a short, or open circuit.
