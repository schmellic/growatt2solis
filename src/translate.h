// =============================================================================
//  translate.h  -  pure protocol logic, no hardware dependencies.
//  Growatt LV BMS CAN (big-endian) -> Pylontech LV CAN (little-endian)
//  Compiles on the host so it can be unit-tested (see test/test_translate.cpp).
// =============================================================================
#pragma once
#include <stdint.h>
#include <string.h>
#include "config.h"

// ---- Growatt CAN IDs (battery bus) -----------------------------------------
static const uint32_t GW_LIMITS    = 0x311;
static const uint32_t GW_FLAGS     = 0x312;
static const uint32_t GW_TELEMETRY = 0x313;
static const uint32_t GW_CAPACITY  = 0x314;
static const uint32_t GW_REQUEST   = 0x319;
static const uint32_t GW_MFR       = 0x320;
static const uint32_t GW_KEEPALIVE = 0x301;   // inverter -> BMS

// ---- Pylontech CAN IDs (inverter bus) --------------------------------------
static const uint32_t PY_351 = 0x351;
static const uint32_t PY_355 = 0x355;
static const uint32_t PY_356 = 0x356;
static const uint32_t PY_359 = 0x359;
static const uint32_t PY_35C = 0x35C;
static const uint32_t PY_35E = 0x35E;
static const uint32_t PY_305 = 0x305;   // inverter -> BMS

// -----------------------------------------------------------------------------
struct BatteryState {
    // 0x311
    uint16_t cvl_dV = 0, ccl_dA = 0, dcl_dA = 0, status_word = 0;
    bool have_311 = false;
    // 0x312
    uint8_t prot1 = 0, prot2 = 0, warn1 = 0, warn2 = 0;
    uint8_t pack_count = PYLON_PACK_COUNT;
    bool have_312 = false;
    // 0x313
    int16_t pack_v_cV = 0;      // normalised to 0.01 V
    int16_t current_dA = 0;     // 0.1 A, POSITIVE = charging
    int16_t temp_dC = 0;        // 0.1 degC
    uint8_t soc_pct = 0, soh_pct = 100;
    bool have_313 = false;
    // 0x314 (logging only)
    uint16_t rm_10mAh = 0, fcc_10mAh = 0, dv_mV = 0, cycles = 0;
    // 0x319
    bool charge_en = false, discharge_en = false;
    bool force_chg_1 = false, force_chg_2 = false;
    bool have_319 = false;

    uint32_t last_rx_ms = 0;    // last 0x311 or 0x313
};

struct PylonFrame { uint32_t id; uint8_t len; uint8_t data[8]; };
struct PylonBurst { PylonFrame f[6]; };   // 359, 351, 355, 356, 35C, 35E

// -----------------------------------------------------------------------------
//  byte helpers
// -----------------------------------------------------------------------------
static inline uint16_t be16u(const uint8_t *d) { return (uint16_t)(((uint16_t)d[0] << 8) | d[1]); }
static inline int16_t  be16s(const uint8_t *d) { return (int16_t)(((uint16_t)d[0] << 8) | d[1]); }
static inline void le16u(uint8_t *d, uint16_t v) { d[0] = (uint8_t)(v & 0xFF); d[1] = (uint8_t)(v >> 8); }
static inline void le16s(uint8_t *d, int16_t v)  { uint16_t u = (uint16_t)v; d[0] = (uint8_t)(u & 0xFF); d[1] = (uint8_t)(u >> 8); }

template <typename T>
static inline T clampv(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// -----------------------------------------------------------------------------
//  0x313 voltage scaling. Growatt's spec says 0.01 V; the growattArkCAN project
//  observed 0.1 V on an ARK pack. AUTO decides by magnitude, which is
//  unambiguous for a 48 V-class pack (0.01 V -> ~4000-6000, 0.1 V -> ~400-600).
// -----------------------------------------------------------------------------
static inline int16_t normaliseVoltage(int16_t raw) {
#if GROWATT_0x313_VOLT_SCALE == VS_0V01
    return raw;
#elif GROWATT_0x313_VOLT_SCALE == VS_0V1
    return (int16_t)(raw * 10);
#else
    return (raw > 2000) ? raw : (int16_t)(raw * 10);
#endif
}

// -----------------------------------------------------------------------------
//  Decode one Growatt frame into the state struct.
//  now_ms is used only to stamp freshness.
// -----------------------------------------------------------------------------
static inline void decodeGrowatt(BatteryState &b, uint32_t id, uint8_t len,
                                 const uint8_t *d, uint32_t now_ms) {
    switch (id) {

    case GW_LIMITS:                                    // 0x311
        if (len < 8) break;
        b.cvl_dV      = be16u(&d[0]);                  // 0.1 V
        b.ccl_dA      = be16u(&d[2]);                  // 0.1 A
        b.dcl_dA      = be16u(&d[4]);                  // 0.1 A
        b.status_word = be16u(&d[6]);
        b.have_311    = true;
        b.last_rx_ms  = now_ms;
        // Status bits live in the LOW byte of the big-endian pair (d[7]):
        // bit 5 = discharge enable, bit 6 = charge enable. This is the
        // authoritative source for enable state - a real capture spanning
        // idle -> discharge -> charge showed these bits track live BMS
        // state correctly, while 0x319's enable bits (below) stayed
        // constant throughout, including during an actual ~10 A discharge.
        b.discharge_en = (b.status_word & 0x0020) != 0;
        b.charge_en    = (b.status_word & 0x0040) != 0;
        break;

    case GW_FLAGS:                                     // 0x312
        if (len < 5) break;
        b.prot1 = d[0]; b.prot2 = d[1];
        b.warn1 = d[2]; b.warn2 = d[3];
        if (d[4] >= 1 && d[4] <= 16) b.pack_count = d[4];
        b.have_312 = true;
        break;

    case GW_TELEMETRY: {                               // 0x313
        if (len < 8) break;
        b.pack_v_cV  = normaliseVoltage(be16s(&d[0]));
        b.current_dA = be16s(&d[2]);                   // + = charging
        b.temp_dC    = be16s(&d[4]);
        b.soc_pct    = d[6] > 100 ? 100 : d[6];
        uint8_t soh  = (uint8_t)(d[7] & 0x7F);
        b.soh_pct    = (soh == 0 || soh > 100) ? 100 : soh;
        b.have_313   = true;
        b.last_rx_ms = now_ms;
        break;
    }

    case GW_CAPACITY:                                  // 0x314
        if (len < 8) break;
        b.rm_10mAh  = be16u(&d[0]);
        b.fcc_10mAh = be16u(&d[2]);
        b.dv_mV     = be16u(&d[4]);
        b.cycles    = be16u(&d[6]);
        break;

    case GW_REQUEST:                                   // 0x319
        if (len < 1) break;
        b.force_chg_2  = (d[0] & 0x04) != 0;           // bit 2
        b.force_chg_1  = (d[0] & 0x08) != 0;           // bit 3
#if ENABLE_0x319_FALLBACK
        // Only used before the first 0x311 arrives; 0x311 is authoritative
        // for enable state once seen (see GW_LIMITS above). A real capture
        // showed this frame's own enable bits (byte0 bits 5/6) don't track
        // live discharge/charge activity - they read constant throughout an
        // idle -> discharge -> charge cycle where 0x311's did.
        if (!b.have_311) {
            b.discharge_en = (d[0] & 0x20) != 0;       // bit 5
            b.charge_en    = (d[0] & 0x40) != 0;       // bit 6
        }
#endif
        b.have_319     = true;
        break;

    default: break;
    }
}

// -----------------------------------------------------------------------------
//  Build the six-frame Pylontech burst.
//  derate == true means the battery link is stale: hold the last known voltage
//  but forbid all current flow and raise the system-error flag.
// -----------------------------------------------------------------------------
static inline PylonBurst buildPylon(const BatteryState &b, bool derate) {
    PylonBurst out;
    memset(&out, 0, sizeof(out));

    uint8_t f359[8] = {0}, f351[8] = {0}, f355[8] = {0};
    uint8_t f356[8] = {0}, f35C[8] = {0}, f35E[8] = {0};

    const bool chg_en = b.charge_en    && !derate;
    const bool dis_en = b.discharge_en && !derate;

    // ---- 0x35C  request flags --------------------------------------------
    // b7 charge enable | b6 discharge enable | b5 force-charge I
    // b4 force-charge II | b3 full-charge request
    uint8_t req = 0;
    if (chg_en) req |= 0x80;
    if (dis_en) req |= 0x40;
    if (!derate && b.force_chg_1) req |= 0x20;
    if (!derate && b.force_chg_2) req |= 0x10;
    f35C[0] = req;
    f35C[1] = 0x00;

    // ---- 0x351  limits ----------------------------------------------------
    // Both current fields are POSITIVE MAGNITUDES: Solis reads them into
    // unsigned Modbus registers, so a negative would appear as ~6400 A.
    uint16_t cvl = clampv<uint16_t>(b.cvl_dV, (uint16_t)CVL_MIN_dV, (uint16_t)CVL_MAX_dV);
    uint16_t ccl = clampv<uint16_t>(b.ccl_dA, (uint16_t)0, (uint16_t)CCL_MAX_dA);
    uint16_t dcl = clampv<uint16_t>(b.dcl_dA, (uint16_t)0, (uint16_t)DCL_MAX_dA);
    if (!b.have_311) { cvl = (uint16_t)CVL_MIN_dV; ccl = 0; dcl = 0; }
    if (!chg_en) ccl = 0;          // belt and braces alongside 0x35C
    if (!dis_en) dcl = 0;
    le16u(&f351[0], cvl);
    le16u(&f351[2], ccl);
    le16u(&f351[4], dcl);
    // bytes 6-7 = 0 (discharge voltage limit; zero is proven safe on Solis)

    // ---- 0x355  SOC / SOH -------------------------------------------------
    le16u(&f355[0], b.soc_pct);
    le16u(&f355[2], b.soh_pct);

    // ---- 0x356  measurements ----------------------------------------------
    // V 0.01 V | I 0.1 A, POSITIVE = charging (same as Growatt, no sign flip)
    // T 0.1 degC
    int16_t v = clampv<int16_t>(b.pack_v_cV, (int16_t)PACK_V_MIN_cV, (int16_t)PACK_V_MAX_cV);
    le16s(&f356[0], v);
    le16s(&f356[2], derate ? (int16_t)0 : b.current_dA);
    le16s(&f356[4], b.temp_dC);

    // ---- 0x359  protection + alarm ----------------------------------------
    // byte0 prot : b3 over-V | b4 under-V | b5 over-T | b6 under-T | b7 dischg OC
    // byte1 prot : b6 chg OC | b7 system error
    // byte2 alarm: b3 high V | b4 low V | b5 high T | b6 low T | b7 dischg high I
    // byte3 alarm: b6 chg high I | b7 internal comm fail
    // byte4 pack count | byte5 'P' | byte6 'N'
    uint8_t p0 = 0, p1 = 0, a0 = 0, a1 = 0;

    if (b.prot1 & 0x14) p0 |= 0x08;   // module OV | cell OV
    if (b.prot1 & 0x0A) p0 |= 0x10;   // module UV | cell UV
    if (b.prot2 & 0xC0) p0 |= 0x20;   // over-temp charge | discharge
    if (b.prot2 & 0x30) p0 |= 0x40;   // under-temp charge | discharge
    if (b.prot1 & 0xA0) p0 |= 0x80;   // discharge OC | short-circuit discharge
    if (b.prot1 & 0x40) p1 |= 0x40;   // charge OC
    if (b.prot2 & 0x03) p1 |= 0x80;   // system error | delta-V fail
    if (b.prot1 & 0x01) p1 |= 0x80;   // soft-start fail -> system error

    if (b.warn1 & 0x0A) a0 |= 0x08;   // module OV | cell OV
    if (b.warn1 & 0x05) a0 |= 0x10;   // module UV | cell UV
    if (b.warn2 & 0xC0) a0 |= 0x20;   // over-temp
    if (b.warn2 & 0x30) a0 |= 0x40;   // under-temp
    if (b.warn1 & 0x80) a0 |= 0x80;   // discharge high current
    if (b.warn1 & 0x10) a1 |= 0x40;   // charge high current
    if (b.warn2 & 0x01) a1 |= 0x80;   // internal comm fail

    if (derate) p1 |= 0x80;           // BMS link lost -> system error

    f359[0] = p0; f359[1] = p1; f359[2] = a0; f359[3] = a1;
    f359[4] = b.pack_count;
    f359[5] = 0x50;   // 'P'
    f359[6] = 0x4E;   // 'N'

    // ---- 0x35E  manufacturer ----------------------------------------------
    memcpy(f35E, PYLON_MANUFACTURER, 8);

    // ---- assemble, in the send order used by known-good implementations ----
    struct { uint32_t id; uint8_t len; const uint8_t *src; } tbl[6] = {
        { PY_359, 7, f359 },
        { PY_351, 8, f351 },
        { PY_355, 4, f355 },
        { PY_356, 6, f356 },
        { PY_35C, 2, f35C },
        { PY_35E, 8, f35E },
    };
    for (int i = 0; i < 6; i++) {
        out.f[i].id  = tbl[i].id;
        out.f[i].len = tbl[i].len;
        memcpy(out.f[i].data, tbl[i].src, 8);
    }
    return out;
}
