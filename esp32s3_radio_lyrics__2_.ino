/*
  ====================================================================
  ESP32-S3 Mini - INTERNET RADIO + LIRIK SYNC di OLED
  --------------------------------------------------------------------
  Speaker I2S (MAX98357A) : BCLK = 5, LRC = 6, DIN(DOUT) = 7
  OLED SSD1306 128x64 (I2C): SDA = 3, SCL = 4
  Servo (cuma pemanis)    : GPIO 2

  CARA KERJA:
   - Putar radio internet -> suara keluar dari speaker.
   - Radio kirim judul lagu (Artist - Title) lewat metadata.
   - ESP32 ambil lirik dari LRCLIB (lrclib.net) otomatis.
   - Lirik yang punya timing (LRC) ditampilkan ikut jalan di OLED.
   - Servo goyang pelan saat musik main (dekorasi).

  Library yang harus di-install (Library Manager):
    1. ESP32-audioI2S   (by schreibfaul1)
    2. Adafruit SSD1306
    3. Adafruit GFX
    4. ESP32Servo
    5. ArduinoJson      (versi 7.x)

  Board   : ESP32S3 Dev Module
  PENTING : kalau board kamu ada PSRAM, AKTIFKAN di Tools > PSRAM.
            Lirik + audio butuh RAM, jauh lebih stabil dgn PSRAM.

  Perintah lewat Serial Monitor (115200 baud):
     n / p   : ganti stasiun (next / prev)
     0-9     : pilih stasiun langsung
     + / -   : volume naik / turun
     ] / [   : geser sinkron lirik +/- 250 ms (buat nyetel pas)
  ====================================================================
*/

#include <WiFi.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <ESP32Servo.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "Audio.h"

// ---------------- KONFIG WIFI ----------------
const char* WIFI_SSID = "ESPServer";
const char* WIFI_PASS = "mayunskuy123";

// ---------------- SAKLAR TES ----------------
// Set ke 0 kalau mau matikan servo total (buat ngetes apakah servo
// penyebab board hang). Set ke 1 untuk normal.
#define USE_SERVO  1

// ---------------- PIN ----------------
#define I2S_BCLK   5
#define I2S_LRC    6     // LRC pindah ke 6 (tidak boleh sama dgn DIN)
#define I2S_DOUT   7     // ke pin DIN di MAX98357A

#define OLED_SDA   3
#define OLED_SCL   4
#define OLED_ADDR  0x3C  // umumnya 0x3C, kadang 0x3D
#define SERVO_PIN  2

// ---------------- DAFTAR STASIUN ----------------
// Prambors = lagu berlirik + kirim metadata judul -> bagus buat lirik.
// Mau tambah stasiun lain? Pakai URL stream MP3/AAC langsung yg
// mengirim metadata "Artist - Title".
struct Station { const char* name; const char* url; };
Station stations[] = {
  { "Prambors",  "http://playerservices.streamtheworld.com/api/livestream-redirect/PRAMBORS_FM.mp3" },
  { "i-Radio",   "https://stream.radiojar.com/4ywdgup3bnzuv" },
  { "Indie Pop", "http://ice1.somafm.com/indiepop-128-mp3" },   // cadangan (kadang instrumental)
};
const int NUM_STATIONS = sizeof(stations) / sizeof(stations[0]);
int currentStation = 0;

// ---------------- OBJEK ----------------
Adafruit_SSD1306 oled(128, 64, &Wire, -1);
Audio  audio;
Servo  servo;

// ---------------- DATA LIRIK ----------------
#define MAX_LRC   110          // maksimal baris lirik
#define LRC_LEN   42           // maksimal karakter per baris
long  lrcTime[MAX_LRC];        // waktu (ms) tiap baris (-1 = tanpa timing)
char  lrcText[MAX_LRC][LRC_LEN];
volatile int  lrcCount  = 0;
volatile bool lrcSynced = false;   // true = punya timing
SemaphoreHandle_t lrcMutex;

int   currentLine = 0;
long  songStartMs = 0;             // perkiraan kapan lagu mulai
long  lyricOffset = 0;             // koreksi sinkron (ms), stel pakai [ ]
unsigned long lastPlainAdvance = 0;

// ---------------- ANTRIAN FETCH LIRIK ----------------
volatile bool fetchPending = false;
char reqArtist[80];
char reqTitle[80];
SemaphoreHandle_t reqMutex;

// ---------------- TAMPILAN ----------------
String headerTitle = "";       // "Artist - Title" mentah dari radio
bool   displayDirty = true;
unsigned long lastDisplay = 0;

// ---------------- SERVO ----------------
int servoAngle = 90, servoDir = 1;
unsigned long lastServo = 0;

// ====================================================================
//  SETUP
// ====================================================================
void setup() {
  Serial.begin(115200);
  delay(800);                       // beri waktu USB CDC siap

  Serial.println();
  Serial.println("=====================================");
  Serial.println("[BOOT] Mulai setup()");
  Serial.printf ("[BOOT] Reset reason   : %d\n", (int)esp_reset_reason());
  Serial.printf ("[BOOT] Flash size     : %u bytes\n", ESP.getFlashChipSize());
  Serial.printf ("[BOOT] Free heap (RAM): %u bytes\n", ESP.getFreeHeap());
  if (psramFound())
    Serial.printf ("[BOOT] PSRAM TERDETEKSI: %u bytes  -> setting PSRAM benar\n", ESP.getPsramSize());
  else
    Serial.println("[BOOT] PSRAM TIDAK ada -> set Tools>PSRAM: Disabled, atau cek QSPI/OPI");
  Serial.println("=====================================");

  Serial.println("[1] Bikin mutex...");
  lrcMutex = xSemaphoreCreateMutex();
  reqMutex = xSemaphoreCreateMutex();
  Serial.println("[1] Mutex OK");

  Serial.println("[2] Init OLED (I2C SDA=3 SCL=4)...");
  Wire.begin(OLED_SDA, OLED_SCL);

  // --- Scan I2C: tunjukkan alamat perangkat yang terdeteksi ---
  Serial.print("[2] Scan I2C, ketemu di alamat:");
  bool foundAny = false;
  for (uint8_t addr = 1; addr < 127; addr++) {
    Wire.beginTransmission(addr);
    if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", addr); foundAny = true; }
  }
  if (!foundAny) Serial.print(" (TIDAK ADA! cek kabel SDA/SCL/VCC/GND)");
  Serial.println();

  if (!oled.begin(SSD1306_SWITCHCAPVCC, OLED_ADDR)) {
    Serial.println("[2] !! OLED GAGAL di 0x3C. Coba ganti OLED_ADDR ke 0x3D. Lanjut tanpa OLED.");
  } else {
    Serial.println("[2] OLED OK");
  }
  oled.clearDisplay();
  oled.setTextColor(SSD1306_WHITE);
  oled.setTextSize(1);
  banner("Menyalakan...");
  Serial.flush();

  Serial.printf ("[3] Connect WiFi ke '%s' ...\n", WIFI_SSID);
  banner("Menyambung WiFi");
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long t0 = millis();
  while (WiFi.status() != WL_CONNECTED) {
    delay(400);
    Serial.print(".");
    if (millis() - t0 > 20000) {     // timeout 20 detik biar nggak diam selamanya
      Serial.println("\n[3] !! WiFi GAGAL 20s. Cek: SSID/password benar? WiFi harus 2.4GHz!");
      banner("WiFi gagal :(");
      t0 = millis();                 // coba terus, tapi log-nya jelas
    }
  }
  Serial.print("\n[3] WiFi OK. IP: ");
  Serial.println(WiFi.localIP());
  Serial.flush();

  // Servo cuma pemanis -> di-init SETELAH WiFi. Kalau board sehat sampai
  // sini berarti servo bukan biang hang. Bisa dimatikan via USE_SERVO.
#if USE_SERVO
  Serial.println("[4] Init servo (pin 2)...");
  ESP32PWM::allocateTimer(0);
  servo.setPeriodHertz(50);
  servo.attach(SERVO_PIN, 500, 2400);
  servo.write(90);
  Serial.println("[4] Servo OK");
#else
  Serial.println("[4] Servo DIMATIKAN (USE_SERVO 0)");
#endif

  Serial.println("[5] Bikin task lirik di core 0...");
  xTaskCreatePinnedToCore(lyricsTask, "lyrics", 10000, NULL, 1, NULL, 0);
  Serial.println("[5] Task lirik OK");

  Serial.println("[6] Set pin I2S audio (BCLK=5 LRC=6 DIN=7)...");
  audio.setPinout(I2S_BCLK, I2S_LRC, I2S_DOUT);
  audio.setVolume(12);   // 0..21
  Serial.println("[6] Audio OK");

  Serial.println("[7] Mulai stasiun pertama...");
  startStation(currentStation);
  Serial.println("[7] setup() SELESAI -> masuk loop()\n");
}

// ====================================================================
//  LOOP (core 1) — prioritas utama: audio.loop()
// ====================================================================
void loop() {
  audio.loop();
  handleSerial();
  updateLyricIndex();
  updateServo();
  updateDisplay();
  if (WiFi.status() != WL_CONNECTED) { WiFi.reconnect(); delay(500); }
}

// ====================================================================
//  STASIUN & SERIAL
// ====================================================================
void startStation(int idx) {
  if (idx < 0) idx = NUM_STATIONS - 1;
  if (idx >= NUM_STATIONS) idx = 0;
  currentStation = idx;
  clearLyrics();
  headerTitle = "";
  displayDirty = true;
  Serial.printf("[RADIO] Menyambung ke stasiun #%d: %s\n", idx, stations[idx].name);
  Serial.printf("[RADIO] URL: %s\n", stations[idx].url);
  banner("Menyambung\nradio...");
  audio.connecttohost(stations[idx].url);
}

void handleSerial() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if      (c == 'n') startStation(currentStation + 1);
  else if (c == 'p') startStation(currentStation - 1);
  else if (c >= '0' && c <= '9') startStation(c - '0');
  else if (c == '+') audio.setVolume(min(21, audio.getVolume() + 1));
  else if (c == '-') audio.setVolume(max(0,  audio.getVolume() - 1));
  else if (c == ']') { lyricOffset += 250; Serial.printf("offset=%ld ms\n", lyricOffset); }
  else if (c == '[') { lyricOffset -= 250; Serial.printf("offset=%ld ms\n", lyricOffset); }
}

// ====================================================================
//  LIRIK: pilih baris aktif berdasarkan waktu
// ====================================================================
void updateLyricIndex() {
  if (lrcCount == 0) return;

  if (lrcSynced) {
    long el = (long)(millis() - songStartMs) + lyricOffset;
    int idx = 0;
    for (int i = 0; i < lrcCount; i++) {
      if (lrcTime[i] <= el) idx = i; else break;
    }
    if (idx != currentLine) { currentLine = idx; displayDirty = true; }
  } else {
    // lirik tanpa timing -> geser pelan tiap 3.5 detik
    if (millis() - lastPlainAdvance > 3500) {
      lastPlainAdvance = millis();
      if (currentLine < lrcCount - 1) { currentLine++; displayDirty = true; }
    }
  }
}

void clearLyrics() {
  if (xSemaphoreTake(lrcMutex, portMAX_DELAY)) {
    lrcCount = 0; lrcSynced = false; currentLine = 0;
    xSemaphoreGive(lrcMutex);
  }
}

// ====================================================================
//  SERVO (pemanis): goyang saat main, diam saat berhenti
// ====================================================================
void updateServo() {
#if USE_SERVO
  if (millis() - lastServo < 25) return;
  lastServo = millis();
  if (audio.isRunning()) {
    servoAngle += servoDir * 2;
    if (servoAngle >= 150) { servoAngle = 150; servoDir = -1; }
    if (servoAngle <= 30)  { servoAngle = 30;  servoDir =  1; }
    servo.write(servoAngle);
  } else servo.write(90);
#endif
}

// ====================================================================
//  OLED
// ====================================================================
void updateDisplay() {
  if (!displayDirty && millis() - lastDisplay < 250) return;
  lastDisplay = millis();
  displayDirty = false;

  oled.clearDisplay();

  // Header: stasiun + volume
  oled.setCursor(0, 0);
  oled.print(stations[currentStation].name);
  oled.setCursor(92, 0);
  oled.print("V"); oled.print(audio.getVolume());
  oled.drawLine(0, 10, 127, 10, SSD1306_WHITE);

  if (xSemaphoreTake(lrcMutex, 10)) {
    if (lrcCount > 0) {
      // jendela 6 baris, baris aktif disorot
      int startLine = currentLine - 2;
      for (int row = 0; row < 6; row++) {
        int li = startLine + row;
        int y  = 14 + row * 8;
        if (li < 0 || li >= lrcCount) continue;
        if (li == currentLine) {
          oled.fillRect(0, y - 1, 128, 9, SSD1306_WHITE);
          oled.setTextColor(SSD1306_BLACK);
          oled.setCursor(2, y); oled.print(cut(lrcText[li]));
          oled.setTextColor(SSD1306_WHITE);
        } else {
          oled.setCursor(2, y); oled.print(cut(lrcText[li]));
        }
      }
    } else {
      // belum ada lirik -> tampilkan judul lagu
      oled.setCursor(0, 22);
      if (headerTitle.length()) {
        printWrapped(headerTitle, 22);
      } else {
        oled.print("Cari lagu...");
      }
    }
    xSemaphoreGive(lrcMutex);
  }
  oled.display();
}

String cut(const char* s) { String t = String(s); return t.substring(0, 21); }

void printWrapped(const String& t, int y) {
  for (int i = 0; i < t.length() && y < 64; i += 21) {
    oled.setCursor(0, y);
    oled.print(t.substring(i, min((int)t.length(), i + 21)));
    y += 10;
  }
}

void banner(const String& msg) {
  oled.clearDisplay();
  oled.setTextSize(1);
  int y = 24;
  int start = 0;
  while (start <= msg.length()) {
    int nl = msg.indexOf('\n', start);
    if (nl < 0) nl = msg.length();
    oled.setCursor(0, y);
    oled.print(msg.substring(start, nl));
    y += 12;
    start = nl + 1;
  }
  oled.display();
}

// ====================================================================
//  CALLBACK dari Audio (core 1)
// ====================================================================
void audio_showstreamtitle(const char* info) {
  Serial.print("[JUDUL] "); Serial.println(info);
  headerTitle = String(info);
  songStartMs = millis();          // lagu baru dianggap mulai sekarang
  lastPlainAdvance = millis();
  clearLyrics();
  displayDirty = true;

  // pisah "Artist - Title", lalu titip ke task lirik
  String s = headerTitle; s.trim();
  int sep = s.indexOf(" - ");
  String a = "", t = "";
  if (sep > 0) { a = s.substring(0, sep); t = s.substring(sep + 3); }
  else         { t = s; }
  a.trim(); t.trim();

  if (xSemaphoreTake(reqMutex, 10)) {
    a.toCharArray(reqArtist, sizeof(reqArtist));
    t.toCharArray(reqTitle,  sizeof(reqTitle));
    fetchPending = true;
    xSemaphoreGive(reqMutex);
  }
}
void audio_info(const char* info)      { Serial.print("[AUDIO] "); Serial.println(info); }
void audio_showstation(const char* s)  { Serial.print("[STASIUN] "); Serial.println(s); }

// ====================================================================
//  TASK LIRIK (core 0) — ambil & parse dari LRCLIB
// ====================================================================
void lyricsTask(void* p) {
  for (;;) {
    if (fetchPending) {
      char a[80], t[80];
      if (xSemaphoreTake(reqMutex, portMAX_DELAY)) {
        strncpy(a, reqArtist, sizeof(a));
        strncpy(t, reqTitle,  sizeof(t));
        fetchPending = false;
        xSemaphoreGive(reqMutex);
      }
      if (strlen(t) > 0) {
        bool ok = fetchLyrics(a, t);
        if (!ok && strlen(a) > 0) fetchLyrics(t, a);  // coba tukar urutan
      }
    }
    vTaskDelay(120 / portTICK_PERIOD_MS);
  }
}

bool fetchLyrics(const char* artist, const char* title) {
  Serial.printf("[LIRIK] Cari: artist='%s' judul='%s'\n", artist, title);
  String url = "https://lrclib.net/api/get?artist_name=" + urlEncode(artist) +
               "&track_name=" + urlEncode(title);

  WiFiClientSecure client;
  client.setInsecure();                 // lewati verifikasi sertifikat
  HTTPClient https;
  if (!https.begin(client, url)) { Serial.println("[LIRIK] begin() gagal"); return false; }
  https.addHeader("User-Agent", "esp32-radio-lyrics/1.0");
  int code = https.GET();
  Serial.printf("[LIRIK] HTTP code: %d\n", code);
  if (code != 200) { https.end(); Serial.println("[LIRIK] tidak ketemu / error"); return false; }

  // filter: ambil field yang perlu saja -> hemat RAM
  JsonDocument filter;
  filter["syncedLyrics"] = true;
  filter["plainLyrics"]  = true;
  filter["instrumental"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, https.getStream(), DeserializationOption::Filter(filter));
  https.end();
  if (err) { Serial.print("[LIRIK] json err: "); Serial.println(err.c_str()); return false; }

  if (doc["instrumental"] | false) { Serial.println("[LIRIK] lagu instrumental"); return false; }
  String synced = doc["syncedLyrics"] | "";
  String plain  = doc["plainLyrics"]  | "";

  if (synced.length() > 0) { Serial.println("[LIRIK] dapat lirik TERSINKRON"); parseSynced(synced); return true; }
  if (plain.length()  > 0) { Serial.println("[LIRIK] dapat lirik biasa (tanpa sync)"); parsePlain(plain); return true; }
  Serial.println("[LIRIK] kosong");
  return false;
}

// Parse "[mm:ss.xx] teks" -> isi array lirik (dengan timing)
void parseSynced(const String& lrc) {
  if (!xSemaphoreTake(lrcMutex, portMAX_DELAY)) return;
  lrcCount = 0; lrcSynced = true;

  int i = 0, n = lrc.length();
  while (i < n && lrcCount < MAX_LRC) {
    int nl = lrc.indexOf('\n', i);
    if (nl < 0) nl = n;
    String line = lrc.substring(i, nl);
    i = nl + 1;

    int lb = line.indexOf('['), rb = line.indexOf(']');
    if (lb == 0 && rb > lb) {
      String ts = line.substring(lb + 1, rb);     // mm:ss.xx
      int colon = ts.indexOf(':'), dot = ts.indexOf('.');
      if (colon > 0) {
        int mm = ts.substring(0, colon).toInt();
        int ss = ts.substring(colon + 1, dot > 0 ? dot : ts.length()).toInt();
        int cs = dot > 0 ? ts.substring(dot + 1).toInt() : 0;
        long ms = ((long)mm * 60 + ss) * 1000L + cs * 10L;
        String text = line.substring(rb + 1); text.trim();
        lrcTime[lrcCount] = ms;
        text.toCharArray(lrcText[lrcCount], LRC_LEN);
        lrcCount++;
      }
    }
  }
  if (lrcCount == 0) lrcSynced = false;
  currentLine = 0;
  xSemaphoreGive(lrcMutex);
  displayDirty = true;
  Serial.printf("lirik sync: %d baris\n", lrcCount);
}

// Lirik tanpa timing -> simpan per baris, nanti digeser pelan
void parsePlain(const String& txt) {
  if (!xSemaphoreTake(lrcMutex, portMAX_DELAY)) return;
  lrcCount = 0; lrcSynced = false;
  int i = 0, n = txt.length();
  while (i < n && lrcCount < MAX_LRC) {
    int nl = txt.indexOf('\n', i);
    if (nl < 0) nl = n;
    String line = txt.substring(i, nl); line.trim();
    i = nl + 1;
    if (line.length() == 0) continue;
    lrcTime[lrcCount] = -1;
    line.toCharArray(lrcText[lrcCount], LRC_LEN);
    lrcCount++;
  }
  currentLine = 0; lastPlainAdvance = millis();
  xSemaphoreGive(lrcMutex);
  displayDirty = true;
  Serial.printf("lirik plain: %d baris\n", lrcCount);
}

// URL-encode sederhana
String urlEncode(const char* s) {
  String out = "";
  for (int i = 0; s[i]; i++) {
    char c = s[i];
    if (isalnum(c)) out += c;
    else if (c == ' ') out += "%20";
    else { char b[4]; sprintf(b, "%%%02X", (unsigned char)c); out += b; }
  }
  return out;
}
