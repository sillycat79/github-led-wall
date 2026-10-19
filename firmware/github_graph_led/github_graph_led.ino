// github led wall ✿
// my github contribution graph on a 64x32 HUB75 LED matrix (Waveshare RGB-Matrix-P5-64x32)
//
// board: "ESP32 Dev Module" (original ESP32 / ESP32-WROOM-32, NOT an ESP32-C3)
// libraries (Arduino Library Manager):
//   - ESP32 HUB75 LED MATRIX PANEL DMA Display  (by mrcodetastic)
//   - Adafruit GFX Library
//   - ArduinoJson (v7)
//
// your wifi + github info lives in secrets.h (copy secrets.example.h → secrets.h).
// secrets.h is gitignored so it never ends up on github.
//
// layout on the panel:
//   rows  3-7   "100 this year"
//   rows 11-17  the 53x7 contribution grid
//   rows 22-26  month initials (J F M A ...) like github's header

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Fonts/TomThumb.h>
#include <time.h>
#include "secrets.h"   // WIFI_SSID, WIFI_PASS, GH_USER, GH_TOKEN

// ---------- settings ----------
const unsigned long REFRESH_MS = 5UL * 60UL * 1000UL;  // check github every 5 min
const uint8_t BRIGHTNESS = 60;                         // 0-255, 40-80 is plenty indoors

// ---------- panel ----------
#define PANEL_W 64
#define PANEL_H 32
#define GRID_Y  11
#define TEXT_BASELINE 7
#define MONTH_BASELINE 27

MatrixPanel_I2S_DMA* display = nullptr;
unsigned long lastFetch = 0;

// leds are way punchier than screens, so these are tuned
// darker/brighter than github's hex values. tweak to taste
uint16_t C_EMPTY, C1, C2, C3, C4, C_TEXT, C_DIM, C_ERR;

uint16_t levelColor(const char* lvl) {
  if (!lvl) return C_EMPTY;
  if (!strcmp(lvl, "FIRST_QUARTILE"))  return C1;
  if (!strcmp(lvl, "SECOND_QUARTILE")) return C2;
  if (!strcmp(lvl, "THIRD_QUARTILE"))  return C3;
  if (!strcmp(lvl, "FOURTH_QUARTILE")) return C4;
  return C_EMPTY;
}

// tiny red dot in the top-right corner = last refresh failed (old graph stays up)
void showError(bool on) {
  display->drawPixel(PANEL_W - 1, 0, on ? C_ERR : 0);
}

void connectWiFi() {
  if (WiFi.status() == WL_CONNECTED) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 20000) delay(250);
}

bool fetchAndDraw() {
  connectWiFi();
  if (WiFi.status() != WL_CONNECTED) { showError(true); return false; }

  WiFiClientSecure client;
  client.setInsecure();  // skips the TLS cert check, fine for a wall toy

  HTTPClient http;
  http.useHTTP10(true);  // no chunked encoding, so ArduinoJson can read the stream
  http.begin(client, "https://api.github.com/graphql");
  http.addHeader("Authorization", String("bearer ") + GH_TOKEN);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("User-Agent", "esp32-github-led-wall");

  String body = String("{\"query\":\"query{user(login:\\\"") + GH_USER +
    "\\\"){contributionsCollection{contributionCalendar{totalContributions "
    "weeks{contributionDays{contributionLevel weekday date}}}}}}\"}";

  int code = http.POST(body);
  if (code != 200) {
    Serial.printf("github HTTP %d\n", code);
    http.end(); showError(true); return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();
  if (err) { Serial.printf("json error: %s\n", err.c_str()); showError(true); return false; }

  JsonObject cal = doc["data"]["user"]["contributionsCollection"]["contributionCalendar"];
  if (cal.isNull()) { Serial.println("no data, check username/token"); showError(true); return false; }

  int total = cal["totalContributions"] | 0;
  JsonArray weeks = cal["weeks"];
  int nWeeks = weeks.size();                 // usually 53
  int x0 = (PANEL_W - nWeeks) / 2;           // center the grid

  display->clearScreen();

  // --- header text ---
  char title[24];
  snprintf(title, sizeof(title), "%d this year", total);
  display->setFont(&TomThumb);
  display->setTextColor(C_TEXT);
  int16_t bx, by; uint16_t bw, bh;
  display->getTextBounds(title, 0, TEXT_BASELINE, &bx, &by, &bw, &bh);
  display->setCursor((PANEL_W - bw) / 2, TEXT_BASELINE);
  display->print(title);

  // --- grid + month labels ---
  const char* MONTHS = "JFMAMJJASOND";
  int prevMonth = -1;
  int lastLabelX = -10;
  display->setTextColor(C_DIM);

  int w = 0;
  for (JsonObject week : weeks) {
    JsonArray days = week["contributionDays"];
    int x = x0 + w;

    for (JsonObject day : days) {
      int wd = day["weekday"] | 0;            // 0 = sunday, same as github
      display->drawPixel(x, GRID_Y + wd, levelColor(day["contributionLevel"]));
    }

    // label the first week of each month (skip if it would bump into the last one)
    const char* date = days[0]["date"];       // "2026-01-04"
    if (date && strlen(date) >= 7) {
      int month = (date[5] - '0') * 10 + (date[6] - '0') - 1;
      if (month != prevMonth && prevMonth != -1 && x - lastLabelX >= 4) {
        display->setCursor(x, MONTH_BASELINE);
        display->print(MONTHS[month]);
        lastLabelX = x;
      }
      prevMonth = month;
    }
    w++;
  }

  showError(false);
  Serial.printf("drew %d weeks, %d contributions\n", nWeeks, total);
  return true;
}

void setup() {
  Serial.begin(115200);

  // default pins (see assets/pinmap.svg):
  // R1 25, G1 26, B1 27, R2 14, G2 12, B2 13,
  // A 23, B 19, C 5, D 17, LAT 4, OE 15, CLK 16
  HUB75_I2S_CFG cfg(PANEL_W, PANEL_H, 1);
  cfg.clkphase = false;  // flip to true if the image looks shifted
  display = new MatrixPanel_I2S_DMA(cfg);
  display->begin();
  display->setBrightness8(BRIGHTNESS);
  display->clearScreen();

  C_EMPTY = display->color565(10, 12, 14);
  C1      = display->color565(0, 45, 15);
  C2      = display->color565(0, 95, 30);
  C3      = display->color565(20, 165, 50);
  C4      = display->color565(60, 255, 90);
  C_TEXT  = display->color565(200, 210, 220);
  C_DIM   = display->color565(70, 75, 85);
  C_ERR   = display->color565(255, 0, 0);

  // boot message
  display->setFont(&TomThumb);
  display->setTextColor(C_DIM);
  display->setCursor(14, 18);
  display->print("loading...");

  connectWiFi();
  configTzTime("EST5EDT,M3.2.0,M11.1.0", "pool.ntp.org");  // toronto time
  fetchAndDraw();
  lastFetch = millis();
}

void loop() {
  if (millis() - lastFetch >= REFRESH_MS) {
    fetchAndDraw();
    lastFetch = millis();
  }
  delay(1000);
}
