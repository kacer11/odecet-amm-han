#include <Arduino.h>
#include <WiFi.h>
#include <AsyncTCP.h>
#include <ESPAsyncWebServer.h>
#include <SPI.h>
#include <SD.h>
#include <time.h>
#include <Preferences.h>
#include <DNSServer.h>
#include <Update.h>
#include <PubSubClient.h>
#include <Wire.h>

// ========== DEBUG ==========
// Nastavte na 1 pro ladění, 0 pro produkci
#define DEBUG 0
#if DEBUG
  #define DBG(x)        Serial.println(F(x))
  #define DBG_VAL(x, v) do { Serial.print(F(x)); Serial.println(v); } while(0)
  #define DBG_FMT(...)  Serial.printf(__VA_ARGS__)
#else
  #define DBG(x)
  #define DBG_VAL(x, v)
  #define DBG_FMT(...)
#endif

// ========== VERZE FIRMWARE ==========
#define FW_VERSION "0.1 ČEZ"


// ========== GRAFY – DATOVÉ TYPY ==========

// Jaká veličina se vykresluje
enum GraphMetric {
  GRAPH_P_PLUS,
  GRAPH_P_MINUS,
  GRAPH_A_PLUS,
  GRAPH_A_MINUS
};

// Jeden bod grafu
struct GraphPoint {
  uint32_t ts;   // unix timestamp (sekundy)
  double   val;
};

// Změna tarifu (step-line)
struct TariffPoint {
  uint32_t ts;
  uint8_t  tariff;  // 1 = T1, 2 = T2
};

// Mapování metric -> CSV sloupec + popis
struct GraphMetricMap {
  GraphMetric metric;
  uint8_t     csvIndex;
  const char* label;
  const char* unit;
};

static const GraphMetricMap GRAPH_METRICS[] = {
  { GRAPH_P_PLUS,  5,  "P+ odběr",  "W"   },
  { GRAPH_P_MINUS, 9,  "P- dodávka","W"   },
  { GRAPH_A_PLUS, 13,  "A+ energie","kWh" },
  { GRAPH_A_MINUS,18,  "A- energie","kWh" }
};

//============HA konfigurace================
struct HAConfig {
  bool enabled = false;
  char host[32] = "";
  uint16_t port = 1883;
  char clientId[32] = "AMM_ESP32";
  char username[32] = "";
  char password[32] = "";
};

HAConfig haConfig;
Preferences haPrefs;
WiFiClient espClient;
PubSubClient mqtt(espClient);

const uint16_t MQTT_BUFFER_SIZE = 1024;

bool haReady = false;
int8_t rtcMqttPending = 0; 


// ========== OBIS REGISTR – JEDINÝ ZDROJ PRAVDY ==========

enum ObisType : uint8_t {
  OBIS_TYPE_NUMBER = 0,
  OBIS_TYPE_STRING = 1,
  OBIS_TYPE_BOOL   = 2
};

enum ObisQuality : uint8_t {
  OBIS_QUALITY_UNKNOWN = 0,
  OBIS_QUALITY_OK      = 1,
  OBIS_QUALITY_SUSPECT = 2,
  OBIS_QUALITY_STALE   = 3
};

struct EventFileInfo {
  String name;
  int year;
  int month;
  int day;
  uint32_t size;
};

struct ObisEntry {
  const char* obisCode;   // "1-0:1.8.0.255"
  uint8_t     ln[6];      // DLMS LN
  ObisType    type;
  const char* unit;       // "kWh", "W", "", ...

  double      valueNumber;
  char        valueString[16];
  bool        valueBool;

  bool        valid;
  ObisQuality quality;
  uint32_t    lastUpdate; // sekundy od startu
};

const size_t OBIS_REG_COUNT = 22;
static ObisEntry g_obisReg[OBIS_REG_COUNT];

// ========== PINY PODLE ZAPOJENÍ ==========

// RS485 (UART2)
#define PIN_RS485_RX 16   // D16
#define PIN_RS485_TX 17   // D17

HardwareSerial RS485(2); // UART2

// ====== RESET BUTTON ======
#define RESET_BTN_PIN 33

unsigned long resetBtnPressMs = 0;
bool resetBtnActive = false;


// ========== SD KARTA ==========
#define SD_CS_PIN   4
#define SD_MOSI_PIN 27
#define SD_SCK_PIN  25
#define SD_MISO_PIN 26

SPIClass sdSPI(VSPI);

bool sdOk = false;
File currentLogFile;

// ====== INFO O LOG SOUBORECH ======

struct LogFileInfo {
  String name;   // DD.MM.YYYY.csv
  int year;
  int month;
  int day;
  uint32_t size;
};

bool parseLogFilename(const char* name, LogFileInfo& out) {
  // očekává DD.MM.YYYY.csv
  if (!name) return false;
  if (strlen(name) != 14) return false;
  if (name[2] != '.' || name[5] != '.' || strcmp(name + 10, ".csv") != 0)
    return false;

  out.day   = atoi(String(name).substring(0, 2).c_str());
  out.month = atoi(String(name).substring(3, 5).c_str());
  out.year  = atoi(String(name).substring(6, 10).c_str());
  out.name  = name;

  return (out.year >= 2000 && out.month >= 1 && out.month <= 12);
}

//========Parser event soboury=====
bool parseEventFilename(const char* name, EventFileInfo& info) {
  if (!name) return false;
  if (strlen(name) != 14) return false;   // YYYY-MM-DD.log

  if (name[4] != '-' || name[7] != '-' || strcmp(name + 10, ".log") != 0)
    return false;

  info.year  = atoi(String(name).substring(0,4).c_str());
  info.month = atoi(String(name).substring(5,7).c_str());
  info.day   = atoi(String(name).substring(8,10).c_str());
  info.name  = name;

  return true;
}


// ====== LIVE EVENT STATE ======
static bool g_liveEventActive = false;
static char g_liveEventType[32] = "";
static char g_liveEventText[128] = "";
static char g_liveEventTime[32] = "";


// Aktuální "datum" log souboru jako text "DD.MM.YYYY" nebo fallback "dayXXX"
char currentLogDate[16] = "";  // 15 znaků + \0

// Konfigurace logování
uint32_t g_sdCardSizeMB = 0;                          // velikost SD v MB (zadá uživatel, 0 = nezadáno)
uint64_t g_logMaxBytes  = 100ULL * 1024ULL * 1024ULL; // default limit 100 MB na logy


AsyncWebServer server(80);

volatile bool g_doRestart = false;
unsigned long g_restartAt = 0;


DNSServer dnsServer;
const byte DNS_PORT = 53;


enum AppWifiMode {
  APP_WIFI_STA,
  APP_WIFI_AP
};

AppWifiMode g_wifiMode = APP_WIFI_STA;



// ========== ČAS (NTP) ==========

const char* NTP_SERVER = "pool.ntp.org";

// Používáme TZ string (CET / CEST), offsety NEPOUŽÍVAT
bool timeSynced = false;

Preferences prefs;

bool rtcPresent = false;


// ========== METER STATE (persistovaný) ==========
static char g_lastMeterSN[16] = "";   // poslední známé SN z NVS
static bool g_meterSNLoaded = false;


// ---------- Pomocné funkce OBISReg ----------

static bool lnEquals(const uint8_t a[6], const uint8_t b[6]) {
  for (int i = 0; i < 6; i++) {
    if (a[i] != b[i]) return false;
  }
  return true;
}

static int obisRegFindIndexByLN(const uint8_t ln[6]) {
  for (size_t i = 0; i < OBIS_REG_COUNT; i++) {
    if (lnEquals(ln, g_obisReg[i].ln)) return (int)i;
  }
  return -1;
}

static int obisRegFindIndexByObis(const char* obis) {
  if (!obis) return -1;
  for (size_t i = 0; i < OBIS_REG_COUNT; i++) {
    if (g_obisReg[i].obisCode && strcmp(g_obisReg[i].obisCode, obis) == 0) {
      return (int)i;
    }
  }
  return -1;
}

void obisRegInit() {
  for (size_t i = 0; i < OBIS_REG_COUNT; i++) {
    memset(&g_obisReg[i], 0, sizeof(ObisEntry));
    g_obisReg[i].quality = OBIS_QUALITY_UNKNOWN;
  }

  size_t i = 0;

  // 0-0:96.1.1.255 SN elektroměru
  g_obisReg[i].obisCode = "0-0:96.1.1.255";
  uint8_t ln0[] = {0x00,0x00,0x60,0x01,0x01,0xFF};
  memcpy(g_obisReg[i].ln, ln0, 6);
  g_obisReg[i].type = OBIS_TYPE_STRING;
  g_obisReg[i].unit = "";
  i++;

  // 0-0:96.3.10.255 stav odpojovače
  g_obisReg[i].obisCode = "0-0:96.3.10.255";
  uint8_t ln1[] = {0x00,0x00,0x60,0x03,0x0A,0xFF};
  memcpy(g_obisReg[i].ln, ln1, 6);
  g_obisReg[i].type = OBIS_TYPE_BOOL;
  g_obisReg[i].unit = "";
  i++;

  // 0-0:17.0.0.255 limiter
  g_obisReg[i].obisCode = "0-0:17.0.0.255";
  uint8_t ln2[] = {0x00,0x00,0x11,0x00,0x00,0xFF};
  memcpy(g_obisReg[i].ln, ln2, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = ""; // scaler/jednotka doladíme
  i++;

  // 0-1:96.3.10.255 R1
  g_obisReg[i].obisCode = "0-1:96.3.10.255";
  uint8_t ln3[] = {0x00,0x01,0x60,0x03,0x0A,0xFF};
  memcpy(g_obisReg[i].ln, ln3, 6);
  g_obisReg[i].type = OBIS_TYPE_BOOL;
  g_obisReg[i].unit = "";
  i++;

  // 0-2:96.3.10.255 R2
  g_obisReg[i].obisCode = "0-2:96.3.10.255";
  uint8_t ln4[] = {0x00,0x02,0x60,0x03,0x0A,0xFF};
  memcpy(g_obisReg[i].ln, ln4, 6);
  g_obisReg[i].type = OBIS_TYPE_BOOL;
  g_obisReg[i].unit = "";
  i++;

  // 0-3:96.3.10.255 R3
  g_obisReg[i].obisCode = "0-3:96.3.10.255";
  uint8_t ln5[] = {0x00,0x03,0x60,0x03,0x0A,0xFF};
  memcpy(g_obisReg[i].ln, ln5, 6);
  g_obisReg[i].type = OBIS_TYPE_BOOL;
  g_obisReg[i].unit = "";
  i++;

  // 0-4:96.3.10.255 R4
  g_obisReg[i].obisCode = "0-4:96.3.10.255";
  uint8_t ln6[] = {0x00,0x04,0x60,0x03,0x0A,0xFF};
  memcpy(g_obisReg[i].ln, ln6, 6);
  g_obisReg[i].type = OBIS_TYPE_BOOL;
  g_obisReg[i].unit = "";
  i++;

  // 0-0:96.14.0.255 aktuální tarif
  g_obisReg[i].obisCode = "0-0:96.14.0.255";
  uint8_t ln7[] = {0x00,0x00,0x60,0x0E,0x00,0xFF};
  memcpy(g_obisReg[i].ln, ln7, 6);
  g_obisReg[i].type = OBIS_TYPE_STRING;
  g_obisReg[i].unit = "";
  i++;

  // 1-0:1.7.0.255 P+ celkem
  g_obisReg[i].obisCode = "1-0:1.7.0.255";
  uint8_t ln8[] = {0x01,0x00,0x01,0x07,0x00,0xFF};
  memcpy(g_obisReg[i].ln, ln8, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "W";
  i++;

  // 1-0:21.7.0.255 P+ L1
  g_obisReg[i].obisCode = "1-0:21.7.0.255";
  uint8_t ln9[]  = {0x01,0x00,0x15,0x07,0x00,0xFF};
  memcpy(g_obisReg[i].ln, ln9, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "W";
  i++;

  // 1-0:41.7.0.255 P+ L2
  g_obisReg[i].obisCode = "1-0:41.7.0.255";
  uint8_t ln10[] = {0x01,0x00,0x29,0x07,0x00,0xFF};
  memcpy(g_obisReg[i].ln, ln10, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "W";
  i++;

  // 1-0:61.7.0.255 P+ L3
  g_obisReg[i].obisCode = "1-0:61.7.0.255";
  uint8_t ln11[] = {0x01,0x00,0x3D,0x07,0x00,0xFF};
  memcpy(g_obisReg[i].ln, ln11, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "W";
  i++;

  // 1-0:2.7.0.255 P- celkem
  g_obisReg[i].obisCode = "1-0:2.7.0.255";
  uint8_t ln12[] = {0x01,0x00,0x02,0x07,0x00,0xFF};
  memcpy(g_obisReg[i].ln, ln12, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "W";
  i++;

  // 1-0:22.7.0.255 P- L1
  g_obisReg[i].obisCode = "1-0:22.7.0.255";
  uint8_t ln13[] = {0x01,0x00,0x16,0x07,0x00,0xFF};
  memcpy(g_obisReg[i].ln, ln13, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "W";
  i++;

  // 1-0:42.7.0.255 P- L2
  g_obisReg[i].obisCode = "1-0:42.7.0.255";
  uint8_t ln14[] = {0x01,0x00,0x2A,0x07,0x00,0xFF};
  memcpy(g_obisReg[i].ln, ln14, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "W";
  i++;

  // 1-0:62.7.0.255 P- L3
  g_obisReg[i].obisCode = "1-0:62.7.0.255";
  uint8_t ln15[] = {0x01,0x00,0x3E,0x07,0x00,0xFF};
  memcpy(g_obisReg[i].ln, ln15, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "W";
  i++;

  // 1-0:1.8.0.255 A+ celkem
  g_obisReg[i].obisCode = "1-0:1.8.0.255";
  uint8_t ln16[] = {0x01,0x00,0x01,0x08,0x00,0xFF};
  memcpy(g_obisReg[i].ln, ln16, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "kWh";
  i++;

  // 1-0:1.8.1.255 A+ T1
  g_obisReg[i].obisCode = "1-0:1.8.1.255";
  uint8_t ln17[] = {0x01,0x00,0x01,0x08,0x01,0xFF};
  memcpy(g_obisReg[i].ln, ln17, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "kWh";
  i++;

  // 1-0:1.8.2.255 A+ T2
  g_obisReg[i].obisCode = "1-0:1.8.2.255";
  uint8_t ln18[] = {0x01,0x00,0x01,0x08,0x02,0xFF};
  memcpy(g_obisReg[i].ln, ln18, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "kWh";
  i++;

  // 1-0:1.8.3.255 A+ T3
  g_obisReg[i].obisCode = "1-0:1.8.3.255";
  uint8_t ln19[] = {0x01,0x00,0x01,0x08,0x03,0xFF};
  memcpy(g_obisReg[i].ln, ln19, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "kWh";
  i++;

  // 1-0:1.8.4.255 A+ T4
  g_obisReg[i].obisCode = "1-0:1.8.4.255";
  uint8_t ln20[] = {0x01,0x00,0x01,0x08,0x04,0xFF};
  memcpy(g_obisReg[i].ln, ln20, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "kWh";
  i++;

  // 1-0:2.8.0.255 A- celkem
  g_obisReg[i].obisCode = "1-0:2.8.0.255";
  uint8_t ln21[] = {0x01,0x00,0x02,0x08,0x00,0xFF};
  memcpy(g_obisReg[i].ln, ln21, 6);
  g_obisReg[i].type = OBIS_TYPE_NUMBER;
  g_obisReg[i].unit = "kWh";
  i++;
}

bool obisRegSetNumberByLN(const uint8_t ln[6], double value, uint32_t ts) {
  int idx = obisRegFindIndexByLN(ln);
  if (idx < 0) return false;
  ObisEntry& e = g_obisReg[idx];
  if (e.type != OBIS_TYPE_NUMBER) return false;
  e.valueNumber = value;
  e.valid = true;
  e.quality = OBIS_QUALITY_OK;
  e.lastUpdate = ts;
  return true;
}

bool obisRegSetStringByLN(const uint8_t ln[6], const char* str, uint32_t ts) {
  int idx = obisRegFindIndexByLN(ln);
  if (idx < 0) return false;
  ObisEntry& e = g_obisReg[idx];
  if (e.type != OBIS_TYPE_STRING) return false;
  if (!str) return false;

  size_t len = strlen(str);
  if (len >= sizeof(e.valueString)) len = sizeof(e.valueString) - 1;
  memcpy(e.valueString, str, len);
  e.valueString[len] = '\0';

  e.valid = true;
  e.quality = OBIS_QUALITY_OK;
  e.lastUpdate = ts;
  return true;
}

bool obisRegSetBoolByLN(const uint8_t ln[6], bool v, uint32_t ts) {
  int idx = obisRegFindIndexByLN(ln);
  if (idx < 0) return false;
  ObisEntry& e = g_obisReg[idx];
  if (e.type != OBIS_TYPE_BOOL) return false;
  e.valueBool = v;
  e.valid = true;
  e.quality = OBIS_QUALITY_OK;
  e.lastUpdate = ts;
  return true;
}

bool obisRegGetByObis(const char* obis, ObisEntry& out) {
  int idx = obisRegFindIndexByObis(obis);
  if (idx < 0) return false;
  out = g_obisReg[idx];
  return true;

}

// ================inicializace SD karty=================

void initSD() {
  DBG("Inicializuji SD kartu...");

  // SPI pro SD (VSPI)
  sdSPI.begin(SD_SCK_PIN, SD_MISO_PIN, SD_MOSI_PIN, SD_CS_PIN);

  if (!SD.begin(SD_CS_PIN, sdSPI)) {
    DBG("SD init FAIL - logovani vypnuto");
    sdOk = false;
    return;
  }

  sdOk = true;
  DBG("SD init OK");

  // Adresář /logs
  if (!SD.exists("/logs")) {
    if (SD.mkdir("/logs")) {
      DBG("Vytvoren adresar /logs");
    } else {
      DBG("Chyba pri vytvareni /logs");
    }
  }
}

// ============Získání data jako text=================

bool getCurrentDateString(char* buf, size_t len) {
  if (!timeSynced) {
    return false;
  }
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 1000)) {
    return false;
  }
  // DD.MM.YYYY
  snprintf(buf, len, "%02d.%02d.%04d",
           timeinfo.tm_mday,
           timeinfo.tm_mon + 1,
           timeinfo.tm_year + 1900);
  return true;
}

// =============Nastavení velikosti SD karty================

void loadConfig() {
  prefs.begin("amm", true);  // read-only
  g_sdCardSizeMB = prefs.getUInt("sd_mb", 0);
  prefs.end();

  // Default limit = 100 MB
  g_logMaxBytes = 100ULL * 1024ULL * 1024ULL;

  // Pokud uživatel zadal SD velikost, nastavíme limit např. na 80 % SD
  if (g_sdCardSizeMB > 0) {
    uint64_t totalBytes = (uint64_t)g_sdCardSizeMB * 1024ULL * 1024ULL;
    g_logMaxBytes = (totalBytes * 80ULL) / 100ULL;  // 80 % z kapacity
  }

  DBG("Config: SD karta = ");
  DBG_VAL("", g_sdCardSizeMB);
  DBG(" MB, max log = ");
  DBG_VAL("", (unsigned long)(g_logMaxBytes / (1024ULL * 1024ULL)));
  DBG(" MB");
}

void saveConfig() {
  prefs.begin("amm", false);  // read-write
  prefs.putUInt("sd_mb", g_sdCardSizeMB);
  prefs.end();

  // přepočítat limit logů
  g_logMaxBytes = 100ULL * 1024ULL * 1024ULL;
  if (g_sdCardSizeMB > 0) {
    uint64_t totalBytes = (uint64_t)g_sdCardSizeMB * 1024ULL * 1024ULL;
    g_logMaxBytes = (totalBytes * 80ULL) / 100ULL;
  }

  DBG("Ulozena konfigurace: SD = ");
  DBG_VAL("", g_sdCardSizeMB);
  DBG(" MB, log limit ~ ");
  DBG_VAL("", (unsigned long)(g_logMaxBytes / (1024ULL * 1024ULL)));
  DBG(" MB");
}

//-------Načtení SN z paměti--------
void initMeterState() {
  prefs.begin("meter", true);   // read-only
  String sn = prefs.getString("last_sn", "");
  prefs.end();

  if (sn.length() > 0 && sn.length() < sizeof(g_lastMeterSN)) {
    strncpy(g_lastMeterSN, sn.c_str(), sizeof(g_lastMeterSN));
    g_lastMeterSN[sizeof(g_lastMeterSN) - 1] = '\0';
    DBG_VAL("Meter: loaded SN = ", g_lastMeterSN);
  } else {
    g_lastMeterSN[0] = '\0';
    DBG("Meter: no stored SN");
  }

  g_meterSNLoaded = true;
}

//------Zapis eventu na SD-------
void logMeterReplacementEvent(const char* oldSN, const char* newSN) {
  if (!sdOk) return;

  if (!SD.exists("/events")) {
    SD.mkdir("/events");
  }

  struct tm tm;
  if (!getLocalTime(&tm, 1000)) return;

  char path[32];
  snprintf(path, sizeof(path),
           "/events/%04d-%02d-%02d.log",
           tm.tm_year + 1900,
           tm.tm_mon + 1,
           tm.tm_mday);

  File f = SD.open(path, FILE_APPEND);
  if (!f) return;

  char ts[32];
  snprintf(ts, sizeof(ts),
           "%04d-%02d-%02d %02d:%02d:%02d",
           tm.tm_year + 1900,
           tm.tm_mon + 1,
           tm.tm_mday,
           tm.tm_hour,
           tm.tm_min,
           tm.tm_sec);

  f.print(ts);
  f.print(";METER_REPLACED;old=");
  f.print(oldSN);
  f.print(";new=");
  f.println(newSN);
  f.close();

  DBG("EVENT: METER_REPLACED");
}

//------ Zapis RTC battery eventu na SD -------
void logRtcBatteryEvent() {
  if (!sdOk) return;

  if (!SD.exists("/events")) {
    SD.mkdir("/events");
  }

  struct tm tm;
  bool timeOk = getLocalTime(&tm, 1000);

  char ts[32];
  char path[32];
  if (timeOk) {
  snprintf(path, sizeof(path),
           "/events/%04d-%02d-%02d.log",
           tm.tm_year + 1900,
           tm.tm_mon + 1,
           tm.tm_mday);
} else {
  snprintf(path, sizeof(path), "/events/unknown_time.log");
}
 if (timeOk) {
  snprintf(ts, sizeof(ts),
           "%04d-%02d-%02d %02d:%02d:%02d",
           tm.tm_year + 1900,
           tm.tm_mon + 1,
           tm.tm_mday,
           tm.tm_hour,
           tm.tm_min,
           tm.tm_sec);
} else {
  strncpy(ts, "unknown-time", sizeof(ts));
}


  File f = SD.open(path, FILE_APPEND);
  if (!f) return;

  f.print(ts);
  f.println(";RTC_BATTERY_LOST;");

  f.close();

  DBG("EVENT: RTC_BATTERY_LOST");
}


bool rtcBatteryWarning = false;


//=========Detekce SN=============
void checkMeterReplacement() {
  if (!g_meterSNLoaded) return;

  ObisEntry e;
  if (!obisRegGetByObis("0-0:96.1.1.255", e) || !e.valid)
    return;

  if (strlen(e.valueString) == 0)
    return;

  // první SN v životě zařízení → jen uložit
  if (g_lastMeterSN[0] == '\0') {
    prefs.begin("meter", false);
    prefs.putString("last_sn", e.valueString);
    prefs.end();

    strncpy(g_lastMeterSN, e.valueString, sizeof(g_lastMeterSN));
    g_lastMeterSN[sizeof(g_lastMeterSN) - 1] = '\0';

    DBG_VAL("Meter: initial SN stored = ", g_lastMeterSN);
    return;
  }

  // změna SN → EVENT
  if (strcmp(e.valueString, g_lastMeterSN) != 0) {
    logMeterReplacementEvent(g_lastMeterSN, e.valueString);
    setLiveEvent(
    "METER_REPLACED",
    "Došlo k výměně elektroměru"
    );


    prefs.begin("meter", false);
    prefs.putString("last_sn", e.valueString);
    prefs.end();

    strncpy(g_lastMeterSN, e.valueString, sizeof(g_lastMeterSN));
    g_lastMeterSN[sizeof(g_lastMeterSN) - 1] = '\0';

    DBG_VAL("Meter: SN changed to ", g_lastMeterSN);
  }
}

//=======Nastavení aktivního menu=========
void setLiveEvent(const char* type, const char* text) {
  struct tm tm;
  if (!getLocalTime(&tm, 1000)) return;

  char ts[32];
  snprintf(ts, sizeof(ts),
           "%04d-%02d-%02d %02d:%02d:%02d",
           tm.tm_year + 1900,
           tm.tm_mon + 1,
           tm.tm_mday,
           tm.tm_hour,
           tm.tm_min,
           tm.tm_sec);

  // uložit do NVS
  prefs.begin("event", false);
  prefs.putBool("active", true);
  prefs.putString("type", type);
  prefs.putString("text", text);
  prefs.putString("time", ts);
  prefs.end();

  // uložit do RAM
  g_liveEventActive = true;
  strncpy(g_liveEventType, type, sizeof(g_liveEventType));
  strncpy(g_liveEventText, text, sizeof(g_liveEventText));
  strncpy(g_liveEventTime, ts, sizeof(g_liveEventTime));

  DBG("LIVE EVENT: set");
}

//============Načtení eventu po startu=============
void loadLiveEvent() {
  prefs.begin("event", true);
  g_liveEventActive = prefs.getBool("active", false);

  if (g_liveEventActive) {
    String t  = prefs.getString("type", "");
    String tx = prefs.getString("text", "");
    String tm = prefs.getString("time", "");

    strncpy(g_liveEventType, t.c_str(), sizeof(g_liveEventType));
    strncpy(g_liveEventText, tx.c_str(), sizeof(g_liveEventText));
    strncpy(g_liveEventTime, tm.c_str(), sizeof(g_liveEventTime));

    DBG("LIVE EVENT: loaded");
  }
  prefs.end();
}

//===================HA Config=====================
void loadHAConfig() {
  haPrefs.begin("ha", true);

  haConfig.enabled = haPrefs.getBool("enabled", false);
  haPrefs.getString("host", haConfig.host, sizeof(haConfig.host));
  haConfig.port = haPrefs.getUShort("port", 1883);
  haPrefs.getString("client_id", haConfig.clientId, sizeof(haConfig.clientId));
  if (strlen(haConfig.clientId) == 0) {
    strncpy(haConfig.clientId, "AMM_ESP32", sizeof(haConfig.clientId));
  }
  haPrefs.getString("username", haConfig.username, sizeof(haConfig.username));
  haPrefs.getString("password", haConfig.password, sizeof(haConfig.password));

  haPrefs.end();
}

void saveHAConfig() {
  haPrefs.begin("ha", false);

  haPrefs.putBool("enabled", haConfig.enabled);
  haPrefs.putString("host", haConfig.host);
  haPrefs.putUShort("port", haConfig.port);
  haPrefs.putString("client_id", haConfig.clientId);
  haPrefs.putString("username", haConfig.username);
  haPrefs.putString("password", haConfig.password);

  haPrefs.end();
}

bool ensureMQTT();
void publishHADiscovery();

bool ensureMQTT() {
  if (!haConfig.enabled) return false;
  if (strlen(haConfig.host) == 0) return false;

  if (!mqtt.connected()) {
    DBG("MQTT connect to ");
    DBG_VAL("", haConfig.host);
    DBG_VAL(":", haConfig.port);

    bool ok;
    if (strlen(haConfig.username) > 0) {
      // Přihlášení s uživatelským jménem a heslem
      ok = mqtt.connect(
        haConfig.clientId,
        haConfig.username,
        strlen(haConfig.password) > 0 ? haConfig.password : nullptr
      );
    } else {
      // Anonymní přihlášení
      ok = mqtt.connect(haConfig.clientId);
    }
    if (!ok) {
      DBG_VAL("MQTT connect failed, state=", mqtt.state());
      return false;
    }

    // ===== MQTT subscribe po connectu =====
    mqtt.subscribe("amm/rtc/ack");
    DBG("MQTT subscribed: amm/rtc/ack");

    // ===== HA Discovery =====
    publishHADiscovery();
  }

  return true;
}

//==============Zavření eventu WEB UI a HA======
void closeLiveEventInternal(const char* source) {

  if (!g_liveEventActive) return;

  prefs.begin("event", false);
  prefs.putBool("active", false);
  prefs.end();

  g_liveEventActive = false;

  DBG("LIVE EVENT: closed");
  if (source) {
    DBG(" (");
    DBG_VAL("", source);
    DBG(")");
  }
  DBG_VAL("", );

  // pokud RTC → smazat OSF bit + okamžitě informuj HA
  if (strcmp(g_liveEventType, "rtc") == 0) {
    if (rtcPresent) {
      Wire.beginTransmission(0x68);
      Wire.write(0x0F);
      Wire.write(0x00);
      Wire.endTransmission();
      DBG("[RTC] OSF bit smazan pri zavreni eventu");
    }
    // Poslat OFF okamžitě, nečekat na další RS485 rámec
    if (ensureMQTT()) {
      mqtt.publish("amm/rtc/battery", "OFF", true);
      DBG("[RTC] MQTT battery OFF odeslano");
    } else {
      // MQTT není dostupné – odložit do publishHA()
      rtcMqttPending = -1;
    }
  }
}

//=================HA Callback====================
void mqttCallback(char* topic, byte* payload, unsigned int length) {
  payload[length] = '\0';

  DBG_FMT("MQTT RX [%s]: %s\n", topic, payload);

  // ACK z Home Assistantu – zavřít RTC event
  if (strcmp(topic, "amm/rtc/ack") == 0) {
    closeLiveEventInternal("ha");
  }
}


//=========Převod pro MQTT=========
void obisToTopic(const char* obis, char* out, size_t outLen) {
  size_t j = 0;
  for (size_t i = 0; obis[i] && j + 1 < outLen; i++) {
    char c = obis[i];
    if (c == ':' || c == '.') c = '_';
    out[j++] = c;
  }
  out[j] = 0;
}


//============HA Discovery===========

struct ObisHAMeta {
  const char* obisCode;
  const char* name;         // lidský název entity
  const char* deviceClass;  // HA device_class (nebo "" pokud žádná)
  const char* stateClass;   // measurement / total_increasing / ""
};

static const ObisHAMeta HA_META[] = {
  { "0-0:96.1.1.255",  "Výrobní číslo",         "",              ""                   },
  { "0-0:96.3.10.255", "Odpojovač",              "connectivity",  ""                   },
  { "0-0:17.0.0.255",  "Limiter",                "power",         "measurement"        },
  { "0-1:96.3.10.255", "Relé 1",                 "",              ""                   },
  { "0-2:96.3.10.255", "Relé 2",                 "",              ""                   },
  { "0-3:96.3.10.255", "Relé 3",                 "",              ""                   },
  { "0-4:96.3.10.255", "Relé 4",                 "",              ""                   },
  { "0-0:96.14.0.255", "Tarif",                  "",              ""                   },
  { "1-0:1.7.0.255",   "P+ odběr celkem",        "power",         "measurement"        },
  { "1-0:21.7.0.255",  "P+ odběr L1",            "power",         "measurement"        },
  { "1-0:41.7.0.255",  "P+ odběr L2",            "power",         "measurement"        },
  { "1-0:61.7.0.255",  "P+ odběr L3",            "power",         "measurement"        },
  { "1-0:2.7.0.255",   "P- dodávka celkem",      "power",         "measurement"        },
  { "1-0:22.7.0.255",  "P- dodávka L1",          "power",         "measurement"        },
  { "1-0:42.7.0.255",  "P- dodávka L2",          "power",         "measurement"        },
  { "1-0:62.7.0.255",  "P- dodávka L3",          "power",         "measurement"        },
  { "1-0:1.8.0.255",   "A+ energie celkem",      "energy",        "total_increasing"   },
  { "1-0:1.8.1.255",   "A+ energie T1",          "energy",        "total_increasing"   },
  { "1-0:1.8.2.255",   "A+ energie T2",          "energy",        "total_increasing"   },
  { "1-0:1.8.3.255",   "A+ energie T3",          "energy",        "total_increasing"   },
  { "1-0:1.8.4.255",   "A+ energie T4",          "energy",        "total_increasing"   },
  { "1-0:2.8.0.255",   "A- energie celkem",      "energy",        "total_increasing"   },
};

void publishHADiscovery() {
  if (!mqtt.connected()) return;

  // Identifikátor zařízení – clientId bez mezer
  const char* devId = haConfig.clientId;

  // Základ state topicu – stejný jako v publishHA()
  // amm/obis/<obis_topic>

  for (size_t i = 0; i < OBIS_REG_COUNT; i++) {
    const ObisEntry& e = g_obisReg[i];
    if (!e.obisCode) continue;

    // Najdi metadata
    const ObisHAMeta* meta = nullptr;
    for (size_t m = 0; m < sizeof(HA_META) / sizeof(HA_META[0]); m++) {
      if (strcmp(HA_META[m].obisCode, e.obisCode) == 0) {
        meta = &HA_META[m];
        break;
      }
    }
    if (!meta) continue;

    // Převod OBIS kódu na topic segment
    char obisTopic[64];
    obisToTopic(e.obisCode, obisTopic, sizeof(obisTopic));

    // Discovery topic
    // homeassistant/<component>/<device_id>/<obis>/config
    char discoveryTopic[128];
    const char* component = (e.type == OBIS_TYPE_BOOL) ? "binary_sensor" : "sensor";
    snprintf(discoveryTopic, sizeof(discoveryTopic),
             "homeassistant/%s/%s/%s/config",
             component, devId, obisTopic);

    // State topic – stejný jako publishHA()
    char stateTopic[128];
    snprintf(stateTopic, sizeof(stateTopic), "amm/obis/%s", obisTopic);

    // Unique ID
    char uniqueId[96];
    snprintf(uniqueId, sizeof(uniqueId), "%s_%s", devId, obisTopic);

    // Sestavení JSON
    String json;
    json.reserve(512);
    json += "{";

    // Název entity
    json += "\"name\":\"";
    json += meta->name;
    json += "\",";

    // Unique ID
    json += "\"unique_id\":\"";
    json += uniqueId;
    json += "\",";

    // State topic
    json += "\"state_topic\":\"";
    json += stateTopic;
    json += "\",";

    // Device class (pokud existuje)
    if (strlen(meta->deviceClass) > 0) {
      json += "\"device_class\":\"";
      json += meta->deviceClass;
      json += "\",";
    }

    // State class (pokud existuje)
    if (strlen(meta->stateClass) > 0) {
      json += "\"state_class\":\"";
      json += meta->stateClass;
      json += "\",";
    }

    // Jednotka (pokud existuje)
    if (e.unit && strlen(e.unit) > 0) {
      json += "\"unit_of_measurement\":\"";
      json += e.unit;
      json += "\",";
    }

    // Binary sensor – payload
    if (e.type == OBIS_TYPE_BOOL) {
      json += "\"payload_on\":\"1\",";
      json += "\"payload_off\":\"0\",";
    }

    // Zařízení – všechny entity patří pod jedno zařízení
    json += "\"device\":{";
    json += "\"identifiers\":[\"";
    json += devId;
    json += "\"],";
    json += "\"name\":\"Elektroměr AMM\",";
    json += "\"model\":\"AMM ESP32\",";
    json += "\"manufacturer\":\"Milan Kučera\"";
    json += "}";

    json += "}";

    mqtt.publish(discoveryTopic, json.c_str(), true);  // retain = true
  }

  // RTC baterie – binary sensor
  {
    char discoveryTopic[128];
    snprintf(discoveryTopic, sizeof(discoveryTopic),
             "homeassistant/binary_sensor/%s/rtc_battery/config", devId);

    String json;
    json.reserve(384);
    json += "{";
    json += "\"name\":\"RTC baterie\",";
    json += "\"unique_id\":\"";      json += devId; json += "_rtc_battery\",";
    json += "\"state_topic\":\"amm/rtc/battery\",";
    json += "\"device_class\":\"battery\",";
    json += "\"payload_on\":\"ON\",";
    json += "\"payload_off\":\"OFF\",";
    json += "\"device\":{";
    json += "\"identifiers\":[\"";   json += devId; json += "\"],";
    json += "\"name\":\"Elektroměr AMM\",";
    json += "\"model\":\"AMM ESP32\",";
    json += "\"manufacturer\":\"Milan Kučera\"";
    json += "}";
    json += "}";

    mqtt.publish(discoveryTopic, json.c_str(), true);
  }

  // RTC ACK – tlačítko pro potvrzení výměny baterie
  {
    char discoveryTopic[128];
    snprintf(discoveryTopic, sizeof(discoveryTopic),
             "homeassistant/button/%s/rtc_ack/config", devId);

    String json;
    json.reserve(384);
    json += "{";
    json += "\"name\":\"Potvrdit výměnu RTC baterie\",";
    json += "\"unique_id\":\"";    json += devId; json += "_rtc_ack\",";
    json += "\"command_topic\":\"amm/rtc/ack\",";
    json += "\"payload_press\":\"ACK\",";
    json += "\"device_class\":\"restart\",";
    json += "\"device\":{";
    json += "\"identifiers\":[\""; json += devId; json += "\"],";
    json += "\"name\":\"Elektroměr AMM\",";
    json += "\"model\":\"AMM ESP32\",";
    json += "\"manufacturer\":\"Milan Kučera\"";
    json += "}";
    json += "}";

    mqtt.publish(discoveryTopic, json.c_str(), true);
  }

  DBG("HA Discovery published");
}

//============Publish HA===========
void publishHA() {
  if (!ensureMQTT()) return;

  haReady = true;

if (rtcMqttPending != 0) {
  DBG_VAL("", "[HA] publishing RTC battery state, pending=");
  DBG_VAL("", rtcMqttPending);

  mqtt.publish(
    "amm/rtc/battery",
    rtcMqttPending == 1 ? "ON" : "OFF",
    true
  );

  rtcMqttPending = 0;
}



  for (size_t i = 0; i < OBIS_REG_COUNT; i++) {
    ObisEntry &e = g_obisReg[i];
    if (!e.valid || !e.obisCode) continue;

    char topic[128];
    char obisTopic[64];

    obisToTopic(e.obisCode, obisTopic, sizeof(obisTopic));
    snprintf(topic, sizeof(topic), "amm/obis/%s", obisTopic);

    switch (e.type) {
      case OBIS_TYPE_NUMBER:
        mqtt.publish(topic, String(e.valueNumber, 3).c_str(), true);
        break;

      case OBIS_TYPE_STRING:
        mqtt.publish(topic, e.valueString ? e.valueString : "", true);
        break;

      case OBIS_TYPE_BOOL:
        mqtt.publish(topic, e.valueBool ? "1" : "0", true);
        break;
    }
  }
}




// ==============Otevírání SD souborů===============

void openLogFileForDate(const char* dateStr) {
  if (!sdOk || !dateStr || !dateStr[0]) return;

  if (currentLogFile) {
    currentLogFile.close();
  }

  // Uložíme si aktuální "datum" (buď skutečné, nebo fallback)
  strncpy(currentLogDate, dateStr, sizeof(currentLogDate));
  currentLogDate[sizeof(currentLogDate) - 1] = '\0';

  char path[32];
  // Soubor: /logs/DD.MM.YYYY.csv nebo /logs/dayXXX.csv (fallback)
  snprintf(path, sizeof(path), "/logs/%s.csv", currentLogDate);

  bool exists = SD.exists(path);
  currentLogFile = SD.open(path, FILE_APPEND);

  if (!currentLogFile) {
    DBG_VAL("Nelze otevrit log soubor: ", path);
    return;
  }

  DBG_VAL("Loguji do souboru: ", path);

// Nový soubor → zapíšeme UTF-8 BOM + hlavičku
if (!exists) {
    // --- ZAPSÁNÍ UTF-8 BOM ---
    currentLogFile.write(0xEF);
    currentLogFile.write(0xBB);
    currentLogFile.write(0xBF);

    // --- HLAVIČKA CSV ---
    currentLogFile.println(
      "čas;"
      "Výrobní číslo;"
      "Tarif;"
      "Limiter;"
      "Odpojovač;"
      "P+ Aktuální odběr celkem;"
      "Aktuální odběr L1;"
      "Aktuální odběr L2;"
      "Aktuální odběr L3;"
      "P- Aktuální dodávka celkem;"
      "Aktuální dodávka L1;"
      "Aktuální dodávka L2;"
      "Aktuální dodávka L3;"
      "Celková odebraná energie;"
      "Odběr v T1;"
      "Odběr v T2;"
      "Odběr v T3;"
      "Odběr v T4;"
      "Celková dodávka;"
      "Relé 1;"
      "Relé 2;"
      "Relé 3;"
      "Relé 4"
    );
    currentLogFile.flush();
}


}

// ====== Udržování limitu velikosti logů na SD ======

void maintainLogStorage() {
  if (!sdOk) return;

  File dir = SD.open("/logs");
  if (!dir || !dir.isDirectory()) {
    DBG("maintainLogStorage: adresar /logs neexistuje");
    return;
  }

  // Vypočítáme celkovou velikost logů
  uint64_t totalBytes = 0;

  String oldestName;
  uint32_t oldestTime = UINT32_MAX;

  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String name = f.name();
      if (name.endsWith(".csv")) {

        uint64_t sz = f.size();
        totalBytes += sz;

        uint32_t t = f.getLastWrite();  // čas poslední změny
        if (t < oldestTime) {
          oldestTime = t;
          oldestName = name;
        }
      }
    }
    f = dir.openNextFile();
  }

  dir.close();

  // Pokud nejsme nad limitem → OK
  if (g_logMaxBytes == 0 || totalBytes <= g_logMaxBytes) {
    return;
  }

  // Nemáme co mazat
  if (oldestName.length() == 0) return;

  DBG("maintainLogStorage: prekrocen limit, total=");
  DBG_VAL("", totalBytes);
  DBG_VAL(" bytes, limit=", g_logMaxBytes);

  DBG_VAL("Mazu nejstarsi log: ", oldestName);

  SD.remove(oldestName);
}


//==============Zapisování do řádku SD===============

void logCurrentObis() {
  if (!sdOk) return;

  char dateBuf[16];

  // Zkusit získat skutečné datum z NTP
  if (!getCurrentDateString(dateBuf, sizeof(dateBuf))) {
    // NTP ještě nefunguje → fallback den podle uptime
    uint32_t nowSec = millis() / 1000;
    long dayIdx = nowSec / 86400;
    snprintf(dateBuf, sizeof(dateBuf), "day%03ld", dayIdx);
  }

  // Pokud se změnilo "datum" → otevřít nový soubor
  if (strcmp(dateBuf, currentLogDate) != 0) {
    openLogFileForDate(dateBuf);
  }

  if (!currentLogFile) return;

  // Připravit čas HH:MM:SS
  char timeBuf[9] = "00:00:00";
  struct tm timeinfo;
  if (getLocalTime(&timeinfo, 1000)) {
    snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d:%02d",
             timeinfo.tm_hour,
             timeinfo.tm_min,
             timeinfo.tm_sec);
  } else {
    // fallback z uptime, kdyby NTP vypadlo
    uint32_t nowSec = millis() / 1000;
    uint32_t daySec = nowSec % 86400;
    uint16_t h = daySec / 3600;
    uint8_t  m = (daySec % 3600) / 60;
    uint8_t  s = daySec % 60;
    snprintf(timeBuf, sizeof(timeBuf), "%02u:%02u:%02u", h, m, s);
  }

  ObisEntry e;

  // 1) čas
  currentLogFile.print(timeBuf);
  currentLogFile.print(';');

// 2) Výrobní číslo (0-0:96.1.1.255)
char currentSN[16] = "";

if (obisRegGetByObis("0-0:96.1.1.255", e) && e.valid) {
  strncpy(currentSN, e.valueString, sizeof(currentSN));
  currentSN[sizeof(currentSN) - 1] = '\0';
}

// běžný zápis SN do datového řádku
if (currentSN[0] != '\0') {
  currentLogFile.print(currentSN);
}
currentLogFile.print(';');


  // 3) Tarif (0-0:96.14.0.255)
  if (obisRegGetByObis("0-0:96.14.0.255", e) && e.valid) {
    currentLogFile.print(e.valueString);
  }
  currentLogFile.print(';');

  // 4) Limiter (0-0:17.0.0.255)
  if (obisRegGetByObis("0-0:17.0.0.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 5) Odpojovač (0-0:96.3.10.255)
  if (obisRegGetByObis("0-0:96.3.10.255", e) && e.valid) {
    currentLogFile.print(e.valueBool ? 1 : 0);
  }
  currentLogFile.print(';');

  // 6) P+ Aktuální odběr celkem (1-0:1.7.0.255)
  if (obisRegGetByObis("1-0:1.7.0.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 7) Aktuální odběr L1 (1-0:21.7.0.255)
  if (obisRegGetByObis("1-0:21.7.0.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 8) Aktuální odběr L2 (1-0:41.7.0.255)
  if (obisRegGetByObis("1-0:41.7.0.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 9) Aktuální odběr L3 (1-0:61.7.0.255)
  if (obisRegGetByObis("1-0:61.7.0.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 10) P- Aktuální dodávka celkem (1-0:2.7.0.255)
  if (obisRegGetByObis("1-0:2.7.0.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 11) Aktuální dodávka L1 (1-0:22.7.0.255)
  if (obisRegGetByObis("1-0:22.7.0.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 12) Aktuální dodávka L2 (1-0:42.7.0.255)
  if (obisRegGetByObis("1-0:42.7.0.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 13) Aktuální dodávka L3 (1-0:62.7.0.255)
  if (obisRegGetByObis("1-0:62.7.0.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 14) Celková odebraná energie (1-0:1.8.0.255)
  if (obisRegGetByObis("1-0:1.8.0.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 15) Odběr v T1 (1-0:1.8.1.255)
  if (obisRegGetByObis("1-0:1.8.1.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 16) Odběr v T2 (1-0:1.8.2.255)
  if (obisRegGetByObis("1-0:1.8.2.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 17) Odběr v T3 (1-0:1.8.3.255)
  if (obisRegGetByObis("1-0:1.8.3.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 18) Odběr v T4 (1-0:1.8.4.255)
  if (obisRegGetByObis("1-0:1.8.4.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 19) Celková dodávka (1-0:2.8.0.255)
  if (obisRegGetByObis("1-0:2.8.0.255", e) && e.valid) {
    currentLogFile.print(e.valueNumber, 3);
  }
  currentLogFile.print(';');

  // 20) Relé 1 (0-1:96.3.10.255)
  if (obisRegGetByObis("0-1:96.3.10.255", e) && e.valid) {
    currentLogFile.print(e.valueBool ? 1 : 0);
  }
  currentLogFile.print(';');

  // 21) Relé 2 (0-2:96.3.10.255)
  if (obisRegGetByObis("0-2:96.3.10.255", e) && e.valid) {
    currentLogFile.print(e.valueBool ? 1 : 0);
  }
  currentLogFile.print(';');

  // 22) Relé 3 (0-3:96.3.10.255)
  if (obisRegGetByObis("0-3:96.3.10.255", e) && e.valid) {
    currentLogFile.print(e.valueBool ? 1 : 0);
  }
  currentLogFile.print(';');

  // 23) Relé 4 (0-4:96.3.10.255) – poslední sloupec BEZ středníku
  if (obisRegGetByObis("0-4:96.3.10.255", e) && e.valid) {
    currentLogFile.print(e.valueBool ? 1 : 0);
  }

  currentLogFile.println();
  currentLogFile.flush();
  maintainLogStorage();
}


// ========== DLMS / OBIS PARSER ==========

static bool readUInt32BE(const uint8_t* buf, size_t len, size_t pos, uint32_t& outVal) {
  if (pos + 4 > len) return false;
  outVal = (uint32_t(buf[pos])   << 24) |
           (uint32_t(buf[pos+1]) << 16) |
           (uint32_t(buf[pos+2]) << 8)  |
            uint32_t(buf[pos+3]);
  return true;
}

static bool readInt32BE(const uint8_t* buf, size_t len, size_t pos, int32_t& outVal) {
  if (pos + 4 > len) return false;
  uint32_t u =
    (uint32_t(buf[pos])   << 24) |
    (uint32_t(buf[pos+1]) << 16) |
    (uint32_t(buf[pos+2]) << 8)  |
     uint32_t(buf[pos+3]);
  outVal = (int32_t)u;  // převedeme na signed (two's complement)
  return true;
}


static bool matchLN(const uint8_t* frame, size_t len, size_t pos, const uint8_t ln[6]) {
  if (pos + 6 > len) return false;
  for (int i = 0; i < 6; i++) {
    if (frame[pos + i] != ln[i]) return false;
  }
  return true;
}

void dlmsParseFrame(const uint8_t* frame, size_t len, uint32_t now) {
  if (!frame || len < 16) return;

  const uint8_t MAX_OBJECTS = 64;
  uint8_t objectsFound = 0;

  const uint8_t LN_SN[]       = {0x00,0x00,0x60,0x01,0x01,0xFF};
  const uint8_t LN_TARIF[]    = {0x00,0x00,0x60,0x0E,0x00,0xFF};
  const uint8_t LN_DISC[]     = {0x00,0x00,0x60,0x03,0x0A,0xFF};
  const uint8_t LN_LIMIT[]    = {0x00,0x00,0x11,0x00,0x00,0xFF};

  const uint8_t LN_R1[]       = {0x00,0x01,0x60,0x03,0x0A,0xFF};
  const uint8_t LN_R2[]       = {0x00,0x02,0x60,0x03,0x0A,0xFF};
  const uint8_t LN_R3[]       = {0x00,0x03,0x60,0x03,0x0A,0xFF};
  const uint8_t LN_R4[]       = {0x00,0x04,0x60,0x03,0x0A,0xFF};

  const uint8_t LN_P_P_TOT[]  = {0x01,0x00,0x01,0x07,0x00,0xFF};
  const uint8_t LN_P_P_L1[]   = {0x01,0x00,0x15,0x07,0x00,0xFF};
  const uint8_t LN_P_P_L2[]   = {0x01,0x00,0x29,0x07,0x00,0xFF};
  const uint8_t LN_P_P_L3[]   = {0x01,0x00,0x3D,0x07,0x00,0xFF};

  const uint8_t LN_P_M_TOT[]  = {0x01,0x00,0x02,0x07,0x00,0xFF};
  const uint8_t LN_P_M_L1[]   = {0x01,0x00,0x16,0x07,0x00,0xFF};
  const uint8_t LN_P_M_L2[]   = {0x01,0x00,0x2A,0x07,0x00,0xFF};
  const uint8_t LN_P_M_L3[]   = {0x01,0x00,0x3E,0x07,0x00,0xFF};

  const uint8_t LN_A_P_TOT[]  = {0x01,0x00,0x01,0x08,0x00,0xFF};
  const uint8_t LN_A_P_T1[]   = {0x01,0x00,0x01,0x08,0x01,0xFF};
  const uint8_t LN_A_P_T2[]   = {0x01,0x00,0x01,0x08,0x02,0xFF};
  const uint8_t LN_A_P_T3[]   = {0x01,0x00,0x01,0x08,0x03,0xFF};
  const uint8_t LN_A_P_T4[]   = {0x01,0x00,0x01,0x08,0x04,0xFF};

  const uint8_t LN_A_M_TOT[]  = {0x01,0x00,0x02,0x08,0x00,0xFF};

  for (size_t pos = 0; pos + 6 < len && objectsFound < MAX_OBJECTS; pos++) {

    // SN
    if (matchLN(frame, len, pos, LN_SN)) {
      size_t p = pos + 6;
      if (p + 3 <= len) {
        uint8_t tag = frame[p];
        uint8_t dt  = frame[p+1];
        uint8_t l   = frame[p+2];
        if (tag == 0x02 && dt == 0x09 && l > 0 && p + 3 + l <= len) {
          char tmp[16];
          size_t copyLen = l;
          if (copyLen >= sizeof(tmp)) copyLen = sizeof(tmp) - 1;
          for (size_t i = 0; i < copyLen; i++) tmp[i] = (char)frame[p+3+i];
          tmp[copyLen] = '\0';
          obisRegSetStringByLN(LN_SN, tmp, now);
          objectsFound++;
        }
      }
      continue;
    }

    // Tarif
    if (matchLN(frame, len, pos, LN_TARIF)) {
      size_t p = pos + 6;
      if (p + 3 <= len) {
        uint8_t tag = frame[p];
        uint8_t dt  = frame[p+1];
        uint8_t l   = frame[p+2];
        if (tag == 0x02 && dt == 0x09 && l > 0 && p + 3 + l <= len) {
          char tmp[16];
          size_t copyLen = l;
          if (copyLen >= sizeof(tmp)) copyLen = sizeof(tmp) - 1;
          for (size_t i = 0; i < copyLen; i++) tmp[i] = (char)frame[p+3+i];
          tmp[copyLen] = '\0';
          obisRegSetStringByLN(LN_TARIF, tmp, now);
          objectsFound++;
        }
      }
      continue;
    }

    // Odpojovač
    if (matchLN(frame, len, pos, LN_DISC)) {
      size_t p = pos + 6;
      if (p + 3 <= len) {
        uint8_t v = frame[p+2];
        bool state = (v != 0);
        obisRegSetBoolByLN(LN_DISC, state, now);
        objectsFound++;
      }
      continue;
    }

// --- Limiter  ---
    if (matchLN(frame, len, pos, LN_LIMIT)) {
      size_t p = pos + 6;
      if (p + 2 + 4 <= len) {
        uint8_t tag = frame[p];
        uint8_t dt  = frame[p+1];
        if (tag == 0x03 && dt == 0x06) {
         int32_t raw = 0;
         if (readInt32BE(frame, len, p+2, raw)) {
          double value = (double)raw; 
          obisRegSetNumberByLN(LN_LIMIT, value, now);
          objectsFound++;
      }
    }
  }
  continue;
}


    // Relé
    auto handleRelay = [&](const uint8_t ln[6]) {
      size_t p2 = pos + 6;
      if (p2 + 3 <= len) {
        uint8_t v = frame[p2+2];
        bool state = (v != 0);
        obisRegSetBoolByLN(ln, state, now);
        objectsFound++;
      }
    };

    if (matchLN(frame, len, pos, LN_R1)) { handleRelay(LN_R1); continue; }
    if (matchLN(frame, len, pos, LN_R2)) { handleRelay(LN_R2); continue; }
    if (matchLN(frame, len, pos, LN_R3)) { handleRelay(LN_R3); continue; }
    if (matchLN(frame, len, pos, LN_R4)) { handleRelay(LN_R4); continue; }

    // Výkony P+, P-
     auto handlePower = [&](const uint8_t ln[6]) {
       size_t p2 = pos + 6;
       if (p2 + 2 + 4 > len) return;
         uint8_t tag2 = frame[p2];
         uint8_t dt2  = frame[p2+1];
         if (tag2 == 0x02 && dt2 == 0x06) {  // signed 32-bit
         int32_t raw = 0;
         if (readInt32BE(frame, len, p2+2, raw)) {
         double value = (double)raw;     // zatím žádný scaler, chápeme jako W
         obisRegSetNumberByLN(ln, value, now);
         objectsFound++;
    }
  }
};


    if (matchLN(frame, len, pos, LN_P_P_TOT)) { handlePower(LN_P_P_TOT); continue; }
    if (matchLN(frame, len, pos, LN_P_P_L1 )) { handlePower(LN_P_P_L1 ); continue; }
    if (matchLN(frame, len, pos, LN_P_P_L2 )) { handlePower(LN_P_P_L2 ); continue; }
    if (matchLN(frame, len, pos, LN_P_P_L3 )) { handlePower(LN_P_P_L3 ); continue; }

    if (matchLN(frame, len, pos, LN_P_M_TOT)) { handlePower(LN_P_M_TOT); continue; }
    if (matchLN(frame, len, pos, LN_P_M_L1 )) { handlePower(LN_P_M_L1 ); continue; }
    if (matchLN(frame, len, pos, LN_P_M_L2 )) { handlePower(LN_P_M_L2 ); continue; }
    if (matchLN(frame, len, pos, LN_P_M_L3 )) { handlePower(LN_P_M_L3 ); continue; }

    // Energie A+ / A-
     auto handleEnergy = [&](const uint8_t ln[6]) {
       size_t p2 = pos + 6;
       if (p2 + 2 + 4 > len) return;
         uint8_t tag2 = frame[p2];
         uint8_t dt2  = frame[p2+1];
         if (tag2 == 0x02 && dt2 == 0x06) {    // signed 32-bit
         int32_t raw = 0;
         if (readInt32BE(frame, len, p2+2, raw)) {
      // Podle logů: raw je v Wh → dělíme 1000 → kWh
         double value = ((double)raw) / 1000.0;
         obisRegSetNumberByLN(ln, value, now);
         objectsFound++;
    }
  }
};


    if (matchLN(frame, len, pos, LN_A_P_TOT)) { handleEnergy(LN_A_P_TOT); continue; }
    if (matchLN(frame, len, pos, LN_A_P_T1 )) { handleEnergy(LN_A_P_T1 ); continue; }
    if (matchLN(frame, len, pos, LN_A_P_T2 )) { handleEnergy(LN_A_P_T2 ); continue; }
    if (matchLN(frame, len, pos, LN_A_P_T3 )) { handleEnergy(LN_A_P_T3 ); continue; }
    if (matchLN(frame, len, pos, LN_A_P_T4 )) { handleEnergy(LN_A_P_T4 ); continue; }

    if (matchLN(frame, len, pos, LN_A_M_TOT)) { handleEnergy(LN_A_M_TOT); continue; }
  }
}

// ========== RS485 RÁMCE (časový gap) ==========

static uint8_t  rsBuf[600];
static size_t   rsLen = 0;
static uint32_t rsLastByteMs = 0;
const  uint32_t RS_GAP_MS = 80; // mezera >80 ms = konec rámce

void handleRS485() {
  while (RS485.available()) {
    int b = RS485.read();
    if (b < 0) break;

    if (rsLen < sizeof(rsBuf)) {
      rsBuf[rsLen++] = (uint8_t)b;
    } else {
      rsLen = 0; // overflow
    }
    rsLastByteMs = millis();
  }

  if (rsLen > 0) {
    uint32_t nowMs = millis();
    if (nowMs - rsLastByteMs > RS_GAP_MS) {
      uint32_t nowSec = nowMs / 1000;

      dlmsParseFrame(rsBuf, rsLen, nowSec);

      if (rsLen > 100) {     // velká vlna → OBIS data
        checkMeterReplacement();
        logCurrentObis();    // už bez parametru
        publishHA();
      }

      rsLen = 0;

    }
  }
}

// =======================================
// Inicializace času – NTP + Timezone
// =======================================
void initTime() {
  // NTP synchronizace – vždy UTC
  configTime(0, 0, NTP_SERVER);
  DBG("NTP: configTime zavolano (UTC)");

  // Timezone pro Evropu/Prague (CET / CEST)
  setenv("TZ", "CET-1CEST,M3.5.0/2,M10.5.0/3", 1);
  tzset();

  DBG("NTP: TZ nastavena (CET/CEST)");
}


// =======================================
// Jednorázová synchronizace času z NTP
// =======================================
bool syncTimeOnce(uint8_t maxAttempts = 10) {
  struct tm timeinfo;

  for (uint8_t i = 0; i < maxAttempts; i++) {
    // getLocalTime už respektuje TZ (CET / CEST)
    if (getLocalTime(&timeinfo, 2000)) {
      timeSynced = true;

      rtcUpdateFromSystem();

      DBG("NTP: cas synchronizovan: ");
      DBG_FMT(
        "%02d.%02d.%04d %02d:%02d:%02d\n",
        timeinfo.tm_mday,
        timeinfo.tm_mon + 1,
        timeinfo.tm_year + 1900,
        timeinfo.tm_hour,
        timeinfo.tm_min,
        timeinfo.tm_sec
      );

      return true;
    }

    DBG("NTP: cekam...");
    delay(500);
  }

  DBG("NTP: nepodarilo se synchronizovat cas");
  return false;
}



// ========== WIFI + WEB ==========

void startAPMode() {
  g_wifiMode = APP_WIFI_AP;

  WiFi.disconnect(true);
  WiFi.mode(WIFI_AP);
  String apName = "AMM-ESP32";
  String apPass = "12345678";   // Vlastní heslo, min. 8 znaků

  WiFi.softAP(apName.c_str(), apPass.c_str());


  IPAddress ip = WiFi.softAPIP();

  // DNS captive portal – VŠECHNO → ESP32
  dnsServer.start(DNS_PORT, "*", ip);

  Serial.printf("AP mód, IP: %s\n", ip.toString().c_str());
}


void initWiFi() {
  prefs.begin("amm", true);
  String ssid = prefs.getString("wifi_ssid", "");
  String pass = prefs.getString("wifi_pass", "");
  prefs.end();

  if (ssid.length() == 0) {
    Serial.println(F("WiFi: neni ulozena -> AP"));
    startAPMode();
    return;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid.c_str(), pass.c_str());

  DBG_VAL("WiFi: pripojuji k ", ssid);

  unsigned long start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
    delay(500);
    DBG_VAL("", '.');
  }
  DBG_VAL("", );

if (WiFi.status() == WL_CONNECTED) {
  g_wifiMode = APP_WIFI_STA;
  Serial.printf("WiFi OK, IP: %s\n", WiFi.localIP().toString().c_str());

  initTime();
  syncTimeOnce();

  // ===== Home Assistant / MQTT =====
  loadHAConfig();

  if (haConfig.enabled && strlen(haConfig.host) > 0) {
    mqtt.setBufferSize(MQTT_BUFFER_SIZE);
    mqtt.setServer(haConfig.host, haConfig.port);
    mqtt.setCallback(mqttCallback);
    DBG("HA MQTT enabled, broker: ");
    DBG_VAL("", haConfig.host);
    DBG_VAL(":", haConfig.port);
    publishHA();
  } else {
    DBG("HA MQTT disabled");
  }

} else {
  Serial.println(F("WiFi FAIL -> AP"));
  startAPMode();
}

}
//============RTC detekce===========
bool rtcDetect() {
  Wire.beginTransmission(0x68);
  return (Wire.endTransmission() == 0);
}

void rtcUpdateFromSystem() {
  if (!rtcPresent) return;

  time_t now = time(nullptr);
  struct tm* t = localtime(&now);

  // ===== zapsat čas do RTC =====
  Wire.beginTransmission(0x68);
  Wire.write(0x00); // sekundy
  Wire.write(t->tm_sec);
  Wire.write(t->tm_min);
  Wire.write(t->tm_hour);
  Wire.write(t->tm_wday + 1);
  Wire.write(t->tm_mday);
  Wire.write(t->tm_mon + 1);
  Wire.write(t->tm_year - 100);
  Wire.endTransmission();

  DBG("[RTC] time written from NTP");
  // OSF bit záměrně NESMAŽEME – maže ho pouze uživatel zavřením eventu v UI.
}



bool rtcReadToSystemTime() {
  if (!rtcPresent) return false;

  Wire.beginTransmission(0x68);
  Wire.write(0x00); // start at seconds
  if (Wire.endTransmission() != 0) return false;

  Wire.requestFrom(0x68, 7);
  if (Wire.available() < 7) return false;

  uint8_t sec   = Wire.read();
  uint8_t min   = Wire.read();
  uint8_t hour  = Wire.read();
  Wire.read(); // day of week (ignored)
  uint8_t day   = Wire.read();
  uint8_t month = Wire.read();
  uint8_t year  = Wire.read();

  auto bcd2dec = [](uint8_t v) {
    return (v >> 4) * 10 + (v & 0x0F);
  };

  struct tm t {};
  t.tm_sec  = bcd2dec(sec & 0x7F);
  t.tm_min  = bcd2dec(min);
  t.tm_hour = bcd2dec(hour & 0x3F);
  t.tm_mday = bcd2dec(day);
  t.tm_mon  = bcd2dec(month & 0x1F) - 1;
  t.tm_year = bcd2dec(year) + 100; // since 1900

  time_t epoch = mktime(&t);
  struct timeval tv = { .tv_sec = epoch, .tv_usec = 0 };
  settimeofday(&tv, nullptr);

  DBG("[RTC] system time set from RTC");
  return true;
}

bool rtcCheckBattery() {
  if (!rtcPresent) return false;

  Wire.beginTransmission(0x68);
  Wire.write(0x0F); // status register
  if (Wire.endTransmission() != 0) return false;

  Wire.requestFrom(0x68, 1);
  if (!Wire.available()) return false;

  uint8_t status = Wire.read();
  return status & 0x80; // OSF bit
}



String htmlEscape(const char* s) {
  if (!s) return String();
  String out;
  out.reserve(strlen(s) + 8);
  for (size_t i = 0; s[i]; i++) {
    switch (s[i]) {
      case '&':  out += F("&amp;");  break;
      case '<':  out += F("&lt;");   break;
      case '>':  out += F("&gt;");   break;
      case '"':  out += F("&quot;"); break;
      case '\'': out += F("&#39;");  break;
      default:   out += s[i];        break;
    }
  }
  return out;
}


// ========== Helpery pro čtení z OBISReg ==========
double getObisNumber(const char* obis, double def = 0.0) {
  ObisEntry e;
  if (obisRegGetByObis(obis, e) && e.valid && e.type == OBIS_TYPE_NUMBER) {
    return e.valueNumber;
  }
  return def;
}

String getObisString(const char* obis) {
  ObisEntry e;
  if (obisRegGetByObis(obis, e) && e.valid && e.type == OBIS_TYPE_STRING) {
    return String(e.valueString);
  }
  return String();
}

bool getObisBool(const char* obis, bool def = false) {
  ObisEntry e;
  if (obisRegGetByObis(obis, e) && e.valid && e.type == OBIS_TYPE_BOOL) {
    return e.valueBool;
  }
  return def;
}

bool isAP() {
  return g_wifiMode == APP_WIFI_AP;
}

bool requireSTA(AsyncWebServerRequest *request) {
  if (isAP()) {
    request->redirect("/wifi");
    return false;
  }
  return true;
}

void clearLogsDirectory() {
  if (!sdOk) return;

  File dir = SD.open("/logs");
  if (!dir || !dir.isDirectory()) {
    DBG("Factory reset: /logs neexistuje");
    return;
  }

  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String name = f.name();
      String fullPath = String("/logs/") + name;

      DBG_VAL("Mazani logu: ", fullPath);

      SD.remove(fullPath);
    }
    f = dir.openNextFile();
  }
  dir.close();

  DBG("Vsechny logy smazany");
}

void clearEventsDirectory() {
  if (!sdOk) return;

  File dir = SD.open("/events");
  if (!dir || !dir.isDirectory()) {
    DBG("Factory reset: /events neexistuje");
    return;
  }

  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      String name = f.name();
      String fullPath = String("/events/") + name;

      DBG_VAL("Mazani eventu: ", fullPath);

      SD.remove(fullPath);
    }
    f = dir.openNextFile();
  }
  dir.close();

  DBG("Vsechny eventy smazany");
}




// ========== GRAPH STATE MACHINE ==========

enum GraphState {
  GRAPH_IDLE,
  GRAPH_PROCESSING,
  GRAPH_DONE,
  GRAPH_ERROR
};

struct GraphJob {
  GraphState  state       = GRAPH_IDLE;
  char        inPath[32]  = "";
  char        csvIndex_   = -1;
  File        outFile;
  File        inFile;
  bool        firstPoint  = true;
  bool        tariffInit  = false;
  uint8_t     lastTariff  = 0;
  String      tariff      = "";
  String      apiDate     = "";
  int         csvIndex    = -1;
  uint8_t     linesChunk  = 0;
};

static GraphJob g_graph;
static const char* GRAPH_TMP = "/tmp_graph.json";
static const char* GRAPH_LOG_TMP = "/tmp_log.csv";

void graphProcess() {
  if (g_graph.state != GRAPH_PROCESSING) return;

  // Přečteme max 50 řádků najednou
  const uint8_t CHUNK = 50;
  uint8_t count = 0;

  while (g_graph.inFile.available() && count < CHUNK) {
    String line = g_graph.inFile.readStringUntil('\n');
    line.trim();
    if (!line.length()) continue;

    char buf[256];
    line.toCharArray(buf, sizeof(buf));
    char* cols[32];
    int colCount = 0;
    char* tok = strtok(buf, ";");
    while (tok && colCount < 32) { cols[colCount++] = tok; tok = strtok(nullptr, ";"); }

    // Tariff
    if (colCount > 2) {
      uint8_t t;
      if (parseTariff(cols[2], t)) {
        if (!g_graph.tariffInit) {
          char tr[32];
          snprintf(tr, sizeof(tr), "[\"%s\",%d]", cols[0], t);
          g_graph.tariff += tr;
          g_graph.lastTariff = t; g_graph.tariffInit = true;
        } else if (t != g_graph.lastTariff) {
          char tr[32];
          snprintf(tr, sizeof(tr), ",[\"%s\",%d]", cols[0], t);
          g_graph.tariff += tr;
          g_graph.lastTariff = t;
        }
      }
    }

    // Point
    if (g_graph.csvIndex >= 0 && colCount > g_graph.csvIndex) {
      double val = atof(cols[g_graph.csvIndex]);
      if (!isnan(val) && !isinf(val)) {
        char pt[48];
        snprintf(pt, sizeof(pt), "%s[\"%s\",%.1f]",
                 g_graph.firstPoint ? "" : ",", cols[0], val);
        g_graph.outFile.print(pt);
        g_graph.firstPoint = false;
      }
    }
    count++;
  }

  // Flush po každém chunku
  g_graph.outFile.flush();

  // Konec souboru
  if (!g_graph.inFile.available()) {
    g_graph.inFile.close();
    // Zapsat zbytek JSON
    g_graph.outFile.printf("],\"tariff\":[%s]}", g_graph.tariff.c_str());
    g_graph.outFile.close();
    g_graph.state = GRAPH_DONE;
    DBG("[Graph] done");
  }
}




void setupWeb() {
 //---------Tovární reset-------
server.on("/factory-reset", HTTP_POST, [](AsyncWebServerRequest *request){
  prefs.begin("amm", false);
  prefs.clear();
  prefs.end();

if (sdOk) {
  clearLogsDirectory();
  clearEventsDirectory();
}

  delay(1000);
  ESP.restart();
});


 //---------Reset wifi----------
  server.on("/reset-wifi", HTTP_POST, [](AsyncWebServerRequest *request){
  prefs.begin("amm", false);
  prefs.remove("wifi_ssid");
  prefs.remove("wifi_pass");
  prefs.end();

  request->send(200, "text/html",
    "<html><body><h2>Wi-Fi resetována. Restartuji…</h2></body></html>");

  delay(1000);
  ESP.restart();
});


  // ---- WiFi setup stránka (AP mód) ----
  server.on("/wifi", HTTP_GET, [](AsyncWebServerRequest *request){
    String page;
    page.reserve(2500);

page += F(
  "<!DOCTYPE html><html lang='cs'><head>"
  "<meta charset='utf-8'>"
  "<meta name='viewport' content='width=device-width, initial-scale=1'>"
  "<title>Nastavení Wi-Fi</title>"
  "<style>"
  "body{margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;"
  "font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
  "background:#020617;color:#e5e7eb;}"
  ".card{width:100%;max-width:420px;background:#020617;"
  "border:1px solid #1f2937;border-radius:14px;"
  "box-shadow:0 20px 40px rgba(0,0,0,.6);padding:24px;}"
  "h1{margin:0 0 12px 0;font-size:20px;text-align:center;}"
  "label{display:block;font-size:13px;color:#9ca3af;margin-top:12px;}"
  "select,input{width:100%;padding:10px;margin-top:6px;"
  "background:#020617;color:#e5e7eb;border:1px solid #1f2937;"
  "border-radius:8px;font-size:14px;}"
  " button{width:100%;margin-top:18px;padding:10px;"
  " background:#0369a1;color:#e5e7eb;border:none;"
  " border-radius:10px;font-size:14px;cursor:pointer;}"
  "button:hover{background:#0284c7;}"
  ".btn-secondary{margin-top:10px;background:#020617;"
  "border:1px solid #1f2937;color:#9ca3af;}"
  ".btn-secondary:hover{background:#020617;color:#e5e7eb;}"
  ".hint{font-size:12px;color:#6b7280;margin-top:12px;text-align:center;}"

  "</style></head><body>"
  "<div class='card'>"
  "<h1>Připojení k Wi-Fi</h1>"
  "<form method='POST' action='/wifi'>"
  "<label>Vyber síť</label>"
  "<select name='ssid'>"
);


    int n = WiFi.scanNetworks();
    for (int i = 0; i < n; i++) {
      page += "<option value='";
      page += WiFi.SSID(i);
      page += "'>";
      page += WiFi.SSID(i);
      page += " (";
      page += String(WiFi.RSSI(i));
      page += " dBm)</option>";
    }

page += F(
  "</select>"
  "<label>Heslo</label>"
  "<input type='password' name='pass' placeholder='(pokud je potřeba)'>"
  "<button type='submit'>Připojit se</button>"
  "<button type='button' class='btn-secondary' onclick='location.reload()'>"
  "Znovu vyhledat Wi-Fi"
  "</button>"

  "<div class='hint'>Zařízení se po uložení restartuje</div>"
  "</form>"
  "</div>"
  "</body></html>"
);


    request->send(200, "text/html; charset=utf-8", page);
  });

  server.on("/wifi", HTTP_POST, [](AsyncWebServerRequest *request){
    if (!request->hasParam("ssid", true)) {
      request->send(400, "text/plain", "SSID chybi");
      return;
    }

    String ssid = request->getParam("ssid", true)->value();
    String pass = request->hasParam("pass", true)
                  ? request->getParam("pass", true)->value()
                  : "";

    prefs.begin("amm", false);
    prefs.putString("wifi_ssid", ssid);
    prefs.putString("wifi_pass", pass);
    prefs.end();

    request->send(200, "text/html",
      "<html><body><h2>Ukladam Wi-Fi a restartuji…</h2></body></html>");

    delay(1000);
    ESP.restart();
  });


  server.on("/api/live", HTTP_GET, [](AsyncWebServerRequest *request){
      if (isAP()) {
    request->send(403, "application/json", "{\"error\":\"AP mode\"}");
    return;
  }
    String json;
    json.reserve(512);

    // Načteme nastavení z Preferences (namespace "amm")
    prefs.begin("amm", true);
    uint8_t elmPh        = prefs.getUChar("elm_ph", 3);         // 1 nebo 3, default 3F
    bool    showPminus   = prefs.getBool("show_pminus", true);  // zobrazit P-?
    bool    tariffFull   = prefs.getBool("tariff_full", true);  // true = T1-4, false = jen T1-2
    bool    energyDecimal= prefs.getBool("energy_decimal", true); // true = desetinná místa
    prefs.end();


    json += F("{");

    // SN a tarif
    json += F("\"sn\":\"");
    json += getObisString("0-0:96.1.1.255");
    json += F("\",");

    json += F("\"tarif\":\"");
    json += getObisString("0-0:96.14.0.255");
    json += F("\",");

    // Limiter a odpojovač
    json += F("\"limiter\":");
    json += String(getObisNumber("0-0:17.0.0.255", 0.0), 3);
    json += F(",");

    json += F("\"odpojovac\":");
    json += (getObisBool("0-0:96.3.10.255", false) ? F("true") : F("false"));
    json += F(",");

    // P+ výkony
    json += F("\"p_p_tot\":");
    json += String(getObisNumber("1-0:1.7.0.255", 0.0), 3);
    json += F(",");

    json += F("\"p_p_l1\":");
    json += String(getObisNumber("1-0:21.7.0.255", 0.0), 3);
    json += F(",");

    json += F("\"p_p_l2\":");
    json += String(getObisNumber("1-0:41.7.0.255", 0.0), 3);
    json += F(",");

    json += F("\"p_p_l3\":");
    json += String(getObisNumber("1-0:61.7.0.255", 0.0), 3);
    json += F(",");

    // P- výkony
    json += F("\"p_m_tot\":");
    json += String(getObisNumber("1-0:2.7.0.255", 0.0), 3);
    json += F(",");

    json += F("\"p_m_l1\":");
    json += String(getObisNumber("1-0:22.7.0.255", 0.0), 3);
    json += F(",");

    json += F("\"p_m_l2\":");
    json += String(getObisNumber("1-0:42.7.0.255", 0.0), 3);
    json += F(",");

    json += F("\"p_m_l3\":");
    json += String(getObisNumber("1-0:62.7.0.255", 0.0), 3);
    json += F(",");

    // Energie A+
    json += F("\"e_p_tot\":");
    json += String(getObisNumber("1-0:1.8.0.255", 0.0), 3);
    json += F(",");

    json += F("\"e_p_t1\":");
    json += String(getObisNumber("1-0:1.8.1.255", 0.0), 3);
    json += F(",");

    json += F("\"e_p_t2\":");
    json += String(getObisNumber("1-0:1.8.2.255", 0.0), 3);
    json += F(",");

    json += F("\"e_p_t3\":");
    json += String(getObisNumber("1-0:1.8.3.255", 0.0), 3);
    json += F(",");

    json += F("\"e_p_t4\":");
    json += String(getObisNumber("1-0:1.8.4.255", 0.0), 3);
    json += F(",");

    // Energie A-
    json += F("\"e_m_tot\":");
    json += String(getObisNumber("1-0:2.8.0.255", 0.0), 3);
    json += F(",");

    // KONFIGURACE PRO WEB UI (podle Nastavení)
    json += F("\"elm_ph\":");
    json += String(elmPh);          // 1 nebo 3
    json += F(",");

    json += F("\"show_pminus\":");
    json += (showPminus ? "true" : "false");
    json += F(",");

    json += F("\"tariff_full\":");
    json += (tariffFull ? "true" : "false");
    json += F(",");

    json += F("\"energy_decimal\":");
    json += (energyDecimal ? "true" : "false");
    json += F(",");

        // ---- uptime & last OBIS update ----
    uint32_t nowSec = millis() / 1000;
    uint32_t lastObis = 0;

    for (size_t i = 0; i < OBIS_REG_COUNT; i++) {
      if (g_obisReg[i].valid && g_obisReg[i].lastUpdate > lastObis) {
        lastObis = g_obisReg[i].lastUpdate;
      }
    }

    json += F("\"uptime\":");
    json += String(nowSec);
    json += F(",");

    json += F("\"last_obis\":");
    json += String(lastObis);
    json += F(",");


    // Relé 1–4
    json += F("\"r1\":");
    json += (getObisBool("0-1:96.3.10.255", false) ? F("true") : F("false"));
    json += F(",");

    json += F("\"r2\":");
    json += (getObisBool("0-2:96.3.10.255", false) ? F("true") : F("false"));
    json += F(",");

    json += F("\"r3\":");
    json += (getObisBool("0-3:96.3.10.255", false) ? F("true") : F("false"));
    json += F(",");

    json += F("\"r4\":");
    json += (getObisBool("0-4:96.3.10.255", false) ? F("true") : F("false"));

    json += F("}");

    request->send(200, "application/json; charset=utf-8", json);
  });

// ====== API: dostupné logy (kalendář) ======
server.on("/api/logs/meta", HTTP_GET, [](AsyncWebServerRequest *request){
  if (!sdOk) {
    request->send(500, "application/json", "{}");
    return;
  }

  bool years[20] = {};
  bool months[20][13] = {};
  bool days[20][13][32] = {};

  int baseYear = 0;

  File dir = SD.open("/logs");
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      const char* name = strrchr(f.name(), '/');
      name = name ? name + 1 : f.name();

      LogFileInfo info;
      if (parseLogFilename(name, info)) {
        if (baseYear == 0 || info.year < baseYear)
          baseYear = info.year;
      }
    }
    f = dir.openNextFile();
  }
  dir.close();

  if (baseYear == 0) {
    request->send(200, "application/json", "{}");
    return;
  }

  dir = SD.open("/logs");
  f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      const char* name = strrchr(f.name(), '/');
      name = name ? name + 1 : f.name();

      LogFileInfo info;
      if (parseLogFilename(name, info)) {
        int y = info.year - baseYear;
        if (y >= 0 && y < 20) {
          years[y] = true;
          months[y][info.month] = true;
          days[y][info.month][info.day] = true;
        }
      }
    }
    f = dir.openNextFile();
  }
  dir.close();

  String json = "{";
  json += "\"baseYear\":" + String(baseYear) + ",";
  json += "\"years\":[";

  bool first = true;
  for (int y = 0; y < 20; y++) {
    if (years[y]) {
      if (!first) json += ",";
      json += String(baseYear + y);
      first = false;
    }
  }
  json += "],\"months\":{";

  first = true;
  for (int y = 0; y < 20; y++) {
    if (!years[y]) continue;
    if (!first) json += ",";
    json += "\"" + String(baseYear + y) + "\":[";
    bool fm = true;
    for (int m = 1; m <= 12; m++) {
      if (months[y][m]) {
        if (!fm) json += ",";
        json += String(m);
        fm = false;
      }
    }
    json += "]";
    first = false;
  }

  json += "},\"days\":{";
  first = true;
  for (int y = 0; y < 20; y++) {
    for (int m = 1; m <= 12; m++) {
      bool any = false;
      for (int d = 1; d <= 31; d++)
        if (days[y][m][d]) any = true;
      if (!any) continue;

      if (!first) json += ",";
      json += "\"" + String(baseYear + y) + "-" + String(m) + "\":[";
      bool fd = true;
      for (int d = 1; d <= 31; d++) {
        if (days[y][m][d]) {
          if (!fd) json += ",";
          json += String(d);
          fd = false;
        }
      }
      json += "]";
      first = false;
    }
  }
  json += "}}";

  request->send(200, "application/json", json);
});


// ====== API: data pro graf ======
server.on("/api/graph", HTTP_GET, [](AsyncWebServerRequest *request){
  if (!sdOk) {
    request->send(500, "application/json", "{\"error\":\"no sd\"}");
    return;
  }
  if (!request->hasParam("metric") || !request->hasParam("files")) {
    request->send(400, "application/json", "{\"error\":\"missing params\"}");
    return;
  }

  // Pokud je zpracování hotové – vrátíme výsledek a resetujeme stav
  if (g_graph.state == GRAPH_DONE) {
    AsyncWebServerResponse* resp = request->beginResponse(SD, GRAPH_TMP, "application/json");
    request->send(resp);
    g_graph.state = GRAPH_IDLE;  // příští request spustí nové zpracování a smaže tmp soubor
    return;
  }

  // Pokud běží zpracování – informujeme klienta
  if (g_graph.state == GRAPH_PROCESSING) {
    request->send(202, "application/json", "{\"status\":\"processing\"}");
    return;
  }

  // Spustit nové zpracování
  GraphMetric metric;
  if (!parseGraphMetric(request->getParam("metric")->value(), metric)) {
    request->send(400, "application/json", "{\"error\":\"bad metric\"}");
    return;
  }
  const GraphMetricMap* map = getMetricMap(metric);
  if (!map) {
    request->send(500, "application/json", "{\"error\":\"no map\"}");
    return;
  }

  String files = request->getParam("files")->value();

  // Zjistit datum a otevřít vstupní soubor
  String fname = files;
  int comma = fname.indexOf(',');
  if (comma >= 0) fname = fname.substring(0, comma);

  int d = 0, m = 0, y = 0;
  if (sscanf(fname.c_str(), "%d.%d.%d.csv", &d, &m, &y) != 3) {
    request->send(400, "application/json", "{\"error\":\"bad filename\"}");
    return;
  }
  char dateStr[11];
  snprintf(dateStr, sizeof(dateStr), "%02d.%02d.%d", d, m, y);

  String path = "/logs/" + fname;
  File inFile = SD.open(path);
  if (!inFile) {
    request->send(404, "application/json", "{\"error\":\"file not found\"}");
    return;
  }
  inFile.readStringUntil('\n'); // header – jen pro ověření že soubor existuje

  // Zapsat hlavičku JSON
  SD.remove(GRAPH_TMP);
  g_graph.outFile = SD.open(GRAPH_TMP, FILE_WRITE);
  if (!g_graph.outFile) {
    inFile.close();
    request->send(500, "application/json", "{\"error\":\"tmp file\"}");
    return;
  }
  inFile.close(); // zavřeme lokální kopii

  // Otevřít vstupní soubor přímo do struct
  g_graph.inFile = SD.open(path);
  if (!g_graph.inFile) {
    g_graph.outFile.close();
    request->send(500, "application/json", "{\"error\":\"reopen failed\"}");
    return;
  }
  g_graph.inFile.readStringUntil('\n'); // header

  g_graph.outFile.printf("{\"date\":\"%s\",\"metric\":\"%s\",\"unit\":\"%s\",\"points\":[",
                 dateStr,
                 request->getParam("metric")->value().c_str(),
                 map->unit);

  // Inicializovat state machine
  g_graph.state      = GRAPH_PROCESSING;
  g_graph.firstPoint = true;
  g_graph.tariffInit = false;
  g_graph.lastTariff = 0;
  g_graph.tariff     = "";
  g_graph.apiDate    = String(dateStr);
  g_graph.csvIndex   = map->csvIndex;

  // Vrátit 202 – zpracování běží
  request->send(202, "application/json", "{\"status\":\"processing\"}");
});




  // Root → redirect na /live
  server.on("/", HTTP_GET, [](AsyncWebServerRequest *request){
    if (isAP()) request->redirect("/wifi");
    else        request->redirect("/live");
});


  // ====== /live – dashboard ======
  server.on("/live", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireSTA(request)) return;

    String page;
    page.reserve(6000);

    page += F(
      "<!DOCTYPE html><html lang='cs'><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>AMM ESP32 – Live</title>"
      "<style>"
      "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
      "background:#020617;color:#e5e7eb;margin:0;padding:16px;}"
      ".shell{max-width:900px;margin:0 auto;}"
      ".nav{display:flex;gap:12px;margin-bottom:16px;}"
      ".nav a{font-size:14px;text-decoration:none;color:#9ca3af;padding:6px 10px;"
      "border-radius:999px;background:#020617;border:1px solid #1f2937;}"
      ".nav a.active{color:#e5e7eb;background:#0369a1;border-color:#0369a1;}"
      ".card{background:#020617;border-radius:12px;padding:16px 20px;"
      "box-shadow:0 10px 30px rgba(0,0,0,0.5);border:1px solid #1f2937;}"
      "h1{font-size:20px;margin:0 0 8px 0;display:flex;align-items:center;justify-content:space-between;}"
      ".badge{font-size:11px;border-radius:999px;padding:2px 8px;border:1px solid transparent;}"
      ".badge-ok{background:#022c22;color:#bbf7d0;border-color:#16a34a;}"
      ".badge-err{background:#7f1d1d;color:#fecaca;border-color:#b91c1c;}"
      ".badge-wait{background:#0f172a;color:#e5e7eb;border-color:#334155;}"
      ".meta{font-size:12px;color:#9ca3af;margin-bottom:8px;}"
      "table{width:100%;border-collapse:collapse;margin-top:8px;font-size:13px;}"
      "th,td{padding:6px 8px;text-align:left;}"
      "th{border-bottom:1px solid #1f2937;color:#9ca3af;font-weight:500;}"
      "tr:nth-child(even){background:#020617;}"
      "tr:nth-child(odd){background:#020617;}"
      ".label{color:#9ca3af;}"
      ".value{font-weight:500;}"
      ".error{color:#fecaca;font-size:13px;margin-top:6px;}"
      ".footer{margin-top:10px;font-size:11px;color:#6b7280;}"
      "a.link{color:#38bdf8;text-decoration:none;font-size:12px;}"
      "a.link:hover{text-decoration:underline;}"
      ".event-card{border:1px solid #7c2d12;background:#020617;border-radius:12px;padding:14px 16px;margin-bottom:16px;box-shadow:0 10px 30px rgba(0,0,0,0.5);}"
      ".event-header{font-size:13px;font-weight:600;color:#f97316;margin-bottom:6px;}"
      ".event-text{font-size:14px;font-weight:500;color:#e5e7eb;}"
      ".event-time{font-size:12px;color:#9ca3af;margin-top:4px;}"
      ".event-btn{margin-top:10px;padding:6px 14px;font-size:13px;background:#7c2d12;color:#fde68a;border:none;border-radius:999px;cursor:pointer;}"
      ".event-btn:hover{background:#9a3412;}"
      ".live-header{display:flex;align-items:flex-start;gap:8px;}"
      ".live-title{font-size:22px;font-weight:600;display:flex;flex-wrap:wrap;gap:6px;flex:1;min-width:0;}"
      ".live-sn{font-weight:600;white-space:nowrap;}"
      ".live-status{white-space:nowrap;flex-shrink:0;}"
      ".live-tariff{font-size:15px;margin-top:6px;}"
      ".live-tariff span{font-weight:500;color:#e5e7eb;}"
      ".relay-grid{display:grid;grid-template-columns:1fr 1fr;row-gap:6px;column-gap:32px;margin-top:6px;}"
      ".relay-item{display:grid;grid-template-columns:150px auto;align-items:baseline;}"
      ".relay-name{color:#9ca3af;}"
      ".relay-on{color:#16a34a;font-weight:500;}"
      ".relay-off{color:#dc2626;font-weight:500;}"
/* ===== Mobile layout ===== */
      "@media (max-width:600px){"
      "  .live-title{font-size:20px;line-height:1.3;}"
      "  .live-tariff{font-size:16px;}"
      "  .relay-grid{grid-template-columns:1fr;}"
      "}"

      "</style></head><body><div class='shell'>"
      "<div class='nav'>"
      "<a href='/live' class='active'>Live</a>"
      "<a href='/obis'>OBIS</a>"
      "<a href='/logs'>Logy</a>"
      "<a href='/events'>Eventy</a>"
      "<a href='/graphs'>Grafy</a>"
      "<a href='/settings'>Nastavení</a>"
      "</div>"
      "<div id='liveEventBox' class='event-card' style='display:none;'>"
      "<div class='event-header'>⚠️ Upozornění</div>"
      "<div id='liveEventText' class='event-text'></div>"
      "<div id='liveEventTime' class='event-time'></div>"
      "<button class='event-btn' onclick='closeLiveEvent()'>Zavřít</button>"
      "</div>"
      "<div class='card'>"
      "<div class='live-header'>"
      "<div class='live-title'>"
      "<span>Odečet elektroměru číslo:</span>"
      "<span id='sn' class='live-sn'>–</span>"
      "</div>"
      "<span id='status' class='badge badge-wait live-status'>Načítám…</span>"
      "</div>"
      "<div class='meta live-tariff'>"
      "Aktuální tarif: <span id='tarif'>–</span>"
      "</div>"
      "<table>"
      "<tr><th colspan='2'>Okamžité výkony</th></tr>"
      "<tr><td class='label'>P+ celkem</td><td class='value' id='p_p_tot'>–</td></tr>"
      "<tr class='pplus-l'><td class='label'>P+ L1</td><td class='value' id='p_p_l1'>–</td></tr>"
      "<tr class='pplus-l'><td class='label'>P+ L2</td><td class='value' id='p_p_l2'>–</td></tr>"
      "<tr class='pplus-l'><td class='label'>P+ L3</td><td class='value' id='p_p_l3'>–</td></tr>"
      "<tr class='pminus-tot'><td class='label'>P- celkem</td><td class='value' id='p_m_tot'>–</td></tr>"
      "<tr class='pminus-l'><td class='label'>P- L1</td><td class='value' id='p_m_l1'>–</td></tr>"
      "<tr class='pminus-l'><td class='label'>P- L2</td><td class='value' id='p_m_l2'>–</td></tr>"
      "<tr class='pminus-l'><td class='label'>P- L3</td><td class='value' id='p_m_l3'>–</td></tr>"

      "<tr><th colspan='2'>Energie A+</th></tr>"
      "<tr><td class='label'>Celkem</td><td class='value' id='e_p_tot'>–</td></tr>"
      "<tr><td class='label'>T1</td><td class='value' id='e_p_t1'>–</td></tr>"
      "<tr><td class='label'>T2</td><td class='value' id='e_p_t2'>–</td></tr>"
      "<tr class='tariff-adv'><td class='label'>T3</td><td class='value' id='e_p_t3'>–</td></tr>"
      "<tr class='tariff-adv'><td class='label'>T4</td><td class='value' id='e_p_t4'>–</td></tr>"

      "<tr><th colspan='2'>Energie A-</th></tr>"
      "<tr><td class='label'>A- celkem</td><td class='value' id='e_m_tot'>–</td></tr>"

      "<tr><th colspan='2'>Stavy relé</th></tr>"
      "<tr><td colspan='2' id='relays'></td></tr>"
      "</table>"

     "<div class='meta'>"
     "Uptime: <span id='uptime'>–</span><br>"
     "Poslední data z elektroměru: <span id='lastObis'>–</span>"
     "</div>"

      "<div class='error' id='error'></div>"
      "<div class='footer'>Data z elektroměru přes OBISReg · aktualizace každé 2 s</div>"
      "</div></div>"

      "<script>"
      "async function checkLiveEvent(){"
      "  try{"
      "    const r=await fetch('/api/live_event',{cache:'no-store'});"
      "    const j=await r.json();"
      "    const box=document.getElementById('liveEventBox');"
      "    if(!j.active){"
      "      box.style.display='none';"
      "      return;"
      "    }"
      "    document.getElementById('liveEventText').innerText=j.text;"
      "    document.getElementById('liveEventTime').innerText='Čas: '+j.time;"
      "    box.style.display='block';"
      "  }catch(e){"
      "    console.log('live event error',e);"
      "  }"
      "}"
      "async function closeLiveEvent(){"
      "  await fetch('/api/close_event',{method:'POST'});"
      "  document.getElementById('liveEventBox').style.display='none';"
      "}"

      "async function fetchLive(){"
      "  const st=document.getElementById('status');"
      "  const err=document.getElementById('error');"
      "  try{"
      "    const res=await fetch('/api/live',{cache:'no-store'});"
      "    if(!res.ok) throw new Error('HTTP '+res.status);"
      "    const d=await res.json();"
      "    err.textContent='';"

      // --- nový blok: rozhodnutí podle elm_ph, show_pminus, tariff_full, desetinná čárka ---
      "    const elmPh = (d.elm_ph===1?1:3);"
      "    const showPm = !!d.show_pminus;"
      "    const tariffFull = !!d.tariff_full;"
      "    const energyDec = d.energy_decimal ? 3 : 0;"

      "    document.querySelectorAll('.pplus-l').forEach(tr=>{"
      "      tr.style.display = (elmPh===3 ? '' : 'none');"
      "    });"

      "    const rowPmTot = document.querySelector('.pminus-tot');"
      "    if(rowPmTot){ rowPmTot.style.display = (showPm ? '' : 'none'); }"

      "    document.querySelectorAll('.pminus-l').forEach(tr=>{"
      "      tr.style.display = (showPm && elmPh===3 ? '' : 'none');"
      "    });"

      "    document.querySelectorAll('.tariff-adv').forEach(tr=>{"
      "      tr.style.display = (tariffFull ? '' : 'none');"
      "    });"
      // --- konec nového bloku ---

      "    document.getElementById('sn').textContent = (d.sn || 'neznámé');"
      "    document.getElementById('tarif').textContent = (d.tarif || '–');"
      "    const f=(v,dec)=> (typeof v==='number'? v.toFixed(dec):'–');"
      "    document.getElementById('p_p_tot').textContent=f(d.p_p_tot,3)+' W';"
      "    document.getElementById('p_p_l1').textContent=f(d.p_p_l1,3)+' W';"
      "    document.getElementById('p_p_l2').textContent=f(d.p_p_l2,3)+' W';"
      "    document.getElementById('p_p_l3').textContent=f(d.p_p_l3,3)+' W';"
      "    document.getElementById('p_m_tot').textContent=f(d.p_m_tot,3)+' W';"
      "    document.getElementById('p_m_l1').textContent=f(d.p_m_l1,3)+' W';"
      "    document.getElementById('p_m_l2').textContent=f(d.p_m_l2,3)+' W';"
      "    document.getElementById('p_m_l3').textContent=f(d.p_m_l3,3)+' W';"
      "    document.getElementById('e_p_tot').textContent=f(d.e_p_tot,energyDec)+' kWh';"
      "    document.getElementById('e_p_t1').textContent=f(d.e_p_t1,energyDec)+' kWh';"
      "    document.getElementById('e_p_t2').textContent=f(d.e_p_t2,energyDec)+' kWh';"
      "    document.getElementById('e_p_t3').textContent=f(d.e_p_t3,energyDec)+' kWh';"
      "    document.getElementById('e_p_t4').textContent=f(d.e_p_t4,energyDec)+' kWh';"
      "    document.getElementById('e_m_tot').textContent=f(d.e_m_tot,energyDec)+' kWh';"

"    const relayDefs = ["
"      { name: 'Topení',          state: d.r1 },"
"      { name: 'Bojler',          state: d.r2 },"
"      { name: 'Blokace výrobny', state: d.r3 },"
"      { name: 'Elektromobil',    state: d.r4 }"
"    ];"

"    let html = \"<div class='relay-grid'>\";"
"    relayDefs.forEach(r => {"
"      html += \"<div class='relay-item'>\" +"
"              \"<span class='relay-name'>\" + r.name + \":</span>\" +"
"              \"<span class='\" + (r.state ? \"relay-on\" : \"relay-off\") + \"'>\" +"
"              (r.state ? \"Zapnuto\" : \"Vypnuto\") +"
"              \"</span></div>\";"
"    });"
"    html += \"</div>\";"
"    document.getElementById('relays').innerHTML = html;"


"    const up = d.uptime || 0;"
"    const dny = Math.floor(up / 86400);"
"    const hod = Math.floor((up % 86400) / 3600);"
"    const min = Math.floor((up % 3600) / 60);"
"    const sec = up % 60;"
""
"    let upStr = '';"
"    if (dny > 0) upStr += dny + 'd ';"
"    upStr += hod + 'h ' + min + 'm ' + sec + 's';"
""
"    document.getElementById('uptime').textContent = upStr;"


"    if (typeof d.last_obis === 'number' && d.last_obis > 0) {"
"      const ago = d.uptime - d.last_obis;"
"      document.getElementById('lastObis').textContent ="
"        ago + ' s zpět';"
"    } else {"
"      document.getElementById('lastObis').textContent = '–';"
"    }"


      "    st.textContent='OK';"
      "    st.className='badge badge-ok';"
      "  }catch(e){"
      "    err.textContent='Chyba při načítání: '+e.message;"
      "    st.textContent='Chyba';"
      "    st.className='badge badge-err';"
      "  }"
      "}"
      "fetchLive();"
      "checkLiveEvent();"
      "setInterval(fetchLive,2000);"
      "setInterval(checkLiveEvent,10000);"
      "</script></body></html>"

    );

    request->send(200, "text/html; charset=utf-8", page);
  });


  // ====== /obis – původní OBIS tabulka v novém designu ======
  server.on("/obis", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireSTA(request)) return;

    String page;
    page.reserve(6000);

    page += F(
      "<!DOCTYPE html><html><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>AMM ESP32 – OBIS</title>"
      "<style>"
      "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
      "background:#020617;color:#e5e7eb;margin:0;padding:16px;}"
      ".shell{max-width:900px;margin:0 auto;}"
      ".nav{display:flex;gap:12px;margin-bottom:16px;}"
      ".nav a{font-size:14px;text-decoration:none;color:#9ca3af;padding:6px 10px;"
      "border-radius:999px;background:#020617;border:1px solid #1f2937;}"
      ".nav a.active{color:#e5e7eb;background:#0369a1;border-color:#0369a1;}"
      ".card{background:#020617;border-radius:12px;padding:16px 20px;"
      "box-shadow:0 10px 30px rgba(0,0,0,0.5);border:1px solid #1f2937;}"
      "h1{font-size:20px;margin:0 0 8px 0;}"
      "table{width:100%;border-collapse:collapse;margin-top:8px;font-size:13px;}"
      "th,td{padding:6px 8px;border-bottom:1px solid #1f2937;text-align:left;}"
      "th{background:#020617;color:#9ca3af;font-weight:500;}"
      "td.num{text-align:right;}"
      ".meta{font-size:12px;color:#9ca3af;margin-bottom:8px;}"
      ".table-wrap{overflow-x:auto;}"
      "table{min-width:700px;}"
      "</style></head><body><div class='shell'>"
      "<div class='nav'>"
      "<a href='/live'>Live</a>"
      "<a href='/obis' class='active'>OBIS</a>"
      "<a href='/logs'>Logy</a>"
      "<a href='/events'>Eventy</a>"
      "<a href='/graphs'>Grafy</a>"
      "<a href='/settings'>Nastavení</a>"
      "</div>"
      "<div class='card'>"
      "<h1>OBIS registry</h1>"
      "<div class='meta'>Jediný zdroj pravdy pro data z elektroměru (OBISReg)</div>"
      "<div class='table-wrap'>"
      "<table>"
      "<tr>"
      "<th>#</th><th>OBIS</th><th>Hodnota</th><th>Jednotka</th><th>Platnost</th><th>Aktualizováno před</th>"
      "</tr>"
    );

    for (size_t i = 0; i < OBIS_REG_COUNT; i++) {
      ObisEntry &e = g_obisReg[i];

      page += F("<tr><td>");
      page += String(i);
      page += F("</td><td>");
      page += htmlEscape(e.obisCode ? e.obisCode : "");
      page += F("</td><td>");

      if (!e.valid) {
        page += F("<i>N/A</i>");
      } else {
        switch (e.type) {
          case OBIS_TYPE_NUMBER:
            page += String(e.valueNumber, 3);
            break;
          case OBIS_TYPE_STRING:
            page += htmlEscape(e.valueString);
            break;
          case OBIS_TYPE_BOOL:
            page += (e.valueBool ? F("true") : F("false"));
            break;
        }
      }

      page += F("</td><td>");
      page += htmlEscape(e.unit ? e.unit : "");
      page += F("</td><td>");
      page += (e.valid ? F("OK") : F("NE"));
      page += F("</td><td class='num'>");

      uint32_t nowSec = millis() / 1000;
       if (!e.valid || e.lastUpdate == 0) {
      page += F("–");
       } else {
      uint32_t diff = (nowSec >= e.lastUpdate)
                    ? (nowSec - e.lastUpdate)
                    : 0;

     if (diff < 60) {
      page += String(diff) + F(" s");
       } else if (diff < 3600) {
      page += String(diff / 60) + F(" min");
       } else if (diff < 86400) {
      page += String(diff / 3600) + F(" h ") +
            String((diff % 3600) / 60) + F(" min");
       } else {
      page += String(diff / 86400) + F(" d ") +
            String((diff % 86400) / 3600) + F(" h");
  }
}

page += F("</td></tr>");

    }

    page += F("</table></div></div></div></body></html>");

    request->send(200, "text/html; charset=utf-8", page);
  });


server.on("/ota", HTTP_POST,
  // === po dokončení uploadu ===
  [](AsyncWebServerRequest *request){
    if (Update.hasError()) {
      request->send(500, "text/plain", "OTA failed");
      return;
    }

    request->send(200, "text/html",
      "<!DOCTYPE html><html><head>"
      "<meta charset='utf-8'>"
      "<title>OTA</title>"
      "<script>"
      "setTimeout(function(){ window.location.href='/live'; }, 2500);"
      "</script>"
      "</head><body style='background:#020617;color:#e5e7eb;"
      "font-family:sans-serif;text-align:center;padding-top:40px;'>"
      "<h2>Firmware nahrán</h2>"
      "<p>Zařízení se restartuje…</p>"
      "</body></html>"
    );

    // restart ODLOŽÍME do loop()
    g_doRestart = true;
    g_restartAt = millis() + 1500;
  },

  // === příjem dat (.bin) ===
  [](AsyncWebServerRequest *request, String filename,
     size_t index, uint8_t *data, size_t len, bool final){

    if (index == 0) {
      Serial.printf("OTA: start %s\n", filename.c_str());
      if (!Update.begin(UPDATE_SIZE_UNKNOWN)) {
        Update.printError(Serial);
      }
    }

    if (len) {
      if (Update.write(data, len) != len) {
        Update.printError(Serial);
      }
    }

    if (final) {
      if (Update.end(true)) {
        Serial.println("OTA: DONE");
      } else {
        Update.printError(Serial);
      }
    }
  }
);



  // ====== /settings – Nastavení ======
  server.on("/settings", HTTP_GET, [](AsyncWebServerRequest *request){
    if (!requireSTA(request)) return;

    // Načteme aktuální nastavení z NVS
    prefs.begin("amm", true);
    uint8_t elmPh      = prefs.getUChar("elm_ph", 3);        // 1 nebo 3, default 3F
    bool    showPminus = prefs.getBool("show_pminus", true); // zobrazit P-?
    bool    tariffFull = prefs.getBool("tariff_full", true); // true = T1-4, false = jen T1-2
    bool    energyDecimal= prefs.getBool("energy_decimal", true); // desetinná místa energie
    prefs.end();

    String page;
    page.reserve(3000);
    page += F(
      "<!DOCTYPE html><html><head>"
      "<meta charset='utf-8'>"
      "<meta name='viewport' content='width=device-width, initial-scale=1'>"
      "<title>Nastavení AMM</title>"
      "<style>"
      "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
      "background:#020617;color:#e5e7eb;margin:0;padding:16px;}"
      ".shell{max-width:900px;margin:0 auto;}"
      ".nav{display:flex;gap:12px;margin-bottom:16px;flex-wrap:wrap;}"
      ".nav a{font-size:14px;text-decoration:none;color:#9ca3af;padding:6px 10px;"
      "border-radius:999px;background:#020617;border:1px solid #1f2937;}"
      ".nav a.active{color:#e5e7eb;background:#0369a1;border-color:#0369a1;}"

      ".card{background:#020617;border-radius:12px;padding:20px;"
      "box-shadow:0 10px 30px rgba(0,0,0,0.5);border:1px solid #1f2937;}"

      "h1{font-size:20px;margin:0 0 12px 0;}"
      "h2{font-size:16px;margin:0 0 8px 0;}"

      ".section{margin-top:20px;padding-top:12px;border-top:1px solid #1f2937;}"

      "label{display:block;font-size:13px;color:#9ca3af;margin:8px 0 4px 0;}"
      "input,select{width:100%;padding:8px;font-size:14px;"
      "background:#020617;color:#e5e7eb;border:1px solid #1f2937;"
      "border-radius:8px;}"

      "input[type=checkbox],input[type=radio]{width:auto;margin-right:6px;}"

      "button{margin-top:12px;padding:10px 14px;font-size:14px;"
      "background:#0369a1;color:#e5e7eb;border:none;"
      "border-radius:10px;cursor:pointer;}"

      "button:hover{background:#0284c7;}"

      ".btn-warn{background:#7c2d12;}"
      ".btn-warn:hover{background:#9a3412;}"

      ".btn-danger{background:#7f1d1d;}"
      ".btn-danger:hover{background:#991b1b;}"

      ".help{font-size:12px;color:#6b7280;margin-top:4px;}"

      "a.link{color:#38bdf8;text-decoration:none;font-size:13px;}"
      "a.link:hover{text-decoration:underline;}"
      "</style></head><body>"
      "<div class='shell'>"
      "<div class='nav'>"
      "<a href='/live'>Live</a>"
      "<a href='/obis'>OBIS</a>"
      "<a href='/logs'>Logy</a>"
      "<a href='/events'>Eventy</a>"
      "<a href='/graphs'>Grafy</a>"
      "<a href='/settings' class='active'>Nastavení</a>"
      "</div>"
      "<div class='card'>"
      "<h1>Nastavení</h1>"
);




    page += F("<form method=\"POST\" action=\"/settings\">");

    // ----- SD karta -----
    page += F("<div class='section'><h2>SD karta</h2>");
    page += F("<label>Velikost SD karty (MB): ");
    page += F("<input type=\"number\" name=\"sd_mb\" min=\"0\" value=\"");
    page += String(g_sdCardSizeMB);
    page += F("\"></label>");

    page += F("<p>Aktuální limit pro logy: ");
    page += String((unsigned long)(g_logMaxBytes / (1024ULL * 1024ULL)));
    page += F(" MB</p>");
    page += F("<p class='help'>Pokud zadáš velikost SD, limit pro logy bude ~80 % této kapacity. 0 = použít výchozí 100 MB.</p>");
    page += F("</div>");

    // ----- Typ elektroměru -----
    page += F("<div class='section'><h2>Typ elektroměru</h2>");
    page += F("<label>");
    page += F("<select name=\"elm_ph\">");
    page += F("<option value=\"1\"");
    if (elmPh == 1) page += F(" selected");
    page += F(">1F elektroměr</option>");
    page += F("<option value=\"3\"");
    if (elmPh != 1) page += F(" selected");
    page += F(">3F elektroměr</option>");
    page += F("</select></label>");
    page += F("<p class='help'>1F: ve webu se zobrazí pouze P+ celkem (a případně P- celkem). 3F: zobrazí se i P+ L1–L3 a P- L1–L3.</p>");
    page += F("</div>");

    // ----- Dodávka P- -----
    page += F("<div class='section'><h2>Zobrazení dodávky (P-)</h2>");
    page += F("<label>");
    page += F("<input type=\"checkbox\" name=\"show_pminus\" value=\"1\"");
    if (showPminus) page += F(" checked");
    page += F("> Zobrazovat P- (dodávku)</label>");
    page += F("<p class='help'>Pokud je vypnuto, nebudou ve webu vidět žádné řádky P-.</p>");
    page += F("</div>");

    // ----- Tarify -----
    page += F("<div class='section'><h2>Zobrazení tarifů</h2>");
    page += F("<label><input type=\"radio\" name=\"tariff_mode\" value=\"basic\"");
    if (!tariffFull) page += F(" checked");
    page += F("> Základní tarify (T1 + T2)</label>");
    page += F("<label><input type=\"radio\" name=\"tariff_mode\" value=\"full\"");
    if (tariffFull) page += F(" checked");
    page += F("> Všechny tarify (T1–T4)</label>");
    page += F("<p class='help'>Základní: zobrazí se jen T1 a T2. Všechny: T1 až T4.</p>");
    page += F("</div>");

        // ----- Zobrazení energie -----
    page += F("<div class='section'><h2>Zobrazení energie</h2>");
    page += F("<label>");
    page += F("<input type=\"checkbox\" name=\"energy_decimal\" value=\"1\"");
    if (energyDecimal) page += F(" checked");
    page += F("> Zobrazovat desetinná místa u energie A+ a A-");
    page += F("</label>");
    page += F("<p class='help'>Pokud je vypnuto, energie se zobrazí jako celé kWh (bez desetinné čárky).</p>");
    page += F("</div>");


// ----- Home Assistant (MQTT) -----
    page += F("<div class='section'><h2>Home Assistant (MQTT)</h2>");

    page += F("<label>");
    page += F("<input type='checkbox' name='ha_enabled' value='1'");
    if (haConfig.enabled) page += F(" checked");
    page += F("> Povolit integraci Home Assistant</label>");

    page += F("<label>MQTT / HA IP nebo hostname:");
    page += F("<input type='text' name='ha_host' value='");
    page += String(haConfig.host);
    page += F("' placeholder='192.168.48.1'></label>");

    page += F("<label>MQTT port:");
    page += F("<input type='number' name='ha_port' value='");
    page += String(haConfig.port);
    page += F("'></label>");

    page += F("<label>MQTT Client ID:");
    page += F("<input type='text' name='ha_client_id' value='");
    page += String(haConfig.clientId);
    page += F("' placeholder='AMM_ESP32'></label>");

    page += F("<label>MQTT uživatelské jméno (volitelné):");
    page += F("<input type='text' name='ha_username' value='");
    page += String(haConfig.username);
    page += F("' placeholder='(prázdné = anonymní přístup)'></label>");

    page += F("<label>MQTT heslo (volitelné):");
    page += F("<input type='password' name='ha_password' value='");
    page += String(haConfig.password);
    page += F("' placeholder='(prázdné = bez hesla)'></label>");

    page += F("<p class='help'>ESP32 bude publikovat data přes MQTT pro Home Assistant. "
           "Změna se projeví po stisknutí tlačítka Uložit.</p>");

    page += F("</div>");

    page += F("<button type=\"submit\">Uložit</button>");
    page += F("</form>");


 //------OTA aktualizace------
   page += F(
  "<div class='section'>"
  "<h2>Firmware (OTA)</h2>"
  "<form method='POST' action='/ota' enctype='multipart/form-data'>"
  "<input type='file' name='update' accept='.bin' required>"
  "<br><br>"
  "<button type='submit' "
  "style='background:#0369a1;color:#fff;'>"
  "Nahrát firmware</button>"
  "</form>"
  "<p class='help'>"
  "Nahraj .bin soubor. "
  "Zařízení se po aktualizaci automaticky restartuje."
  "</p>"
  "</div>"
);


 //------Reset tlačítka------
  page += F(
  "<div class='section'>"
  "<h2>Reset zařízení</h2>"

  "<button type='button' class='btn-warn' "
  "onclick=\"if(confirm('Opravdu resetovat Wi-Fi nastavení?')) "
  "fetch('/reset-wifi',{method:'POST'});\">"
  "Reset Wi-Fi</button>"

  "<br><br>"

  "<button type='button' class='btn-danger' "
  "onclick=\"if(confirm('POZOR! Dojde ke smazání Wi-Fi, nastavení i SD karty. Pokračovat?')) "
  "fetch('/factory-reset',{method:'POST'});\">"
  "Tovární reset</button>"
  "</div>"
);



    page += F("<div style='margin-top:24px;padding-top:12px;border-top:1px solid #1f2937;"
              "font-size:12px;color:#6b7280;text-align:right;'>"
              "Firmware verze: " FW_VERSION "<br>"
              "Autor: Milan Kučera<br>"
               "<a href='https://github.com/kacer11/odecet-amm-han' target='_blank' "
               "style='color:#38bdf8;text-decoration:none;'>Stránka projektu</a>"
               "</div>");

    page += F("</div></div></body></html>");

    request->send(200, "text/html; charset=utf-8", page);
  });


  // ====== /settings (POST) ======
  server.on("/settings", HTTP_POST, [](AsyncWebServerRequest *request){
    // --- SD karta ---
    if (request->hasParam("sd_mb", true)) {
      const AsyncWebParameter* p = request->getParam("sd_mb", true);
      uint32_t val = strtoul(p->value().c_str(), nullptr, 10);
      g_sdCardSizeMB = val;
      saveConfig();  // přepočítá g_logMaxBytes a uloží sd_mb do NVS
    }

    // Načteme stávající hodnoty, kdyby něco nepřišlo
    prefs.begin("amm", false);
    uint8_t elmPh        = prefs.getUChar("elm_ph", 3);
    bool    showPminus   = prefs.getBool("show_pminus", true);
    bool    tariffFull   = prefs.getBool("tariff_full", true);
    bool    energyDecimal= prefs.getBool("energy_decimal", true);


    // --- Typ elektroměru (1F / 3F) ---
    if (request->hasParam("elm_ph", true)) {
      const AsyncWebParameter* p = request->getParam("elm_ph", true);
      String v = p->value();
      if (v == "1") elmPh = 1;
      else          elmPh = 3;
    }

    // --- Zobrazení P- ---
    // Checkbox: pokud není v POST, znamená to false
    showPminus = request->hasParam("show_pminus", true);

    // --- Tarify ---
    if (request->hasParam("tariff_mode", true)) {
      const AsyncWebParameter* p = request->getParam("tariff_mode", true);
      String v = p->value();
      if (v == "basic") tariffFull = false;
      else              tariffFull = true;
    }

        // --- Zobrazení desetinných míst energie ---
    energyDecimal = request->hasParam("energy_decimal", true);

    // ===== Home Assistant (MQTT) =====
haConfig.enabled = request->hasParam("ha_enabled", true);

if (request->hasParam("ha_host", true)) {
  strncpy(haConfig.host,
           request->getParam("ha_host", true)->value().c_str(),
           sizeof(haConfig.host) - 1);
  haConfig.host[sizeof(haConfig.host) - 1] = 0;
}

if (request->hasParam("ha_port", true)) {
  haConfig.port = request->getParam("ha_port", true)->value().toInt();
}

if (request->hasParam("ha_client_id", true)) {
  String val = request->getParam("ha_client_id", true)->value();
  if (val.length() == 0) val = "AMM_ESP32";
  strncpy(haConfig.clientId, val.c_str(), sizeof(haConfig.clientId) - 1);
  haConfig.clientId[sizeof(haConfig.clientId) - 1] = 0;
}

if (request->hasParam("ha_username", true)) {
  strncpy(haConfig.username,
           request->getParam("ha_username", true)->value().c_str(),
           sizeof(haConfig.username) - 1);
  haConfig.username[sizeof(haConfig.username) - 1] = 0;
}

if (request->hasParam("ha_password", true)) {
  strncpy(haConfig.password,
           request->getParam("ha_password", true)->value().c_str(),
           sizeof(haConfig.password) - 1);
  haConfig.password[sizeof(haConfig.password) - 1] = 0;
}

saveHAConfig();

// okamžitě aplikujeme změnu bez restartu
mqtt.disconnect();
if (haConfig.enabled && strlen(haConfig.host) > 0) {
  mqtt.setServer(haConfig.host, haConfig.port);
}



    // Uložíme zpět do NVS
    prefs.putUChar("elm_ph", elmPh);
    prefs.putBool("show_pminus", showPminus);
    prefs.putBool("tariff_full", tariffFull);
    prefs.putBool("energy_decimal", energyDecimal);
    prefs.end();


    request->redirect("/settings");
  });


  // ====== /logs – výpis logů ======
server.on("/logs", HTTP_GET, [](AsyncWebServerRequest *request){
  if (!requireSTA(request)) return;

  String page;
  page.reserve(7000);

  page += F(
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Logy AMM</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
    "background:#020617;color:#e5e7eb;margin:0;padding:16px;}"
    ".shell{max-width:900px;margin:0 auto;}"
    ".nav{display:flex;gap:12px;margin-bottom:16px;}"
    ".nav a{font-size:14px;text-decoration:none;color:#9ca3af;padding:6px 10px;"
    "border-radius:999px;background:#020617;border:1px solid #1f2937;}"
    ".nav a.active{color:#e5e7eb;background:#0369a1;border-color:#0369a1;}"
    ".card{background:#020617;border-radius:12px;padding:16px 20px;"
    "box-shadow:0 10px 30px rgba(0,0,0,0.5);border:1px solid #1f2937;}"
    "table{width:100%;border-collapse:collapse;margin-top:8px;font-size:13px;}"
    "th,td{padding:6px 8px;border-bottom:1px solid #1f2937;text-align:left;}"
    "th{color:#9ca3af;font-weight:500;}"
    "a.link{color:#38bdf8;text-decoration:none;}"
    "a.link:hover{text-decoration:underline;}"
    ".filter{margin-bottom:12px;font-size:13px;display:flex;flex-wrap:wrap;gap:8px;align-items:center;}"
    ".filter select{"
    "  padding:8px 10px;"
    "  font-size:13px;"
    "  background:#020617;"
    "  color:#e5e7eb;"
    "  border:1px solid #1f2937;"
    "  border-radius:8px;"
    "}"
    ".filter button{"
    "  padding:8px 14px;"
    "  font-size:13px;"
    "  background:#0369a1;"
    "  color:#e5e7eb;"
    "  border:none;"
    "  border-radius:10px;"
    "  cursor:pointer;"
    "}"
    ".filter button:hover{background:#0284c7;}"

    "</style></head><body><div class='shell'>"
    "<div class='nav'>"
    "<a href='/live'>Live</a>"
    "<a href='/obis'>OBIS</a>"
    "<a href='/logs' class='active'>Logy</a>"
    "<a href='/events'>Eventy</a>"
    "<a href='/graphs'>Grafy</a>"
    "<a href='/settings'>Nastavení</a>"
    "</div>"
    "<div class='card'>"
    "<h1>Logy</h1>"
    "<div style='font-size:12px;color:#9ca3af;margin-top:4px;margin-bottom:12px;'>"
    "Z důvodu optimalizace je výpis logů nastaven na 64 nejnovějších logů. "
    "Pro zobrazení starších použij filtr.</div>"
);


  if (!sdOk) {
    page += F("<p><b>SD karta není inicializovaná.</b></p></div></div></body></html>");
    request->send(200, "text/html; charset=utf-8", page);
    return;
  }

  // ---- Načtení filtrů z URL ----
  int filterYear  = request->hasParam("year")  ? request->getParam("year")->value().toInt()  : 0;
  int filterMonth = request->hasParam("month") ? request->getParam("month")->value().toInt() : 0;
  String order    = request->hasParam("order") ? request->getParam("order")->value()         : "desc";

  // ---- Načtení logů ze SD ----
LogFileInfo files[64];
int fileCount = 0;
// --- dynamický rozsah let (posuvné okno) ---
static const int MAX_YEARS = 20;
bool years[MAX_YEARS] = {};
int yearBase = 0;   // nejstarší rok na SD

bool months[13] = {};   // 1–12

// ===== FÁZE 1: META SCAN (bez limitu) =====
File dir = SD.open("/logs");
File f = dir.openNextFile();
while (f) {
  if (!f.isDirectory()) {
    const char* base = strrchr(f.name(), '/');
    base = base ? base + 1 : f.name();

    LogFileInfo info;
    if (parseLogFilename(base, info)) {
       if (yearBase == 0 || info.year < yearBase)
         yearBase = info.year;
      months[info.month] = true;
    }
  }
  f = dir.openNextFile();
}
dir.close();

// --- druhý průchod: označení dostupných let ---
dir = SD.open("/logs");
f = dir.openNextFile();
while (f) {
  if (!f.isDirectory()) {
    const char* base = strrchr(f.name(), '/');
    base = base ? base + 1 : f.name();

    LogFileInfo info;
    if (parseLogFilename(base, info)) {
      int idx = info.year - yearBase;
      if (idx >= 0 && idx < MAX_YEARS) {
        years[idx] = true;
      }
    }
  }
  f = dir.openNextFile();
}
dir.close();


// ===== FÁZE 2: DATA SCAN (s filtrem + limitem 64) =====
dir = SD.open("/logs");
f = dir.openNextFile();
while (f && fileCount < 64) {
  if (!f.isDirectory()) {
    const char* base = strrchr(f.name(), '/');
    base = base ? base + 1 : f.name();

    LogFileInfo info;
    if (parseLogFilename(base, info)) {

      // --- aplikace filtru ---
      if (filterYear  && info.year  != filterYear)  { f = dir.openNextFile(); continue; }
      if (filterMonth && info.month != filterMonth) { f = dir.openNextFile(); continue; }

      info.size = f.size();
      files[fileCount++] = info;
    }
  }
  f = dir.openNextFile();
}
dir.close();


  // ---- Řazení ----
  auto cmp = [&](const LogFileInfo& a, const LogFileInfo& b) {
    if (a.year != b.year)   return order == "asc" ? a.year < b.year : a.year > b.year;
    if (a.month != b.month) return order == "asc" ? a.month < b.month : a.month > b.month;
    return order == "asc" ? a.day < b.day : a.day > b.day;
  };
  std::sort(files, files + fileCount, cmp);

  // ---- FILTR FORM ----
  page += F("<form class='filter' method='GET' action='/logs'>Rok: <select name='year'>");
  page += F("<option value='0'>Vše</option>");
for (int i = 0; i < MAX_YEARS; i++) {
  if (years[i]) {
    int y = yearBase + i;
    page += "<option value='" + String(y) + "'";
    if (filterYear == y) page += " selected";
    page += ">" + String(y) + "</option>";
  }
}

  page += F("</select> Měsíc: <select name='month'><option value='0'>Vše</option>");
  for (int m = 1; m <= 12; m++) {
    if (months[m]) {
      page += "<option value='" + String(m) + "'";
      if (filterMonth == m) page += " selected";
      page += ">" + String(m) + "</option>";
    }
  }
  page += F("</select> Řazení: <select name='order'>");
  page += "<option value='desc'" + String(order=="desc"?" selected":"") + ">Nejnovější</option>";
  page += "<option value='asc'"  + String(order=="asc" ?" selected":"") + ">Nejstarší</option>";
  page += F("</select> <button type='submit'>Filtrovat</button></form>");

  // ---- TABULKA ----
  page += F("<table><tr><th>Soubor</th><th>Velikost [B]</th></tr>");

  bool any = false;
  for (int i = 0; i < fileCount; i++) {
    if (filterYear  && files[i].year  != filterYear)  continue;
    if (filterMonth && files[i].month != filterMonth) continue;

    page += F("<tr><td><a class='link' href='/download?file=");
    page += files[i].name;
    page += F("'>");
    page += files[i].name;
    page += F("</a></td><td>");
    page += String(files[i].size);
    page += F("</td></tr>");
    any = true;
  }

  if (!any) {
    page += F("<tr><td colspan='2'><i>Žádné logy pro zvolený filtr.</i></td></tr>");
  }

  page += F("</table></div></div></body></html>");

  request->send(200, "text/html; charset=utf-8", page);
});

// ====== /graphs – Grafy ======
server.on("/graphs", HTTP_GET, [](AsyncWebServerRequest *request){
  if (!requireSTA(request)) return;

  String page;
  page.reserve(9000);

  page += F(
    "<!DOCTYPE html><html lang='cs'><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>AMM – Grafy</title>"
    "<script src='https://cdn.jsdelivr.net/npm/chart.js'></script>"
    "<script src='https://cdn.jsdelivr.net/npm/date-fns'></script>"
    "<script src='https://cdn.jsdelivr.net/npm/chartjs-adapter-date-fns'></script>"
    "<script src='https://cdn.jsdelivr.net/npm/date-fns/locale/cs'></script>"

    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
    "background:#020617;color:#e5e7eb;margin:0;padding:16px;}"
    ".shell{max-width:1000px;margin:0 auto;}"

    ".nav{display:flex;gap:12px;margin-bottom:16px;}"
    ".nav a{font-size:14px;text-decoration:none;color:#9ca3af;padding:6px 10px;"
    "border-radius:999px;background:#020617;border:1px solid #1f2937;}"
    ".nav a.active{color:#e5e7eb;background:#0369a1;border-color:#0369a1;}"

    ".card{background:#020617;border-radius:12px;padding:16px 20px;"
    "box-shadow:0 10px 30px rgba(0,0,0,0.5);border:1px solid #1f2937;}"

    /* === SEKCE JAKO /live === */
    ".section{margin-top:20px;padding-top:12px;border-top:1px solid #1f2937;}"
    ".section:first-of-type{margin-top:0;padding-top:0;border-top:none;}"
    ".section-title{font-size:14px;color:#9ca3af;margin-bottom:8px;}"

    "select,button{padding:8px;border-radius:8px;border:1px solid #1f2937;"
    "background:#020617;color:#e5e7eb;font-size:14px;}"
    ".controls{display:flex;flex-wrap:wrap;gap:8px;margin-bottom:12px;}"
    "canvas{max-height:420px;}"

    "button.primary{background:#0369a1;border-color:#0369a1;color:#ffffff;}"
    "button.primary:hover{background:#0284c7;border-color:#0284c7;}"
    "</style></head><body><div class='shell'>"

    "<div class='nav'>"
    "<a href='/live'>Live</a>"
    "<a href='/obis'>OBIS</a>"
    "<a href='/logs'>Logy</a>"
    "<a href='/events'>Eventy</a>"
    "<a href='/graphs' class='active'>Grafy</a>"
    "<a href='/settings'>Nastavení</a>"
    "</div>"

    "<div class='card'>"
    "<h2>Grafy z logů</h2>"
    "<div style='font-size:12px;color:#9ca3af;margin-top:4px;margin-bottom:12px;'>"
    "Výkon ESP32 je limitován pro vícedenní grafy, pro ně použij <a href='https://dip.cezdistribuce.cz/' target='_blank' style='color:#38bdf8;'>web distributora</a>.<br>"
    "Nebo stáhni požadované dny z logu/SD karty a vytvoř externě (excel, umělá inteligence)</div>"

    /* ===== FILTR ===== */
    "<div class='section'>"
    "<div class='section-title'>Filtr</div>"
    "<div class='controls'>"
    "<select id='metric'>"
    "<option value='p_plus'>P+ (okamžitý odběr)</option>"
    "<option value='p_minus'>P- (okamžitá dodávka)</option>"
    "<option value='a_plus'>A+ (spotřeba)</option>"
    "<option value='a_minus'>A- (dodávka)</option>"
    "</select>"
    "<select id='year'></select>"
    "<select id='month'></select>"
    "<select id='day'></select>"
    "<button class='primary' onclick='loadGraph()'>Zobrazit</button>"
    "</div>"
    "</div>"

    /* ===== ZOBRAZOVÁNÍ ===== */
    "<div class='section'>"
    "<div class='section-title'>Zobrazování grafu</div>"
    "<div style='font-size:14px;display:flex;align-items:center;gap:16px;flex-wrap:wrap;'>"
    "<label><input type='checkbox' id='chk_pplus' checked onchange='toggleDataset(0)'> Energie</label>"
    "<label><input type='checkbox' id='chk_tarif' checked onchange='toggleDataset(1)'> Tarif</label>"
    "</div>"
    "</div>"

    /* ===== GRAF ===== */
    "<div class='section'>"
    "<div class='section-title'>Graf</div>"
    "<div style='position:relative;height:300px;'>"
    "<canvas id='chart'></canvas>"
    "</div>"
    "<div style='margin-top:12px;font-size:13px;color:#9ca3af;'>"
    "<div style='display:flex;align-items:center;gap:8px;margin-bottom:6px;'>"
    "<span style='white-space:nowrap;'>Od:</span>"
    "<input type='range' id='zoomStart' min='0' max='100' value='0' style='flex:1;accent-color:#0369a1;'>"
    "</div>"
    "<div style='display:flex;align-items:center;gap:8px;margin-bottom:8px;'>"
    "<span style='white-space:nowrap;'>Do:</span>"
    "<input type='range' id='zoomEnd' min='0' max='100' value='100' style='flex:1;accent-color:#0369a1;'>"
    "</div>"
    "<div style='display:flex;justify-content:space-between;align-items:center;'>"
    "<span id='zoomInfo' style='font-size:12px;color:#6b7280;'></span>"
    "<button class='primary' onclick='resetZoom()' style='padding:4px 14px;font-size:13px;'>Reset zoom</button>"
    "</div>"
    "</div>"
    "</div>"

    "</div>"

    "<script>"
    "let chart=null;"
    "let allLabels=[];"
    "let allDatasets=[];"

    "function resetZoom(){"
    " document.getElementById('zoomStart').value=0;"
    " document.getElementById('zoomEnd').value=100;"
    " applyZoom();"
    "}"

    "function applyZoom(){"
    " if(!chart||!allLabels.length) return;"
    " const total=allLabels.length;"
    " const s=parseInt(document.getElementById('zoomStart').value);"
    " const e=parseInt(document.getElementById('zoomEnd').value);"
    " const from=Math.floor(s/100*total);"
    " const to=Math.max(from+10,Math.floor(e/100*total));"
    " const fromT=allLabels[from];"
    " const toT=allLabels[Math.min(to-1,total-1)];"
    // Slice labels a energy data
    " chart.data.labels=allLabels.slice(from,to);"
    " chart.data.datasets[0].data=allDatasets[0].slice(from,to);"
    // Tarif – filtrujeme {x,y} objekty v rozsahu
    " chart.data.datasets[1].data=allDatasets[1].filter(p=>p[0]>=fromT&&p[0]<=toT).map(p=>({x:p[0],y:p[1]}));"
    " chart.update('none');"
    " document.getElementById('zoomInfo').textContent=fromT+' – '+toT;"
    "}"

    "document.addEventListener('DOMContentLoaded',()=>{"
    " document.getElementById('zoomStart').addEventListener('input',applyZoom);"
    " document.getElementById('zoomEnd').addEventListener('input',applyZoom);"
    "});"

    "function toggleDataset(idx){"
    " if(!chart) return;"
    " chart.setDatasetVisibility(idx,!chart.isDatasetVisible(idx));"
    " chart.update();"
    "}"

    "async function loadMeta(){"
    " const r=await fetch('/api/logs/meta');"
    " const d=await r.json();"
    " const y=document.getElementById('year');"
    " y.innerHTML='';"
    " d.years.forEach(v=>{y.innerHTML+=`<option value='${v}'>${v}</option>`;});"
    " y.onchange=()=>loadMonths(d);"
    " loadMonths(d);"
    "}"

    "function loadMonths(d){"
    " const y=document.getElementById('year').value;"
    " const m=document.getElementById('month');"
    " m.innerHTML='';"
    " (d.months[y]||[]).forEach(v=>{m.innerHTML+=`<option value='${v}'>${v}</option>`;});"
    " m.onchange=()=>loadDays(d);"
    " loadDays(d);"
    "}"

    "function loadDays(d){"
    " const y=document.getElementById('year').value;"
    " const m=document.getElementById('month').value;"
    " const key=y+'-'+m;"
    " const sel=document.getElementById('day');"
    " sel.innerHTML='';"
    " (d.days[key]||[]).forEach(v=>{sel.innerHTML+=`<option value='${v}'>${v}</option>`;});"
    "}"

    "async function loadGraph(){"
    " const d=document.getElementById('day').value;"
    " const m=document.getElementById('month').value;"
    " const y=document.getElementById('year').value;"
    " const metric=document.getElementById('metric').value;"

    " const energyLabelMap={"
    "  p_plus:'Okamžitý činný výkon odběru',"
    "  p_minus:'Okamžitý činný výkon dodávky',"
    "  a_plus:'Celková spotřeba',"
    "  a_minus:'Celková dodávka'"
    " };"
    " const energyLabel=energyLabelMap[metric]||'Energie';"

    " const fname=(d.padStart(2,'0')+'.'+m.padStart(2,'0')+'.'+y+'.csv');"
    " const url=`/api/graph?metric=${metric}&files=${fname}`;"

    // Pollování dokud není výsledek připraven
    " let data=null;"
    " for(let i=0;i<60;i++){"
    "  const r=await fetch(url,{cache:'no-store'});"
    "  if(r.status===200){data=await r.json();break;}"
    "  if(r.status!==202){break;}"
    "  await new Promise(res=>setTimeout(res,500));"
    " }"
    " if(!data){console.error('Graph timeout');return;}"

    " const baseDate=data.date;"
    " const labels=data.points.map(p=>p[0]);"
    " const ptsData=data.points.map(p=>p[1]);"
    " const tarData=data.tariff;"

    // Uložit celá data pro zoom slider
    " allLabels=labels;"
    " allDatasets=[ptsData, tarData];"

    " if(chart) chart.destroy();"
    " Chart.defaults.locale='cs';"

    " chart=new Chart(document.getElementById('chart'),{"
    "  type:'line',"
    "  data:{"
    "   labels:labels,"
    "   datasets:["
    "    {label:energyLabel,data:ptsData,borderColor:'#38bdf8',yAxisID:'y',pointRadius:0,tension:0},"
    "    {label:'Tarif',data:tarData.map(p=>({x:p[0],y:p[1]})),yAxisID:'y2',showLine:false,pointRadius:4,"
    "     pointHoverRadius:6,"
    "     pointBackgroundColor:ctx=>ctx.raw&&ctx.raw.y==1?'#ef4444':'#22c55e',"
    "     pointBorderColor:ctx=>ctx.raw&&ctx.raw.y==1?'#ef4444':'#22c55e'}"
    "   ]},"
    "  options:{"
    "   animation:false,"
    "   responsive:true,"
    "   maintainAspectRatio:false,"
    "   scales:{"
    "    x:{type:'category',ticks:{autoSkip:true,maxTicksLimit:12,"
    "     callback:function(value,index,ticks){"
    "      const t=this.getLabelForValue(value);"
    "      if(!t) return '';"
    "      const p=t.split(':');"
    "      if(p.length<2) return '';"
    "      const total=this.chart.data.labels.length;"
    // Interval závisí na počtu viditelných bodů
    "      const step=total<=60?1:total<=120?2:total<=360?5:total<=720?10:60;"
    "      const minVal=parseInt(p[0])*60+parseInt(p[1]);"
    "      return(minVal%step===0)?p[0]+':'+p[1].padStart(2,'0'):'';"
    "     }"
    "    }},"
    "    y:{title:{display:true,text:data.unit}},"
    "    y2:{display:false,min:0.5,max:2.5}"
    "   },"
    "   plugins:{"
    "    legend:{onClick:null,labels:{usePointStyle:true,pointStyle:'line'}},"
    "    tooltip:{callbacks:{title:(items)=>baseDate+' '+items[0].label}}"
    "   }"
    "  }"
    " });"

    " document.getElementById('chk_pplus').checked=true;"
    " document.getElementById('chk_tarif').checked=true;"
    " document.getElementById('zoomStart').value=0;"
    " document.getElementById('zoomEnd').value=100;"
    " document.getElementById('zoomInfo').textContent='';"
    "}"

    "loadMeta();"
    "</script></body></html>"
  );

  request->send(200,"text/html; charset=utf-8",page);
});


  // ====== Stažení jednoho log souboru – /download (bez změny designu) ======
server.on("/download", HTTP_GET, [](AsyncWebServerRequest *request){
  if (!requireSTA(request)) return;
  if (!sdOk) {
    request->send(500, "text/plain", "SD karta neni inicializovana");
    return;
  }

  if (!request->hasParam("file")) {
    request->send(400, "text/plain", "Chybi parametr 'file'");
    return;
  }

  String type = request->hasParam("type") ? request->getParam("type")->value() : "logs";
  String fname = request->getParam("file")->value();

  // povolíme jen logs / events
  if (type != "logs" && type != "events") {
    request->send(400, "text/plain", "Neplatny typ");
    return;
  }

  // validace názvu souboru (BEZ cest)
  if (fname.length() == 0 || fname.indexOf('/') >= 0 || fname.indexOf('\\') >= 0) {
    request->send(400, "text/plain", "Neplatny nazev souboru");
    return;
  }

  for (size_t i = 0; i < fname.length(); i++) {
    char c = fname[i];
    bool ok =
      (c >= '0' && c <= '9') ||
      (c >= 'A' && c <= 'Z') ||
      (c >= 'a' && c <= 'z') ||
      (c == '.' || c == '-' || c == '_');
    if (!ok) {
      request->send(400, "text/plain", "Neplatny nazev souboru");
      return;
    }
  }

  String path = "/" + type + "/" + fname;

  if (!SD.exists(path)) {
    request->send(404, "text/plain", "Soubor nenalezen");
    return;
  }

  const char* mime =
    (type == "events") ? "text/plain" : "text/csv";

  AsyncWebServerResponse *response =
    request->beginResponse(SD, path, mime);

  response->addHeader(
    "Content-Disposition",
    String("attachment; filename=\"") + fname + "\""
  );

  request->send(response);
});


  // ====== Captive portal chování v AP módu ======
server.onNotFound([](AsyncWebServerRequest *request){
  if (isAP()) {
    request->redirect("/wifi");
  } else {
    request->send(404, "text/plain", "Not found");
  }
});

//============API pro live eventy==============
server.on("/api/live_event", HTTP_GET, [](AsyncWebServerRequest *req) {
  if (!g_liveEventActive) {
    req->send(200, "application/json", "{\"active\":false}");
    return;
  }

  String json = "{";
  json += "\"active\":true,";
  json += "\"type\":\"" + String(g_liveEventType) + "\",";
  json += "\"text\":\"" + String(g_liveEventText) + "\",";
  json += "\"time\":\"" + String(g_liveEventTime) + "\"";
  json += "}";

  req->send(200, "application/json", json);
});

//==================API pro close eventy=========
server.on("/api/close_event", HTTP_POST, [](AsyncWebServerRequest *req) {
  closeLiveEventInternal("web");
  req->send(200, "application/json", "{\"ok\":true}");
});


//===========Eventy===============
server.on("/events", HTTP_GET, [](AsyncWebServerRequest *request){
  if (!requireSTA(request)) return;

  String page;
  page.reserve(7000);

  page += F(
    "<!DOCTYPE html><html><head>"
    "<meta charset='utf-8'>"
    "<meta name='viewport' content='width=device-width, initial-scale=1'>"
    "<title>Eventy AMM</title>"
    "<style>"
    "body{font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif;"
    "background:#020617;color:#e5e7eb;margin:0;padding:16px;}"
    ".shell{max-width:900px;margin:0 auto;}"
    ".nav{display:flex;gap:12px;margin-bottom:16px;}"
    ".nav a{font-size:14px;text-decoration:none;color:#9ca3af;padding:6px 10px;"
    "border-radius:999px;background:#020617;border:1px solid #1f2937;}"
    ".nav a.active{color:#e5e7eb;background:#0369a1;border-color:#0369a1;}"
    ".card{background:#020617;border-radius:12px;padding:16px 20px;"
    "box-shadow:0 10px 30px rgba(0,0,0,0.5);border:1px solid #1f2937;}"
    "table{width:100%;border-collapse:collapse;margin-top:8px;font-size:13px;}"
    "th,td{padding:6px 8px;border-bottom:1px solid #1f2937;text-align:left;}"
    "th{color:#9ca3af;font-weight:500;}"
    "a.link{color:#38bdf8;text-decoration:none;}"
    "a.link:hover{text-decoration:underline;}"
    ".filter{margin-bottom:12px;font-size:13px;display:flex;flex-wrap:wrap;gap:8px;align-items:center;}"
    ".filter select{padding:8px 10px;font-size:13px;background:#020617;color:#e5e7eb;border:1px solid #1f2937;border-radius:8px;}"
    ".filter button{padding:8px 14px;font-size:13px;background:#0369a1;color:#e5e7eb;border:none;border-radius:10px;cursor:pointer;}"
    ".filter button:hover{background:#0284c7;}"
    "</style></head><body><div class='shell'>"
    "<div class='nav'>"
    "<a href='/live'>Live</a>"
    "<a href='/obis'>OBIS</a>"
    "<a href='/logs'>Logy</a>"
    "<a href='/events' class='active'>Eventy</a>"
    "<a href='/graphs'>Grafy</a>"
    "<a href='/settings'>Nastavení</a>"
    "</div>"
    "<div class='card'>"
    "<h1>Eventy</h1>"
    "<div style='font-size:12px;color:#9ca3af;margin-top:4px;margin-bottom:12px;'>"
    "<div>Zde se zobrazují záznamy o výměně elektroměru a vybité RTC (časové) baterii.</div>"
    "<div>Zobrazuje se maximálně 64 nejnovějších souborů.</div>"
    "</div>"
  );

  if (!sdOk) {
    page += F("<p><b>SD karta není inicializovaná.</b></p></div></div></body></html>");
    request->send(200, "text/html; charset=utf-8", page);
    return;
  }

  int filterYear  = request->hasParam("year")  ? request->getParam("year")->value().toInt()  : 0;
  int filterMonth = request->hasParam("month") ? request->getParam("month")->value().toInt() : 0;
  String order    = request->hasParam("order") ? request->getParam("order")->value()         : "desc";

  EventFileInfo files[64];
  int fileCount = 0;

  static const int MAX_YEARS = 20;
  bool years[MAX_YEARS] = {};
  bool months[13] = {};
  int yearBase = 0;

  File dir = SD.open("/events");
  File f = dir.openNextFile();
  while (f) {
    if (!f.isDirectory()) {
      const char* base = strrchr(f.name(), '/');
      base = base ? base + 1 : f.name();
      EventFileInfo info;
      if (parseEventFilename(base, info)) {
        if (yearBase == 0 || info.year < yearBase) yearBase = info.year;
        months[info.month] = true;
      }
    }
    f = dir.openNextFile();
  }
  dir.close();

  // ===== druhý průchod: dostupné roky =====
dir = SD.open("/events");
f = dir.openNextFile();
while (f) {
  if (!f.isDirectory()) {
    const char* base = strrchr(f.name(), '/');
    base = base ? base + 1 : f.name();

    EventFileInfo info;
    if (parseEventFilename(base, info)) {
      int idx = info.year - yearBase;
      if (idx >= 0 && idx < MAX_YEARS) {
        years[idx] = true;
      }
    }
  }
  f = dir.openNextFile();
}
dir.close();


  dir = SD.open("/events");
  f = dir.openNextFile();
  while (f && fileCount < 64) {
    if (!f.isDirectory()) {
      const char* base = strrchr(f.name(), '/');
      base = base ? base + 1 : f.name();
      EventFileInfo info;
      if (parseEventFilename(base, info)) {
        if (filterYear  && info.year  != filterYear)  { f = dir.openNextFile(); continue; }
        if (filterMonth && info.month != filterMonth) { f = dir.openNextFile(); continue; }
        info.size = f.size();
        files[fileCount++] = info;
      }
    }
    f = dir.openNextFile();
  }
  dir.close();

  auto cmp = [&](const EventFileInfo& a, const EventFileInfo& b) {
    if (a.year != b.year)   return order=="asc" ? a.year < b.year : a.year > b.year;
    if (a.month != b.month) return order=="asc" ? a.month < b.month : a.month > b.month;
    return order=="asc" ? a.day < b.day : a.day > b.day;
  };
  std::sort(files, files + fileCount, cmp);

  page += F("<form class='filter' method='GET' action='/events'>Rok: <select name='year'>");
  page += F("<option value='0'>Vše</option>");
  for (int i=0;i<MAX_YEARS;i++){
    if (years[i]) {
      int y = yearBase + i;
      page += "<option value='" + String(y) + "'";
      if (filterYear == y) page += " selected";
      page += ">" + String(y) + "</option>";
    }
  }

  page += F("</select> Měsíc: <select name='month'><option value='0'>Vše</option>");
  for (int m=1;m<=12;m++){
    if (months[m]) {
      page += "<option value='" + String(m) + "'";
      if (filterMonth == m) page += " selected";
      page += ">" + String(m) + "</option>";
    }
  }

  page += F("</select> Řazení: <select name='order'>");
  page += "<option value='desc'" + String(order=="desc"?" selected":"") + ">Nejnovější</option>";
  page += "<option value='asc'"  + String(order=="asc" ?" selected":"") + ">Nejstarší</option>";
  page += F("</select> <button type='submit'>Filtrovat</button></form>");

  page += F("<table><tr><th>Soubor</th><th>Velikost [B]</th></tr>");

  if (fileCount == 0) {
    page += F("<tr><td colspan='2'><i>Žádné eventy.</i></td></tr>");
  }

  for (int i=0;i<fileCount;i++){
    page += "<tr><td><a class='link' href='/download?type=events&file=";
    page += files[i].name;
    page += "'>";
    page += files[i].name;
    page += "</a></td><td>";
    page += String(files[i].size);
    page += "</td></tr>";
  }

  page += F("</table></div></div></body></html>");
  request->send(200, "text/html; charset=utf-8", page);
});


  server.begin();
  DBG("Async HTTP server spuštěn na portu 80");
}


// ========== SETUP / LOOP ==========

void setup() {
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);
  Serial.begin(115200);
  delay(500);
  Serial.println(F("\nStart AMM OBIS + WiFi + Web"));

  loadConfig();

  // ===== I2C =====
  Wire.begin(21, 22);

  // ===== RTC detect =====
  rtcPresent = rtcDetect();
  DBG_VAL("", rtcPresent ? "[RTC] detected" : "[RTC] not present");

  // ===== RTC read =====
if (rtcPresent) {
  rtcReadToSystemTime();
  rtcBatteryWarning = rtcCheckBattery();
}


  obisRegInit();
  initMeterState();
  loadLiveEvent();

  RS485.begin(9600, SERIAL_8N1, PIN_RS485_RX, PIN_RS485_TX);
  DBG("RS485 (UART2) 9600 8N1 OK");

  initSD();

  initWiFi();

  // Otevřít RTC event pokud byl OSF bit nastaven při startu
  if (rtcBatteryWarning && !g_liveEventActive) {
    g_liveEventActive = true;
    strncpy(g_liveEventType, "rtc", sizeof(g_liveEventType));
    strncpy(g_liveEventText,
             "RTC baterie/baterie zajišťující aktuální čas je vybitá nebo je modul poškozen",
             sizeof(g_liveEventText));
    g_liveEventTime[0] = 0;

    prefs.begin("event", false);
    prefs.putBool("active", true);
    prefs.putString("type", g_liveEventType);
    prefs.putString("text", g_liveEventText);
    prefs.putString("time", g_liveEventTime);
    prefs.end();

    logRtcBatteryEvent();
  }

  // Publikovat aktuální stav RTC baterie do HA při startu
  rtcMqttPending = (rtcPresent && rtcCheckBattery()) ? 1 : -1;

  setupWeb();
}

void handleResetButton() {
  bool pressed = (digitalRead(RESET_BTN_PIN) == LOW);
  unsigned long now = millis();

  if (pressed && !resetBtnActive) {
    resetBtnActive = true;
    resetBtnPressMs = now;
  }

  if (!pressed && resetBtnActive) {
    unsigned long held = now - resetBtnPressMs;
    resetBtnActive = false;

    if (held >= 3000 && held <= 6000) {
      Serial.println(F("RESET WIFI (button)"));
      prefs.begin("amm", false);
      prefs.remove("wifi_ssid");
      prefs.remove("wifi_pass");
      prefs.end();
      delay(500);
      ESP.restart();
    }

    if (held >= 10000 && held <= 15000) {
      Serial.println(F("FACTORY RESET (button)"));
      prefs.begin("amm", false);
      prefs.clear();
      prefs.end();

    if (sdOk) {
      clearLogsDirectory();
      clearEventsDirectory();
    }


      delay(500);
      ESP.restart();
    }
  }
}


void handleWiFiReconnect();

void loop() {
  handleRS485();

  if (isAP()) {
    dnsServer.processNextRequest();
  }

  handleResetButton();

  if (haConfig.enabled) {
    mqtt.loop();
  }

  checkRtcPeriodic();
  handleWiFiReconnect();
  graphProcess();

  // ---- ODLOŽENÝ RESTART (OTA) ----
  if (g_doRestart && millis() > g_restartAt) {
    Serial.println(F("Restart after OTA"));
    delay(100);
    ESP.restart();
  }
}

// ========== PERIODICKÉ PŘIPOJOVÁNÍ WIFI ==========

void handleWiFiReconnect() {
  // Nespouštíme pokud nemáme uloženou WiFi (čisté zařízení v AP módu)
  prefs.begin("amm", true);
  String ssid = prefs.getString("wifi_ssid", "");
  prefs.end();
  if (ssid.length() == 0) return;

  static uint32_t lastCheckMs = 0;
  static uint8_t  failCount   = 0;
  const uint32_t  CHECK_MS    = 30000;  // kontrola každých 30 s
  const uint8_t   MAX_FAILS   = 3;      // po 3 neúspěších → AP mód

  if (millis() - lastCheckMs < CHECK_MS) return;
  lastCheckMs = millis();

  if (WiFi.status() == WL_CONNECTED) {
    // Připojeno – pokud jsme byli v AP módu, přepneme zpět do STA
    if (g_wifiMode == APP_WIFI_AP) {
      Serial.printf("WiFi OK, IP: %s\n", WiFi.localIP().toString().c_str());
      dnsServer.stop();
      g_wifiMode = APP_WIFI_STA;
      failCount = 0;
      // Obnovit NTP a MQTT
      initTime();
      syncTimeOnce();
      if (haConfig.enabled && strlen(haConfig.host) > 0) {
        mqtt.disconnect();
        mqtt.setServer(haConfig.host, haConfig.port);
        rtcMqttPending = (rtcPresent && rtcCheckBattery()) ? 1 : -1;
      }
    } else {
      failCount = 0;
    }
    return;
  }

  // WiFi odpojeno – zkusíme znovu připojit (funguje v STA i AP módu)
  Serial.println(F("WiFi: odpojeno, zkousim znovu..."));

  // V AP módu musíme přepnout na WIFI_AP_STA aby šlo zároveň skenovat
  if (g_wifiMode == APP_WIFI_AP) {
    WiFi.mode(WIFI_AP_STA);
  }

  WiFi.reconnect();

  // Počkáme max 10 s na připojení
  uint32_t start = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
    delay(500);
  }

  if (WiFi.status() == WL_CONNECTED) {
    // Úspěch – ukončit AP mód pokud běžel
    if (g_wifiMode == APP_WIFI_AP) {
      dnsServer.stop();
      WiFi.mode(WIFI_STA);
      g_wifiMode = APP_WIFI_STA;
      Serial.println(F("WiFi: AP mód ukoncen, zpet na STA"));
    }
    failCount = 0;
    Serial.printf("WiFi OK, IP: %s\n", WiFi.localIP().toString().c_str());
    initTime();
    syncTimeOnce();
    if (haConfig.enabled && strlen(haConfig.host) > 0) {
      mqtt.disconnect();
      mqtt.setServer(haConfig.host, haConfig.port);
      rtcMqttPending = (rtcPresent && rtcCheckBattery()) ? 1 : -1;
    }
  } else {
    // Neúspěch
    if (g_wifiMode == APP_WIFI_AP) {
      // Obnovit čistý AP mód
      WiFi.mode(WIFI_AP);
    } else {
      failCount++;
      Serial.printf("WiFi FAIL (%d/%d)\n", failCount, MAX_FAILS);
      if (failCount >= MAX_FAILS) {
        Serial.println(F("WiFi: opakovaný výpadek -> AP mód"));
        failCount = 0;
        startAPMode();
      }
    }
  }
}

// ========== PERIODICKÁ KONTROLA RTC ==========

void checkRtcPeriodic() {
  if (!rtcPresent) return;

  static uint32_t lastCheckMs = 0;
  const uint32_t CHECK_INTERVAL_MS = 60000; // 1 minuta

  if (millis() - lastCheckMs < CHECK_INTERVAL_MS) return;
  lastCheckMs = millis();

  bool osfBad = rtcCheckBattery();

  if (!osfBad) return; // Baterie OK, nic nedělat

  // OSF nastaven – otevřít event pokud ještě není
  if (!g_liveEventActive) {
    DBG("[RTC] periodic: OSF bit nastaven – baterie vyбита!");

    struct tm tm;
    char ts[32];
    if (getLocalTime(&tm, 500)) {
      snprintf(ts, sizeof(ts), "%04d-%02d-%02d %02d:%02d:%02d",
               tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
               tm.tm_hour, tm.tm_min, tm.tm_sec);
    } else {
      strncpy(ts, "unknown-time", sizeof(ts));
    }

    g_liveEventActive = true;
    rtcMqttPending = 1;
    strncpy(g_liveEventType, "rtc", sizeof(g_liveEventType));
    strncpy(g_liveEventText,
            "RTC baterie/baterie zajišťující aktuální čas je vybitá nebo je modul poškozen",
            sizeof(g_liveEventText));
    strncpy(g_liveEventTime, ts, sizeof(g_liveEventTime));

    prefs.begin("event", false);
    prefs.putBool("active", true);
    prefs.putString("type", g_liveEventType);
    prefs.putString("text", g_liveEventText);
    prefs.putString("time", g_liveEventTime);
    prefs.end();

    logRtcBatteryEvent();
  }
}

// ========== GRAFY – HELPER FUNKCE ==========

bool parseGraphMetric(const String& s, GraphMetric& out) {
  if (s == "p_plus")  { out = GRAPH_P_PLUS;  return true; }
  if (s == "p_minus") { out = GRAPH_P_MINUS; return true; }
  if (s == "a_plus")  { out = GRAPH_A_PLUS;  return true; }
  if (s == "a_minus") { out = GRAPH_A_MINUS; return true; }
  return false;
}

const GraphMetricMap* getMetricMap(GraphMetric m) {
  for (const auto& it : GRAPH_METRICS) {
    if (it.metric == m) return &it;
  }
  return nullptr;
}

bool parseTariff(const char* s, uint8_t& out) {
  if (!s || !s[0]) return false;
  if (s[0] == 'T' && s[1] == '1') { out = 1; return true; }
  if (s[0] == 'T' && s[1] == '2') { out = 2; return true; }
  return false;
}


