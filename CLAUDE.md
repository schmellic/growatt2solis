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
captures/         real CAN captures from a genuine GBLI6532 + SPH6000 pairing
enclosure/        parametric OpenSCAD enclosure for the Phase 2 translator board
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
   present, plus five IDs not in Growatt's published spec - `0x322`, `0x323`,
   `0x324`, `0x329`, `0x330`. None of these are needed for the Pylon output
   mapping, so `translate.h` doesn't touch them, but `sniffer.cpp` now
   decodes three with best-effort, clearly-labelled ("GUESS:") guesses from
   the real capture data: `0x324` looks like a paged ASCII serial/model
   string (page 0 → `"GPJ021"`, page 1 → `"9522062"`, page 2 →
   `"BB7"`/`"C07"` - the last chunk differs between the two parallel packs);
   `0x329`'s byte 0 cycles 1/2 exactly matching the 2-pack system, so it's
   likely per-pack telemetry, units unconfirmed; `0x330` is by far the
   highest-frequency ID seen (10k+ frames in one capture) and reads like
   individual cell-voltage pairs in mV, similar to `0x315`-`0x318`. `0x322`
   and `0x323` never varied at all across any capture, so there's nothing to
   decode yet beyond raw bytes.
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
4. **Practically resolved — this project's gateway does not need to drive
   WAKE.** The GBLI6532's own quick installation guide (see Protocol
   references) documents two power-on methods side by side - the physical
   POWER button, or "PCS voltage signal activates battery" (almost certainly
   PCS-WAKE+/WAKE−, pins 7/8) - so WAKE is a real designed feature, not just
   a forum workaround. But two clean, deliberately-isolated captures
   (2026-08-23, `captures/2026-08-23-gbli6532-sph6000-battery-only.log` and
   `captures/2026-08-23-gbli6532-sph6000-wake-test.log`) showed the actual sequence
   that matters for this project: with the battery powered via the POWER
   button and a real Growatt inverter already sending `0x301` (exactly what
   this project's `sendGrowattKeepalive()` already does, unmodified), the
   battery's `0x311` enable bits went from `mode=0` (disabled, `CCL=DCL=0.0A`)
   to `mode=1` (`chg_en=1 dis_en=1`) **within 2-4 seconds of its first
   frame on the bus** - no minutes-long self-test, no extra handshake beyond
   what's already implemented. (An earlier theory here, that a multi-minute
   inverter self-test was what caused the enable flip, turned out to be
   wrong - that transition was actually a manual POWER-button press that
   hadn't been mentioned in the moment. Correcting it here so the reasoning
   trail is honest, not just the conclusion.) The GBLI6532's own display
   cycling through Standby -> Checking -> Normal over a longer, separately-
   timed period appears to be the inverter's own broader readiness check,
   not something gating the battery's CAN-level enable state.
   **Remaining gap, not blocking:** the exact WAKE electrical
   characteristics (sustained level vs. pulse, voltage) are still
   unconfirmed - a multimeter check in steady state read ~0 V - but since
   the POWER button plus this project's existing `0x301` behavior already
   gets to fully enabled in seconds, driving WAKE doesn't look necessary for
   Phase 2 hardware. **The GBLI6532 also has a documented power button**
   (§5.2 of the same guide): hold it 2 seconds to turn the pack on or off -
   the correct way to power-cycle it, safer than disconnecting DC terminals
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
7. **New finding, practically resolved — the battery protectively disables
   itself if it loses CAN comms, independent of this gateway's own
   derate/silence logic.** During the first live run against a real Solis
   (2026-08-31, `captures/2026-08-31-gbli6532-t2can-solis-live-charging.log`),
   the T-2Can was physically disconnected mid-discharge to move it to a
   laptop - dropping both CAN links at once. The battery responded by
   setting `chg_en=0 dis_en=0` and opening its own relays (ALM light lit)
   within its normal ~1 s telemetry cadence, well before this gateway's own
   5 s/30 s derate/silence timers would even trigger - this is the battery's
   *own* BMS protecting itself, not something this project's firmware
   caused or controls. It recovered cleanly within ~4.5 minutes of
   reconnection (`CCL`/`DCL` starting at the same 25.6 A seen in every other
   re-enable capture before ramping to the full 208.4 A ceiling), with no
   repeat and no lasting effect. **Still open:** `prot=`/`warn=` (the
   `0x312` bytes this project decodes) stayed `0000` throughout the entire
   event - whatever internally triggers this isn't visible in the bytes
   currently parsed. Doesn't block anything (the behavior itself is exactly
   the "fail toward disconnection" this project wants), but worth knowing:
   don't expect to see this coming via `0x312`'s protection/warning flags,
   and avoid disconnecting the gateway mid-operation unless deliberate.
8. **RESOLVED (2026-09-03) — root cause found: this gateway was sending
   the wrong `0x301` payload from the very first commit.** See the full
   resolution at the end of this item. The investigation trail below is
   kept in full because it's what got there, and because most of it
   remains genuinely true and useful (WAKE ruled out, TWAI transmit
   reliability verified, the standalone-timer reframe) even though the
   headline mystery had a much simpler explanation underneath it all.
   Originally: the GBLI6532 latches `chg_en=0 dis_en=0` exactly 594 s
   (9m54s) after every enable, independent of CAN traffic content, and
   does not self-recover. First seen 2026-08-31 during ordinary live operation (no
   comms interruption at all - distinct from item 7 above). Reproduced four
   times total the same day; the clearest evidence is in
   (`captures/2026-08-31-gbli6532-t2can-laptop-power-10min.log`, raw serial
   capture via `cat` rather than `pio device monitor`, which needs a real
   TTY and can't run unattended/backgrounded), which spans two full cycles:
   `chg_en` went 0→1 at t=58s and 0→1 again (after a manual POWER-button
   cycle) at t=1090s, and **both times it dropped back to 0 exactly 594
   seconds later** (t=652s and t=1684s) - not "about 10 minutes," the exact
   same interval to the second, twice independently. That precision rules
   out a coincidence or a soft threshold - it's a real countdown timer
   internal to the BMS, keyed off the enable transition itself, not off
   power-on or anything on the bus. Throughout both cycles the gateway
   itself never missed a beat (`inv0x305=yes` held, no reboot, keepalive at
   its normal 1 Hz) and `prot=`/`warn=` stayed `0000` - same blind spot as
   item 7, no visibility into *why* via the bytes this project decodes.
   Two theories were floated and **both failed to survive checking against
   evidence already in hand**, worth keeping here so they aren't tried
   again: (a) a sustained WAKE+/WAKE− signal being required long-term -
   ruled out because a real multimeter check against the genuine SPH6000
   pairing read ~0 V steady-state on those pins, so a real Growatt inverter
   evidently doesn't hold them at any voltage either; (b) the BMS
   distrusting a *literally* byte-identical `0x301` replayed forever - an
   untested guess, dropped in favor of getting real data instead of trying
   more firmware changes blind. Cross-checked against the official spec
   (freshly re-fetched and read in full, not from memory): no documented
   watchdog/heartbeat requirement beyond the 1 Hz `0x301` this project
   already sends unchanged, so nothing there points to a missing step
   either.

   **Confirmed: this is specific to this gateway's replay, not inherent to
   the battery.** Reconnected the real SPH6000 directly to the battery
   (`captures/2026-08-31-gbli6532-sph6000-t2can-sniffer-594s-test.log`,
   passive tap via a new `sniffer-t2can` PlatformIO env - the plain
   `sniffer` env lacks the `ARDUINO_USB_CDC_ON_BOOT` flag the T-2Can needs
   for serial output, which would have silently produced an empty log
   again). The real inverter ran **14m51s with zero disables**, `chg_en=1
   dis_en=1` the entire time including through real discharge at -56.6A -
   well past three separate 594 s failures on the same battery. A real
   Growatt inverter simply doesn't hit this wall.

   Two more candidate explanations were checked directly against data
   already in hand, rather than guessed at, and **both ruled out**:
   - **Payload content:** the real `0x301` is byte-for-byte
     `0B 16 21 2C 37 42 4D 58` - identical to what this gateway already
     sends - for the entire 14m51s. Not a "distrusts a static replay" issue.
   - **Frame-rate/load correlation:** the real capture initially looked
     dramatically busier (a raw cumulative average suggested `0x311` at
     ~76 Hz vs. this gateway's ~1 Hz), but that number was skewed by a
     large burst in the capture's first ~20s. Re-measured from a clean
     steady-state window far from the burst, and separately checked
     whether rate tracks load (it doesn't - frame rate stayed flat across
     both an idle stretch and a sustained -57 A discharge in the same
     capture) - so neither "poll rate" nor "load" explain the gap the way
     first suspected. Left here so this isn't re-investigated from scratch.

   Reproduced the exact 594 s interval a **third** time in the cleanest
   possible topology - battery and gateway only, no Solis, no SPH6000 at
   all
   (`captures/2026-08-31-gbli6532-t2can-batteryonly-594s-clean.log`,
   enable t=68s -> disable t=664s = 596 s, within the 2 s print-resolution
   margin of the other two) - which also rules out the Solis or any
   shared-bus artifact having anything to do with it. Added `VERBOSE_STATS`
   to `config.h`/`main.cpp` (2026-08-31): a per-ID frame-rate table,
   identical format to `sniffer.cpp`'s, built into the translator firmware
   itself so it can be compared directly against a sniffer capture without
   needing a second device on the bus.

   **2026-09-01 to 2026-09-03: several more theories checked and ruled
   out, then a real lead found.** In rough order:
   - **WAKE pin, revisited with real instrumentation - now conclusively
     ruled out.** Built an optocoupler-isolated (PC817) tap on
     WAKE+/WAKE-, feeding an interrupt-driven edge logger on a spare GPIO
     (`sniffer.cpp`, DevKit build only). First pass logged zero edges
     across 30+ minutes of healthy real-SPH6000 operation plus a full
     disable/re-enable cycle, but a positive-control check (deliberately
     applying a known 3.3V test pulse) revealed the circuit genuinely
     couldn't detect anything - the module's own onboard pull-up was far
     too strong for how weak this specific part's phototransistor
     response turned out to be, and the LED was under-driven at 3.3V too.
     Root-caused with real measurements, not guessing: swapping in an
     external 1M ohm pull-up (replacing, not paralleling, the module's own
     10k - two in parallel can only ever be *lower* than either alone, a
     mistake caught and corrected mid-test) plus a brighter LED drive (5V
     instead of 3.3V, ~19mA instead of ~10.5mA) took the response from an
     unusable 0.3-0.4V droop to a clean 0.86V droop, comfortably past the
     ESP32's ~0.825V guaranteed-LOW threshold. Re-ran the full positive-
     control test with the corrected circuit and firmware (`INPUT`, not
     `INPUT_PULLUP` - the internal ~45k pull-up would have undone the
     external 1M the same way the module's own 10k did) and it now
     reliably logs real edges. With a *validated* detector, re-ran the
     real-system test end to end - 30+ minutes healthy, a full disable,
     a full re-enable - and got zero edges again, this time meaning it.
     Independently reconfirmed a third way: built an RJ45 cable with only
     pins 4/5 (CANH/CANL) wired, WAKE genuinely absent rather than just
     inert, and the real SPH6000 both sustained the battery for 41+
     minutes *and* revived it from the locked state (see below) using
     nothing but those two wires. Three independent lines of evidence,
     three different angles, all agreeing: WAKE plays no role in any of
     this. Closed for good.
   - **The real inverter's own `0x301` rate is dynamic, but not in a way
     that explains this.** With no battery on the bus at all, a real
     SPH6000's `0x301` rate jumps from its normal ~1 Hz to ~60-90 Hz -
     confirmed directly, sustained, not a burst artifact. But this only
     happens at connection/reconnection; re-checked the full 14m51s clean
     capture end-to-end and the rate never bursts again once steady - so
     it's a "still looking for a battery" behavior, not a periodic
     re-confirmation the battery could be keying its trust off. Doesn't
     explain the 594 s wall, since in our own failures the battery is
     responding normally to this gateway right up until it isn't.
   - **Extended (29-bit) CAN IDs, from a different project's Growatt
     decoder.** `ai-republic/bms-to-inverter`'s Growatt module polls each
     field individually via extended-ID requests (e.g. `0x1311`) rather
     than a single broadcast keepalive. Checked directly against the real
     14m51s capture: every frame ID in it is a standard 3-hex-digit ID,
     nothing extended ever appears. Not what a real GBLI6532/SPH6000 pair
     actually does - ruled out.
   - **Full byte-by-byte diff, every ID, not just the ones already
     decoded.** Compared raw payloads immediately before/after both a
     disable and a re-enable transition, across every ID including the
     ones long marked "never varies" (`0x322`, `0x323`) and the
     best-effort GUESS ones (`0x329`, `0x330`, `0x314`, `0x319`). This is
     where the real lead turned up - see below. Everything else stayed
     either completely static or showed only ordinary continuous noise
     (cell-voltage drift, capacity-accounting jitter) present equally on
     both sides of the transition, not gated by it.

   **Real lead found: `0x323` byte 4 tracks the disabled state.**
   `0x323`'s full payload is `20 0E 10 00 XX 00 00 00`; byte 4 (`XX`) had
   been `00` in every capture so far and was assumed static. It isn't -
   confirmed independently in two separate sessions/topologies:
   - `captures/2026-09-03-gbli6532-sph6000-devkit-can-wake-combined-session.log`:
     disable at t=2388.243s, byte 4 flips `00→04` at t=2389.027s (0.8s
     later), flips back `04→00` at t=2499.197s, re-enable at t≈2501.27s
     (2s later).
   - `captures/2026-09-03-gbli6532-devkit-t2can-swap-test.log`: byte 4
     flips to `04` at t=502.053s, disable at t=507.5s (5.4s later this
     time - the flip led rather than followed).

   Tightly clustered around the transition in both cases (single-digit
   seconds either side), not scattered through the hundreds of seconds of
   otherwise-normal operation in between - this rules out coincidence, but
   the inconsistent before/after ordering between the two instances means
   it's not yet clear whether byte 4 is a cause, a symptom, or something
   else changing at roughly the same moment. Nothing in the official spec
   documents `0x323` at all (it's one of the five IDs outside Growatt's
   published spec, alongside `0x322`, `0x324`, `0x329`, `0x330`). Byte 4
   going `0x04` (bit 2 set, on the simplest reading) is the first
   CAN-visible signal this project has found all project that actually
   correlates with the 594 s mystery, rather than staying silent through
   it the way `prot=`/`warn=`/everything else has. Root cause of what
   byte 4 *is* - and whether it's readable early enough to act on - still
   open. **Confirmed a third time, independently** (2026-09-03,
   `captures/2026-09-03-gbli6532-t2can-clean-burst-test.log`, the
   cleanest single-cycle test run all project): flips at t=564.246s, 1.1s
   *before* the disable at t=565.357s. That makes it two leads and one
   lag out of three - most likely just measurement noise around a signal
   genuinely tied to the same underlying moment (both `0x311` and `0x323`
   are on their own independent ~1 Hz cycles, not synchronized to each
   other), rather than a reliably-early warning we could act on yet.

   **`0x311`'s own timing can go bursty too - but it's a different
   phenomenon, not a predictor of the 594 s disable.** While digging
   through the byte-diff data, noticed `0x311` (and everything else the
   battery broadcasts) can shift from a clean, metronomic exactly-1.000s
   cadence to tight clusters of frames 11-23ms apart, separated by
   ~0.8-1.3s gaps. Traced when this starts in
   `captures/2026-09-03-gbli6532-sph6000-devkit-can-wake-combined-session.log`:
   right around t=1771s, exactly when the SPH6000 was disconnected - not
   near the eventual disable at t=2388s at all. Matches something noticed
   right back at the start of this project too: the very first real
   SPH6000 capture also opened with a burst, right when the sniffer tap
   was first connected. The pattern that fits both: bursting correlates
   with an *unconfirmed/unstable* connection state in general (freshly
   connecting, or the counterpart having just vanished entirely) - not
   specifically with approaching this gateway's own 594 s wall. Confirmed
   directly: ran a fully clean gateway-only session
   (`captures/2026-09-03-gbli6532-t2can-clean-burst-test.log`) where the
   connection was already well-established, and `0x311` stayed perfectly
   metronomic straight through its own disable at t=565.357s - no burst
   at all. So this is a real, separate phenomenon, not a second predictor
   of the mystery - worth knowing about, not worth chasing further as a
   lead on its own.

   **Major new finding: a real inverter can revive an already-locked
   battery with nothing but a fresh CAN reconnection - no power cycle.**
   Never directly tested before today. Sequence: battery locked via this
   gateway's own 594 s wall (`chg_en=0 dis_en=0`, confirmed via `0x311`
   and via `0x323` byte 4 reading `0x04`), then swapped the T-2Can out
   for the real SPH6000 *without* touching the battery's power at all.
   Confirmed disable at t=565.357s in the T-2Can session; confirmed
   re-enable at t=760.195s after the SPH6000 was reconnected - 194.8s
   between disable and re-enable. Done with the CAN-only cable (pins 4/5
   only, WAKE genuinely absent), so this is also further, independent
   confirmation that WAKE has nothing to do with any of this - it
   doesn't even matter for *recovery* from the locked state, not just
   for staying out of it in the first place. See
   `captures/2026-09-03-gbli6532-t2can-clean-burst-test.log` for both
   the disable and the SPH6000's revival, logged in the same continuous
   capture.

   **Correction from a repeat run: that 194.8 s figure was measurement
   slop, not a real delay - the battery's relays click immediately, every
   time, the instant the SPH6000 is actually physically reconnected.**
   Re-ran the identical test for extra confidence
   (`captures/2026-09-03-gbli6532-sph6000-revival-repeat.log`): battery
   confirmed still locked at t=21.054s and t=44.504s, SPH6000 physically
   reconnected at t≈44.5s, and `chg_en`/`dis_en` flipped back to 1 at
   t=46.033s - about 1.5s later, which given how the reconnection
   timestamp was itself only a rough spoken-aloud reference point (not a
   precise event marker), is consistent with an immediate response.
   Paul confirmed directly: every time the SPH6000 has been plugged back
   in during this project's testing, the packs have come back to life
   and the relays have clicked **immediately** - there is no inverter
   self-test delay gating this, that theory (floated after the first
   trial, to explain the apparent 194.8s gap) was wrong. The real
   explanation for the first trial's 194.8s figure is almost certainly
   mundane: it measures from the battery's own disable event to the
   eventual re-enable, and that window includes however long the
   physical act of swapping the T-2Can out for the SPH6000 actually
   took, not any electrical or protocol delay. The corrected, sharper
   finding: once a real inverter is actually physically present and
   sending on the bus, the battery's response is immediate, not merely
   "fast" - reinforcing how stark the contrast is with this gateway,
   which does not revive the battery on reconnection at all.

   **Reframe: the 594 s disable itself is not caused by this gateway at
   all - it happens even with the battery connected to nothing.** Paul
   confirmed directly (2026-09-03): the same enable-then-disable cycle,
   on the same ~594 s timescale, happens with the battery powered on via
   its POWER button and genuinely nothing plugged into its PCS/Link port
   - no gateway, no inverter, no CAN device of any kind. Observed
   directly off the battery's own display/LEDs during the setup mistake
   captured (then deleted, per Paul's request) as
   `2026-09-03-gbli6532-t2can-revival-test.log`'s precursor ("Nothing is
   connected to the battery oops") - so this is a direct visual
   observation, not a logged CAN capture. This is the single biggest
   reframe of open question 8 to date: the ~594 s enable→disable cycle
   is not something this gateway (or any CAN traffic) triggers - it's
   the battery's own default/fallback behavior when powered on, full
   stop. The real mystery was never "why does the gateway's replay cause
   a disable" - it's "what does a real inverter do, continuously, that
   *suppresses* this default timer for as long as it's connected,"
   since a real SPH6000 session runs 14+ minutes with zero disables.
   Whatever that mechanism is, it must be doing something active and
   ongoing (not a one-off handshake), because the timer is evidently the
   battery's default behavior in the total absence of any inverter at
   all - and this gateway's byte-identical, correctly-timed `0x301`
   replay isn't providing it.

   **This gateway cannot do the same thing.** Directly tested the
   analogous case: got the battery locked again via this gateway's own
   0x301 replay (same mechanism), then - instead of swapping to the
   SPH6000 - physically disconnected and reconnected the T-2Can itself,
   mirroring exactly what worked for the real inverter. It did not
   revive the battery; confirmed still `chg_en=0 dis_en=0` well past
   the point where the SPH6000's equivalent reconnection had already
   worked (`captures/2026-09-03-gbli6532-t2can-revival-test.log`).
   **Caveat added on review, since resolved in part:** that same log
   shows zero individually-timestamped `0x301` frames anywhere in its
   visible ~975-1035s window - only a frozen stats-table row (count
   stuck at 59089, a placeholder payload `11 22 33 44 55 66 77 88`
   rather than the real `0B 16 21 2C 37 42 4D 58`) - meaning the
   gateway's own keepalive transmission may have silently died before or
   during this specific test.

   **`twai_transmit()` reliability directly verified with
   `VERBOSE_KEEPALIVE_TX`** (2026-09-03,
   `captures/2026-09-03-gbli6532-t2can-keepalive-tx-verify.log`, fresh
   reflash of `translator-t2can`). Two things confirmed: (1) with
   nothing on the bus at all (battery not yet physically connected),
   `twai_transmit()` genuinely does return `ESP_ERR_TIMEOUT` - 30 of the
   first ~40 calls failed this way - which is textbook CAN behavior for
   a lone transmitter with no one to ACK its frames, not a firmware bug.
   (2) The instant the battery was physically connected (t=51s), every
   subsequent call succeeded - **zero failures** from t=51s through the
   natural disable at t=649s (598s later, consistent with the
   established ~594-596s figure) and beyond. `bus_err` climbed once
   early (to 190881, likely connector noise) then froze, `tx_err`
   recovered to 0, `state=RUNNING` throughout - no bus-off, no silent
   stall. So: `twai_transmit()` is not fundamentally broken, and its
   reliability isn't what causes the natural 594s disable - that still
   happens on schedule even with a perfectly clean, 100%-successful
   keepalive. This does NOT yet clear the specific
   `t2can-revival-test.log` result, though - that test still needs its
   own clean rerun with this same debug build before its
   gateway-can't-revive conclusion can be fully trusted, since a locked
   (but still physically connected) battery's CAN transceiver should
   still ACK at the bus level regardless of its BMS enable state, and
   this new evidence doesn't explain why that log showed zero frames
   despite that.

   **Rerun completed on the verified-good build - same result, now
   trustworthy.** Same session as above: let the battery lock naturally
   (t=649s), then physically disconnected and reconnected the T-2Can
   itself (Paul's own prediction beforehand: "I don't think it will
   work as couldn't keep them alive" - correct). `0x311` byte7 stayed
   `0x00` (`chg_en=0 dis_en=0`) after reconnection, `0x323` byte4 read
   `0x04` (the known locked-state correlate), and the keepalive kept
   returning clean `ESP_OK` with no bus errors throughout. So this is
   now a clean, instrumented-and-verified negative result, not one
   clouded by an unconfirmed transmit path: **this gateway genuinely
   cannot revive an already-locked battery via reconnection, under
   conditions where transmission is proven 100% reliable** - while the
   real SPH6000 does it in ~1-2 seconds under the same physical
   reconnection event. Whatever the real inverter does differently is
   still not visible in `twai_transmit()` success/failure or in TWAI
   bus-health counters - it must be something about the frame content,
   sequence, or signal characteristics themselves.
   This sharpens the whole mystery meaningfully: it isn't only about
   *sustaining* trust over 594 s - this gateway apparently cannot even
   perform whatever the real inverter does to *re-establish* it, despite
   attempting the same kind of fresh-connection event. That points toward
   whatever's different being present from the very first frames of a
   connection, not something that only diverges after a long time.

   **Checked directly whether anything discrete happens at the 594 s
   mark inside a *successful* real-inverter session - it doesn't.**
   Computed the exact timestamp 594 s after the real SPH6000 session's
   own enable point (368.403s + 594s = 962.403s) in
   `captures/2026-08-31-gbli6532-sph6000-t2can-sniffer-594s-test.log`
   and checked both that exact instant and a 10s window around it:
   `chg_en` stays 1, `0x323` byte 4 stays `0x00`, normal frame mix, normal
   rates, nothing unusual at all. So whatever protects a real session
   isn't a discrete trigger that fires at that point and gets handled -
   it's something continuous, present from the start of the connection,
   that hasn't shown up in any byte this project has ever looked at.

   Also worth noting: the exact 594 s figure hasn't repeated as cleanly in
   the more recent multi-connect/disconnect test sessions (one measured
   ~146 s, another ~487-507 s depending on which reference point you
   start from) - but those sessions involved several connect/disconnect
   cycles in quick succession with real timing ambiguity around each one,
   unlike the three clean single-cycle tests that gave 594-596 s three
   times independently. Treat 594 s as the trustworthy number from clean
   conditions, and the shorter recent measurements as consistent with
   the same mechanism under messier conditions rather than a contradiction.

   **THE ACTUAL RESOLUTION (2026-09-03): `sendGrowattKeepalive()` in
   `main.cpp` had been sending the wrong `0x301` payload since the very
   first commit (`a90952f`, 2026-08-20).** It sent
   `0x11 0x22 0x33 0x44 0x55 0x66 0x77 0x88` - the generic example value
   from Growatt's protocol document - not the real payload captured off
   a genuine SPH6000 (`0B 16 21 2C 37 42 4D 58`, see item 3 above). This
   was a documentation/code mismatch that went unnoticed for the entire
   project: item 3 states the hardcoded payload "matches what's already
   hardcoded in `sendGrowattKeepalive()`" - a claim that was never
   actually checked byte-for-byte against the source, it was asserted
   and then trusted. Every single "gateway session" finding logged
   throughout this whole investigation - the 594 s timer, the failed
   revivals, the TWAI health checks, all of it - was generated while
   this gateway was silently sending the wrong bytes. The full byte-diff
   work earlier in this item that concluded "payload content doesn't
   matter" was, in hindsight, only ever comparing the *wrong* payload
   against itself before/after a transition - it never had the real
   payload in the picture at all to compare against.

   Found by finally comparing the two directly: while investigating
   whether `twai_transmit()` itself was silently failing (it wasn't,
   see the TWAI verification above), a check of the actual stats-table
   row for `0x301` in `captures/2026-09-03-gbli6532-t2can-revival-test.log`
   showed payload `11 22 33 44 55 66 77 88` - assumed at the time to be
   stale/frozen leftover data. Cross-checking that value directly
   against `main.cpp`'s source (not assumed, actually read) showed it
   wasn't stale at all - it's the literal value the firmware has always
   sent. `git log -S` confirmed it's been there since the first commit.

   Fixed: `main.cpp`'s `p[8]` array corrected to the real captured
   payload. Rebuilt, reflashed the T-2Can, battery power-cycled fresh
   (`captures/2026-09-03-gbli6532-t2can-correctpayload-test.log`),
   enabled normally at t=51s as always - but this time **stayed enabled
   for 2793+ seconds (46+ minutes) with zero disables**, nearly 5x past
   the old 594 s wall, `twai_transmit()` clean throughout (the only
   `ESP_ERR_TIMEOUT`s were pre-connection, same as every other test).
   Confirmed live with Paul watching the pack the whole time. This is
   the root cause: **the battery was never being fooled by a subtle
   protocol or timing difference - it was correctly, appropriately
   distrusting a keepalive that was never the right bytes in the first
   place.** Every other finding in this item (WAKE ruled out, TWAI
   transmit reliability, the standalone-timer behavior, the `0x323`
   byte 4 correlation) remains true and is kept above for the record,
   but none of them were the actual cause - this was.

   **Still worth doing as follow-up, not blocking:** run this corrected
   build through a full multi-hour/overnight soak and a real
   charge/discharge cycle to build further confidence beyond the 46-
   minute idle-session test; decide whether the `0x323` byte 4
   correlation (harmless, was tracking the old bug's symptom) is worth
   keeping in `sniffer.cpp`'s decode or can be dropped now that the
   real cause is known; update `SNIFFING.md`/`README.md` if either
   references the old payload or the "hard 594 s ceiling" limitation,
   which no longer applies.

`SNIFFING.md` is the procedure for answering the original four questions
from the existing GBLI ↔ SPH6000 link.

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

**Battery temperature doesn't appear anywhere in the Solis's own UI on
`PYLON_LV`** (2026-09-03, first live run) - there's no field for it at all,
not a blank/zero value. Checked our own encoding against the real Pylon
CANBUS Protocol V1.2 spec PDF directly (not from memory): `0x356` bytes 4-5,
0.1°C, 16-bit signed little-endian, is exactly what `translate.h` sends
(matches the ~25°C values seen throughout every capture), and there's no
separate min/max-temperature frame (e.g. `0x373`) defined in this spec that
Solis could be expecting instead - `0x356` is the only temperature field
Pylon's protocol has. So this looks like a Solis UI limitation for this
battery profile, not a translation bug - nothing to fix here unless new
evidence turns up.

**Confirmed real-world (2026-08-31, first live run):** the GBLI6532 itself
also fails toward disconnection independently of anything this gateway does.
Physically disconnecting the T-2Can mid-discharge (both CAN links dropped at
once) caused the battery to disable charge/discharge and open its own
relays within its normal ~1 s telemetry cadence - not waiting for any
documented timeout. It recovered cleanly within minutes of reconnection with
no lasting effect. See open question 7 below and the capture referenced
there. This is reassuring defense-in-depth: even if this gateway's own
derate/silence logic somehow failed, the battery's own BMS independently
protects itself the same way.

**RESOLVED (2026-09-03), previously an open operational limitation:** for
most of this project (from the very first commit until 2026-09-03), this
gateway's `0x301` keepalive sent the wrong payload - Growatt's protocol-doc
example value, not the real payload a genuine inverter sends - which caused
the battery to latch disabled ~594 s after every enable and never
self-recover without a manual POWER-button cycle. See open question 8 for
the full trail. Fixed by correcting the payload in `sendGrowattKeepalive()`;
confirmed staying enabled 46+ minutes with the fix in place, nearly 5x past
the old wall. Throughout, this was still "fail toward disconnection, not
toward uncontrolled current" even while the bug was live - so it was never
a safety issue, just an operational one, and it's now believed resolved.

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
