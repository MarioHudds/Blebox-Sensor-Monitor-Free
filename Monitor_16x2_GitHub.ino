// ===================================================================================
//
//      ESP32-C3 TSHS Project - Version 3.44 PL (Upgraded)
//
// ===================================================================================
//
// v3.44 Changelog:
//   - FIX (Protected AP Mode): The monitor no longer resets if the saved protected
//     sensor AP is unavailable on startup. It now uses the same persistent
//     reconnection logic as the standard AP mode.
//
// v3.43 Changelog:
//   - FIX (AP Mode): The monitor no longer resets its settings if the saved sensor
//     AP is unavailable on startup. It will now persistently try to reconnect
//     until successful, while still allowing for a manual reset via long press.
//
// v3.42 Changelog:
//   - FIX: Implemented a visual indicator for sensor connection loss.
//   - UX: When the sensor is disconnected, the monitor now displays "Utracono sygnal"
//     (Signal lost) instead of showing stale data, while continuing to attempt
//     reconnection in the background.
//
// ===================================================================================
//
// -- Hardware & Pinout --
// Board: ESP32-C3 SuperMini / SuperMini Plus
// LCD Display (I2C): 16x2
//   - SDA: GPIO 10
//   - SCL: GPIO 20
// Button:
//   - PIN: GPIO 4
// Onboard RGB LED (WS2812):
//   - DIN: GPIO 8
//
// ===================================================================================


// --- Core Libraries ---
// [EDUKACJA]: Podstawowe biblioteki do obsługi sieci, zapytań HTTP i interfejsów sprzętowych.
#include <WiFi.h>
#include <HTTPClient.h>
#include <Wire.h>               // Obsługa magistrali I2C (dla wyświetlacza)
#include <ArduinoJson.h>        // Parsowanie odpowiedzi z sensorów Blebox (które zwracają dane w formacie JSON)
#include <LiquidCrystal_I2C.h>  // Sterowanie ekranem LCD
#include <EEPROM.h>             // Zapisywanie ustawień w pamięci nieulotnej (flash)

// --- New Libraries for WiFi Mode & LED ---
// [EDUKACJA]: Biblioteki do zaawansowanych funkcji sieciowych.
#include <DNSServer.h>          // Przechwytywanie zapytań DNS (niezbędne do działania "Captive Portal")
#include <WebServer.h>          // Wbudowany miniserwer WWW do konfiguracji przez przeglądarkę
#include <ESPmDNS.h>            // Multicast DNS: pozwala szukać urządzeń po nazwach (np. sensor.local) bez znajomości ich IP
#include <Adafruit_NeoPixel.h>

// --- Configuration ---
#define NEOPIXEL_PIN 8
#define NUM_PIXELS   1
Adafruit_NeoPixel strip(NUM_PIXELS, NEOPIXEL_PIN, NEO_GRB + NEO_KHZ800);

const int I2C_ADDR = 0x27; // [EDUKACJA]: Najpopularniejszy adres I2C dla tanich modułów LCD. Jeśli masz krzaczki na ekranie, sprawdź 0x3F.
const int SDA_PIN = 10;
const int SCL_PIN = 20;
LiquidCrystal_I2C lcd(I2C_ADDR, 16, 2);

const int BUTTON_PIN = 4;
const char* CAPTIVE_PORTAL_SSID = "Monitor_TSHS";
const char* MONITOR_HOSTNAME = "monitor";
WebServer webServer(80); // [EDUKACJA]: Serwer nasłuchuje na standardowym porcie HTTP (80)
DNSServer dnsServer;

// --- EEPROM Configuration ---
// [EDUKACJA][BEZPIECZEŃSTWO]: Ta struktura przechowuje dane konfiguracyjne. 
// Uwaga dla zaawansowanych: hasła (wifiPassword) są tutaj zapisywane jawnym tekstem. 
// Przy fizycznym dostępie do układu ESP32 można je odczytać. W projektach domowych to akceptowalne ryzyko.
struct Settings {
  int connectionMode;
  char wifiSSID[33];
  char wifiPassword[65];
  char mDNSSensorName[65];
  char mDNSSensorFriendlyName[33];
  char apModeSsid[33];

// New fields for protected AP mode
  bool connectToProtectedAP;
  char protectedApSsid[33];
  char protectedApPassword[65];
};
const int EEPROM_ADDR = 0;

const int EEPROM_SIZE = sizeof(Settings) + 1; // [EDUKACJA]: Rezerwujemy dokładnie tyle bajtów, ile waży nasza struktura
Settings settings;

// --- Enums, Globals, Constants, Structs... ---
enum ConnectionMode { MODE_UNSET = 0, MODE_AP = 1, MODE_WIFI = 2, MODE_AP_SECURE_SETUP = 3 };

bool firstWifiRun = true;
int buttonState = HIGH;
int lastButtonState = HIGH;
unsigned long lastDebounceTime = 0;

unsigned long buttonPressStartTime = 0;
bool longPressTriggered = false;
const int MAX_FOUND_APS = 10;
String foundAP_SSIDs[MAX_FOUND_APS];
String foundAP_DisplayNames[MAX_FOUND_APS];

int foundAPCount = 0;
const int MAX_MDNS_SERVICES = 10;
String foundMDNS_Names[MAX_MDNS_SERVICES];
String foundMDNS_Hostnames[MAX_MDNS_SERVICES];
int foundMDNSCount = 0;

String selectedSensorHostname = "";
String baseApiUrl = "";
IPAddress sensorIP;

// Variable to cache the sensor's IP address
String connectedSensorType = "";
unsigned long lastDataFetchTime = 0;

// [EDUKACJA]: Interwał odpytywania (polling) ustawiony na 5000 ms (5 sekund). 
// Sensory Blebox nie potrafią same "wypchnąć" (push) informacji o każdej drobnej zmianie temperatury 
// bez skomplikowanej konfiguracji dziesiątek akcji. Dlatego nasz monitor regularnie odpytuje sensor o stan.
const unsigned long DATA_FETCH_INTERVAL = 5000; 

const char* TEMP_ENDPOINT_PATH = "/state";
const char* INFO_ENDPOINT_PATH = "/info";

const char* TEMP_AP_PREFIX_1 = "tempSensor-";
const char* TEMP_AP_PREFIX_2 = "tempSensor_v2-";
const char* HUMIDITY_AP_PREFIX = "humiditySensor-";
const int NO_PRESS = 0;

const int SHORT_PRESS_DETECTED = 1;
const int LONG_PRESS_DETECTED = 2;
const unsigned long DEBOUNCE_DELAY = 50; // [EDUKACJA]: Eliminacja drgań styków przycisku sprzętowego

const unsigned long SHORT_PRESS_MAX_DURATION = 1000; // Krótkie kliknięcie (do 1 sekundy)
const unsigned long LONG_PRESS_MIN_DURATION = 3000;  // Długie kliknięcie (powyżej 3 sekund, np. do resetu)

// [EDUKACJA]: Definicje własnych znaków (piksel po pikselu) dla ekranu HD44780
byte degreeSymbol[8] = { B00110, B01001, B01001, B00110, B00000, B00000, B00000, B00000 };
byte upArrow[8] = { B00100, B01110, B11111, B00100, B00100, B00000, B00000, B00000 };
byte downArrow[8] = { B00000, B00000, B00100, B00100, B11111, B01110, B00100, B00000 };
byte rssiGraphic1[8] = { B00000, B00000, B00000, B00000, B00000, B00000, B10000, B10000 };
byte rssiGraphic2[8] = { B00000, B00000, B00000, B00000, B01000, B01000, B11000, B11000 };
byte rssiGraphic3[8] = { B00000, B00000, B00100, B00100, B01100, B01100, B11100, B11100 };
byte rssiGraphic4[8] = { B00010, B00010, B00110, B00110, B01110, B01110, B11110, B11110 };
byte waterDrop[8] = { B00100, B00100, B01010, B01010, B11001, B11001, B11101, B01110 };

const byte DEGREE_CHAR_INDEX = 0, UP_ARROW_CHAR_INDEX = 1, DOWN_ARROW_CHAR_INDEX = 2, RSSI_GRAPHIC_1_INDEX = 3, RSSI_GRAPHIC_2_INDEX = 4, RSSI_GRAPHIC_3_INDEX = 5, RSSI_GRAPHIC_4_INDEX = 6, WATER_DROP_CHAR_INDEX = 7;

// [EDUKACJA]: Struktury grupujące powiązane ze sobą dane dla porządku w kodzie
struct SensorReading { float temperature; int tempTrend; bool tempSuccess; float humidity; int humidityTrend; bool humiditySuccess; };

struct DeviceInfo { String deviceName; String apiType; bool success; };
String lastDisplayedName = "", lastDisplayedTempValueStr = "", lastDisplayedHumidityValueStr = "";

int lastDisplayedTempTrend = -1, lastDisplayedHumidityTrend = -1;
long lastDisplayedRSSI = 999;
byte lastDisplayedRssiGraphicIndex = ' ';
String lastConnectedSensorType = "";

int consecutiveFetchFailures = 0;
const int MAX_CONSECUTIVE_FETCH_FAILURES = 3;
bool sensorIsConnected = true;

// Tracks the connection state to prevent screen flicker

// --- Function Declarations ---
void loadSettings(); void saveSettings(); void eraseSettings(); void setup();

void loop(); int checkButton();
ConnectionMode selectConnectionMode();
void startCaptivePortal(); void handleWifiConfigRoot(); void handleWifiConfigSave();
void handleNotFound();
bool discoverAndSelectMDNS_Sensor();
bool scanAndSelectAP();

bool connectToAP(const String& ssid, const char* password = nullptr);
DeviceInfo fetchDeviceInfo(const String& endpoint); SensorReading fetchTempSensorData(const String& endpoint);

SensorReading fetchMultiSensorData(const String& endpoint);

// ===================================================================================
//                                  SETUP
// ===================================================================================
void setup() {
  pinMode(BUTTON_PIN, INPUT_PULLUP); // [EDUKACJA]: Używamy wbudowanego rezystora podciągającego. Przycisk zwiera do masy (GND).

  strip.begin();
  strip.clear();
  strip.setPixelColor(0, strip.Color(0, 0, 0)); // Wyłączenie drażniącej, wbudowanej diody RGB na płytce SuperMini
  strip.show();

  Wire.begin(SDA_PIN, SCL_PIN);
  lcd.init();
  lcd.backlight();
  lcd.clear();
  delay(50);
  
  // [EDUKACJA]: Wgrywanie własnych znaków do pamięci sterownika ekranu
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

  // =======================================================================
  // --- MODIFIED BLOCK FOR PROTECTED AP MODE STARTUP ---
  // =======================================================================
  if (settings.connectToProtectedAP) {
      lcd.clear();

      lcd.print("AP Chroniony");
      delay(1500);
      lcd.clear();
      lcd.print("Laczenie z...");
      lcd.setCursor(0,1);
      lcd.print(settings.protectedApSsid);
      
      // Persistent retry loop for protected AP.
      // [EDUKACJA]: Blokuje działanie układu do skutku, zapobiegając "sypaniu się" logiki, gdy sensor wstaje wolniej niż nasz ekran.
      while (!connectToAP(settings.protectedApSsid, settings.protectedApPassword)) {
          lcd.clear();
          lcd.print("Brak AP");

// "AP not found"
          lcd.setCursor(0, 1);
          lcd.print("Ponawiam za 2s...");

// "Retrying in 2s..."
          delay(2000);

// Wait before the next full connection attempt.
      }
      settings.connectionMode = MODE_AP;

// If we exit the loop, we are connected.
  // =======================================================================
  // --- END OF MODIFIED BLOCK ---
  // =======================================================================
  } else {
    // Normal startup sequence if not using a protected AP
    if (settings.connectionMode == MODE_UNSET) {
      settings.connectionMode = selectConnectionMode();

      if (settings.connectionMode == MODE_AP_SECURE_SETUP) {
          startCaptivePortal();

// This will handle setup and restart
      }
      saveSettings();

    }
  }

  bool setupOk = false;
  if (settings.connectionMode == MODE_AP) {
    if (strlen(settings.apModeSsid) > 0) { // A standard (unprotected) sensor AP has been saved
        lcd.clear();

        lcd.print("Laczenie z...");
        lcd.setCursor(0,1);
        lcd.print(settings.apModeSsid);

        // Persistent retry loop for standard AP.
        while (!connectToAP(settings.apModeSsid)) {
            lcd.clear();
            lcd.print("Nie znaleziono AP");

// "AP not found"
            lcd.setCursor(0, 1);
            lcd.print("Ponawiam za 2s...");

// "Retrying in 2s..."
            delay(2000);

// Wait before the next full connection attempt.
        }
        setupOk = true;

// If we exit the loop, it means we are connected.

    } else if (!settings.connectToProtectedAP) { // No AP saved, so scan for a new one
        setupOk = scanAndSelectAP();

    } else {
        setupOk = true;

// Already connected to a protected AP from the logic above
    }

  } else if (settings.connectionMode == MODE_WIFI) {
    // [EDUKACJA]: Tryb WiFi z infrastrukturą. Łączymy się do domowego routera, co wymaga podania hasła.
    if (strlen(settings.wifiSSID) == 0) {
      startCaptivePortal(); // Brak zapisanej sieci - uruchamiamy portal konfiguracyjny (własny Access Point ESP)
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
      if (checkButton() == LONG_PRESS_DETECTED) {
        lcd.clear();

        lcd.print("Anulowano.");
        lcd.setCursor(0,1);
        lcd.print("Reset ustawien...");
        eraseSettings(); // [BEZPIECZEŃSTWO]: Twardy reset usuwa zapisane jawnym tekstem hasła z pamięci flash
        delay(2000);
        ESP.restart();
      }
      
      delay(250);

      lcd.print(".");
      if (millis() - startTime > 20000) { // Timeout 20 sekund
        lcd.clear();
        lcd.print("Blad polaczenia");
        lcd.setCursor(0, 1);

        lcd.print("Sprawdz haslo...");
        delay(3000);
        eraseSettings(); 
        ESP.restart();
      }
    }

    // [EDUKACJA]: Uruchomienie mDNS. Pozwala to sieci wiedzieć, że to urządzenie nazywa się "monitor".
    if (!MDNS.begin(MONITOR_HOSTNAME)) {
        lcd.clear();

        lcd.print("Blad mDNS");
        delay(3000);
        ESP.restart();
    }
    
    if (strlen(settings.mDNSSensorName) == 0) {
      setupOk = discoverAndSelectMDNS_Sensor();

    } else {
      selectedSensorHostname = settings.mDNSSensorName;
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

  lcd.clear();
  lcd.print("Gotowe");
  lastDataFetchTime = 0;
  delay(1000);
  lcd.clear();
}


// ===================================================================================
//                                  MAIN LOOP
// ===================================================================================

void loop() {
  if (checkButton() == LONG_PRESS_DETECTED) {
    lcd.clear();

    lcd.print("Reset ustawien"); 
    lcd.setCursor(0, 1);
    lcd.print("Restartuje...");
    eraseSettings();
    delay(2000);
    ESP.restart();
  }

  // [EDUKACJA]: Klasyczny wzorzec z użyciem millis() zamiast delay(). 
  // Pozwala na wykonywanie innych akcji (jak sprawdzanie przycisku) bez "zawieszania" mikrokontrolera.
  if (millis() - lastDataFetchTime >= DATA_FETCH_INTERVAL) {
    lastDataFetchTime = millis();

    // --- Connection Handling (AP & WiFi) ---
    // This part ensures the device is connected to a network before fetching data.

    if (settings.connectionMode == MODE_AP) {
      if (WiFi.status() != WL_CONNECTED) {
          lcd.clear();

          lcd.print("Utracono AP"); lcd.setCursor(0,1); lcd.print("Ponawiam...");
          if (settings.connectToProtectedAP) {
              connectToAP(settings.protectedApSsid, settings.protectedApPassword);

          } else {
              connectToAP(settings.apModeSsid);

          }
          return;

      }
      baseApiUrl = "http://" + WiFi.gatewayIP().toString();

    } else if (settings.connectionMode == MODE_WIFI) {
      if (WiFi.status() != WL_CONNECTED) {
          lcd.clear();

          lcd.print("Utracono WiFi"); lcd.setCursor(0,1); lcd.print("Ponawiam...");
          WiFi.begin(settings.wifiSSID, settings.wifiPassword);
          unsigned long connectStart = millis();

          while (WiFi.status() != WL_CONNECTED) {
              if (checkButton() == LONG_PRESS_DETECTED) {
                  lcd.clear();

                  lcd.print("Reset ustawien..."); eraseSettings(); delay(2000); ESP.restart();
              }
              if (millis() - connectStart > 10000) { return;

              }
              delay(100);

          }
      }
      if (firstWifiRun) {
          lcd.clear();

          lcd.print("Lokalizuje..."); lcd.setCursor(0,1); lcd.print(settings.mDNSSensorFriendlyName);
          delay(3000);
          firstWifiRun = false;
      }
      
      // [EDUKACJA]: Zapytanie mDNS (Multicast DNS). Bleboxy rozgłaszają się w sieci pod unikalnymi nazwami (.local).
      // Zamiast sztywnego IP, ESP szuka urządzenia odpowiadającego tej nazwie.
      if (!sensorIP) {
          int n = MDNS.queryService("bbxsrv", "tcp");

          bool found = false;
          if (n > 0) {
              for (int i = 0; i < n; i++) {
                  String foundHost = String(MDNS.hostname(i)) + ".local";

                  if (foundHost == selectedSensorHostname) {
                      sensorIP = MDNS.queryHost(MDNS.hostname(i));

                      if (sensorIP) { found = true; break; }
                  }
              }
          }
          if (!found) {
              lcd.clear();

              lcd.print("Nie znaleziono"); lcd.setCursor(0, 1); lcd.print(settings.mDNSSensorFriendlyName);
              delay(2000);
              return;
          }
      }
      baseApiUrl = "http://" + sensorIP.toString();

    }

    // --- DATA FETCH AND DISPLAY LOGIC ---
    // [EDUKACJA]: Tutaj wykonujemy fizyczne zapytanie GET po HTTP do sensora Blebox.
    DeviceInfo currentDeviceInfo = fetchDeviceInfo(baseApiUrl + INFO_ENDPOINT_PATH);

    SensorReading sensorData;
    bool dataFetchSuccessful = false;

    if (connectedSensorType.isEmpty() && currentDeviceInfo.success) { connectedSensorType = currentDeviceInfo.apiType;

    }
    
    // [EDUKACJA]: Bleboxy mają różne API zależnie od rodzaju (multiSensor vs pojedynczy). Parsujemy odpowiedni punkt wejścia.
    if (connectedSensorType == "multiSensor") { 
        sensorData = fetchMultiSensorData(baseApiUrl + TEMP_ENDPOINT_PATH);

        dataFetchSuccessful = sensorData.tempSuccess || sensorData.humiditySuccess;
    } else { 
        sensorData = fetchTempSensorData(baseApiUrl + TEMP_ENDPOINT_PATH);

        dataFetchSuccessful = sensorData.tempSuccess;
    }

    if (dataFetchSuccessful) {
        // --- On Success: Reset counter and display data ---
        consecutiveFetchFailures = 0;

        sensorIsConnected = true; // Mark as connected

        long currentRSSI = WiFi.RSSI();

        String currentNameStr = currentDeviceInfo.success && !currentDeviceInfo.deviceName.isEmpty() ? currentDeviceInfo.deviceName : "Brak nazwy";
        char tempFormatBuffer[10];
        String currentTempValueStr = "N/A";

        if (sensorData.tempSuccess) { dtostrf(sensorData.temperature / 100.0, 4, 1, tempFormatBuffer); currentTempValueStr = String(tempFormatBuffer);

        }
        
        char humFormatBuffer[10];

        String currentHumidityValueStr = "N/A";
        if (sensorData.humiditySuccess && connectedSensorType == "multiSensor") { dtostrf(sensorData.humidity / 100.0, 4, 1, humFormatBuffer); currentHumidityValueStr = String(humFormatBuffer);

        }
        
        int currentTempTrend = sensorData.tempSuccess ?

        sensorData.tempTrend : 0;
        int currentHumidityTrend = sensorData.humiditySuccess ? sensorData.humidityTrend : 0;
        
        // [EDUKACJA]: Przeliczenie surowej wartości dBi na ikonkę zasięgu
        byte currentRssiGraphicToDisplay = ' ';

        if (currentRSSI >= -60) currentRssiGraphicToDisplay = RSSI_GRAPHIC_4_INDEX;
        else if (currentRSSI >= -70) currentRssiGraphicToDisplay = RSSI_GRAPHIC_3_INDEX;

        else if (currentRSSI >= -80) currentRssiGraphicToDisplay = RSSI_GRAPHIC_2_INDEX;
        else if (currentRSSI < -80 ) currentRssiGraphicToDisplay = RSSI_GRAPHIC_1_INDEX;

        // This logic now only runs on a successful fetch, or if something has changed
        // [EDUKACJA]: Optymalizacja! Odświeżamy powolny ekran LCD tylko wtedy, gdy dane uległy zmianie, co zapobiega irytującemu mruganiu.
        if (currentNameStr != lastDisplayedName || currentTempValueStr != lastDisplayedTempValueStr || currentHumidityValueStr != lastDisplayedHumidityValueStr || currentTempTrend != lastDisplayedTempTrend || currentHumidityTrend != lastDisplayedHumidityTrend || currentRssiGraphicToDisplay != lastDisplayedRssiGraphicIndex || connectedSensorType != lastConnectedSensorType) {
            lcd.clear();

            lcd.setCursor(0, 0);
            String nameToDisplay = currentNameStr;
            if (nameToDisplay.length() > 14) nameToDisplay = nameToDisplay.substring(0, 14);
            lcd.print(nameToDisplay);

            if (currentRssiGraphicToDisplay != ' ') { lcd.setCursor(14, 0); lcd.print(" "); lcd.setCursor(15, 0); lcd.write(currentRssiGraphicToDisplay);

            }
            
            lcd.setCursor(0, 1);

            if (connectedSensorType == "multiSensor") {
                if (sensorData.tempSuccess) { lcd.print(currentTempValueStr);

                lcd.write(DEGREE_CHAR_INDEX); lcd.print("C"); if (currentTempTrend == 1) { lcd.print("-"); } else if (currentTempTrend == 2) { lcd.write(DOWN_ARROW_CHAR_INDEX);

                } else if (currentTempTrend == 3) { lcd.write(UP_ARROW_CHAR_INDEX); } } else { lcd.print("Temp N/A");

                }
                lcd.print(" ");

                if (sensorData.humiditySuccess) { lcd.print(currentHumidityValueStr); lcd.print("%"); lcd.write(WATER_DROP_CHAR_INDEX); if (currentHumidityTrend == 1) { lcd.print("-"); } else if (currentHumidityTrend == 2) { lcd.write(DOWN_ARROW_CHAR_INDEX);

                } else if (currentHumidityTrend == 3) { lcd.write(UP_ARROW_CHAR_INDEX); } } else { lcd.print("Wilg. N/A");

                }
            } else {
                if (sensorData.tempSuccess) { lcd.print(currentTempValueStr);

                lcd.write(DEGREE_CHAR_INDEX); lcd.print("C"); if (currentTempTrend == 1) { lcd.print("-"); } else if (currentTempTrend == 2) { lcd.write(DOWN_ARROW_CHAR_INDEX);

                } else if (currentTempTrend == 3) { lcd.write(UP_ARROW_CHAR_INDEX); } } else { lcd.print("Temp N/A");

                }
            }
            
            lastDisplayedName = currentNameStr;

            lastDisplayedTempValueStr = currentTempValueStr; lastDisplayedHumidityValueStr = currentHumidityValueStr; lastDisplayedTempTrend = currentTempTrend; lastDisplayedHumidityTrend = currentHumidityTrend; lastDisplayedRSSI = currentRSSI; lastDisplayedRssiGraphicIndex = currentRssiGraphicToDisplay;

            lastConnectedSensorType = connectedSensorType;
        }
    } else {
        // --- On Failure: Increment counter and display error message ---
        consecutiveFetchFailures++;

        // Only update the screen to "Signal Lost" if it was previously connected.

        // This prevents the screen from flickering the same error message every 5 seconds.

        if (sensorIsConnected) {
            sensorIsConnected = false;

            // Mark as disconnected
            lcd.clear();

            // Display the last known name of the sensor for context
            String nameToDisplay = lastDisplayedName;

            if (nameToDisplay.isEmpty()) nameToDisplay = "Sensor"; // Fallback name
            if (nameToDisplay.length() > 16) nameToDisplay = nameToDisplay.substring(0, 16);

            lcd.print(nameToDisplay);
            
            // Show the error message
            lcd.setCursor(0, 1);

            lcd.print("Utracono sygnal"); // "Signal lost"
        }

        // After 3 failures, clear the IP to force a full mDNS re-discovery
        if (consecutiveFetchFailures >= MAX_CONSECUTIVE_FETCH_FAILURES) {
            sensorIP = IPAddress(0, 0, 0, 0);

            consecutiveFetchFailures = 0; 
        }
    }
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
    lcd.print(">");
    lcd.print(modeNames[selectionIndex]);
    lcd.setCursor(0, 1);
    lcd.print(" ");
    lcd.print(modeNames[(selectionIndex + 1) % numModes]);

    int pressType = NO_PRESS;
    while(pressType == NO_PRESS) { pressType = checkButton(); delay(10);

    }
    if (pressType == SHORT_PRESS_DETECTED) {
      selectionIndex = (selectionIndex + 1) % numModes;

    } else if (pressType == LONG_PRESS_DETECTED) {
      if (selectionIndex == 0) return MODE_AP;

      if (selectionIndex == 1) return MODE_AP_SECURE_SETUP;
      if (selectionIndex == 2) return MODE_WIFI;

    }
  }
}

// --- PHASE 1: Captive Portal for WiFi Credentials ---
void startCaptivePortal() {
  lcd.clear();
  lcd.print("Konfiguruj WiFi");
  lcd.setCursor(0,1);
  lcd.print(CAPTIVE_PORTAL_SSID);

  // [EDUKACJA]: Uruchomienie punktu dostępowego (SoftAP) w celu konfiguracji urządzenia.
  WiFi.softAP(CAPTIVE_PORTAL_SSID);
  
  // [EDUKACJA]: Uruchomienie serwera DNS przekierowującego wszystkie zapytania (*) na IP naszego ESP.
  // Dzięki temu, na telefonie po podłączeniu wyskoczy powiadomienie "Zaloguj się do sieci".
  dnsServer.start(53, "*", WiFi.softAPIP());
  
  webServer.on("/", HTTP_GET, handleWifiConfigRoot);
  webServer.on("/save", HTTP_POST, handleWifiConfigSave);
  webServer.onNotFound(handleNotFound);
  webServer.begin();
  
  while(true) {
    dnsServer.processNextRequest();
    webServer.handleClient();

    if (checkButton() == LONG_PRESS_DETECTED) {
      lcd.clear();
      lcd.print("Reset ustawien...");
      eraseSettings();
      delay(2000);
      ESP.restart();

    }
    delay(10);
  }
}

// [EDUKACJA]: Strona HTML serwowana przez ESP32
void handleWifiConfigRoot() {
  lcd.clear();
  lcd.print("Skanowanie...");
  int n = WiFi.scanNetworks();
  lcd.clear();
  lcd.print(CAPTIVE_PORTAL_SSID);
  lcd.setCursor(0, 1);
  lcd.print("192.168.4.1");

  String options = "";
  for (int i = 0; i < n; ++i) { options += "<option value='" + WiFi.SSID(i) + "'>" + WiFi.SSID(i) + "</option>";

  }
  // Surowy HTML trzymany w pamięci programu. Ważne, by tagi formularza POST celowały w endpoint /save
  String html = R"(<!DOCTYPE html><html><head><title>Monitor TSHS Setup</title><meta name=viewport content='width=device-width, initial-scale=1'><style>body{font-family:sans-serif;text-align:center;background:#f2f2f2;}div{background:white;padding:20px;border-radius:10px;display:inline-block;margin-top:20px;max-width:300px;}input, select{width:100%;padding:10px;margin:10px 0;border-radius:5px;border:1px solid #ccc;box-sizing:border-box;}button{background:#4CAF50;color:white;padding:12px 20px;border:none;border-radius:5px;cursor:pointer;width:100%;}</style><script>function updateSSID(){var dropdown=document.getElementById('ssid-select');var selectedSSID=dropdown.options[dropdown.selectedIndex].value;if(selectedSSID){document.getElementById('ssid-input').value=selectedSSID;}}</script></head><body><div><h1>Konfiguracja WiFi</h1><p>Wybierz siec z listy lub wpisz ja recznie.</p><form action='/save' method='POST'><select id='ssid-select' onchange='updateSSID()'><option value=''>-- Wybierz siec --</option>)" + options + R"(</select><input type='text' id='ssid-input' name='ssid' placeholder='Nazwa sieci (SSID)' required><input type='password' name='pass' placeholder='Haslo'><button type='submit'>Zapisz</button></form></div></body></html>)";

  webServer.send(200, "text/html", html);
}

void handleWifiConfigSave() {
  lcd.clear();
  lcd.print("Zapisuje...");
  String ssid = webServer.arg("ssid");
  String password = webServer.arg("pass");

  // Check if the selected SSID is a known sensor AP
  if (ssid.startsWith(TEMP_AP_PREFIX_1) || ssid.startsWith(TEMP_AP_PREFIX_2) || ssid.startsWith(HUMIDITY_AP_PREFIX)) {
      settings.connectToProtectedAP = true;

      // [EDUKACJA]: Funkcja strncpy dba o to, żeby nie przekroczyć rozmiaru tablicy (Buffer Overflow) w pamięci
      strncpy(settings.protectedApSsid, ssid.c_str(), 32);
      strncpy(settings.protectedApPassword, password.c_str(), 64);
      settings.protectedApSsid[32] = '\0';
      settings.protectedApPassword[64] = '\0';
      settings.connectionMode = MODE_AP;

// Set mode for next boot
  } else {
      // Otherwise, save as normal WiFi credentials
      settings.connectToProtectedAP = false;

      strncpy(settings.wifiSSID, ssid.c_str(), 32);
      strncpy(settings.wifiPassword, password.c_str(), 64);
      settings.wifiSSID[32] = '\0';
      settings.wifiPassword[64] = '\0';
      settings.connectionMode = MODE_WIFI;
  }
  
  saveSettings();

  String response = "<h1>Zapisano!</h1><p>Uruchamiam ponownie...</p>";
  webServer.send(200, "text/html", response);
  delay(2000);
  ESP.restart();
}

void handleNotFound() { webServer.sendHeader("Location", "/", true); webServer.send(302, "text/plain", "");

}

// --- UNIFIED SENSOR SELECTION FUNCTION ---
bool discoverAndSelectMDNS_Sensor() {
    lcd.clear();
    lcd.print("Szukam sensorow...");
    
    // [EDUKACJA]: Bleboxy emitują rekord DNS typu 'bbxsrv' po protokole TCP.
    int n = MDNS.queryService("bbxsrv", "tcp");

    foundMDNSCount = 0;
    
    if (n == 0) {
        lcd.clear();
        lcd.print("Brak sensorow");
        delay(3000);

        return false;
    }

    lcd.clear();
    lcd.print("Znaleziono ");
    lcd.print(n);
    //lcd.print(" urzadzen");
    delay(2000);

    lcd.clear();
    lcd.print("Filtruje...");

    for (int i = 0; i < n && foundMDNSCount < MAX_MDNS_SERVICES; i++) {
        String hostName = MDNS.hostname(i);

        IPAddress serviceIP = MDNS.queryHost(hostName);
        if (serviceIP != IPAddress(0,0,0,0)) {
            String url = "http://" + serviceIP.toString() + INFO_ENDPOINT_PATH;

            DeviceInfo info = fetchDeviceInfo(url);
            if (info.success) {
                if (info.apiType == "tempSensor") {
                    foundMDNS_Names[foundMDNSCount] = info.deviceName;

                    foundMDNS_Hostnames[foundMDNSCount] = hostName;
                    foundMDNSCount++;
                } else if (info.apiType == "multiSensor") {
                    String stateUrl = "http://" + serviceIP.toString() + TEMP_ENDPOINT_PATH;

                    HTTPClient stateHttp;
                    if (stateHttp.begin(stateUrl.c_str())) {
                        int stateCode = stateHttp.GET();

                        if (stateCode > 0) {
                            String statePayload = stateHttp.getString();

                            // [EDUKACJA]: Alokacja pamięci dla parsera JSON. ArduinoJson używa StaticJsonDocument 
                            // do alokacji na stosie, by uniknąć fragmentacji sterty (heap) - kluczowe dla stabilności na małych chipach.
                            StaticJsonDocument<512> stateDoc;
                            deserializeJson(stateDoc, statePayload);
                            if (stateDoc.containsKey("multiSensor") && stateDoc["multiSensor"]["sensors"]) {
                                for (JsonObject sensor : stateDoc["multiSensor"]["sensors"].as<JsonArray>()) {
                                    if (String(sensor["type"]) == "humidity") {
               
                                         foundMDNS_Names[foundMDNSCount] = info.deviceName;

                                        foundMDNS_Hostnames[foundMDNSCount] = hostName;
                                        foundMDNSCount++;
                                        break; 
                                    }
                                }
                            }
                        }
           
                             stateHttp.end();

                }
                }
            }
        }
    }

    if (foundMDNSCount == 0) {
        lcd.clear();

        lcd.print("Brak sensorow");
        lcd.setCursor(0,1);
        lcd.print("do wyswietlenia");
        delay(3000);
        return false;
    }

    lcd.clear();
    lcd.print("Znaleziono ");
    lcd.print(foundMDNSCount);
    //lcd.print(" pasujacych");
    delay(2000);

    int currentIndex = 0;
    while(true) {
      lcd.clear();
      lcd.setCursor(0, 0);
      lcd.print(">");
      lcd.print(foundMDNS_Names[currentIndex].substring(0,15));

      if (foundMDNSCount > 1) {
          lcd.setCursor(0, 1);
          lcd.print(" ");

          lcd.print(foundMDNS_Names[(currentIndex + 1) % foundMDNSCount].substring(0,15));
      }

      int pressType = NO_PRESS;

      while(pressType == NO_PRESS) { pressType = checkButton(); delay(10); }

      if (pressType == SHORT_PRESS_DETECTED) {
        currentIndex = (currentIndex + 1) % foundMDNSCount;

      } else if (pressType == LONG_PRESS_DETECTED) {
        selectedSensorHostname = foundMDNS_Hostnames[currentIndex] + ".local";

        strncpy(settings.mDNSSensorName, selectedSensorHostname.c_str(), 64);
        strncpy(settings.mDNSSensorFriendlyName, foundMDNS_Names[currentIndex].c_str(), 32);
        saveSettings();
        
        lcd.clear();
        lcd.print("Wybrano:");
        lcd.setCursor(0,1);
        lcd.print(foundMDNS_Names[currentIndex].substring(0, 16));
        delay(2000);
        return true;

      }
    }
    return false;

}

// ===================================================================================
//                          ORIGINAL AP MODE FUNCTIONS
// ===================================================================================
bool scanAndSelectAP() {
  lcd.clear();

  lcd.print("Skanuje WiFi...");
  int n = WiFi.scanNetworks();
  
  String tempSSIDs[MAX_FOUND_APS];
  int tempCount = 0;

  if (n > 0) {
    for (int i = 0; i < n && tempCount < MAX_FOUND_APS; ++i) {
      String ssid = WiFi.SSID(i);

      // [EDUKACJA]: Filtrowanie widocznych sieci, żeby pokazać tylko te niezabezpieczone, zaczynające się od nazwy urządzenia.
      if ((ssid.startsWith(TEMP_AP_PREFIX_1) || ssid.startsWith(TEMP_AP_PREFIX_2) || ssid.startsWith(HUMIDITY_AP_PREFIX)) && (WiFi.encryptionType(i) == WIFI_AUTH_OPEN)) {
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
  while(true) {
    lcd.clear();
    lcd.setCursor(0, 0);
    lcd.print(">");
    lcd.print(foundAP_DisplayNames[currentIndex].substring(0,15));

    if (foundAPCount > 1) {
        lcd.setCursor(0, 1);
        lcd.print(" ");

        lcd.print(foundAP_DisplayNames[(currentIndex + 1) % foundAPCount].substring(0,15));
    }

    int pressType = NO_PRESS;
    while(pressType == NO_PRESS) { pressType = checkButton();

    delay(10); }

    if (pressType == SHORT_PRESS_DETECTED) {
      currentIndex = (currentIndex + 1) % foundAPCount;

    } else if (pressType == LONG_PRESS_DETECTED) {
      String selectedSSID = foundAP_SSIDs[currentIndex];
      
      lcd.clear();
      lcd.print("Wybrano:");
      lcd.setCursor(0,1);
      lcd.print(foundAP_DisplayNames[currentIndex].substring(0,16));

      delay(2000);

      lcd.clear();
      lcd.print("Laczenie z:");
      lcd.setCursor(0,1);
      lcd.print(foundAP_DisplayNames[currentIndex].substring(0,16));

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
  }
  return false;

}

bool connectToAP(const String& ssid, const char* password) {
    WiFi.begin(ssid.c_str(), password);
    unsigned long connectStart = millis();

    while (WiFi.status() != WL_CONNECTED) {
        if (checkButton() == LONG_PRESS_DETECTED) {
            lcd.clear();

            lcd.print("Reset ustawien");
            lcd.setCursor(0, 1);
            lcd.print("Restartuje...");
            eraseSettings();
            delay(2000);
            ESP.restart();
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
// [EDUKACJA]: Odpytywanie endpointu /info dostarczającego meta-dane o sensorze.
DeviceInfo fetchDeviceInfo(const String& endpoint) {
  DeviceInfo result = {"", "", false};

  if (WiFi.status() != WL_CONNECTED) {
      if (WiFi.softAPgetStationNum() == 0 && settings.connectionMode != MODE_AP) {
          return result;

      }
  }
  HTTPClient http;
  if (!http.begin(endpoint.c_str())) {
    return result;
  }
  http.setTimeout(2000); // [EDUKACJA]: Timeout ratuje sytuację, gdy urządzenie przestanie odpowiadać w sieci, chroniąc przed zawieszeniem programu.

  int httpResponseCode = http.GET();
  if (httpResponseCode > 0) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;

    DeserializationError error = deserializeJson(doc, payload);
    if (!error) {
      // [EDUKACJA]: Nawigacja po zagnieżdżonych strukturach JSON -> {"device": {"deviceName": "Salon"}}
      if (doc.containsKey("device") && doc["device"].is<JsonObject>()) {
        if (doc["device"].containsKey("deviceName")) result.deviceName = String(doc["device"]["deviceName"].as<const char*>());

        if (doc["device"].containsKey("type")) result.apiType = String(doc["device"]["type"].as<const char*>());
        result.success = true;
      }
    }
  }
  http.end();
  return result;

}

SensorReading fetchTempSensorData(const String& endpoint) {
  SensorReading result = {0.0f, 0, false, 0.0f, 0, false};

  if (WiFi.status() != WL_CONNECTED) return result;
  HTTPClient http;
  if (!http.begin(endpoint.c_str())) {
    return result;
  }
  http.setTimeout(2000);

  int httpResponseCode = http.GET();
  if (httpResponseCode > 0) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    deserializeJson(doc, payload);

    if (doc.containsKey("tempSensor") && doc["tempSensor"]["sensors"][0]) {
      JsonObject firstSensor = doc["tempSensor"]["sensors"][0];

      if (firstSensor.containsKey("value")) {
        result.temperature = firstSensor["value"].as<float>();
        result.tempSuccess = true;

      }
      if (firstSensor.containsKey("trend")) result.tempTrend = firstSensor["trend"].as<int>();
    }
  }
  http.end();
  return result;

}

SensorReading fetchMultiSensorData(const String& endpoint) {
  SensorReading result = {0.0f, 0, false, 0.0f, 0, false};

  if (WiFi.status() != WL_CONNECTED) return result;
  HTTPClient http;
  if (!http.begin(endpoint.c_str())) {
    return result;
  }
  http.setTimeout(2000);

  int httpResponseCode = http.GET();
  if (httpResponseCode > 0) {
    String payload = http.getString();
    StaticJsonDocument<512> doc;
    deserializeJson(doc, payload);

    if (doc.containsKey("multiSensor") && doc["multiSensor"]["sensors"]) {
      for (JsonObject sensor : doc["multiSensor"]["sensors"].as<JsonArray>()) {
        String sensorType = sensor["type"].as<String>();

        if (sensorType == "temperature") {
          if (sensor.containsKey("value")) {
            result.temperature = sensor["value"].as<float>();

            result.tempSuccess = true;
          }
          if (sensor.containsKey("trend")) result.tempTrend = sensor["trend"].as<int>();

        } else if (sensorType == "humidity") {
          if (sensor.containsKey("value")) {
            result.humidity = sensor["value"].as<float>();

            result.humiditySuccess = true;
          }
          if (sensor.containsKey("trend")) result.humidityTrend = sensor["trend"].as<int>();

        }
      }
    }
  }
  http.end();
  return result;

}

int checkButton() {
  int reading = digitalRead(BUTTON_PIN);
  int event = NO_PRESS;

  if (reading != lastButtonState) {
    lastDebounceTime = millis();

  }
  if ((millis() - lastDebounceTime) > DEBOUNCE_DELAY) {
    if (reading != buttonState) {
      buttonState = reading;

      if (buttonState == LOW) {
        buttonPressStartTime = millis();
        longPressTriggered = false;

      } else {
        unsigned long pressDuration = millis() - buttonPressStartTime;

        if (buttonPressStartTime != 0 && !longPressTriggered && pressDuration < SHORT_PRESS_MAX_DURATION) {
          event = SHORT_PRESS_DETECTED;

        }
        buttonPressStartTime = 0;

      }
    } else if (buttonState == LOW && !longPressTriggered) {
      if (buttonPressStartTime != 0 && (millis() - buttonPressStartTime >= LONG_PRESS_MIN_DURATION)) {
        event = LONG_PRESS_DETECTED;

        longPressTriggered = true;
      }
    }
  }
  lastButtonState = reading;
  return event;

}

// [EDUKACJA]: Funkcje obsługujące pamięć EEPROM (która na ESP32 w rzeczywistości jest emulowana na pamięci flash).
void loadSettings() {
  EEPROM.get(EEPROM_ADDR, settings);
  if (settings.connectionMode < MODE_UNSET || settings.connectionMode > MODE_WIFI) {
    eraseSettings(); // Zabezpieczenie przed wczytaniem losowych śmieci przy pierwszym uruchomieniu

  }
}

void saveSettings() {
  EEPROM.put(EEPROM_ADDR, settings);
  EEPROM.commit(); // [EDUKACJA]: Na ESP32 samo .put nie zapisuje fizycznie danych – trzeba zawołać .commit() by zapisać blok flash.
}

void eraseSettings() {
  Settings blankSettings;
  memset(&blankSettings, 0, sizeof(Settings)); // Zerowanie pamięci
  blankSettings.connectionMode = MODE_UNSET;

  memcpy(&settings, &blankSettings, sizeof(Settings));
  saveSettings();
}