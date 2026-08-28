# Enclosure

`BMS_Translator_Case.stl` / `BMS_Translator_Lid.stl` — the 3D-printable
enclosure for the LilyGo T-2Can translator board (Phase 2 hardware),
designed against the real board and the real RJ45-to-RJ45 panel-mount
couplers rather than placeholder dimensions.

Designed externally (Fusion 360), not from a parametric source tracked in
this repo — these STLs are the source of truth. There's no `.scad`/`.f3d`
file here to regenerate them from; if the design needs to change, it's
edited at the source and the STLs re-exported and dropped in here.

## Parts referenced

- LilyGo T-2Can (non-FD) — see `PARTS.md`. Confirmed: CAN connections are
  screw terminals, not onboard RJ45 jacks - each RJ45 coupler wires directly
  to its screw terminal block.
- RJ45-to-RJ45 panel-mount coupler, 2-pack (one per CAN bus) —
  [PENGLIN RJ45 8P8C horizontal female adapter](https://www.amazon.co.uk/dp/B0GGBC8JXG)

## History

An earlier parametric OpenSCAD design (`t2can_enclosure.scad`) lived here
first, built from placeholder/unverified board and coupler dimensions before
either part was in hand. Superseded once the real hardware arrived and a
proper design could be done against it - removed rather than kept as a
stale fallback.
