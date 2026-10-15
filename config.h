#pragma once
#include <Arduino.h>

// =============================================================================
// config.h — Keihin Universal Scanner v0.5
// Konfigurasi Hardware: ESP32-C3 Super Mini
// =============================================================================

// --- PINOUT K-LINE (UART1) ---
// Terhubung ke Transceiver (PC817/BC547 atau L9637D)
#define K_RXD 3
#define K_TXD 4

// --- PINOUT DEV1 (UART0) ---
// Jalur komunikasi ke ESP32 Utama / Dashboard
// Menggunakan hardware UART0 (Default C3)
#define D1_RXD 20
#define D1_TXD 21

// --- INDIKATOR ---
#define LED_PIN 8  // LED bawaan ESP32-C3 Super Mini (Active LOW)

// --- TIMING & LOGIC ---
#define POLL_INTERVAL_MS 250  // Kecepatan request data ke ECU
#define RETRY_DELAY_MS 1200   // Jeda saat terjadi error koneksi
#define MAX_RETRY 3           // Maksimal percobaan sebelum reset state

// =============================================================================
// MACRO LOGGING (SERIAL MONITOR TOGGLE)
// =============================================================================
// Variable 'serialEnabled' didefinisikan di main.ino
extern bool serialEnabled;

// Gunakan SLOG(x) untuk Serial.print/printf
#define SLOG(x)        \
  if (serialEnabled) { \
    x;                 \
  }

// Gunakan SLOGF(x) untuk Serial.println(F(x)) — hemat RAM
#define SLOGF(x)          \
  if (serialEnabled) {    \
    Serial.println(F(x)); \
  }

// =============================================================================
// HARDWARE SERIAL OBJECTS
// =============================================================================
// Dideklarasikan di main.ino, dipanggil di uart.h/decoder.h
extern HardwareSerial KLine;
extern HardwareSerial Dev1;

// =============================================================================
// GLOBAL CONSTANTS
// =============================================================================
const char VERSION[] = "0.5-C3";
