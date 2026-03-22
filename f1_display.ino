#include <ESP8266WiFi.h>
#include <ESP8266HTTPClient.h>
#include <WiFiClientSecureBearSSL.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <LittleFS.h>
#include <ArduinoJson.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <ESP8266httpUpdate.h>

TFT_eSPI tft = TFT_eSPI();
ESP8266WebServer server(80);

const char* FW_VERSION = "1.1.4";
const char* CONFIG_FILE = "/config.json";
const char* AP_SSID = "F1-Display-Setup";
const char* HOSTNAME = "f1-display";

const char* otaManifestUrl               = "https://marcusharris-1993.github.io/f1_display/version.json";
const unsigned long OTA_CHECK_TIMEOUT_MS = 10000UL;

const char* driverStandingsUrl      = "https://api.jolpi.ca/ergast/f1/current/driverStandings.json";
const char* constructorStandingsUrl = "https://api.jolpi.ca/ergast/f1/current/constructorStandings.json";
const char* nextRaceUrl             = "https://api.jolpi.ca/ergast/f1/current/next.json";
const char* lastRaceResultsUrl      = "https://api.jolpi.ca/ergast/f1/current/last/results.json";
const char* lastRacePitStopsUrl     = "https://api.jolpi.ca/ergast/f1/current/last/pitstops.json";

const int MAX_DRIVERS = 30;
const int MAX_CONSTRUCTORS = 20;
const int MAX_RACE_EVENTS = 10;
const int MAX_LAST_RACE_DRIVERS = 30;

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

struct PodiumEntry {
  int position;
  String surname;
  String team;
};

struct LastRaceInfo {
  bool valid;
  String raceName;
  String round;
  String locality;
  String country;
  PodiumEntry podium[3];
  int podiumCount;
};

struct FastestPitStopInfo {
  bool valid;
  String raceName;
  String round;
  String driverSurname;
  String team;
  String durationText;
  String lap;
  String stop;
};

DriverStanding drivers[MAX_DRIVERS];
ConstructorStanding constructors[MAX_CONSTRUCTORS];
NextRaceInfo nextRace;
LastRaceInfo lastRace;
FastestPitStopInfo fastestPitStop;
AppConfig config;

String lastRaceDriverIds[MAX_LAST_RACE_DRIVERS];
String lastRaceDriverNames[MAX_LAST_RACE_DRIVERS];
String lastRaceDriverTeams[MAX_LAST_RACE_DRIVERS];
int lastRaceDriverMapCount = 0;

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
  PAGE_NEXT_RACE,
  PAGE_LAST_RACE,
  PAGE_FASTEST_PIT_STOP
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

int compareVersions(const String& currentVersion, const String& newVersion) {
  int c1 = 0, c2 = 0, c3 = 0;
  int n1 = 0, n2 = 0, n3 = 0;

  sscanf(currentVersion.c_str(), "%d.%d.%d", &c1, &c2, &c3);
  sscanf(newVersion.c_str(), "%d.%d.%d", &n1, &n2, &n3);

  if (n1 != c1) return (n1 > c1) ? 1 : -1;
  if (n2 != c2) return (n2 > c2) ? 1 : -1;
  if (n3 != c3) return (n3 > c3) ? 1 : -1;
  return 0;
}

String timezoneLabelFromKey(const String& key) {
  for (int i = 0; i < TIMEZONE_COUNT; i++) {
    if (key == timezoneOptions[i].key) return String(timezoneOptions[i].label);
  }
  return "Unknown";
}

bool parseDurationSeconds(const String& text, float& secondsOut) {
  if (text.length() == 0) return false;
  secondsOut = text.toFloat();
  return secondsOut > 0.0f;
}

bool lookupLastRaceDriver(const String& driverId, String& surnameOut, String& teamOut) {
  for (int i = 0; i < lastRaceDriverMapCount; i++) {
    if (lastRaceDriverIds[i] == driverId) {
      surnameOut = lastRaceDriverNames[i];
      teamOut = lastRaceDriverTeams[i];
      return true;
    }
  }
  return false;
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
    applyRuntimeConfig();
    return false;
  }

  File f = LittleFS.open(CONFIG_FILE, "r");
  if (!f) {
    logLine("Failed to open config file, using defaults");
    setDefaultConfig();
    applyRuntimeConfig();
    return false;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, f);
  f.close();

  if (err) {
    logLine("Config parse failed, using defaults");
    setDefaultConfig();
    applyRuntimeConfig();
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

String formatLastRefreshTime() {
  time_t raw = time(nullptr);
  struct tm* local = localtime(&raw);
  if (local == nullptr) return "UNKNOWN";
  char buffer[24];
  strftime(buffer, sizeof(buffer), "%d/%m %H:%M", local);
  return String(buffer);
}

bool beginSecureHttp(HTTPClient& http, std::unique_ptr<BearSSL::WiFiClientSecure>& client, const char* url) {
  client.reset(new BearSSL::WiFiClientSecure);
  client->setInsecure();
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);
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
  addRaceEvent(nextRace, "SO", race["SprintShootout"]);
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

bool fetchLastRaceResults() {
  std::unique_ptr<BearSSL::WiFiClientSecure> client;
  HTTPClient http;

  lastRace.valid = false;
  lastRace.podiumCount = 0;
  lastRaceDriverMapCount = 0;

  if (!beginSecureHttp(http, client, lastRaceResultsUrl)) return false;

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  StaticJsonDocument<2048> filter;
  JsonObject raceFilter = filter["MRData"]["RaceTable"]["Races"][0].to<JsonObject>();
  raceFilter["round"] = true;
  raceFilter["raceName"] = true;
  raceFilter["Circuit"]["Location"]["locality"] = true;
  raceFilter["Circuit"]["Location"]["country"] = true;
  JsonObject resultFilter = raceFilter["Results"][0].to<JsonObject>();
  resultFilter["position"] = true;
  resultFilter["Driver"]["driverId"] = true;
  resultFilter["Driver"]["familyName"] = true;
  resultFilter["Constructor"]["name"] = true;

  DynamicJsonDocument doc(26000);
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

  JsonArray results = race["Results"];
  if (results.isNull()) {
    http.end();
    return false;
  }

  lastRace.valid = true;
  lastRace.round = String(race["round"] | "");
  lastRace.raceName = normalizeToAscii(String(race["raceName"] | ""));
  lastRace.locality = normalizeToAscii(String(race["Circuit"]["Location"]["locality"] | ""));
  lastRace.country = normalizeToAscii(String(race["Circuit"]["Location"]["country"] | ""));
  lastRace.podiumCount = 0;

  for (JsonObject row : results) {
    String driverId = String(row["Driver"]["driverId"] | "");
    String surname = normalizeToAscii(String(row["Driver"]["familyName"] | ""));
    String team = normalizeToAscii(String(row["Constructor"]["name"] | ""));
    int pos = row["position"].as<int>();

    if (lastRaceDriverMapCount < MAX_LAST_RACE_DRIVERS) {
      lastRaceDriverIds[lastRaceDriverMapCount] = driverId;
      lastRaceDriverNames[lastRaceDriverMapCount] = surname;
      lastRaceDriverTeams[lastRaceDriverMapCount] = team;
      lastRaceDriverMapCount++;
    }

    if (pos >= 1 && pos <= 3 && lastRace.podiumCount < 3) {
      lastRace.podium[lastRace.podiumCount].position = pos;
      lastRace.podium[lastRace.podiumCount].surname = surname;
      lastRace.podium[lastRace.podiumCount].team = team;
      lastRace.podiumCount++;
    }
  }

  http.end();
  return true;
}

bool fetchFastestPitStop() {
  std::unique_ptr<BearSSL::WiFiClientSecure> client;
  HTTPClient http;

  fastestPitStop.valid = false;

  if (!beginSecureHttp(http, client, lastRacePitStopsUrl)) return false;

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    http.end();
    return false;
  }

  StaticJsonDocument<1536> filter;
  JsonObject raceFilter = filter["MRData"]["RaceTable"]["Races"][0].to<JsonObject>();
  raceFilter["round"] = true;
  raceFilter["raceName"] = true;
  JsonObject pitFilter = raceFilter["PitStops"][0].to<JsonObject>();
  pitFilter["driverId"] = true;
  pitFilter["lap"] = true;
  pitFilter["stop"] = true;
  pitFilter["duration"] = true;

  DynamicJsonDocument doc(24000);
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

  JsonArray pitStops = race["PitStops"];
  if (pitStops.isNull()) {
    http.end();
    return false;
  }

  bool found = false;
  float bestDuration = 0.0f;
  String bestDriverId;
  String bestLap;
  String bestStop;
  String bestDurationText;

  for (JsonObject row : pitStops) {
    String durationText = String(row["duration"] | "");
    float seconds = 0.0f;
    if (!parseDurationSeconds(durationText, seconds)) continue;

    if (!found || seconds < bestDuration) {
      found = true;
      bestDuration = seconds;
      bestDriverId = String(row["driverId"] | "");
      bestLap = String(row["lap"] | "");
      bestStop = String(row["stop"] | "");
      bestDurationText = durationText;
    }
  }

  if (!found) {
    http.end();
    return false;
  }

  String surname = bestDriverId;
  String team = "";
  lookupLastRaceDriver(bestDriverId, surname, team);

  fastestPitStop.valid = true;
  fastestPitStop.round = String(race["round"] | "");
  fastestPitStop.raceName = normalizeToAscii(String(race["raceName"] | ""));
  fastestPitStop.driverSurname = normalizeToAscii(surname);
  fastestPitStop.team = normalizeToAscii(team);
  fastestPitStop.durationText = bestDurationText;
  fastestPitStop.lap = bestLap;
  fastestPitStop.stop = bestStop;

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

  if (rowIndex > 0) tft.drawFastHLine(0, y - 2, SCREEN_W, COL_LINE);

  uint16_t posColor = (d.position <= 3) ? COL_TITLE : COL_TEXT;

  tft.setTextColor(posColor, COL_BG);
  tft.drawRightString(String(d.position), 34, y + 1, 2);

  tft.setTextColor(COL_TEXT, COL_BG);
  tft.drawString(clipText(d.surname, 11), 48, y + 1, 2);

  tft.setTextColor(COL_HIGHLIGHT, COL_BG);
  tft.drawString(clipText(d.team, 15), 155, y + 1, 2);

  tft.setTextColor(COL_TITLE, COL_BG);
  tft.drawRightString(d.points, 312, y + 1, 2);
}

void drawConstructorRow(int rowIndex, const ConstructorStanding& c) {
  int rowH = CONTENT_H / CONSTRUCTOR_ROWS_PER_PAGE;
  int y = CONTENT_TOP + (rowIndex * rowH);

  if (rowIndex > 0) tft.drawFastHLine(0, y - 2, SCREEN_W, COL_LINE);

  uint16_t posColor = (c.position <= 3) ? COL_TITLE : COL_TEXT;

  tft.setTextColor(posColor, COL_BG);
  tft.drawRightString(String(c.position), 34, y + 1, 2);

  tft.setTextColor(COL_HIGHLIGHT, COL_BG);
  tft.drawString(clipText(c.name, 24), 50, y + 1, 2);

  tft.setTextColor(COL_TITLE, COL_BG);
  tft.drawRightString(c.points, 312, y + 1, 2);
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

void renderLastRacePage() {
  clearMainArea();
  drawTopHeader("F1 LAST RACE", "P105");
  drawTitleBar("PODIUM");

  if (!lastRace.valid || lastRace.podiumCount == 0) {
    tft.setTextColor(COL_ALERT, COL_BG);
    tft.drawString("NO DATA", 8, CONTENT_TOP + 8, 2);
    drawFooter("UPDATED " + lastRefreshText);
    return;
  }

  int y = HEADER_H + TITLE_H + 4;
  drawRaceInfoLine("ROUND", lastRace.round, y, COL_TITLE); y += 16;
  drawRaceInfoLine("RACE", clipText(lastRace.raceName, 24), y, COL_TEXT); y += 16;
  drawRaceInfoLine("PLACE", clipText(lastRace.locality + ", " + lastRace.country, 24), y, COL_HIGHLIGHT); y += 20;

  tft.setTextColor(COL_LABEL, COL_BG);
  tft.drawString("POS", 8, y, 2);
  tft.drawString("DRIVER", 50, y, 2);
  tft.drawString("TEAM", 170, y, 2);
  tft.drawFastHLine(0, y + 16, SCREEN_W, COL_LABEL);
  y += 20;

  for (int i = 0; i < lastRace.podiumCount; i++) {
    if (i > 0) tft.drawFastHLine(0, y - 3, SCREEN_W, COL_LINE);

    uint16_t posColor = (i == 0) ? COL_TITLE : COL_TEXT;

    tft.setTextColor(posColor, COL_BG);
    tft.drawRightString(String(lastRace.podium[i].position), 34, y, 2);

    tft.setTextColor(COL_TEXT, COL_BG);
    tft.drawString(clipText(lastRace.podium[i].surname, 13), 50, y, 2);

    tft.setTextColor(COL_HIGHLIGHT, COL_BG);
    tft.drawString(clipText(lastRace.podium[i].team, 14), 170, y, 2);

    y += 24;
  }

  drawFooter("UPDATED " + lastRefreshText);
}

void renderFastestPitStopPage() {
  clearMainArea();
  drawTopHeader("F1 PIT STOP", "P106");
  drawTitleBar("FASTEST LAST RACE");

  if (!fastestPitStop.valid) {
    tft.setTextColor(COL_ALERT, COL_BG);
    tft.drawString("NO DATA", 8, CONTENT_TOP + 8, 2);
    drawFooter("UPDATED " + lastRefreshText);
    return;
  }

  int y = HEADER_H + TITLE_H + 6;
  drawRaceInfoLine("ROUND", fastestPitStop.round, y, COL_TITLE); y += 18;
  drawRaceInfoLine("RACE", clipText(fastestPitStop.raceName, 24), y, COL_TEXT); y += 18;
  drawRaceInfoLine("DRIVER", clipText(fastestPitStop.driverSurname, 20), y, COL_TEXT); y += 18;
  drawRaceInfoLine("TEAM", clipText(fastestPitStop.team, 20), y, COL_HIGHLIGHT); y += 18;
  drawRaceInfoLine("TIME", fastestPitStop.durationText + "S", y, COL_TITLE); y += 18;
  drawRaceInfoLine("LAP", fastestPitStop.lap, y, COL_TEXT); y += 18;
  drawRaceInfoLine("STOP", fastestPitStop.stop, y, COL_TEXT); y += 18;

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

void showOtaStatus(const String& line1, const String& line2 = "", const String& line3 = "") {
  renderSetupScreen(line1, line2, line3);
}

bool fetchOtaManifest(String& newVersion, String& firmwareUrl) {
  std::unique_ptr<BearSSL::WiFiClientSecure> client;
  HTTPClient http;

  if (!beginSecureHttp(http, client, otaManifestUrl)) {
    logLine("OTA: failed to begin manifest request");
    return false;
  }

  http.setTimeout(OTA_CHECK_TIMEOUT_MS);
  http.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  int httpCode = http.GET();
  if (httpCode != HTTP_CODE_OK) {
    logLine("OTA: manifest HTTP error " + String(httpCode));
    http.end();
    return false;
  }

  DynamicJsonDocument doc(1024);
  DeserializationError err = deserializeJson(doc, http.getStream());
  http.end();

  if (err) {
    logLine("OTA: manifest JSON parse failed");
    return false;
  }

  newVersion = String(doc["version"] | "");
  firmwareUrl = String(doc["firmware"] | "");

  if (newVersion.length() == 0 || firmwareUrl.length() == 0) {
    logLine("OTA: manifest missing version or firmware URL");
    return false;
  }

  return true;
}

void checkForOtaUpdate() {
  if (WiFi.status() != WL_CONNECTED) {
    logLine("OTA: skipped, WiFi not connected");
    return;
  }

  String onlineVersion;
  String firmwareUrl;

  showOtaStatus("CHECKING FOR UPDATE", "CURRENT: " + String(FW_VERSION));

  if (!fetchOtaManifest(onlineVersion, firmwareUrl)) {
    logLine("OTA: manifest fetch failed");
    delay(800);
    return;
  }

  logLine("OTA: current version = " + String(FW_VERSION));
  logLine("OTA: online version  = " + onlineVersion);
  logLine("OTA: firmware URL    = " + firmwareUrl);

  int versionCompare = compareVersions(String(FW_VERSION), onlineVersion);

  if (versionCompare >= 0) {
    logLine("OTA: no newer firmware available");
    showOtaStatus("NO UPDATE NEEDED", "VERSION " + String(FW_VERSION));
    delay(800);
    return;
  }

  showOtaStatus("UPDATING FIRMWARE", "TO VERSION " + onlineVersion, "PLEASE WAIT");

  BearSSL::WiFiClientSecure otaClient;
  otaClient.setInsecure();

  ESPhttpUpdate.rebootOnUpdate(true);
  ESPhttpUpdate.setFollowRedirects(HTTPC_FORCE_FOLLOW_REDIRECTS);

  t_httpUpdate_return ret = ESPhttpUpdate.update(otaClient, firmwareUrl);

  switch (ret) {
    case HTTP_UPDATE_FAILED:
      logLine("OTA: update failed, error (" +
              String(ESPhttpUpdate.getLastError()) + "): " +
              ESPhttpUpdate.getLastErrorString());
      showOtaStatus("UPDATE FAILED", ESPhttpUpdate.getLastErrorString());
      delay(2000);
      break;

    case HTTP_UPDATE_NO_UPDATES:
      logLine("OTA: server reported no updates");
      showOtaStatus("NO UPDATE FOUND");
      delay(1000);
      break;

    case HTTP_UPDATE_OK:
      logLine("OTA: update successful, rebooting");
      break;
  }
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

    case PAGE_LAST_RACE:
      renderLastRacePage();
      break;

    case PAGE_FASTEST_PIT_STOP:
      renderFastestPitStopPage();
      break;

    default:
      renderDriversPage(0, "P101", "UPDATED " + lastRefreshText);
      break;
  }
}

void nextPage() {
  switch (currentPage) {
    case PAGE_DRIVERS_1:
      currentPage = (driverCount > DRIVER_ROWS_PER_PAGE) ? PAGE_DRIVERS_2 : PAGE_CONSTRUCTORS;
      break;

    case PAGE_DRIVERS_2:
      currentPage = PAGE_CONSTRUCTORS;
      break;

    case PAGE_CONSTRUCTORS:
      currentPage = PAGE_NEXT_RACE;
      break;

    case PAGE_NEXT_RACE:
      currentPage = PAGE_LAST_RACE;
      break;

    case PAGE_LAST_RACE:
      currentPage = PAGE_FASTEST_PIT_STOP;
      break;

    case PAGE_FASTEST_PIT_STOP:
    default:
      currentPage = PAGE_DRIVERS_1;
      break;
  }

  renderCurrentPage();
}

bool connectToWifi() {
  if (config.wifiSsid.length() == 0) {
    logLine("WiFi SSID not configured");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.hostname(HOSTNAME);
  WiFi.begin(config.wifiSsid.c_str(), config.wifiPassword.c_str());

  renderSetupScreen("CONNECTING WIFI", config.wifiSsid);

  unsigned long startMs = millis();
  int retryCounter = 0;

  while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < WIFI_RETRY_WINDOW_MS) {
    delay(500);
    Serial.print(".");
    retryCounter++;
    if (retryCounter >= WIFI_RETRY_COUNT) retryCounter = 0;
    yield();
  }
  Serial.println();

  if (WiFi.status() == WL_CONNECTED) {
    ipAddressText = WiFi.localIP().toString();
    logLine("WiFi connected: " + ipAddressText);
    return true;
  }

  logLine("WiFi connection failed");
  return false;
}

void startApMode() {
  WiFi.mode(WIFI_AP);
  WiFi.softAP(AP_SSID);
  IPAddress ip = WiFi.softAPIP();
  ipAddressText = ip.toString();
  inApMode = true;
  logLine("AP mode started: " + ipAddressText);
  renderSetupScreen("SETUP MODE ACTIVE", AP_SSID, ipAddressText);
}

void syncTimeNow() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov", "time.google.com");

  for (int i = 0; i < 20; i++) {
    time_t now = time(nullptr);
    if (now > 100000) {
      logLine("Time sync complete");
      return;
    }
    delay(500);
    yield();
  }

  logLine("Time sync timeout");
}

bool refreshAllData() {
  bool okDrivers = fetchDriverStandings();
  bool okConstructors = fetchConstructorStandings();
  bool okNextRace = fetchNextRace();
  bool okLastRace = fetchLastRaceResults();
  bool okPit = fetchFastestPitStop();

  bool overall = okDrivers || okConstructors || okNextRace || okLastRace || okPit;

  if (overall) {
    lastRefreshText = formatLastRefreshTime();
    lastRefresh = millis();
  }

  logLine("Refresh status:");
  logLine("  Drivers: " + String(okDrivers ? "OK" : "FAIL"));
  logLine("  Constructors: " + String(okConstructors ? "OK" : "FAIL"));
  logLine("  Next race: " + String(okNextRace ? "OK" : "FAIL"));
  logLine("  Last race: " + String(okLastRace ? "OK" : "FAIL"));
  logLine("  Pit stop: " + String(okPit ? "OK" : "FAIL"));

  return overall;
}

String buildHtmlPage() {
  String html;
  html.reserve(7000);

  html += "<!doctype html><html><head><meta charset='utf-8'>";
  html += "<meta name='viewport' content='width=device-width,initial-scale=1'>";
  html += "<title>F1 Display Setup</title>";
  html += "<style>";
  html += "body{font-family:Arial,sans-serif;background:#111;color:#eee;margin:0;padding:20px;}";
  html += ".card{max-width:720px;margin:0 auto;background:#1b1b1b;padding:20px;border-radius:10px;}";
  html += "h1,h2{margin-top:0;color:#ff0;}label{display:block;margin-top:12px;margin-bottom:6px;color:#8ff;}";
  html += "input,select{width:100%;padding:10px;border-radius:6px;border:1px solid #555;background:#000;color:#fff;box-sizing:border-box;}";
  html += "button,.btn{display:inline-block;margin-top:16px;padding:10px 14px;border:none;border-radius:6px;background:#0044cc;color:#fff;text-decoration:none;cursor:pointer;}";
  html += ".danger{background:#b00020;} .muted{color:#bbb;font-size:14px;} .row{margin-top:8px;} .mono{font-family:monospace;}";
  html += "</style></head><body><div class='card'>";

  html += "<h1>F1 Display Setup</h1>";
  html += "<p class='muted'>Firmware: <span class='mono'>" + htmlEscape(String(FW_VERSION)) + "</span></p>";
  html += "<p class='muted'>IP: <span class='mono'>" + htmlEscape(ipAddressText) + "</span></p>";
  html += "<p class='muted'>Mode: " + String(inApMode ? "Access Point" : "WiFi Client") + "</p>";

  html += "<h2>Configuration</h2>";
  html += "<form method='post' action='/save'>";
  html += "<label>WiFi SSID</label><input name='wifiSsid' value='" + htmlEscape(config.wifiSsid) + "'>";
  html += "<label>WiFi Password</label><input name='wifiPassword' type='password' value='" + htmlEscape(config.wifiPassword) + "'>";

  html += "<label>Timezone</label><select name='timezoneKey'>";
  for (int i = 0; i < TIMEZONE_COUNT; i++) {
    html += "<option value='" + htmlEscape(String(timezoneOptions[i].key)) + "'";
    if (config.timezoneKey == timezoneOptions[i].key) html += " selected";
    html += ">" + htmlEscape(String(timezoneOptions[i].label)) + "</option>";
  }
  html += "</select>";

  html += "<label>Page interval (seconds)</label><input name='pageIntervalSec' type='number' min='3' max='60' value='" + String(config.pageIntervalSec) + "'>";
  html += "<label>Refresh interval (minutes)</label><input name='refreshIntervalMin' type='number' min='1' max='120' value='" + String(config.refreshIntervalMin) + "'>";

  html += "<label>Clock format</label><select name='clockFormat24'>";
  html += "<option value='1'" + String(config.clockFormat24 ? " selected" : "") + ">24 hour</option>";
  html += "<option value='0'" + String(!config.clockFormat24 ? " selected" : "") + ">12 hour</option>";
  html += "</select>";

  html += "<div class='row'><button type='submit'>Save Configuration</button></div>";
  html += "</form>";

  html += "<h2>Actions</h2>";
  html += "<p><a class='btn' href='/refresh'>Refresh Data Now</a></p>";
  html += "<p><a class='btn' href='/ota'>Check OTA Update</a></p>";
  html += "<p><a class='btn danger' href='/reset'>Reset Settings</a></p>";

  html += "<h2>Device Info</h2>";
  html += "<p class='muted'>Last refresh: " + htmlEscape(lastRefreshText) + "</p>";
  html += "<p class='muted'>Timezone: " + htmlEscape(timezoneLabelFromKey(config.timezoneKey)) + "</p>";
  html += "</div></body></html>";

  return html;
}

void handleRoot() {
  server.send(200, "text/html", buildHtmlPage());
}

void handleSave() {
  config.wifiSsid = trimCopy(server.arg("wifiSsid"));
  config.wifiPassword = server.arg("wifiPassword");
  config.pageIntervalSec = (uint16_t)server.arg("pageIntervalSec").toInt();
  config.refreshIntervalMin = (uint16_t)server.arg("refreshIntervalMin").toInt();
  config.clockFormat24 = (server.arg("clockFormat24") != "0");

  String tzKey = trimCopy(server.arg("timezoneKey"));
  if (!applyTimezoneFromKey(tzKey)) {
    applyTimezoneFromKey("GMT");
  }

  applyRuntimeConfig();
  saveConfig();

  server.send(200, "text/html",
    "<html><body style='font-family:Arial;background:#111;color:#fff;padding:20px;'>"
    "<h2>Configuration saved</h2>"
    "<p>Device will reboot in 3 seconds.</p>"
    "</body></html>"
  );

  delay(3000);
  ESP.restart();
}

void handleRefresh() {
  bool ok = false;

  if (WiFi.status() == WL_CONNECTED) {
    syncTimeNow();
    ok = refreshAllData();
    renderCurrentPage();
  }

  server.send(200, "text/html",
    String("<html><body style='font-family:Arial;background:#111;color:#fff;padding:20px;'><h2>Refresh ") +
    (ok ? "successful" : "failed") +
    "</h2><p><a href='/'>Back</a></p></body></html>");
}

void handleReset() {
  clearConfigFile();
  server.send(200, "text/html",
    "<html><body style='font-family:Arial;background:#111;color:#fff;padding:20px;'>"
    "<h2>Settings cleared</h2><p>Device will reboot in 3 seconds.</p></body></html>"
  );
  delay(3000);
  ESP.restart();
}

void handleOta() {
  server.send(200, "text/html",
    "<html><body style='font-family:Arial;background:#111;color:#fff;padding:20px;'>"
    "<h2>Checking OTA update</h2><p>The device screen will show progress.</p><p><a href='/'>Back</a></p></body></html>"
  );
  delay(500);
  checkForOtaUpdate();
  renderCurrentPage();
}

void startWebServer() {
  if (webServerStarted) return;

  server.on("/", HTTP_GET, handleRoot);
  server.on("/save", HTTP_POST, handleSave);
  server.on("/refresh", HTTP_GET, handleRefresh);
  server.on("/reset", HTTP_GET, handleReset);
  server.on("/ota", HTTP_GET, handleOta);

  server.begin();
  webServerStarted = true;
  logLine("Web server started");
}

void startMdnsIfNeeded() {
  if (mdnsStarted) return;
  if (MDNS.begin(HOSTNAME)) {
    MDNS.addService("http", "tcp", 80);
    mdnsStarted = true;
    logLine("mDNS started");
  } else {
    logLine("mDNS failed");
  }
}

void setupDisplay() {
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextWrap(false);
  initPalette();
}

void setup() {
  Serial.begin(115200);
  delay(200);

  setupDisplay();
  renderSetupScreen("BOOTING");

  if (!LittleFS.begin()) {
    renderSetupScreen("LITTLEFS FAILED");
    while (true) {
      delay(1000);
    }
  }

  setDefaultConfig();
  loadConfig();
  applyRuntimeConfig();

  if (connectToWifi()) {
    inApMode = false;
    syncTimeNow();
    startMdnsIfNeeded();
    startWebServer();
    checkForOtaUpdate();
    refreshAllData();
    renderCurrentPage();
  } else {
    startApMode();
    startWebServer();
  }

  lastPageSwitch = millis();
}

void loop() {
  server.handleClient();
  MDNS.update();

  if (!inApMode) {
    if (WiFi.status() != WL_CONNECTED) {
      ipAddressText = "0.0.0.0";
    } else {
      ipAddressText = WiFi.localIP().toString();
    }

    unsigned long now = millis();

    if (now - lastPageSwitch >= pageIntervalMs) {
      lastPageSwitch = now;
      nextPage();
    }

    if (WiFi.status() == WL_CONNECTED && (now - lastRefresh >= refreshIntervalMs)) {
      syncTimeNow();
      refreshAllData();
      renderCurrentPage();
    }
  }

  delay(20);
}
