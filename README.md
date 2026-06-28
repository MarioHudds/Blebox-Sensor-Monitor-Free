# 🌡️ Monitor Sensorów Blebox (ESP32-C3) / Blebox Sensor Monitor

*(Scroll down for the English version)*

Projekt garażowy open-source pozwalający na ciągłe wyświetlanie danych z czujników ekosystemu Blebox (tempSensor, humiditySensor, multiSensor) na klasycznym ekranie LCD 16x2 lun 24x3.

Zamiast polegać na chmurze producenta, urządzenie wykorzystuje architekturę hybrydową Bleboxa – komunikuje się z czujnikami całkowicie lokalnie, w obrębie domowej sieci Wi-Fi lub przez bezpośredni punkt dostępowy (AP) sensora, używając otwartego API po protokole HTTP (mechanizm pollingu).

## 🚀 Funkcje

* **Lokalne działanie:** Brak opóźnień chmurowych, odporność na awarie internetu.
* **Auto-wykrywanie mDNS:** Nie musisz znać adresu IP swojego sensora. Monitor sam znajdzie go w sieci po przyjaznej nazwie (`.local`).
* **Captive Portal:** Łatwa konfiguracja z poziomu przeglądarki w telefonie po pierwszym uruchomieniu.
* **Wsparcie dla sieci chronionych:** Możliwość łączenia się z zabezpieczonymi sieciami AP (Access Point) udostępnianymi przez czujniki.
* **Niezawodność:** Wbudowane mechanizmy ponawiania połączenia i obsługi błędów sieci (np. brak migotania ekranu przy chwilowej utracie sygnału).

## 🛠️ Wymagania sprzętowe (Pinout)

Projekt został zoptymalizowany dla mikrokontrolerów z rodziny **ESP32-C3** (np. SuperMini).

* **Wyświetlacz LCD 16x2 (I2C, adres 0x27):**
  * `SDA` -> GPIO 10
  * `SCL` -> GPIO 20
* **Przycisk fizyczny (do nawigacji i resetu):**
  * `PIN` -> GPIO 4 (zwiera do GND, używa wbudowanego pull-up)
* **Dioda RGB (wyłączana programowo, by nie raziła):**
  * `DIN` -> GPIO 8 (wbudowana w płytkę SuperMini)

## 💻 Środowisko programistyczne i instalacja

**Ważne:** Oprogramowanie zostało stworzone i przetestowane w środowisku **Arduino IDE w wersji 2.3.6**. Ze względu na dynamiczny rozwój rdzenia ESP32 dla Arduino, używanie nowszych wersji kompilatora może wymagać drobnych korekt w kodzie lub downgrade'u bibliotek.

### Krok 1: Przygotowanie Arduino IDE

1. Dodaj wsparcie dla płytek ESP32 w *Preferencjach* (Adres URL menedżera dodatkowych płytek).
2. W Menedżerze Płytek zainstaluj pakiet `esp32` (autor: Espressif Systems).
3. Wybierz płytkę z menu: **ESP32C3 Dev Module** (lub odpowiednik dla Twojej płytki).

### Krok 2: Wymagane biblioteki

Otwórz Menedżera Bibliotek w Arduino IDE i upewnij się, że masz zainstalowane poniższe pozycje:

* `ArduinoJson` (autor: Benoit Blanchon) – do parsowania odpowiedzi HTTP
* `LiquidCrystal I2C` (autor: Frank de Brabander) – do obsługi ekranu
* `Adafruit NeoPixel` (autor: Adafruit) – do obsługi wbudowanej diody LED
* *Biblioteki wbudowane w rdzeń ESP32 (nie musisz ich doinstalowywać):* `WiFi`, `HTTPClient`, `Wire`, `EEPROM`, `DNSServer`, `WebServer`, `ESPmDNS`.

### Krok 3: Wgrywanie i bezpieczeństwo

Otwórz plik `.ino` i wgraj go na płytkę. **Uwaga dotycząca bezpieczeństwa:** Twoje hasła do Wi-Fi zapisywane są w wydzielonej partycji pamięci (EEPROM/Flash). Przed przekazaniem gotowego urządzenia komuś obcemu, zawsze wykonaj twardy reset (przytrzymanie przycisku przez >3 sekundy), aby wymazać swoje poufne dane dostępowe.

## ⚙️ Pierwsze uruchomienie

1. Po włączeniu na ekranie pojawi się informacja o trybie pracy.
2. Jeśli urządzenie nie jest skonfigurowane, uruchomi własną sieć Wi-Fi o nazwie **Monitor_TSHS**.
3. Połącz się z tą siecią ze swojego telefonu/komputera. Zostaniesz automatycznie przekierowany na stronę konfiguracyjną (jeśli nie, wpisz w przeglądarce `192.168.4.1`).
4. Wybierz z listy swoją sieć domową Wi-Fi i podaj hasło. Zapisz i poczekaj na restart urządzenia.

## 📄 Licencja

Projekt udostępniany na licencji **MIT**. Możesz modyfikować kod, używać go w projektach komercyjnych i dowolnie rozpowszechniać. Twórcy nie ponoszą odpowiedzialności za błędy wynikające z działania oprogramowania.

---

# 🇬🇧 English Version

An open-source, garage-developed standalone display for the Blebox ecosystem sensors (tempSensor, humiditySensor, multiSensor) using a classic 16x2 LCD.

Instead of relying on the manufacturer's cloud, this device uses Blebox's hybrid architecture – communicating with sensors entirely locally within your home Wi-Fi network or directly via the sensor's Access Point (AP). It fetches data using the open HTTP API (polling mechanism).

## 🚀 Features

* **Local Operation:** No cloud latency, immune to internet outages.
* **mDNS Auto-Discovery:** No need to hardcode IP addresses. The monitor finds the sensor via its `.local` hostname.
* **Captive Portal:** Easy mobile browser configuration on first boot.
* **Protected Network Support:** Ability to connect to secure APs provided by sensors.
* **Reliability:** Built-in reconnection mechanisms and network error handling (prevents screen flickering during temporary signal loss).

## 🛠️ Hardware Requirements (Pinout)

The project is optimized for the **ESP32-C3** family (e.g., SuperMini board).

* **16x2 LCD Display (I2C, address 0x27):**
  * `SDA` -> GPIO 10
  * `SCL` -> GPIO 20
* **Physical Button (navigation & reset):**
  * `PIN` -> GPIO 4 (shorts to GND, uses internal pull-up)
* **RGB LED (disabled via software):**
  * `DIN` -> GPIO 8 (built into the SuperMini board)

## 💻 Development Environment & Installation

**Important:** This software was developed and tested using **Arduino IDE version 2.3.6**. Due to the rapid development of the ESP32 Arduino core, compiling on newer versions might require minor code adjustments or library downgrades.

### Step 1: Arduino IDE Setup

1. Add ESP32 board support in *Preferences* (Additional Boards Manager URLs).
2. Install the `esp32` package by Espressif Systems via the Boards Manager.
3. Select your board: **ESP32C3 Dev Module** (or similar).

### Step 2: Required Libraries

Open the Library Manager and install:

* `ArduinoJson` (by Benoit Blanchon) – for parsing HTTP responses
* `LiquidCrystal I2C` (by Frank de Brabander) – for the display
* `Adafruit NeoPixel` (by Adafruit) – for onboard LED management
* *Core libraries (included with ESP32 core, no installation needed):* `WiFi`, `HTTPClient`, `Wire`, `EEPROM`, `DNSServer`, `WebServer`, `ESPmDNS`.

### Step 3: Flashing & Security Practices

Open the `.ino` file and flash it to your board. **Security Note:** Your Wi-Fi credentials are saved in the device's non-volatile memory (EEPROM/Flash). Before handing the physical device to someone else, always perform a hard reset (hold the button for >3 seconds) to erase your access data.

## ⚙️ Initial Setup

1. Upon power-up, the screen will display the current connection mode.
2. If unconfigured, it will broadcast a Wi-Fi network named **Monitor_TSHS**.
3. Connect to this network using your smartphone. You should be automatically redirected to the setup portal (if not, navigate to `192.168.4.1`).
4. Select your home Wi-Fi network, enter the password, save, and wait for the device to reboot.

## 📄 License

This project is licensed under the **MIT License**. You are free to modify, distribute, and use it in commercial applications. The authors are not liable for any issues arising from the use of this software.
