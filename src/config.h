// =============================================================================
//  config.h  -  Growatt GBLI 6532  ->  Solis S6-EH1P8K-L-PLUS  CAN translator
//  Everything you are likely to want to change lives in this file.
// =============================================================================
#pragma once
#ifdef ARDUINO
#include <Arduino.h>
#endif

// -----------------------------------------------------------------------------
// 0. BOARD
// -----------------------------------------------------------------------------
//  BOARD_ESP32_CAN_X2  : Autosport Labs ESP32-CAN-X2 (ESP32-S3)
//                       CAN1 = built-in TWAI, CAN2 = MCP2515 @ 16 MHz.
//                       Matches this firmware's architecture exactly.
//  BOARD_ESP32_DEVKIT  : plain ESP32 DevKit + SN65HVD230 (TWAI)
//                       + MCP2515 module (see README for the 3.3 V caveat).
//  BOARD_LILYGO_T2CAN  : LilyGo T-2Can (ESP32-S3), non-FD (MCP2515) variant.
//                       Same TWAI+MCP2515 architecture as ESP32-CAN-X2, just
//                       different pins, and its MCP2515 RST line needs an
//                       active-low pulse at boot (see main.cpp). Pins are
//                       LilyGo's own, from github.com/Xinyuan-LilyGO/T-2Can.
//                       CONFIRMED against the real schematic (2026-08-31):
//                       GPIO 7/6 (TWAI) is CAN-B, the MCP2515 drives CAN-A,
//                       and both signals pass through an MS4553S 3.3V<->5V
//                       level shifter per channel before reaching each
//                       channel's isolated Mornsun TD501MCAN transceiver.
#define BOARD_ESP32_CAN_X2   1
#define BOARD_ESP32_DEVKIT   2
#define BOARD_LILYGO_T2CAN   3

// Overridable from platformio.ini via `-D BOARD=...` (see the `*-devkit`
// envs) so switching boards doesn't require editing this file.
#ifndef BOARD
#define BOARD                BOARD_ESP32_CAN_X2
#endif

// -----------------------------------------------------------------------------
// 1. MODE
// -----------------------------------------------------------------------------
// SNIFF_ONLY: do NOT transmit anything to the inverter. Just listen on the
// battery bus and dump every frame to serial. Set to false once verified.
// (For tapping a LIVE battery<->Growatt link, flash the separate `sniffer`
// environment instead - it runs the CAN controller in listen-only mode.)
#define SNIFF_ONLY              true

#define VERBOSE_DECODE          true    // one-line decoded battery summary
#define VERBOSE_TX              false   // dump every outgoing Pylon frame

// -----------------------------------------------------------------------------
// 2. HARDWARE PINS
// -----------------------------------------------------------------------------
#if BOARD == BOARD_ESP32_CAN_X2
    // --- CAN1 (built-in TWAI) -> BATTERY bus
    #define TWAI_TX_PIN         GPIO_NUM_7
    #define TWAI_RX_PIN         GPIO_NUM_6
    // --- CAN2 (MCP2515 over SPI) -> INVERTER bus
    #define MCP_CS_PIN          10
    #define MCP_INT_PIN         3
    #define MCP_SCK_PIN         12
    #define MCP_MISO_PIN        13
    #define MCP_MOSI_PIN        11
    #define MCP_CRYSTAL         MCP_16MHZ
    #define STATUS_LED_PIN      -1      // no convenient user LED; set if you add one

#elif BOARD == BOARD_ESP32_DEVKIT
    // --- Built-in TWAI + SN65HVD230 breakout -> BATTERY bus
    #define TWAI_TX_PIN         GPIO_NUM_5
    #define TWAI_RX_PIN         GPIO_NUM_4
    // --- MCP2515 module on VSPI -> INVERTER bus
    #define MCP_CS_PIN          15
    #define MCP_INT_PIN         27
    #define MCP_SCK_PIN         18
    #define MCP_MISO_PIN        19
    #define MCP_MOSI_PIN        23
    // Blue "niren" MCP2515 boards are usually 8 MHz, black ones often 16 MHz.
    // The wrong value here means no communication at all.
    #define MCP_CRYSTAL         MCP_8MHZ
    #define STATUS_LED_PIN      2

#elif BOARD == BOARD_LILYGO_T2CAN
    // Pins per LilyGo's pin_config.h. "CAN B" (TWAI) -> BATTERY bus,
    // "CAN A" (MCP2515) -> INVERTER bus - keeps this project's existing
    // TWAI=battery / MCP2515=inverter convention.
    #define TWAI_TX_PIN         GPIO_NUM_7
    #define TWAI_RX_PIN         GPIO_NUM_6
    #define MCP_CS_PIN          10
    #define MCP_INT_PIN         8
    #define MCP_SCK_PIN         12
    #define MCP_MISO_PIN        13
    #define MCP_MOSI_PIN        11
    // MCP2515 RST is a broken-out GPIO here (unlike CAN-X2/DevKit, where it's
    // tied on-board) and must be pulsed low then high before MCP.begin() -
    // see mcpInit() in main.cpp. Undefined on boards that don't need it.
    #define MCP_RST_PIN         9
    // CONFIRMED against the real schematic (X1, 2026-08-31): 16 MHz.
    #define MCP_CRYSTAL         MCP_16MHZ
    #define STATUS_LED_PIN      -1      // unconfirmed; set if you find one
#else
    #error "Set BOARD to a supported value"
#endif

// -----------------------------------------------------------------------------
// 3. TIMING
// -----------------------------------------------------------------------------
#define PYLON_TX_INTERVAL_MS    1000    // Pylon protocol is a strict 1 Hz burst
#define PYLON_TX_GAP_MS         5       // gap between frames within the burst

#define GROWATT_0x301_ENABLE    true    // send the inverter->BMS keepalive
#define GROWATT_0x301_INTERVAL  1000

// Battery data considered stale after this long with no 0x311/0x313.
// -> current limits forced to 0, charge/discharge disabled, system-error set.
#define BATT_STALE_DERATE_MS    5000
// After this long, stop transmitting entirely so the Solis raises
// Batt_Comm_FAIL and safely disconnects the battery.
#define BATT_STALE_SILENCE_MS   30000

// -----------------------------------------------------------------------------
// 4. SAFETY CLAMPS  (applied to what we FORWARD to the Solis)
// -----------------------------------------------------------------------------
// Solis accepts 40.0 - 60.0 V. GBLI 6532 datasheet range is 48.0 - 57.6 V.
#define CVL_MAX_dV              576     // 57.6 V - never ask Solis for more
#define CVL_MIN_dV              480     // 48.0 V - reject nonsense low values
#define CCL_MAX_dA              1050    // 105.0 A ceiling on charge current
#define DCL_MAX_dA              1050    // 105.0 A ceiling on discharge current

// Plausibility window for the pack voltage reported in 0x356 (0.01 V units).
#define PACK_V_MIN_cV           4000    // 40.00 V
#define PACK_V_MAX_cV           6000    // 60.00 V

// -----------------------------------------------------------------------------
// 5. PROTOCOL QUIRKS
// -----------------------------------------------------------------------------
// Growatt's spec says 0x313 bytes 0-1 are 0.01 V; growattArkCAN observed 0.1 V.
// Confirmed 0.01 V against a real GBLI6532 capture (53.20-53.40 V through an
// idle -> discharge -> charge cycle - sane; the 0.1 V reading would be ~530 V).
#define VS_AUTO   0
#define VS_0V01   1
#define VS_0V1    2
#define GROWATT_0x313_VOLT_SCALE   VS_0V01

// Manufacturer string sent in 0x35E. Use "PYLON   " with the PYLON_LV profile.
#define PYLON_MANUFACTURER      "PYLON   "

// Packs reported in 0x359 byte 4 (overridden by 0x312 byte 4 if plausible).
#define PYLON_PACK_COUNT        1

// 0x311 is the authoritative source for charge/discharge-enable state (see
// translate.h) - a real capture showed 0x319's own enable bits don't track
// live state. This flag only controls whether 0x319's enable bits are used
// as a temporary fallback before the first 0x311 has arrived. Leave true.
#define ENABLE_0x319_FALLBACK   true
