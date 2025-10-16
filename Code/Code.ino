/*
   - speed calculation between consecutive IR sensors (distance = 0.1 m)
   - sending update to Firebase RTDB every 5s
*/

#include <WiFi.h>
#include <HTTPClient.h>
#include <time.h>

// ---------- CONFIG ----------
const char* WIFI_SSID = "ZSRR";
const char* WIFI_PASS = "34567890";
//const char* WIFI_PASS = "34567890";

//Put your coords here (decimal)

// const float LAT = 23.8103;    // example: Dhaka
// const float LON = 90.4125;    // example: Dhaka

const float LAT = 51.5074;    // London latitude
const float LON = -0.1278;    // London longitude

// timezone offset in hours from UTC (e.g., Bangladesh = +6)
// const int TZ_OFFSET_HOURS = 6; // for BD 
const int TZ_OFFSET_HOURS = 0; // for London 


// Pins (change if needed)
const int IR_PINS[4]    = {14, 27, 12, 13};
const int RELAY_PINS[4] = {26, 25, 33, 32};

// logic config -- adjust if your modules behave differently
const bool IR_ACTIVE_HIGH = false;   // set true if IR OUT = HIGH when motion
const bool RELAY_ACTIVE_LOW = false; // set true if relay is activated by LOW on IN

// Hold time after last detected motion (milliseconds)
const unsigned long HOLD_MS = 10000UL; // 10 seconds

// How often to refresh sunrise/sunset (ms)
const unsigned long SUN_REFRESH_MS = 10UL * 60UL * 1000UL; // every 10 minutes

// Firebase RTDB (no auth token added here)
const char* FIREBASE_DB = "https://embedded-be95a-default-rtdb.asia-southeast1.firebasedatabase.app";
const unsigned long FIREBASE_SEND_MS = 1000UL; // send update every 1 second

// distance between consecutive IR sensors in meters
const float DIST_BETWEEN_SENSORS_M = 0.10f; // 10 cm -> 0.1 m

// ---------- state ----------
unsigned long lastMotionTs[4] = {0,0,0,0};
bool relayState[4] = {false,false,false,false};

unsigned long lastSunFetch = 0;
int sunriseMinutesLocal = 6*60; // default sunrise 06:00
int sunsetMinutesLocal  = 18*60; // default sunset 18:00

// ----- additional state for speed calc & firebase -----
bool prevIRState[4] = {false,false,false,false}; // for rising-edge detection
unsigned long triggerTime[4] = {0,0,0,0}; // rising-edge times
float lastSpeeds_m_s[3] = {0.0f, 0.0f, 0.0f}; // speed between pairs: 0-1,1-2,2-3 (m/s)
unsigned long lastFirebaseSend = 0;

// helper to set relay considering active-low modules
void setRelay(int idx, bool on) {
  relayState[idx] = on;
  if (RELAY_ACTIVE_LOW) digitalWrite(RELAY_PINS[idx], on ? LOW : HIGH);
  else digitalWrite(RELAY_PINS[idx], on ? HIGH : LOW);
}

// read IR considering active-high/low
bool readIR(int idx) {
  int v = digitalRead(IR_PINS[idx]);
  return IR_ACTIVE_HIGH ? (v == HIGH) : (v == LOW); // true => motion
}

// ---------- networking helpers ----------
String buildApiUrl() {
  String url = "https://api.sunrise-sunset.org/json?lat=";
  url += String(LAT, 6);
  url += "&lng=";
  url += String(LON, 6);
  url += "&formatted=0"; // get ISO 8601 UTC
  return url;
}

// parse "2025-10-12T17:32:00+00:00" -> hour and minute as ints (UTC)
bool parseISOTimeHM(const String &iso, int &h, int &m) {
  int t = iso.indexOf('T');
  if (t < 0) return false;
  String timepart = iso.substring(t + 1); // "17:32:00+00:00" or "17:32:00Z"
  // first two chars = hour, next 3..4 = minute
  if (timepart.length() < 5) return false;
  h = timepart.substring(0,2).toInt();
  m = timepart.substring(3,5).toInt();
  return true;
}

// fetch sunrise/sunset and convert to local minutes since midnight
void fetchSunTimes() {
  if (WiFi.status() != WL_CONNECTED) return;

  String url = buildApiUrl();
  HTTPClient http;
  http.begin(url);
  int code = http.GET();
  if (code != 200) {
    Serial.printf("Sun API HTTP error: %d\n", code);
    http.end();
    return;
  }
  String payload = http.getString();
  http.end();

  // very small parsing to extract "sunrise":"...","sunset":"..."
  int idxSunrise = payload.indexOf("\"sunrise\"");
  int idxSunset  = payload.indexOf("\"sunset\"");
  if (idxSunrise < 0 || idxSunset < 0) {
    Serial.println("Sun API response parse error");
    return;
  }

  int q1 = payload.indexOf('"', idxSunrise + 10); // first quote after key
  int q2 = payload.indexOf('"', q1 + 1);
  String sunriseISO = payload.substring(q1 + 1, q2);

  int q3 = payload.indexOf('"', idxSunset + 9);
  int q4 = payload.indexOf('"', q3 + 1);
  String sunsetISO = payload.substring(q3 + 1, q4);

  int sHourUtc = 0, sMinUtc = 0;
  int setHourUtc = 0, setMinUtc = 0;
  if (!parseISOTimeHM(sunriseISO, sHourUtc, sMinUtc) || !parseISOTimeHM(sunsetISO, setHourUtc, setMinUtc)) {
    Serial.println("Failed to parse sunrise/sunset strings.");
    return;
  }

  // convert UTC hour -> local hour by adding TZ offset
  int sunriseLocalHour = sHourUtc + TZ_OFFSET_HOURS;
  int sunsetLocalHour  = setHourUtc + TZ_OFFSET_HOURS;

  // wraparound
  if (sunriseLocalHour >= 24) sunriseLocalHour -= 24;
  if (sunriseLocalHour < 0) sunriseLocalHour += 24;
  if (sunsetLocalHour >= 24) sunsetLocalHour -= 24;
  if (sunsetLocalHour < 0) sunsetLocalHour += 24;

  sunriseMinutesLocal = sunriseLocalHour * 60 + sMinUtc; // (minute same; care if TZ offset changed hour already)
  sunsetMinutesLocal  = sunsetLocalHour * 60 + setMinUtc;

  // normalize minutes to 0..1439
  sunriseMinutesLocal = (sunriseMinutesLocal % (24*60) + (24*60)) % (24*60);
  sunsetMinutesLocal  = (sunsetMinutesLocal % (24*60) + (24*60)) % (24*60);

  Serial.printf("Sunrise local: %02d:%02d  Sunset local: %02d:%02d\n",
    sunriseLocalHour, sMinUtc, sunsetLocalHour, setMinUtc);
}

// Returns minutes since local midnight using getLocalTime()
int currentLocalMinutes() {
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo)) {
    // fallback: use millis (not great)
    unsigned long ms = millis();
    unsigned long minutes = (ms / 60000) % (24*60);
    return (int)minutes;
  }
  return timeinfo.tm_hour * 60 + timeinfo.tm_min;
}

bool isNightNow() {
  int now = currentLocalMinutes();
  int s = sunriseMinutesLocal;
  int e = sunsetMinutesLocal;
  // night if now >= sunset OR now < sunrise
  if (e <= s) {
    // normal: e (sunset) > s (sunrise) (same day)
    // but if weird values, fallback
  }
  return (now >= e) || (now < s);
}

// ---------- set up ----------
void setup() {
  Serial.begin(115200);
  delay(500);

  // init pins
  for (int i=0;i<4;i++) {
    pinMode(IR_PINS[i], INPUT);
    pinMode(RELAY_PINS[i], OUTPUT);
    // set relays off initially
    if (RELAY_ACTIVE_LOW) digitalWrite(RELAY_PINS[i], HIGH);
    else digitalWrite(RELAY_PINS[i], LOW);
  }

  // connect WiFi
  Serial.printf("Connecting to WiFi %s\n", WIFI_SSID);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  unsigned long wifiStart = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - wifiStart < 20000) {
    delay(300);
    Serial.print(".");
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("WiFi connected.");
  } else {
    Serial.println("WiFi connection failed (will retry later).");
  }

  // set system time using NTP so local time functions work
  // you can set timezone offset as hours (TZ_OFFSET_HOURS)
  configTime(TZ_OFFSET_HOURS * 3600, 0, "pool.ntp.org", "time.nist.gov");
  // fetch initial sun times
  fetchSunTimes();
  lastSunFetch = millis();

  // initialize prevIRState from current readings
  for (int i=0;i<4;i++) prevIRState[i] = readIR(i);
}

// ---------- helper: send JSON to Firebase (PATCH) ----------
void sendFirebaseUpdate(bool nightNow) {
  if (WiFi.status() != WL_CONNECTED) return;

  // build JSON manually (no ArduinoJson)
  // include timestamp (millis), night, relays array, speeds_m_s array (3 elements)
  unsigned long ts = millis();

  String url = String(FIREBASE_DB) + "/street.json"; // using "street" node
  HTTPClient http;
  http.begin(url);
  http.addHeader("Content-Type", "application/json; charset=utf-8");

  String json = "{";
  json += "\"timestamp_ms\":";
  json += String(ts);
  json += ",\"night\":";
  json += (nightNow ? "true" : "false");

  // relays
  json += ",\"relays\":[";
  for (int i=0;i<4;i++) {
    json += (relayState[i] ? "true" : "false");
    if (i < 3) json += ",";
  }
  json += "]";

  // speeds (3 values)
  json += ",\"speeds_m_s\":[";
  for (int i=0;i<3;i++) {
    // use 3 decimal places
    json += String(lastSpeeds_m_s[i], 3);
    if (i < 2) json += ",";
  }
  json += "]";

  json += "}";

  int httpCode = http.PATCH(json); // use PATCH to update node
  if (httpCode > 0) {
    String resp = http.getString();
    Serial.printf("Firebase update HTTP %d\n", httpCode);
    // optionally print response for debugging:
    // Serial.println(resp);
  } else {
    Serial.printf("Firebase update failed, error: %s\n", http.errorToString(httpCode).c_str());
  }
  http.end();
}

// ---------- main loop ----------
void loop() {
  // update sun times periodically
  if (millis() - lastSunFetch > SUN_REFRESH_MS) {
    fetchSunTimes();
    lastSunFetch = millis();
  }

  bool night = isNightNow();
  static bool prevNight = false;
  if (night != prevNight) {
    Serial.printf("isNight? %d\n", night);
    prevNight = night;
  }

  // --- read sensors, preserve your existing motion -> relay logic ---
  for (int i=0;i<4;i++) { 
    bool motion = readIR(i); // true if detected

    // rising-edge detection for triggerTime / speed calculation ---
    if (motion && !prevIRState[i]) {
      // rising edge for sensor i
      unsigned long now = millis();
      triggerTime[i] = now;
      Serial.printf("Sensor %d rising at %lu\n", i+1, now);

      // compute speed between previous sensor (i-1) -> i
      if (i > 0 && triggerTime[i-1] != 0) {
        long deltaMs = (long)(triggerTime[i] - triggerTime[i-1]);
        if (deltaMs > 0 && deltaMs < 20000) { // ignore impossible values (>20s)
          float deltaS = deltaMs / 1000.0f;
          float speed_m_s = DIST_BETWEEN_SENSORS_M / deltaS;
          lastSpeeds_m_s[i-1] = speed_m_s;
          Serial.printf("Speed between %d and %d: %.3f m/s (dt=%ld ms)\n", i, i+1, speed_m_s, deltaMs);
        }
      }
    }

    // save prev state for next loop
    prevIRState[i] = motion;

    // === your original behavior (unchanged except uses motion variable) ===
    if (night && motion) {
      lastMotionTs[i] = millis();
      if (!relayState[i]) {
        setRelay(i, true);
        Serial.printf("CH%d: motion -> relay ON\n", i+1);
      }
    }

    // if relay is on, check hold time
    if (relayState[i]) {
      if (!night) {
        // Daytime: force off
        setRelay(i, false);
        Serial.printf("CH%d: daytime -> relay OFF\n", i+1);
      } else {
        if (millis() - lastMotionTs[i] >= HOLD_MS) {
          setRelay(i, false);
          Serial.printf("CH%d: hold expired -> relay OFF\n", i+1);
        }
      }
    }
  }

  //  periodic firebase update every FIREBASE_SEND_MS ---
  if (millis() - lastFirebaseSend >= FIREBASE_SEND_MS) {
    lastFirebaseSend = millis();
    sendFirebaseUpdate(night);
  }

  delay(80); // small debounce/poll delay
}