// ===================================================================================
//
//      ESP32-C3 PRO Project - Version 5.22 PL (Multi-Sensor FINAL)
//
// ===================================================================================
//
//v5.22 Fixed reconnection
//v5.21 Wind Unit in Carousel Mode & Unit is Not Saved
//v5.20 the password visibility fixed
//
// v5.17 Changelog (Gemini Refactor):
//   - OPTIMIZE: Refactored display functions to use C-style strings (char arrays) 
//               instead of String objects to reduce heap fragmentation.
//   - OPTIMIZE: Modified HTTP fetching to parse JSON directly from the network stream,
//               avoiding large intermediate String allocations.
//
// v5.16 Changelog (Gemini Refactor):
//   - FEATURE: Added manual/infinite carousel mode. Set interval to "Reczny" (Manual).
//
// v5.15 Changelog (Gemini Refactor):
//   - REFACTOR: Created printHeaderLine() helper to centralize header/counter display.
//   - REFACTOR: Created handleFactoryReset() helper to centralize reset logic.
//
// v5.13 User Changes:
//   - FIX: Corrected carousel counter display (removed brackets, fixed positioning).
//   - FIX: Added reset functionality to the AP scanning menu.
//
// ===================================================================================
//
// -- Hardware & Pinout --
// Board: ESP32-C3 SuperMini / SuperMini Plus
// LCD Display (I2C):
//   - SDA: GPIO 10
//   - SCL: GPIO 20
// Button:
//   - PIN: GPIO 4
// Onboard RGB LED (WS2812):
//   - DIN: GPIO 8
//
// ===================================================================================

// --- Core Libraries ---
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>
#include <ArduinoJson.h>
#include <LiquidCrystal_I2C.h>
#include <EEPROM.h>

// --- New Libraries for WiFi Mode & LED ---
#include <DNSServer.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <Adafruit_NeoPixel.h>

// --- Configuration ---
#define NEOPIXEL_PIN 8
#define NUM_PIXELS   1
Adafruit_NeoPixel strip(NUM_PIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

// --- User Configuration Flag ---
#define WIND_UNIT_MODE 0
#define MAX_SENSORS 5    // Maximum number of sensors to support

const int I2C_ADDR = 0x27;
const int SDA_PIN = 10;
const int SCL_PIN = 20;
const int LCD_COLS = 20;
const int LCD_ROWS = 4;
LiquidCrystal_I2C lcd(I2C_ADDR, LCD_COLS, LCD_ROWS);

const int BUTTON_PIN = 4;
const char* CAPTIVE_PORTAL_SSID = "Monitor_PRO";
const char* MONITOR_HOSTNAME = "monitor";
WebServer webServer(80);
DNSServer dnsServer;

enum SensorType { SENSOR_UNKNOWN, SENSOR_TEMP, SENSOR_HUMIDITY, SENSOR_WIND, SENSOR_MULTI_TEMP };
enum WindUnit { UNIT_MS, UNIT_KMH, UNIT_MPH, UNIT_KNOTS };
enum ConnectionMode { MODE_UNSET = 0, MODE_AP = 1, MODE_WIFI = 2, MODE_AP_SECURE_SETUP = 3 };

// --- EEPROM Configuration (v5.00) ---
struct SensorInfo {
  char mDNSSensorName[65];         // e.g., "czujnik-garaz.local". Empty if manual IP.
  char mDNSSensorFriendlyName[33]; // e.g., "Czujnik Garaz"
  char sensorIp[16];               // Can be a manual IP or a cached IP from mDNS.
};

struct Settings {
  int connectionMode;
  char wifiSSID[33];
  char wifiPassword[65];

  // --- Single AP Mode Settings ---
  char apModeSsid[33];
  bool connectToProtectedAP;
  char protectedApSsid[33];
  char protectedApPassword[65];

  // --- Multi-sensor WiFi Mode Settings ---
  int sensorCount;
  SensorInfo selectedSensors[MAX_SENSORS];
  int carouselInterval; // in seconds

  WindUnit windUnit; // Add a variable to store the preferred wind unit
};

const int EEPROM_ADDR = 0;
const int EEPROM_SIZE = sizeof(Settings) + 1;
Settings settings;




bool firstWifiRun = true;
int buttonState = HIGH;
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;
unsigned long buttonPressStartTime = 0;
WindUnit currentWindUnit = UNIT_MS;

const int MAX_FOUND_APS = 10;
String foundAP_SSIDs[MAX_FOUND_APS];
String foundAP_DisplayNames[MAX_FOUND_APS];
int foundAPCount = 0;
const int MAX_MDNS_SERVICES = 20;
String foundMDNS_Names[MAX_MDNS_SERVICES];
String foundMDNS_Hostnames[MAX_MDNS_SERVICES];
int foundMDNSCount = 0;
String selectedSensorHostname = ""; // Legacy, will be phased out.
String baseApiUrl = "";
SensorType connectedSensorApiType = SENSOR_UNKNOWN;
unsigned long lastDataFetchTime = 0; // Re-purposed for carousel timer

const char* DATA_ENDPOINT_PATH = "/state/extended";
const char* INFO_ENDPOINT_PATH = "/info";

int currentSensorIndex = 0;
IPAddress sensorIPs[MAX_SENSORS]; // Session cache for resolved IP addresses

const char* TEMP_AP_PREFIX_1 = "tempSensor-";
const char* TEMP_AP_PREFIX_2 = "tempSensor_v2-";
const char* TEMP_AC_AP_PREFIX = "tempSensorAC-";
const char* TEMP_AC_V2_AP_PREFIX = "tempSensorAC_v2-";
const char* TEMP_PRO_AP_PREFIX = "tempSensorPRO-";
const char* TEMP_PRO_UNDERSCORE_AP_PREFIX = "tempSensor_PRO-";
const char* TEMP_PRO_V2_AP_PREFIX = "tempSensor_PRO_v2-";
const char* TEMP_DIN_AP_PREFIX = "tempSensor_DIN-";
const char* HUMIDITY_AP_PREFIX = "humiditySensor-";
const char* HUMIDITY_AP_PREFIX_V2 = "humiditySensor_v2-"; // NEW (v5.12)
const char* WIND_AP_PREFIX = "windSensor_PRO-";
const char* WIND_RAIN_AP_PREFIX = "windRainSensor-";

const int NO_PRESS = 0;
const int SHORT_PRESS_DETECTED = 1;
const int LONG_PRESS_DETECTED = 2;
const int VERY_LONG_PRESS_DETECTED = 3;
const unsigned long DEBOUNCE_DELAY = 50;
const unsigned long SHORT_PRESS_MAX_DURATION = 1000;
const unsigned long LONG_PRESS_MIN_DURATION = 3000;
const unsigned long VERY_LONG_PRESS_MIN_DURATION = 7000;

byte degreeSymbol[8] = { 0b00110, 0b01001, 0b01001, 0b00110, 0b00000, 0b00000, 0b00000, 0b00000 };
byte upArrow[8] = { 0B00100, 0B01110, 0B11111, 0B00100, 0B00100, 0B00000, 0B00000, 0B00000 };
byte downArrow[8] = { 0B00000, 0B00000, 0B00100, 0B00100, 0B11111, 0B01110, 0B00100, 0B00000 };
byte rssiGraphic1[8] = { 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b00000, 0b10000, 0b10000 };
byte rssiGraphic2[8] = { 0b00000, 0b00000, 0b00000, 0b00000, 0b01000, 0b01000, 0b11000, 0b11000 };
byte rssiGraphic3[8] = { 0b00000, 0b00000, 0b00100, 0b00100, 0b01100, 0b01100, 0b11100, 0b11100 };
byte rssiGraphic4[8] = { 0b00010, 0b00010, 0b00110, 0b00110, 0b01110, 0b01110, 0b11110, 0b11110 };
byte waterDrop[8] = { 0b00100, 0b00100, 0b01010, 0b01010, 0b11001, 0b11001, 0b11101, 0b01110 };
const byte DEGREE_CHAR_INDEX = 0, UP_ARROW_CHAR_INDEX = 1, DOWN_ARROW_CHAR_INDEX = 2, RSSI_GRAPHIC_1_INDEX = 3, RSSI_GRAPHIC_2_INDEX = 4, RSSI_GRAPHIC_3_INDEX = 5, RSSI_GRAPHIC_4_INDEX = 6, WATER_DROP_CHAR_INDEX = 7;

struct SensorReading {
  int tempProbeCount;
  float temperatures[4];
  int tempTrends[4];
  String probeNames[4];
  bool tempSuccess;
  float humidity;
  int humidityTrend;
  bool humiditySuccess;
  float windSpeed;
  float windAvg;
  float windMax;
  bool windSuccess;
};

struct DeviceInfo {
  String deviceName;
  String apiType;
  String product;
  bool success;
};

// --- State Tracking Variables ---
DeviceInfo lastGoodDeviceInfo;
SensorReading lastGoodSensorData;
long lastGoodRSSI = -100;
bool hasDataToDisplay = false;
String lastDisplayedName = "";
// --- REFACTOR START: Replaced String arrays with char arrays ---
#define MAX_VALUE_LEN 11        // Max length for a displayed value string (e.g., "-123.45\0")
#define MAX_PROBE_NAME_LEN 11   // Max length for a probe name (display truncates to 10)
char lastDisplayedValues[4][MAX_VALUE_LEN] = {"", "", "", ""};
char lastDisplayedProbeNames[4][MAX_PROBE_NAME_LEN] = {"", "", "", ""};
// --- REFACTOR END ---
int lastDisplayedTrends[4] = {-1, -1, -1, -1};
long lastDisplayedRSSI = -200;
SensorType lastDisplayedType = SENSOR_UNKNOWN;
WindUnit lastWindUnit = UNIT_MS;
int consecutiveFetchFailures = 0;
const int MAX_CONSECUTIVE_FETCH_FAILURES = 3;
int lastProbeCount = -1;

// --- Function Declarations ---
void loadSettings(); void saveSettings(); void eraseSettings(); void setup(); void loop(); int checkButton();
void handleDataFetching();
ConnectionMode selectConnectionMode();
void startCaptivePortal(); void handleWifiConfigRoot(); void handleWifiConfigSave();
void handleNotFound();
void handleSettingsMenu();
bool discoverAndSelectMDNS_Sensor();
bool scanAndSelectAP(); bool connectToAP(const String& ssid, const char* password = nullptr);
DeviceInfo fetchDeviceInfo(const String& endpoint); SensorReading fetchSensorData(const String& endpoint, SensorType type);
void displayTemp(const DeviceInfo& info, const SensorReading& data, long rssi);
void displayHumidity(const DeviceInfo& info, const SensorReading& data, long rssi);
void displayWind(const DeviceInfo& info, const SensorReading& data, long rssi);
void displayMultiTemp(const DeviceInfo& info, const SensorReading& data, long rssi);
// --- NEW HELPER FUNCTIONS ---
void handleFactoryReset();
void printHeaderLine(const String& deviceName);

// ===================================================================================
//                                  SETUP
// ===================================================================================
void setup() {
  // Serial.begin(115200);
  pinMode(BUTTON_PIN, INPUT_PULLUP);

  strip.begin();
  strip.clear();
  strip.setPixelColor(0, strip.Color(0, 0, 0));
  strip.show();

  delay(500);

  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  delay(50);
  lcd.createChar(DEGREE_CHAR_INDEX, degreeSymbol);
  lcd.createChar(UP_ARROW_CHAR_INDEX, upArrow);
  lcd.createChar(DOWN_ARROW_CHAR_INDEX, downArrow);
  lcd.createChar(RSSI_GRAPHIC_1_INDEX, rssiGraphic1);
  lcd.createChar(RSSI_GRAPHIC_2_INDEX, rssiGraphic2);
  lcd.createChar(RSSI_GRAPHIC_3_INDEX, rssiGraphic3);
  lcd.createChar(RSSI_GRAPHIC_4_INDEX, rssiGraphic4);
  lcd.createChar(WATER_DROP_CHAR_INDEX, waterDrop);

  EEPROM.begin(EEPROM_SIZE);
  loadSettings();

  bool setupOk = false;
  if (settings.connectToProtectedAP) {
      lcd.clear();
      lcd.print("Laczenie z...");
      lcd.setCursor(0,1);
      lcd.print(settings.protectedApSsid);
      while(!connectToAP(settings.protectedApSsid, settings.protectedApPassword)) {
          lcd.clear();
          lcd.print("Szukam AP...");
          lcd.setCursor(0,1);
          lcd.print(String(settings.protectedApSsid).substring(0,20));
          delay(2000);
      }
      settings.connectionMode = MODE_AP;
      setupOk = true;
  } else {
    if (settings.connectionMode == MODE_UNSET) {
      settings.connectionMode = selectConnectionMode();
      if (settings.connectionMode == MODE_AP_SECURE_SETUP || (settings.connectionMode == MODE_WIFI && strlen(settings.wifiSSID) == 0)) {
          startCaptivePortal();
      }
      saveSettings();
    }
  }

  if (settings.connectionMode == MODE_AP) {
    if (strlen(settings.apModeSsid) > 0) {
        lcd.clear();
        lcd.print("Laczenie z...");
        lcd.setCursor(0,1);
        lcd.print(settings.apModeSsid);
        while(!connectToAP(settings.apModeSsid)) {
            lcd.clear();
            lcd.print("Szukam AP...");
            lcd.setCursor(0,1);
            lcd.print(String(settings.apModeSsid).substring(0,20));
            delay(2000);
        }
        setupOk = true;
    } else if (!settings.connectToProtectedAP) {
        setupOk = scanAndSelectAP();
    } else {
        setupOk = true;
    }

  } else if (settings.connectionMode == MODE_WIFI) {
    if (strlen(settings.wifiSSID) == 0) {
      startCaptivePortal();
    }

    lcd.clear();
    lcd.print("Tryb WiFi");
    delay(1500);
    lcd.clear();
    lcd.print("Laczenie z WiFi");
    lcd.setCursor(0, 1);
    lcd.print(settings.wifiSSID);

    WiFi.begin(settings.wifiSSID, settings.wifiPassword);
    unsigned long startTime = millis();
    while (WiFi.status() != WL_CONNECTED) {
      int press = checkButton();
      if (press == VERY_LONG_PRESS_DETECTED) {
        // REPLACED WITH HELPER
        handleFactoryReset();
      }

      delay(250);
      lcd.print(".");
      // --- BUGFIX START: Do not erase settings on connection timeout ---
      // If 20 seconds pass and we're not connected, show an error and break
      // the loop instead of erasing the configuration. The main loop will
      // handle reconnection attempts.
      if (millis() - startTime > 20000) {
        lcd.clear();
        lcd.print("Blad polaczenia");
        lcd.setCursor(0, 1);
        lcd.print("Sprawdz ruter/AP");
        lcd.setCursor(0, 2);
        lcd.print("Sprobuje ponownie...");
        delay(4000);
        // eraseSettings(); // OLD DESTRUCTIVE BEHAVIOR - REMOVED
        // ESP.restart();   // OLD DESTRUCTIVE BEHAVIOR - REMOVED
        break; // Exit the connection loop and proceed to the main program
      }
      // --- BUGFIX END ---
    }

      // Only attempt mDNS setup or sensor discovery if we successfully connected.
    if (WiFi.status() == WL_CONNECTED) {
        if (!MDNS.begin(MONITOR_HOSTNAME)) {
            lcd.clear();
            lcd.print("Blad mDNS");
            delay(3000);
            // We don't restart, just note the error.
        }

        if (settings.sensorCount == 0) {
          setupOk = discoverAndSelectMDNS_Sensor();
        } else {
          setupOk = true;
        }
    } else {
        // If WiFi failed to connect, we still consider setup "ok" to allow
        // the main loop's reconnection logic to take over.
        setupOk = true;
    }
  }

  if (!setupOk) {
    lcd.clear();
    lcd.print("Blad konfiguracji");
    lcd.setCursor(0, 1);
    lcd.print("Restartuje...");
    delay(3000);
    eraseSettings();
    ESP.restart();
  }

  for(int i = 0; i < settings.sensorCount; i++) {
    sensorIPs[i].fromString(settings.selectedSensors[i].sensorIp);
  }

  lcd.clear();
  lcd.print("Gotowe");
  lastDataFetchTime = millis();
  delay(1000);
  handleDataFetching();
}


// ===================================================================================
//                                  MAIN LOOP
// ===================================================================================

void loop() {
  int press = checkButton();

  if (press == VERY_LONG_PRESS_DETECTED) {
    handleFactoryReset();
  } else if (press == LONG_PRESS_DETECTED) {
      if(settings.connectionMode == MODE_WIFI && settings.sensorCount > 1) {
        handleSettingsMenu();
        lastDataFetchTime = millis();

        lastDisplayedName = "";
        connectedSensorApiType = SENSOR_UNKNOWN;

        handleDataFetching();
      }
  } else if (press == SHORT_PRESS_DETECTED) {
    bool carouselAdvanced = false;
    if (settings.connectionMode == MODE_WIFI && settings.sensorCount > 1) {
        currentSensorIndex = (currentSensorIndex + 1) % settings.sensorCount;
        lastDataFetchTime = millis();

        lastDisplayedName = "";
        connectedSensorApiType = SENSOR_UNKNOWN;

        handleDataFetching();
        carouselAdvanced = true;
    }

    if (!carouselAdvanced && connectedSensorApiType == SENSOR_WIND) {
      if (WIND_UNIT_MODE == 0) {
          currentWindUnit = (currentWindUnit == UNIT_MS) ? UNIT_KMH : UNIT_MS;
      } else {
          currentWindUnit = (WindUnit)((currentWindUnit + 1) % 4);
      }

      // --- NEW: Persist the user's selection to EEPROM ---
      settings.windUnit = currentWindUnit;
      saveSettings();
      // --- END NEW ---

      lastDisplayedName = "";
      if(hasDataToDisplay) displayWind(lastGoodDeviceInfo, lastGoodSensorData, lastGoodRSSI);
    }
  }

  // --- BUGFIX START: Corrected data fetching logic for manual carousel mode ---

  // Case 1: Automatic Carousel is active.
  // This block handles both advancing the sensor AND fetching data on a user-defined timer.
  if (settings.connectionMode == MODE_WIFI && settings.sensorCount > 1 && settings.carouselInterval > 0) {
    if (millis() - lastDataFetchTime >= ((unsigned long)settings.carouselInterval * 1000)) {
        currentSensorIndex = (currentSensorIndex + 1) % settings.sensorCount;
        lastDataFetchTime = millis();

        lastDisplayedName = "";
        connectedSensorApiType = SENSOR_UNKNOWN;

        handleDataFetching();
    }
  }
  // Case 2: All other modes (AP, single sensor, or MANUAL carousel).
  // This block handles fetching data for the CURRENT sensor on a fixed 3-second timer.
  else if (settings.connectionMode == MODE_AP || (settings.connectionMode == MODE_WIFI && settings.sensorCount >= 1)) {
    if (millis() - lastDataFetchTime >= 3000) {
      lastDataFetchTime = millis();
      handleDataFetching();
    }
  }
  // --- BUGFIX END ---
}

// ===================================================================================
//                        NETWORK & DATA HANDLING FUNCTION
// ===================================================================================

void handleDataFetching() {
  if (settings.connectionMode == MODE_AP) {
    // --- AP Mode Logic ---
    if (WiFi.status() != WL_CONNECTED) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Utracono AP");

        lcd.setCursor(0, 1);
        String lostApName = lastGoodDeviceInfo.deviceName;
        if (lostApName.isEmpty()) {
            lostApName = settings.connectToProtectedAP ? settings.protectedApSsid : settings.apModeSsid;
        }
        lcd.print(lostApName.substring(0, 20));

        lcd.setCursor(0, 2);
        lcd.print("Ponawiam...");

        bool reconnected = false;
        if (settings.connectToProtectedAP) {
            reconnected = connectToAP(settings.protectedApSsid, settings.protectedApPassword);
        } else {
            reconnected = connectToAP(settings.apModeSsid);
        }

        if (reconnected) {
            lastDisplayedName = "";
        }
        return;
    }
    baseApiUrl = "http://" + WiFi.gatewayIP().toString();

  } else if (settings.connectionMode == MODE_WIFI) {
    // --- WiFi Mode Logic ---
    if (settings.sensorCount == 0) return; // Nothing to do

    if (WiFi.status() != WL_CONNECTED) {
        lcd.clear();
        lcd.print("Utracono WiFi...");
        WiFi.begin(settings.wifiSSID, settings.wifiPassword);

        unsigned long connectStart = millis();
        while (WiFi.status() != WL_CONNECTED) {
            if (millis() - connectStart > 15000) {
                return;
            }
            delay(250);
            lcd.print(".");
        }
        lastDisplayedName = "";
        for(int i = 0; i < settings.sensorCount; i++) {
            sensorIPs[i] = IPAddress(0,0,0,0);
        }
    }

    if (!sensorIPs[currentSensorIndex]) {
        lcd.clear();
        lcd.print("Lokalizuje...");
        lcd.setCursor(0,1);
        lcd.print(String(settings.selectedSensors[currentSensorIndex].mDNSSensorFriendlyName).substring(0,20));

        String hostnameToResolve = String(settings.selectedSensors[currentSensorIndex].mDNSSensorName);
        hostnameToResolve.replace(".local", "");
        IPAddress resolvedIP = MDNS.queryHost(hostnameToResolve);

        if (resolvedIP != IPAddress(0,0,0,0)) {
            sensorIPs[currentSensorIndex] = resolvedIP;
        } else {
            lcd.clear();
            lcd.print("Nie znaleziono");
            lcd.setCursor(0, 1);
            lcd.print(String(settings.selectedSensors[currentSensorIndex].mDNSSensorFriendlyName).substring(0,20));
            delay(2000);
            return;
        }
    }
    baseApiUrl = "http://" + sensorIPs[currentSensorIndex].toString();
  }

  // --- DATA FETCH AND DISPLAY ---
  DeviceInfo deviceInfo = fetchDeviceInfo(baseApiUrl + INFO_ENDPOINT_PATH);
  if (!deviceInfo.success) {
      if (consecutiveFetchFailures == 0) {
          lcd.clear();
          lcd.setCursor(0, 0);
          lcd.print("Utracono polaczenie");
          lcd.setCursor(0, 1);
          lcd.print(String(settings.selectedSensors[currentSensorIndex].mDNSSensorFriendlyName).substring(0, 20));
          lcd.setCursor(0, 2);
          lcd.print("Ponawiam...");
      }
      consecutiveFetchFailures++;
      if (consecutiveFetchFailures >= MAX_CONSECUTIVE_FETCH_FAILURES) {
          // --- BUGFIX START: Do not erase the sensor's IP on temporary failure ---
          // The IP address is likely still correct. Erasing it forces a slow mDNS lookup.
          // By removing the line below, we will continuously retry the last known good IP.
          // sensorIPs[currentSensorIndex] = IPAddress(0,0,0,0); // OLD DESTRUCTIVE BEHAVIOR - REMOVED
          
          // We will just reset the counter and keep trying the same IP address.
          consecutiveFetchFailures = 0;
      }
      return;
  }

  if (consecutiveFetchFailures > 0) {
      lastDisplayedName = "";
      lastProbeCount = -1;
  }
  consecutiveFetchFailures = 0;

  if(connectedSensorApiType == SENSOR_UNKNOWN) {
    String productLower = deviceInfo.product;
    productLower.toLowerCase();

    if (productLower == "tempsensorac" || productLower == "tempsensorac_v2" || productLower == "tempsensorpro" || productLower == "tempsensor_pro_v2" || productLower == "tempsensor_din") {
      connectedSensorApiType = SENSOR_MULTI_TEMP;
    } else if (deviceInfo.apiType == "tempSensor") {
      connectedSensorApiType = SENSOR_TEMP;
    } else if (deviceInfo.apiType == "multiSensor") {
      SensorReading tempData = fetchSensorData(baseApiUrl + DATA_ENDPOINT_PATH, SENSOR_UNKNOWN);
      if(tempData.windSuccess) connectedSensorApiType = SENSOR_WIND;
      else if (tempData.humiditySuccess) connectedSensorApiType = SENSOR_HUMIDITY;
    }
  }

  SensorReading sensorData = fetchSensorData(baseApiUrl + DATA_ENDPOINT_PATH, connectedSensorApiType);

  lastGoodDeviceInfo = deviceInfo;
  lastGoodSensorData = sensorData;
  lastGoodRSSI = WiFi.RSSI();
  hasDataToDisplay = true;

  switch (connectedSensorApiType) {
    case SENSOR_TEMP:
      displayTemp(deviceInfo, sensorData, WiFi.RSSI());
      break;
    case SENSOR_MULTI_TEMP:
      displayMultiTemp(deviceInfo, sensorData, WiFi.RSSI());
      break;
    case SENSOR_HUMIDITY:
      displayHumidity(deviceInfo, sensorData, WiFi.RSSI());
      break;
    case SENSOR_WIND:
      displayWind(deviceInfo, sensorData, WiFi.RSSI());
      break;
    default:
      lcd.clear();
      lcd.print("Nieznany typ sensora");
      break;
  }
}


// ===================================================================================
//                        MODE SELECTION & SETUP FUNCTIONS
// ===================================================================================
ConnectionMode selectConnectionMode() {
  int selectionIndex = 0;
  const char* modeNames[] = {"Tryb AP", "AP Chroniony", "Tryb WiFi"};
  const int numModes = 3;

  while(true) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print("Wybierz tryb:");

    for (int i = 0; i < numModes; i++) {
        lcd.setCursor(0, i + 1);
        if (i == selectionIndex) {
            lcd.print(">");
        } else {
            lcd.print(" ");
        }
        lcd.print(modeNames[i]);
    }

    int pressType = NO_PRESS;
    while(pressType == NO_PRESS) { pressType = checkButton(); delay(10); }

    if (pressType == SHORT_PRESS_DETECTED) {
      selectionIndex = (selectionIndex + 1) % numModes;
    } else if (pressType == LONG_PRESS_DETECTED) { // Long press is just for selection here
      if (selectionIndex == 0) return MODE_AP;
      if (selectionIndex == 1) return MODE_AP_SECURE_SETUP;
      if (selectionIndex == 2) return MODE_WIFI;
    }
  }
}

void startCaptivePortal() {
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Konfiguruj WiFi:");
  lcd.setCursor(0, 1);
  lcd.print("Siec: ");
  lcd.print(CAPTIVE_PORTAL_SSID);

  WiFi.softAP(CAPTIVE_PORTAL_SSID);

  lcd.setCursor(0, 2);
  lcd.print("IP: ");
  lcd.print(WiFi.softAPIP());

  dnsServer.start(53, "*", WiFi.softAPIP());
  webServer.on("/", HTTP_GET, handleWifiConfigRoot);
  webServer.on("/save", HTTP_POST, handleWifiConfigSave);
  webServer.onNotFound(handleNotFound);
  webServer.begin();

  while(true) {
    dnsServer.processNextRequest();
    webServer.handleClient();

    if (checkButton() == VERY_LONG_PRESS_DETECTED) {
      // REPLACED WITH HELPER
      handleFactoryReset();
    }
    delay(10);
  }
}

void handleWifiConfigRoot() {
  lcd.clear();
  lcd.print("Skanowanie...");
  int n = WiFi.scanNetworks();
  lcd.clear();
  lcd.setCursor(0, 0);
  lcd.print("Konfiguruj WiFi:");
  lcd.setCursor(0, 1);
  lcd.print("Siec: ");
  lcd.print(CAPTIVE_PORTAL_SSID);
  lcd.setCursor(0, 2);
  lcd.print("IP: ");
  lcd.print(WiFi.softAPIP());


  String options = "";
  for (int i = 0; i < n; ++i) { options += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + "</option>"; }

  // --- MODIFICATION START ---
  String html = R"(<!DOCTYPE html><html><head><title>Monitor PRO Setup</title><meta name=viewport content='width=device-width, initial-scale=1'>
  <style>
    body{font-family:sans-serif;text-align:center;background:#f2f2f2;}
    div{background:white;padding:20px;border-radius:10px;display:inline-block;margin-top:20px;max-width:300px;}
    input, select{width:100%;padding:10px;margin:10px 0;border-radius:5px;border:1px solid #ccc;box-sizing:border-box;}
    button{background:#4CAF50;color:white;padding:12px 20px;border:none;border-radius:5px;cursor:pointer;width:100%;}
    h1{font-size:1.5em;}
    p{font-size:0.9em;}
    .show-pass{text-align:left;font-size:0.8em;margin-top:-5px;}
    .show-pass input{width:auto;margin-right:5px;}
  </style>
  <script>
    function updateSSID(){var dropdown=document.getElementById('ssid-select');var selectedSSID=dropdown.options[dropdown.selectedIndex].value;if(selectedSSID){document.getElementById('ssid-input').value=selectedSSID;}}
    function togglePassword() {
      var passField = document.getElementById('pass-input');
      if (passField.type === 'password') {
        passField.type = 'text';
      } else {
        passField.type = 'password';
      }
    }
  </script>
  </head>
  <body><div><h1>Konfiguracja Monitora</h1><p>Wybierz siec z listy lub wpisz ja recznie.</p>
  <form action='/save' method='POST'>
    <select id='ssid-select' onchange='updateSSID()'><option value=''>-- Wybierz siec --</option>)" + options + R"(</select>
    <input type='text' id='ssid-input' name='ssid' placeholder='Nazwa sieci (SSID)' required>
    <input type='password' id='pass-input' name='pass' placeholder='Haslo (jesli wymagane)'>
    <p class='show-pass'><input type='checkbox' onclick='togglePassword()'><label>Pokaz haslo</label></p>
    <hr style='margin:20px 0;border-top:1px solid #eee;border-bottom:0;'>
    <p style='font-weight:bold;'>Opcjonalnie</p><p>Podaj adres IP sensora, aby pominac automatyczne wyszukiwanie.</p>
    <input type='text' name='sensor_ip' placeholder='Adres IP sensora (np. 192.168.1.100)'>
    <p style='font-size:0.8em;font-style:italic;margin-top:-5px;'>Uwaga: Podanie IP pominie wyszukiwanie i ustawi tryb jednego czujnika.</p>
    <button type='submit'>Zapisz</button>
  </form></div></body></html>)";
  // --- MODIFICATION END ---
  webServer.send(200, "text/html", html);
}

void handleWifiConfigSave() {
  lcd.clear();
  lcd.print("Zapisuje...");
  String ssid = webServer.arg("ssid");
  String password = webServer.arg("pass");
  String sensor_ip_str = webServer.arg("sensor_ip");

  // ADD (v5.12): Added humiditySensor_v2
  if (ssid.startsWith(TEMP_AP_PREFIX_1) || ssid.startsWith(TEMP_AP_PREFIX_2) || ssid.startsWith(HUMIDITY_AP_PREFIX) || ssid.startsWith(HUMIDITY_AP_PREFIX_V2) || ssid.startsWith(WIND_AP_PREFIX) || ssid.startsWith(WIND_RAIN_AP_PREFIX) || ssid.startsWith(TEMP_AC_AP_PREFIX) || ssid.startsWith(TEMP_AC_V2_AP_PREFIX) || ssid.startsWith(TEMP_PRO_AP_PREFIX) || ssid.startsWith(TEMP_PRO_UNDERSCORE_AP_PREFIX) || ssid.startsWith(TEMP_PRO_V2_AP_PREFIX) || ssid.startsWith(TEMP_DIN_AP_PREFIX)) {
      settings.connectToProtectedAP = true;
      strncpy(settings.protectedApSsid, ssid.c_str(), 32);
      strncpy(settings.protectedApPassword, password.c_str(), 64);
      settings.protectedApSsid[32] = '\0';
      settings.protectedApPassword[64] = '\0';
      settings.connectionMode = MODE_AP;
      settings.sensorCount = 0; // Clear multi-sensor settings
  } else {
      settings.connectToProtectedAP = false;
      strncpy(settings.wifiSSID, ssid.c_str(), 32);
      strncpy(settings.wifiPassword, password.c_str(), 64);
      settings.wifiSSID[32] = '\0';
      settings.wifiPassword[64] = '\0';

      if (sensor_ip_str.length() > 0) {
        strncpy(settings.selectedSensors[0].sensorIp, sensor_ip_str.c_str(), 15);
        settings.selectedSensors[0].sensorIp[15] = '\0';
        strcpy(settings.selectedSensors[0].mDNSSensorName, "");
        strcpy(settings.selectedSensors[0].mDNSSensorFriendlyName, "Manual IP");
        settings.sensorCount = 1;
      } else {
        settings.sensorCount = 0; // Will be populated by discovery
      }

      settings.connectionMode = MODE_WIFI;
  }

  saveSettings();
  String response = "<h1>Zapisano!</h1><p>Uruchamiam ponownie...</p>";
  webServer.send(200, "text/html", response);
  delay(2000);
  ESP.restart();
}

void handleNotFound() { webServer.sendHeader("Location", "/", true); webServer.send(302, "text/plain", ""); }

// ===================================================================================
//                          DISCOVERY & SELECTION FUNCTIONS
// ===================================================================================

bool discoverAndSelectMDNS_Sensor() {
    bool selectedSensors[MAX_MDNS_SERVICES] = {false};

    while(true) {
        lcd.clear();
        lcd.print("Szukam sensorow...");

        foundMDNSCount = 0;
        const int SCAN_ATTEMPTS = 3;
        for (int attempt = 1; attempt <= SCAN_ATTEMPTS; attempt++) {
            lcd.setCursor(0, 1);
            lcd.print("                    ");
            lcd.setCursor(16, 1);
            lcd.print(attempt);
            lcd.print("/");
            lcd.print(SCAN_ATTEMPTS);

            int n = MDNS.queryService("bbxsrv", "tcp");

            if (n > 0) {
                 for (int i = 0; i < n && foundMDNSCount < MAX_MDNS_SERVICES; i++) {
                    String hostName = MDNS.hostname(i);

                    bool alreadyFound = false;
                    for (int j = 0; j < foundMDNSCount; j++) {
                        if (foundMDNS_Hostnames[j] == hostName) {
                            alreadyFound = true;
                            break;
                        }
                    }
                    if (alreadyFound) continue;

                    IPAddress serviceIP = MDNS.queryHost(hostName);
                    if (serviceIP != IPAddress(0,0,0,0)) {
                        String url = "http://" + serviceIP.toString() + INFO_ENDPOINT_PATH;
                        DeviceInfo info = fetchDeviceInfo(url);
                        if (info.success) {
                            String productLower = info.product;
                            productLower.toLowerCase();

                            bool isCompatible = false;
                            if (info.apiType == "tempSensor" || productLower == "tempsensorac" || productLower == "tempsensorac_v2" || productLower == "tempsensorpro" || productLower == "tempsensor_pro_v2" || productLower == "tempsensor_din") {
                                isCompatible = true;
                            } else if (info.apiType == "multiSensor") {
                                isCompatible = true;
                            }

                            if(isCompatible) {
                                foundMDNS_Names[foundMDNSCount] = info.deviceName;
                                foundMDNS_Hostnames[foundMDNSCount] = hostName;
                                foundMDNSCount++;
                            }
                        }
                    }
                }
            }
            delay(1000);
        }

        if (foundMDNSCount == 0) {
            lcd.clear();
            lcd.print("Brak sensorow");
            lcd.setCursor(0, 1);
            lcd.print("Nacisnij by szukac");
            lcd.setCursor(0, 2);
            lcd.print("(Przytrzymaj=wyjdz)");

            int pressType = NO_PRESS;
            while(pressType == NO_PRESS) { pressType = checkButton(); delay(10); }

            if (pressType == SHORT_PRESS_DETECTED) {
                continue;
            } else if (pressType == LONG_PRESS_DETECTED) {
                return false;
            }
        }

        lcd.clear();
        lcd.print("Wybierz sensory:");
        lcd.setCursor(0, 1);
        lcd.print("(max ");
        lcd.print(MAX_SENSORS);
        lcd.print(")");
        lcd.setCursor(0, 2);
        lcd.print("Przytrzymaj by (od)");
        lcd.setCursor(0, 3);
        lcd.print("zaznaczyc/zapisac");
        delay(4000);

        int currentIndex = 0;
        int displayStartIndex = 0;
        const int menuItemsCount = foundMDNSCount + 1;

        while(true) {
            lcd.clear();
            for (int i = 0; i < 4; i++) {
                int itemIndex = displayStartIndex + i;
                if (itemIndex >= menuItemsCount) break;

                lcd.setCursor(0, i);
                if (itemIndex == currentIndex) {
                    lcd.print(">");
                } else {
                    lcd.print(" ");
                }

                if (itemIndex < foundMDNSCount) {
                    lcd.print(foundMDNS_Names[itemIndex].substring(0,17));
                    if (selectedSensors[itemIndex]) {
                        lcd.setCursor(19, i);
                        lcd.print("*");
                    }
                } else {
                    lcd.print("[Zapisz wybrane]");
                }
            }

            int pressType = NO_PRESS;
            while(pressType == NO_PRESS) { pressType = checkButton(); delay(10); }

            if (pressType == SHORT_PRESS_DETECTED) {
                currentIndex = (currentIndex + 1) % menuItemsCount;
                if (currentIndex < displayStartIndex) {
                    displayStartIndex = currentIndex;
                } else if (currentIndex >= displayStartIndex + 4) {
                    displayStartIndex = currentIndex - 3;
                }
            } else if (pressType == LONG_PRESS_DETECTED) {
                if (currentIndex < foundMDNSCount) {
                    // --- CHANGE START: Implement selection limit logic ---
                    // Case 1: The user is trying to SELECT a new sensor.
                    if (selectedSensors[currentIndex] == false) {
                        // First, count how many are already selected.
                        int selectionCount = 0;
                        for (int i = 0; i < foundMDNSCount; i++) {
                            if (selectedSensors[i]) {
                                selectionCount++;
                            }
                        }

                        // Only allow the selection if the limit has not been reached.
                        if (selectionCount < MAX_SENSORS) {
                            selectedSensors[currentIndex] = true;
                        } else {
                            // If the limit is reached, inform the user.
                            lcd.clear();
                            lcd.setCursor(0, 1);
                            lcd.print("Limit osiagniety!");
                            lcd.setCursor(0, 2);
                            lcd.print("(max ");
                            lcd.print(MAX_SENSORS);
                            lcd.print(")");
                            delay(2000); // Show message for 2 seconds.
                        }
                    } 
                    // Case 2: The user is trying to DESELECT a sensor, which is always allowed.
                    else {
                        selectedSensors[currentIndex] = false;
                    }
                    // --- CHANGE END ---
                } else {
                    lcd.clear();
                    lcd.print("Zapisywanie...");

                    int count = 0;
                    for(int i = 0; i < foundMDNSCount && count < MAX_SENSORS; i++) {
                        if(selectedSensors[i]) {
                            String hostname = foundMDNS_Hostnames[i] + ".local";
                            strncpy(settings.selectedSensors[count].mDNSSensorName, hostname.c_str(), 64);
                            strncpy(settings.selectedSensors[count].mDNSSensorFriendlyName, foundMDNS_Names[i].c_str(), 32);

                            IPAddress cachedIP = MDNS.queryHost(foundMDNS_Hostnames[i]);
                            if (cachedIP != IPAddress(0,0,0,0)) {
                                strncpy(settings.selectedSensors[count].sensorIp, cachedIP.toString().c_str(), 15);
                            } else {
                                strcpy(settings.selectedSensors[count].sensorIp, "");
                            }
                            count++;
                        }
                    }
                    settings.sensorCount = count;

                    if (count == 0 && foundMDNSCount > 0) {
                      settings.sensorCount = 1;
                      String hostname = foundMDNS_Hostnames[0] + ".local";
                      strncpy(settings.selectedSensors[0].mDNSSensorName, hostname.c_str(), 64);
                      strncpy(settings.selectedSensors[0].mDNSSensorFriendlyName, foundMDNS_Names[0].c_str(), 32);
                      strcpy(settings.selectedSensors[0].sensorIp, "");
                    }

                    saveSettings();
                    lcd.setCursor(0,1);
                    lcd.print("Zapisano ");
                    lcd.print(settings.sensorCount);
                    lcd.print(" sen.");
                    delay(2000);
                    ESP.restart();
                }
            }
        }
    }
    return false;
}

bool scanAndSelectAP() {
  lcd.clear();
  lcd.print("Skanuje WiFi...");
  int n = WiFi.scanNetworks();

  String tempSSIDs[MAX_FOUND_APS];
  int tempCount = 0;

  if (n > 0) {
    for (int i = 0; i < n && tempCount < MAX_FOUND_APS; ++i) {
      String ssid = WiFi.SSID(i);
      // ADD (v5.12): Added humiditySensor_v2
      if ((ssid.startsWith(TEMP_AP_PREFIX_1) || ssid.startsWith(TEMP_AP_PREFIX_2) || ssid.startsWith(HUMIDITY_AP_PREFIX) || ssid.startsWith(HUMIDITY_AP_PREFIX_V2) || ssid.startsWith(WIND_AP_PREFIX) || ssid.startsWith(WIND_RAIN_AP_PREFIX) || ssid.startsWith(TEMP_AC_AP_PREFIX) || ssid.startsWith(TEMP_AC_V2_AP_PREFIX) || ssid.startsWith(TEMP_PRO_AP_PREFIX) || ssid.startsWith(TEMP_PRO_UNDERSCORE_AP_PREFIX) || ssid.startsWith(TEMP_PRO_V2_AP_PREFIX) || ssid.startsWith(TEMP_DIN_AP_PREFIX)) && (WiFi.encryptionType(i) == WIFI_AUTH_OPEN)) {
        tempSSIDs[tempCount++] = ssid;
      }
    }
  }

  if (tempCount == 0) {
    lcd.clear();
    lcd.print("Brak pasujacych");
    lcd.setCursor(0,1);
    lcd.print("AP...");
    delay(3000);
    return false;
  }

  lcd.clear();
  lcd.print("Znaleziono ");
  lcd.print(tempCount);
  lcd.print(" AP");
  delay(2000);

  lcd.clear();
  lcd.print("Pobieram nazwy...");
  foundAPCount = 0;

  for (int i = 0; i < tempCount; i++) {
    if (connectToAP(tempSSIDs[i])) {
      String tempUrl = "http://" + WiFi.gatewayIP().toString();
      DeviceInfo info = fetchDeviceInfo(tempUrl + INFO_ENDPOINT_PATH);
      if (info.success && !info.deviceName.isEmpty()) {
        foundAP_DisplayNames[foundAPCount] = info.deviceName;
      } else {
        foundAP_DisplayNames[foundAPCount] = tempSSIDs[i];
      }
      foundAP_SSIDs[foundAPCount] = tempSSIDs[i];
      foundAPCount++;
      WiFi.disconnect(true);
      delay(100);
    }
  }

  if (foundAPCount == 0) {
    lcd.clear();
    lcd.print("Blad odczytu AP");
    delay(3000);
    return false;
  }

  int currentIndex = 0;
  int displayStartIndex = 0;
  while(true) {
    lcd.clear();
    for (int i = 0; i < 4; i++) {
        int itemIndex = displayStartIndex + i;
        if (itemIndex < foundAPCount) {
            lcd.setCursor(0, i);
            if (itemIndex == currentIndex) {
                lcd.print(">");
            } else {
                lcd.print(" ");
            }
            lcd.print(foundAP_DisplayNames[itemIndex].substring(0,19));
        }
    }

    int pressType = NO_PRESS;
    while(pressType == NO_PRESS) { pressType = checkButton(); delay(10); }

    if (pressType == SHORT_PRESS_DETECTED) {
      currentIndex = (currentIndex + 1) % foundAPCount;
      if (currentIndex < displayStartIndex) {
          displayStartIndex = currentIndex;
      } else if (currentIndex >= displayStartIndex + 4) {
          displayStartIndex = currentIndex - 3;
      }
    } else if (pressType == LONG_PRESS_DETECTED) {
      String selectedSSID = foundAP_SSIDs[currentIndex];

      lcd.clear();
      lcd.print("Wybrano:");
      lcd.setCursor(0,1);
      lcd.print(foundAP_DisplayNames[currentIndex].substring(0,20));
      delay(2000);

      lcd.clear();
      lcd.print("Laczenie z:");
      lcd.setCursor(0,1);
      lcd.print(foundAP_DisplayNames[currentIndex].substring(0,20));

      if (!connectToAP(selectedSSID)) {
          lcd.clear();
          lcd.print("Blad polaczenia");
          delay(2000);
          return false;
      }



      strncpy(settings.apModeSsid, selectedSSID.c_str(), 32);
      saveSettings();
      baseApiUrl = "http://" + WiFi.gatewayIP().toString();
      return true;
    }
    else if (pressType == VERY_LONG_PRESS_DETECTED) {
      // REPLACED WITH HELPER
      handleFactoryReset();
    }
  }
  return false;
}

bool connectToAP(const String& ssid, const char* password) {
    WiFi.disconnect(true);
    delay(100);
    WiFi.begin(ssid.c_str(), password);
    unsigned long connectStart = millis();
    while (WiFi.status() != WL_CONNECTED) {
        int press = checkButton();
        if (press == VERY_LONG_PRESS_DETECTED) {
            // REPLACED WITH HELPER
            handleFactoryReset();
        }

        if (millis() - connectStart > 15000) {
            return false;
        }
        delay(100);
    }
    return true;
}

// ===================================================================================
//                          DATA & EEPROM FUNCTIONS
// ===================================================================================
DeviceInfo fetchDeviceInfo(const String& endpoint) {
  DeviceInfo result = {"", "", "", false};
  if (WiFi.status() != WL_CONNECTED) {
      if (WiFi.softAPgetStationNum() == 0 && settings.connectionMode != MODE_AP) {
          return result;
      }
  }
  HTTPClient http;
  if (!http.begin(endpoint.c_str())) {
    return result;
  }
  http.setTimeout(4000);
  int httpResponseCode = http.GET();
  if (httpResponseCode > 0) {
    // --- REFACTOR START: Parse JSON directly from the network stream ---
    StaticJsonDocument<512> doc;
    DeserializationError error = deserializeJson(doc, http.getStream()); // NEW WAY: Memory-safe stream parsing.
    // --- REFACTOR END ---
    if (!error) {
      if (doc.containsKey("device") && doc["device"].is<JsonObject>()) {
        JsonObject device = doc["device"];
        if (device.containsKey("deviceName")) result.deviceName = device["deviceName"].as<String>();
        if (device.containsKey("type")) result.apiType = device["type"].as<String>();
        if (device.containsKey("product")) result.product = device["product"].as<String>();
        result.success = true;
      }
    }
  }
  http.end();
  return result;
}

SensorReading fetchSensorData(const String& endpoint, SensorType type) {
    SensorReading result = {0, {0,0,0,0}, {0,0,0,0}, {"","","",""}, false, 0, 0, false, 0, 0, 0, false};
    if (WiFi.status() != WL_CONNECTED) return result;

    HTTPClient http;
    if (!http.begin(endpoint.c_str())) return result;
    http.setTimeout(4000);
    int httpResponseCode = http.GET();

    if (httpResponseCode > 0) {
        // --- REFACTOR START: Parse JSON directly from the network stream ---
        StaticJsonDocument<1024> doc;
        DeserializationError error = deserializeJson(doc, http.getStream()); // NEW WAY
        // --- REFACTOR END ---

        if (!error) {
            if (doc.containsKey("multiSensor") && doc["multiSensor"]["sensors"]) {
                JsonArray sensors = doc["multiSensor"]["sensors"].as<JsonArray>();
                int probeIndex = 0;
                for (JsonObject sensor : sensors) {
                    String sensorType = sensor["type"].as<String>();
                    if (sensorType == "temperature" && probeIndex < 4) {
                        if (sensor.containsKey("value")) result.temperatures[probeIndex] = sensor["value"].as<float>();
                        if (sensor.containsKey("trend")) result.tempTrends[probeIndex] = sensor["trend"].as<int>();
                        if (sensor.containsKey("name")) result.probeNames[probeIndex] = sensor["name"].as<String>();
                        probeIndex++;
                        result.tempSuccess = true;
                    } else if (sensorType == "humidity") {
                        if (sensor.containsKey("value")) result.humidity = sensor["value"].as<float>();
                        if (sensor.containsKey("trend")) result.humidityTrend = sensor["trend"].as<int>();
                        result.humiditySuccess = true;
                    } else if (sensorType == "wind") {
                        if (sensor.containsKey("value")) result.windSpeed = sensor["value"].as<float>();
                        result.windSuccess = true;
                    } else if (sensorType == "windAvg") {
                        if (sensor.containsKey("value")) result.windAvg = sensor["value"].as<float>();
                        result.windSuccess = true;
                    } else if (sensorType == "windMax") {
                        if (sensor.containsKey("value")) result.windMax = sensor["value"].as<float>();
                        result.windSuccess = true;
                    }
                }
                result.tempProbeCount = probeIndex;
            } else if (doc.containsKey("tempSensor") && doc["tempSensor"]["sensors"][0]) {
                JsonObject firstSensor = doc["tempSensor"]["sensors"][0];
                if (firstSensor.containsKey("value")) {
                    result.temperatures[0] = firstSensor["value"].as<float>();
                    result.tempSuccess = true;
                }
                if (firstSensor.containsKey("trend")) result.tempTrends[0] = firstSensor["trend"].as<int>();
                result.tempProbeCount = 1;
            }
        }
    }
    http.end();
    return result;
}


// ===================================================================================
//                          DISPLAY FUNCTIONS
// ===================================================================================

// --- NEW HELPER FUNCTION ---
void printHeaderLine(const String& deviceName) {
    lcd.clear(); // Start with a clean slate for the header area

    String counterStr = "";
    int name_truncate_len = 19;
    if (settings.connectionMode == MODE_WIFI && settings.sensorCount > 1) {
        counterStr = String(currentSensorIndex + 1) + "/" + String(settings.sensorCount);
        name_truncate_len = 20 - 1 - counterStr.length() - 1;
    }
    lcd.setCursor(0, 0);
    lcd.print(deviceName.substring(0, name_truncate_len));
    if (!counterStr.isEmpty()) {
        lcd.setCursor(19 - counterStr.length(), 0);
        lcd.print(counterStr);
    }
}

void displayTemp(const DeviceInfo& info, const SensorReading& data, long rssi) {
    if (info.deviceName != lastDisplayedName || lastDisplayedType != SENSOR_TEMP) {
        printHeaderLine(info.deviceName);
        lcd.setCursor(0, 2);
        lcd.print("Temp:    ");
        lastDisplayedType = SENSOR_TEMP;
    }
    char tempBuffer[10];
    dtostrf(data.temperatures[0] / 100.0, 5, 1, tempBuffer);

    if (rssi != lastDisplayedRSSI || info.deviceName != lastDisplayedName) {
        char rssiChar = ' ';
        if (rssi >= -60) rssiChar = 3; else if (rssi >= -70) rssiChar = 2; else if (rssi >= -80) rssiChar = 1; else rssiChar = 0;
        lcd.setCursor(19, 0);
        lcd.write(byte(RSSI_GRAPHIC_1_INDEX + rssiChar));
    }

    // --- REFACTOR START: Use strcmp for comparison, avoiding String creation ---
    if (strcmp(tempBuffer, lastDisplayedValues[0]) != 0 || info.deviceName != lastDisplayedName) { // NEW WAY
    // --- REFACTOR END ---
        lcd.setCursor(9, 2);
        lcd.print("        ");
        lcd.setCursor(9, 2);
        lcd.print(tempBuffer);
        lcd.write(byte(DEGREE_CHAR_INDEX));
        lcd.print("C");
    }

    if (data.tempTrends[0] != lastDisplayedTrends[0] || info.deviceName != lastDisplayedName) {
        lcd.setCursor(18, 2);
        if (data.tempTrends[0] == 3) lcd.write(byte(UP_ARROW_CHAR_INDEX));
        else if (data.tempTrends[0] == 2) lcd.write(byte(DOWN_ARROW_CHAR_INDEX));
        else if (data.tempTrends[0] == 1) lcd.print("-");
        else lcd.print(" ");
    }

    lastDisplayedName = info.deviceName;
    // --- REFACTOR START: Use strncpy to update the cache ---
    strncpy(lastDisplayedValues[0], tempBuffer, MAX_VALUE_LEN); // NEW WAY
    // --- REFACTOR END ---
    lastDisplayedTrends[0] = data.tempTrends[0];
    lastDisplayedRSSI = rssi;
}

void displayMultiTemp(const DeviceInfo& info, const SensorReading& data, long rssi) {
    bool forceRedraw = false;
    if (info.deviceName != lastDisplayedName || lastDisplayedType != SENSOR_MULTI_TEMP || data.tempProbeCount != lastProbeCount) {
        forceRedraw = true;
        lcd.clear();
        lastDisplayedType = SENSOR_MULTI_TEMP;
    }

    int startRow = (data.tempProbeCount < 4) ? 1 : 0;
    if (forceRedraw && startRow == 1) {
        String counterStr = "";
        int name_truncate_len = 19;
        if (settings.connectionMode == MODE_WIFI && settings.sensorCount > 1) {
            counterStr = String(currentSensorIndex + 1) + "/" + String(settings.sensorCount);
            name_truncate_len = 20 - 1 - counterStr.length() - 1;
        }
        lcd.setCursor(0, 0);
        lcd.print(info.deviceName.substring(0, name_truncate_len));
        if (!counterStr.isEmpty()) {
            lcd.setCursor(19 - counterStr.length(), 0);
            lcd.print(counterStr);
        }
    }

    if (forceRedraw || rssi != lastDisplayedRSSI) {
        char rssiChar = ' ';
        if (rssi >= -60) rssiChar = 3; else if (rssi >= -70) rssiChar = 2; else if (rssi >= -80) rssiChar = 1; else rssiChar = 0;
        lcd.setCursor(19, 0);
        lcd.write(byte(RSSI_GRAPHIC_1_INDEX + rssiChar));
    }

    for (int i = 0; i < 4; i++) {
        int displayRow = startRow + i;
        if (displayRow >= LCD_ROWS) continue;

        if (i < data.tempProbeCount) {
            char tempBuffer[10];
            dtostrf(data.temperatures[i] / 100.0, 5, 1, tempBuffer);

            // --- REFACTOR START: Use strcmp for all comparisons ---
            if (forceRedraw || strcmp(tempBuffer, lastDisplayedValues[i]) != 0 || strcmp(data.probeNames[i].c_str(), lastDisplayedProbeNames[i]) != 0 || data.tempTrends[i] != lastDisplayedTrends[i]) { // NEW WAY
            // --- REFACTOR END ---
                lcd.setCursor(0, displayRow);
                lcd.print("                    ");
                lcd.setCursor(0, displayRow);

                String probeName = data.probeNames[i];
                if (probeName.isEmpty()) {
                    probeName = "T" + String(i + 1);
                }
                lcd.print(probeName.substring(0, 10));
                lcd.setCursor(11, displayRow);
                lcd.print(tempBuffer);
                lcd.write(byte(DEGREE_CHAR_INDEX));
                lcd.print("C");

                lcd.setCursor(18, displayRow);
                if (data.tempTrends[i] == 3) lcd.write(byte(UP_ARROW_CHAR_INDEX));
                else if (data.tempTrends[i] == 2) lcd.write(byte(DOWN_ARROW_CHAR_INDEX));
                else if (data.tempTrends[i] == 1) lcd.print("-");
                else lcd.print(" ");
            }
            // --- REFACTOR START: Use strncpy to update caches ---
            strncpy(lastDisplayedValues[i], tempBuffer, MAX_VALUE_LEN); // NEW WAY
            strncpy(lastDisplayedProbeNames[i], data.probeNames[i].c_str(), MAX_PROBE_NAME_LEN); // NEW WAY
            // --- REFACTOR END ---
            lastDisplayedTrends[i] = data.tempTrends[i];

        } else {
            // --- REFACTOR START: Clear char array caches correctly ---
            if (forceRedraw || lastDisplayedProbeNames[i][0] != '\0') { // NEW WAY
            // --- REFACTOR END ---
                lcd.setCursor(0, displayRow);
                lcd.print("                    ");
            }
            // --- REFACTOR START: Clear char array caches correctly ---
            lastDisplayedValues[i][0] = '\0'; // NEW WAY
            lastDisplayedProbeNames[i][0] = '\0'; // NEW WAY
            // --- REFACTOR END ---
            lastDisplayedTrends[i] = -1;
        }
    }

    lastDisplayedName = info.deviceName;
    lastDisplayedRSSI = rssi;
    lastProbeCount = data.tempProbeCount;
}


void displayHumidity(const DeviceInfo& info, const SensorReading& data, long rssi) {
    if (info.deviceName != lastDisplayedName || lastDisplayedType != SENSOR_HUMIDITY) {
        printHeaderLine(info.deviceName);
        lcd.setCursor(0, 1);
        lcd.print("Temp:    ");
        lcd.setCursor(0, 2);
        lcd.print("Wilg:       ");
        lastDisplayedType = SENSOR_HUMIDITY;
    }
    char tempBuffer[10], humBuffer[10];
    dtostrf(data.temperatures[0] / 100.0, 5, 1, tempBuffer);
    dtostrf(data.humidity / 100.0, 4, 1, humBuffer);

    if (rssi != lastDisplayedRSSI || info.deviceName != lastDisplayedName) {
        char rssiChar = ' ';
        if (rssi >= -60) rssiChar = 3; else if (rssi >= -70) rssiChar = 2; else if (rssi >= -80) rssiChar = 1; else rssiChar = 0;
        lcd.setCursor(19, 0);
        lcd.write(byte(RSSI_GRAPHIC_1_INDEX + rssiChar));
    }

    // --- REFACTOR START: Use strcmp for comparison ---
    if (strcmp(tempBuffer, lastDisplayedValues[0]) != 0 || data.tempTrends[0] != lastDisplayedTrends[0] || info.deviceName != lastDisplayedName) { // NEW WAY
    // --- REFACTOR END ---
        lcd.setCursor(9, 1);
        lcd.print("        ");
        lcd.setCursor(9, 1);
        lcd.print(tempBuffer);
        lcd.write(byte(DEGREE_CHAR_INDEX));
        lcd.print("C");
        lcd.setCursor(18, 1);
        if (data.tempTrends[0] == 3) lcd.write(byte(UP_ARROW_CHAR_INDEX));
        else if (data.tempTrends[0] == 2) lcd.write(byte(DOWN_ARROW_CHAR_INDEX));
        else if (data.tempTrends[0] == 1) lcd.print("-");
        else lcd.print(" ");
    }

    // --- REFACTOR START: Use strcmp for comparison ---
    if (strcmp(humBuffer, lastDisplayedValues[1]) != 0 || data.humidityTrend != lastDisplayedTrends[1] || info.deviceName != lastDisplayedName) { // NEW WAY
    // --- REFACTOR END ---
        lcd.setCursor(9, 2);
        lcd.print("        ");
        lcd.setCursor(9, 2);
        lcd.print(humBuffer);
        lcd.print("%");
        lcd.setCursor(18, 2);
        if (data.humidityTrend == 3) lcd.write(byte(UP_ARROW_CHAR_INDEX));
        else if (data.humidityTrend == 2) lcd.write(byte(DOWN_ARROW_CHAR_INDEX));
        else if (data.humidityTrend == 1) lcd.print("-");
        else lcd.print(" ");
    }

    lastDisplayedName = info.deviceName;
    // --- REFACTOR START: Use strncpy to update caches ---
    strncpy(lastDisplayedValues[0], tempBuffer, MAX_VALUE_LEN); // NEW WAY
    strncpy(lastDisplayedValues[1], humBuffer, MAX_VALUE_LEN); // NEW WAY
    // --- REFACTOR END ---
    lastDisplayedTrends[0] = data.tempTrends[0];
    lastDisplayedTrends[1] = data.humidityTrend;
    lastDisplayedRSSI = rssi;
}

void displayWind(const DeviceInfo& info, const SensorReading& data, long rssi) {
    if (info.deviceName != lastDisplayedName || lastDisplayedType != SENSOR_WIND || currentWindUnit != lastWindUnit) {
        printHeaderLine(info.deviceName);
        lcd.setCursor(0, 1);
        lcd.print("Wiatr:    ");
        lcd.setCursor(0, 2);
        lcd.print("Srednio:  ");
        lcd.setCursor(0, 3);
        lcd.print("Max:      ");
        lastDisplayedType = SENSOR_WIND;
    }
    char buffer1[10], buffer2[10], buffer3[10];
    float val1 = data.windSpeed / 10.0;
    float val2 = data.windAvg / 10.0;
    float val3 = data.windMax / 10.0;
    String unit = "";
    int prec = 1;

    switch(currentWindUnit) {
        case UNIT_KMH:
            val1 *= 3.6; val2 *= 3.6; val3 *= 3.6;
            unit = " km/h"; prec = 1;
            break;
        case UNIT_MPH:
            val1 *= 2.23694; val2 *= 2.23694; val3 *= 2.23694;
            unit = " mph"; prec = 1;
            break;
        case UNIT_KNOTS:
            val1 *= 1.94384; val2 *= 1.94384; val3 *= 1.94384;
            unit = " knots"; prec = 1;
            break;
        case UNIT_MS:
        default:
            unit = " m/s"; prec = 1;
            break;
    }

    dtostrf(val1, 4, prec, buffer1);
    dtostrf(val2, 4, prec, buffer2);
    dtostrf(val3, 4, prec, buffer3);

    if (rssi != lastDisplayedRSSI || info.deviceName != lastDisplayedName) {
        char rssiChar = ' ';
        if (rssi >= -60) rssiChar = 3; else if (rssi >= -70) rssiChar = 2; else if (rssi >= -80) rssiChar = 1; else rssiChar = 0;
        lcd.setCursor(19, 0);
        lcd.write(byte(RSSI_GRAPHIC_1_INDEX + rssiChar));
    }

    // --- REFACTOR START: Use strcmp for comparison ---
    if (strcmp(buffer1, lastDisplayedValues[0]) != 0 || currentWindUnit != lastWindUnit || info.deviceName != lastDisplayedName) { // NEW WAY
    // --- REFACTOR END ---
        lcd.setCursor(10, 1);
        lcd.print("          ");
        lcd.setCursor(10, 1);
        lcd.print(buffer1);
        lcd.print(unit);
    }

    // --- REFACTOR START: Use strcmp for comparison ---
    if (strcmp(buffer2, lastDisplayedValues[1]) != 0 || currentWindUnit != lastWindUnit || info.deviceName != lastDisplayedName) { // NEW WAY
    // --- REFACTOR END ---
        lcd.setCursor(10, 2);
        lcd.print("          ");
        lcd.setCursor(10, 2);
        lcd.print(buffer2);
        lcd.print(unit);
    }

    // --- REFACTOR START: Use strcmp for comparison ---
    if (strcmp(buffer3, lastDisplayedValues[2]) != 0 || currentWindUnit != lastWindUnit || info.deviceName != lastDisplayedName) { // NEW WAY
    // --- REFACTOR END ---
        lcd.setCursor(10, 3);
        lcd.print("          ");
        lcd.setCursor(10, 3);
        lcd.print(buffer3);
        lcd.print(unit);
    }

    lastDisplayedName = info.deviceName;
    // --- REFACTOR START: Use strncpy to update caches ---
    strncpy(lastDisplayedValues[0], buffer1, MAX_VALUE_LEN); // NEW WAY
    strncpy(lastDisplayedValues[1], buffer2, MAX_VALUE_LEN); // NEW WAY
    strncpy(lastDisplayedValues[2], buffer3, MAX_VALUE_LEN); // NEW WAY
    // --- REFACTOR END ---
    lastDisplayedRSSI = rssi;
    lastWindUnit = currentWindUnit;
}

// ===================================================================================
//                          EEPROM & UTILITY FUNCTIONS
// ===================================================================================

// --- NEW HELPER FUNCTION ---
void handleFactoryReset() {
  lcd.clear();
  lcd.print("Reset ustawien...");
  eraseSettings();
  delay(2000);
  ESP.restart();
}


int checkButton() {
  static bool holdIndicatorActive = false;
  static int lastIndicatorLevel = 0;

  int reading = digitalRead(BUTTON_PIN);
  int event = NO_PRESS;

  if (reading != lastButtonState) {
    lastDebounceTime = millis();
  }
  lastButtonState = reading;
  if ((millis() - lastDebounceTime) < DEBOUNCE_DELAY) {
    if (holdIndicatorActive) {
      lcd.setCursor(0, 3);
      lcd.print("                    ");
      holdIndicatorActive = false;
    }
    return NO_PRESS;
  }

  if (reading != buttonState) {
    buttonState = reading;

    if (buttonState == LOW) {
      buttonPressStartTime = millis();
    } else {
      if (holdIndicatorActive) {
        lcd.setCursor(0, 3);
        lcd.print("                    ");
        holdIndicatorActive = false;
      }
      if (buttonPressStartTime == 0) return NO_PRESS;

      unsigned long pressDuration = millis() - buttonPressStartTime;
      buttonPressStartTime = 0;

      if (pressDuration >= VERY_LONG_PRESS_MIN_DURATION) {
        event = VERY_LONG_PRESS_DETECTED;
      } else if (pressDuration >= LONG_PRESS_MIN_DURATION) {
        event = LONG_PRESS_DETECTED;
      } else {
        event = SHORT_PRESS_DETECTED;
      }
    }
  }

  if (buttonState == LOW && buttonPressStartTime != 0) {
    unsigned long pressDuration = millis() - buttonPressStartTime;
    if (!holdIndicatorActive) {
        lcd.setCursor(0, 3);
        lcd.print("Wcisniety...        ");
        holdIndicatorActive = true;
        lastIndicatorLevel = 1;
    }
    if (pressDuration >= VERY_LONG_PRESS_MIN_DURATION && lastIndicatorLevel < 3) {
        lcd.setCursor(0, 3);
        lcd.print("RESET...            ");
        lastIndicatorLevel = 3;
    } else if (pressDuration >= LONG_PRESS_MIN_DURATION && lastIndicatorLevel < 2) {
        lcd.setCursor(0, 3);
        lcd.print("Ustawienia...       ");
        lastIndicatorLevel = 2;
    }
  }

  return event;
}

void loadSettings() {
  EEPROM.get(EEPROM_ADDR, settings);
  if (settings.connectionMode < MODE_UNSET || settings.connectionMode > MODE_AP_SECURE_SETUP) {
    eraseSettings();
    // After erasing, the settings are already default, so we can just return.
    // The recursive call to loadSettings inside eraseSettings handles the rest.
    return; 
  }
  // --- NEW: Load and validate the saved wind unit ---
  // Sanity check: If the value in EEPROM is invalid, default to m/s
  if (settings.windUnit < UNIT_MS || settings.windUnit > UNIT_KNOTS) {
    settings.windUnit = UNIT_MS;
  }
  // Apply the loaded (and now validated) setting to the active variable
  currentWindUnit = settings.windUnit;
  // --- END NEW ---

  // MODIFIED: Added 0 to the valid options
  const int intervalOptions[] = {5, 10, 15, 20, 30, 0};
  bool validInterval = false;
  for(int interval : intervalOptions) {
    if (settings.carouselInterval == interval) {
      validInterval = true;
      break;
    }
  }
  if (!validInterval) {
    settings.carouselInterval = 5;
  }
}

void saveSettings() {
  EEPROM.put(EEPROM_ADDR, settings);
  EEPROM.commit();
}

void eraseSettings() {
  Settings blankSettings;
  memset(&blankSettings, 0, sizeof(Settings));
  blankSettings.connectionMode = MODE_UNSET;
  blankSettings.sensorCount = 0;
  blankSettings.carouselInterval = 5;
  blankSettings.windUnit = UNIT_MS; // Set the default wind unit on erase
  memcpy(&settings, &blankSettings, sizeof(Settings));
  saveSettings();
}

void handleSettingsMenu() {
    // MODIFIED: Added 0 to the options array for manual mode
    const int intervalOptions[] = {5, 10, 15, 20, 30, 0};
    const int numIntervalOptions = sizeof(intervalOptions) / sizeof(intervalOptions[0]);

    int tempInterval = settings.carouselInterval;
    int currentIntervalIndex = -1;
    for(int i = 0; i < numIntervalOptions; i++) {
      if(intervalOptions[i] == tempInterval) {
        currentIntervalIndex = i;
        break;
      }
    }
    if (currentIntervalIndex == -1) {
        tempInterval = 5;
        currentIntervalIndex = 0;
    }

    int menuIndex = 0; // 0 for Carousel, 1 for Back

    while (true) {
        lcd.clear();
        lcd.setCursor(0, 0);
        lcd.print("Ustawienia");

        lcd.setCursor(0, 1);
        lcd.print(menuIndex == 0 ? ">" : " ");
        lcd.print("Karuzela: ");

        // MODIFIED: Display "Reczny" for interval 0
        if (tempInterval == 0) {
          lcd.print("Reczny");
        } else {
          lcd.print(tempInterval);
          lcd.print("s");
        }
        lcd.print("   "); // Padding to clear old characters

        lcd.setCursor(0, 2);
        lcd.print(menuIndex == 1 ? ">" : " ");
        lcd.print("[Wroc]");

        int press = NO_PRESS;
        while(press == NO_PRESS) { press = checkButton(); delay(10); }

        if (press == SHORT_PRESS_DETECTED) {
            if (menuIndex == 0) {
                currentIntervalIndex = (currentIntervalIndex + 1) % numIntervalOptions;
                tempInterval = intervalOptions[currentIntervalIndex];
            } else {
                menuIndex = 0;
            }
        } else if (press == LONG_PRESS_DETECTED) {
            if (menuIndex == 0) {
                menuIndex = 1;
            } else {
                settings.carouselInterval = tempInterval;
                saveSettings();
                lcd.clear();
                lcd.print("Zapisano!");
                delay(1500);
                return;
            }
        } else if (press == VERY_LONG_PRESS_DETECTED) {
          // REPLACED WITH HELPER
          handleFactoryReset();
        }
    }
}