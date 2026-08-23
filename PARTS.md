# Parts list

Two phases. **Phase 1 costs about £15 and answers the questions that decide
everything else** — buy it first, sniff your live Growatt link, then commit to
the phase 2 hardware.

Prices are indicative (Aug 2026, UK) and exclude shipping. AliExpress is roughly
half the Amazon UK price on the generic modules but takes 2–3 weeks.

---

## Phase 1 — sniff the live GBLI ↔ SPH6000 link

You only need **one** CAN channel to listen. This is genuinely the important
purchase: it confirms the frame layout, settles the `0x313` voltage-scaling
question, and captures the real `0x301` keepalive that a genuine Growatt inverter
sends — which is undocumented everywhere I could find.

| # | Part | Qty | ~£ | Notes |
|---|---|---|---|---|
| 1 | **ESP32 DevKit V1** (ESP-WROOM-32, 30-pin) | 1 | 4–8 | Any classic ESP32. Not an ESP32-C3 — you want the TWAI peripheral. |
| 2 | **SN65HVD230 CAN transceiver module** (blue "CJMCU-230" / Waveshare) | 1 | 3–5 | 3.3 V native, so it wires straight to the ESP32 with no level shifting. |
| 3 | **RJ45 breakout board with screw terminals** | 1 | 4–8 | Winford `BRK8P8C`, CZH-Labs, or any "RJ45 to screw terminal" board. |
| 4 | **RJ45 splitter / 2-way buss board**, *or* a spare Cat5 patch lead to sacrifice | 1 | 3–6 | See "How to tap" below. |
| 5 | Dupont jumper wires, USB cable | — | 3 | You probably have these. |

**Total: ~£15–30.**

### ⚠️ Remove the 120 Ω resistor from the SN65HVD230 module

Almost every SN65HVD230 breakout has a 120 Ω termination resistor fitted (usually
`R2`, sometimes a jumper). Your battery↔SPH6000 bus is **already correctly
terminated at both ends**. Adding a third resistor drops the bus to 40 Ω, which
is out of spec and the recommendation below is still to remove it.

**Real-world update (2026-08-23):** a genuine GBLI6532↔SPH6000 link was
sniffed for an extended session - cold boot, idle, a full charge→discharge→
charge cycle, multiple wake tests - with a real SN65HVD230 module confirmed
(multimeter-verified) to still have its 120 Ω resistor fitted, i.e. running
at the out-of-spec 40 Ω the whole time. No communication issues observed on
either the sniffer's reception or the live battery↔inverter link itself. So
this is evidently more tolerant in practice than the strict spec suggests,
at least on a short, low-noise stub - but that's one data point on one
installation, not a green light to skip it. Two reasons to still remove it:
it's simply correct practice with no downside, and Phase 2's `translator`
firmware **actively transmits** on this bus rather than just listening,
which is a different electrical loading situation than passive sniffing.

Desolder it, or cut the jumper, before you plug into anything live. Check with a
multimeter across CAN_H/CAN_L on the module alone — you want open circuit, not
120 Ω.

### How to tap without cutting anything

The GBLI's **PCS** port is the one going to the SPH6000. CAN is on pins 4 (CAN_H,
blue) and 5 (CAN_L, blue/white), and CAN is a multidrop bus, so you can simply
hang a short stub off it:

```
  GBLI PCS ──────── existing cable ──────── SPH6000 battery port
                         │
                    RJ45 splitter
                         │
                    (≤ 30 cm)
                         │
              RJ45 breakout → pins 4,5 → SN65HVD230 → ESP32
```

Keep the stub **under 30 cm**. Don't terminate it. Don't connect pins 7/8 (WAKE)
— you're not the inverter and shouldn't pretend to be.

If you'd rather not buy a splitter: cut a spare patch lead, join blue to blue and
blue/white to blue/white through the breakout, and land a tail on the ESP32.

The sniffer firmware runs the CAN controller in **listen-only mode** — it never
ACKs, never transmits, never sends error frames. Your battery and inverter carry
on working while you watch.

---

## Phase 2 — the translator

Now you need **two** independent CAN channels.

**Possible new requirement, not yet confirmed:** a forum user pairing a
GBLI6532 with a non-Growatt inverter reports needing to actively drive the
PCS WAKE+ pin (pin 8, +5 V relative to WAKE−/pin 7) to wake the pack and
close its relays at all — see `SNIFFING.md`'s WAKE-pin section and the link
in `CLAUDE.md`'s Protocol references. If tomorrow's sniffing session confirms
this on the real SPH6000 link, Phase 2 will likely need a small addition (a
5 V source and a resistor to that pin) beyond what's listed below. Nothing
added to the list yet since the exact figures aren't confirmed.

### Recommended: Autosport Labs ESP32-CAN-X2 — ~$55 / ~£45

| Part | Qty | Notes |
|---|---|---|
| [ESP32-CAN-X2](https://www.autosportlabs.com/product/esp32-can-x2-dual-can-bus-automotive-grade-development-board/) | 1 | ESP32-S3, CAN1 = built-in TWAI, CAN2 = MCP2515 @ 16 MHz |
| RJ45 breakout with screw terminals | 2 | one per bus |
| Cat5 patch leads | 2 | to battery PCS, to Solis battery port |
| 5 V USB supply or 12 V | 1 | board takes 6–20 V on the JST header, or USB-C |

**Why this one:** its architecture is exactly what the firmware already targets —
TWAI on GPIO 6/7 for the battery bus, MCP2515 on SPI (CS 10, SCK 12, MISO 13,
MOSI 11, INT 3) for the inverter bus. It's the default `BOARD` setting in
`config.h`; nothing to port. It also has ±14 kV ESD protection on CAN_H/CAN_L and
**breakable jumpers for the onboard 120 Ω terminators**, which matters — see below.

**Caveat:** no galvanic isolation. Fine in practice for a short, fixed
installation, but see the isolated option below.

**Termination on this board:** both channels ship terminated. On the **Solis
side, break the jumper** — the inverter already terminates its end and the
battery... isn't there, so actually you want to think about this per bus:

- **Battery bus** (ESP32 ↔ GBLI PCS): this is now a two-node bus, ESP32 and
  battery. Keep the ESP32's terminator. Should read ~60 Ω end to end.
- **Inverter bus** (ESP32 ↔ Solis): also two nodes. Solis terminates internally,
  so **break the ESP32's jumper on this channel**. Measure: ~120 Ω, not 60 Ω.

Measure both with the DC breaker open before you power anything.

### Budget alternative: ESP32 DevKit + SN65HVD230 + MCP2515 — ~£20

Reuses your phase 1 parts. Build with the `-devkit` PlatformIO envs
(`pio run -e translator-devkit -t upload`) — no need to edit `config.h`, they
pass `BOARD = BOARD_ESP32_DEVKIT` at compile time.

| Part | Qty | ~£ |
|---|---|---|
| ESP32 DevKit V1 (from phase 1) | 1 | — |
| SN65HVD230 module (from phase 1) | 1 | — |
| MCP2515 + TJA1050 CAN module | 1 | 3–5 |

**There is a real gotcha here.** The ubiquitous blue MCP2515 module is a **5 V**
board. Its `SO` (MISO) line idles at 5 V, and the ESP32's GPIOs are **not 5 V
tolerant**. Wiring it up naively works for a while and then damages the ESP32.

Your options, least to most effort:

1. **Buy a 3.3 V-native module** — some ship with a TJA1051T/3 or SN65HVD230
   instead of the TJA1050. Read the listing photos carefully; the transceiver part
   number is printed on the chip.
2. **Put a divider on SO only** — 3.3 kΩ / 6.8 kΩ from `SO` to the ESP32's MISO
   to ground. Crude but effective; SPI at these speeds tolerates it.
3. **The split-rail mod** — cut the trace between the MCP2515's VCC and the
   TJA1050's VCC, feed the MCP2515 3.3 V and the TJA1050 5 V, plus a divider on
   the TJA1050's RXD output. Well documented, fiddly, and you're doing surgery on
   a board that sits in a battery safety path.

If you're leaving this installed permanently next to a 6.5 kWh battery, spend the
extra £25 on the CAN-X2 and skip all of that.

### If you want isolation

| Option | ~£ | Notes |
|---|---|---|
| [LilyGo T-2Can](https://github.com/Xinyuan-LilyGO/T-2Can) (non-FD, `T-2Can_V1.0`) | ~£27 | ESP32-S3. **Supported** — `BOARD_LILYGO_T2CAN` / `translator-t2can` env. Corrects an earlier note here: per LilyGo's own pin config and example firmware, it's actually **one TWAI channel + one MCP2515** (same architecture as the CAN-X2, different pins), not two MCP2515s — so no TWAI porting was needed, just pin numbers and a reset-pin pulse the CAN-X2 doesn't need. Its TWAI pins happen to match the CAN-X2's, so the existing `sniffer` env works for phase-1 sniffing on it too. Untested against real hardware here — see caveats below. |
| Stark CMR / BECom | £70+ | DIN-rail, isolated, purpose-built for battery↔inverter gateways. Overkill unless you're going the Battery-Emulator route. |

Galvanic isolation is worth having when one bus reaches a 48 V battery and the
other reaches a grid-tied inverter. It isn't strictly necessary, and plenty of
people run unisolated gateways for years — but it's the difference between a
ground fault costing you an ESP32 and costing you an inverter.

**T-2Can caveats, not yet confirmed against a real board:**
- **Isolation.** LilyGo markets "signal isolation design (SGND/DGND)" and third-party
  coverage calls it "dual isolated," but neither states the mechanism (optocouplers /
  isolated DC-DC per channel vs just split ground planes). Check the schematic
  (`T-2Can_V1.0.pdf` in their repo) before relying on it for true galvanic isolation.
- **Which physical port is which.** The board has two connectors; firmware-side,
  "CAN B" (GPIO 6/7, TWAI) is wired here to the **battery**, "CAN A" (MCP2515) to
  the **inverter** — matching this project's existing convention. Confirm against
  the board's silkscreen/schematic which physical connector is which before wiring.
- **MCP2515 oscillator.** Set to `MCP_8MHZ` in `config.h` on the strength of LilyGo's
  own example not overriding a library default — not confirmed against the
  schematic. If the MCP2515 never comes up, try `MCP_16MHZ`.
- Also note: it's the **non-FD** `T-2Can_V1.0` this project targets. The `T-2Can-Fd`
  variant uses an MCP2518 and a different library — not supported here.

---

## Consumables

| Item | Notes |
|---|---|
| 120 Ω resistors | a couple spare, for measuring and for fixing termination mistakes |
| Multimeter | non-negotiable — you'll be checking bus resistance and pin continuity |
| Cat5/Cat5e patch leads | 2–3, T568B |
| Small enclosure + DIN clip | for the permanent install |

---

## What to buy right now

Just phase 1. Everything about phase 2 — including whether the mapping in this
firmware is even correct for your pack — depends on what the sniffer shows you.

An ESP32 DevKit, an SN65HVD230 module, an RJ45 breakout, and a splitter. Under
£20, and you'll know within an evening of it arriving whether this whole approach
works.
