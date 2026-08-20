# Growatt GBLI 6532 → Solis S6-EH1P8K-L-PLUS CAN translator

An ESP32 gateway that sits between a Growatt GBLI 6532 battery and a Solis
S6-EH1P8K-L-PLUS hybrid inverter, decoding Growatt's low-voltage BMS CAN
protocol and re-emitting it as the Pylontech LV CAN protocol the Solis
understands.

> **Status: not yet validated against real hardware.** Four things are inferred
> rather than confirmed — whether the GBLI 6532 emits the `0x311`-series protocol
> at all, the `0x313` voltage scaling, the real `0x301` keepalive payload, and
> what the PCS WAKE pins do. They're listed in [`CLAUDE.md`](CLAUDE.md) and
> [`SNIFFING.md`](SNIFFING.md) is the procedure for answering them. If you have a
> GBLI 6532 on a Growatt inverter, a capture in [`captures/`](captures/) would be
> the first published one anywhere I could find.
>
> **No licence yet** — all rights reserved for now. Ask before reusing; I'll
> settle on something permissive once it's proven working.

> **Safety.** This sits in the control path of a 48 V, ~100 A battery. A bug here
> can mean the inverter charges past the BMS's limits. The battery's own
> protection MOSFETs remain the last line of defence, but do not treat them as
> the only one. Configure conservative charge voltage / SOC limits in the Solis
> menu as well, commission with the DC breaker in reach, and watch the first
> full charge and discharge cycle before leaving it unattended.

---

## Why a translator is needed

The two devices genuinely do not speak the same language:

| | Growatt GBLI 6532 | Solis S6-EH1P8K-L-PLUS |
|---|---|---|
| Protocol | Growatt LV BMS CAN | Pylontech LV CAN (`PYLON CANBUS V1.2`) |
| Bit rate | 500 kbit/s | 500 kbit/s |
| ID format | 11-bit standard | 11-bit standard |
| **Byte order** | **big-endian** | **little-endian** |
| Frames out | `0x311 0x312 0x313 0x314 0x315–0x318 0x319 0x320 0x321` | `0x351 0x355 0x356 0x359 0x35C 0x35E` |
| Frames in | `0x301` (keepalive from inverter) | `0x305` (keepalive from inverter) |

Bit rate and ID format match, so no rate conversion is needed — but every field
has to be re-laid-out, byte-swapped, and the fault bits re-mapped.

There is **no DIP switch on the GBLI 6532 that makes it emit Pylontech**. Growatt
does protocol selection on the *inverter*, not the battery, so with a Solis on
the other end there is nothing to select. Hence this gateway.

### Frame mapping

| Growatt | → | Pylontech | Notes |
|---|---|---|---|
| `0x311` CVL / CCL / DCL | → | `0x351` | byte-swapped; both current fields stay **positive magnitudes** |
| `0x313` SOC, SOH | → | `0x355` | promoted to uint16 |
| `0x313` V / I / T | → | `0x356` | voltage normalised to 0.01 V; **current sign is the same on both sides (+ = charging)**, so no flip |
| `0x312` protection + warning | → | `0x359` | bit positions differ — explicitly re-mapped |
| `0x319` enables + force-charge | → | `0x35C` | bit positions differ |
| — | → | `0x35E` | fixed `"PYLON   "` |
| `0x301` | ← | — | gateway generates it toward the battery |

---

## Hardware

You need **two independent CAN buses**. Do not join them — that is the whole point.

Full shopping list, with prices and the gotchas, is in **[`PARTS.md`](PARTS.md)**.
Short version:

| Board | `BOARD` in `config.h` | Notes |
|---|---|---|
| **Autosport Labs ESP32-CAN-X2** (~£45) | `BOARD_ESP32_CAN_X2` *(default)* | ESP32-S3. CAN1 = built-in TWAI, CAN2 = MCP2515 @ 16 MHz — exactly this firmware's architecture, nothing to port. Breakable 120 Ω jumpers. Not isolated. |
| **ESP32 DevKit + SN65HVD230 + MCP2515** (~£20) | `BOARD_ESP32_DEVKIT` | Budget. **The common blue MCP2515 module is 5 V and the ESP32 is not 5 V tolerant** — see `PARTS.md` before wiring it. |
| LilyGo T-2CAN (~£27) | — | Two *isolated* channels, both MCP2515. Cheaper and isolated, but the battery side needs porting off TWAI. |

Pin assignments for both supported boards live in the `BOARD` block at the top of
`config.h`.

### Termination

120 Ω at each end of each bus, and **only** at the ends. Both of your buses are
two-node: ESP32↔battery, and ESP32↔Solis.

- **Inverter bus:** the Solis terminates internally, so the ESP32 side keeps its
  terminator. End to end you should measure ~60 Ω.
- **Battery bus:** the GBLI terminates its end, so again keep the ESP32's. ~60 Ω.
- **When sniffing a live bus, remove the terminator entirely** — see
  [`SNIFFING.md`](SNIFFING.md). A third resistor on an already-terminated bus can
  break the working link.

Measure with a multimeter before powering anything.

---

## Wiring

### Battery end — GBLI 6532 **PCS** port

The GBLI 6532 has three RJ45 sockets. Use **PCS** (the inverter-facing one). Do
**not** use Link-In / Link-Out — those carry the inter-battery bus on different
pins and would silently not work.

| Pin | Signal |
|---|---|
| 1 | RS485-B |
| 2 | RS485-A |
| 3 | GND-ISO |
| **4** | **CAN_H** ← blue |
| **5** | **CAN_L** ← blue/white |
| 6 | GND-ISO |
| 7 | PCS-WAKE− |
| 8 | PCS-WAKE+ |

### Inverter end — Solis battery COM port

| Pin | Signal |
|---|---|
| **4** | **CAN_H** ← blue |
| **5** | **CAN_L** ← blue/white |

T568B on both ends. Pins 4/5 on both sides means a standard patch cable is
pin-compatible — you're just cutting each one and landing it on the ESP32.

### Two things that catch people out

1. **The WAKE pair (PCS pins 7/8).** A real Growatt inverter drives this, and the
   GBLI may refuse to come out of sleep and start transmitting CAN without it.
   If you see nothing on the battery bus in sniff mode, this is the first suspect.
   Its electrical behaviour is undocumented — check continuity/voltage on a
   working Growatt setup if you can, or try a link between 7 and 8.
2. **The master-select plug.** The GBLI uses plugs on the Link-In/Link-Out ports
   to elect the master pack. A standalone battery still needs the correct plug
   fitted or it may never enumerate.

### Termination

120 Ω at each end of each bus. The Solis has termination built in, so fit one
120 Ω resistor across CAN_H/CAN_L at the ESP32's inverter-side transceiver.
End-to-end you should measure ~60 Ω. Same on the battery side.

---

## Build

PlatformIO:

```bash
pio run -t upload
pio device monitor
```

Arduino IDE: install **coryjfowler/MCP_CAN_lib**, copy `src/*.cpp` and `src/*.h`
into a sketch folder, rename `main.cpp` to match the folder.

Run the host-side tests any time you change the translation logic — they check
byte-level encoding, the endianness flip, flag re-mapping, clamping and the
stale-link derate path, with no hardware required:

```bash
cd test && g++ -std=c++17 -I../src -o test_translate test_translate.cpp && ./test_translate
```

---

## Commissioning — do these in order

### Step 1 — sniff the existing Growatt link *(do this first)*

If the batteries are still connected to a Growatt inverter, tap that live bus
before changing anything. It settles four things that no datasheet does: whether
the GBLI really speaks this protocol, the `0x313` voltage scaling, the real
`0x301` keepalive payload, and what the WAKE pins do.

Full procedure in **[`SNIFFING.md`](SNIFFING.md)**. It uses a separate
listen-only firmware build that cannot disturb the working system:

```bash
pio run -e sniffer -t upload && pio device monitor
```

If the Growatt is already gone, the same build works against the battery alone —
you just won't capture `0x301`.

### Step 2 — fold what you learned into `config.h`

- Set `GROWATT_0x313_VOLT_SCALE` to `VS_0V01` or `VS_0V1` instead of `VS_AUTO`.
- If the real `0x301` payload differs from the spec example, update
  `sendGrowattKeepalive()` in `src/main.cpp`.
- If `0x312` reported more than one pack, check `PYLON_PACK_COUNT`.

### Step 3 — dry-run the translator on the battery alone

Build the `translator` env with `SNIFF_ONLY` still `true`, battery bus connected,
inverter bus not. Confirm the summary line is sane:

```
[42s] OK V=52.50V I=+20.0A T=23.4C SOC=62% SOH=100% | CVL=56.8V CCL=60.0A DCL=90.0A | ...
```

SOC should match the Growatt app. Current should be positive while charging.

### Step 4 — configure the Solis

Advanced Settings → Storage Energy Set → Battery Model: **`PYLON_LV`**.

(`Lithium Battery LV` is the documented fallback and also works, but `PYLON_LV`
is the profile a Solis has been verified against with this protocol.)

Then set the battery menu limits conservatively — max charge/discharge current,
over-discharge SOC, max charge SOC. The Solis appears to partly ignore the CAN
charge-voltage limit and use its own float setting, so **do not rely on `0x351`
CVL as your only over-charge protection**.

### Step 5 — go live

Set `SNIFF_ONLY` to `false`, reflash, connect the inverter bus. Within a few
seconds the Solis should stop reporting `Batt_Comm_FAIL` and show the battery
SOC. `inv0x305=yes` in the serial summary confirms the inverter is talking back.

Watch a full charge and a full discharge before leaving it alone. Check that the
inverter's reported battery power direction matches reality — if charge/discharge
appear inverted, the `0x356` current sign needs flipping (it shouldn't; both
protocols use positive = charging, but it's the one thing worth eyeballing).

---

## Failure behaviour

| Condition | Response |
|---|---|
| No battery frames yet | Nothing transmitted; Solis raises `Batt_Comm_FAIL` and stays disconnected |
| Battery silent > 5 s | Charge and discharge current limits forced to 0, `0x35C` enables cleared, `0x359` system-error set |
| Battery silent > 30 s | Transmission stops entirely; Solis faults and disconnects |
| BMS reports a protection | Re-mapped into `0x359`; Solis surfaces it as `Alarm-BMS` |
| Absurd BMS values | Clamped to the ceilings in `config.h` before forwarding |

The design principle throughout: when in doubt, ask the inverter for **less**,
and fail toward disconnection rather than toward uncontrolled current.

---

## A note on Battery-Emulator

[dalathegreat/Battery-Emulator](https://github.com/dalathegreat/Battery-Emulator)
already contains a mature, field-proven `PYLON-LV-CAN` **inverter** implementation —
the exact output side this project needs. What it lacks is a Growatt **low-voltage**
battery decoder (it supports Growatt ARK **HV** only).

So the better long-term route, once you've confirmed the frame layout on your own
pack, is to contribute a `GROWATT-LV-BATTERY` decoder to that project. You'd
inherit its webserver, MQTT/Home Assistant integration, OTA updates, event log and
its much broader testing — and `translate.h` here is essentially the decoder that
would need porting.

Worth knowing if you do: Battery-Emulator's `PYLON-LV-CAN.cpp` `0x359` over-current
logic appears to have a sign bug (it treats positive current as discharge, which
contradicts its own datalayer convention). This project drives those bits from the
BMS's own protection flags instead, which is more reliable anyway.

---

## Protocol references

- Growatt BMS CAN-Bus protocol, low voltage — [V1.04 PDF](https://www.amosplanet.org/wp-content/uploads/2022/04/Growatt-BMS-CAN-Bus-protocol-low-voltage-V1.04-1.pdf)
- PYLON CANBUS protocol V1.2 — [spec PDF](https://akkudoktor.net/uploads/short-url/oLZIl9bFdMC1doN4OnIvXbazHMl.pdf)
- Solis Modbus register map (registers 33141–33146 mirror the BMS CAN frames, which is how the field scalings were cross-checked) — [PDF](https://akkudoktor.net/uploads/short-url/kbwIjN8pl8idrCP04UstaxtSmlC.pdf)
- GBLI 6532 quick installation guide (PCS/Link pinouts) — [PDF](https://solsol.eu/file/view/1805/growatt_instman_gbli-6532-quick-guide_en.pdf)
- Solis S6-EH1P8K-L-PLUS user manual — [PDF](https://139708663.fs1.hubspotusercontent-eu1.net/hubfs/139708663/Fiches%20techniques/Solis/Solis_Manual_S6-EH1P8K-L-PLUS_EUR_V1,2(20251030).pdf)
- `martc55/Jbd2Solis` — a JBD-BMS→Solis Pylon emulator, verified working against a Solis LV hybrid — [GitHub](https://github.com/martc55/Jbd2Solis)
- `edibg/growattArkCAN` — independent capture of Growatt `0x311`/`0x313` — [GitHub](https://github.com/edibg/growattArkCAN)
