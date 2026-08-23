// =============================================================================
//  t2can_enclosure.scad  -  3D-printable enclosure for the LilyGo T-2Can
//  translator board (Phase 2 hardware, growatt2solis project).
//
//  STATUS: every dimension below is a named parameter. The ones marked
//  CONFIRMED come from a real part or measurement. The ones marked
//  PLACEHOLDER are estimates - neither the T-2Can's exact PCB dimensions nor
//  the RJ45 coupler's exact cutout size were confirmable without the
//  physical parts in hand. Update the PLACEHOLDER block once you can
//  measure the real board and coupler with calipers; everything else
//  (walls, standoffs, screw bosses, vent slots) derives from these values
//  and will resize automatically.
//
//  Two RJ45-to-RJ45 press-fit couplers (the Amazon PENGLIN 2-pack) mount
//  through one long wall - battery bus on one, inverter bus on the other -
//  with a short internal patch cable from each to the board's own onboard
//  RJ45 jacks. A cutout on the opposite end gives USB-C access for
//  power/flashing.
//
//  Usage:
//    openscad -o base.stl -D 'part="base"' t2can_enclosure.scad
//    openscad -o lid.stl  -D 'part="lid"'  t2can_enclosure.scad
//  or just open this file in the OpenSCAD GUI - it previews both at once,
//  laid out side by side ready to slice.
// =============================================================================

$fn = 48;   // circle smoothness for previews; bump up before final export

// -----------------------------------------------------------------------------
// PLACEHOLDER - confirm against real hardware, then update these three lines.
// -----------------------------------------------------------------------------
board_length = 91;   // mm, long edge (source: one unverified online listing)
board_width  = 39;   // mm, short edge (source: same, unverified)
board_height = 18;   // mm, tallest point incl. RJ45 jacks/USB-C connector on the board

// PLACEHOLDER - the Amazon RJ45-to-RJ45 coupler's actual panel-mount cutout.
// Typical press-fit RJ45 couplers of this style use a rectangular snap-in
// cutout around this size; verify against the real part before printing.
rj45_cutout_w = 16.0;   // mm
rj45_cutout_h = 13.5;   // mm

// -----------------------------------------------------------------------------
// CONFIRMED / DESIGN CHOICES - safe to rely on, these are just build parameters.
// -----------------------------------------------------------------------------
wall            = 2.2;    // mm, printable in one 0.4mm-nozzle wall pass or two
board_margin_xy = 4;      // mm clearance around the board footprint, each side
standoff_h      = 6;      // mm, lifts the board off the floor for component clearance
standoff_d      = 6;      // mm, standoff post diameter
lid_clearance   = 20;     // mm, interior height above the board top for wiring/patch cables

screw_boss_d      = 7;    // mm, corner boss diameter (base + lid)
screw_hole_d      = 2.8;  // mm, BLIND pilot hole in the base post (self-tapping M3 bites in)
screw_clearance_d = 3.4;  // mm, THROUGH hole in the lid (M3 passes freely, grips only in the base)
corner_inset      = 6;    // mm, boss centre inset from each exterior corner

usb_cutout_w    = 10;     // mm, USB-C cable clearance cutout on the end wall
usb_cutout_h    = 6;      // mm
usb_cutout_z    = 10;     // mm, height of cutout centre above the floor

vent_slot_w     = 2;      // mm
vent_slot_len   = 14;     // mm
vent_slot_gap   = 4;      // mm between slots
vent_slot_count = 4;      // slots in the single row (not literal "rows")

// -----------------------------------------------------------------------------
// Derived dimensions - don't edit these directly, they follow from the above.
// -----------------------------------------------------------------------------
inner_l = board_length + 2 * board_margin_xy;
inner_w = board_width  + 2 * board_margin_xy;
inner_h = standoff_h + board_height + lid_clearance;

outer_l = inner_l + 2 * wall;
outer_w = inner_w + 2 * wall;
base_h  = inner_h * 0.4 + wall;   // base holds the board + a bit of headroom
lid_h   = inner_h - (base_h - wall) + wall;

// -----------------------------------------------------------------------------
//  Rounded rectangle helper (2D), extruded where needed.
// -----------------------------------------------------------------------------
module rounded_rect(l, w, r) {
    hull() {
        for (x = [r, l - r])
            for (y = [r, w - r])
                translate([x, y]) circle(r = r);
    }
}

// -----------------------------------------------------------------------------
//  BASE: floor + walls + board standoffs + RJ45 coupler cutouts + USB-C cutout
//  + wall-mount screw tabs.
// -----------------------------------------------------------------------------
module base() {
    difference() {
        union() {
            // outer shell
            linear_extrude(base_h)
                rounded_rect(outer_l, outer_w, 3);

            // corner screw bosses - solid posts flush with the rim, with a
            // BLIND pilot hole drilled down from the top (see below), so a
            // self-tapping screw driven down through the lid's clearance
            // hole has something to actually bite into.
            for (pos = corner_positions())
                translate(pos) cylinder(d = screw_boss_d, h = base_h);

            // board standoffs, inset from the board footprint corners
            for (pos = board_corner_positions())
                translate([pos[0], pos[1], wall])
                    cylinder(d = standoff_d, h = standoff_h);
        }

        // hollow out the interior
        translate([wall, wall, wall])
            linear_extrude(base_h)
                rounded_rect(outer_l - 2 * wall, outer_w - 2 * wall, 2);

        // corner screw pilot holes - BLIND, drilled down from the top of
        // each post, leaving ~2mm of solid material at the bottom so they
        // don't punch through to the outside underside of the base.
        for (pos = corner_positions())
            translate([pos[0], pos[1], 2])
                cylinder(d = screw_hole_d, h = base_h - 2 + 0.5);

        // two RJ45 coupler cutouts on one long wall (y = 0 face), spaced
        // across the board's long axis
        translate([outer_l * 0.30, -1, wall + 3])
            cube([rj45_cutout_w, wall + 2, rj45_cutout_h]);
        translate([outer_l * 0.65, -1, wall + 3])
            cube([rj45_cutout_w, wall + 2, rj45_cutout_h]);

        // USB-C cutout on the short end wall (x = 0 face)
        translate([-1, outer_w / 2 - usb_cutout_w / 2, wall + usb_cutout_z])
            cube([wall + 2, usb_cutout_w, usb_cutout_h]);
    }

    // wall-mount tabs, one on each short end, screw hole for a wood/self-tap screw
    for (mirror_x = [0, 1])
        mirror([mirror_x, 0, 0])
            translate([mirror_x ? 0 : outer_l, outer_w / 2, 0])
                mount_tab();
}

module mount_tab() {
    difference() {
        translate([-4, -9, 0]) cube([8, 18, wall]);
        translate([-4, 0, -1]) cylinder(d = 4.2, h = wall + 2);
    }
}

// -----------------------------------------------------------------------------
//  LID: shell + ventilation slots, sized to press over the base's bosses.
// -----------------------------------------------------------------------------
module lid() {
    difference() {
        union() {
            linear_extrude(lid_h)
                rounded_rect(outer_l, outer_w, 3);

            for (pos = corner_positions())
                translate(pos) cylinder(d = screw_boss_d + 2, h = 3);
        }

        translate([wall, wall, -1])
            linear_extrude(lid_h - wall + 1)
                rounded_rect(outer_l - 2 * wall, outer_w - 2 * wall, 2);

        // through clearance holes - M3 passes freely, grips only in the base
        for (pos = corner_positions())
            translate([pos[0], pos[1], -1])
                cylinder(d = screw_clearance_d, h = lid_h + 5);

        // ventilation slots
        for (row = [0 : vent_slot_count - 1])
            translate([outer_l * 0.5 - (vent_slot_count * (vent_slot_len + vent_slot_gap)) / 2
                       + row * (vent_slot_len + vent_slot_gap),
                       outer_w * 0.5 - vent_slot_w / 2, lid_h - wall - 0.5])
                cube([vent_slot_len, vent_slot_w, wall + 1]);
    }
}

// -----------------------------------------------------------------------------
//  Corner helpers
// -----------------------------------------------------------------------------
function corner_positions() = [
    [corner_inset, corner_inset],
    [outer_l - corner_inset, corner_inset],
    [corner_inset, outer_w - corner_inset],
    [outer_l - corner_inset, outer_w - corner_inset],
];

function board_corner_positions() = [
    [wall + board_margin_xy, wall + board_margin_xy],
    [wall + board_margin_xy + board_length, wall + board_margin_xy],
    [wall + board_margin_xy, wall + board_margin_xy + board_width],
    [wall + board_margin_xy + board_length, wall + board_margin_xy + board_width],
];

// -----------------------------------------------------------------------------
//  Render selection
// -----------------------------------------------------------------------------
part = "preview";   // override with -D 'part="base"' or -D 'part="lid"'

if (part == "base") base();
else if (part == "lid") lid();
else {
    base();
    translate([0, outer_w + 15, 0]) lid();
}
