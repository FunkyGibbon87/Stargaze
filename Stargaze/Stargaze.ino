/*
  Stargazing Forecast - landscape, two pages (10 s toggle)
  Board: Waveshare ESP32-C6-LCD-1.47 (172x320 ST7789, used sideways: 320x172)
  Data:  Open-Meteo hourly forecast (no API key) + moon maths done on the board
         + 7Timer! astro forecast for seeing and transparency (every 3 h, optional:
           if 7Timer is down the score simply ignores those two factors)

  Page 1: title, score, verdict, best hour, 6-hour graph
  Page 2: current conditions, observations, last update / next update

  Low-heat version: dimmed PWM backlight, 80 MHz CPU, WiFi modem sleep.
  WiFi names and passwords (primary + secondary) live in the secrets.h tab.

  Non-blocking design:
   - loop() never waits: WiFi scan/connect, NTP, page toggle, redraws and clock all run off millis().
   - The HTTPS request runs in its own FreeRTOS task, so loop() stays responsive
     while the download is in progress.
*/

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <time.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "secrets.h"                   // WIFI_SSID, WIFI_PASS, WIFI_SSID2, WIFI_PASS2

// Smooth font for the big score. Looks in the GFX library first, then in the
// Adafruit GFX library. If neither has it, the big score uses the built-in blocky font.
#if __has_include("font/FreeSansBold24pt7b.h")
  #include "font/FreeSansBold24pt7b.h"
  #define BIG_FONT (&FreeSansBold24pt7b)
#elif __has_include(<Fonts/FreeSansBold24pt7b.h>)
  #include <Fonts/FreeSansBold24pt7b.h>
  #define BIG_FONT (&FreeSansBold24pt7b)
#endif

// ======================= USER SETTINGS =======================
const float LATITUDE  = -27.47;       // Brisbane. Southern hemisphere is negative.
const float LONGITUDE = 153.03;       // East is positive.
const char* TZ_INFO   = "AEST-10";    // Queensland, no daylight saving

const uint32_t REFRESH_MS      = 30UL * 60UL * 1000UL;  // fetch every 30 minutes
const uint32_t RETRY_MS        = 60UL * 1000UL;         // retry after a failed fetch
const uint32_t WIFI_RETRY_MS   = 5UL * 1000UL;          // wait between WiFi attempts
const uint32_t WIFI_TIMEOUT_MS = 15UL * 1000UL;         // give up on one WiFi attempt after this
const uint32_t PAGE_MS         = 10UL * 1000UL;         // page toggle time
const uint32_t ASTRO_REFRESH_MS = 3UL * 3600UL * 1000UL; // 7Timer seeing/transparency every 3 hours

const uint8_t SCREEN_ROTATION = 1;    // landscape: use 1, or 3 if the picture is upside down
const uint8_t BACKLIGHT_PERCENT = 15; // 0-100. Lower = cooler and easier on the eyes at night

// Scoring tweaks (see computeFactors)
const float MOON_MAX_PENALTY = 0.6f;  // full moon high in the sky removes 60% of the score
const float HIGH_CLOUD_WEIGHT = 0.8f; // how much thin high cloud counts compared with low/mid (0-1)
const float CLOUD_POWER       = 2.0f; // higher = cloud is punished harder
const float SEEING_STEP       = 0.04f; // score lost per 7Timer seeing step worse than 3 (8 = -20%)
const float TRANSP_STEP       = 0.04f; // score lost per 7Timer transparency step worse than 3 (8 = -20%)
// =============================================================

// ---------------- Display ----------------
#define TFT_BL 22
Arduino_DataBus *bus = new Arduino_ESP32SPI(15 /*DC*/, 14 /*CS*/, 7 /*SCK*/, 6 /*MOSI*/, GFX_NOT_DEFINED /*MISO*/);
Arduino_GFX *gfx = new Arduino_ST7789(bus, 21 /*RST*/, SCREEN_ROTATION, true /*IPS*/, 172, 320, 34, 0, 34, 0);

const uint16_t C_BLACK = 0x0000;
const uint16_t C_WHITE = 0xFFFF;
const uint16_t C_GREY  = 0xCE79;   // label/text grey. Brighter: 0xDEFB. Previous: 0xB5B6. Original: 0x8410
const uint16_t C_DIM   = 0x4208;
const uint16_t C_DARK  = 0x2104;
const uint16_t C_AMBER = 0xFD20;

// ---------------- Layout (landscape 320 x 172) ----------------
const int SCREEN_W     = 320;
const int SCREEN_H     = 172;
const int SHOW_HOURS   = 6;
const int FETCH_HOURS  = 12;
const int GRAPH_LEFT   = 140;
const int SLOT_W       = 30;
const int GRAPH_TOP    = 34;    // y of score 100
const int GRAPH_BOTTOM = 150;   // y of score 0
const int GRAPH_H      = GRAPH_BOTTOM - GRAPH_TOP;

// ---------------- Data types ----------------
struct HourInfo {
  time_t t;                       // start of the hour (UTC epoch)
  float lowCloud, midCloud, highCloud;
  float humidity, wind, visibility, rainProb, temp, dewPoint;
  float moonAlt;
  int seeing;                     // 7Timer 1 (best) .. 8 (worst), -1 = no data
  int transp;                     // 7Timer 1 (best) .. 8 (worst), -1 = no data
  float score;
};

struct Forecast {
  HourInfo h[FETCH_HOURS];
  int count;
  float moonIllum;                // 0..1
  time_t fetchedAt;
};

struct Factors {
  float cloud, rain, moon, humidity, wind, haze, seeing, transp;
};

struct EqCoords { double ra, dec, dist; };

enum WifiState { WS_IDLE, WS_SCANNING, WS_CONNECTING };

// ---------------- Shared state ----------------
Forecast shared;                  // written by the fetch task
Forecast current;                 // used by loop()
volatile bool newDataFlag = false;
volatile uint32_t nextFetchAtMs = 0;   // millis() time of the next fetch (0 = not known yet)
SemaphoreHandle_t dataMutex;
char statusMsg[40] = "Starting...";    // written by the fetch task (guarded by dataMutex)
char cachedStatus[40] = "Starting..."; // copy used by loop()

bool haveData = false;
bool needRedraw = false;
bool ntpStarted = false;
int page = 0;                     // 0 = graph, 1 = conditions
int lastStart = -1;
uint32_t lastTick = 0;
uint32_t lastPageSwitch = 0;

// WiFi connection state machine
WifiState wifiState = WS_IDLE;
uint32_t wifiStateSince = 0;
bool preferSecondary = false;
const char* wifiTarget = "";

// 7Timer points (only touched by the fetch task)
const int MAX_ASTRO = 30;
time_t astroT[MAX_ASTRO];
int astroSeeing[MAX_ASTRO];
int astroTransp[MAX_ASTRO];
int astroCount = 0;

// ======================= Moon maths =======================
const double D2R = M_PI / 180.0;
const double OBLIQ = D2R * 23.4397;

double daysSinceJ2000(time_t t) { return (double)t / 86400.0 - 10957.5; }

// Sun position: only needed to work out the moon phase
EqCoords sunCoords(double d) {
  double M = D2R * (357.5291 + 0.98560028 * d);
  double C = D2R * (1.9148 * sin(M) + 0.02 * sin(2 * M) + 0.0003 * sin(3 * M));
  double L = M + C + D2R * 102.9372 + M_PI;
  EqCoords c;
  c.ra   = atan2(sin(L) * cos(OBLIQ), cos(L));
  c.dec  = asin(sin(OBLIQ) * sin(L));
  c.dist = 149598000.0;
  return c;
}

EqCoords moonCoords(double d) {
  double L = D2R * (218.316 + 13.176396 * d);
  double M = D2R * (134.963 + 13.064993 * d);
  double F = D2R * (93.272 + 13.229350 * d);
  double l = L + D2R * 6.289 * sin(M);
  double b = D2R * 5.128 * sin(F);
  EqCoords c;
  c.ra   = atan2(sin(l) * cos(OBLIQ) - tan(b) * sin(OBLIQ), cos(l));
  c.dec  = asin(sin(b) * cos(OBLIQ) + cos(b) * sin(OBLIQ) * sin(l));
  c.dist = 385001.0 - 20905.0 * cos(M);
  return c;
}

double altitudeDeg(double d, EqCoords c) {
  double lw  = D2R * -LONGITUDE;
  double phi = D2R * LATITUDE;
  double st  = D2R * (280.16 + 360.9856235 * d) - lw;
  double H   = st - c.ra;
  return asin(sin(phi) * sin(c.dec) + cos(phi) * cos(c.dec) * cos(H)) / D2R;
}

double moonIllumination(double d) {
  EqCoords s = sunCoords(d), m = moonCoords(d);
  double phi = acos(sin(s.dec) * sin(m.dec) + cos(s.dec) * cos(m.dec) * cos(s.ra - m.ra));
  double inc = atan2(s.dist * sin(phi), m.dist - s.dist * cos(phi));
  return (1 + cos(inc)) / 2;
}

// ======================= Scoring (0..100) =======================
// Every factor is 0..1 and they multiply, so any single killer (overcast, rain)
// pulls the whole score down, while mild problems only trim it.
// The sun is not part of the score: it rates weather and moon conditions only.
float clamp01(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }

Factors computeFactors(const HourInfo &h, float moonIllum) {
  Factors f;

  // Cloud: low and mid cloud blocks the sky; high thin cloud counts a bit less
  float clearSky = (1 - clamp01(h.lowCloud / 100.0f)) *
                   (1 - clamp01(h.midCloud / 100.0f)) *
                   (1 - HIGH_CLOUD_WEIGHT * clamp01(h.highCloud / 100.0f));
  f.cloud = powf(clearSky, CLOUD_POWER);

  // Rain chance
  f.rain = 1.0f - 0.8f * clamp01(h.rainProb / 100.0f);

  // Moon: only hurts when it is above the horizon, more so when full and high
  f.moon = 1.0f - MOON_MAX_PENALTY * moonIllum * clamp01(h.moonAlt / 25.0f);

  // Humidity above 60% reduces transparency (up to -30%)
  f.humidity = 1.0f - 0.3f * clamp01((h.humidity - 60.0f) / 40.0f);

  // Wind above 15 km/h shakes telescopes and hurts seeing (up to -30%)
  f.wind = 1.0f - 0.3f * clamp01((h.wind - 15.0f) / 25.0f);

  // Haze/smoke/fog: visibility under 20 km trims the score (down to -40%)
  f.haze = 0.6f + 0.4f * clamp01((h.visibility - 5000.0f) / 15000.0f);

  // Seeing (steadiness of the air): 1-3 is fine, each step worse trims the score
  f.seeing = (h.seeing >= 1) ? 1.0f - SEEING_STEP * max(0, h.seeing - 3) : 1.0f;

  // Transparency (clarity of the air above us): 1-3 is fine, each step worse trims it
  f.transp = (h.transp >= 1) ? 1.0f - TRANSP_STEP * max(0, h.transp - 3) : 1.0f;

  return f;
}

float computeScore(const Factors &f) {
  return 100.0f * f.cloud * f.rain * f.moon * f.humidity * f.wind * f.haze *
         f.seeing * f.transp;
}

// Which factor is holding the score back the most?
const char* limitingFactor(const Factors &f) {
  float minV = f.cloud;
  const char* name = "Cloud";
  if (f.rain     < minV) { minV = f.rain;     name = "Rain"; }
  if (f.moon     < minV) { minV = f.moon;     name = "Moon"; }
  if (f.humidity < minV) { minV = f.humidity; name = "Humidity"; }
  if (f.wind     < minV) { minV = f.wind;     name = "Wind"; }
  if (f.haze     < minV) { minV = f.haze;     name = "Haze"; }
  if (f.seeing   < minV) { minV = f.seeing;   name = "Seeing"; }
  if (f.transp   < minV) { minV = f.transp;   name = "Transp"; }
  if (minV > 0.9f) return "None";
  return name;
}

// 7Timer scales: 1 is best, 8 is worst
const char* seeingWord(int s) {
  if (s < 1)  return "no data";
  if (s <= 2) return "Excellent";
  if (s == 3) return "Good";
  if (s <= 5) return "Fair";
  return "Poor";
}

const char* transpWord(int t) {
  if (t < 1)  return "no data";
  if (t <= 2) return "Excellent";
  if (t <= 4) return "Good";
  if (t <= 6) return "Fair";
  return "Poor";
}

const char* dewRisk(float spread) {
  if (spread < 2) return "High";
  if (spread < 4) return "Medium";
  return "Low";
}

// Verdict bands (uses the rounded score, the same number shown on screen):
//   0-24 Terrible, 25-45 Bad, 46-65 Poor, 66-79 Maybe, 80-89 Good, 90-100 Amazing!
const char* verdict(float s) {
  int r = (int)roundf(s);
  if (r < 25) return "Terrible";
  if (r <= 45) return "Bad";
  if (r <= 65) return "Poor";
  if (r <= 79) return "Maybe";
  if (r <= 89) return "Good";
  return "Amazing!";
}

uint16_t makeColor(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

// Colour scale, weighted toward the red end. The colour is blended smoothly
// between these anchor points (score, red, green, blue):
//   0-25  Terrible: one flat red   26-45 Bad: red to bright red   46-65 Poor: orange
//   66-79 Maybe: yellow            80-89 Good: green             90+   Amazing: vivid green
struct ColorStop { float score; uint8_t r, g, b; };
const ColorStop COLOR_STOPS[] = {
  {   0, 200,   0,   0 },   // red (flat from 0 to 25)
  {  25, 200,   0,   0 },   // red
  {  45, 255,  40,   0 },   // bright red
  {  58, 255, 130,   0 },   // orange
  {  68, 255, 215,   0 },   // yellow
  {  79, 210, 230,   0 },   // yellow-green
  {  84,  60, 200,  40 },   // green
  {  90,   0, 240,  90 },   // greener than green
  { 100,   0, 255, 130 }
};
const int NUM_COLOR_STOPS = sizeof(COLOR_STOPS) / sizeof(COLOR_STOPS[0]);

uint16_t scoreColor(float s) {
  if (s <= COLOR_STOPS[0].score) {
    return makeColor(COLOR_STOPS[0].r, COLOR_STOPS[0].g, COLOR_STOPS[0].b);
  }
  for (int i = 1; i < NUM_COLOR_STOPS; i++) {
    if (s <= COLOR_STOPS[i].score) {
      const ColorStop &a = COLOR_STOPS[i - 1];
      const ColorStop &b = COLOR_STOPS[i];
      float t = (s - a.score) / (b.score - a.score);
      uint8_t r  = (uint8_t)(a.r + (b.r - a.r) * t);
      uint8_t g  = (uint8_t)(a.g + (b.g - a.g) * t);
      uint8_t bl = (uint8_t)(a.b + (b.b - a.b) * t);
      return makeColor(r, g, bl);
    }
  }
  const ColorStop &last = COLOR_STOPS[NUM_COLOR_STOPS - 1];
  return makeColor(last.r, last.g, last.b);
}

// ======================= WiFi: scan, pick, connect (non-blocking) =======================
// Prefers the primary network when it is visible, uses the secondary when only
// that one is visible, and alternates between them if it can't tell (hidden SSIDs)
// or if an attempt times out.
void handleWiFi(uint32_t ms) {
  if (WiFi.status() == WL_CONNECTED) {
    wifiState = WS_IDLE;
    preferSecondary = false;          // next time, try the primary first again
    return;
  }

  switch (wifiState) {
    case WS_IDLE:
      if (ms - wifiStateSince >= WIFI_RETRY_MS) {
        WiFi.scanNetworks(true);      // async scan: returns immediately
        wifiState = WS_SCANNING;
        wifiStateSince = ms;
      }
      break;

    case WS_SCANNING: {
      int n = WiFi.scanComplete();
      if (n == WIFI_SCAN_RUNNING) {
        if (ms - wifiStateSince > WIFI_TIMEOUT_MS) {   // scan seems stuck
          WiFi.scanDelete();
          wifiState = WS_IDLE;
          wifiStateSince = ms;
        }
        break;
      }

      bool seePrimary = false, seeSecondary = false;
      for (int i = 0; i < n; i++) {   // n is negative if the scan failed, so this is skipped
        String s = WiFi.SSID(i);
        if (s == WIFI_SSID)  seePrimary = true;
        if (s == WIFI_SSID2) seeSecondary = true;
      }
      WiFi.scanDelete();

      bool useSecondary;
      if (seePrimary && !seeSecondary)      useSecondary = false;
      else if (seeSecondary && !seePrimary) useSecondary = true;
      else                                  useSecondary = preferSecondary;

      if (useSecondary) {
        wifiTarget = WIFI_SSID2;
        WiFi.begin(WIFI_SSID2, WIFI_PASS2);   // returns immediately
      } else {
        wifiTarget = WIFI_SSID;
        WiFi.begin(WIFI_SSID, WIFI_PASS);     // returns immediately
      }
      wifiState = WS_CONNECTING;
      wifiStateSince = ms;
      break;
    }

    case WS_CONNECTING:
      if (ms - wifiStateSince > WIFI_TIMEOUT_MS) {
        WiFi.disconnect();
        preferSecondary = !preferSecondary;   // next time try the other one if we can't tell
        wifiState = WS_IDLE;
        wifiStateSince = ms;
      }
      break;
  }
}

// ======================= Background fetch task =======================
void setStatus(const char* msg) {
  xSemaphoreTake(dataMutex, portMAX_DELAY);
  strlcpy(statusMsg, msg, sizeof(statusMsg));
  xSemaphoreGive(dataMutex);
}

float jf(JsonArray a, int i, float def) {
  if (i >= (int)a.size() || a[i].isNull()) return def;
  return a[i].as<float>();
}

// Days since 1970-01-01 for a UTC calendar date (no timezone involved)
long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const long era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m > 2 ? m - 3 : m + 9) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097 + (long)doe - 719468;
}

// 7Timer ASTRO product: seeing + transparency in 3-hour steps.
// Runs in the fetch task, so a slow 7Timer server never holds up loop().
bool fetch7Timer() {
  String url = "http://www.7timer.info/bin/astro.php?lon=" + String(LONGITUDE, 2) +
               "&lat=" + String(LATITUDE, 2) + "&ac=0&unit=metric&output=json&tzshift=0";

  WiFiClient client;
  HTTPClient http;
  http.useHTTP10(true);            // plain stream, no chunked encoding
  http.setTimeout(20000);          // 7Timer can be slow
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != 200) {
    Serial.printf("7Timer HTTP %d\n", code);
    http.end();
    return false;
  }

  // Only keep the fields we need
  JsonDocument filter;
  filter["init"] = true;
  filter["dataseries"][0]["timepoint"] = true;
  filter["dataseries"][0]["seeing"] = true;
  filter["dataseries"][0]["transparency"] = true;

  JsonDocument doc;
  DeserializationError err =
      deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) {
    Serial.printf("7Timer JSON: %s\n", err.c_str());
    return false;
  }

  // "init" is the model run time in UTC: "YYYYMMDDHH"
  const char* init = doc["init"] | "";
  if (strlen(init) != 10) return false;
  char b[5] = {0};
  memcpy(b, init, 4);      int y  = atoi(b);
  memset(b, 0, sizeof(b));
  memcpy(b, init + 4, 2);  int mo = atoi(b);
  memcpy(b, init + 6, 2);  int d  = atoi(b);
  memcpy(b, init + 8, 2);  int hh = atoi(b);
  time_t initT = (time_t)daysFromCivil(y, mo, d) * 86400 + hh * 3600;

  int n = 0;
  for (JsonObject p : doc["dataseries"].as<JsonArray>()) {
    if (n >= MAX_ASTRO) break;
    int tp = p["timepoint"] | 0;
    int se = p["seeing"] | -1;
    int tr = p["transparency"] | -1;
    astroT[n]      = initT + (time_t)tp * 3600;
    astroSeeing[n] = (se >= 1 && se <= 8) ? se : -1;
    astroTransp[n] = (tr >= 1 && tr <= 8) ? tr : -1;
    n++;
  }
  astroCount = n;
  Serial.printf("7Timer OK, %d points\n", n);
  return n > 0;
}

// Give an hour the nearest 7Timer reading (within 90 minutes of mid-hour)
void attachAstro(HourInfo &h) {
  h.seeing = -1;
  h.transp = -1;
  time_t mid = h.t + 1800;
  long best = 5401;
  for (int k = 0; k < astroCount; k++) {
    long dt = labs((long)(astroT[k] - mid));
    if (dt < best) {
      best = dt;
      h.seeing = astroSeeing[k];
      h.transp = astroTransp[k];
    }
  }
}

bool fetchForecast(Forecast &out) {
  String url = "https://api.open-meteo.com/v1/forecast?latitude=" + String(LATITUDE, 4) +
               "&longitude=" + String(LONGITUDE, 4) +
               "&hourly=cloud_cover_low,cloud_cover_mid,cloud_cover_high,relative_humidity_2m,"
               "wind_speed_10m,visibility,precipitation_probability,temperature_2m,dew_point_2m"
               "&forecast_hours=12&timeformat=unixtime";

  WiFiClientSecure client;
  client.setInsecure();            // skips certificate checking: fine for public weather data
  HTTPClient http;
  http.setTimeout(10000);
  if (!http.begin(client, url)) { setStatus("HTTP begin failed"); return false; }

  int code = http.GET();
  if (code != 200) {
    char b[40];
    snprintf(b, sizeof(b), "HTTP error %d", code);
    setStatus(b);
    http.end();
    return false;
  }
  String body = http.getString();
  http.end();

  JsonDocument doc;
  if (deserializeJson(doc, body)) { setStatus("JSON parse error"); return false; }

  JsonArray times = doc["hourly"]["time"];
  JsonArray low   = doc["hourly"]["cloud_cover_low"];
  JsonArray mid   = doc["hourly"]["cloud_cover_mid"];
  JsonArray high  = doc["hourly"]["cloud_cover_high"];
  JsonArray hum   = doc["hourly"]["relative_humidity_2m"];
  JsonArray wind  = doc["hourly"]["wind_speed_10m"];
  JsonArray vis   = doc["hourly"]["visibility"];
  JsonArray rain  = doc["hourly"]["precipitation_probability"];
  JsonArray temp  = doc["hourly"]["temperature_2m"];
  JsonArray dew   = doc["hourly"]["dew_point_2m"];

  if (times.size() == 0) { setStatus("No forecast data"); return false; }

  out.count = min((int)times.size(), FETCH_HOURS);
  out.fetchedAt = time(nullptr);
  out.moonIllum = (float)moonIllumination(daysSinceJ2000((time_t)times[0].as<long>() + 1800));

  for (int i = 0; i < out.count; i++) {
    HourInfo &h = out.h[i];
    h.t          = (time_t)times[i].as<long>();
    h.lowCloud   = jf(low, i, 50);
    h.midCloud   = jf(mid, i, 50);
    h.highCloud  = jf(high, i, 50);
    h.humidity   = jf(hum, i, 70);
    h.wind       = jf(wind, i, 10);
    h.visibility = jf(vis, i, 24000);
    h.rainProb   = jf(rain, i, 0);
    h.temp       = jf(temp, i, 20);
    h.dewPoint   = jf(dew, i, 15);

    double d = daysSinceJ2000(h.t + 1800);   // evaluate at the middle of the hour
    h.moonAlt = (float)altitudeDeg(d, moonCoords(d));
    attachAstro(h);
    h.score   = computeScore(computeFactors(h, out.moonIllum));
  }
  return true;
}

void fetchTask(void *param) {
  uint32_t lastTry = 0;
  uint32_t interval = 0;
  bool first = true;
  bool haveAstro = false;
  uint32_t lastAstro = 0;
  static Forecast tmp;

  for (;;) {
    bool timeOk = time(nullptr) > 1700000000;
    if (WiFi.status() == WL_CONNECTED && timeOk && (first || millis() - lastTry >= interval)) {
      first = false;
      lastTry = millis();
      setStatus("Updating...");

      // Seeing/transparency first (every 3 h). A failure here is not shown as an
      // error: the score just carries on without those two factors.
      if (!haveAstro || millis() - lastAstro >= ASTRO_REFRESH_MS) {
        if (fetch7Timer()) {
          haveAstro = true;
          lastAstro = millis();
        }
      }

      if (fetchForecast(tmp)) {
        xSemaphoreTake(dataMutex, portMAX_DELAY);
        shared = tmp;
        newDataFlag = true;
        xSemaphoreGive(dataMutex);
        setStatus("OK");
        interval = REFRESH_MS;
      } else {
        interval = RETRY_MS;
      }
      nextFetchAtMs = lastTry + interval;
    }
    vTaskDelay(pdMS_TO_TICKS(500));   // yields to loop(); does not block it
  }
}

// ======================= Status helpers =======================
void refreshStatus() {
  if (xSemaphoreTake(dataMutex, 0) == pdTRUE) {
    strlcpy(cachedStatus, statusMsg, sizeof(cachedStatus));
    xSemaphoreGive(dataMutex);
  }
}

// True if there is something worth warning about (WiFi down, fetch error)
bool problemText(char* out, size_t n) {
  if (WiFi.status() != WL_CONNECTED) { strlcpy(out, "WiFi down", n); return true; }
  if (strcmp(cachedStatus, "OK") != 0 && strcmp(cachedStatus, "Updating...") != 0) {
    strlcpy(out, cachedStatus, n);
    return true;
  }
  return false;
}

// ======================= Drawing helpers =======================
// Clears a w x 10 strip and prints one line of size-1 text in it
void drawLine(int x, int y, int w, uint16_t color, const char* txt) {
  gfx->fillRect(x, y, w, 10, C_BLACK);
  gfx->setTextSize(1);
  gfx->setTextColor(color);
  gfx->setCursor(x, y);
  gfx->print(txt);
}

// Grey label with a value next to it
void drawKV(int x, int y, int valueX, const char* label, const char* value, uint16_t valueColor) {
  gfx->setTextSize(1);
  gfx->setTextColor(C_GREY);
  gfx->setCursor(x, y);
  gfx->print(label);
  gfx->setTextColor(valueColor);
  gfx->setCursor(valueX, y);
  gfx->print(value);
}

// First hour in the forecast that hasn't finished yet
int windowStart(time_t now) {
  for (int i = 0; i < current.count; i++) {
    if (current.h[i].t + 3600 > now) return i;
  }
  return current.count;
}

void drawHeader() {
  gfx->fillRect(0, 0, SCREEN_W, 20, C_BLACK);

  // Title centred at the top (size-2 text is 12 pixels wide per character)
  const char* title = (page == 0) ? "Stargaze" : "Conditions";
  int titleW = strlen(title) * 12;
  gfx->setTextSize(2);
  gfx->setTextColor(C_WHITE);
  gfx->setCursor((SCREEN_W - titleW) / 2, 2);
  gfx->print(title);

  // page dots, left of the clock
  gfx->fillCircle(250, 9, 2, page == 0 ? C_GREY : C_DIM);
  gfx->fillCircle(260, 9, 2, page == 1 ? C_GREY : C_DIM);
}

void drawClock() {
  gfx->fillRect(SCREEN_W - 52, 4, 52, 12, C_BLACK);
  time_t now = time(nullptr);
  if (now < 1700000000) return;
  struct tm lt;
  localtime_r(&now, &lt);
  char buf[8];
  snprintf(buf, sizeof(buf), "%02d:%02d", lt.tm_hour, lt.tm_min);
  gfx->setTextSize(1);
  gfx->setTextColor(C_GREY);
  gfx->setCursor(SCREEN_W - 36, 6);
  gfx->print(buf);
}

// ======================= Page 1: graph =======================
void drawScreen1(int start) {
  int n = current.count - start;
  if (n > SHOW_HOURS) n = SHOW_HOURS;
  if (n <= 0) { drawLine(2, 60, 316, C_WHITE, "Forecast out of date"); return; }

  const HourInfo *hrs = &current.h[start];
  const HourInfo &cur = hrs[0];
  char buf[40];
  struct tm lt;

  // Big score for the current hour
  uint16_t col = scoreColor(cur.score);
  snprintf(buf, sizeof(buf), "%d", (int)roundf(cur.score));
#ifdef BIG_FONT
  // Smooth bold font. With a GFX font the cursor y is the baseline (bottom of the digits).
  gfx->setFont(BIG_FONT);
  gfx->setTextSize(1);
  gfx->setTextColor(col);
  gfx->setCursor(2, 62);
  gfx->print(buf);
  gfx->setFont((const GFXfont *)NULL);   // back to the built-in font for everything else
#else
  // Fallback: built-in blocky font
  gfx->setTextSize(5);
  gfx->setTextColor(col);
  gfx->setCursor(2, 24);
  gfx->print(buf);
#endif

  gfx->setTextSize(1);
  gfx->setTextColor(C_GREY);
  gfx->setCursor(2, 68);
  gfx->print("/100 now");

  gfx->setTextSize(2);
  gfx->setTextColor(col);
  gfx->setCursor(2, 82);
  gfx->print(verdict(cur.score));

  // Best hour in the window
  int best = 0;
  for (int i = 1; i < n; i++) if (hrs[i].score > hrs[best].score) best = i;
  localtime_r(&hrs[best].t, &lt);
  if (hrs[best].score < 25) snprintf(buf, sizeof(buf), "No good window");
  else snprintf(buf, sizeof(buf), "Best: %02dh (%d)", lt.tm_hour, (int)roundf(hrs[best].score));
  drawLine(2, 106, 112, C_WHITE, buf);

  // Dew risk and seeing for the current hour (dew turns amber when it's high)
  float spread = cur.temp - cur.dewPoint;
  snprintf(buf, sizeof(buf), "Dew risk: %s", dewRisk(spread));
  drawLine(2, 122, 112, spread < 2 ? C_AMBER : C_GREY, buf);

  snprintf(buf, sizeof(buf), "Seeing: %s", seeingWord(cur.seeing));
  drawLine(2, 135, 112, C_GREY, buf);

  // Gridlines and axis labels
  gfx->setTextSize(1);
  for (int v = 0; v <= 100; v += 25) {
    int y = GRAPH_BOTTOM - (v * GRAPH_H) / 100;
    gfx->drawFastHLine(GRAPH_LEFT, y, SLOT_W * SHOW_HOURS, C_DARK);
    snprintf(buf, sizeof(buf), "%d", v);
    gfx->setTextColor(C_GREY);
    gfx->setCursor(GRAPH_LEFT - 24, y - 3);
    gfx->print(buf);
  }

  // Bars, score values and hour labels
  for (int i = 0; i < n; i++) {
    int s = (int)roundf(hrs[i].score);
    if (s < 0) s = 0;
    if (s > 100) s = 100;
    int bh = (s * GRAPH_H) / 100;
    int x = GRAPH_LEFT + i * SLOT_W + 4;
    int w = SLOT_W - 8;
    if (bh > 0) gfx->fillRect(x, GRAPH_BOTTOM - bh, w, bh, scoreColor(s));

    snprintf(buf, sizeof(buf), "%d", s);
    int tw = strlen(buf) * 6;
    gfx->setTextColor(C_WHITE, C_BLACK);
    gfx->setCursor(x + (w - tw) / 2, GRAPH_BOTTOM - bh - 10);
    gfx->print(buf);

    localtime_r(&hrs[i].t, &lt);
    snprintf(buf, sizeof(buf), "%02dh", lt.tm_hour);
    gfx->setTextColor(C_GREY, C_BLACK);
    gfx->setCursor(x + (w - 18) / 2, GRAPH_BOTTOM + 5);
    gfx->print(buf);
  }
}

// Warning strip at the bottom left of page 1 (only shows when something is wrong)
void drawWarn() {
  char buf[40];
  if (!problemText(buf, sizeof(buf))) buf[0] = 0;
  drawLine(2, 158, 112, C_AMBER, buf);
}

// ======================= Page 2: conditions =======================
// Rows that change every second or minute: last update, next update, status
void drawUpdateRows() {
  gfx->fillRect(0, 124, SCREEN_W, SCREEN_H - 124, C_BLACK);
  char buf[64];
  time_t now = time(nullptr);

  // Last update
  struct tm lt;
  localtime_r(&current.fetchedAt, &lt);
  int ago = (int)((now - current.fetchedAt) / 60);
  if (ago < 1) snprintf(buf, sizeof(buf), "%02d:%02d (just now)", lt.tm_hour, lt.tm_min);
  else snprintf(buf, sizeof(buf), "%02d:%02d (%d min ago)", lt.tm_hour, lt.tm_min, ago);
  drawKV(4, 126, 78, "Last update", buf, C_WHITE);

  // Next update
  if (strcmp(cachedStatus, "Updating...") == 0) {
    snprintf(buf, sizeof(buf), "updating now...");
  } else if (nextFetchAtMs == 0) {
    snprintf(buf, sizeof(buf), "soon");
  } else {
    int32_t rem = (int32_t)(nextFetchAtMs - millis());
    if (rem <= 0) snprintf(buf, sizeof(buf), "due now");
    else snprintf(buf, sizeof(buf), "in %d min", (int)((rem + 59999) / 60000));
  }
  drawKV(4, 139, 78, "Next update", buf, C_WHITE);

  // Status: warning if something is wrong, otherwise WiFi network and signal strength
  char warn[40];
  if (problemText(warn, sizeof(warn))) {
    drawLine(4, 153, 312, C_AMBER, warn);
  } else {
    snprintf(buf, sizeof(buf), "WiFi: %s  %d dBm", WiFi.SSID().c_str(), (int)WiFi.RSSI());
    drawLine(4, 153, 312, C_GREY, buf);
  }
}

void drawScreen2(int start) {
  if (start >= current.count) { drawLine(2, 60, 316, C_WHITE, "Forecast out of date"); return; }

  const HourInfo &cur = current.h[start];
  Factors f = computeFactors(cur, current.moonIllum);
  char buf[40];
  const int AX = 4, AV = 70, BX = 170, BV = 236;
  const int Y0 = 26, DY = 13;

  // Column A: score, cloud, rain, humidity
  snprintf(buf, sizeof(buf), "%d  %s", (int)roundf(cur.score), verdict(cur.score));
  drawKV(AX, Y0, AV, "Score", buf, scoreColor(cur.score));

  snprintf(buf, sizeof(buf), "%d%%", (int)roundf(cur.lowCloud));
  drawKV(AX, Y0 + DY, AV, "Low cloud", buf, C_WHITE);

  snprintf(buf, sizeof(buf), "%d%%", (int)roundf(cur.midCloud));
  drawKV(AX, Y0 + 2 * DY, AV, "Mid cloud", buf, C_WHITE);

  snprintf(buf, sizeof(buf), "%d%%", (int)roundf(cur.highCloud));
  drawKV(AX, Y0 + 3 * DY, AV, "High cloud", buf, C_WHITE);

  snprintf(buf, sizeof(buf), "%d%%", (int)roundf(cur.rainProb));
  drawKV(AX, Y0 + 4 * DY, AV, "Rain", buf, C_WHITE);

  snprintf(buf, sizeof(buf), "%d%%", (int)roundf(cur.humidity));
  drawKV(AX, Y0 + 5 * DY, AV, "Humidity", buf, C_WHITE);

  // Column B: wind, moon, temperature and observations
  snprintf(buf, sizeof(buf), "%d km/h", (int)roundf(cur.wind));
  drawKV(BX, Y0, BV, "Wind", buf, C_WHITE);

  snprintf(buf, sizeof(buf), "%d%% lit, %s", (int)roundf(current.moonIllum * 100.0f),
           cur.moonAlt > 0 ? "up" : "down");
  drawKV(BX, Y0 + DY, BV, "Moon", buf, C_WHITE);

  snprintf(buf, sizeof(buf), "%d C", (int)roundf(cur.temp));
  drawKV(BX, Y0 + 2 * DY, BV, "Temp", buf, C_WHITE);

  float spread = cur.temp - cur.dewPoint;
  snprintf(buf, sizeof(buf), "%s (%d C)", dewRisk(spread), (int)roundf(spread));
  drawKV(BX, Y0 + 3 * DY, BV, "Dew risk", buf, C_WHITE);

  snprintf(buf, sizeof(buf), "%d km", (int)roundf(cur.visibility / 1000.0f));
  drawKV(BX, Y0 + 4 * DY, BV, "Visibility", buf, C_WHITE);

  drawKV(BX, Y0 + 5 * DY, BV, "Limiter", limitingFactor(f), C_WHITE);

  // Row 7: 7Timer seeing (column A) and transparency (column B)
  if (cur.seeing >= 1) snprintf(buf, sizeof(buf), "%s %d/8", seeingWord(cur.seeing), cur.seeing);
  else                 snprintf(buf, sizeof(buf), "no data");
  drawKV(AX, Y0 + 6 * DY, AV, "Seeing", buf, cur.seeing >= 1 ? C_WHITE : C_DIM);

  if (cur.transp >= 1) snprintf(buf, sizeof(buf), "%s %d/8", transpWord(cur.transp), cur.transp);
  else                 snprintf(buf, sizeof(buf), "no data");
  drawKV(BX, Y0 + 6 * DY, BV, "Transp", buf, cur.transp >= 1 ? C_WHITE : C_DIM);

  gfx->drawFastHLine(4, 120, SCREEN_W - 8, C_DARK);
  drawUpdateRows();
}

// ======================= Whole-page redraw and "no data yet" =======================
void drawPage(int start) {
  gfx->fillScreen(C_BLACK);
  drawHeader();
  if (page == 0) drawScreen1(start);
  else drawScreen2(start);
  drawClock();
}

void drawNoData() {
  char buf[64];
  if (WiFi.status() != WL_CONNECTED) {
    if (wifiState == WS_SCANNING)        strlcpy(buf, "Searching for WiFi...", sizeof(buf));
    else if (wifiState == WS_CONNECTING) snprintf(buf, sizeof(buf), "Connecting to %s...", wifiTarget);
    else                                 strlcpy(buf, "WiFi connecting...", sizeof(buf));
  } else if (time(nullptr) < 1700000000) {
    strlcpy(buf, "Syncing clock...", sizeof(buf));
  } else {
    strlcpy(buf, cachedStatus, sizeof(buf));
  }
  drawLine(2, 70, 316, C_GREY, buf);
}

// ======================= Arduino entry points =======================
void setup() {
  Serial.begin(115200);

  setCpuFrequencyMhz(80);               // plenty for this job, and runs cooler than 160 MHz

  if (!gfx->begin()) Serial.println("gfx begin failed");
  gfx->fillScreen(C_BLACK);
  drawHeader();

  // Dimmed PWM backlight (5 kHz, 8-bit). Switched on after the screen is cleared.
  ledcAttach(TFT_BL, 5000, 8);
  ledcWrite(TFT_BL, (255 * BACKLIGHT_PERCENT) / 100);

  dataMutex = xSemaphoreCreateMutex();

  WiFi.mode(WIFI_STA);
  WiFi.setSleep(true);                  // modem sleep between WiFi packets
  WiFi.setAutoReconnect(false);         // handleWiFi() manages connecting and reconnecting
  wifiStateSince = millis() - WIFI_RETRY_MS;   // start the first scan straight away

  xTaskCreate(fetchTask, "fetch", 16384, NULL, 1, NULL);
}

void loop() {
  uint32_t ms = millis();

  // 1. Scan for / connect to WiFi (primary preferred, secondary as fallback)
  handleWiFi(ms);

  // 2. Start the clock sync once WiFi is up (runs in the background)
  if (!ntpStarted && WiFi.status() == WL_CONNECTED) {
    configTzTime(TZ_INFO, "pool.ntp.org", "time.google.com");
    ntpStarted = true;
  }

  // 3. Pick up a new forecast from the fetch task, if there is one
  if (newDataFlag && xSemaphoreTake(dataMutex, 0) == pdTRUE) {
    current = shared;
    newDataFlag = false;
    xSemaphoreGive(dataMutex);
    if (!haveData) lastPageSwitch = ms;   // start the 10 s page timer at first data
    haveData = true;
    needRedraw = true;
  }

  // 4. Once a second: toggle page, slide the 6-hour window, redraw, update clock and status
  if (ms - lastTick >= 1000) {
    lastTick = ms;
    time_t now = time(nullptr);
    refreshStatus();

    if (haveData && now > 1700000000) {
      if (ms - lastPageSwitch >= PAGE_MS) {
        page = 1 - page;
        lastPageSwitch = ms;
        needRedraw = true;
      }
      int start = windowStart(now);
      if (needRedraw || start != lastStart) {
        lastStart = start;
        needRedraw = false;
        drawPage(start);                 // full redraw, includes the clock
      } else {
        drawClock();
        if (page == 0) drawWarn();
        else drawUpdateRows();
      }
    } else {
      drawClock();
      drawNoData();
    }
  }
}