// =============================================================================
//  Host-side test for the Growatt -> Pylontech translation logic.
//    g++ -std=c++17 -I../src -o test_translate test_translate.cpp && ./test_translate
//  No hardware required. Verifies byte-level encoding, endianness flip,
//  flag remapping, clamping and the stale-link derate path.
// =============================================================================
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <initializer_list>
#include "translate.h"

static int failures = 0;

static void expectBytes(const char *what, const PylonFrame &f,
                        std::initializer_list<int> want) {
    size_t i = 0; bool ok = (f.len == want.size());
    if (ok) for (int w : want) { if (f.data[i++] != (uint8_t)w) { ok = false; break; } }
    printf("  %-34s id=0x%03X len=%u  [", what, f.id, f.len);
    for (uint8_t j = 0; j < f.len; j++) printf("%02X%s", f.data[j], j + 1 < f.len ? " " : "");
    printf("]  %s\n", ok ? "PASS" : "*** FAIL ***");
    if (!ok) {
        failures++;
        printf("      expected [");
        size_t k = 0; for (int w : want) printf("%02X%s", (uint8_t)w, ++k < want.size() ? " " : "");
        printf("]\n");
    }
}

static void expect(const char *what, bool cond) {
    printf("  %-34s %s\n", what, cond ? "PASS" : "*** FAIL ***");
    if (!cond) failures++;
}

static const PylonFrame &byId(const PylonBurst &b, uint32_t id) {
    for (int i = 0; i < 6; i++) if (b.f[i].id == id) return b.f[i];
    static PylonFrame dummy{}; return dummy;
}

int main() {
    printf("\n=== Growatt GBLI -> Solis PYLON_LV translation tests ===\n");

    // -------------------------------------------------------------------------
    printf("\n[1] Normal operation, mid-SOC, charging at 20.0 A\n");
    {
        BatteryState b;

        // 0x311 (BIG-endian): CVL 56.8 V = 568 = 0x0238
        //                     CCL 60.0 A = 600 = 0x0258
        //                     DCL 90.0 A = 900 = 0x0384
        //                     status: charging + chg/dis enabled = 0x0062
        uint8_t d311[8] = {0x02,0x38, 0x02,0x58, 0x03,0x84, 0x00,0x62};
        decodeGrowatt(b, 0x311, 8, d311, 1000);

        // 0x313 (BIG-endian): V 52.50 V @0.01V = 5250 = 0x1482
        //                     I +20.0 A       =  200 = 0x00C8
        //                     T  23.4 C       =  234 = 0x00EA
        //                     SOC 62 %, SOH byte 0x64 (=100)
        uint8_t d313[8] = {0x14,0x82, 0x00,0xC8, 0x00,0xEA, 0x3E, 0x64};
        decodeGrowatt(b, 0x313, 8, d313, 1000);

        // 0x312: no faults, 1 pack
        uint8_t d312[8] = {0x00,0x00,0x00,0x00, 0x01, 'G','W', 0x10};
        decodeGrowatt(b, 0x312, 8, d312, 1000);

        // 0x319: chemistry LiFePO4, discharge+charge enable = 0x60
        uint8_t d319[8] = {0x60,0,0,0,0,0,0,0};
        decodeGrowatt(b, 0x319, 8, d319, 1000);

        expect("0x313 voltage -> 5250 (0.01V)", b.pack_v_cV == 5250);
        expect("0x313 current -> +200 (0.1A)",  b.current_dA == 200);
        expect("SOC 62 / SOH 100",              b.soc_pct == 62 && b.soh_pct == 100);
        expect("charge+discharge enabled",      b.charge_en && b.discharge_en);

        PylonBurst p = buildPylon(b, false);
        // 0x351 LITTLE-endian: 568=0x0238 -> 38 02 ; 600=0x0258 -> 58 02 ; 900=0x0384 -> 84 03
        expectBytes("0x351 CVL/CCL/DCL",  byId(p, 0x351), {0x38,0x02, 0x58,0x02, 0x84,0x03, 0x00,0x00});
        expectBytes("0x355 SOC/SOH",      byId(p, 0x355), {0x3E,0x00, 0x64,0x00});
        expectBytes("0x356 V/I/T",        byId(p, 0x356), {0x82,0x14, 0xC8,0x00, 0xEA,0x00});
        expectBytes("0x359 no faults",    byId(p, 0x359), {0x00,0x00,0x00,0x00, 0x01, 0x50,0x4E});
        expectBytes("0x35C chg+dis en",   byId(p, 0x35C), {0xC0, 0x00});
        expectBytes("0x35E 'PYLON   '",   byId(p, 0x35E), {0x50,0x59,0x4C,0x4F,0x4E,0x20,0x20,0x20});
    }

    // -------------------------------------------------------------------------
    printf("\n[2] Discharging - current must stay NEGATIVE (two's complement LE)\n");
    {
        BatteryState b;
        uint8_t d311[8] = {0x02,0x38, 0x02,0x58, 0x03,0x84, 0x00,0x62};
        decodeGrowatt(b, 0x311, 8, d311, 1000);
        // I = -45.3 A -> -453 = 0xFE3B big-endian
        uint8_t d313[8] = {0x13,0x88, 0xFE,0x3B, 0x00,0xC8, 0x2D, 0x64};
        decodeGrowatt(b, 0x313, 8, d313, 1000);
        expect("0x313 current -> -453", b.current_dA == -453);
        PylonBurst p = buildPylon(b, false);
        // 50.00 V = 5000 = 0x1388 -> 88 13 ; -453 = 0xFE3B -> 3B FE ; 20.0C = 200 = 0x00C8 -> C8 00
        expectBytes("0x356 negative current", byId(p, 0x356), {0x88,0x13, 0x3B,0xFE, 0xC8,0x00});
    }

    // -------------------------------------------------------------------------
    printf("\n[3] 0x313 voltage - VS_0V01 is a straight pass-through\n");
    {
        // GROWATT_0x313_VOLT_SCALE is now pinned to VS_0V01, confirmed
        // against a real GBLI6532 capture (53.20-53.40 V through an
        // idle -> discharge -> charge cycle). The VS_AUTO/VS_0V1 branches in
        // normaliseVoltage() are compile-time alternatives for a different
        // pack and aren't exercised by this build.
        BatteryState b;
        uint8_t d313[8] = {0x14,0xC8, 0x00,0x00, 0x00,0xC8, 0x50, 0x64};   // 5320 = 53.20V
        decodeGrowatt(b, 0x313, 8, d313, 1000);
        expect("5320 (0.01V) passes through unchanged", b.pack_v_cV == 5320);
    }

    // -------------------------------------------------------------------------
    printf("\n[4] Protection flag remapping (Growatt 0x312 -> Pylon 0x359)\n");
    {
        BatteryState b;
        uint8_t d311[8] = {0x02,0x38, 0x02,0x58, 0x03,0x84, 0x00,0x62};
        decodeGrowatt(b, 0x311, 8, d311, 1000);
        uint8_t d313[8] = {0x14,0x82, 0x00,0x00, 0x00,0xC8, 0x3E, 0x64};
        decodeGrowatt(b, 0x313, 8, d313, 1000);
        // prot1 bit4 = cell over-voltage (0x10); prot2 bit6 = over-temp charge (0x40)
        // warn2 bit0 = internal comms fail (0x01)
        uint8_t d312[8] = {0x10, 0x40, 0x00, 0x01, 0x01, 'G','W', 0x10};
        decodeGrowatt(b, 0x312, 8, d312, 1000);
        PylonBurst p = buildPylon(b, false);
        // expect 0x359 byte0 = over-volt(0x08) | over-temp(0x20) = 0x28
        //        byte3 = internal comm fail (0x80)
        expectBytes("0x359 OV + OT + comm-fail", byId(p, 0x359), {0x28,0x00,0x00,0x80, 0x01, 0x50,0x4E});
    }

    // -------------------------------------------------------------------------
    printf("\n[5] Charge disabled by BMS (per 0x311) -> CCL must also be zeroed\n");
    {
        BatteryState b;
        // 0x311 status byte (d[7]) = 0x20: bit5 (discharge) set, bit6 (charge)
        // clear. 0x311 is authoritative for enable state - see test [9].
        uint8_t d311[8] = {0x02,0x38, 0x02,0x58, 0x03,0x84, 0x00,0x20};
        decodeGrowatt(b, 0x311, 8, d311, 1000);
        uint8_t d313[8] = {0x16,0x0C, 0x00,0x00, 0x00,0xC8, 0x64, 0x64};
        decodeGrowatt(b, 0x313, 8, d313, 1000);
        PylonBurst p = buildPylon(b, false);
        expectBytes("0x351 CCL forced to 0", byId(p, 0x351), {0x38,0x02, 0x00,0x00, 0x84,0x03, 0x00,0x00});
        expectBytes("0x35C discharge only",  byId(p, 0x35C), {0x40, 0x00});
    }

    // -------------------------------------------------------------------------
    printf("\n[6] Safety clamps on absurd BMS values\n");
    {
        BatteryState b;
        // CVL 99.9 V, CCL 300.0 A, DCL 300.0 A - all beyond the configured ceilings
        uint8_t d311[8] = {0x03,0xE7, 0x0B,0xB8, 0x0B,0xB8, 0x00,0x62};
        decodeGrowatt(b, 0x311, 8, d311, 1000);
        // pack voltage 99.99 V - outside the 40-60 V window Solis accepts
        uint8_t d313[8] = {0x27,0x0F, 0x00,0x00, 0x00,0xC8, 0x64, 0x64};
        decodeGrowatt(b, 0x313, 8, d313, 1000);
        PylonBurst p = buildPylon(b, false);
        // CVL clamped to 57.6 V = 576 = 0x0240 -> 40 02 ; limits to 105.0 A = 1050 = 0x041A -> 1A 04
        expectBytes("0x351 clamped", byId(p, 0x351), {0x40,0x02, 0x1A,0x04, 0x1A,0x04, 0x00,0x00});
        // voltage clamped to 60.00 V = 6000 = 0x1770 -> 70 17
        expect("0x356 voltage clamped to 60.00 V",
               byId(p, 0x356).data[0] == 0x70 && byId(p, 0x356).data[1] == 0x17);
    }

    // -------------------------------------------------------------------------
    printf("\n[7] Stale battery link -> derate\n");
    {
        BatteryState b;
        uint8_t d311[8] = {0x02,0x38, 0x02,0x58, 0x03,0x84, 0x00,0x62};
        decodeGrowatt(b, 0x311, 8, d311, 1000);
        uint8_t d313[8] = {0x14,0x82, 0x00,0xC8, 0x00,0xEA, 0x3E, 0x64};
        decodeGrowatt(b, 0x313, 8, d313, 1000);
        PylonBurst p = buildPylon(b, /*derate=*/true);
        expectBytes("0x35C all requests cleared", byId(p, 0x35C), {0x00, 0x00});
        expectBytes("0x351 both limits zeroed",   byId(p, 0x351), {0x38,0x02, 0x00,0x00, 0x00,0x00, 0x00,0x00});
        expectBytes("0x356 current forced to 0",  byId(p, 0x356), {0x82,0x14, 0x00,0x00, 0xEA,0x00});
        expect("0x359 system-error bit set", (byId(p, 0x359).data[1] & 0x80) != 0);
    }

    // -------------------------------------------------------------------------
    printf("\n[8] No 0x311 yet -> refuse all current\n");
    {
        BatteryState b;
        PylonBurst p = buildPylon(b, false);
        expectBytes("0x351 safe defaults", byId(p, 0x351), {0xE0,0x01, 0x00,0x00, 0x00,0x00, 0x00,0x00});
        expectBytes("0x35C nothing enabled", byId(p, 0x35C), {0x00, 0x00});
    }

    // -------------------------------------------------------------------------
    printf("\n[9] 0x319's stale enable bits must not override 0x311's live ones\n");
    printf("    (regression for a real capture: 0x319 read constant 0xC0 -\n");
    printf("    discharge-disabled - through an actual ~10 A discharge, while\n");
    printf("    0x311 correctly tracked it)\n");
    {
        BatteryState b;

        // 0x319 arrives first, before any 0x311: charge enabled, discharge
        // NOT enabled (byte0 = 0x40). Pre-0x311, this is a legitimate
        // fallback, so it should be believed for now.
        uint8_t d319[8] = {0x40,0,0,0,0,0,0,0};
        decodeGrowatt(b, 0x319, 8, d319, 1000);
        expect("pre-0x311 fallback: dis_en false", !b.discharge_en);
        expect("pre-0x311 fallback: chg_en true",  b.charge_en);

        // 0x311 arrives: both enabled (d[7] = 0x62, as in the real capture
        // during an active discharge). This must now win outright.
        uint8_t d311[8] = {0x02,0x38, 0x02,0x58, 0x03,0x84, 0x00,0x62};
        decodeGrowatt(b, 0x311, 8, d311, 1000);
        expect("0x311 sets dis_en true",  b.discharge_en);
        expect("0x311 sets chg_en true",  b.charge_en);

        // The next 0x319 (same stale byte0 = 0x40, exactly as observed for
        // the whole real capture) must NOT be allowed to clobber it back.
        decodeGrowatt(b, 0x319, 8, d319, 1000);
        expect("stale 0x319 does not override dis_en", b.discharge_en);
        expect("stale 0x319 does not override chg_en",  b.charge_en);

        uint8_t d313[8] = {0x14,0xFA, 0xFC,0x18, 0x00,0xEA, 0x63, 0x63};   // I = -100.0 A
        decodeGrowatt(b, 0x313, 8, d313, 1000);
        PylonBurst p = buildPylon(b, false);
        expect("DCL not zeroed by stale 0x319", byId(p, 0x351).data[4] != 0 || byId(p, 0x351).data[5] != 0);
    }

    printf("\n=== %s (%d failure%s) ===\n\n",
           failures ? "FAILURES" : "ALL TESTS PASSED",
           failures, failures == 1 ? "" : "s");
    return failures ? 1 : 0;
}
