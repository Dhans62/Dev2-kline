#include "config.h"
#include "decoder.h"
#include "profiles.h"
#include "uart.h"
#include <Arduino.h>
#include <HardwareSerial.h>
// =============================================================================
// main.ino — Keihin Universal Scanner DEV2 v0.5
// Hardware: ESP32-C3 Super Mini
//
// Perintah Serial Monitor:
//   R = reconnect        S = raw sniffer
//   B = brute force      T = reset TPS
//   V = speed hunter     X = poll all tables
//   D = DTC aktif        H = DTC history
//   C = clear DTC        P = toggle Serial Monitor output
// =============================================================================
// Hardware objects
HardwareSerial KLine(1);  // UART1 → K-Line ke ECM
HardwareSerial Dev1(0);
// Serial toggle
bool serialEnabled = true;
// =============================================================================
// SCANNER STATE
// =============================================================================
enum ScannerState {
  STATE_INIT,
  STATE_HANDSHAKE,
  STATE_GET_ROMID,
  STATE_GET_BARO,
  STATE_DETECT_TABLE,
  STATE_LIVE_DATA,
  STATE_ERROR
};
ScannerState state = STATE_INIT;
uint8_t retryCount = 0;
const ProfileMap *activeProfile = nullptr;
byte activeTableID = 0x11;
LiveData liveData;
LiveData prevData;
bool rawSnifferMode = false;
uint32_t lastPollMs = 0;
uint32_t lastInitMs = 0;
// =============================================================================
// LED
// =============================================================================
uint32_t ledTimer = 0;
int ledBlinkCount = 0;
bool ledState = HIGH;
bool setupPhase = true;
// =============================================================================
// X POLL MODE — poll semua tabel known secara berulang
// =============================================================================
const byte X_TABLES[] = {0x00, 0x20, 0x61, 0x70, 0xA6, 0xD0, 0xD1};
const char *X_NAMES[] = {"T00", "T20", "T61", "T70", "TA6", "TD0", "TD1"};
const int X_COUNT = sizeof(X_TABLES) / sizeof(byte);
const uint32_t X_INTERVAL_MS = 800;
bool xPollMode = false;
uint32_t xLastPollMs = 0;
byte xPrevFrame[7][32];
byte xPrevLen[7];
bool xPrevValid[7];
void initXPrev() {
  memset(xPrevFrame, 0, sizeof(xPrevFrame));
  memset(xPrevLen, 0, sizeof(xPrevLen));
  memset(xPrevValid, 0, sizeof(xPrevValid));
}
// =============================================================================
// K-LINE UTILS
// =============================================================================
byte calcChecksum(byte *data, int len) {
  byte sum = 0;
  for (int i = 0; i < len; i++) sum += data[i];
  return (byte)(0 - sum);
}
// delay(40) JANGAN diubah — timing hardware PC817+BC547
void sendCommand(byte *cmd, int len) {
  while (KLine.available()) KLine.read();
  KLine.write(cmd, len);
  delay(40);
  for (int i = 0; i < len; i++) {
    if (KLine.available()) KLine.read();
  }
}
int readResponse(byte *buf, int maxLen, uint32_t timeoutMs = 200) {
  uint32_t t = millis();
  int len = 0;
  while (millis() - t < timeoutMs && len < maxLen) {
    if (KLine.available()) {
      buf[len++] = KLine.read();
      t = millis();
    }
  }
  return len;
}
int findFrameStart(byte *buf, int len, byte tableID) {
  for (int i = 0; i < len - 3; i++) {
    if (buf[i] == 0x02 && buf[i + 2] == 0x71 && buf[i + 3] == tableID) return i;
  }
  return -1;
}
int requestTable(byte tableID, byte *buf, int maxLen) {
  byte cmd[] = {0x72, 0x05, 0x71, tableID, 0x00};
  cmd[4] = calcChecksum(cmd, 4);
  sendCommand(cmd, 5);
  delay(100);
  return readResponse(buf, maxLen, 150);
}
// =============================================================================
// STATE MACHINE
// =============================================================================
void doStateInit() {
  SLOGF("[INIT] K-Line bus reset...");
  KLine.end();
  pinMode(K_TXD, OUTPUT);
  digitalWrite(K_TXD, HIGH);
  delay(1500);
  digitalWrite(K_TXD, LOW);
  delay(70);
  digitalWrite(K_TXD, HIGH);
  delay(120);
  KLine.begin(10400, SERIAL_8N1, K_RXD, K_TXD);
  state = STATE_HANDSHAKE;
}
void doStateHandshake() {
  byte wakeUp[] = {0xFE, 0x04, 0x72, 0x8C};
  KLine.write(wakeUp, 4);
  delay(100);
  while (KLine.available()) KLine.read();
  byte init[] = {0x72, 0x05, 0x00, 0xF0, 0x99};
  KLine.write(init, 5);
  delay(150);
  for (int i = 0; i < 5; i++) {
    if (KLine.available()) KLine.read();
  }
  delay(100);
  if (KLine.available()) {
    while (KLine.available()) KLine.read();
    SLOGF("[HANDSHAKE] OK");
    state = STATE_GET_ROMID;
    retryCount = 0;
  } else {
    retryCount++;
    SLOG(Serial.printf("[HANDSHAKE] No response (%d/%d)\n", retryCount, MAX_RETRY));
    state = (retryCount >= MAX_RETRY) ? STATE_ERROR : STATE_INIT;
  }
}
void doStateGetROMID() {
  byte cmd[] = {0x72, 0x05, 0x71, 0x00, 0x18};
  sendCommand(cmd, 5);
  delay(200);
  byte buf[32];
  int len = readResponse(buf, 32, 300);
  int idPos = -1;
  for (int i = 0; i < len - 4; i++) {
    if (buf[i] == 0x02 && buf[i + 2] == 0x71 && buf[i + 3] == 0x00) {
      idPos = i + 4;
      break;
    }
  }
  uint32_t romID = 0;
  if (idPos >= 0 && (idPos + 3) < len) {
    romID = ((uint32_t)buf[idPos] << 24) | ((uint32_t)buf[idPos + 1] << 16) |
            ((uint32_t)buf[idPos + 2] << 8) | (uint32_t)buf[idPos + 3];
  }
  SLOG(Serial.printf("[ROM ID] %08X\n", romID));
  activeProfile = findProfile(romID);
  if (activeProfile != nullptr) {
    SLOG(Serial.printf("[PROFILE] %s (%s)\n", activeProfile->motorName, activeProfile->ecuType));
    rawSnifferMode = false;
  } else {
    SLOGF("[PROFILE] N/A — Generic mode, Raw Sniffer ON");
    activeProfile = &PROFILE_UNKNOWN;
    rawSnifferMode = true;
  }
  prevData.tps_raw_min = activeProfile->tps_default_min;
  prevData.tps_raw_max = activeProfile->tps_default_max;
  prevData.baro_kpa = -999.0f;
  prevData.speed_kph = -999.0f;
  activeTableID = 0x11;
  state = STATE_GET_BARO;
}
void doStateGetBARO() {
  if (activeProfile->baro_tableID == 0x00 || activeProfile->idx_baro < 0) {
    SLOGF("[BARO] Tidak tersedia di profil ini");
    state = STATE_DETECT_TABLE;
    return;
  }
  byte buf[64];
  int len = requestTable(activeProfile->baro_tableID, buf, 64);
  int s = findFrameStart(buf, len, activeProfile->baro_tableID);
  if (s >= 0 && len > (s + activeProfile->idx_baro)) {
    byte rawBaro = buf[s + activeProfile->idx_baro];
    prevData.baro_kpa = decodeBARO(rawBaro);
    SLOG(Serial.printf("[BARO] %.1f kPa (raw=0x%02X dari T%02X[%02d])\n", prevData.baro_kpa,
                       rawBaro, activeProfile->baro_tableID, activeProfile->idx_baro));
  } else {
    SLOGF("[BARO] Tabel tidak merespon — lanjut tanpa BARO");
  }
  state = STATE_DETECT_TABLE;
}
void doStateDetectTable() {
  byte buf[64];
  int len = requestTable(0x11, buf, 64);
  int s11 = findFrameStart(buf, len, 0x11);
  if (len == 0) {
    SLOGF("[TABLE] T11 no response -> INIT");
    state = STATE_INIT;
    return;
  }
  if (s11 >= 0) {
    byte frameLen = buf[s11 + 1];
    bool isNoData = (frameLen == 0x05);
    bool hasData = false;
    if (!isNoData) {
      for (int i = 4; i < frameLen && i < 20; i++) {
        if (buf[s11 + i] != 0x00 && buf[s11 + i] != 0xFF) {
          hasData = true;
          break;
        }
      }
    }
    if (hasData) {
      activeTableID = 0x11;
      SLOGF("[TABLE] T11 OK");
      state = STATE_LIVE_DATA;
      return;
    }
    SLOGF("[TABLE] T11 no-data -> coba T17");
  }
  memset(buf, 0, sizeof(buf));
  len = requestTable(0x17, buf, 64);
  int s17 = findFrameStart(buf, len, 0x17);
  if (s17 >= 0 && len > 5) {
    activeTableID = 0x17;
    SLOGF("[TABLE] T17 OK");
    state = STATE_LIVE_DATA;
  } else {
    SLOGF("[TABLE] T11 & T17 tidak ada data -> ERROR");
    state = STATE_ERROR;
  }
}
void doStateLiveData() {
  byte buf[128];
  int len = requestTable(activeTableID, buf, 128);
  int s = findFrameStart(buf, len, activeTableID);
  if (s < 0) {
    retryCount++;
    if (retryCount >= MAX_RETRY) {
      SLOGF("[LIVE] ECU timeout -> reconnect");
      sendUARTDisconnect();
      state = STATE_INIT;
      retryCount = 0;
    }
    return;
  }
  retryCount = 0;
  liveData = parseFrame(buf, s, *activeProfile, prevData);
  if (activeProfile->speed_tableID != 0x00 && activeProfile->idx_speed >= 0) {
    byte sbuf[64];
    int slen = requestTable(activeProfile->speed_tableID, sbuf, 64);
    int ss = findFrameStart(sbuf, slen, activeProfile->speed_tableID);
    if (ss >= 0 && slen > (ss + activeProfile->idx_speed))
      liveData.speed_kph = decodeSPEED(sbuf[ss + activeProfile->idx_speed]);
  }
  prevData = liveData;
  if (!liveData.valid) return;
  // UART ke DEV1 — selalu dikirim (tidak terpengaruh serialEnabled)
  sendUARTData(liveData, true);
  // Serial Monitor — hanya kalau enabled
  SLOG(printLiveData(liveData));
  if (rawSnifferMode && serialEnabled) {
    byte frameLen = buf[s + 1];
    Serial.printf("[RAW T%02X]\n", activeTableID);
    printRawFrame(buf, s, frameLen);
  }
}
void doStateError() {
  if (millis() - lastInitMs > RETRY_DELAY_MS) {
    SLOGF("[ERROR] Retry...");
    sendUARTDisconnect();
    state = STATE_INIT;
    retryCount = 0;
    lastInitMs = millis();
  }
}
// =============================================================================
// X POLL — poll semua tabel known, tampilkan diff
// =============================================================================
void doXPoll() {
  if (serialEnabled) Serial.println(F("--- X POLL ---"));
  for (int t = 0; t < X_COUNT; t++) {
    byte buf[64];
    int len = requestTable(X_TABLES[t], buf, 64);
    int s = findFrameStart(buf, len, X_TABLES[t]);
    if (s < 0 || len <= 5) {
      if (xPrevValid[t] && serialEnabled) Serial.printf("[%s] NO RESPONSE *\n", X_NAMES[t]);
      xPrevValid[t] = false;
      continue;
    }
    byte frameLen = buf[s + 1];
    int dataStart = s + 4;
    int dataLen = frameLen - 4;
    if (dataLen <= 0 || dataLen > 28) {
      dataLen = frameLen;
      dataStart = s;
    }
    bool hasChange = !xPrevValid[t] || xPrevLen[t] != dataLen;
    if (!hasChange) {
      for (int i = 0; i < dataLen; i++) {
        if (buf[dataStart + i] != xPrevFrame[t][i]) {
          hasChange = true;
          break;
        }
      }
    }
    if (serialEnabled) {
      Serial.printf("[%s] ", X_NAMES[t]);
      for (int i = 0; i < dataLen && i < 28; i++) {
        byte cur = buf[dataStart + i];
        bool diff = xPrevValid[t] && (cur != xPrevFrame[t][i]);
        Serial.printf(diff ? "*%02X " : " %02X ", cur);
      }
      if (hasChange)
        Serial.println(F("<"));
      else
        Serial.println();
    }
    memcpy(xPrevFrame[t], buf + dataStart, dataLen);
    xPrevLen[t] = dataLen;
    xPrevValid[t] = true;
  }
}
// =============================================================================
// COMMAND HANDLER
// =============================================================================
void handleCommand(char key) {
  switch (key) {
    case 'r':
    case 'R':
      SLOGF("[CMD] Force reconnect");
      state = STATE_INIT;
      break;
    case 's':
    case 'S':
      rawSnifferMode = !rawSnifferMode;
      if (serialEnabled) {
        Serial.printf("[CMD] Raw Sniffer: %s\n", rawSnifferMode ? "ON" : "OFF");
      }
      break;
    case 'p':
    case 'P':
      serialEnabled = !serialEnabled;
      if (serialEnabled) {
        Serial.println(F("[SERIAL] ON — output aktif"));
      } else {
        // Opsional: beri tahu sebelum benar-benar OFF
        Serial.println(F("[SERIAL] OFF — silent mode"));
      }
      break;
    case 'b':
    case 'B': {  // WAJIB KURUNG UNTUK DEKLARASI VARIABEL DALAM CASE
      SLOGF("[CMD] Brute force T00-TFF...");
      for (int i = 0; i <= 0xFF; i++) {
        byte buf[128];
        int len = requestTable((byte)i, buf, 128);
        int s = findFrameStart(buf, len, (byte)i);
        if (s >= 0 && len > 5 && serialEnabled) {
          Serial.printf("\n[FOUND] Table 0x%02X | %d bytes:\n", i, buf[s + 1]);
          printRawFrame(buf, s, buf[s + 1]);
        }
      }
      SLOGF("[CMD] Brute force selesai");
    } break;
    case 't':
    case 'T':
      if (activeProfile) {
        prevData.tps_raw_min = activeProfile->tps_default_min;
        prevData.tps_raw_max = activeProfile->tps_default_max;
        SLOGF("[CMD] TPS auto-learn reset");
      }
      break;
    case 'v':
    case 'V': {  // PROTEKSI SCOPE VARIABEL
      SLOGF("[CMD] Speed hunter — scan TD0, T61, TD1");
      byte tables[] = {0xD0, 0x61, 0xD1};
      const char *names[] = {"TD0", "T61", "TD1"};
      for (int t = 0; t < 3; t++) {
        byte buf[64];
        int len = requestTable(tables[t], buf, 64);
        int s = findFrameStart(buf, len, tables[t]);
        if (serialEnabled) {
          if (s >= 0 && len > 5) {
            Serial.printf("[%s] %d bytes:\n", names[t], buf[s + 1]);
            printRawFrame(buf, s, buf[s + 1]);
          } else {
            Serial.printf("[%s] no response\n", names[t]);
          }
        }
      }
    } break;
    case 'x':
    case 'X':
      xPollMode = !xPollMode;
      if (xPollMode) {
        initXPrev();
        SLOGF("[X] Poll mode ON — tekan X untuk stop");
        if (serialEnabled) {
          Serial.print(F("[X] Tabel: "));
          for (int t = 0; t < X_COUNT; t++) {
            Serial.printf("T%02X ", X_TABLES[t]);
          }
          Serial.println();
        }
      } else {
        SLOGF("[X] Poll mode OFF");
      }
      break;
    case 'd':
    case 'D': {
      SLOGF("[DTC] Membaca kode fault aktif...");
      byte cmd[] = {0x72, 0x05, 0x74, 0x01, 0x00};
      cmd[4] = calcChecksum(cmd, 4);
      sendCommand(cmd, 5);
      delay(300);
      byte buf[64];
      int len = readResponse(buf, 64, 400);
      if (len > 0)
        parseDTC(buf, len, 0x74);
      else {
        SLOGF("[DTC] Tidak ada response dari ECM");
      }
    } break;
    case 'h':
    case 'H': {
      SLOGF("[DTC] Membaca kode fault history...");
      byte cmd[] = {0x72, 0x05, 0x73, 0x01, 0x00};
      cmd[4] = calcChecksum(cmd, 4);
      sendCommand(cmd, 5);
      delay(300);
      byte buf[64];
      int len = readResponse(buf, 64, 400);
      if (len > 0)
        parseDTC(buf, len, 0x73);
      else {
        SLOGF("[DTC] Tidak ada response dari ECM");
      }
    } break;
    case 'c':
    case 'C': {
      SLOGF("[DTC] CLEAR semua kode fault...");
      byte cmd[] = {0x72, 0x05, 0x14, 0x01, 0x00};
      cmd[4] = calcChecksum(cmd, 4);
      sendCommand(cmd, 5);
      delay(300);
      byte buf[32];
      int len = readResponse(buf, 32, 400);
      bool ok = false;
      for (int i = 0; i < len - 2; i++) {
        if (buf[i] == 0x02 && buf[i + 2] == 0x14) {
          ok = true;
          break;
        }
      }
      if (ok) {
        SLOGF("[DTC] Clear berhasil");
      } else {
        SLOGF("[DTC] Tidak ada konfirmasi dari ECM");
      }
    } break;
    default:
      break;
  }
}
// =============================================================================
// SETUP & LOOP
// =============================================================================
void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, HIGH);

  Serial.begin(115200);  // USB-C native, tidak pakai GPIO
  Dev1.begin(115200, SERIAL_8N1, D1_RXD, D1_TXD);

  if (Serial) {
    delay(500);
  }

  // --- Tampilan Informasi saat Booting ---
  SLOGF("=================================");
  SLOGF("    Keihin Scanner DEV2 v0.5     ");
  SLOGF("    ESP32-C3 Super Mini         ");
  SLOGF("=================================");

  // Ambil nomor PIN langsung dari config.h agar tidak ada kebohongan publik
  SLOG(Serial.printf(" K-Line : GPIO%d(TX)  GPIO%d(RX)\n", K_TXD, K_RXD));
  SLOG(Serial.printf(" DEV1   : GPIO%d(TX) GPIO%d(RX)\n", D1_TXD, D1_RXD));

  SLOGF("---------------------------------");
  SLOGF(" R=reconnect  S=sniffer  B=brute ");
  SLOGF(" T=reset TPS  V=speed    X=poll  ");
  SLOGF(" D=DTC aktif  H=history  C=clear ");
  SLOGF(" P=toggle Serial Monitor output  ");
  SLOGF("---------------------------------");
  
// --- INISIALISASI TIMER ---
  ledTimer = millis();
  setupPhase = true; 
  ledBlinkCount = 0;
}

void loop() {
  // --- Task 1: LED Blink Indicator (Non-Blocking) ---
  if (setupPhase) {
    if (millis() - ledTimer >= 100) {
      ledState = !ledState;
      digitalWrite(LED_PIN, ledState);
      ledTimer = millis();
      if (++ledBlinkCount >= 4) {
        setupPhase = false;
        digitalWrite(LED_PIN, LOW); // LED ON (Scanner Ready)
      }
    }
  }

  // --- Task 2: Command Handler ---
  if (Serial.available()) handleCommand((char)Serial.read());

  // --- Task 3: Keihin Scanner State Machine ---
  switch (state) {
    case STATE_INIT:         doStateInit(); break;
    case STATE_HANDSHAKE:    doStateHandshake(); break;
    case STATE_GET_ROMID:    doStateGetROMID(); break;
    case STATE_GET_BARO:     doStateGetBARO(); break;
    case STATE_DETECT_TABLE: doStateDetectTable(); break;
    case STATE_LIVE_DATA:
      if (millis() - lastPollMs >= POLL_INTERVAL_MS) {
        lastPollMs = millis();
        doStateLiveData();
      }
      break;
    case STATE_ERROR:        doStateError(); break;
    default:                 state = STATE_INIT; break;
  }

  // --- Task 4: Parallel X-Poll Mode ---
  if (xPollMode && (millis() - xLastPollMs >= X_INTERVAL_MS)) {
    xLastPollMs = millis();
    doXPoll();
  }
}
