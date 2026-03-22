#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <TFT_eSPI.h>

TFT_eSPI tft = TFT_eSPI();
ESP8266WebServer server(80);

const char* FW_VERSION = "1.1.3";
const char* CONFIG_FILE = "/config.json";
const char* AP_SSID = "F1-Display-Setup";
const char* HOSTNAME = "f1-display";

const char* driverStandingsUrl      = "https://api.jolpi.ca/ergast/f1/current/driverStandings.json";
const char* constructorStandingsUrl = "https://api.jolpi.ca/ergast/f1/current/constructorStandings.json";
const char* nextRaceUrl             = "https://api.jolpi.ca/ergast/f1/current/next.json";

const int MAX_DRIVERS = 30;
const int MAX_CONSTRUCTORS = 20;
const int MAX_RACE_EVENTS = 10;

const int SCREEN_W = 320;
const int SCREEN_H = 240;

const int HEADER_H = 26;
const int TITLE_H = 18;
const int TABLE_HEADER_H = 18;
const int FOOTER_H = 18;
const int CONTENT_TOP = HEADER_H + TITLE_H + TABLE_HEADER_H + 2;
const int CONTENT_BOTTOM = SCREEN_H - FOOTER_H;
const int CONTENT_H = CONTENT_BOTTOM - CONTENT_TOP;

const int DRIVER_ROWS_PER_PAGE = 12;
const int CONSTRUCTOR_ROWS_PER_PAGE = 12;
const int MAX_VISIBLE_RACE_EVENTS = 8;

const unsigned long WIFI_RETRY_WINDOW_MS = 30000UL;
const int WIFI_RETRY_COUNT = 3;

uint16_t COL_BG;
uint16_t COL_HEADER_BG;
uint16_t COL_TITLE;
uint16_t COL_LABEL;
uint16_t COL_TEXT;
uint16_t COL_HIGHLIGHT;
uint16_t COL_ALERT;
uint16_t COL_LINE;
uint16_t COL_FOOTER_BG;

struct AppConfig {
  String wifiSsid;
  String wifiPassword;
  String timezoneKey;
  String timezoneTz;
  uint16_t pageIntervalSec;
  uint16_t refreshIntervalMin;
  bool clockFormat24;
};

struct DriverStanding {
  int position;
  String surname;
  String team;
  String points;
};

struct ConstructorStanding {
  int position;
  String name;
  String points;
};

struct RaceEvent {
  bool present;
  String label;
  String date;
  String time;
  long long epochUtc;
};

struct NextRaceInfo {
  bool valid;
  String raceName;
  String circuitName;
  String locality;
  String country;
  String round;
  RaceEvent events[MAX_RACE_EVENTS];
  int eventCount;
};

DriverStanding drivers[MAX_DRIVERS];
ConstructorStanding constructors[MAX_CONSTRUCTORS];
NextRaceInfo nextRace;
AppConfig config;

int driverCount = 0;
int constructorCount = 0;

String lastRefreshText = "NEVER";
String ipAddressText = "0.0.0.0";

unsigned long pageIntervalMs = 8000UL;
unsigned long refreshIntervalMs = 900000UL;

enum PageType {
  PAGE_DRIVERS_1,
  PAGE_DRIVERS_2,
  PAGE_CONSTRUCTORS,
  PAGE_NEXT_RACE
};

PageType currentPage = PAGE_DRIVERS_1;

unsigned long lastPageSwitch = 0;
unsigned long lastRefresh = 0;

bool inApMode = false;
bool webServerStarted = false;
bool mdnsStarted = false;

struct TimezoneOption {
  const char* key;
  const char* label;
  const char* tz;
};

TimezoneOption timezoneOptions[] = {
  {"GMT", "GMT / BST", "GMT0BST,M3.5.0/1,M10.5.0/2"},
  {"CET", "CET / CEST", "CET-1CEST,M3.5.0/2,M10.5.0/3"}
};

const int TIMEZONE_COUNT = sizeof(timezoneOptions) / sizeof(timezoneOptions[0]);

void logLine(const String& s) {
  Serial.println(s);
}

String htmlEscape(const String& s) {
  String out = s;
  out.replace("&", "&amp;");
  out.replace("<", "&lt;");
  out.replace(">", "&gt;");
  out.replace("\"", "&quot;");
  return out;
}

String normalizeToAscii(String input) {
  input.replace("Á", "A"); input.replace("À", "A"); input.replace("Â", "A"); input.replace("Ä", "A"); input.replace("Ã", "A"); input.replace("Å", "A");
  input.replace("Æ", "AE"); input.replace("Ç", "C");
  input.replace("É", "E"); input.replace("È", "E"); input.replace("Ê", "E"); input.replace("Ë", "E");
  input.replace("Í", "I"); input.replace("Ì", "I"); input.replace("Î", "I"); input.replace("Ï", "I");
  input.replace("Ñ", "N");
  input.replace("Ó", "O"); input.replace("Ò", "O"); input.replace("Ô", "O"); input.replace("Ö", "O"); input.replace("Õ", "O"); input.replace("Ø", "O");
  input.replace("Ú", "U"); input.replace("Ù", "U"); input.replace("Û", "U"); input.replace("Ü", "U");
  input.replace("Ý", "Y"); input.replace("ß", "SS");

  input.replace("á", "A"); input.replace("à", "A"); input.replace("â", "A"); input.replace("ä", "A"); input.replace("ã", "A"); input.replace("å", "A");
  input.replace("æ", "AE"); input.replace("ç", "C");
  input.replace("é", "E"); input.replace("è", "E"); input.replace("ê", "E"); input.replace("ë", "E");
  input.replace("í", "I"); input.replace("ì", "I"); input.replace("î", "I"); input.replace("ï", "I");
  input.replace("ñ", "N");
  input.replace("ó", "O"); input.replace("ò", "O"); input.replace("ô", "O"); input.replace("ö", "O"); input.replace("õ", "O"); input.replace("ø", "O");
  input.replace("ú", "U"); input.replace("ù", "U"); input.replace("û", "U"); input.replace("ü", "U");
  input.replace("ý", "Y"); input.replace("ÿ", "Y");
  return input;
}

String clipText(String text, int maxChars) {
  if ((int)text.length() <= maxChars) return text;
  if (maxChars <= 1) return text.substring(0, maxChars);
  return text.substring(0, maxChars - 1) + ".";
}

String trimCopy(String value) {
  value.trim();
  return value;
}

String timezoneLabelFromKey(const String& key) {
  for (int i = 0; i < TIMEZONE_COUNT; i++) {
    if (key == timezoneOptions[i].key) return String(timezoneOptions[i].label);
  }
  return "Unknown";
}

void setDefaultConfig() {
  config.wifiSsid = "";
  config.wifiPassword = "";
  config.timezoneKey = "GMT";
  config.timezoneTz = "GMT0BST,M3.5.0/1,M10.5.0/2";
  config.pageIntervalSec = 8;
  config.refreshIntervalMin = 15;
  config.clockFormat24 = true;
}

bool applyTimezoneFromKey(const String& key) {
  for (int i = 0; i < TIMEZONE_COUNT; i++) {
    if (key == timezoneOptions[i].key) {
      config.timezoneKey = timezoneOptions[i].key;
      config.timezoneTz = timezoneOptions[i].tz;
      return true;
    }
  }
  return false;
}

void applyRuntimeConfig() {
  if (config.pageIntervalSec < 3) config.pageIntervalSec = 3;
  if (config.pageIntervalSec > 60) config.pageIntervalSec = 60;
  if (config.refreshIntervalMin < 1) config.refreshIntervalMin = 1;
  if (config.refreshIntervalMin > 120) config.refreshIntervalMin = 120;

  pageIntervalMs = (unsigned long)config.pageIntervalSec * 1000UL;
  refreshIntervalMs = (unsigned long)config.refreshIntervalMin * 60000UL;

  setenv("TZ", config.timezoneTz.c_str(), 1);
  tzset();

  logLine("Runtime config applied");
  logLine("Timezone: " + config.timezoneKey);
  logLine("Page interval sec: " + String(config.pageIntervalSec));
  logLine("Refresh interval min: " + String(config.refreshIntervalMin));
  logLine("Clock format: " + String(config.clockFormat24 ? "24h" : "12h"));
}

bool saveConfig() {
  DynamicJsonDocument doc(1024);
  doc["wifiSsid"] = config.wifiSsid;
  doc["wifiPassword"] = config.wifiPassword;
  doc["timezoneKey"] = config.timezoneKey;
  doc["timezoneTz"] = config.timezoneTz;
  doc["pageIntervalSec"] = config.pageIntervalSec;
  doc["refreshIntervalMin"] = config.refreshIntervalMin;
  doc["clockFormat24"] = config.clockFormat24;

  File f = LittleFS.open(CONFIG_FILE, "w");
  if (!f) {
    logLine("Failed to open config for write");
    return false;
  }

  size_t written = serializeJson(doc, f);
  f.close();

  if (written == 0) {
    logLine("Failed to serialize config");
    return false;
  }

  logLine("Config saved successfully");
  return true;
}

bool loadConfig() {
  if (!LittleFS.exists(CONFIG_FILE)) {
    logLine("Config file not found, using defaults");
    setDefaultConfig();
    return false;
  }

  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) {
    logLine("Failed to open config file, using defaults");
    setDefaultConfig();
    return false;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    logLine("Config parse failed, using defaults");
    setDefaultConfig();
    return false;
  }

  config.wifiSsid = String(doc["wifiSsid"] | "");
  config.wifiPassword = String(doc["wifiPassword"] | "");
  config.timezoneKey = String(doc["timezoneKey"] | "GMT");
  config.timezoneTz = String(doc["timezoneTz"] | "GMT0BST,M3.5.0/1,M10.5.0/2");
  config.pageIntervalSec = doc["pageIntervalSec"] | 8;
  config.refreshIntervalMin = doc["refreshIntervalMin"] | 15;
  config.clockFormat24 = doc["clockFormat24"] | true;

  if (!applyTimezoneFromKey(config.timezoneKey)) {
    logLine("Invalid timezone in config, reverting to defaults");
    setDefaultConfig();
  }

  applyRuntimeConfig();
  logLine("Config loaded successfully");
  return true;
}

void clearConfigFile() {
  if (LittleFS.exists(CONFIG_FILE)) {
    LittleFS.remove(CONFIG_FILE);
    logLine("Config file removed");
  }
}

String formatClockShortLocal() {
  time_t raw = time(nullptr);
  struct tm* local = localtime(&raw);
  if (local == nullptr) return "--:--";

  char buffer[12];

  if (config.clockFormat24) {
    strftime(buffer, sizeof(buffer), "%H:%M", local);
    return String(buffer);
  } else {
    strftime(buffer, sizeof(buffer), "%I:%M %p", local);
    String s = String(buffer);
    if (s.startsWith("0")) s.remove(0, 1);
    return s;
  }
}

String formatEpochDisplay(long long epochUtc) {
  time_t raw = (time_t)epochUtc;
  struct tm* local = localtime(&raw);
  if (local == nullptr) return "UNKNOWN";

  char buffer[24];
  if (config.clockFormat24) {
    strftime(buffer, sizeof(buffer), "%d/%m/%Y %H%M", local);
    return String(buffer);
  } else {
    strftime(buffer, sizeof(buffer), "%d/%m/%Y %I:%M %p", local);
    String s = String(buffer);
    int spacePos = s.indexOf(' ');
    if (spacePos >= 0 && s.substring(spacePos + 1).startsWith("0")) {
      s.remove(spacePos + 1, 1);
    }
    return s;
  }
}

bool beginSecureHttp(HTTPClient& http, std::unique_ptr<BearSSL::WiFiClientSecure>& client, const char* url) {
  client.reset(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  return http.begin(*client, url);
}

long long daysFromCivil(int y, unsigned m, unsigned d) {
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const unsigned yoe = (unsigned)(y - era * 400);
  const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + (long long)doe - 719468LL;
}

long long makeEpochUtc(int year, int month, int day, int hour, int minute, int second) {
  long long days = daysFromCivil(year, month, day);
  return days * 86400LL + hour * 3600LL + minute * 60LL + second;
}

bool parseUtcDateTimeToEpoch(const String& dateStr, const String& timeStr, long long& epochUtc) {
  if (dateStr.length() < 10) return false;

  int year = dateStr.substring(0, 4).toInt();
  int month = dateStr.substring(5, 7).toInt();
  int day = dateStr.substring(8, 10).toInt();

  int hour = 0;
  int minute = 0;
  int second = 0;

  if (timeStr.length() >= 8) {
    hour = timeStr.substring(0, 2).toInt();
    minute = timeStr.substring(3, 5).toInt();
    second = timeStr.substring(6, 8).toInt();
  }

  epochUtc = makeEpochUtc(year, month, day, hour, minute, second);
  return true;
}

void addRaceEvent(NextRaceInfo& race, const String& label, JsonVariant node) {
  if (node.isNull()) return;
  if (race.eventCount >= MAX_RACE_EVENTS) return;

  String dateVal = String(node["date"] | "");
  String timeVal = String(node["time"] | "");
  if (dateVal.length() == 0) return;

  RaceEvent e;
  e.present = true;
  e.label = label;
  e.date = dateVal;
  e.time = timeVal;
  e.epochUtc = 0;

  if (!parseUtcDateTimeToEpoch(e.date, e.time, e.epochUtc)) return;

  race.events[race.eventCount++] = e;
}

void sortRaceEvents(NextRaceInfo& race) {
  for (int i = 0; i < race.eventCount - 1; i++) {
    for (int j = i + 1; j < race.eventCount; j++) {
      if (race.events[j].epochUtc < race.events[i].epochUtc) {
        RaceEvent tmp = race.events[i];
        race.events[i] = race.events[j];
        race.events[j] = tmp;
      }
    }
  }
}

bool fetchDriverStandings() {
  std::unique_ptr<BearSSL::WiFiClientSecure> client;
  HTTPClient http;

  if (!beginSecureHttp(http, client, driverStandingsUrl)) return false;

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  StaticJsonDocument<768> filter;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["position"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["points"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Driver"]["familyName"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"][0]["Constructors"][0]["name"] = true;

  DynamicJsonDocument doc(18000);
  DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  if (error) {
    http.end();
    return false;
  }

  JsonArray standings = doc["MRData"]["StandingsTable"]["StandingsLists"][0]["DriverStandings"];
  if (standings.isNull()) {
    http.end();
    return false;
  }

  driverCount = 0;
  for (JsonObject row : standings) {
    if (driverCount >= MAX_DRIVERS) break;

    String surname = String(row["Driver"]["familyName"] | "");
    String team = String(row["Constructors"][0]["name"] | "");

    drivers[driverCount].position = row["position"].as<int>();
    drivers[driverCount].surname = normalizeToAscii(surname);
    drivers[driverCount].team = normalizeToAscii(team);
    drivers[driverCount].points = String(row["points"] | "");
    driverCount++;
  }

  http.end();
  return true;
}

bool fetchConstructorStandings() {
  std::unique_ptr<BearSSL::WiFiClientSecure> client;
  HTTPClient http;

  if (!beginSecureHttp(http, client, constructorStandingsUrl)) return false;

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  StaticJsonDocument<512> filter;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"][0]["position"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"][0]["points"] = true;
  filter["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"][0]["Constructor"]["name"] = true;

  DynamicJsonDocument doc(10000);
  DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  if (error) {
    http.end();
    return false;
  }

  JsonArray standings = doc["MRData"]["StandingsTable"]["StandingsLists"][0]["ConstructorStandings"];
  if (standings.isNull()) {
    http.end();
    return false;
  }

  constructorCount = 0;
  for (JsonObject row : standings) {
    if (constructorCount >= MAX_CONSTRUCTORS) break;

    String name = String(row["Constructor"]["name"] | "");
    constructors[constructorCount].position = row["position"].as<int>();
    constructors[constructorCount].name = normalizeToAscii(name);
    constructors[constructorCount].points = String(row["points"] | "");
    constructorCount++;
  }

  http.end();
  return true;
}

bool fetchNextRace() {
  std::unique_ptr<BearSSL::WiFiClientSecure> client;
  HTTPClient http;

  if (!beginSecureHttp(http, client, nextRaceUrl)) return false;

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  StaticJsonDocument<1536> filter;
  JsonObject raceFilter = filter["MRData"]["RaceTable"]["Races"][0].to<JsonObject>();
  raceFilter["round"] = true;
  raceFilter["raceName"] = true;
  raceFilter["Circuit"]["circuitName"] = true;
  raceFilter["Circuit"]["Location"]["locality"] = true;
  raceFilter["Circuit"]["Location"]["country"] = true;
  raceFilter["date"] = true;
  raceFilter["time"] = true;
  raceFilter["FirstPractice"]["date"] = true;
  raceFilter["FirstPractice"]["time"] = true;
  raceFilter["SecondPractice"]["date"] = true;
  raceFilter["SecondPractice"]["time"] = true;
  raceFilter["ThirdPractice"]["date"] = true;
  raceFilter["ThirdPractice"]["time"] = true;
  raceFilter["Qualifying"]["date"] = true;
  raceFilter["Qualifying"]["time"] = true;
  raceFilter["SprintQualifying"]["date"] = true;
  raceFilter["SprintQualifying"]["time"] = true;
  raceFilter["SprintShootout"]["date"] = true;
  raceFilter["SprintShootout"]["time"] = true;
  raceFilter["Sprint"]["date"] = true;
  raceFilter["Sprint"]["time"] = true;

  DynamicJsonDocument doc(14000);
  DeserializationError error = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  if (error) {
    http.end();
    return false;
  }

  JsonObject race = doc["MRData"]["RaceTable"]["Races"][0];
  if (race.isNull()) {
    http.end();
    return false;
  }

  nextRace.valid = true;
  nextRace.round = String(race["round"] | "");
  nextRace.raceName = normalizeToAscii(String(race["raceName"] | ""));
  nextRace.circuitName = normalizeToAscii(String(race["Circuit"]["circuitName"] | ""));
  nextRace.locality = normalizeToAscii(String(race["Circuit"]["Location"]["locality"] | ""));
  nextRace.country = normalizeToAscii(String(race["Circuit"]["Location"]["country"] | ""));
  nextRace.eventCount = 0;

  addRaceEvent(nextRace, "FP1", race["FirstPractice"]);
  addRaceEvent(nextRace, "FP2", race["SecondPractice"]);
  addRaceEvent(nextRace, "FP3", race["ThirdPractice"]);
  addRaceEvent(nextRace, "SQ", race["SprintQualifying"]);
  addRaceEvent(nextRace, "SQ", race["SprintShootout"]);
  addRaceEvent(nextRace, "QUAL", race["Qualifying"]);
  addRaceEvent(nextRace, "SPR", race["Sprint"]);

  if (nextRace.eventCount < MAX_RACE_EVENTS) {
    String raceDate = String(race["date"] | "");
    String raceTime = String(race["time"] | "");

    if (raceDate.length() > 0) {
      RaceEvent e;
      e.present = true;
      e.label = "RACE";
      e.date = raceDate;
      e.time = raceTime;
      e.epochUtc = 0;

      if (parseUtcDateTimeToEpoch(e.date, e.time, e.epochUtc)) {
        nextRace.events[nextRace.eventCount++] = e;
      }
    }
  }

  sortRaceEvents(nextRace);
  http.end();
  return true;
}

void initPalette() {
  COL_BG        = tft.color565(0, 0, 0);
  COL_HEADER_BG = tft.color565(0, 0, 160);
  COL_TITLE     = tft.color565(255, 255, 0);
  COL_LABEL     = tft.color565(0, 255, 255);
  COL_TEXT      = tft.color565(255, 255, 255);
  COL_HIGHLIGHT = tft.color565(0, 255, 0);
  COL_ALERT     = tft.color565(255, 0, 0);
  COL_LINE      = tft.color565(70, 70, 70);
  COL_FOOTER_BG = tft.color565(0, 0, 120);
}

void drawFooter(const String& leftText) {
  int y = SCREEN_H - FOOTER_H;
  tft.fillRect(0, y, SCREEN_W, FOOTER_H, COL_FOOTER_BG);
  tft.drawFastHLine(0, y, SCREEN_W, COL_LABEL);

  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(COL_LABEL, COL_FOOTER_BG);
  tft.drawString(leftText, 6, y + 3, 1);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_TITLE, COL_FOOTER_BG);
  tft.drawString(ipAddressText, SCREEN_W - 6, y + 3, 1);

  tft.setTextDatum(TL_DATUM);
}

void drawTopHeader(const String& title, const String& pageCode) {
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_HEADER_BG);
  tft.drawFastHLine(0, HEADER_H - 1, SCREEN_W, COL_LABEL);

  tft.setTextColor(COL_TITLE, COL_HEADER_BG);
  tft.drawString(pageCode, 6, 5, 2);

  tft.setTextColor(COL_TEXT, COL_HEADER_BG);
  tft.drawString(title, 70, 5, 2);

  tft.setTextDatum(TR_DATUM);
  tft.setTextColor(COL_LABEL, COL_HEADER_BG);
  tft.drawString(formatClockShortLocal(), SCREEN_W - 6, 5, 2);
  tft.setTextDatum(TL_DATUM);
}

void drawTitleBar(const String& subtitle) {
  tft.fillRect(0, HEADER_H, SCREEN_W, TITLE_H, COL_BG);
  tft.setTextColor(COL_TITLE, COL_BG);
  tft.drawString(subtitle, 6, HEADER_H + 1, 2);
}

void drawTableHeaderDrivers() {
  int y = HEADER_H + TITLE_H;

  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString("POS", 8, y, 2);
  tft.drawString("DRIVER", 48, y, 2);
  tft.drawString("TEAM", 155, y, 2);
  tft.drawRightString("PTS", 312, y, 2);

  tft.drawFastHLine(0, y + 16, SCREEN_W, COL_LABEL);
}

void drawTableHeaderConstructors() {
  int y = HEADER_H + TITLE_H;

  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString("POS", 8, y, 2);
  tft.drawString("TEAM", 50, y, 2);
  tft.drawRightString("PTS", 312, y, 2);

  tft.drawFastHLine(0, y + 16, SCREEN_W, COL_LABEL);
}

void drawDriverRow(int rowIndex, const DriverStanding& d) {
  int rowH = CONTENT_H / DRIVER_ROWS_PER_PAGE;
  int y = CONTENT_TOP + (rowIndex * rowH);

  if (rowIndex > 0) {
    tft.drawFastHLine(0, y - 2, SCREEN_W, COL_LINE);
  }

  uint16_t posColor = (d.position <= 3) ? COL_TITLE : COL_TEXT;

  tft.setTextColor(posColor, COL_BG);
  tft.drawRightString(String(d.position), 34, y + 1, 1);

  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(clipText(d.surname, 11), 48, y + 1, 1);

  tft.setTextColor(COL_HIGHLIGHT, COL_BG);
  tft.drawString(clipText(d.team, 15), 155, y + 3, 1);

  tft.setTextColor(COL_TITLE, COL_BG);
  tft.drawRightString(d.points, 312, y + 1, 1);
}

void drawConstructorRow(int rowIndex, const ConstructorStanding& c) {
  int rowH = CONTENT_H / CONSTRUCTOR_ROWS_PER_PAGE;
  int y = CONTENT_TOP + (rowIndex * rowH);

  if (rowIndex > 0) {
    tft.drawFastHLine(0, y - 2, SCREEN_W, COL_LINE);
  }

  uint16_t posColor = (c.position <= 3) ? COL_TITLE : COL_TEXT;

  tft.setTextColor(posColor, COL_BG);
  tft.drawRightString(String(c.position), 34, y + 1, 1);

  tft.setTextColor(COL_HIGHLIGHT, COL_BG);
  tft.drawString(clipText(c.name, 24), 50, y + 1, 1);

  tft.setTextColor(COL_TITLE, COL_BG);
  tft.drawRightString(c.points, 312, y + 1, 1);
}

void drawRaceInfoLine(const String& label, const String& value, int y, uint16_t valueColor) {
  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString(label, 8, y, 2);

  tft.setTextColor(valueColor, COL_BG);
  tft.drawString(value, 88, y, 2);
}

void drawRaceEventRow(int rowIndex, const RaceEvent& e, int startY) {
  const int rowH = 15;
  int y = startY + (rowIndex * rowH);

  if (rowIndex > 0) tft.drawFastHLine(0, y - 2, SCREEN_W, COL_LINE);

  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString(e.label, 8, y, 2);

  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(clipText(formatEpochDisplay(e.epochUtc), 18), 64, y, 2);
}

void clearMainArea() {
  tft.fillScreen(COL_BG);
}

void renderDriversPage(int startIndex, const String& pageCode, const String& footerLeft) {
  clearMainArea();
  drawTopHeader("F1 STANDINGS", pageCode);
  drawTitleBar("DRIVER CHAMPIONSHIP");
  drawTableHeaderDrivers();

  if (driverCount == 0) {
    tft.setTextColor(COL_ALERT, COL_BG);
    tft.drawString("NO DATA", 8, CONTENT_TOP + 8, 2);
    drawFooter(footerLeft);
    return;
  }

  int endIndex = min(startIndex + DRIVER_ROWS_PER_PAGE, driverCount);
  int row = 0;
  for (int i = startIndex; i < endIndex; i++) {
    drawDriverRow(row++, drivers[i]);
  }

  drawFooter(footerLeft);
}

void renderConstructorsPage() {
  clearMainArea();
  drawTopHeader("F1 STANDINGS", "P103");
  drawTitleBar("CONSTRUCTOR CHAMPIONSHIP");
  drawTableHeaderConstructors();

  if (constructorCount == 0) {
    tft.setTextColor(COL_ALERT, COL_BG);
    tft.drawString("NO DATA", 8, CONTENT_TOP + 8, 2);
    drawFooter("UPDATED " + lastRefreshText);
    return;
  }

  int count = min(constructorCount, CONSTRUCTOR_ROWS_PER_PAGE);
  for (int i = 0; i < count; i++) {
    drawConstructorRow(i, constructors[i]);
  }

  drawFooter("UPDATED " + lastRefreshText);
}

void renderNextRacePage() {
  clearMainArea();
  drawTopHeader("F1 NEXT RACE", "P104");
  drawTitleBar("RACE WEEKEND");

  if (!nextRace.valid) {
    tft.setTextColor(COL_ALERT, COL_BG);
    tft.drawString("NO DATA", 8, CONTENT_TOP + 8, 2);
    drawFooter("UPDATED " + lastRefreshText);
    return;
  }

  int y = HEADER_H + TITLE_H + 2;

  drawRaceInfoLine("ROUND", nextRace.round, y, COL_TITLE); y += 16;
  drawRaceInfoLine("RACE", clipText(nextRace.raceName, 24), y, COL_TEXT); y += 16;
  drawRaceInfoLine("TRACK", clipText(nextRace.circuitName, 23), y, COL_HIGHLIGHT); y += 16;
  drawRaceInfoLine("PLACE", clipText(nextRace.locality + ", " + nextRace.country, 24), y, COL_TEXT); y += 18;

  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString("EVENT", 8, y, 2);
  tft.drawString("DATE/TIME", 64, y, 2);
  tft.drawFastHLine(0, y + 16, SCREEN_W, COL_LABEL);
  y += 18;

  int visible = min(nextRace.eventCount, MAX_VISIBLE_RACE_EVENTS);
  for (int i = 0; i < visible; i++) {
    drawRaceEventRow(i, nextRace.events[i], y);
  }

  drawFooter("UPDATED " + lastRefreshText);
}

void renderSetupScreen(const String& line1, const String& line2 = "", const String& line3 = "") {
  tft.fillScreen(COL_BG);
  tft.fillRect(0, 0, SCREEN_W, HEADER_H, COL_HEADER_BG);

  tft.setTextColor(COL_TITLE, COL_HEADER_BG);
  tft.drawString("P100", 6, 5, 2);

  tft.setTextColor(COL_TEXT, COL_HEADER_BG);
  tft.drawString("F1 CEEFAX", 70, 5, 2);

  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString(line1, 10, 60, 2);

  if (line2.length()) {
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString(line2, 10, 90, 2);
  }

  if (line3.length()) {
    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString(line3, 10, 120, 2);
  }

  drawFooter(inApMode ? "AP MODE" : "STARTING");
}

void renderCurrentPage() {
  switch (currentPage) {
    case PAGE_DRIVERS_1:
      renderDriversPage(0, "P101", "UPDATED " + lastRefreshText);
      break;
    case PAGE_DRIVERS_2:
      renderDriversPage(DRIVER_ROWS_PER_PAGE, "P102", "UPDATED " + lastRefreshText);
      break;
    case PAGE_CONSTRUCTORS:
      renderConstructorsPage();
      break;
    case PAGE_NEXT_RACE:
      renderNextRacePage();
      break;
  }
}

void nextPage() {
  currentPage = (PageType)(((int)currentPage + 1) % 4);
}

String getTimezoneOptionsHtml() {
  String html;
  for (int i = 0; i < TIMEZONE_COUNT; i++) {
    html += "<option value='";
    html += timezoneOptions[i].key;
    html += "'";
    if (config.timezoneKey == timezoneOptions[i].key) html += " selected";
    html += ">";
    html += timezoneOptions[i].label;
    html += "</option>";
  }
  return html;
}

String getSettingsPageHtml(const String& message = "") {
  String html;
  html += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>F1 Display Settings</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;margin:20px;background:#f4f4f4;color:#111;}";
  html += "h1{font-size:24px;margin-bottom:8px;}label{display:block;margin-top:14px;font-weight:bold;}";
  html += "input,select{width:100%;padding:10px;margin-top:6px;box-sizing:border-box;}";
  html += "button{display:inline-block;margin-top:16px;padding:12px 18px;border:none;background:#0033aa;color:#fff;cursor:pointer;}";
  html += ".card{max-width:760px;background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,0.1);}";
  html += ".msg{padding:10px;background:#e8f4ff;border-left:4px solid #0033aa;margin-bottom:16px;}";
  html += ".danger{background:#b00020;}.actions form{display:inline-block;margin-right:8px;}";
  html += "small{color:#555;}";
  html += "</style></head><body><div class='card'>";
  html += "<h1>F1 Display Settings</h1>";

  if (message.length()) {
    html += "<div class='msg'>" + htmlEscape(message) + "</div>";
  }

  html += "<p><strong>Firmware:</strong> " + String(FW_VERSION);
  html += "<br><strong>Mode:</strong> " + String(inApMode ? "Access Point" : "Normal");
  html += "<br><strong>IP:</strong> " + htmlEscape(ipAddressText);
  html += "<br><strong>Hostname:</strong> " + String(inApMode ? "Not available in AP mode" : "f1-display.local");
  html += "<br><strong>Timezone:</strong> " + htmlEscape(timezoneLabelFromKey(config.timezoneKey));
  html += "</p>";

  html += "<form method='POST' action='/save'>";
  html += "<label>WiFi SSID</label>";
  html += "<input name='wifiSsid' value='" + htmlEscape(config.wifiSsid) + "' maxlength='32' required>";

  html += "<label>WiFi Password</label>";
  html += "<input name='wifiPassword' type='password' value='" + htmlEscape(config.wifiPassword) + "' maxlength='63'>";

  html += "<label>Timezone</label>";
  html += "<select name='timezoneKey'>" + getTimezoneOptionsHtml() + "</select>";

  html += "<label>Page Interval (seconds)</label>";
  html += "<input name='pageIntervalSec' type='number' min='3' max='60' value='" + String(config.pageIntervalSec) + "' required>";

  html += "<label>Refresh Interval (minutes)</label>";
  html += "<input name='refreshIntervalMin' type='number' min='1' max='120' value='" + String(config.refreshIntervalMin) + "' required>";

  html += "<label>Clock Format</label>";
  html += "<select name='clockFormat'><option value='24'";
  if (config.clockFormat24) html += " selected";
  html += ">24 Hour</option><option value='12'";
  if (!config.clockFormat24) html += " selected";
  html += ">12 Hour</option></select>";

  html += "<button type='submit'>Save Settings</button>";
  html += "</form>";

  html += "<div class='actions'>";
  html += "<form method='POST' action='/reboot'><button type='submit'>Reboot Device</button></form>";
  html += "<form method='POST' action='/reset-settings'><button type='submit' class='danger'>Reset Settings</button></form>";
  html += "</div>";

  html += "<p><small>Saving settings will reboot the device after 3 seconds.</small></p>";
  html += "</div></body></html>";
  return html;
}

String getSavedPageHtml(const String& message) {
  String html;
  html += "<!DOCTYPE html><html><head><meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<meta http-equiv='refresh' content='3;url=/'>";
  html += "<title>Saved</title>";
  html += "<style>body{font-family:Arial,sans-serif;background:#f4f4f4;padding:20px;} .card{max-width:600px;background:#fff;padding:20px;border-radius:8px;box-shadow:0 2px 8px rgba(0,0,0,.1);}</style>";
  html += "</head><body><div class='card'><h1>" + htmlEscape(message) + "</h1><p>Device will reboot in 3 seconds.</p></div></body></html>";
  return html;
}

void delayedRebootMessage(const String& line1, const String& line2) {
  renderSetupScreen(line1, line2, "REBOOTING...");
  delay(3000);
  ESP.restart();
}

void handleRoot() {
  server.send(200, "text/html", getSettingsPageHtml());
}

void handleSave() {
  if (!server.hasArg("wifiSsid") || !server.hasArg("timezoneKey") || !server.hasArg("pageIntervalSec") ||
      !server.hasArg("refreshIntervalMin") || !server.hasArg("clockFormat")) {
    server.send(400, "text/html", getSettingsPageHtml("Missing required fields."));
    return;
  }

  String wifiSsid = trimCopy(server.arg("wifiSsid"));
  String wifiPassword = trimCopy(server.arg("wifiPassword"));
  String timezoneKey = trimCopy(server.arg("timezoneKey"));
  int pageIntervalSec = server.arg("pageIntervalSec").toInt();
  int refreshIntervalMin = server.arg("refreshIntervalMin").toInt();
  String clockFormat = trimCopy(server.arg("clockFormat"));

  if (wifiSsid.length() < 1 || wifiSsid.length() > 32) {
    server.send(400, "text/html", getSettingsPageHtml("WiFi SSID must be between 1 and 32 characters."));
    return;
  }

  if (pageIntervalSec < 3 || pageIntervalSec > 60) {
    server.send(400, "text/html", getSettingsPageHtml("Page interval must be between 3 and 60 seconds."));
    return;
  }

  if (refreshIntervalMin < 1 || refreshIntervalMin > 120) {
    server.send(400, "text/html", getSettingsPageHtml("Refresh interval must be between 1 and 120 minutes."));
    return;
  }

  config.wifiSsid = wifiSsid;
  config.wifiPassword = wifiPassword;
  config.pageIntervalSec = (uint16_t)pageIntervalSec;
  config.refreshIntervalMin = (uint16_t)refreshIntervalMin;
  config.clockFormat24 = (clockFormat == "24");

  if (!applyTimezoneFromKey(timezoneKey)) {
    server.send(400, "text/html", getSettingsPageHtml("Invalid timezone selected."));
    return;
  }

  logLine("Saving updated settings");
  applyRuntimeConfig();

  if (!saveConfig()) {
    server.send(500, "text/html", getSettingsPageHtml("Failed to save settings."));
    return;
  }

  server.send(200, "text/html", getSavedPageHtml("Settings saved"));
  delay(200);
  delayedRebootMessage("SETTINGS SAVED", "REBOOT IN 3 SECONDS");
}

void handleReboot() {
  logLine("Web request: reboot");
  server.send(200, "text/html", getSavedPageHtml("Rebooting device"));
  delay(200);
  delayedRebootMessage("REBOOT DEVICE", "PLEASE WAIT");
}

void handleResetSettings() {
  logLine("Web request: reset settings");
  clearConfigFile();
  setDefaultConfig();
  saveConfig();
  server.send(200, "text/html", getSavedPageHtml("Settings reset"));
  delay(200);
  delayedRebootMessage("SETTINGS RESET", "STARTING SETUP MODE");
}

void startWebServer() {
  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/reboot", HTTP_POST, handleReboot);
  server.on("/reset-settings", HTTP_POST, handleResetSettings);
  server.begin();
  webServerStarted = true;
  logLine("Web server started");
}

void updateIpAddressText() {
  if (inApMode) {
    ipAddressText = WiFi.softAPIP().toString();
  } else if (WiFi.status() == WL_CONNECTED) {
    ipAddressText = WiFi.localIP().toString();
  } else {
    ipAddressText = "0.0.0.0";
  }
}

void startApMode() {
  inApMode = true;
  mdnsStarted = false;
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  updateIpAddressText();

  renderSetupScreen("SETUP MODE", AP_SSID, ipAddressText);

  if (!webServerStarted) {
    startWebServer();
  }

  logLine("AP mode started");
  logLine("AP SSID: " + String(AP_SSID));
  logLine("AP IP: " + ipAddressText);
}

bool connectWiFi() {
  if (config.wifiSsid.length() == 0) {
    logLine("No WiFi config present");
    return false;
  }

  inApMode = false;
  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPassword.c_str());

  for (int attempt = 1; attempt <= WIFI_RETRY_COUNT; attempt++) {
    logLine("WiFi attempt " + String(attempt));

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_RETRY_WINDOW_MS) {
      delay(500);
      Serial.print(".");
      if (webServerStarted) server.handleClient();
    }
    Serial.println();

    if (WiFi.status() == WL_CONNECTED) {
      updateIpAddressText();
      logLine("WiFi connected");
      logLine("SSID: " + config.wifiSsid);
      logLine("IP: " + ipAddressText);
      return true;
    }

    WiFi.disconnect();
    delay(500);
    WiFi.begin(config.wifiSsid.c_str(), config.wifiPassword.c_str());
  }

  logLine("WiFi failed after retries");
  return false;
}

void startMdnsIfNeeded() {
  if (!inApMode && WiFi.status() == WL_CONNECTED) {
    mdnsStarted = MDNS.begin(HOSTNAME);
    if (mdnsStarted) {
      logLine("mDNS started: http://f1-display.local");
    } else {
      logLine("mDNS failed to start");
    }
  }
}

void initTimeSystem() {
  logLine("Starting time sync");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  applyRuntimeConfig();

  time_t now = time(nullptr);
  int retries = 0;
  while (now < 100000 && retries < 20) {
    delay(500);
    now = time(nullptr);
    retries++;
  }

  if (now >= 100000) {
    logLine("Time sync complete");
  } else {
    logLine("Time sync not confirmed");
  }
}

void refreshAllData() {
  if (inApMode || WiFi.status() != WL_CONNECTED) return;

  logLine("Refreshing F1 data...");
  bool okDrivers = fetchDriverStandings();
  bool okConstructors = fetchConstructorStandings();
  bool okNextRace = fetchNextRace();

  logLine("Drivers: " + String(okDrivers ? "OK" : "FAIL"));
  logLine("Constructors: " + String(okConstructors ? "OK" : "FAIL"));
  logLine("Next race: " + String(okNextRace ? "OK" : "FAIL"));

  lastRefresh = millis();
  lastRefreshText = formatClockShortLocal();
}

void setupDisplay() {
  tft.init();
  tft.setRotation(1);
  initPalette();
  renderSetupScreen("BOOTING...");
}

void setup() {
  Serial.begin(115200);
  delay(1000);

  logLine("");
  logLine("Booting F1 Display");
  logLine("Firmware: " + String(FW_VERSION));

  if (!LittleFS.begin()) {
    logLine("LittleFS mount failed");
  } else {
    logLine("LittleFS mounted");
  }

  setDefaultConfig();
  loadConfig();

  setupDisplay();
  applyRuntimeConfig();

  if (!connectWiFi()) {
    startWebServer();
    startApMode();
    return;
  }

  updateIpAddressText();
  startWebServer();
  startMdnsIfNeeded();
  initTimeSystem();
  refreshAllData();
  renderCurrentPage();

  lastPageSwitch = millis();
}

void loop() {
  if (webServerStarted) {
    server.handleClient();
  }

  if (!inApMode && mdnsStarted) {
    MDNS.update();
  }

  if (inApMode) {
    delay(10);
    return;
  }

  if (WiFi.status() != WL_CONNECTED) {
    logLine("WiFi disconnected, attempting reconnect");
    if (!connectWiFi()) {
      startApMode();
      return;
    }
    updateIpAddressText();
    startMdnsIfNeeded();
    renderCurrentPage();
  }

  unsigned long now = millis();

  if (now - lastRefresh >= refreshIntervalMs) {
    refreshAllData();
    renderCurrentPage();
  }

  if (now - lastPageSwitch >= pageIntervalMs) {
    lastPageSwitch = now;
    nextPage();
    renderCurrentPage();
  }
}