// =============================================================================
//  sniffer.cpp  -  LISTEN-ONLY CAN sniffer for the Growatt battery bus
//  -------------------------------------------------------------------------
//  Purpose: tap the LIVE link between the GBLI 6532 and a Growatt SPH6000 and
//  record exactly what both ends say, WITHOUT disturbing the working system.
//
//  The TWAI controller runs in TWAI_MODE_LISTEN_ONLY: it never sends an ACK
//  bit, never sends an error frame, and never transmits. Electrically the node
//  is a passive stub. This is the only safe way to instrument a bus that is
//  currently keeping a real battery and inverter talking.
//
//  Build:  pio run -e sniffer -t upload && pio device monitor
//
//  Serial commands (type the letter + Enter):
//    r  toggle raw frame logging
//    s  print the statistics table now
//    c  clear all statistics
//    d  toggle Growatt decode lines
// =============================================================================

#include <Arduino.h>
#include "driver/twai.h"
#include "config.h"
#include "translate.h"

#define MAX_IDS         40
#define STATS_INTERVAL  15000

struct IdStat {
    uint32_t id;
    uint32_t count;
    uint32_t firstMs;
    uint32_t lastMs;
    uint32_t minGap;
    uint32_t maxGap;
    uint8_t  dlc;
    uint8_t  last[8];
    uint8_t  changed;      // bitmask: which byte positions have ever changed
    bool     used;
};

static IdStat stats[MAX_IDS];
static uint32_t totalFrames = 0;
static bool logRaw    = true;
static bool logDecode = true;

static BatteryState batt;

// =============================================================================
//  WAKE pin monitor - DevKit diagnostic build only.
//  GPIO16 - PCS-WAKE+ tapped through a PC817 optocoupler (active-low output:
//  HIGH = WAKE+ absent/idle, LOW = WAKE+ energized). Added 2026-09-01 to test
//  whether a real Growatt inverter pulses WAKE rather than holding it at a
//  sustained level - a multimeter's steady-state reading can't catch a brief
//  pulse, this can. See CLAUDE.md open question 8. Not part of this board's
//  normal pin set - only meaningful with the optocoupler circuit wired up.
// =============================================================================
#if BOARD == BOARD_ESP32_DEVKIT
#define WAKE_MONITOR_PIN 16
#define WAKE_LOG_SIZE    128

struct WakeEvent { uint32_t ms; uint8_t level; };
static volatile WakeEvent wakeLog[WAKE_LOG_SIZE];
static volatile uint16_t  wakeHead = 0;
static uint16_t           wakePrinted = 0;
static volatile uint32_t  wakeEdgeCount = 0;

void IRAM_ATTR wakeISR() {
    uint16_t h = wakeHead;
    wakeLog[h].ms = millis();
    wakeLog[h].level = (uint8_t)digitalRead(WAKE_MONITOR_PIN);
    wakeHead = (uint16_t)((h + 1) % WAKE_LOG_SIZE);
    wakeEdgeCount++;
}

static void wakeMonitorSetup() {
    // Plain INPUT, not INPUT_PULLUP: the optocoupler's own onboard pull-up
    // turned out far too weak to register the real signal (measured
    // 2026-09-03 - a 1M ohm external pull-up was needed to get a usable
    // swing). The ESP32's internal pull-up (~45k ohm) would sit in
    // parallel with that external resistor and drag the effective value
    // straight back down, undoing the fix - so this pin must rely
    // entirely on the external 1M ohm resistor now, no internal pull.
    pinMode(WAKE_MONITOR_PIN, INPUT);
    attachInterrupt(digitalPinToInterrupt(WAKE_MONITOR_PIN), wakeISR, CHANGE);
    Serial.printf("WAKE monitor armed on GPIO%d (active-low: LOW = WAKE+ energized)\n",
                  WAKE_MONITOR_PIN);
    Serial.printf("Idle level right now: %s\n",
                  digitalRead(WAKE_MONITOR_PIN) ? "HIGH (absent)" : "LOW (energized)");
}

static void wakeMonitorPoll() {
    while (wakePrinted != wakeHead) {
        uint32_t ms    = wakeLog[wakePrinted].ms;
        uint8_t  level = wakeLog[wakePrinted].level;
        Serial.printf("[%lu.%03lus] WAKE edge #%lu -> %s\n",
                      (unsigned long)(ms / 1000), (unsigned long)(ms % 1000),
                      (unsigned long)wakePrinted,
                      level ? "HIGH (idle/absent)" : "LOW (energized)");
        wakePrinted = (uint16_t)((wakePrinted + 1) % WAKE_LOG_SIZE);
    }
}
#endif

// -----------------------------------------------------------------------------
static IdStat *findOrAdd(uint32_t id) {
    for (int i = 0; i < MAX_IDS; i++)
        if (stats[i].used && stats[i].id == id) return &stats[i];
    for (int i = 0; i < MAX_IDS; i++)
        if (!stats[i].used) {
            stats[i].used = true;
            stats[i].id = id;
            stats[i].minGap = 0xFFFFFFFF;
            return &stats[i];
        }
    return nullptr;
}

static void record(uint32_t id, uint8_t dlc, const uint8_t *d, uint32_t now) {
    IdStat *s = findOrAdd(id);
    if (!s) return;
    if (s->count == 0) {
        s->firstMs = now;
    } else {
        uint32_t gap = now - s->lastMs;
        if (gap < s->minGap) s->minGap = gap;
        if (gap > s->maxGap) s->maxGap = gap;
        for (uint8_t i = 0; i < dlc && i < 8; i++)
            if (s->last[i] != d[i]) s->changed |= (uint8_t)(1u << i);
    }
    s->lastMs = now;
    s->dlc = dlc;
    memcpy(s->last, d, dlc > 8 ? 8 : dlc);
    s->count++;
    totalFrames++;
}

// -----------------------------------------------------------------------------
static void printRaw(uint32_t now, uint32_t id, uint8_t dlc, const uint8_t *d) {
    char line[110];
    int n = snprintf(line, sizeof(line), "%8lu.%03lu  0x%03X  [%u]  ",
                     (unsigned long)(now / 1000), (unsigned long)(now % 1000),
                     (unsigned)id, (unsigned)dlc);
    for (uint8_t i = 0; i < dlc && n < (int)sizeof(line) - 4; i++)
        n += snprintf(line + n, sizeof(line) - n, "%02X ", d[i]);
    Serial.println(line);
}

// Human-readable decode of the frames we actually care about.
static void printDecode(uint32_t id, uint8_t dlc, const uint8_t *d) {
    char l[160];
    switch (id) {
    case 0x311:
        if (dlc < 8) return;
        snprintf(l, sizeof(l),
            "        0x311  CVL=%.1fV  CCL=%.1fA  DCL=%.1fA  status=0x%04X "
            "(chg_en=%d dis_en=%d mode=%u)",
            be16u(&d[0]) / 10.0f, be16u(&d[2]) / 10.0f, be16u(&d[4]) / 10.0f,
            be16u(&d[6]),
            (d[7] & 0x40) ? 1 : 0, (d[7] & 0x20) ? 1 : 0, (unsigned)(d[7] & 0x03));
        Serial.println(l);
        break;

    case 0x312: {
        if (dlc < 8) return;
        snprintf(l, sizeof(l),
            "        0x312  prot=%02X %02X  warn=%02X %02X  packs=%u  mfr=%c%c  cells=%u",
            d[0], d[1], d[2], d[3], (unsigned)d[4],
            isprint(d[5]) ? d[5] : '?', isprint(d[6]) ? d[6] : '?', (unsigned)d[7]);
        Serial.println(l);
        break;
    }

    case 0x313: {
        if (dlc < 8) return;
        int16_t raw = be16s(&d[0]);
        snprintf(l, sizeof(l),
            "        0x313  Vraw=%d  -> %.2fV if 0.01V  /  %.1fV if 0.1V  |  "
            "I=%+.1fA  T=%.1fC  SOC=%u%%  SOH=%u",
            raw, raw / 100.0f, raw / 10.0f,
            be16s(&d[2]) / 10.0f, be16s(&d[4]) / 10.0f,
            (unsigned)d[6], (unsigned)(d[7] & 0x7F));
        Serial.println(l);
        break;
    }

    case 0x314:
        if (dlc < 8) return;
        snprintf(l, sizeof(l),
            "        0x314  RM=%.2fAh  FCC=%.2fAh  dV=%umV  cycles=%u",
            be16u(&d[0]) / 100.0f, be16u(&d[2]) / 100.0f,
            (unsigned)be16u(&d[4]), (unsigned)be16u(&d[6]));
        Serial.println(l);
        break;

    case 0x319:
        if (dlc < 1) return;
        snprintf(l, sizeof(l),
            "        0x319  chem=%u  forceI=%d forceII=%d  dis_en=%d chg_en=%d  (byte0=0x%02X)",
            (unsigned)(d[0] & 0x03),
            (d[0] & 0x08) ? 1 : 0, (d[0] & 0x04) ? 1 : 0,
            (d[0] & 0x20) ? 1 : 0, (d[0] & 0x40) ? 1 : 0, d[0]);
        Serial.println(l);
        break;

    case 0x320:
        if (dlc < 4) return;
        snprintf(l, sizeof(l), "        0x320  mfr=%c%c  hw=%u  sw=%u",
            isprint(d[0]) ? d[0] : '?', isprint(d[1]) ? d[1] : '?',
            (unsigned)d[2], (unsigned)d[3]);
        Serial.println(l);
        break;

    case 0x301:
        Serial.println(F("        0x301  <-- KEEPALIVE FROM THE GROWATT INVERTER."));
        Serial.println(F("               This payload is undocumented. Write it down."));
        break;

    // ---- Not in Growatt's published spec. Seen on a real GBLI6532 but not
    // yet confirmed - decoded here as best-effort guesses, clearly labelled,
    // so more captures can confirm or correct them. Not used anywhere in
    // translate.h or the Pylon output.
    case 0x323: {
        // Payload is 20 0E 10 00 XX 00 00 00 - only byte 4 has ever been
        // seen to change. Confirmed 2026-09-03, independently in two
        // separate sessions: byte 4 reads 0x04 tightly clustered around
        // the mystery 594s disable (see CLAUDE.md open question 8) and
        // 0x00 otherwise - the first CAN-visible signal found so far that
        // actually correlates with that state, not yet understood, not
        // yet consistent on whether it leads or follows chg_en.
        if (dlc < 5) return;
        snprintf(l, sizeof(l),
            "        0x323  byte4=0x%02X  (GUESS: correlates with the 594s disable - see CLAUDE.md)",
            (unsigned)d[4]);
        Serial.println(l);
        break;
    }

    case 0x324: {
        // Looks like a paged ASCII string (serial/model number): byte0
        // cycles 0,1,2 across frames, the rest renders as printable ASCII
        // ("GPJ021", "9522062", "BB7"/"C07" seen on a real 2-pack GBLI6532,
        // the last chunk differing between the two packs).
        if (dlc < 1) return;
        char ascii[8] = {0};
        for (uint8_t i = 1; i < dlc && i < 8; i++)
            ascii[i - 1] = isprint(d[i]) ? (char)d[i] : '.';
        snprintf(l, sizeof(l),
            "        0x324  page=%u  ascii=\"%s\"  (GUESS: paged serial/model string)",
            (unsigned)d[0], ascii);
        Serial.println(l);
        break;
    }

    case 0x329: {
        // byte0 cycles 1/2, matching this pack's 2-unit parallel group
        // exactly - looks like per-pack telemetry, but the units/meaning of
        // the two 16-bit values aren't confirmed.
        if (dlc < 8) return;
        snprintf(l, sizeof(l),
            "        0x329  pack=%u  valA=%u  valB=%u  (GUESS: per-pack values, units unknown)",
            (unsigned)d[0], (unsigned)be16u(&d[2]), (unsigned)be16u(&d[6]));
        Serial.println(l);
        break;
    }

    case 0x330: {
        // By far the highest-frequency ID seen (10k+ frames in one
        // capture). Last 4 bytes read as two close-together 16-bit values
        // in the ~3300-3400 range - looks like individual cell-voltage
        // pairs in mV, similar to 0x315-0x318, cycling through cells.
        // byte1/byte3 look like index counters.
        if (dlc < 8) return;
        snprintf(l, sizeof(l),
            "        0x330  idx=%u,%u  cellA=%umV  cellB=%umV  (GUESS: cell voltage pair)",
            (unsigned)d[1], (unsigned)d[3], (unsigned)be16u(&d[4]), (unsigned)be16u(&d[6]));
        Serial.println(l);
        break;
    }

    default: break;
    }
}

// -----------------------------------------------------------------------------
static void printStats() {
    Serial.println();
    Serial.println(F("======================================================================"));
    Serial.printf ("  %lu frames, %lu s elapsed\n",
                   (unsigned long)totalFrames, (unsigned long)(millis() / 1000));
    Serial.println(F("  ID     count   period(ms)  DLC  last payload             changing"));
    Serial.println(F("  ----------------------------------------------------------------------"));

    for (int i = 0; i < MAX_IDS; i++) {
        if (!stats[i].used) continue;
        IdStat &s = stats[i];
        uint32_t mean = (s.count > 1) ? (s.lastMs - s.firstMs) / (s.count - 1) : 0;

        char payload[26] = {0};
        int n = 0;
        for (uint8_t b = 0; b < s.dlc && b < 8; b++)
            n += snprintf(payload + n, sizeof(payload) - n, "%02X ", s.last[b]);

        char chg[10] = {0};
        for (uint8_t b = 0; b < s.dlc && b < 8; b++)
            chg[b] = (s.changed & (1u << b)) ? '*' : '.';

        Serial.printf("  0x%03X  %6lu  %5lu (%lu-%lu)  %u   %-24s %s\n",
                      (unsigned)s.id, (unsigned long)s.count, (unsigned long)mean,
                      (unsigned long)(s.minGap == 0xFFFFFFFF ? 0 : s.minGap),
                      (unsigned long)s.maxGap,
                      (unsigned)s.dlc, payload, chg);
    }

    // What did we learn about the voltage scaling?
    if (batt.have_313) {
        Serial.println(F("  ----------------------------------------------------------------------"));
        Serial.printf("  Auto-scaled pack voltage: %.2f V  (SOC %u%%)\n",
                      batt.pack_v_cV / 100.0f, (unsigned)batt.soc_pct);
        Serial.println(F("  Cross-check this against the Growatt app before trusting it."));
    }

    bool sawKeepalive = false;
    for (int i = 0; i < MAX_IDS; i++)
        if (stats[i].used && stats[i].id == 0x301) sawKeepalive = true;
    if (!sawKeepalive)
        Serial.println(F("  NOTE: no 0x301 seen - are you tapping the inverter side too?"));

    Serial.println(F("======================================================================"));
    Serial.println();
}

// -----------------------------------------------------------------------------
static void handleSerial() {
    while (Serial.available()) {
        int c = Serial.read();
        switch (c) {
        case 'r': logRaw = !logRaw;
                  Serial.printf("\n[raw logging %s]\n", logRaw ? "ON" : "OFF"); break;
        case 'd': logDecode = !logDecode;
                  Serial.printf("\n[decode %s]\n", logDecode ? "ON" : "OFF"); break;
        case 's': printStats(); break;
        case 'c': memset(stats, 0, sizeof(stats)); totalFrames = 0;
                  Serial.println(F("\n[stats cleared]")); break;
        default: break;
        }
    }
}

// =============================================================================
void setup() {
    Serial.begin(115200);
    delay(400);
    Serial.println();
    Serial.println(F("=== Growatt battery-bus sniffer  (LISTEN-ONLY) ==="));
    Serial.println(F("Safe to tap a live battery <-> Growatt inverter link:"));
    Serial.println(F("this node never ACKs, never transmits, never sends error frames."));
    Serial.println(F("Commands: r=raw  d=decode  s=stats  c=clear"));
    Serial.println();

    twai_general_config_t g =
        TWAI_GENERAL_CONFIG_DEFAULT(TWAI_TX_PIN, TWAI_RX_PIN, TWAI_MODE_LISTEN_ONLY);
    g.rx_queue_len = 64;
    twai_timing_config_t t = TWAI_TIMING_CONFIG_500KBITS();
    twai_filter_config_t f = TWAI_FILTER_CONFIG_ACCEPT_ALL();

    if (twai_driver_install(&g, &t, &f) != ESP_OK || twai_start() != ESP_OK) {
        Serial.println(F("FATAL: TWAI init failed."));
        for (;;) delay(1000);
    }
    Serial.println(F("Listening at 500 kbit/s. Waiting for frames..."));
    Serial.println(F("(silence for >30 s => check CAN_H/CAN_L, the WAKE pins, or the tap)"));
    Serial.println();

#if BOARD == BOARD_ESP32_DEVKIT
    wakeMonitorSetup();
    Serial.println();
#endif
}

void loop() {
    static uint32_t lastStats = 0;
    static uint32_t lastFrameMs = 0;
    static bool warned = false;

    handleSerial();

#if BOARD == BOARD_ESP32_DEVKIT
    wakeMonitorPoll();
#endif

    twai_message_t msg;
    while (twai_receive(&msg, pdMS_TO_TICKS(10)) == ESP_OK) {
        uint32_t now = millis();
        lastFrameMs = now;
        warned = false;

        record(msg.identifier, msg.data_length_code, msg.data, now);
        if (logRaw)    printRaw(now, msg.identifier, msg.data_length_code, msg.data);
        if (logDecode) printDecode(msg.identifier, msg.data_length_code, msg.data);

        decodeGrowatt(batt, msg.identifier, msg.data_length_code, msg.data, now);
    }

    uint32_t now = millis();
    if (!warned && totalFrames == 0 && now > 30000) {
        warned = true;
        Serial.println(F("\n!! 30 s with no frames. Check: CAN_H on pin 4 / CAN_L on pin 5"));
        Serial.println(F("   of the battery PCS port, the WAKE pair (pins 7/8), and that"));
        Serial.println(F("   you have NOT added a 120 ohm resistor on this stub.\n"));
    }
    if (totalFrames > 0 && lastFrameMs && (now - lastFrameMs) > 10000) {
        Serial.println(F("[bus has gone quiet]"));
        lastFrameMs = 0;
    }

    if (now - lastStats >= STATS_INTERVAL) {
        lastStats = now;
        printStats();
    }
}
