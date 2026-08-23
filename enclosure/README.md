# Enclosure

`t2can_enclosure.scad` — a parametric 3D-printable box for the LilyGo T-2Can
translator board (Phase 2 hardware). Two RJ45-to-RJ45 press-fit couplers
mount through one wall (battery bus on one, inverter bus on the other), with
a short internal patch cable from each to the board's own onboard RJ45
jacks. USB-C stays accessible on the opposite end for power/flashing.

## Status

Every real-world dimension is a named parameter at the top of the file,
split into two groups:

- **CONFIRMED / DESIGN CHOICES** — wall thickness, standoff sizes, screw
  sizes, clearances. Safe to build against.
- **PLACEHOLDER** — the T-2Can's own PCB dimensions and the RJ45 coupler's
  panel-mount cutout size. Neither was confirmable from official
  documentation or a real part in hand; the board dimensions come from one
  unverified online listing. **Measure the real board and the coupler with
  calipers once they arrive, update those lines, and everything else
  (walls, standoffs, bosses, vent slots) resizes automatically.**

## Usage

```bash
# open interactively (recommended first look)
openscad t2can_enclosure.scad

# export both halves for slicing
openscad -o base.stl -D 'part="base"' t2can_enclosure.scad
openscad -o lid.stl  -D 'part="lid"'  t2can_enclosure.scad
```

Assembly: base and lid join via 4 corner screw posts. The base's posts have
a blind pilot hole sized for a self-tapping M3 screw; the lid has oversized
clearance holes so the screw passes through freely and grips only in the
base.

## Parts referenced

- LilyGo T-2Can (non-FD) — see `PARTS.md`
- RJ45-to-RJ45 panel-mount coupler, 2-pack (one per CAN bus) —
  [PENGLIN RJ45 8P8C horizontal female adapter](https://www.amazon.co.uk/dp/B0GGBC8JXG)
