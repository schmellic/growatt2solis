// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Paul <schmellic@gmail.com>
// =============================================================================
//  Growatt GBLI 6532  ->  Solis S6-EH1P8K-L-PLUS   CAN protocol translator
//  -------------------------------------------------------------------------
//  Battery side : Growatt low-voltage BMS CAN protocol
//                 500 kbit/s, 11-bit IDs, BIG-endian
//                 BMS->inv  0x311 0x312 0x313 0x314 0x315-0x318 0x319 0x320
//                 inv->BMS  0x301 (keepalive)
//
//  Inverter side: Pylontech low-voltage CAN (PYLON CANBUS V1.2)
//                 500 kbit/s, 11-bit IDs, LITTLE-endian
//                 BMS->inv  0x351 0x355 0x356 0x359 0x35C 0x35E  @ 1 Hz
//                 inv->BMS  0x305 (keepalive; liveness only)
//
//  Set the Solis battery profile to PYLON_LV.
//
//  Hardware: ESP32 + built-in TWAI (battery bus) + MCP2515 on VSPI (inverter)
//  Library : coryjfowler/MCP_CAN_lib
// =============================================================================

#include <Arduino.h>
#include <SPI.h>
#include <mcp_can.h>
#include "driver/twai.h"
#include "config.h"
#include "translate.h"

static BatteryState batt;
static uint32_t lastInverterAliveMs = 0;
static bool     invSeen = false;

#if VERBOSE_STATS
// -----------------------------------------------------------------------------
//  Per-ID frame rate table, identical format to sniffer.cpp's, added to
//  directly compare real battery-side traffic rates while this gateway is
//  driving the bus against the same table captured while a real Growatt
//  inverter drives it - see CLAUDE.md open question 8. Pure observation,
//  no effect on translation/safety behavior.
// -----------------------------------------------------------------------------
#define STATS_MAX_IDS       40
#define STATS_INTERVAL_MS   15000

struct IdStat {
    uint32_t id;
    uint32_t count;
    uint32_t firstMs;
    uint32_t lastMs;
    uint32_t minGap;
    uint32_t maxGap;
    bool     used;
};
static IdStat statsTable[STATS_MAX_IDS];
static uint32_t statsTotalFrames = 0;

static void statsRecord(uint32_t id, uint32_t now) {
    IdStat *s = nullptr;
    for (int i = 0; i < STATS_MAX_IDS; i++)
        if (statsTable[i].used && statsTable[i].id == id) { s = &statsTable[i]; break; }
    if (!s) {
        for (int i = 0; i < STATS_MAX_IDS; i++)
            if (!statsTable[i].used) {
                s = &statsTable[i];
                s->used = true;
                s->id = id;
                s->minGap = 0xFFFFFFFF;
                break;
            }
    }
    if (!s) return;
    if (s->count == 0) {
        s->firstMs = now;
    } else {
        uint32_t gap = now - s->lastMs;
        if (gap < s->minGap) s->minGap = gap;
        if (gap > s->maxGap) s->maxGap = gap;
    }
    s->lastMs = now;
    s->count++;
    statsTotalFrames++;
}

// -----------------------------------------------------------------------------
//  TWAI bus-health stats: error counters and controller state, not visible at
//  the decoded-frame level at all. Added after frame content, timing and rate
//  were all checked and ruled out as differences from a real Growatt inverter
//  - see CLAUDE.md open question 8. If this gateway's transceiver is doing
//  something subtly different electrically, this is where it would show up.
// -----------------------------------------------------------------------------
static const char *twaiStateName(twai_state_t s) {
    switch (s) {
    case TWAI_STATE_STOPPED:    return "STOPPED";
    case TWAI_STATE_RUNNING:    return "RUNNING";
    case TWAI_STATE_BUS_OFF:    return "BUS_OFF";
    case TWAI_STATE_RECOVERING: return "RECOVERING";
    default:                    return "?";
    }
}

static void twaiHealthPrint() {
    twai_status_info_t s;
    if (twai_get_status_info(&s) != ESP_OK) {
        Serial.println(F("  [twai_get_status_info failed]"));
        return;
    }
    Serial.printf(
        "  TWAI: state=%s  tx_err=%lu rx_err=%lu  tx_failed=%lu  "
        "rx_missed=%lu rx_overrun=%lu  arb_lost=%lu  bus_err=%lu\n",
        twaiStateName(s.state),
        (unsigned long)s.tx_error_counter, (unsigned long)s.rx_error_counter,
        (unsigned long)s.tx_failed_count,
        (unsigned long)s.rx_missed_count, (unsigned long)s.rx_overrun_count,
        (unsigned long)s.arb_lost_count, (unsigned long)s.bus_error_count);
}

static void statsPrint() {
    Serial.println();
    Serial.println(F("---- battery-bus rate stats (VERBOSE_STATS) ----------------------------"));
    Serial.printf ("  %lu frames, %lu s elapsed\n",
                   (unsigned long)statsTotalFrames, (unsigned long)(millis() / 1000));
    Serial.println(F("  ID     count   period(ms)  (min-max gap)"));
    for (int i = 0; i < STATS_MAX_IDS; i++) {
        if (!statsTable[i].used) continue;
        IdStat &s = statsTable[i];
        uint32_t mean = (s.count > 1) ? (s.lastMs - s.firstMs) / (s.count - 1) : 0;
        Serial.printf("  0x%03X  %6lu  %5lu (%lu-%lu)\n",
                      (unsigned)s.id, (unsigned long)s.count, (unsigned long)mean,
                      (unsigned long)(s.minGap == 0xFFFFFFFF ? 0 : s.minGap),
                      (unsigned long)s.maxGap);
    }
    Serial.println(F("--------------------------------------------------------------------------"));
    Serial.println();
}
#endif

MCP_CAN MCP(MCP_CS_PIN);

// -----------------------------------------------------------------------------
static void blink(uint8_t times) {
#if STATUS_LED_PIN >= 0
    for (uint8_t i = 0; i < times; i++) {
        digitalWrite(STATUS_LED_PIN, HIGH); delay(60);
        digitalWrite(STATUS_LED_PIN, LOW);  delay(120);
    }
#else
    (void)times;
#endif
}

static void dumpFrame(const char *tag, uint32_t id, uint8_t len, const uint8_t *d) {
    char line[96];
    int n = snprintf(line, sizeof(line), "%s 0x%03X [%u] ", tag, (unsigned)id, (unsigned)len);
    for (uint8_t i = 0; i < len && n < (int)sizeof(line) - 4; i++)
        n += snprintf(line + n, sizeof(line) - n, "%02X ", d[i]);
    Serial.println(line);
}

// =============================================================================
//  Battery bus  (ESP32 TWAI)
// =============================================================================
static bool twaiInit() {
    twai_general_config_t g = TWAI_GENERAL_CONFIG_DEFAULT(TWAI_TX_PIN, TWAI_RX_PIN, TWAI_MODE_NORMAL);
    g.rx_queue_len = 32;
    g.tx_queue_len = 16;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();
    if (twai_driver_install(&g, &t, &f) != ESP_OK) return false;
    return twai_start() == ESP_OK;
}

static void pollBatteryBus() {
    twai_message_t msg;
    while (twai_receive(&msg, 0) == ESP_OK) {
        if (msg.extd || msg.rtr) continue;      // Growatt LV is 11-bit data only
#if SNIFF_ONLY
        dumpFrame("BAT RX", msg.identifier, msg.data_length_code, msg.data);
#endif
#if VERBOSE_STATS
        statsRecord(msg.identifier, millis());
#endif
        decodeGrowatt(batt, msg.identifier, msg.data_length_code, msg.data, millis());
    }
}

static void sendGrowattKeepalive() {
#if GROWATT_0x301_ENABLE
    twai_message_t m;
    memset(&m, 0, sizeof(m));
    m.identifier = GW_KEEPALIVE;
    m.data_length_code = 8;
    // Real payload captured from a genuine Growatt SPH6000, static across
    // idle/discharge/charge/boot - see CLAUDE.md open question 3. This was
    // WRONGLY the protocol doc's example placeholder (0x11..0x88) from the
    // very first commit until 2026-09-03 - every prior gateway session in
    // this project sent that placeholder, never this real value.
    static const uint8_t p[8] = {0x0B, 0x16, 0x21, 0x2C, 0x37, 0x42, 0x4D, 0x58};
    memcpy(m.data, p, 8);
#if VERBOSE_KEEPALIVE_TX
    esp_err_t r = twai_transmit(&m, pdMS_TO_TICKS(10));
    Serial.printf("[keepalive] twai_transmit() -> %s\n", esp_err_to_name(r));
#else
    twai_transmit(&m, pdMS_TO_TICKS(10));
#endif
#endif
}

// =============================================================================
//  Inverter bus  (MCP2515)
// =============================================================================
static bool mcpInit() {
#ifdef MCP_RST_PIN
    // Some boards (e.g. LilyGo T-2Can) break out MCP2515 RST as a GPIO
    // instead of tying it on-board; it needs an active-low pulse to bring
    // the chip out of reset before SPI will get a response.
    pinMode(MCP_RST_PIN, OUTPUT);
    digitalWrite(MCP_RST_PIN, HIGH);
    delay(100);
    digitalWrite(MCP_RST_PIN, LOW);
    delay(100);
    digitalWrite(MCP_RST_PIN, HIGH);
    delay(100);
#endif
    SPI.begin(MCP_SCK_PIN, MCP_MISO_PIN, MCP_MOSI_PIN, MCP_CS_PIN);
    if (MCP.begin(MCP_ANY, CAN_500KBPS, MCP_CRYSTAL) != CAN_OK) return false;
    MCP.setMode(MCP_NORMAL);
    pinMode(MCP_INT_PIN, INPUT);
    return true;
}

static void pollInverterBus() {
    while (MCP.checkReceive() == CAN_MSGAVAIL) {
        unsigned long rxId = 0; uint8_t len = 0; uint8_t buf[8];
        MCP.readMsgBuf(&rxId, &len, buf);
        uint32_t id = (uint32_t)(rxId & 0x1FFFFFFFUL);
        if (id == PY_305) { lastInverterAliveMs = millis(); invSeen = true; }
#if SNIFF_ONLY
        dumpFrame("INV RX", id, len, buf);
#endif
    }
}

static void sendPylonBurst(bool derate) {
    PylonBurst burst = buildPylon(batt, derate);
    for (int i = 0; i < 6; i++) {
        MCP.sendMsgBuf(burst.f[i].id, 0, burst.f[i].len, burst.f[i].data);
#if VERBOSE_TX
        dumpFrame("INV TX", burst.f[i].id, burst.f[i].len, burst.f[i].data);
#endif
        if (i < 5) delay(PYLON_TX_GAP_MS);
    }
}

// =============================================================================
static void printSummary(bool derate, bool silent) {
    char line[240];
    snprintf(line, sizeof(line),
        "[%lus] %s%s V=%.2fV I=%+.1fA T=%.1fC SOC=%u%% SOH=%u%% | "
        "CVL=%.1fV CCL=%.1fA DCL=%.1fA | chg=%d dis=%d fc1=%d fc2=%d | "
        "prot=%02X%02X warn=%02X%02X | packs=%u | inv0x305=%s",
        (unsigned long)(millis() / 1000),
        silent ? "SILENT " : (derate ? "DERATE " : ""),
        (batt.have_311 && batt.have_313) ? "OK" : "WAIT",
        batt.pack_v_cV / 100.0f,
        batt.current_dA / 10.0f,
        batt.temp_dC / 10.0f,
        (unsigned)batt.soc_pct, (unsigned)batt.soh_pct,
        batt.cvl_dV / 10.0f, batt.ccl_dA / 10.0f, batt.dcl_dA / 10.0f,
        (int)batt.charge_en, (int)batt.discharge_en,
        (int)batt.force_chg_1, (int)batt.force_chg_2,
        batt.prot1, batt.prot2, batt.warn1, batt.warn2,
        (unsigned)batt.pack_count,
        invSeen ? "yes" : "no");
    Serial.println(line);
}

// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(300);
    Serial.println();
    Serial.println(F("=== Growatt GBLI 6532 -> Solis PYLON_LV translator ==="));
#if SNIFF_ONLY
    Serial.println(F("MODE: SNIFF ONLY - nothing is sent to the inverter."));
#else
    Serial.println(F("MODE: ACTIVE - translating to the inverter."));
#endif

#if STATUS_LED_PIN >= 0
    pinMode(STATUS_LED_PIN, OUTPUT);
    digitalWrite(STATUS_LED_PIN, LOW);
#endif

    if (!twaiInit()) {
        Serial.println(F("FATAL: TWAI (battery bus) init failed."));
        for (;;) { blink(2); delay(800); }
    }
    Serial.println(F("Battery bus (TWAI) up at 500 kbit/s."));

    if (!mcpInit()) {
        Serial.println(F("FATAL: MCP2515 init failed - check CS pin, wiring and crystal (8 vs 16 MHz)."));
        for (;;) { blink(3); delay(800); }
    }
    Serial.println(F("Inverter bus (MCP2515) up at 500 kbit/s."));
    blink(1);
}

void loop() {
    static uint32_t lastPylonTx = 0, lastKeepalive = 0, lastPrint = 0;
#if VERBOSE_STATS
    static uint32_t lastStats = 0;
#endif

    pollBatteryBus();
    pollInverterBus();

    const uint32_t now = millis();

    if (now - lastKeepalive >= GROWATT_0x301_INTERVAL) {
        lastKeepalive = now;
        sendGrowattKeepalive();
    }

    const uint32_t age = (batt.last_rx_ms == 0) ? now : (now - batt.last_rx_ms);
    const bool derate = (batt.last_rx_ms == 0) || (age > BATT_STALE_DERATE_MS);
    const bool silent = (batt.last_rx_ms == 0) || (age > BATT_STALE_SILENCE_MS);

    if (now - lastPylonTx >= PYLON_TX_INTERVAL_MS) {
        lastPylonTx = now;
#if !SNIFF_ONLY
        if (!silent) sendPylonBurst(derate);
#endif
#if STATUS_LED_PIN >= 0
        digitalWrite(STATUS_LED_PIN, (!silent && !derate) ? HIGH : LOW);
#endif
    }

#if VERBOSE_DECODE
    if (now - lastPrint >= 2000) {
        lastPrint = now;
        printSummary(derate, silent);
    }
#endif

#if VERBOSE_STATS
    if (now - lastStats >= STATS_INTERVAL_MS) {
        lastStats = now;
        statsPrint();
        twaiHealthPrint();
    }
#endif
}
